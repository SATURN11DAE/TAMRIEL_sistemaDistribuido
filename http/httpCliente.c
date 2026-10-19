#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <curl/curl.h>

#include "util/headers/configuracoes.h"
#include "util/headers/lamport.h"
#include "util/headers/util.h"
#include "headers/httpCliente.h"

static long suspeitoAte[MAX_VIZINHOS];
static pthread_mutex_t travaSuspeita = PTHREAD_MUTEX_INITIALIZER;

static void marcarSuspeita(const Vizinho *v, int suspeito) {
    int i = (int) (v - servidor.vizinhos);
    if (i < 0 || i >= MAX_VIZINHOS) return;

    pthread_mutex_lock(&travaSuspeita);
    suspeitoAte[i] = suspeito ? agoraMonotonicoMs() + SUSPEITA_MS : 0;
    pthread_mutex_unlock(&travaSuspeita);
}

int clienteSuspeito(const char *idVizinho) {
    const Vizinho *v = buscarVizinho(idVizinho);
    if (!v) return 0;

    int i = (int) (v - servidor.vizinhos), r;
    pthread_mutex_lock(&travaSuspeita);
    r = suspeitoAte[i] > agoraMonotonicoMs();
    pthread_mutex_unlock(&travaSuspeita);
    return r;
}

static size_t aoReceberCorpo(void *dados, size_t tamanho, size_t n, void *ud) {
    size_t real = tamanho * n;
    Resposta *resposta = (Resposta *) ud;

    size_t atual = strlen(resposta->corpo);
    char *novo = realloc(resposta->corpo, atual + real + 1);

    if (!novo) return 0;
    resposta->corpo = novo;

    memcpy(resposta->corpo + atual, dados, real);
    resposta->corpo[atual + real] = 0;

    return real;
}

static size_t aoReceberHeader(char *buffer, size_t tamanho, size_t n, void *ud) {
    static const char nome[] = "X-Lamport-Clock:";
    size_t total = tamanho * n;

    if (total > sizeof(nome) - 1 && strncasecmp(buffer, nome, sizeof(nome) - 1) == 0) {
        *(int *) ud = atoi(buffer + sizeof(nome) - 1);
    }

    return total;
}

void respostaLiberar(Resposta *resposta) {
    free(resposta->corpo);
    resposta->corpo = NULL;
}

static int requisicao(const char *idVizinho, const char *metodo, const char *caminho,
    const char *corpo, int relogio, long timeoutMs, Resposta *resposta) {

    memset(resposta, 0, sizeof *resposta);
    resposta->corpo = calloc(1, 1);

    const Vizinho *v = buscarVizinho(idVizinho);
    if (!v) {
        snprintf(resposta->erro, sizeof resposta->erro, "'%s' nao faz fronteira com %s", idVizinho, servidor.id);
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {snprintf(resposta->erro, sizeof resposta->erro, "falha no curl_easy_init"); return -1;}

    char url[512], hID[100], hClock[64];

    snprintf(url, sizeof url, "http://%s:%d%s", v->host, v->porta, caminho);
    snprintf(hID, sizeof hID, "X-Server-Id: %s", servidor.id);
    snprintf(hClock, sizeof hClock, "X-Lamport-Clock: %d", relogio);

    struct curl_slist *headers = NULL;

    headers = curl_slist_append(headers, hID);
    headers = curl_slist_append(headers, hClock);
    headers = curl_slist_append(headers, "Accept: application/json");

    if (corpo) headers = curl_slist_append(headers, "Content-Type: application/json");

    int clockRecebido = -1;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, metodo);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, aoReceberCorpo);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resposta);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, aoReceberHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &clockRecebido);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeoutMs > 0 ? timeoutMs : (long) TIMEOUT_REQUISICAO);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long) TIMEOUT_CONEXAO);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (corpo) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, corpo);

    printf("\n[REDE] -> %s %s (relogio %d)\n", metodo, url, relogio);
    if (corpo) printf("[REDE]    corpo: %.400s\n", corpo);

    CURLcode res = curl_easy_perform(curl);

    int retorno;
    if (res != CURLE_OK) {
        resposta->timeout = (res == CURLE_OPERATION_TIMEDOUT || res == CURLE_COULDNT_CONNECT ||
                             res == CURLE_COULDNT_RESOLVE_HOST || res == CURLE_GOT_NOTHING ||
                             res == CURLE_RECV_ERROR || res == CURLE_SEND_ERROR);
        snprintf(resposta->erro, sizeof resposta->erro, "%s", curl_easy_strerror(res));
        if (resposta->timeout) marcarSuspeita(v, 1);

        printf("[REDE] !! falha com %s: %s\n", v->id, resposta->erro);
        retorno = -1;
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resposta->codigoHTTP);
        if (clockRecebido >= 0) lamportAtualizar(clockRecebido);
        marcarSuspeita(v, 0);

        printf("[REDE] <- %ld de %s (relogio agora %d)\n", resposta->codigoHTTP, v->id, lamportMostrar());
        retorno = 0;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return retorno;
}

