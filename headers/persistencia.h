#ifndef PERSISTENCIA_H
#define PERSISTENCIA_H

/*
 * Persistência de Dados.
 *
 * Gerencia o armazenamento em memória e em disco (arquivos JSON)
 * dos usuários, caronas e reservas do sistema. Também lida com a
 * concorrência através de travas (mutexes) para garantir consistência.
 *
 * MODELO DE CARONA: uma carona é uma ROTA (cidades[0..n-1]) com n-1 trechos.
 * Cada trecho i vai de cidades[i] a cidades[i+1] e tem horário, preço e
 * capacidade próprios. Uma reserva pega uma FAIXA da rota (origem -> destino),
 * e o assento escolhido fica livre/ocupado apenas nos trechos dessa faixa.
 */

#include "configuracoes.h"
#include "util.h"

#define PASTA_JSON "json"

/* Códigos de retorno do armazenamento */
#define ARMAZENAMENTO_OK              0
#define ARMAZENAMENTO_NAO_ENCONTRADO  1
#define ARMAZENAMENTO_INDISPONIVEL    2
#define ARMAZENAMENTO_CONFLITO        3
#define ARMAZENAMENTO_EXISTE          4
#define ARMAZENAMENTO_PROIBIDO        5
#define ARMAZENAMENTO_INVALIDO        6   /* origem/destino fora da rota da carona */
#define ARMAZENAMENTO_ERRO           -1

/* Limites da rota */
#define MAX_ASSENTOS 60
#define MAX_CIDADES_ROTA 10
#define MAX_TRECHOS_ROTA (MAX_CIDADES_ROTA - 1)

/* Tipos de usuário */
#define TIPO_MOTORISTA   "motorista"
#define TIPO_PASSAGEIRO  "passageiro"

/* Estados de uma reserva (campo Reserva.estado) */
#define ESTADO_PREPARADO   "PREPARADO"    /* assento guardado, aguardando a decisão do coordenador */
#define ESTADO_COMITADO    "COMITADO"     /* reserva confirmada */
#define ESTADO_CANCELANDO  "CANCELANDO"   /* cancelamento decidido, ainda propagando aos outros servidores */

/* ESTRUTURAS DE DADOS */

typedef struct {
    char id[TAM_ID_ENTIDADE];
    char nome[96];
    char tipo[16];                                  /* TIPO_MOTORISTA ou TIPO_PASSAGEIRO */
    char salt[33];                                  /* hexadecimal */
    char hash[65];                                  /* hexadecimal */
} Usuario;

typedef struct {
    char   id[TAM_ID_ENTIDADE];
    char   motorista[TAM_ID_ENTIDADE];
    int    qtdCidades;                              /* >= 2 */
    char   cidades[MAX_CIDADES_ROTA][TAM_CIDADE];
    char   horarios[MAX_TRECHOS_ROTA][TAM_HORARIO]; /* horário de saída de cidades[i] */
    double precos[MAX_TRECHOS_ROTA];                /* preço de cidades[i] -> cidades[i+1] */
    int    assentos[MAX_TRECHOS_ROTA];              /* capacidade do trecho i */
    int    livres[MAX_TRECHOS_ROTA];                /* calculado (não persistido) */
} Carona;

typedef struct {
    char servidor[MAX_ID];
    char idCarona[TAM_ID_ENTIDADE];
    char origem[TAM_CIDADE];     /* cidade de embarque (nome, dentro da rota da carona) */
    char destino[TAM_CIDADE];    /* cidade de desembarque */
    int  de;                     /* índice de 'origem' na rota (resolvido no prepare) */
    int  ate;                    /* índice de 'destino' na rota (resolvido no prepare) */
    int  assento;
} TrechoRes;

