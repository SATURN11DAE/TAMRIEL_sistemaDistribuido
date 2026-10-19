#ifndef PERSISTENCIA_H
#define PERSISTENCIA_H

#include "configuracoes.h"
#include "util.h"

#define PASTA_JSON "json"

#define ARMAZENAMENTO_OK              0
#define ARMAZENAMENTO_NAO_ENCONTRADO  1
#define ARMAZENAMENTO_INDISPONIVEL    2
#define ARMAZENAMENTO_CONFLITO        3
#define ARMAZENAMENTO_EXISTE          4
#define ARMAZENAMENTO_PROIBIDO        5
#define ARMAZENAMENTO_ERRO           -1

#define MAX_ASSENTOS 60

typedef struct {
    char id[64];
    char nome[96];
    char tipo[16];
    char salt[33];
    char hash[65];
} Usuario;

typedef struct {
    char id[64];
    char motorista[64];
    char origem[96];
    char destino[96];
    char horario[32];
    double preco;
    int assentos;
    int livres;
} Carona;

typedef struct {
    char servidor[MAX_ID];
    char idCarona[64];
    int assento;
} TrechoRes;

typedef struct {
    char idTransacao[128];
    char estado[16];
    char coordenadorId[MAX_ID];
    char anterior[MAX_ID];
    char proximo[MAX_ID];
    char passageiro[64];
    int  relogio;
    long criadoEm;
    int  qtdLocais;
    TrechoRes locais[MAX_TRECHOS_TX];
    int  qtdItin;
    TrechoRes itin[MAX_TRECHOS_TX];

    int  ativo;
    int  jaExistia;
    char falha[64];
} Reserva;

int  armazenamentoIniciar(void);
void armazenamentoEncerrar(void);
int  armazenamentoQuantidade(void);

int  usuarioSalvar(const Usuario *u);
int  usuarioBuscar(const char *id, Usuario *saida);
char *usuariosListarJSON(void);

int  caronaSalvar(const Carona *c);
int  caronaBuscar(const char *id, Carona *saida);
int  caronaListar(const char *origem, Carona **lista, int *qtd);
char *caronasListarJSON(void);
char *caronasListarJSONDe(const char *motorista);
int  caronaRemover(const char *id, const char *motorista);
void caronaJSON(Buf *b, const Carona *c, int incluirLivres);

int  reservaPreparar(Reserva *r, int esperaMs);
int  reservaBuscar(const char *id, Reserva *saida);
int  reservaDecidir(const char *id, const char *estado, const TrechoRes *itin, int qtdItin);
int  reservaApagar(const char *id);
int  reservaListarPorEstado(const char *estado, Reserva **lista, int *qtd);
int  reservaRemover(const char *id);
void reservaAtivo(const char *id, int ativo);
int  reservaPendentes(int idadeMinS, Reserva **lista, int *qtd);
char *reservasListarJSON(const char *passageiro, int soCoordenador);
char *armazenamentoListarJSON(void);

#endif
