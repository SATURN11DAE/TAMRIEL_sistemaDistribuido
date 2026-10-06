#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>

#include "persistencia.h"
#include "lamport.h"
#include "util.h"

static Reserva *reservas = NULL;
static int quantidade = 0;
static int capacidade = 0;
static pthread_mutex_t trava = PTHREAD_MUTEX_INITIALIZER;

static void caminhoArquivo(char *saida, size_t tamanho) {
    snprintf(saida, tamanho, "%s/%s.json", PASTA_JSON, servidor.id);
}

static int procurar(const char *idTransacao) {
    for (int i = 0; i < quantidade; i++) {
        if (strcmp(reservas[i].idTransacao, idTransacao) == 0) {
            return i;
        }
    }
    return -1;
}

static int adicionar(const Reserva *reserva) {
    if (quantidade == capacidade) {
        int nova = capacidade ? capacidade * 2 : 16;
        Reserva *novo = realloc(reservas, (size_t) nova * sizeof *reservas);

        if (!novo) return ARMAZENAMENTO_ERRO;
        reservas = novo;
        capacidade = nova;
    }

    reservas[quantidade++] = *reserva;
    return ARMAZENAMENTO_OK;
}

static int gravarArquivo(void) {
    char caminho[160], temporario[170];
    caminhoArquivo(caminho, sizeof caminho);
    snprintf(temporario, sizeof temporario, "%s.tmp", caminho);

    FILE *arquivo = fopen(temporario, "w");
    if (!arquivo) {
        perror("[ARMAZENAMENTO] fopen");
        return ARMAZENAMENTO_ERRO;
    }

    fprintf(arquivo, "{\n  \"servidor\": \"%s\",\n  \"reservas\": [\n", servidor.id);

    for (int i = 0; i < quantidade; i++) {
        char t[256], e[64], c[128], a[128];
        jsonEscape(reservas[i].idTransacao, t, sizeof t);
        jsonEscape(reservas[i].estado, e, sizeof e);
        jsonEscape(reservas[i].coordenadorId, c, sizeof c);
        jsonEscape(reservas[i].idCarona, a, sizeof a);

        fprintf(arquivo,
            "    {\"id_transacao\": \"%s\", \"estado\": \"%s\", \"coordenador_id\": \"%s\", "
            "\"id_carona\": \"%s\", \"relogio_logico\": %d}%s\n",
            t, e, c, a, reservas[i].relogio, i < quantidade - 1 ? "," : "");
    }

    fprintf(arquivo, "  ]\n}\n");
    fflush(arquivo);
    fsync(fileno(arquivo));
    fclose(arquivo);

    if (rename(temporario, caminho) != 0) {
        perror("[ARMAZENAMENTO] rename");
        return ARMAZENAMENTO_ERRO;
    }
    return ARMAZENAMENTO_OK;
}

int armazenamentoIniciar(void) {
    if (mkdir(PASTA_JSON, 0755) != 0 && errno != EEXIST) {
        perror("[ARMAZENAMENTO] mkdir");
        return ARMAZENAMENTO_ERRO;
    }

    char caminho[160];
    caminhoArquivo(caminho, sizeof caminho);

    FILE *arquivo = fopen(caminho, "r");
    if (!arquivo) {
        printf("[ARMAZENAMENTO] %s nao existe ainda, comecando vazio.\n", caminho);
        return ARMAZENAMENTO_OK;
    }

    pthread_mutex_lock(&trava);

    int maiorRelogio = 0;
    char linha[1024];

    while (fgets(linha, sizeof linha, arquivo)) {
        Reserva reserva;
        memset(&reserva, 0, sizeof reserva);

        if (!jsonExtrairString(linha, "id_transacao", reserva.idTransacao, sizeof reserva.idTransacao)) {
            continue;
        }

        jsonExtrairString(linha, "estado", reserva.estado, sizeof reserva.estado);
        jsonExtrairString(linha, "coordenador_id", reserva.coordenadorId, sizeof reserva.coordenadorId);
        jsonExtrairString(linha, "id_carona", reserva.idCarona, sizeof reserva.idCarona);
        reserva.relogio = jsonExtrairInt(linha, "relogio_logico");

        if (reserva.relogio > maiorRelogio) maiorRelogio = reserva.relogio;
        adicionar(&reserva);
    }

    fclose(arquivo);
    pthread_mutex_unlock(&trava);

    if (maiorRelogio > 0) lamportAtualizar(maiorRelogio);

    printf("[ARMAZENAMENTO] %d reserva(s) carregada(s) de %s\n", quantidade, caminho);
    return ARMAZENAMENTO_OK;
}

