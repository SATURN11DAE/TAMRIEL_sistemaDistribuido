#ifndef API_CONSORCIO_H
#define API_CONSORCIO_H

/*
 * API Consórcio (Cliente)
 *
 * Provê funções de alto nível para acessar os endpoints REST do servidor.
 * Usada pela interface de terminal (terminal.c) para fazer login,
 * buscar viagens, reservar assentos e publicar caronas.
 */

#include "configuracoes.h"
#include "json.h"

typedef struct {
    int   rede;         /* 1 se a conexão de rede foi bem sucedida, 0 caso contrário */
    long  http;         /* Código de status HTTP retornado pelo servidor */
    char  erro[160];    /* Descrição do erro em caso de falha de rede */
    ValorJSON *json;    /* Corpo da resposta já parseado em JSON (se houver) */
} ApiResp;

/* Funções Base */
int         apiOk(const ApiResp *resposta);
const char *apiMensagem(const ApiResp *resposta);
void        apiLiberar(ApiResp *resposta);   /* libera o JSON da resposta */

void        apiIniciar(const char *baseURL);
void        apiEncerrar(void);
const char *apiBaseURL(void);

/* Autenticação e Conta */
ApiResp     apiCadastrar(const char *id, const char *nome, const char *tipo, const char *senha);
int         apiEntrar(const char *id, const char *senha, ApiResp *resposta);
void        apiSair(void);
const char *apiUsuarioId(void);
const char *apiUsuarioNome(void);
const char *apiUsuarioTipo(void);
ApiResp     apiSaude(void);

/* Motorista */
/* Publica UMA carona com varias paradas; horarios/precos/assentos tem (n - 1) itens (um por trecho). */
ApiResp     apiPublicarRota(int numCidades, char (*cidades)[TAM_CIDADE], char (*horarios)[TAM_HORARIO],
const double *precos, const int *assentos);

ApiResp     apiMinhasCaronas(void);
ApiResp     apiExcluirCarona(const char *idCarona);

/* Passageiro */
ApiResp     apiBuscarItinerarios(const char *origem, const char *destino);
ApiResp     apiReservar(const ValorJSON *itinerario);
ApiResp     apiMinhasReservas(void);
ApiResp     apiCancelarReserva(const char *idTransacao);

#endif
