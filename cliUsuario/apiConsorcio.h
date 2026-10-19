#ifndef API_CONSORCIO_H
#define API_CONSORCIO_H

#include "util/headers/json.h"

typedef struct {
    int   rede;
    long  http;
    char  erro[160];
    JVal *json;
} ApiResp;

int         apiOk(const ApiResp *r);
const char *apiMensagem(const ApiResp *r);
void        apiLiberar(ApiResp *r);

void        apiIniciar(const char *baseURL);
void        apiEncerrar(void);
const char *apiBaseURL(void);

ApiResp     apiCadastrar(const char *id, const char *nome, const char *tipo
const char *senha);
int         apiEntrar(const char *id, const char *senha, ApiResp *r);
void        apiSair(void);
int         apiLogado(void);
const char *apiUsuarioId(void);
const char *apiUsuarioNome(void);
const char *apiUsuarioTipo(void);
ApiResp     apiSaude(void);

ApiResp     apiPublicarCarona(const char *origem, const char *destino, const char *horario,
double preco, int assentos);
ApiResp     apiMinhasCaronas(void);
ApiResp     apiExcluirCarona(const char *idCarona);

ApiResp     apiBuscarItinerarios(const char *origem, const char *destino);
ApiResp     apiCancelarReserva(const char *idTransacao);

#endif
