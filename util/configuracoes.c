#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "headers/configuracoes.h"

Servidor servidor;

typedef struct {
    const char *id;
    int porta;
    const char *vizinhos[8];
} Regiao;

static const Regiao REGIOES[] = {
    {"skyrim",            3001, {"high_rock", "hammerfell", "cyrodiil", "morrowind", NULL}},
    {"morrowind",         3002, {"skyrim", "cyrodiil", "black_marsh", NULL}},
    {"cyrodiil",          3003, {"skyrim", "hammerfell", "morrowind", "black_marsh", "elsweyr", "valenwood", "summerset_islands", NULL}},
    {"high_rock",         3004, {"skyrim", "hammerfell", NULL}},
    {"hammerfell",        3005, {"high_rock", "skyrim", "cyrodiil", "valenwood", "summerset_islands", NULL}},
    {"black_marsh",       3006, {"morrowind", "cyrodiil", "elsweyr", NULL}},
    {"elsweyr",           3007, {"valenwood", "cyrodiil", "black_marsh", NULL}},
    {"valenwood",         3008, {"elsweyr", "cyrodiil", "hammerfell", "summerset_islands", NULL}},
    {"summerset_islands", 3009, {"hammerfell", "cyrodiil", "valenwood", NULL}},
};
#define QTD_REGIOES (int) (sizeof(REGIOES) / sizeof(REGIOES[0]))

static void normalizar(const char *entrada, char *saida, size_t tamanho) {
    size_t i;
    for (i = 0; entrada[i] && i < tamanho - 1; i++) {
        if (entrada[i] == ' ' || entrada[i] == '-') {
            saida[i] = '_';
        } else {
            saida[i] = (char) tolower((unsigned char) entrada[i]);
        }
    }
    saida[i] = 0;
}

static const Regiao *buscarRegiao(const char *id) {
    for (int i = 0; i < QTD_REGIOES; i++) {
        if (strcmp(REGIOES[i].id, id) == 0) {
            return &REGIOES[i];
        }
    }
    return NULL;
}

int configuracoesCarregar(const char *id, int portaOverride) {
    char normal[MAX_ID];
    normalizar(id, normal, sizeof normal);

    const Regiao *eu = buscarRegiao(normal);
    if (!eu) return -1;

    memset(&servidor, 0, sizeof servidor);
    strcpy(servidor.id, eu->id);
    servidor.porta = portaOverride > 0 ? portaOverride : eu->porta;

    for (int i = 0; eu->vizinhos[i] && servidor.qtdVizinhos < MAX_VIZINHOS; i++) {
        const Regiao *viz = buscarRegiao(eu->vizinhos[i]);
        if (!viz) continue;

        Vizinho *v = &servidor.vizinhos[servidor.qtdVizinhos++];
        strcpy(v->id, viz->id);
        v->porta = viz->porta;

        char nomeHOST[80] = "HOST_";
        size_t k = strlen(nomeHOST);

        for (const char *p = viz->id; *p && k < sizeof nomeHOST - 1; p++) {
            nomeHOST[k++] = (char) toupper((unsigned char) *p);
        }

        nomeHOST[k] = 0;
        const char *host = getenv(nomeHOST);
        snprintf(v->host, sizeof v->host, "%s", host ? host : "localhost");
    }
    return 0;
}

const Vizinho *buscarVizinho(const char *id) {
    char normal[MAX_ID];
    normalizar(id, normal, sizeof normal);

    for (int i = 0; i < servidor.qtdVizinhos; i++) {
        if (strcmp(servidor.vizinhos[i].id, normal) == 0) {
            return &servidor.vizinhos[i];
        }
    }
    return NULL;
}

void listarRegioes(void) {
    printf("Regiões Válidas:\n");
    for (int i = 0; i < QTD_REGIOES; i++) {
        printf("  %-18s (porta padrão %d)\n", REGIOES[i].id, REGIOES[i].porta);
    }
}
