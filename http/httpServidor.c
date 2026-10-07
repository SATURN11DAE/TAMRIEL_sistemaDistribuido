#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/headers/configuracoes.h"
#include "util/headers/lamport.h"
#include "util/headers/util.h"
#include "util/headers/persistencia.h"

#include "headers/httpServidor.h"
 
typedef struct {
    char  *corpo;
    size_t tamanho;
} Contexto;
 
static enum MHD_Result responder(struct MHD_Connection *conexao, unsigned int status, const char *json) {
    struct MHD_Response *resposta = MHD_create_response_from_buffer(strlen(json), (void *) json,
                                    MHD_RESPMEM_MUST_COPY);
    char clock[32];
    snprintf(clock, sizeof clock, "%d", lamportMostrar());
 
    MHD_add_response_header(resposta, "Content-Type", "application/json");
    MHD_add_response_header(resposta, "X-Server-Id", servidor.id);
    MHD_add_response_header(resposta, "X-Lamport-Clock", clock);
 
    enum MHD_Result retorno = MHD_queue_response(conexao, status, resposta);
    MHD_destroy_response(resposta);
    printf("[SERVIDOR] -> respondeu %u: %s", status, json);

    if (json[strlen(json) - 1] != '\n') printf("\n");
    return retorno;
}

static enum MHD_Result erro(struct MHD_Connection *conexao, unsigned int status, const char *msg) {
    char m[256], json[400];
    jsonEscape(msg, m, sizeof m);
    snprintf(json, sizeof json, "{\"erro\": \"%s\"}\n", m);

    return responder(conexao, status, json);
}
 
// GET /health
static enum MHD_Result estaVivo(struct MHD_Connection *conexao) {
    char json[200];
    snprintf(json, sizeof json, "{\"status\": \"%s ta on\", \"relogio\": %d}\n",
                                servidor.id, lamportMostrar());

    return responder(conexao, MHD_HTTP_OK, json);
}

// GET /trechos?origem=&destino=&visitados=a,b
static enum MHD_Result buscarTrechos(struct MHD_Connection *conexao) {
    const char *origem  = MHD_lookup_connection_value(conexao, MHD_GET_ARGUMENT_KIND, "origem");
    const char *destino = MHD_lookup_connection_value(conexao, MHD_GET_ARGUMENT_KIND, "destino");
    const char *visit   = MHD_lookup_connection_value(conexao, MHD_GET_ARGUMENT_KIND, "visitados");

    if (!origem || !destino) return erro(conexao, MHD_HTTP_BAD_REQUEST,
        "Informe ?origem=...&destino=...");
    printf("[SERVIDOR]    busca %s -> %s (visitados: %s)\n", origem, destino, visit ? visit : "-");
 
    char o[256], d[256], id[MAX_ID * 2], json[1024];
    jsonEscape(origem, o, sizeof o);
    jsonEscape(destino, d, sizeof d);
    jsonEscape(servidor.id, id, sizeof id);
    snprintf(json, sizeof json,
            "{\"itinerario_id\": \"stub\", \"trechos\": [{\"servidor\": \"%s\", \"id_carona\": \"stub-01\", "
            "\"origem\": \"%s\", \"destino\": \"%s\", \"preco\": 0.0}]}\n", id, o, d);

    return responder(conexao, MHD_HTTP_OK, json);
}

// POST /reservas/prepare
static enum MHD_Result preparar(struct MHD_Connection *conexao, const Contexto *ctx) {
    Reserva reserva;
    memset(&reserva, 0, sizeof reserva);

    if (!ctx->corpo || !jsonExtrairString(ctx->corpo, "id_transacao", reserva.idTransacao,
                                          sizeof reserva.idTransacao)) {
        return erro(conexao, MHD_HTTP_BAD_REQUEST, "Corpo sem id_transacao");
    }

    strcpy(reserva.coordenadorId, "?");
    jsonExtrairString(ctx->corpo, "coordenador_id", reserva.coordenadorId, sizeof reserva.coordenadorId);
    jsonExtrairString(ctx->corpo, "id_carona", reserva.idCarona, sizeof reserva.idCarona);
    reserva.relogio = jsonExtrairInt(ctx->corpo, "relogio_logico");
    strcpy(reserva.estado, "PREPARADO");

