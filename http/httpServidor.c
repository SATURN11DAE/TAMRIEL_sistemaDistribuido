/*
 * Implementação do Servidor HTTP
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "headers/lamport.h"
#include "headers/configuracoes.h"
#include "headers/util.h"
#include "headers/persistencia.h"
#include "headers/auth.h"

#include "headers/busca.h"
#include "headers/cadastro.h"
#include "headers/transacao.h"

#include "headers/httpServidor.h"

/*
 * Contexto de UMA conexão: o corpo da requisição vai chegando em pedaços e
 * é acumulado aqui. Criado na 1ª chamada de tratar() e liberado em finalizar().
 */
typedef struct {
    char  *corpo;
    size_t tamanho;
} Contexto;

/* Funções Base de Resposta HTTP */

/*
 * Envia a resposta ao cliente e a libera.
 * Acrescenta os cabeçalhos Content-Type, X-Server-Id, X-Lamport-Clock e CORS.
 *   - r: resposta a enviar; r.json é liberado aqui (dê o free só uma vez)
 * Se r.json for NULL (faltou memória), responde 500.
 */
static enum MHD_Result enviar(struct MHD_Connection *conexao, RespostaServidor r) {
    const char *json = r.json ? r.json : "{\"erro\": \"Sem memoria\"}\n";
    unsigned int status = r.json ? r.http : 500;

    struct MHD_Response *resposta = MHD_create_response_from_buffer(strlen(json), (void *) json, MHD_RESPMEM_MUST_COPY);
    char clock[32];
    snprintf(clock, sizeof clock, "%d", lamportObterValor());

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

/*
 * Atalho: envia uma resposta de erro {"erro": "msg"} com o status dado.
 */
static enum MHD_Result erro(struct MHD_Connection *conexao, unsigned int status, const char *msg) {
    return enviar(conexao, criarRespostaErro(status, msg));
}

/*
 * Lê um parâmetro da URL (?nome=valor). Retorna NULL se não existir.
 * O texto pertence ao libmicrohttpd, não libere.
 */
static const char *arg(struct MHD_Connection *c, const char *nome) {
    return MHD_lookup_connection_value(c, MHD_GET_ARGUMENT_KIND, nome);
}

/*
 * Monta a resposta de /health: o status do servidor e o valor do relógio de Lamport.
 */
static RespostaServidor estaVivo(void) {
    Buffer buffer; 
    bufferIniciar(&buffer);
    bufferAdicionar(&buffer, "{\"status\": ");
    
    char msg[100];
    snprintf(msg, sizeof msg, "%s ta on", servidor.id);
    
    bufferAdicionarStringJSON(&buffer, msg);
    bufferAdicionar(&buffer, ", \"relogio\": %d}\n", lamportObterValor());
    
    return criarRespostaJSON(200, bufferExtrairTexto(&buffer));
}

/* Lógica de Casamento de Rotas e Identificadores */

/*
 * Reconhece o caminho "/reservas/<id><sufixo>" e extrai o id.
 *   - sub: caminho depois do prefixo (ex.: "/reservas/abc/commit")
 *   - sufixo: "/commit", "/cancel", "/status" ou "" (só o id)
 *   - id / n: buffer de saída e seu tamanho
 * Retorna 1 se o caminho casa e o id é válido (identificadorValido), 0 caso contrário.
 */
static int casarReserva(const char *sub, const char *sufixo, char *id, size_t n) {
    const char *prefixo = "/reservas/";
    if (strncmp(sub, prefixo, strlen(prefixo)) != 0) return 0;

    const char *p = sub + strlen(prefixo);
    size_t tamanhoTotal = strlen(p);
    size_t tamanhoSufixo = strlen(sufixo);
    
    if (tamanhoTotal <= tamanhoSufixo || strcmp(p + tamanhoTotal - tamanhoSufixo, sufixo) != 0) return 0;

    size_t tamanhoId = tamanhoTotal - tamanhoSufixo;
    if (tamanhoId >= n) return 0;

    memcpy(id, p, tamanhoId);
    id[tamanhoId] = '\0';

    return strchr(id, '/') == NULL && identificadorValido(id);
}

/*
 * Reconhece o caminho "<prefixo><id>" (ex.: "/caronas/abc") e extrai o id.
 * Retorna 1 se casa e o id é válido (identificadorValido), 0 caso contrário.
 */
static int casarRecurso(const char *sub, const char *prefixo, char *id, size_t n) {
    size_t pl = strlen(prefixo);
    if (strncmp(sub, prefixo, pl) != 0) return 0;

    const char *p = sub + pl;
    size_t il = strlen(p);
    if (il == 0 || il >= n) return 0;

    memcpy(id, p, il + 1);
    return strchr(id, '/') == NULL && identificadorValido(id);
}

/* Roteadores Específicos */

/*
 * Roteador de /api/consorcio (comunicação entre servidores).
 *   - sub: caminho depois do prefixo
 *   - remetente: valor do cabeçalho X-Server-Id (quem enviou)
 * Rotas:
 *   GET    /health, /vivo              estaVivo
 *   GET    /trechos                    buscaTrechos
 *   GET    /reservas                   lista as reservas deste servidor
 *   POST   /reservas/prepare           txPrepare
 *   PUT    /reservas/{id}/commit       txCommit
 *   PUT    /reservas/{id}/cancel       txCancel
 *   GET    /reservas/{id}/status       txStatus
 *   DELETE /reservas/{id}              txRollback
 * Qualquer outra combinação devolve 404.
 */
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
    
