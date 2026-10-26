/*
 * Implementação do Cliente HTTP
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <curl/curl.h>

#include "headers/configuracoes.h"
#include "headers/lamport.h"
#include "headers/util.h"
#include "headers/httpCliente.h"

/* Gerenciamento de Suspeitas de Falha */
static long suspeitoAte[MAX_VIZINHOS];
static pthread_mutex_t travaSuspeita = PTHREAD_MUTEX_INITIALIZER;

/*
 * Marca ou desmarca um vizinho como suspeito de ter caído.
 *   - v: ponteiro para um item de servidor.vizinhos (o índice sai da posição dele)
 *   - suspeito: 1 = suspeito pelos próximos SUSPEITA_MS ms; 0 = limpa a suspeita
 * Thread-safe (usa travaSuspeita).
 */
static void marcarSuspeita(const Vizinho *v, int suspeito) {
    int i = (int) (v - servidor.vizinhos);
    if (i < 0 || i >= MAX_VIZINHOS) return;

    pthread_mutex_lock(&travaSuspeita);
    suspeitoAte[i] = suspeito ? agoraMonotonicoMs() + SUSPEITA_MS : 0;
    pthread_mutex_unlock(&travaSuspeita);
}

/*
 * Retorna 1 se o vizinho está marcado como suspeito e o prazo ainda não venceu.
 * Retorna 0 se não estiver suspeito ou se o id não for vizinho deste servidor.
 */
int clienteSuspeito(const char *idVizinho) {
    const Vizinho *v = buscarVizinho(idVizinho);
    if (!v) return 0;

    int i = (int) (v - servidor.vizinhos);
    int resultado;
    
    pthread_mutex_lock(&travaSuspeita);
    resultado = suspeitoAte[i] > agoraMonotonicoMs();
    pthread_mutex_unlock(&travaSuspeita);
    
    return resultado;
}

/* Callbacks do cURL */

/*
 * Callback do cURL: chamado a cada pedaço do corpo recebido.
 *   - ud: a Resposta onde o corpo vai sendo acumulado (realloc)
 * Retorna quantos bytes tratou; retornar menos que o recebido (0 se faltar
 * memória) faz o cURL abortar a requisição.
 */
static size_t aoReceberCorpo(void *dados, size_t tamanho, size_t n, void *ud) {
    size_t real = tamanho * n;
    Resposta *resposta = (Resposta *) ud;

    size_t atual = strlen(resposta->corpo);
    char *novo = realloc(resposta->corpo, atual + real + 1);

    if (!novo) return 0;
    resposta->corpo = novo;

    memcpy(resposta->corpo + atual, dados, real);
    resposta->corpo[atual + real] = '\0';

    return real;
}

/*
 * Callback do cURL: chamado a cada linha de cabeçalho da resposta.
 * Se for "X-Lamport-Clock: N", guarda N no int apontado por 'ud'.
 * Retorna sempre o tamanho recebido (o cURL exige isso para continuar).
 */
static size_t aoReceberHeader(char *buffer, size_t tamanho, size_t n, void *ud) {
    static const char nome[] = "X-Lamport-Clock:";
    size_t total = tamanho * n;

    if (total > sizeof(nome) - 1 && strncasecmp(buffer, nome, sizeof(nome) - 1) == 0) {
        *(int *) ud = atoi(buffer + sizeof(nome) - 1);
    }

    return total;
}

/*
 * Libera o corpo da resposta e o deixa NULL. Chamar depois de cada requisição.
 */
void respostaLiberar(Resposta *resposta) {
    free(resposta->corpo);
    resposta->corpo = NULL;
}

/* Função Base de Requisição */

