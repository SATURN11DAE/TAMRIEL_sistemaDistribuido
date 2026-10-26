/* 
 * Mapa do mundo e configuração deste servidor
 *
 * Cada servidor representa uma região de Tamriel, de The Elder Scrolls.
 * Aqui definimos quais regiões existem, em que porta cada uma roda e
 * quais são vizinhas entre si.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "headers/configuracoes.h"

/* Configuração deste servidor (preenchida por configuracoesCarregar). */
Servidor servidor;

/* Descrição fixa de uma região do mapa. */
typedef struct {
    const char *id;
    int porta;
    const char *vizinhos[8];   /* Máximo possível de vizinhos, a lista terminada por NULL */
} Regiao;

/* Tabela com todas as regiões. Para adicionar uma região nova, basta incluir uma linha aqui
 * (e incluí-la na lista de vizinhos das regiões que fazem fronteira com ela). */
static const Regiao REGIOES[] = {
    {"skyrim",            3001, {"high_rock", "hammerfell", "cyrodiil", "morrowind", NULL}},
    {"morrowind",         3002, {"skyrim", "cyrodiil", "black_marsh", NULL}},
    {"cyrodiil",          3003, {"skyrim", "hammerfell", "morrowind", "black_marsh",
                                "elsweyr", "valenwood", "summerset_islands", NULL}},
    {"high_rock",         3004, {"skyrim", "hammerfell", NULL}},
    {"hammerfell",        3005, {"high_rock", "skyrim", "cyrodiil", "valenwood",
                                "summerset_islands", NULL}},
    {"black_marsh",       3006, {"morrowind", "cyrodiil", "elsweyr", NULL}},
    {"elsweyr",           3007, {"valenwood", "cyrodiil", "black_marsh", NULL}},
    {"valenwood",         3008, {"elsweyr", "cyrodiil", "hammerfell",
                                "summerset_islands", NULL}},
    {"summerset_islands", 3009, {"hammerfell", "cyrodiil", "valenwood", NULL}},
};

/* Quantidade de regiões = tamanho total da tabela / tamanho de uma linha */
#define QTD_REGIOES (int) (sizeof(REGIOES) / sizeof(REGIOES[0]))

/*
 * Padroniza o nome de uma região: tudo minúsculo, e espaço/hífen viram '_'.
 *   - entrada: ex. "High Rock" ou "high-rock"
 *   - saida / tamanho: buffer de destino; o texto é truncado se não couber
 * Resultado: "high_rock".
 */
void configuracoesNormalizar(const char *entrada, char *saida, size_t tamanho) {
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

/*
 * Procura uma região na tabela REGIOES pelo id (já normalizado).
 * Retorna um ponteiro para a linha da tabela, ou NULL se não existir.
 */
static const Regiao *buscarRegiao(const char *id) {
    for (int i = 0; i < QTD_REGIOES; i++) {
        if (strcmp(REGIOES[i].id, id) == 0) {
            return &REGIOES[i];
        }
    }
    return NULL;
}

/*
 * Retorna 1 se a região existe no mapa, 0 se não.
 */
int configuracoesRegiaoValida(const char *id) {
    return buscarRegiao(id) != NULL;
}

/*
 * Retorna 1 se a região "b" está na lista de vizinhas da região "a".
 * Retorna 0 se não estiver ou se "a" não existir. A relação é lida só na
 * lista de "a", então a tabela deve declarar a fronteira nos dois lados.
 */
int configuracoesSaoVizinhas(const char *a, const char *b) {
    const Regiao *regiaoA = buscarRegiao(a);
    if (!regiaoA) return 0;
    for (int i = 0; regiaoA->vizinhos[i]; i++) {
        if (strcmp(regiaoA->vizinhos[i], b) == 0) return 1;
    }
    return 0;
}

/*
 * Prepara a variável global "servidor" para a região escolhida.
 *   - id: nome da região (aceita "High Rock", "high-rock", por causa de
 *     configuracoesNormalizar)
 *   - portaOverride: se > 0, usa essa porta em vez da padrão
 * O host de cada vizinho vem da variável de ambiente HOST_<NOME>
 * (ex.: HOST_SKYRIM=192.168.0.10); se não existir, assume "localhost".
 * Retorna 0 se deu certo, -1 se a região não existe.
 */
int configuracoesCarregar(const char *id, int portaOverride) {
    char idNormalizado[MAX_ID];
    configuracoesNormalizar(id, idNormalizado, sizeof idNormalizado);

    const Regiao *minhaRegiao = buscarRegiao(idNormalizado);
    if (!minhaRegiao) return -1;

    memset(&servidor, 0, sizeof servidor);
    strcpy(servidor.id, minhaRegiao->id);
    servidor.porta = portaOverride > 0 ? portaOverride : minhaRegiao->porta;

    /* Copia cada vizinho para a configuração, já descobrindo o endereço dele. */
    for (int i = 0; minhaRegiao->vizinhos[i] && servidor.qtdVizinhos < MAX_VIZINHOS; i++) {
        const Regiao *regiaoVizinha = buscarRegiao(minhaRegiao->vizinhos[i]);
        if (!regiaoVizinha) continue;

        Vizinho *vizinho = &servidor.vizinhos[servidor.qtdVizinhos++];
        strcpy(vizinho->id, regiaoVizinha->id);
        vizinho->porta = regiaoVizinha->porta;

        /* O host de cada vizinho vem de uma variável de ambiente chamada HOST_<NOME>,
         * ex.: HOST_SKYRIM=192.168.0.10. Se não existir, assume "localhost". */
        char nomeVariavelHost[80] = "HOST_";
        size_t posicao = strlen(nomeVariavelHost);

        for (const char *letra = regiaoVizinha->id; *letra && posicao < sizeof nomeVariavelHost - 1; letra++) {
            nomeVariavelHost[posicao++] = (char) toupper((unsigned char) *letra);
        }
        nomeVariavelHost[posicao] = 0;

        const char *host = getenv(nomeVariavelHost);
        snprintf(vizinho->host, sizeof vizinho->host, "%s", host ? host : "localhost");
    }
    return 0;
}

/*
 * Procura, entre os vizinhos DESTE servidor, o que tem o id informado.
 *   - id: normalizado antes da comparação
 * Retorna um ponteiro para servidor.vizinhos[i], ou NULL se não achar.
 * O ponteiro NÃO deve ser liberado.
 */
const Vizinho *buscarVizinho(const char *id) {
    char idNormalizado[MAX_ID];
    configuracoesNormalizar(id, idNormalizado, sizeof idNormalizado);

    for (int i = 0; i < servidor.qtdVizinhos; i++) {
        if (strcmp(servidor.vizinhos[i].id, idNormalizado) == 0) {
            return &servidor.vizinhos[i];
        }
    }
    return NULL;
}

/*
 * Imprime na tela todas as regiões disponíveis e suas portas padrão
 * (útil para mensagem de ajuda).
 */
void listarRegioes(void) {
    printf("Regiões Válidas:\n");
    for (int i = 0; i < QTD_REGIOES; i++) {
        printf("  %-18s (porta padrão %d)\n", REGIOES[i].id, REGIOES[i].porta);
    }
}