    printf("[SERVIDOR]    PREPARE tx=%s coordenador=%s\n", reserva.idTransacao, reserva.coordenadorId);

    // Reenvio da mesma transacao nao duplica nem rebaixa o estado (idempotente)
    Reserva existente;
    if (armazenamentoBuscar(reserva.idTransacao, &existente) == ARMAZENAMENTO_OK) {
        printf("[SERVIDOR]    tx ja existe (%s), mantendo.\n", existente.estado);
    } else if (armazenamentoSalvar(&reserva) != ARMAZENAMENTO_OK) {
        return erro(conexao, MHD_HTTP_INTERNAL_SERVER_ERROR, "Falha ao gravar a reserva em disco");
    }

    char t[256], json[512];
    jsonEscape(reserva.idTransacao, t, sizeof t);
    snprintf(json, sizeof json, "{\"id_transacao\": \"%s\", \"status\": \"READY\"}\n", t);

    return responder(conexao, MHD_HTTP_OK, json);
}

// PUT /reservas/{id}/commit
static enum MHD_Result commitar(struct MHD_Connection *conexao, const char *idTransacao) {
    printf("[SERVIDOR]    COMMIT tx=%s\n", idTransacao);

    int retorno = armazenamentoAtualizarEstado(idTransacao, "COMITADO");
    if (retorno == ARMAZENAMENTO_NAO_ENCONTRADO) {
        return erro(conexao, MHD_HTTP_NOT_FOUND, "Transacao desconhecida (nao houve prepare)");
    }
    if (retorno == ARMAZENAMENTO_ERRO) {
        return erro(conexao, MHD_HTTP_INTERNAL_SERVER_ERROR, "Falha ao gravar a reserva em disco");
    }

    char t[256], json[512];
    jsonEscape(idTransacao, t, sizeof t);
    snprintf(json, sizeof json,
    "{\"id_transacao\": \"%s\", \"mensagem\": \"Transacao COMMITTED com sucesso.\"}\n", t);

    return responder(conexao, MHD_HTTP_OK, json);
}

// DELETE /reservas/{id}  (idempotente: apagar algo que nao existe tambem da 200)
static enum MHD_Result rollback(struct MHD_Connection *conexao, const char *idTransacao) {
    printf("[SERVIDOR]    ROLLBACK tx=%s\n", idTransacao);

    if (armazenamentoRemover(idTransacao) == ARMAZENAMENTO_ERRO) {
        return erro(conexao, MHD_HTTP_INTERNAL_SERVER_ERROR, "Falha ao gravar a remocao em disco");
    }

    char t[256], json[512];
    jsonEscape(idTransacao, t, sizeof t);
    snprintf(json, sizeof json,
    "{\"id_transacao\": \"%s\", \"mensagem\": \"Transacao ABORTED com sucesso.\"}\n", t);

    return responder(conexao, MHD_HTTP_OK, json);
}

// GET /reservas/{id}/status
static enum MHD_Result status(struct MHD_Connection *conexao, const char *idTransacao) {
    printf("[SERVIDOR]    STATUS tx=%s\n", idTransacao);

    Reserva reserva;
    if (armazenamentoBuscar(idTransacao, &reserva) != ARMAZENAMENTO_OK) {
        return erro(conexao, MHD_HTTP_NOT_FOUND, "Transacao desconhecida");
    }

    char t[256], e[64], json[512];
    jsonEscape(reserva.idTransacao, t, sizeof t);
    jsonEscape(reserva.estado, e, sizeof e);
    snprintf(json, sizeof json, "{\"id_transacao\": \"%s\", \"status\": \"%s\"}\n", t, e);

    return responder(conexao, MHD_HTTP_OK, json);
}

// GET /reservas  (lista tudo que este servidor tem guardado)
static enum MHD_Result listarReservas(struct MHD_Connection *conexao) {
    char *json = armazenamentoListarJSON();
    if (!json) return erro(conexao, MHD_HTTP_INTERNAL_SERVER_ERROR, "Sem memoria");

    enum MHD_Result retorno = responder(conexao, MHD_HTTP_OK, json);
    free(json);

    return retorno;
}

