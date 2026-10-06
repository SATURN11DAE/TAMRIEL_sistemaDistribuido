#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <curl/curl.h>

#include "util/configuracoes.h"
#include "util/lamport.h"
#include "util/util.h"
#include "httpCliente.h"

static size_t cabecalhoCorpo(void *dados, size_t tamanho, size_t n, void *ud) {
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

static size_t cabecalhoHeader(char *buffer, size_t tamanho, size_t n, void *ud) {
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

//Requisição a um vizinho
static int requisicao(const char *idVizinho, const char *metodo, const char *caminho,
    const char *corpo, int relogio, Resposta *resposta) {

    memset(resposta, 0, sizeof *resposta);
    resposta->corpo = calloc(1, 1);

    const Vizinho *v = buscarVizinho(idVizinho);
    if (!v) {
        snprintf(resposta->erro, sizeof resposta->erro, "'%s' nao faz fronteira com %s", idVizinho, servidor.id);
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {snprintf(resposta->erro, sizeof resposta->erro, "falha no curl_easy_init"); return -1;}
 
    char url[256], hID[100], hClock[64];

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
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cabecalhoCorpo);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resposta);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, cabecalhoHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &clockRecebido);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long) TIMEOUT_REQUISICAO);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long) TIMEOUT_CONEXAO);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (corpo) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, corpo);

    printf("\n[REDE] -> %s %s (relogio %d)\n", metodo, url, relogio);
    if (corpo) printf("[REDE]    corpo: %s\n", corpo);
 
    CURLcode res = curl_easy_perform(curl);

    int retorno;
    if (res != CURLE_OK) {
        resposta->timeout = (res == CURLE_OPERATION_TIMEDOUT || res == CURLE_COULDNT_CONNECT);
        snprintf(resposta->erro, sizeof resposta->erro, "%s", curl_easy_strerror(res));

        printf("[REDE] !! falha com %s: %s\n", v->id, resposta->erro);
        retorno = -1;
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resposta->codigoHTTP);
        if (clockRecebido >= 0) lamportAtualizar(clockRecebido);

        printf("[REDE] <- %ld de %s (relogio agora %d)\n", resposta->codigoHTTP, v->id, lamportMostrar());
        retorno = 0;
    }
 
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return retorno;
}

int clienteSaude (const char *idVizinho, Resposta *resposta) {
    return requisicao(idVizinho, "GET", PREFIXO_CONSORCIO "/health", NULL, lamportTique(), resposta);
}

int clienteTrechos(const char *idVizinho, const char *origem, const char *destino,
    const char *visitados, Resposta *resposta) {

    CURL *tmp = curl_easy_init();
    char *o = curl_easy_escape(tmp, origem, 0);
    char *d = curl_easy_escape(tmp, destino, 0);
    char *vis = curl_easy_escape(tmp, visitados ? visitados : "", 0);
 
    char caminho[512];
    snprintf(caminho, sizeof caminho, PREFIXO_CONSORCIO
            "/trechos?origem=%s&destino=%s&visitados=%s", o, d, vis);
 
    curl_free(o); curl_free(d); curl_free(vis);
    curl_easy_cleanup(tmp);
    
    return requisicao(idVizinho, "GET", caminho, NULL, lamportTique(), resposta);
}

int clientePreparar(const char *idVizinho, const char *idTransacao,
    const char *idCarona, Resposta *resposta) {

    int relogio = lamportTique();
    char t[128], c[128], viz[MAX_ID * 2], coord[MAX_ID * 2], corpo[1024];

    jsonEscape(idTransacao, t, sizeof t);
    jsonEscape(idCarona, c, sizeof c);
    jsonEscape(idVizinho, viz, sizeof viz);
    jsonEscape(servidor.id, coord, sizeof coord);

    snprintf(corpo, sizeof corpo,
        "{\"id_transacao\": \"%s\", \"coordenador_id\": \"%s\", \"relogio_logico\": %d, "
        "\"trechos_solicitados\": [{\"servidor\": \"%s\", \"id_carona\": \"%s\"}]}",
        t, coord, relogio, viz, c);

    return requisicao(idVizinho, "POST", PREFIXO_CONSORCIO "/reservas/prepare", corpo, relogio, resposta);
}

static int reservaPorID(const char *id, const char *metodo, const char *sufixo,
    const char *idTransacao, Resposta *resposta) {

    CURL *tmp = curl_easy_init();
    char *t = curl_easy_escape(tmp, idTransacao, 0);

    char caminho[256];
    snprintf(caminho, sizeof caminho, PREFIXO_CONSORCIO "/reservas/%s%s", t, sufixo);

    curl_free(t);
    curl_easy_cleanup(tmp);

    return requisicao(id, metodo, caminho, NULL, lamportTique(), resposta);
}

int clienteCommitar (const char *idVizinho, const char *idTransacao, Resposta *resposta) {
    return reservaPorID(idVizinho, "PUT", "/commit", idTransacao, resposta);
}

int clienteRollback(const char *idVizinho, const char *idTransacao, Resposta *resposta) {
    return reservaPorID(idVizinho, "DELETE", "", idTransacao, resposta);
}

int clienteStatus (const char *idVizinho, const char *idTransacao, Resposta *resposta) {
    return reservaPorID(idVizinho, "GET", "/status", idTransacao, resposta);
}

int clienteReservas(const char *idVizinho, Resposta *resposta) {
    return requisicao(idVizinho, "GET", PREFIXO_CONSORCIO "/reservas", NULL, lamportTique(), resposta);
}
