/*
 * sofs.c - Implementação do sistema de arquivos sofs.
 *
 * A camada de blocos (sofs-block) é usada para todos os acessos ao disco;
 * a camada de bitmap (bitmap2) gerencia o controle de blocos e i-nodes livres.
 *
 * Layout do sistema de arquivos dentro de uma partição (em ordem):
 *   [bloco 0]          superbloco
 *   [blocos 1 .. bb]   bitmap de blocos livres   (bb = freeBlocksBitmapSize)
 *   [bb+1 .. bb+bi]    bitmap de i-nodes livres  (bi = freeInodeBitmapSize)
 *   [bb+bi+1 .. ...]   área de i-nodes           (10% dos blocos, arredondado para cima)
 *   [resto]            blocos de dados
 */

#include <string.h>
#include <stdio.h>
#include "sofs.h"
#include "sofs-block.h"

#define MAX_OPEN_FILES   10
#define MAX_LINK_DEPTH   8

/* -------------------------------------------------------------------------
 * Forward declarations of block/inode allocators
 * ---------------------------------------------------------------------- */
static int alloc_data_block(void);
static int free_data_block(unsigned int abs_block_num);
static int alloc_inode(void);
static int free_inode(unsigned int inode_num);

/* -------------------------------------------------------------------------
 * Estado interno de montagem
 * ---------------------------------------------------------------------- */

static int g_mounted = false;
static struct sofs_superbloco g_superbloco;
static unsigned int g_superbloco_sector;

/* -------------------------------------------------------------------------
 * Tabela de arquivos abertos
 * ---------------------------------------------------------------------- */

static struct {
    int inodeNumber;       /* -1 = slot livre */
    int currentPointer;
} g_open_files[MAX_OPEN_FILES];

/* -------------------------------------------------------------------------
 * Estado do diretório (sofs_opendir/readdir/closedir)
 * ---------------------------------------------------------------------- */

static int g_dir_open = 0;
static int g_dir_entry_index;

/* -------------------------------------------------------------------------
 * Auxiliar: lê o MBR e localiza a partição <partition>.
 * ---------------------------------------------------------------------- */
static int read_partition_info(int partition,
                               unsigned int *first_sector,
                               unsigned int *num_sectors)
{
    unsigned char mbr_buf[SECTOR_SIZE];
    struct sofs_mbr *mbr;

    if (read_sector(0, mbr_buf) != 0)
        return -1;

    mbr = (struct sofs_mbr *)mbr_buf;

    if (partition < 0 || partition >= (int)mbr->numPartitions)
        return -1;

    *first_sector = mbr->partitionTable[partition].firstSector;
    *num_sectors  = mbr->partitionTable[partition].lastSector
                    - mbr->partitionTable[partition].firstSector + 1;
    return 0;
}

/* -------------------------------------------------------------------------
 * Funções auxiliares de cálculo
 * ---------------------------------------------------------------------- */

static unsigned int block_size(void)
{
    return g_superbloco.blockSize * SECTOR_SIZE;
}

static unsigned int ptrs_per_block(void)
{
    return block_size() / sizeof(DWORD);
}

static unsigned int first_data_block_num(void)
{
    return 1
        + g_superbloco.freeBlocksBitmapSize
        + g_superbloco.freeInodeBitmapSize
        + g_superbloco.inodeAreaSize;
}

static unsigned int entries_per_dir_block(void)
{
    return block_size() / sizeof(struct sofs_record);
}

/* -------------------------------------------------------------------------
 * Leitura/escrita de i-nodes em disco
 * ---------------------------------------------------------------------- */

static int read_inode(unsigned int inode_num, struct sofs_inode *inode)
{
    unsigned int bs = block_size();
    unsigned int ipb = bs / sizeof(struct sofs_inode);
    unsigned int inode_block = 1
        + g_superbloco.freeBlocksBitmapSize
        + g_superbloco.freeInodeBitmapSize
        + inode_num / ipb;
    unsigned int offset = inode_num % ipb;
    unsigned char *buf = (unsigned char *)__builtin_alloca(bs);

    if (read_block(inode_block, buf) != 0)
        return -1;
    memcpy(inode, buf + offset * sizeof(struct sofs_inode),
           sizeof(struct sofs_inode));
    return 0;
}

