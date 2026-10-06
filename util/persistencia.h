#ifndef PERSISTENCIA_H
#define PERSISTENCIA_H

#include "configuracoes.h"

#define PASTA_JSON "json"

#define ARMAZENAMENTO_OK 0
#define ARMAZENAMENTO_NAO_ENCONTRADO 1
#define ARMAZENAMENTO_ERRO -1

typedef struct {
    char idTransacao[128];
    char estado[16];
    char coordenadorId[MAX_ID];
    char idCarona[64];
    int relogio;
} Reserva;

int armazenamentoIniciar(void);
int armazenamentoSalvar(const Reserva *reserva);
int armazenamentoBuscar(const char *idTransacao, Reserva *saida);
int armazenamentoAtualizarEstado(const char *idTransacao, const char *estado);
int armazenamentoRemover(const char *idTransacao);
char *armazenamentoListarJSON(void);
int armazenamentoQuantidade(void);
void armazenamentoEncerrar(void);

#endif