typedef struct {
    char idTransacao[128];
    char estado[16];             /* ESTADO_PREPARADO, ESTADO_COMITADO ou ESTADO_CANCELANDO */
    char coordenadorId[MAX_ID];  /* servidor que iniciou a transação */
    char anterior[MAX_ID];       /* servidor que enviou o prepare a este */
    char proximo[MAX_ID];        /* próximo servidor da cadeia ("" se este for o último) */
    char passageiro[TAM_ID_ENTIDADE];
    int  relogio;                /* relógio de Lamport do início da transação */
    long criadoEm;               /* segundos desde a época (Unix) */
    int  qtdLocais;
    TrechoRes locais[MAX_TRECHOS_TX];   /* trechos que pertencem a ESTE servidor */
    int  qtdItin;
    TrechoRes itin[MAX_TRECHOS_TX];     /* itinerário completo (só o coordenador preenche) */

    /* Campos só de execução (não são gravados em disco): */
    int  ativo;                  /* 1 enquanto uma requisição está tratando a transação; a recuperação a ignora */
    int  jaExistia;              /* preenchido por reservaPreparar: 1 se o prepare era repetido */
    char falha[64];              /* id da carona que causou a recusa no prepare */
} Reserva;

/* GERENCIAMENTO GERAL */
int  armazenamentoIniciar(void);
void armazenamentoEncerrar(void);
int  armazenamentoQuantidade(void);
char *armazenamentoListarJSON(void);

/* USUÁRIOS */
int  usuarioSalvar(const Usuario *u);
int  usuarioBuscar(const char *id, Usuario *saida);
char *usuariosListarJSON(void);

/* CARONAS */
int  caronaSalvar(const Carona *c);
int  caronaBuscar(const char *id, Carona *saida);
/* Lista caronas; se 'origem' != NULL, só as que PARTEM dessa cidade em algum trecho. */
int  caronaListar(const char *origem, Carona **lista, int *qtd);
char *caronasListarJSON(void);
char *caronasListarJSONDe(const char *motorista);
/* ARMAZENAMENTO_CONFLITO se a carona tem reservas; _PROIBIDO se pertence a outro motorista. */
int  caronaRemover(const char *id, const char *motorista);
void caronaJSON(Buffer *buffer, const Carona *c, int incluirLivres);

/* Índice da cidade na rota (case-insensitive) ou -1. Não usa trava. */
int  caronaIndiceCidade(const Carona *c, const char *cidade);
/* Quantos assentos estão livres em TODOS os trechos da faixa [de, ate). -1 se inválido. */
int  caronaLivresIntervalo(const char *id, int de, int ate);

/* RESERVAS */

/* Fase 1 do 2PC: escolhe e guarda assentos para os r->locais, gravando a reserva como PREPARADO.
 * Bloqueia até 'esperaMs' se uma reserva PREPARADO mais antiga disputa o mesmo assento.
 * Idempotente: se o id já existe, devolve a reserva guardada com r->jaExistia = 1. */
int  reservaPreparar(Reserva *r, int esperaMs);
int  reservaBuscar(const char *id, Reserva *saida);
/* Muda o estado e grava em disco; se 'itin' != NULL, guarda também o itinerário completo. */
int  reservaDecidir(const char *id, const char *estado, const TrechoRes *itin, int qtdItin);
/* Rollback: remove a reserva, mas recusa (ARMAZENAMENTO_CONFLITO) se já estiver COMITADO ou CANCELANDO. */
int  reservaRemover(const char *id);
/* Remove a reserva sem checar o estado (usado ao concluir um cancelamento). */
int  reservaApagar(const char *id);
int  reservaListarPorEstado(const char *estado, Reserva **lista, int *qtd);
/* Marca se uma requisição está tratando a transação (ver Reserva.ativo). */
void reservaAtivo(const char *id, int ativo);
/* Reservas PREPARADO, inativas e com pelo menos 'idadeMinS' segundos (candidatas à recuperação). */
int  reservaPendentes(int idadeMinS, Reserva **lista, int *qtd);
/* Se 'passageiro' != NULL, filtra por ele; se 'soCoordenador', só as que este servidor coordena. */
char *reservasListarJSON(const char *passageiro, int soCoordenador);

#endif