static int write_inode(unsigned int inode_num, struct sofs_inode *inode)
{
    unsigned int bs = block_size();
    unsigned int ipb = bs / sizeof(struct sofs_inode);
    unsigned int inode_block = 1
        + g_superbloco.freeBlocksBitmapSize
        + g_superbloco.freeInodeBitmapSize
        + inode_num / ipb;
    unsigned int offset = inode_num % ipb;
    unsigned char *buf = (unsigned char *)__builtin_alloca(bs);

    if (read_block(inode_block, buf) != 0)
        return -1;
    memcpy(buf + offset * sizeof(struct sofs_inode), inode,
           sizeof(struct sofs_inode));
    if (write_block(inode_block, buf) != 0)
        return -1;
    return 0;
}

/* -------------------------------------------------------------------------
 * Resolução de ponteiros de bloco (direto, indireção simples, dupla)
 *
 * get_data_block() resolve um índice de bloco lógico dentro de um arquivo
 * para o número absoluto do bloco de dados em disco.
 * Se allocate != 0 e o bloco/ponteiro não existir, aloca novos blocos.
 * ---------------------------------------------------------------------- */

static int get_data_block(unsigned int inode_num, struct sofs_inode *inode,
                          unsigned int logical_block, int allocate)
{
    unsigned int bs = block_size();
    unsigned int ppb = ptrs_per_block();
    unsigned char *buf;

    /* Ponteiros diretos */
    if (logical_block < 2) {
        if (inode->dataPtr[logical_block] == 0) {
            if (!allocate) return -1;
            int blk = alloc_data_block();
            if (blk < 0) return -1;
            inode->dataPtr[logical_block] = (DWORD)blk;
            if (write_inode(inode_num, inode) != 0) return -1;
        }
        return (int)inode->dataPtr[logical_block];
    }

    unsigned int idx = logical_block - 2;

    /* Indireção simples */
    if (idx < ppb) {
        if (inode->singleIndPtr == 0) {
            if (!allocate) return -1;
            int blk = alloc_data_block();
            if (blk < 0) return -1;
            inode->singleIndPtr = (DWORD)blk;
            if (write_inode(inode_num, inode) != 0) return -1;
        }
        buf = (unsigned char *)__builtin_alloca(bs);
        if (read_block(inode->singleIndPtr, buf) != 0) return -1;
        DWORD *ptrs = (DWORD *)buf;
        if (ptrs[idx] == 0) {
            if (!allocate) return -1;
            int blk = alloc_data_block();
            if (blk < 0) return -1;
            ptrs[idx] = (DWORD)blk;
            if (write_block(inode->singleIndPtr, buf) != 0) return -1;
        }
        return (int)ptrs[idx];
    }

    /* Indireção dupla */
    idx -= ppb;
    unsigned int idx1 = idx / ppb;
    unsigned int idx2 = idx % ppb;

    if (idx1 >= ppb)
        return -1;

    if (inode->doubleIndPtr == 0) {
        if (!allocate) return -1;
        int blk = alloc_data_block();
        if (blk < 0) return -1;
        inode->doubleIndPtr = (DWORD)blk;
        if (write_inode(inode_num, inode) != 0) return -1;
    }

    buf = (unsigned char *)__builtin_alloca(bs);
    if (read_block(inode->doubleIndPtr, buf) != 0) return -1;
    DWORD *l1 = (DWORD *)buf;
    if (l1[idx1] == 0) {
        if (!allocate) return -1;
        int blk = alloc_data_block();
        if (blk < 0) return -1;
        l1[idx1] = (DWORD)blk;
        if (write_block(inode->doubleIndPtr, buf) != 0) return -1;
    }

    if (read_block(l1[idx1], buf) != 0) return -1;
    DWORD *l2 = (DWORD *)buf;
    if (l2[idx2] == 0) {
        if (!allocate) return -1;
        int blk = alloc_data_block();
        if (blk < 0) return -1;
        l2[idx2] = (DWORD)blk;
        if (write_block(l1[idx1], buf) != 0) return -1;
    }
    return (int)l2[idx2];
}

/* -------------------------------------------------------------------------
 * Liberação de todos os blocos de dados referenciados por um i-node
 * ---------------------------------------------------------------------- */