int armazenamentoSalvar(const Reserva *reserva) {
    pthread_mutex_lock(&trava);

    int retorno;
    int i = procurar(reserva->idTransacao);

    if (i >= 0) {
        reservas[i] = *reserva;
        retorno = ARMAZENAMENTO_OK;
    } else {
        retorno = adicionar(reserva);
    }

    if (retorno == ARMAZENAMENTO_OK) retorno = gravarArquivo();

    pthread_mutex_unlock(&trava);
    return retorno;
}

int armazenamentoBuscar(const char *idTransacao, Reserva *saida) {
    pthread_mutex_lock(&trava);

    int i = procurar(idTransacao);
    if (i >= 0 && saida) *saida = reservas[i];

    pthread_mutex_unlock(&trava);
    return i >= 0 ? ARMAZENAMENTO_OK : ARMAZENAMENTO_NAO_ENCONTRADO;
}

int armazenamentoAtualizarEstado(const char *idTransacao, const char *estado) {
    pthread_mutex_lock(&trava);

    int retorno;
    int i = procurar(idTransacao);

    if (i < 0) {
        retorno = ARMAZENAMENTO_NAO_ENCONTRADO;
    } else {
        snprintf(reservas[i].estado, sizeof reservas[i].estado, "%s", estado);
        retorno = gravarArquivo();
    }

    pthread_mutex_unlock(&trava);
    return retorno;
}

int armazenamentoRemover(const char *idTransacao) {
    pthread_mutex_lock(&trava);

    int retorno;
    int i = procurar(idTransacao);

    if (i < 0) {
        retorno = ARMAZENAMENTO_NAO_ENCONTRADO;
    } else {
        memmove(&reservas[i], &reservas[i + 1], (size_t) (quantidade - i - 1) * sizeof *reservas);
        quantidade--;
        retorno = gravarArquivo();
    }

    pthread_mutex_unlock(&trava);
    return retorno;
}

char *armazenamentoListarJSON(void) {
    pthread_mutex_lock(&trava);

    size_t tamanho = 128 + (size_t) quantidade * 700;
    char *json = malloc(tamanho);

    if (!json) {
        pthread_mutex_unlock(&trava);
        return NULL;
    }

    size_t usado = (size_t) snprintf(json, tamanho, "{\"servidor\": \"%s\", \"total\": %d, \"reservas\": [",
                                    servidor.id, quantidade);

    for (int i = 0; i < quantidade; i++) {
        char t[256], e[64], c[128], a[128];
        jsonEscape(reservas[i].idTransacao, t, sizeof t);
        jsonEscape(reservas[i].estado, e, sizeof e);
        jsonEscape(reservas[i].coordenadorId, c, sizeof c);
        jsonEscape(reservas[i].idCarona, a, sizeof a);

        usado += (size_t) snprintf(json + usado, tamanho - usado,
            "%s{\"id_transacao\": \"%s\", \"estado\": \"%s\", \"coordenador_id\": \"%s\", "
            "\"id_carona\": \"%s\", \"relogio_logico\": %d}",
            i > 0 ? ", " : "", t, e, c, a, reservas[i].relogio);
    }

    snprintf(json + usado, tamanho - usado, "]}\n");

    pthread_mutex_unlock(&trava);
    return json;
}

int armazenamentoQuantidade(void) {
    pthread_mutex_lock(&trava);
    int total = quantidade;
    pthread_mutex_unlock(&trava);

    return total;
}

void armazenamentoEncerrar(void) {
    pthread_mutex_lock(&trava);
    free(reservas);
    reservas = NULL;
    quantidade = capacidade = 0;
    pthread_mutex_unlock(&trava);
}
