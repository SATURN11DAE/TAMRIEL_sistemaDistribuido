#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/headers/configuracoes.h"
#include "util/headers/lamport.h"
#include "util/headers/util.h"
#include "util/headers/persistencia.h"
#include "util/headers/auth.h"
#include "consorcio/headers/busca.h"
#include "consorcio/headers/cadastro.h"
#include "consorcio/headers/transacao.h"

#include "headers/httpServidor.h"

typedef struct {
    char  *corpo;
    size_t tamanho;
} Contexto;

static enum MHD_Result enviar(struct MHD_Connection *conexao, Resp r) {
    const char *json = r.json ? r.json : "{\"erro\": \"Sem memoria\"}\n";
    unsigned int status = r.json ? r.http : 500;

    struct MHD_Response *resposta = MHD_create_response_from_buffer(strlen(json), (void *) json,
                                    MHD_RESPMEM_MUST_COPY);
    char clock[32];
    snprintf(clock, sizeof clock, "%d", lamportMostrar());

    MHD_add_response_header(resposta, "Content-Type", "application/json");
    MHD_add_response_header(resposta, "X-Server-Id", servidor.id);
    MHD_add_response_header(resposta, "X-Lamport-Clock", clock);

    MHD_add_response_header(resposta, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(resposta, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    MHD_add_response_header(resposta, "Access-Control-Allow-Headers", "Content-Type, Authorization, X-Server-Id, X-Lamport-Clock");


    enum MHD_Result retorno = MHD_queue_response(conexao, status, resposta);
    MHD_destroy_response(resposta);

    printf("[SERVIDOR] -> respondeu %u: %.300s", status, json);
    if (json[strlen(json) - 1] != '\n') printf("\n");

    free(r.json);
    return retorno;
}

static enum MHD_Result erro(struct MHD_Connection *conexao, unsigned int status, const char *msg) {
    return enviar(conexao, respErro(status, msg));
}

static const char *arg(struct MHD_Connection *c, const char *nome) {
    return MHD_lookup_connection_value(c, MHD_GET_ARGUMENT_KIND, nome);
}

static Resp estaVivo(void) {
    Buf b; bufIni(&b);
    bufAdd(&b, "{\"status\": ");
    char msg[100];
    snprintf(msg, sizeof msg, "%s ta on", servidor.id);
    bufStr(&b, msg);
    bufAdd(&b, ", \"relogio\": %d}\n", lamportMostrar());
    return respJSON(200, bufTomar(&b));
}

static int casarReserva(const char *sub, const char *sufixo, char *id, size_t n) {
    const char *prefixo = "/reservas/";
    if (strncmp(sub, prefixo, strlen(prefixo)) != 0) return 0;

    const char *p = sub + strlen(prefixo);
    size_t tl = strlen(p), sl = strlen(sufixo);
    if (tl <= sl || strcmp(p + tl - sl, sufixo) != 0) return 0;

    size_t il = tl - sl;
    if (il >= n) return 0;

    memcpy(id, p, il);
    id[il] = 0;

    return strchr(id, '/') == NULL && idValido(id);
}

static enum MHD_Result rotasConsorcio(struct MHD_Connection *c, const char *sub, const char *metodo,
                                      const Contexto *ctx, const char *remetente) {
    char id[128];
    int get = strcmp(metodo, "GET") == 0, post = strcmp(metodo, "POST") == 0;
    int put = strcmp(metodo, "PUT") == 0, del = strcmp(metodo, "DELETE") == 0;

    if (get && (strcmp(sub, "/health") == 0 || strcmp(sub, "/vivo") == 0)) return enviar(c, estaVivo());

    if (get && strcmp(sub, "/trechos") == 0) {
        const char *vis = arg(c, "visitados");
        return enviar(c, buscaTrechos(arg(c, "origem"), arg(c, "destino"), vis));
    }
    if (get && strcmp(sub, "/reservas") == 0) return enviar(c, respJSON(200, armazenamentoListarJSON()));
    if (post && strcmp(sub, "/reservas/prepare") == 0) return enviar(c, txPrepare(ctx->corpo, remetente));
    if (put && casarReserva(sub, "/commit", id, sizeof id)) return enviar(c, txCommit(id));
    if (put && casarReserva(sub, "/cancel", id, sizeof id)) return enviar(c, txCancel(id));
    if (get && casarReserva(sub, "/status", id, sizeof id)) return enviar(c, txStatus(id));
    if (del && casarReserva(sub, "", id, sizeof id)) return enviar(c, txRollback(id));

    return erro(c, 404, "Rota ou metodo inexistente em /api/consorcio");
}

static int casarRecurso(const char *sub, const char *prefixo, char *id, size_t n) {
    size_t pl = strlen(prefixo);
    if (strncmp(sub, prefixo, pl) != 0) return 0;

    const char *p = sub + pl;
    size_t il = strlen(p);
    if (il == 0 || il >= n) return 0;

    memcpy(id, p, il + 1);
    return strchr(id, '/') == NULL && idValido(id);
}

static Resp respEu(const Usuario *u) {
    Buf b; bufIni(&b);
    bufAdd(&b, "{\"id\": ");   bufStr(&b, u->id);
    bufAdd(&b, ", \"nome\": "); bufStr(&b, u->nome);
    bufAdd(&b, ", \"tipo\": "); bufStr(&b, u->tipo);
    bufAdd(&b, ", \"servidor\": "); bufStr(&b, servidor.id);
    bufAdd(&b, "}\n");
    return respJSON(200, bufTomar(&b));
}

static enum MHD_Result rotasCliente(struct MHD_Connection *c, const char *sub, const char *metodo,
                                    const Contexto *ctx) {
    int get = strcmp(metodo, "GET") == 0, post = strcmp(metodo, "POST") == 0;
    int del = strcmp(metodo, "DELETE") == 0;
    char id[128];

    if (post && strcmp(sub, "/usuarios") == 0) return enviar(c, cadastroUsuario(ctx->corpo));

    Usuario eu;
    const char *auth = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Authorization");
    if (!authRequisicao(auth, &eu)) {
        return erro(c, 401, "Credenciais invalidas ou ausentes (HTTP Basic: id e senha)");
    }
    int motorista = strcmp(eu.tipo, "motorista") == 0;

    if (get && strcmp(sub, "/eu") == 0) return enviar(c, respEu(&eu));
    if (get && strcmp(sub, "/itinerarios") == 0) return enviar(c, buscaItinerarios(arg(c, "origem"), arg(c, "destino")));

    if (strcmp(sub, "/caronas") == 0 || strncmp(sub, "/caronas/", 9) == 0) {
        if (!motorista) return erro(c, 403, "Somente motoristas acessam /caronas");

        if (post && strcmp(sub, "/caronas") == 0) return enviar(c, cadastroCaronaComo(eu.id, ctx->corpo));
        if (get && strcmp(sub, "/caronas") == 0)  return enviar(c, cadastroListarCaronasDe(eu.id));
        if (del && casarRecurso(sub, "/caronas/", id, sizeof id)) return enviar(c, cadastroRemoverCarona(eu.id, id));
    }

    if (strcmp(sub, "/reservas") == 0 || strncmp(sub, "/reservas/", 10) == 0) {
        if (motorista) return erro(c, 403, "Somente passageiros acessam /reservas");

        if (post && strcmp(sub, "/reservas") == 0) return enviar(c, txReservarComo(eu.id, ctx->corpo));
        if (get && strcmp(sub, "/reservas") == 0)  return enviar(c, respJSON(200, reservasListarJSON(eu.id, 1)));
        if (del && casarRecurso(sub, "/reservas/", id, sizeof id)) return enviar(c, txCancelar(id, eu.id));
    }

    return erro(c, 404, "Rota ou metodo inexistente em /api/cliente");
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

    if (ctx->corpo) printf("[SERVIDOR]    corpo: %.400s\n", ctx->corpo);

    if (strcmp(metodo, "OPTIONS") == 0) {
        return enviar(conexao, respJSON(200, strdup("{}")));
    }

    size_t pc = strlen(PREFIXO_CONSORCIO), pl = strlen(PREFIXO_CLIENTE);

    if (strncmp(url, PREFIXO_CONSORCIO, pc) == 0 && (url[pc] == '/' || url[pc] == 0)) {
        return rotasConsorcio(conexao, url + pc, metodo, ctx, hId);
    }
    if (strncmp(url, PREFIXO_CLIENTE, pl) == 0 && (url[pl] == '/' || url[pl] == 0)) {
        return rotasCliente(conexao, url + pl, metodo, ctx);
    }

    return erro(conexao, 404, "Rota fora de /api/consorcio e /api/cliente");
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
                            MHD_OPTION_NOTIFY_COMPLETED, &finalizar, NULL,
                            MHD_OPTION_CONNECTION_TIMEOUT, 30u,
                            MHD_OPTION_END);
}

void servidorHTTPParar(struct MHD_Daemon *satanas) {
    if (satanas) MHD_stop_daemon(satanas);
}