static void free_all_data_blocks(struct sofs_inode *inode)
{
    unsigned int bs = block_size();
    unsigned int ppb = ptrs_per_block();
    unsigned char *buf;
    unsigned int i, j;

    for (i = 0; i < 2; i++) {
        if (inode->dataPtr[i] != 0) {
            free_data_block(inode->dataPtr[i]);
            inode->dataPtr[i] = 0;
        }
    }

    if (inode->singleIndPtr != 0) {
        buf = (unsigned char *)__builtin_alloca(bs);
        if (read_block(inode->singleIndPtr, buf) == 0) {
            DWORD *ptrs = (DWORD *)buf;
            for (i = 0; i < ppb; i++) {
                if (ptrs[i] != 0)
                    free_data_block(ptrs[i]);
            }
        }
        free_data_block(inode->singleIndPtr);
        inode->singleIndPtr = 0;
    }

    if (inode->doubleIndPtr != 0) {
        buf = (unsigned char *)__builtin_alloca(bs);
        if (read_block(inode->doubleIndPtr, buf) == 0) {
            DWORD *l1 = (DWORD *)buf;
            for (i = 0; i < ppb; i++) {
                if (l1[i] != 0) {
                    unsigned char *buf2 = (unsigned char *)__builtin_alloca(bs);
                    if (read_block(l1[i], buf2) == 0) {
                        DWORD *l2 = (DWORD *)buf2;
                        for (j = 0; j < ppb; j++) {
                            if (l2[j] != 0)
                                free_data_block(l2[j]);
                        }
                    }
                    free_data_block(l1[i]);
                }
            }
        }
        free_data_block(inode->doubleIndPtr);
        inode->doubleIndPtr = 0;
    }

    inode->bytesFileSize = 0;
    inode->blocksFileSize = 0;
}

/* -------------------------------------------------------------------------
 * Acesso ao diretório raiz (inode 0)
 * ---------------------------------------------------------------------- */

/*
 * Lê um registro de diretório do diretório raiz pelo índice.
 * Retorna 0 em caso de sucesso.
 */
static int read_root_record(int entry_index, struct sofs_record *rec)
{
    unsigned int erpb = entries_per_dir_block();
    unsigned int logical_block = (unsigned int)entry_index / erpb;
    unsigned int offset = (unsigned int)entry_index % erpb;
    unsigned int bs = block_size();
    struct sofs_inode root_inode;

    if (read_inode(0, &root_inode) != 0)
        return -1;

    int blk = get_data_block(0, &root_inode, logical_block, 0);
    if (blk < 0)
        return -1;

    unsigned char *buf = (unsigned char *)__builtin_alloca(bs);
    if (read_block((unsigned int)blk, buf) != 0)
        return -1;
    memcpy(rec, buf + offset * sizeof(struct sofs_record),
           sizeof(struct sofs_record));
    return 0;
}

/*
 * Escreve um registro de diretório no diretório raiz pelo índice.
 * Retorna 0 em caso de sucesso.
 */
static int write_root_record(int entry_index, struct sofs_record *rec)
{
    unsigned int erpb = entries_per_dir_block();
    unsigned int logical_block = (unsigned int)entry_index / erpb;
    unsigned int offset = (unsigned int)entry_index % erpb;
    unsigned int bs = block_size();
    struct sofs_inode root_inode;

    if (read_inode(0, &root_inode) != 0)
        return -1;

    int blk = get_data_block(0, &root_inode, logical_block, 0);
    if (blk < 0)
        return -1;

    unsigned char *buf = (unsigned char *)__builtin_alloca(bs);
    if (read_block((unsigned int)blk, buf) != 0)
        return -1;
    memcpy(buf + offset * sizeof(struct sofs_record), rec,
           sizeof(struct sofs_record));
    if (write_block((unsigned int)blk, buf) != 0)
        return -1;
    return 0;
}

/*
 * Busca um arquivo pelo nome no diretório raiz.
 * Retorna o índice da entrada, ou -1 se não encontrado.
 */