static char *escapar(const char *s) {
    CURL *tmp = curl_easy_init();
    char *e = tmp ? curl_easy_escape(tmp, s ? s : "", 0) : NULL;
    char *r = e ? strdup(e) : strdup("");
    if (e) curl_free(e);
    if (tmp) curl_easy_cleanup(tmp);
    return r;
}

int clienteSaude(const char *idVizinho, Resposta *resposta) {
    return requisicao(idVizinho, "GET", PREFIXO_CONSORCIO "/health", NULL, lamportTique(), 0, resposta);
}

int clienteTrechos(const char *idVizinho, const char *origem, const char *destino,
    const char *visitados, Resposta *resposta) {

    char *o = escapar(origem), *d = escapar(destino), *vis = escapar(visitados);

    char caminho[1024];
    snprintf(caminho, sizeof caminho, PREFIXO_CONSORCIO
            "/trechos?origem=%s&destino=%s&visitados=%s", o, d, vis);
    free(o); free(d); free(vis);

    return requisicao(idVizinho, "GET", caminho, NULL, lamportTique(), 0, resposta);
}

int clienteReservas(const char *idVizinho, Resposta *resposta) {
    return requisicao(idVizinho, "GET", PREFIXO_CONSORCIO "/reservas", NULL, lamportTique(), 0, resposta);
}

int clientePrepararCorpo(const char *idVizinho, const char *corpo, long timeoutMs, Resposta *resposta) {
    return requisicao(idVizinho, "POST", PREFIXO_CONSORCIO "/reservas/prepare", corpo,
                      lamportTique(), timeoutMs, resposta);
}

int clientePreparar(const char *idVizinho, const char *idTransacao,
    const char *idCarona, Resposta *resposta) {

    int relogio = lamportTique();
    Buf b; bufIni(&b);

    bufAdd(&b, "{\"id_transacao\": ");    bufStr(&b, idTransacao);
    bufAdd(&b, ", \"coordenador_id\": "); bufStr(&b, servidor.id);
    bufAdd(&b, ", \"passageiro\": \"teste\", \"relogio_logico\": %d, "
               "\"trechos_solicitados\": [{\"servidor\": ", relogio);
    bufStr(&b, idVizinho);
    bufAdd(&b, ", \"id_carona\": ");      bufStr(&b, idCarona);
    bufAdd(&b, "}]}");

    int rc = requisicao(idVizinho, "POST", PREFIXO_CONSORCIO "/reservas/prepare", b.p, relogio, 0, resposta);
    bufLiberar(&b);
    return rc;
}

static int reservaPorID(const char *id, const char *metodo, const char *sufixo,
    const char *idTransacao, Resposta *resposta) {

    char *t = escapar(idTransacao);

    char caminho[512];
    snprintf(caminho, sizeof caminho, PREFIXO_CONSORCIO "/reservas/%s%s", t, sufixo);
    free(t);

    return requisicao(id, metodo, caminho, NULL, lamportTique(), 0, resposta);
}

int clienteCommitar(const char *idVizinho, const char *idTransacao, Resposta *resposta) {
    return reservaPorID(idVizinho, "PUT", "/commit", idTransacao, resposta);
}

int clienteRollback(const char *idVizinho, const char *idTransacao, Resposta *resposta) {
    return reservaPorID(idVizinho, "DELETE", "", idTransacao, resposta);
}

int clienteStatus(const char *idVizinho, const char *idTransacao, Resposta *resposta) {
    return reservaPorID(idVizinho, "GET", "/status", idTransacao, resposta);
}

int clienteCancelar(const char *idVizinho, const char *idTransacao, Resposta *resposta) {
    char *t = escapar(idTransacao);

    char caminho[512];
    snprintf(caminho, sizeof caminho, PREFIXO_CONSORCIO "/reservas/%s/cancel", t);
    free(t);

    return requisicao(idVizinho, "PUT", caminho, NULL, lamportTique(), 8000, resposta);
}
