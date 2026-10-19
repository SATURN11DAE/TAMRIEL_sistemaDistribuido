#ifndef HTTP_CLIENTE_H
#define HTTP_CLIENTE_H

typedef struct {
    long codigoHTTP;
    char *corpo;
    int timeout;
    char erro[128];
} Resposta;

void respostaLiberar(Resposta *resposta);
int clienteSuspeito(const char *idVizinho);

int clienteSaude   (const char *idVizinho, Resposta *resposta);
int clienteTrechos (const char *idVizinho, const char *origem, const char *destino,
const char *visitados, Resposta *resposta);
int clienteReservas(const char *idVizinho, Resposta *resposta);

int clientePrepararCorpo(const char *idVizinho, const char *corpo, long timeoutMs, Resposta *resposta);
int clientePreparar(const char *idVizinho, const char *idTransacao,
const char *idCarona, Resposta *resposta);
int clienteCommitar(const char *idVizinho, const char *idTransacao, Resposta *resposta);
int clienteRollback(const char *idVizinho, const char *idTransacao, Resposta *resposta);
int clienteStatus  (const char *idVizinho, const char *idTransacao, Resposta *resposta);
int clienteCancelar(const char *idVizinho, const char *idTransacao, Resposta *resposta);

#endif