/* Roteamento
Casa "/reservas/<id><sufixo>" e extrai o id. Ex.: sufixo "/commit", ou "" para DELETE.*/
static int vendaCasada(const char *sub, const char *sufixo, char *id, size_t n) {
    const char *prefixo = "/reservas/";
    if (strncmp(sub, prefixo, strlen(prefixo)) != 0) return 0;

    const char *p = sub + strlen(prefixo);
    size_t tl = strlen(p), sl = strlen(sufixo);
    if (tl <= sl || strcmp(p + tl - sl, sufixo) != 0) return 0;

    size_t il = tl - sl;
    if (il >= n) return 0;

    memcpy(id, p, il);
    id[il] = 0;

    return strchr(id, '/') == NULL;
}

static enum MHD_Result tratar(void *cls, struct MHD_Connection *conexao, const char *url,
    const char *metodo, const char *versao, const char *dados, size_t *tamanhoDados,
    void **con_cls) {

    (void) cls; (void) versao;

    if (*con_cls == NULL) {
        *con_cls = calloc(1, sizeof(Contexto));
        return *con_cls ? MHD_YES : MHD_NO;
    }
    Contexto *ctx = (Contexto *) *con_cls;
 
    if (*tamanhoDados > 0) {
        char *novo = realloc(ctx->corpo, ctx->tamanho + *tamanhoDados + 1);
        if (!novo) return MHD_NO;
        ctx->corpo = novo;

        memcpy(ctx->corpo + ctx->tamanho, dados, *tamanhoDados);
        ctx->tamanho += *tamanhoDados;
        ctx->corpo[ctx->tamanho] = 0;
        *tamanhoDados = 0;

        return MHD_YES;
    }

    const char *hClock = MHD_lookup_connection_value(conexao, MHD_HEADER_KIND, "X-Lamport-Clock");
    const char *hId = MHD_lookup_connection_value(conexao, MHD_HEADER_KIND, "X-Server-Id");

    int relogio = hClock ? lamportAtualizar(atoi(hClock)) : lamportTique();
    printf("\n[SERVIDOR] <- %s %s de %s (relogio recebido %s, agora %d)\n",
           metodo, url, hId ? hId : "?", hClock ? hClock : "-", relogio);

    if (ctx->corpo) printf("[SERVIDOR]    corpo: %s\n", ctx->corpo);
 
    size_t pl = strlen(PREFIXO_CONSORCIO);
    if (strncmp(url, PREFIXO_CONSORCIO, pl) != 0) {
        return erro(conexao, MHD_HTTP_NOT_FOUND, "Rota fora de /api/consorcio");
    }
    const char *sub = url + pl;
    char id[128];
 
    int get = strcmp(metodo, "GET") == 0, post = strcmp(metodo, "POST") == 0;
    int put = strcmp(metodo, "PUT") == 0, del = strcmp(metodo, "DELETE") == 0;
 
    if (get && (strcmp(sub, "/health") == 0 || strcmp(sub, "/vivo") == 0)) return estaVivo(conexao);
    if (get && strcmp(sub, "/trechos") == 0) return buscarTrechos(conexao);
    if (get && strcmp(sub, "/reservas") == 0) return listarReservas(conexao);
    if (post && strcmp(sub, "/reservas/prepare") == 0) return preparar(conexao, ctx);
    if (put && vendaCasada(sub, "/commit", id, sizeof id)) return commitar(conexao, id);
    if (get && vendaCasada(sub, "/status", id, sizeof id)) return status(conexao, id);
    if (del && vendaCasada(sub, "", id, sizeof id)) return rollback(conexao, id);

    return erro(conexao, MHD_HTTP_NOT_FOUND, "Rota ou metodo inexistente");
}

static void finalizar(void *cls, struct MHD_Connection *c, void **con_cls,
enum MHD_RequestTerminationCode t) {

    (void) cls; (void) c; (void) t;
    Contexto *ctx = (Contexto *) *con_cls;

    if (ctx) {free(ctx->corpo); free(ctx);}
    *con_cls = NULL;
}
 
struct MHD_Daemon *servidorHTTPIniciar(int porta) {
    return MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_THREAD_PER_CONNECTION,
                            (uint16_t) porta, NULL, NULL, &tratar, NULL,
                            MHD_OPTION_NOTIFY_COMPLETED, &finalizar, NULL, MHD_OPTION_END);
}

void servidorHTTPParar(struct MHD_Daemon *satanas) {
    if (satanas) MHD_stop_daemon(satanas);
}