    if (get && strcmp(sub, "/reservas") == 0) return enviar(c, criarRespostaJSON(200, armazenamentoListarJSON()));
    if (post && strcmp(sub, "/reservas/prepare") == 0) return enviar(c, txPrepare(ctx->corpo, remetente));
    if (put && casarReserva(sub, "/commit", id, sizeof id)) return enviar(c, txCommit(id));
    if (put && casarReserva(sub, "/cancel", id, sizeof id)) return enviar(c, txCancel(id));
    if (get && casarReserva(sub, "/status", id, sizeof id)) return enviar(c, txStatus(id));
    if (del && casarReserva(sub, "", id, sizeof id)) return enviar(c, txRollback(id));

    return erro(c, 404, "Rota ou metodo inexistente em /api/consorcio");
}

/*
 * Monta a resposta de GET /eu: id, nome, tipo do usuário e o servidor atual.
 */
static RespostaServidor respEu(const Usuario *u) {
    Buffer buffer; 
    bufferIniciar(&buffer);
    
    bufferAdicionar(&buffer, "{\"id\": ");   
    bufferAdicionarStringJSON(&buffer, u->id);
    
    bufferAdicionar(&buffer, ", \"nome\": "); 
    bufferAdicionarStringJSON(&buffer, u->nome);
    
    bufferAdicionar(&buffer, ", \"tipo\": "); 
    bufferAdicionarStringJSON(&buffer, u->tipo);
    
    bufferAdicionar(&buffer, ", \"servidor\": "); 
    bufferAdicionarStringJSON(&buffer, servidor.id);
    
    bufferAdicionar(&buffer, "}\n");
    return criarRespostaJSON(200, bufferExtrairTexto(&buffer));
}

/*
 * Roteador de /api/cliente (usado por motoristas e passageiros).
 *   - sub: caminho depois do prefixo
 * Só POST /usuarios é público; todas as outras rotas exigem HTTP Basic.
 * Rotas:
 *   POST   /usuarios                   cadastroUsuario
 *   GET    /eu                         dados do usuário logado
 *   GET    /itinerarios                buscaItinerarios
 *   POST   /rotas                      (motorista) cadastroRota
 *   POST   /caronas                    (motorista) cadastroCaronaComo
 *   GET    /caronas                    (motorista) cadastroListarCaronasDe
 *   DELETE /caronas/{id}               (motorista) cadastroRemoverCarona
 *   POST   /reservas                   (passageiro) txReservarComo
 *   GET    /reservas                   (passageiro) reservasListarJSON
 *   DELETE /reservas/{id}              (passageiro) txCancelar
 * Perfil errado devolve 403; credenciais ruins, 401; rota desconhecida, 404.
 */
static enum MHD_Result rotasCliente(struct MHD_Connection *c, const char *sub, const char *metodo, const Contexto *ctx) {
    int get = strcmp(metodo, "GET") == 0, post = strcmp(metodo, "POST") == 0;
    int del = strcmp(metodo, "DELETE") == 0;
    char id[128];

    if (post && strcmp(sub, "/usuarios") == 0) return enviar(c, cadastroUsuario(ctx->corpo));

    Usuario eu;
    const char *auth = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Authorization");
    
    if (!authRequisicao(auth, &eu)) {
        return erro(c, 401, "Credenciais invalidas ou ausentes (HTTP Basic: id e senha)");
    }
    
    int motorista = strcmp(eu.tipo, TIPO_MOTORISTA) == 0;

    if (get && strcmp(sub, "/eu") == 0) return enviar(c, respEu(&eu));
    if (get && strcmp(sub, "/itinerarios") == 0) return enviar(c, buscaItinerarios(arg(c, "origem"), arg(c, "destino")));

    // =========================================================================
    // INÍCIO DA NOVA PARTE: Rota para receber o JSON completo com a viagem toda
    // =========================================================================
    if (strcmp(sub, "/rotas") == 0) {
        if (!motorista) return erro(c, 403, "Somente motoristas acessam /rotas");
        
        if (post) return enviar(c, cadastroRota(eu.id, ctx->corpo));
    }
    // =========================================================================
    // FIM DA NOVA PARTE
    // =========================================================================