/*
 * Função base: faz UMA requisição HTTP a um vizinho e preenche a resposta.
 *   - idVizinho: id da região de destino (deve ser vizinha deste servidor)
 *   - metodo / caminho: ex. "GET", "/api/consorcio/health"
 *   - corpo: JSON a enviar (NULL se não houver)
 *   - relogio: valor do relógio de Lamport enviado em X-Lamport-Clock
 *   - timeoutMs: tempo total máximo; <= 0 usa TIMEOUT_REQUISICAO
 *   - resposta: sempre zerada e com corpo alocado (libere com respostaLiberar)
 * Efeitos: sincroniza o relógio de Lamport com o da resposta e marca o vizinho
 * como suspeito se houver falha de rede (ou limpa a suspeita se responder).
 * Retorna 0 se o vizinho respondeu (qualquer código HTTP, veja codigoHTTP),
 * -1 se não houve resposta (veja falhaRede e erro).
 */
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
    if (!curl) {
        snprintf(resposta->erro, sizeof resposta->erro, "falha no curl_easy_init"); 
        return -1;
    }

    char url[512], headerID[100], headerClock[64];

    snprintf(url, sizeof url, "http://%s:%d%s", v->host, v->porta, caminho);
    snprintf(headerID, sizeof headerID, "X-Server-Id: %s", servidor.id);
    snprintf(headerClock, sizeof headerClock, "X-Lamport-Clock: %d", relogio);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, headerID);
    headers = curl_slist_append(headers, headerClock);
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
        resposta->falhaRede = (res == CURLE_OPERATION_TIMEDOUT || res == CURLE_COULDNT_CONNECT ||
                             res == CURLE_COULDNT_RESOLVE_HOST || res == CURLE_GOT_NOTHING ||
                             res == CURLE_RECV_ERROR || res == CURLE_SEND_ERROR);
        snprintf(resposta->erro, sizeof resposta->erro, "%s", curl_easy_strerror(res));
        if (resposta->falhaRede) marcarSuspeita(v, 1);

        printf("[REDE] !! falha com %s: %s\n", v->id, resposta->erro);
        retorno = -1;
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resposta->codigoHTTP);
        if (clockRecebido >= 0) lamportSincronizar(clockRecebido);
        marcarSuspeita(v, 0);

        printf("[REDE] <- %ld de %s (relogio agora %d)\n", resposta->codigoHTTP, v->id, lamportObterValor());
        retorno = 0;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return retorno;
}

/*
 * Codifica um texto para uso em URL (ex.: "São Paulo" -> "S%C3%A3o%20Paulo").
 *   - s: NULL é tratado como ""
 * Retorna uma cópia alocada (quem chama dá free).
 */
static char *escapar(const char *s) {
    CURL *tmp = curl_easy_init();
    char *e = tmp ? curl_easy_escape(tmp, s ? s : "", 0) : NULL;
    char *r = e ? strdup(e) : strdup("");
    if (e) curl_free(e);
    if (tmp) curl_easy_cleanup(tmp);
    return r;
}

/* Funções Públicas de Cliente */

/*
 * GET /health: pergunta se o vizinho está vivo.
 */
int clienteSaude(const char *idVizinho, Resposta *resposta) {
    return requisicao(idVizinho, "GET", PREFIXO_CONSORCIO "/health", NULL, lamportIncrementar(), 0, resposta);
}

/*
 * GET /trechos: pede os trechos que o vizinho tem entre origem e destino.
 *   - visitados: lista (CSV) de servidores já percorridos, evita ciclos na busca
 */
int clienteTrechos(const char *idVizinho, const char *origem, const char *destino, const char *visitados, Resposta *resposta) {
    char *o = escapar(origem), *d = escapar(destino), *vis = escapar(visitados);
    char caminho[1024];
    
    snprintf(caminho, sizeof caminho, PREFIXO_CONSORCIO "/trechos?origem=%s&destino=%s&visitados=%s", o, d, vis);
    
    free(o); free(d); free(vis);
    return requisicao(idVizinho, "GET", caminho, NULL, lamportIncrementar(), 0, resposta);
}

/*
 * GET /reservas: lista as reservas guardadas no vizinho.
 */
int clienteReservas(const char *idVizinho, Resposta *resposta) {
    return requisicao(idVizinho, "GET", PREFIXO_CONSORCIO "/reservas", NULL, lamportIncrementar(), 0, resposta);
}