static int find_record_by_name(const char *name)
{
    struct sofs_inode root_inode;
    struct sofs_record rec;

    if (read_inode(0, &root_inode) != 0)
        return -1;

    if (root_inode.bytesFileSize == 0)
        return -1;

    int total_entries = (int)(root_inode.bytesFileSize / sizeof(struct sofs_record));

    for (int i = 0; i < total_entries; i++) {
        if (read_root_record(i, &rec) != 0)
            return -1;
        if (rec.TypeVal != TYPEVAL_INVALIDO &&
            strncmp(rec.name, name, 50) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * Encontra um registro livre (TYPEVAL_INVALIDO) no diretório raiz.
 * Se não houver espaço, tenta alocar um novo bloco de dados para o
 * diretório raiz.
 * Retorna o índice da entrada, ou -1 em caso de erro.
 */
static int find_free_record(void)
{
    struct sofs_inode root_inode;
    struct sofs_record rec;
    unsigned int bs = block_size();

    if (read_inode(0, &root_inode) != 0)
        return -1;

    int total_entries = (int)(root_inode.bytesFileSize / sizeof(struct sofs_record));

    for (int i = 0; i < total_entries; i++) {
        if (read_root_record(i, &rec) != 0)
            return -1;
        if (rec.TypeVal == TYPEVAL_INVALIDO)
            return i;
    }

    /* Tenta alocar um novo bloco de dados para o diretório raiz */
    unsigned int next_logical = root_inode.bytesFileSize / bs;
    if (get_data_block(0, &root_inode, next_logical, 1) < 0)
        return -1;

    root_inode.bytesFileSize += bs;
    root_inode.blocksFileSize = root_inode.bytesFileSize / bs;
    if (write_inode(0, &root_inode) != 0)
        return -1;

    return total_entries;
}

/*
 * Resolve um nome de arquivo, seguindo softlinks recursivamente.
 * depth: profundidade atual (0 na primeira chamada).
 * Retorna 0 com *out_rec preenchido em caso de sucesso.
 */
static int resolve_name(const char *name, struct sofs_record *out_rec,
                        int depth)
{
    if (depth > MAX_LINK_DEPTH)
        return -1;

    int idx = find_record_by_name(name);
    if (idx < 0)
        return -1;

    if (read_root_record(idx, out_rec) != 0)
        return -1;

    if (out_rec->TypeVal == TYPEVAL_LINK) {
        struct sofs_inode link_inode;
        if (read_inode(out_rec->inodeNumber, &link_inode) != 0)
            return -1;
        unsigned int bs = block_size();
        unsigned char *buf = (unsigned char *)__builtin_alloca(bs);
        if (read_block(link_inode.dataPtr[0], buf) != 0)
            return -1;
        return resolve_name((const char *)buf, out_rec, depth + 1);
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Funções básicas de criação/destruição de blocos de dados e i-nodes.
 * ---------------------------------------------------------------------- */

static int alloc_data_block(void)
{
    int bit;
    unsigned int block_size_val;
    unsigned char *buf;

    if (!g_mounted)
        return -1;

    bit = searchBitmap2(BITMAP_DADOS, 0);
    if (bit < 0)
        return -1;

    if (setBitmap2(BITMAP_DADOS, bit, 1) != 0)
        return -1;

    block_size_val = g_superbloco.blockSize * SECTOR_SIZE;
    buf = (unsigned char *)__builtin_alloca(block_size_val);
    memset(buf, 0, block_size_val);

    unsigned int fdb = first_data_block_num();

    if (write_block(fdb + (unsigned int)bit, buf) != 0) {
        setBitmap2(BITMAP_DADOS, bit, 0);
        return -1;
    }

    return (int)(fdb + (unsigned int)bit);
}

static int free_data_block(unsigned int abs_block_num)
{
    unsigned int fdb;
    int bit;

    if (!g_mounted)
        return -1;

    fdb = first_data_block_num();

    if (abs_block_num < fdb)
        return -1;

    bit = (int)(abs_block_num - fdb);
    return setBitmap2(BITMAP_DADOS, bit, 0);
}

static int alloc_inode(void)
{
    int bit;
    unsigned int inode_block;
    unsigned int inodes_per_block;
    unsigned int inode_offset;
    unsigned char *buf;
    unsigned int block_size_val;

    if (!g_mounted)
        return -1;

    bit = searchBitmap2(BITMAP_INODE, 0);
    if (bit < 0)
        return -1;

    if (setBitmap2(BITMAP_INODE, bit, 1) != 0)
        return -1;

    block_size_val   = g_superbloco.blockSize * SECTOR_SIZE;
    inodes_per_block = block_size_val / sizeof(struct sofs_inode);
    inode_block    = 1
        + g_superbloco.freeBlocksBitmapSize
        + g_superbloco.freeInodeBitmapSize
        + (unsigned int)bit / inodes_per_block;
    inode_offset   = (unsigned int)bit % inodes_per_block;

    buf = (unsigned char *)__builtin_alloca(block_size_val);
    if (read_block(inode_block, buf) != 0) {
        setBitmap2(BITMAP_INODE, bit, 0);
        return -1;
    }

    memset(buf + inode_offset * sizeof(struct sofs_inode), 0,
           sizeof(struct sofs_inode));

    if (write_block(inode_block, buf) != 0) {
        setBitmap2(BITMAP_INODE, bit, 0);
        return -1;
    }

    return bit;
}

static int free_inode(unsigned int inode_num)
{
    if (!g_mounted)
        return -1;

    return setBitmap2(BITMAP_INODE, (int)inode_num, 0);
}

/* -------------------------------------------------------------------------
 * Gerência do sistema de arquivos
 * ---------------------------------------------------------------------- */

int sofs_identify(char *name, int size)
{
    const char *id = "Grupo SOFS";
    if (name == NULL || size <= 0)
        return -1;
    strncpy(name, id, size - 1);
    name[size - 1] = '\0';
    return 0;
}

int sofs_format(int partition, int sectors_per_block)
{
    unsigned int first_sector, num_sectors;
    unsigned int num_blocks;
    unsigned int inode_area_blocks;
    unsigned int bitmap_blocks_data;
    unsigned int bitmap_blocks_inode;
    unsigned char block_buf[sectors_per_block * SECTOR_SIZE];
    struct sofs_superbloco *sb;

    if (sectors_per_block <= 0)
        return -1;

    if (read_partition_info(partition, &first_sector, &num_sectors) != 0)
        return -1;

    if (init_block_layer(first_sector, (unsigned int)sectors_per_block) != 0)
        return -1;

    num_blocks = num_sectors / (unsigned int)sectors_per_block;

    inode_area_blocks = (num_blocks + 9) / 10;

    bitmap_blocks_data  = (num_blocks + 8 * sectors_per_block * SECTOR_SIZE - 1)
                          / (8 * sectors_per_block * SECTOR_SIZE);
    bitmap_blocks_inode = (inode_area_blocks + 8 * sectors_per_block * SECTOR_SIZE - 1)
                          / (8 * sectors_per_block * SECTOR_SIZE);

    /* Constrói e grava o superbloco (bloco 0 da partição) */
    memset(block_buf, 0, sizeof(block_buf));
    sb = (struct sofs_superbloco *)block_buf;
    memcpy(sb->id, "SOFS", 4);
    sb->version              = 0x7E32;
    sb->superblockSize       = 1;
    sb->freeBlocksBitmapSize = (WORD)bitmap_blocks_data;
    sb->freeInodeBitmapSize  = (WORD)bitmap_blocks_inode;
    sb->inodeAreaSize        = (WORD)inode_area_blocks;
    sb->blockSize            = (WORD)sectors_per_block;
    sb->diskSize             = (DWORD)num_blocks;

    {
        DWORD *words = (DWORD *)block_buf;
        DWORD  sum   = words[0] + words[1] + words[2] + words[3] + words[4];
        sb->Checksum = ~sum;
    }

    if (write_block(0, block_buf) != 0)
        return -1;

    /* Inicializa com zeros as áreas de bitmap e de i-nodes */
    unsigned int block_size_bytes = (unsigned int)sectors_per_block * SECTOR_SIZE;
    memset(block_buf, 0, block_size_bytes);

    unsigned int total_bitmap_blocks = bitmap_blocks_data + bitmap_blocks_inode;
    for (unsigned int i = 1; i <= total_bitmap_blocks; i++) {
        if (write_block(i, block_buf) != 0)
            return -1;
    }

    for (unsigned int i = 1 + total_bitmap_blocks;
         i <= total_bitmap_blocks + inode_area_blocks; i++) {
        if (write_block(i, block_buf) != 0)
            return -1;
    }

    /* Marca o i-node 0 como ocupado no bitmap de i-nodes */
    unsigned int inode_bitmap_block = 1 + bitmap_blocks_data;
    if (read_block(inode_bitmap_block, block_buf) != 0)
        return -1;
    block_buf[0] |= 0x01;
    if (write_block(inode_bitmap_block, block_buf) != 0)
        return -1;

    /* Marca o primeiro bloco de dados como ocupado no bitmap de dados */
    if (read_block(1, block_buf) != 0)
        return -1;
    block_buf[0] |= 0x01;
    if (write_block(1, block_buf) != 0)
        return -1;

    /* Inicializa o i-node 0 (diretório raiz) */
    unsigned int fdb = 1 + bitmap_blocks_data + bitmap_blocks_inode + inode_area_blocks;
    unsigned int inode0_block = 1 + bitmap_blocks_data + bitmap_blocks_inode;

    struct sofs_inode root_inode;
    memset(&root_inode, 0, sizeof(root_inode));
    root_inode.dataPtr[0]    = (DWORD)fdb;
    root_inode.bytesFileSize  = block_size_bytes;
    root_inode.blocksFileSize = 1;
    root_inode.RefCounter     = 1;

    if (read_block(inode0_block, block_buf) != 0)
        return -1;
    memcpy(block_buf, &root_inode, sizeof(root_inode));
    if (write_block(inode0_block, block_buf) != 0)
        return -1;

    /* Zera o bloco de dados do diretório raiz */
    memset(block_buf, 0, block_size_bytes);
    if (write_block(fdb, block_buf) != 0)
        return -1;

    return 0;
}

int sofs_mount(int partition)
{
    unsigned int first_sector, num_sectors;
    unsigned char sector_buf[SECTOR_SIZE];
    struct sofs_superbloco *sb;

    if (g_mounted)
        return -1;

    if (read_partition_info(partition, &first_sector, &num_sectors) != 0)
        return -1;

    if (read_sector(first_sector, sector_buf) != 0)
        return -1;

    sb = (struct sofs_superbloco *)sector_buf;

    if (memcmp(sb->id, "SOFS", 4) != 0)
        return -1;

    if (init_block_layer(first_sector, (unsigned int)sb->blockSize) != 0)
        return -1;

    g_superbloco_sector = first_sector;
    if (openBitmap2((int)g_superbloco_sector) != 0)
        return -1;

    memcpy(&g_superbloco, sb, sizeof(g_superbloco));

    for (int i = 0; i < MAX_OPEN_FILES; i++)
        g_open_files[i].inodeNumber = -1;

    g_dir_open = 0;

    g_mounted = true;
    return 0;
}

int sofs_umount(void)
{
    if (!g_mounted)
        return -1;

    closeBitmap2();
    reset_block_layer();
    memset(&g_superbloco, 0, sizeof(g_superbloco));
    g_mounted = false;
    return 0;
}

/* -------------------------------------------------------------------------
 * Operações de arquivo
 * ---------------------------------------------------------------------- */

SOFS_FILE sofs_create(char *filename)
{
    if (!g_mounted || filename == NULL)
        return -1;

    struct sofs_record rec;
    int existing_idx = find_record_by_name(filename);

    if (existing_idx >= 0) {
        if (read_root_record(existing_idx, &rec) != 0)
            return -1;
        struct sofs_inode inode;
        if (read_inode(rec.inodeNumber, &inode) != 0)
            return -1;
        free_all_data_blocks(&inode);
        if (write_inode(rec.inodeNumber, &inode) != 0)
            return -1;
    } else {
        int inum = alloc_inode();
        if (inum < 0)
            return -1;

        int free_idx = find_free_record();
        if (free_idx < 0) {
            free_inode((unsigned int)inum);
            return -1;
        }

        memset(&rec, 0, sizeof(rec));
        rec.TypeVal = TYPEVAL_REGULAR;
        strncpy(rec.name, filename, 50);
        rec.inodeNumber = (DWORD)inum;

        if (write_root_record(free_idx, &rec) != 0) {
            free_inode((unsigned int)inum);
            return -1;
        }
        existing_idx = free_idx;
    }

    if (read_root_record(existing_idx, &rec) != 0)
        return -1;

    int handle = -1;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (g_open_files[i].inodeNumber < 0) {
            handle = i;
            break;
        }
    }
    if (handle < 0)
        return -1;

    g_open_files[handle].inodeNumber    = (int)rec.inodeNumber;
    g_open_files[handle].currentPointer = 0;
    return (SOFS_FILE)handle;
}

int sofs_delete(char *name)
{
    if (!g_mounted || name == NULL)
        return -1;

    int idx = find_record_by_name(name);
    if (idx < 0)
        return -1;

    struct sofs_record rec;
    if (read_root_record(idx, &rec) != 0)
        return -1;

    struct sofs_inode inode;
    if (read_inode(rec.inodeNumber, &inode) != 0)
        return -1;

    if (rec.TypeVal == TYPEVAL_REGULAR && inode.RefCounter > 1) {
        inode.RefCounter--;
        if (write_inode(rec.inodeNumber, &inode) != 0)
            return -1;
    } else {
        free_all_data_blocks(&inode);
        free_inode(rec.inodeNumber);
    }

    memset(&rec, 0, sizeof(rec));
    rec.TypeVal = TYPEVAL_INVALIDO;
    if (write_root_record(idx, &rec) != 0)
        return -1;

    return 0;
}

SOFS_FILE sofs_open(char *name)
{
    if (!g_mounted || name == NULL)
        return -1;

    struct sofs_record rec;
    if (resolve_name(name, &rec, 0) != 0)
        return -1;

    int handle = -1;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (g_open_files[i].inodeNumber < 0) {
            handle = i;
            break;
        }
    }
    if (handle < 0)
        return -1;

    g_open_files[handle].inodeNumber    = (int)rec.inodeNumber;
    g_open_files[handle].currentPointer = 0;
    return (SOFS_FILE)handle;
}

int sofs_close(SOFS_FILE handle)
{
    if (handle < 0 || handle >= MAX_OPEN_FILES)
        return -1;
    if (g_open_files[handle].inodeNumber < 0)
        return -1;

    g_open_files[handle].inodeNumber = -1;
    return 0;
}

int sofs_read(SOFS_FILE handle, char *buffer, int size)
{
    if (!g_mounted || handle < 0 || handle >= MAX_OPEN_FILES)
        return -1;
    if (g_open_files[handle].inodeNumber < 0 || buffer == NULL || size <= 0)
        return -1;

    struct sofs_inode inode;
    if (read_inode((unsigned int)g_open_files[handle].inodeNumber, &inode) != 0)
        return -1;

    unsigned int bs = block_size();
    int pos = g_open_files[handle].currentPointer;
    int file_size = (int)inode.bytesFileSize;
    int bytes_read = 0;

    while (bytes_read < size && pos < file_size) {
        unsigned int logical_block = (unsigned int)pos / bs;
        unsigned int byte_offset   = (unsigned int)pos % bs;

        int blk = get_data_block(
            (unsigned int)g_open_files[handle].inodeNumber,
            &inode, logical_block, 0);
        if (blk < 0)
            break;

        unsigned char *buf = (unsigned char *)__builtin_alloca(bs);
        if (read_block((unsigned int)blk, buf) != 0)
            break;

        int to_copy = (int)(bs - byte_offset);
        if (to_copy > size - bytes_read)
            to_copy = size - bytes_read;
        if (to_copy > file_size - pos)
            to_copy = file_size - pos;
        if (to_copy <= 0)
            break;

        memcpy(buffer + bytes_read, buf + byte_offset, (size_t)to_copy);
        bytes_read    += to_copy;
        pos           += to_copy;
    }

    g_open_files[handle].currentPointer = pos;
    return bytes_read;
}

int sofs_write(SOFS_FILE handle, char *buffer, int size)
{
    if (!g_mounted || handle < 0 || handle >= MAX_OPEN_FILES)
        return -1;
    if (g_open_files[handle].inodeNumber < 0 || buffer == NULL || size <= 0)
        return -1;

    struct sofs_inode inode;
    unsigned int inum = (unsigned int)g_open_files[handle].inodeNumber;
    if (read_inode(inum, &inode) != 0)
        return -1;

    unsigned int bs = block_size();
    int pos = g_open_files[handle].currentPointer;
    int bytes_written = 0;

    while (bytes_written < size) {
        unsigned int logical_block = (unsigned int)pos / bs;
        unsigned int byte_offset   = (unsigned int)pos % bs;

        int blk = get_data_block(inum, &inode, logical_block, 1);
        if (blk < 0)
            break;

        unsigned char *buf = (unsigned char *)__builtin_alloca(bs);
        if (read_block((unsigned int)blk, buf) != 0)
            break;

        int to_copy = (int)(bs - byte_offset);
        if (to_copy > size - bytes_written)
            to_copy = size - bytes_written;

        memcpy(buf + byte_offset, buffer + bytes_written, (size_t)to_copy);
        if (write_block((unsigned int)blk, buf) != 0)
            break;

        bytes_written += to_copy;
        pos           += to_copy;

        if ((unsigned int)pos > inode.bytesFileSize) {
            inode.bytesFileSize = (unsigned int)pos;
            inode.blocksFileSize = (inode.bytesFileSize + bs - 1) / bs;
            if (write_inode(inum, &inode) != 0)
                return -1;
        }
    }

    g_open_files[handle].currentPointer = pos;
    return bytes_written;
}

/* -------------------------------------------------------------------------
 * Operações de diretório
 * ---------------------------------------------------------------------- */

int sofs_opendir(void)
{
    if (!g_mounted)
        return -1;

    g_dir_open        = 1;
    g_dir_entry_index = 0;
    return 0;
}

int sofs_readdir(SOFS_DIRENT *dentry)
{
    if (!g_mounted || !g_dir_open || dentry == NULL)
        return -1;

    struct sofs_inode root_inode;
    if (read_inode(0, &root_inode) != 0)
        return -1;

    int total_entries = (int)(root_inode.bytesFileSize / sizeof(struct sofs_record));
    struct sofs_record rec;

    while (g_dir_entry_index < total_entries) {
        if (read_root_record(g_dir_entry_index, &rec) != 0)
            return -1;

        g_dir_entry_index++;

        if (rec.TypeVal != TYPEVAL_INVALIDO) {
            memset(dentry->name, 0, sizeof(dentry->name));
            strncpy(dentry->name, rec.name, SOFS_MAX_FILE_NAME_SIZE);
            dentry->fileType = rec.TypeVal;

            struct sofs_inode file_inode;
            if (read_inode(rec.inodeNumber, &file_inode) != 0)
                return -1;
            dentry->fileSize = file_inode.bytesFileSize;
            return 0;
        }
    }

    return -1;
}

int sofs_closedir(void)
{
    if (!g_mounted || !g_dir_open)
        return -1;

    g_dir_open        = 0;
    g_dir_entry_index = 0;
    return 0;
}

/* -------------------------------------------------------------------------
 * Operações de link
 * ---------------------------------------------------------------------- */

int sofs_sln(char *linkname, char *filename)
{
    if (!g_mounted || linkname == NULL || filename == NULL)
        return -1;

    int inum = alloc_inode();
    if (inum < 0)
        return -1;

    int blk = alloc_data_block();
    if (blk < 0) {
        free_inode((unsigned int)inum);
        return -1;
    }

    unsigned int bs = block_size();
    unsigned char *buf = (unsigned char *)__builtin_alloca(bs);
    memset(buf, 0, bs);
    strncpy((char *)buf, filename, bs - 1);
    if (write_block((unsigned int)blk, buf) != 0) {
        free_data_block((unsigned int)blk);
        free_inode((unsigned int)inum);
        return -1;
    }

    struct sofs_inode link_inode;
    memset(&link_inode, 0, sizeof(link_inode));
    link_inode.dataPtr[0]    = (DWORD)blk;
    link_inode.blocksFileSize = 1;
    link_inode.bytesFileSize  = (DWORD)(strlen(filename) + 1);
    link_inode.RefCounter     = 1;
    if (write_inode((unsigned int)inum, &link_inode) != 0) {
        free_data_block((unsigned int)blk);
        free_inode((unsigned int)inum);
        return -1;
    }

    int free_idx = find_free_record();
    if (free_idx < 0) {
        free_data_block((unsigned int)blk);
        free_inode((unsigned int)inum);
        return -1;
    }

    struct sofs_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.TypeVal = TYPEVAL_LINK;
    strncpy(rec.name, linkname, 50);
    rec.inodeNumber = (DWORD)inum;

    if (write_root_record(free_idx, &rec) != 0) {
        free_data_block((unsigned int)blk);
        free_inode((unsigned int)inum);
        return -1;
    }

    return 0;
}

int sofs_hln(char *linkname, char *filename)
{
    if (!g_mounted || linkname == NULL || filename == NULL)
        return -1;

    struct sofs_record target_rec;
    if (resolve_name(filename, &target_rec, 0) != 0)
        return -1;

    if (target_rec.TypeVal != TYPEVAL_REGULAR)
        return -1;

    struct sofs_inode target_inode;
    if (read_inode(target_rec.inodeNumber, &target_inode) != 0)
        return -1;
    target_inode.RefCounter++;
    if (write_inode(target_rec.inodeNumber, &target_inode) != 0)
        return -1;

    int free_idx = find_free_record();
    if (free_idx < 0) {
        target_inode.RefCounter--;
        write_inode(target_rec.inodeNumber, &target_inode);
        return -1;
    }

    struct sofs_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.TypeVal = TYPEVAL_REGULAR;
    strncpy(rec.name, linkname, 50);
    rec.inodeNumber = target_rec.inodeNumber;

    if (write_root_record(free_idx, &rec) != 0) {
        target_inode.RefCounter--;
        write_inode(target_rec.inodeNumber, &target_inode);
        return -1;
    }

    return 0;
}
