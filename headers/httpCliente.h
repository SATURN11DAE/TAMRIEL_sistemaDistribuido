#ifndef HTTP_CLIENTE_H
#define HTTP_CLIENTE_H

/*
 * Cliente HTTP (Usando libcurl)
 *
 * Responsável por enviar requisições HTTP para os outros servidores
 * da rede (vizinhos). Lida com timeouts, repasses do relógio de Lamport
 * e marcação de vizinhos como "suspeitos" em caso de falhas de conexão.
 */

typedef struct {
    long codigoHTTP;    /* status HTTP (só vale se a requisição retornou 0) */
    char *corpo;        /* corpo da resposta, nunca NULL após a requisição (libere com respostaLiberar) */
    int falhaRede;      /* 1 se não houve resposta (timeout, conexão recusada, host inalcançável...) */
    char erro[128];     /* descrição do erro quando a requisição falha */
} Resposta;

/* Libera a memória alocada para o corpo da resposta. */
void respostaLiberar(Resposta *resposta);

/* Retorna 1 se o vizinho estiver marcado como suspeito devido a quedas recentes. */
int clienteSuspeito(const char *idVizinho);

/* ROTAS DO CONSÓRCIO (Comunicação entre servidores) */
int clienteSaude(const char *idVizinho, Resposta *resposta);
int clienteTrechos(const char *idVizinho, const char *origem, const char *destino, 
                    const char *visitados, Resposta *resposta);
int clienteReservas(const char *idVizinho, Resposta *resposta);
int clientePrepararCorpo(const char *idVizinho, const char *corpo,
                    long timeoutMs, Resposta *resposta);
int clientePreparar(const char *idVizinho, const char *idTransacao,
                    const char *idCarona, Resposta *resposta);
int clienteCommitar(const char *idVizinho, const char *idTransacao, Resposta *resposta);
int clienteRollback(const char *idVizinho, const char *idTransacao, Resposta *resposta);
int clienteStatus(const char *idVizinho, const char *idTransacao, Resposta *resposta);
int clienteCancelar(const char *idVizinho, const char *idTransacao, Resposta *resposta);

#endif