/*
 * POST /reservas/prepare: fase 1 do 2PC com o corpo JSON já montado.
 *   - timeoutMs: prazo próprio para essa chamada (<= 0 usa o padrão)
 */
int clientePrepararCorpo(const char *idVizinho, const char *corpo, long timeoutMs, Resposta *resposta) {
    return requisicao(idVizinho, "POST", PREFIXO_CONSORCIO "/reservas/prepare", corpo, lamportIncrementar(), timeoutMs, resposta);
}

/*
 * POST /reservas/prepare com um corpo mínimo montado aqui (um único trecho do vizinho).
 * O passageiro é fixo ("teste"), então serve para testes de rede, não para
 * reservas reais (essas usam clientePrepararCorpo).
 */
int clientePreparar(const char *idVizinho, const char *idTransacao, const char *idCarona, Resposta *resposta) {
    int relogio = lamportIncrementar();
    Buffer buffer; 
    bufferIniciar(&buffer);

    bufferAdicionar(&buffer, "{\"id_transacao\": ");    
    bufferAdicionarStringJSON(&buffer, idTransacao);
    
    bufferAdicionar(&buffer, ", \"coordenador_id\": "); 
    bufferAdicionarStringJSON(&buffer, servidor.id);
    
    bufferAdicionar(&buffer, ", \"passageiro\": \"teste\", \"relogio_logico\": %d, \"trechos_solicitados\": [{\"servidor\": ", relogio);
    bufferAdicionarStringJSON(&buffer, idVizinho);
    
    bufferAdicionar(&buffer, ", \"id_carona\": ");      
    bufferAdicionarStringJSON(&buffer, idCarona);
    bufferAdicionar(&buffer, "}]}");

    int rc = requisicao(idVizinho, "POST", PREFIXO_CONSORCIO "/reservas/prepare", buffer.p, relogio, 0, resposta);
    bufferLiberar(&buffer);
    return rc;
}

/*
 * Auxiliar das rotas /reservas/{id}<sufixo>: monta o caminho com o id escapado.
 *   - sufixo: "/commit", "/status" ou "" (para DELETE da própria reserva)
 */
static int reservaPorID(const char *id, const char *metodo, const char *sufixo, const char *idTransacao, Resposta *resposta) {
    char *t = escapar(idTransacao);
    char caminho[512];
    
    snprintf(caminho, sizeof caminho, PREFIXO_CONSORCIO "/reservas/%s%s", t, sufixo);
    free(t);

    return requisicao(id, metodo, caminho, NULL, lamportIncrementar(), 0, resposta);
}

/*
 * PUT /reservas/{id}/commit: fase 2 do 2PC, confirma a reserva.
 */
int clienteCommitar(const char *idVizinho, const char *idTransacao, Resposta *resposta) {
    return reservaPorID(idVizinho, "PUT", "/commit", idTransacao, resposta);
}

/*
 * DELETE /reservas/{id}: fase 2 do 2PC, desfaz a reserva preparada.
 */
int clienteRollback(const char *idVizinho, const char *idTransacao, Resposta *resposta) {
    return reservaPorID(idVizinho, "DELETE", "", idTransacao, resposta);
}

/*
 * GET /reservas/{id}/status: consulta o estado da reserva no vizinho.
 */
int clienteStatus(const char *idVizinho, const char *idTransacao, Resposta *resposta) {
    return reservaPorID(idVizinho, "GET", "/status", idTransacao, resposta);
}

/*
 * PUT /reservas/{id}/cancel: pede o cancelamento de uma reserva confirmada.
 * O prazo é maior (8 s) porque o cancelamento se propaga pela cadeia de servidores.
 */
int clienteCancelar(const char *idVizinho, const char *idTransacao, Resposta *resposta) {
    char *t = escapar(idTransacao);
    char caminho[512];
    
    snprintf(caminho, sizeof caminho, PREFIXO_CONSORCIO "/reservas/%s/cancel", t);
    free(t);

    return requisicao(idVizinho, "PUT", caminho, NULL, lamportIncrementar(), 8000, resposta);
}