    if (strcmp(sub, "/caronas") == 0 || strncmp(sub, "/caronas/", 9) == 0) {
        if (!motorista) return erro(c, 403, "Somente motoristas acessam /caronas");

        if (post && strcmp(sub, "/caronas") == 0) return enviar(c, cadastroCaronaComo(eu.id, ctx->corpo));
        if (get && strcmp(sub, "/caronas") == 0)  return enviar(c, cadastroListarCaronasDe(eu.id));
        if (del && casarRecurso(sub, "/caronas/", id, sizeof id)) return enviar(c, cadastroRemoverCarona(eu.id, id));
    }

    if (strcmp(sub, "/reservas") == 0 || strncmp(sub, "/reservas/", 10) == 0) {
        if (motorista) return erro(c, 403, "Somente passageiros acessam /reservas");

        if (post && strcmp(sub, "/reservas") == 0) return enviar(c, txReservarComo(eu.id, ctx->corpo));
        if (get && strcmp(sub, "/reservas") == 0)  return enviar(c, criarRespostaJSON(200, reservasListarJSON(eu.id, 1)));
        if (del && casarRecurso(sub, "/reservas/", id, sizeof id)) return enviar(c, txCancelar(id, eu.id));
    }

    return erro(c, 404, "Rota ou metodo inexistente em /api/cliente");
}

/* Dispatcher Central e Configuração do Daemon */

/*
 * Callback central do libmicrohttpd: chamado várias vezes por requisição.
 *   1ª chamada: cria o Contexto da conexão.
 *   Chamadas seguintes com dados: acumulam o corpo no Contexto.
 *   Última chamada (sem dados): sincroniza o relógio de Lamport, trata o
 *   OPTIONS (CORS) e encaminha para rotasConsorcio ou rotasCliente.
 * URLs fora de /api/consorcio e /api/cliente devolvem 404.
 */
static enum MHD_Result tratar(void *cls, struct MHD_Connection *conexao, const char *url,
                              const char *metodo, const char *versao, const char *dados, 
                              size_t *tamanhoDados, void **con_cls) {

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
        ctx->corpo[ctx->tamanho] = '\0';
        *tamanhoDados = 0;

        return MHD_YES;
    }

    const char *headerClock = MHD_lookup_connection_value(conexao, MHD_HEADER_KIND, "X-Lamport-Clock");
    const char *headerId = MHD_lookup_connection_value(conexao, MHD_HEADER_KIND, "X-Server-Id");

    int relogio = headerClock ? lamportSincronizar(atoi(headerClock)) : lamportIncrementar();
    
    printf("\n[SERVIDOR] <- %s %s de %s (relogio recebido %s, agora %d)\n",
           metodo, url, headerId ? headerId : "?", headerClock ? headerClock : "-", relogio);

    if (ctx->corpo) printf("[SERVIDOR]    corpo: %.400s\n", ctx->corpo);

    if (strcmp(metodo, "OPTIONS") == 0) {
        return enviar(conexao, criarRespostaJSON(200, strdup("{}")));
    }

    size_t tamanhoConsorcio = strlen(PREFIXO_CONSORCIO);
    size_t tamanhoCliente = strlen(PREFIXO_CLIENTE);

    if (strncmp(url, PREFIXO_CONSORCIO, tamanhoConsorcio) == 0 && (url[tamanhoConsorcio] == '/' || url[tamanhoConsorcio] == '\0')) {
        return rotasConsorcio(conexao, url + tamanhoConsorcio, metodo, ctx, headerId);
    }
    if (strncmp(url, PREFIXO_CLIENTE, tamanhoCliente) == 0 && (url[tamanhoCliente] == '/' || url[tamanhoCliente] == '\0')) {
        return rotasCliente(conexao, url + tamanhoCliente, metodo, ctx);
    }

    return erro(conexao, 404, "Rota fora de /api/consorcio e /api/cliente");
}

/*
 * Callback do libmicrohttpd chamado quando a conexão termina: libera o Contexto.
 */
static void finalizar(void *cls, struct MHD_Connection *c, void **con_cls, enum MHD_RequestTerminationCode t) {
    (void) cls; (void) c; (void) t;
    Contexto *ctx = (Contexto *) *con_cls;

    if (ctx) {
        free(ctx->corpo); 
        free(ctx);
    }
    *con_cls = NULL;
}

/*
 * Inicia o servidor HTTP em background (uma thread por conexão, timeout de 30 s).
 *   - porta: porta TCP onde escutar
 * Retorna o daemon criado, ou NULL se falhou (ex.: porta ocupada).
 */
struct MHD_Daemon *servidorHTTPIniciar(int porta) {
    return MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_THREAD_PER_CONNECTION,
                            (uint16_t) porta, NULL, NULL, &tratar, NULL,
                            MHD_OPTION_NOTIFY_COMPLETED, &finalizar, NULL,
                            MHD_OPTION_CONNECTION_TIMEOUT, 30u,
                            MHD_OPTION_END);
}

/*
 * Encerra o servidor e libera seus recursos. Aceita NULL.
 */
void servidorHTTPParar(struct MHD_Daemon *daemon) {
    if (daemon) MHD_stop_daemon(daemon);
}
