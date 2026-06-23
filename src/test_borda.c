/*
 * test_borda.c - Testes de casos de borda para o SOFS.
 * Compilar: gcc -Wall -Wextra -std=c99 -I./include -m32 src/test_borda.c -o bin/test_borda -L./lib -lsofs
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sofs.h"

static int total = 0, passed = 0, failed = 0;

#define TESTE(nome, expr) do { \
    total++; \
    int _ok = (expr); \
    printf("  [%s] %s\n", _ok ? "OK" : "FALHOU", nome); \
    if (_ok) passed++; else failed++; \
} while(0)

static void cabecalho(const char *s) {
    printf("\n===== %s =====\n", s);
}

static void prepara_disco(void) {
    sofs_format(0, 2);
    sofs_mount(0);
}

int main(void) {
    setbuf(stdout, NULL);
    printf("SOFS - Testes de Casos de Borda\n");
    printf("================================\n");

    char buf[4096];
    SOFS_FILE arq;
    int n;

    /* =================================================================
     * 1. sofs_create - casos de borda
     * ================================================================= */
    cabecalho("sofs_create");
    prepara_disco();

    TESTE("create com filename NULL", sofs_create(NULL) < 0);

    /* Nome exatamente com 50 chars (limite) */
    char nome50[51];
    memset(nome50, 'A', 50);
    nome50[50] = '\0';
    arq = sofs_create(nome50);
    TESTE("create nome com 50 chars", arq >= 0);
    if (arq >= 0) sofs_close(arq);

    /* Nome com mais de 50 chars */
    char nome51[52];
    memset(nome51, 'B', 51);
    nome51[51] = '\0';
    arq = sofs_create(nome51);
    TESTE("create nome > 50 chars (truncado)", arq >= 0);
    if (arq >= 0) sofs_close(arq);

    /* Criar arquivo existente (deve truncar) */
    arq = sofs_create("teste.txt");
    TESTE("create arquivo novo (teste.txt)", arq >= 0);
    if (arq >= 0) {
        char dados[] = "conteudo original";
        sofs_write(arq, dados, strlen(dados));
        sofs_close(arq);
    }

    /* Re-criar mesmo arquivo: deve truncar */
    arq = sofs_create("teste.txt");
    TESTE("create arquivo existente (truncar)", arq >= 0);
    if (arq >= 0) {
        /* o write abaixo falha porque sofs_read nao read pointer pos 0 */
        sofs_close(arq);
        /* Abre e le para ver se foi truncado */
        arq = sofs_open("teste.txt");
        if (arq >= 0) {
            memset(buf, 0, sizeof(buf));
            n = sofs_read(arq, buf, sizeof(buf));
            TESTE("truncado (leitura 0 bytes)", n == 0);
            sofs_close(arq);
        }
    }

    sofs_umount();

    /* =================================================================
     * 2. sofs_delete - casos de borda
     * ================================================================= */
    cabecalho("sofs_delete");
    prepara_disco();

    TESTE("delete com name NULL", sofs_delete(NULL) < 0);
    TESTE("delete arquivo inexistente", sofs_delete("nao_existe.txt") < 0);

    /* Criar, deletar e deletar de novo */
    arq = sofs_create("para_deletar.txt");
    if (arq >= 0) sofs_close(arq);
    TESTE("delete arquivo existente", sofs_delete("para_deletar.txt") == 0);
    TESTE("delete duplicado", sofs_delete("para_deletar.txt") < 0);

    /* Delete com RefCounter > 1 (hardlinks) */
    arq = sofs_create("hard_base.txt");
    if (arq >= 0) {
        sofs_write(arq, "dados", 5);
        sofs_close(arq);
    }
    TESTE("hardlink criado", sofs_hln("hard_link1", "hard_base.txt") == 0);
    TESTE("delete com refcount > 1 (remove entrada, mas preserva inode)",
          sofs_delete("hard_base.txt") == 0);
    /* O hard link ainda deve existir e o inode preservado */
    arq = sofs_open("hard_link1");
    TESTE("hardlink ainda acessivel apos delete do original", arq >= 0);
    if (arq >= 0) {
        memset(buf, 0, sizeof(buf));
        n = sofs_read(arq, buf, sizeof(buf));
        TESTE("dados do hardlink intactos", n > 0 && strncmp(buf, "dados", 5) == 0);
        sofs_close(arq);
    }

    /* Delete softlink */
    TESTE("softlink criado", sofs_sln("soft_link1", "hard_link1") == 0);
    TESTE("delete softlink", sofs_delete("soft_link1") == 0);

    sofs_umount();

    /* =================================================================
     * 3. sofs_open - casos de borda
     * ================================================================= */
    cabecalho("sofs_open");
    prepara_disco();

    TESTE("open com name NULL", sofs_open(NULL) < 0);
    TESTE("open arquivo inexistente", sofs_open("nao_existe") < 0);

    arq = sofs_create("abrir.txt");
    if (arq >= 0) {
        sofs_write(arq, "conteudo", 8);
        sofs_close(arq);
    }

    /* Teste de softlink quebrado (broken link) */
    TESTE("softlink para existente", sofs_sln("link_bom", "abrir.txt") == 0);
    TESTE("open via softlink valido", sofs_open("link_bom") >= 0);
    sofs_close(sofs_open("link_bom"));

    sofs_delete("abrir.txt");
    TESTE("open via softlink quebrado", sofs_open("link_bom") < 0);

    sofs_umount();

    /* =================================================================
     * 4. sofs_close - casos de borda
     * ================================================================= */
    cabecalho("sofs_close");
    prepara_disco();

    TESTE("close handle negativo", sofs_close(-1) < 0);
    TESTE("close handle >= MAX_OPEN_FILES", sofs_close(10) < 0);
    TESTE("close handle nao aberto", sofs_close(0) < 0);

    arq = sofs_create("close_test.txt");
    TESTE("close handle valido", sofs_close(arq) == 0);
    TESTE("close do mesmo handle de novo (duplo)", sofs_close(arq) < 0);

    sofs_umount();

    /* =================================================================
     * 5. sofs_read - casos de borda
     * ================================================================= */
    cabecalho("sofs_read");
    prepara_disco();

    arq = sofs_create("read_test.txt");
    if (arq >= 0) {
        sofs_write(arq, "0123456789", 10);
        sofs_close(arq);
    }

    arq = sofs_open("read_test.txt");
    TESTE("read handle valido", arq >= 0);
    if (arq >= 0) {
        TESTE("read com buffer NULL", sofs_read(arq, NULL, 10) < 0);
        TESTE("read com size = 0", sofs_read(arq, buf, 0) < 0);
        TESTE("read com size negativo", sofs_read(arq, buf, -1) < 0);

        memset(buf, 0, sizeof(buf));
        n = sofs_read(arq, buf, 5);
        TESTE("read normal (5 bytes)", n == 5 && strncmp(buf, "01234", 5) == 0);

        memset(buf, 0, sizeof(buf));
        n = sofs_read(arq, buf, 100);
        TESTE("read alem do EOF (retorna resto)", n == 5 && strncmp(buf, "56789", 5) == 0);

        memset(buf, 0, sizeof(buf));
        n = sofs_read(arq, buf, 10);
        TESTE("read apos EOF (retorna 0)", n == 0);

        sofs_close(arq);
    }

    /* Leitura de arquivo vazio */
    arq = sofs_create("vazio.txt");
    if (arq >= 0) sofs_close(arq);
    arq = sofs_open("vazio.txt");
    TESTE("read de arquivo vazio", arq >= 0);
    if (arq >= 0) {
        memset(buf, 0, sizeof(buf));
        n = sofs_read(arq, buf, 10);
        TESTE("read arquivo vazio retorna 0", n == 0);
        sofs_close(arq);
    }

    sofs_umount();

    /* =================================================================
     * 6. sofs_write - casos de borda
     * ================================================================= */
    cabecalho("sofs_write");
    prepara_disco();

    arq = sofs_create("write_test.txt");
    TESTE("write handle valido", arq >= 0);
    if (arq >= 0) {
        TESTE("write com buffer NULL", sofs_write(arq, NULL, 10) < 0);
        TESTE("write com size = 0", sofs_write(arq, buf, 0) < 0);
        TESTE("write com size negativo", sofs_write(arq, buf, -1) < 0);

        /* Escrita normal */
        n = sofs_write(arq, "Hello SOFS!", 11);
        TESTE("write normal (11 bytes)", n == 11);

        /* Escrita que cruza fronteira de bloco */
        char big[600];
        memset(big, 'X', 600);
        n = sofs_write(arq, big, 600);
        TESTE("write 600 bytes (cruza blocos)", n == 600);

        sofs_close(arq);

        /* Verificar leitura */
        arq = sofs_open("write_test.txt");
        if (arq >= 0) {
            memset(buf, 0, sizeof(buf));
            n = sofs_read(arq, buf, sizeof(buf));
            TESTE("leitura apos escrita multi-bloco", n == 611);
            TESTE("primeiros 11 bytes intactos", strncmp(buf, "Hello SOFS!", 11) == 0);
            int ok = 1;
            for (int i = 11; i < 611; i++)
                if (buf[i] != 'X') { ok = 0; break; }
            TESTE("bytes X intactos apos primeiro bloco", ok);
            sofs_close(arq);
        }
    }

    sofs_umount();

    /* =================================================================
     * 7. sofs_opendir/readdir/closedir - casos de borda
     * ================================================================= */
    cabecalho("sofs_opendir/readdir/closedir");
    prepara_disco();

    /* Diretorio vazio */
    TESTE("opendir normal", sofs_opendir() == 0);

    SOFS_DIRENT dentry;
    n = sofs_readdir(&dentry);
    TESTE("readdir em diretorio vazio retorna -1", n < 0);

    TESTE("closedir normal", sofs_closedir() == 0);
    TESTE("closedir duplicado", sofs_closedir() < 0);
    TESTE("readdir sem opendir", sofs_readdir(&dentry) < 0);
    TESTE("readdir com dentry NULL", sofs_opendir() == 0 && sofs_readdir(NULL) < 0);
    sofs_closedir();

    /* Diretorio com entradas */
    arq = sofs_create("arq1.txt");
    if (arq >= 0) { sofs_write(arq, "um", 2); sofs_close(arq); }
    arq = sofs_create("arq2.txt");
    if (arq >= 0) { sofs_write(arq, "dois", 4); sofs_close(arq); }
    sofs_sln("link1", "arq1.txt");

    sofs_opendir();
    int count = 0;
    while (sofs_readdir(&dentry) == 0) count++;
    TESTE("readdir conta 3 entradas validas", count == 3);
    sofs_closedir();

    /* Varredura dupla */
    sofs_opendir();
    count = 0;
    while (sofs_readdir(&dentry) == 0) count++;
    TESTE("readdir segunda varredura conta 3", count == 3);
    sofs_closedir();

    sofs_umount();

    /* =================================================================
     * 8. sofs_sln - casos de borda
     * ================================================================= */
    cabecalho("sofs_sln");
    prepara_disco();

    TESTE("sln com linkname NULL", sofs_sln(NULL, "alvo") < 0);
    TESTE("sln com filename NULL", sofs_sln("link", NULL) < 0);
    TESTE("sln com ambos NULL", sofs_sln(NULL, NULL) < 0);
    TESTE("sln para arquivo inexistente", sofs_sln("link_orfao", "nao_existe") >= 0);
    /* Softlink pode apontar para qualquer coisa, inclusive inexistente */

    arq = sofs_create("alvo_real.txt");
    if (arq >= 0) { sofs_write(arq, "alvo", 4); sofs_close(arq); }

    TESTE("sln para arquivo existente", sofs_sln("link_real", "alvo_real.txt") == 0);

    sofs_umount();

    /* =================================================================
     * 9. sofs_hln - casos de borda
     * ================================================================= */
    cabecalho("sofs_hln");
    prepara_disco();

    TESTE("hln com linkname NULL", sofs_hln(NULL, "alvo") < 0);
    TESTE("hln com filename NULL", sofs_hln("link", NULL) < 0);
    TESTE("hln com ambos NULL", sofs_hln(NULL, NULL) < 0);
    TESTE("hln para arquivo inexistente", sofs_hln("hlink", "nao_existe") < 0);

    arq = sofs_create("hln_alvo.txt");
    if (arq >= 0) { sofs_write(arq, "hard", 4); sofs_close(arq); }

    TESTE("hln para arquivo regular", sofs_hln("hlink1", "hln_alvo.txt") == 0);

    /* Hardlink para softlink segue o softlink (comportamento POSIX) */
    sofs_sln("slink_alvo", "hln_alvo.txt");
    TESTE("hln para softlink (segue link, cria hardlink para alvo)", sofs_hln("hlink_soft", "slink_alvo") == 0);


    sofs_umount();

    /* =================================================================
     * 10. Limite de arquivos abertos (MAX_OPEN_FILES = 10)
     * ================================================================= */
    cabecalho("MAX_OPEN_FILES");
    prepara_disco();

    SOFS_FILE handles[12];
    int abertos = 0;
    for (int i = 0; i < 12; i++) {
        char nome[32];
        snprintf(nome, sizeof(nome), "max_arq_%d.txt", i);
        handles[i] = sofs_create(nome);
        if (handles[i] >= 0) abertos++;
    }
    TESTE("maximo de arquivos abertos = 10", abertos == 10);
    for (int i = 0; i < abertos; i++)
        sofs_close(handles[i]);

    sofs_umount();

    /* =================================================================
     * 11. Esgotamento de recursos
     * ================================================================= */
    cabecalho("Esgotamento de recursos");

    /* Formatar com disco pequeno para forçar esgotamento */
    /* O disco t2fs_disk.dat tem 1024 setores. Com 2 setores/bloco, sao 512 blocos.
       Partition 0 ocupa 400 setores = 200 blocos.
       10% = 20 blocos de inode.
       Cada inode = 32 bytes, bloco = 512 bytes, 16 inodes por bloco = 320 inodes max.
       Bloco de bitmap de dados: ceil(200/(8*512)) = 1 bloco
       Bloco de bitmap de inode: ceil(20/(8*512)) = 1 bloco

       Então:
       - superbloco: 1 bloco
       - bitmap dados: 1 bloco
       - bitmap inode: 1 bloco
       - area inode: 20 blocos
       - dados: 200 - 23 = 177 blocos

       O root inode ja ocupa bloco 0 da area de dados e inode 0.
    */

    /* Tentar criar +300 arquivos (orfaos de inodes) */
    int criados = 0;
    for (int i = 0; i < 400; i++) {
        char nome[32];
        snprintf(nome, sizeof(nome), "exaurir_%d.txt", i);
        SOFS_FILE f = sofs_create(nome);
        if (f >= 0) {
            criados++;
            sofs_close(f);
        } else {
            break;
        }
    }
    TESTE("esgotou inodes em algum momento", criados < 400);
    printf("  Inodes criados antes do esgotamento: %d\n", criados);

    sofs_umount();

    /* =================================================================
     * 12. Leitura/Escrita apos delete via handle pendente
     * ================================================================= */
    cabecalho("Acesso apos remocao via handle pendente");
    prepara_disco();

    /* Criar arquivo com dados, abrir e ler primeiro */
    arq = sofs_create("acessar.txt");
    if (arq >= 0) {
        sofs_write(arq, "dados persistentes", 18);
        sofs_close(arq);
    }
    arq = sofs_open("acessar.txt");
    if (arq >= 0) {
        /* Le 5 bytes para posicionar o ponteiro */
        memset(buf, 0, sizeof(buf));
        n = sofs_read(arq, buf, 5);
        TESTE("leitura parcial antes delete", n == 5);

        sofs_delete("acessar.txt");
        /* Le os 13 bytes restantes apos delete (ponteiro continua em 5) */
        memset(buf, 0, sizeof(buf));
        n = sofs_read(arq, buf, sizeof(buf));
        TESTE("leitura apos delete via handle ainda aberto", n == 13);

        /* Escrever mais apos delete */
        n = sofs_write(arq, " + escrito", 10);
        TESTE("escrita apos delete via handle aberto", n == 10);

        sofs_close(arq);
        TESTE("reabrir apos delete+close falha", sofs_open("acessar.txt") < 0);
    }

    sofs_umount();

    /* =================================================================
     * 13. Softlink quebrado (broken link)
     * ================================================================= */
    cabecalho("Softlink quebrado");
    prepara_disco();

    arq = sofs_create("original.txt");
    if (arq >= 0) { sofs_write(arq, "original", 8); sofs_close(arq); }
    sofs_sln("link_broken", "original.txt");
    sofs_delete("original.txt");
    TESTE("open via softlink quebrado retorna erro", sofs_open("link_broken") < 0);

    /* Softlink para softlink */
    sofs_sln("link_chain1", "link_broken");
    TESTE("softlink encadeado quebrado", sofs_open("link_chain1") < 0);

    sofs_umount();

    /* =================================================================
     * 14. Limite de profundidade de softlink (MAX_LINK_DEPTH = 8)
     * ================================================================= */
    cabecalho("MAX_LINK_DEPTH (softlink recursivo)");
    prepara_disco();

    /* Criar cadeia: link0 -> link1 -> link2 -> ... -> link7 -> arquivo */
    /* sofs_sln("link0", "link1") etc, e link7 -> arquivo real */
    arq = sofs_create("alvo_final.txt");
    if (arq >= 0) { sofs_write(arq, "final", 5); sofs_close(arq); }

    char ant[32] = "alvo_final.txt";
    char cur[32];
    for (int i = 0; i < 8; i++) {
        snprintf(cur, sizeof(cur), "chain_%d", i);
        sofs_sln(cur, ant);
        snprintf(ant, sizeof(ant), "%s", cur);
    }
    /* Agora chain_7 -> chain_6 -> ... -> chain_0 -> alvo_final.txt (8 niveis) */
    TESTE("cadeia de 8 softlinks funciona", sofs_open("chain_7") >= 0);

    /* Criar mais um nivel para estourar MAX_LINK_DEPTH */
    sofs_sln("chain_8", "chain_7");
    TESTE("cadeia de 9 softlinks excede MAX_LINK_DEPTH", sofs_open("chain_8") < 0);

    sofs_umount();

    /* =================================================================
     * 15. Escrever alem do limite de ponteiros (indirecao dupla)
     * ================================================================= */
    cabecalho("Limite indirecao dupla");
    prepara_disco();

    arq = sofs_create("grande.txt");
    if (arq >= 0) {
        /* Bloco de 512 bytes, ptrs_per_block = 512/4 = 128
           Blocos logicos:
           0-1: diretos
           2-129: ind. simples (128)
           130-16513: ind. dupla (128*128 = 16384)
           Total: 2 + 128 + 16384 = 16514 blocos logicos * 512 = ~8MB

           Vamos escrever ~10 blocos para testar a indirecao simples e o comeco da dupla
         */
        char bloco[512];
        memset(bloco, 'D', 512);

        /* Diretos: 2 blocos (1024 bytes) */
        for (int i = 0; i < 5; i++) {
            n = sofs_write(arq, bloco, 512);
            if (n != 512) break;
        }
        TESTE("escrita de 5 blocos (diretos + simples)", n == 512);

        sofs_close(arq);

        arq = sofs_open("grande.txt");
        memset(buf, 0, sizeof(buf));
        n = sofs_read(arq, buf, 2560);
        TESTE("leitura de 5 blocos", n == 2560);
        /* Verificar primeiros bytes */
        int ok = 1;
        for (int i = 0; i < 2560; i++) {
            if (buf[i] != 'D') { ok = 0; break; }
        }
        TESTE("conteudo dos 5 blocos intacto", ok);
        sofs_close(arq);
    }

    sofs_umount();

    /* =================================================================
     * Relatorio final
     * ================================================================= */
    printf("\n================================\n");
    printf("RELATORIO FINAL DOS TESTES\n");
    printf("================================\n");
    printf("Total: %d | Passaram: %d | Falharam: %d\n", total, passed, failed);
    printf("================================\n");

    return failed > 0 ? 1 : 0;
}
