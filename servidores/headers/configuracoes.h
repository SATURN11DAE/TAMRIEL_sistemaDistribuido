#ifndef CONFIGURACOES_H
#define CONFIGURACOES_H

/*
 * Configurações globais e mapa do mundo.
 *
 * Define as regiões de Tamriel (The Elder Scrolls) que cada servidor
 * pode representar, suas portas e a topologia da rede (quais são vizinhas).
 * Contém os limites e timeouts usados na rede e no consenso.
 */

#include <stddef.h>

/* Constantes de identificação e rede */
#define MAX_VIZINHOS 10
#define MAX_ID 50
#define PREFIXO_CONSORCIO "/api/consorcio"
#define PREFIXO_CLIENTE   "/api/cliente"

/* Timeouts e intervalos */
#define TIMEOUT_REQUISICAO 3000      /* ms: tempo total de uma requisição a um vizinho */
#define TIMEOUT_CONEXAO 1500         /* ms: tempo para estabelecer a conexão */
#define ESPERA_MAX_MS 800            /* ms: espera máxima por um assento disputado no prepare */
#define SUSPEITA_MS 5000             /* ms: quanto tempo um vizinho sem resposta fica "suspeito" */
#define TIMEOUT_PREPARADO_S 5        /* s: idade mínima de uma reserva PREPARADO para a recuperação agir */
#define INTERVALO_RECUPERACAO_S 2    /* s: intervalo entre ciclos da thread de recuperação */

/* Limites de transação */
#define MAX_TRECHOS_TX 8

/* Tamanhos de texto compartilhados entre módulos (buffers, structs e assinaturas) */
#define TAM_ID_ENTIDADE 64           /* id de usuário, carona e passageiro */
#define TAM_CIDADE 96
#define TAM_HORARIO 32

typedef struct {
    char id[MAX_ID];
    char host[64];
    int porta;
} Vizinho;

typedef struct {
    char id[MAX_ID];
    int porta;
    int qtdVizinhos;
    Vizinho vizinhos[MAX_VIZINHOS];
} Servidor;

/* Configuração global do servidor atual */
extern Servidor servidor;

/* Padroniza o nome: tudo minúsculo, substituindo espaços e hifens por '_' */
void configuracoesNormalizar(const char *entrada, char *saida, size_t tamanho);

/* Prepara a variável global "servidor" para a região escolhida e descobre os hosts.
 * Retorna 0 em caso de sucesso. */
int configuracoesCarregar(const char *id, int portaOverride);

/* Verifica se a região existe no mapa. Retorna 1 se existir. */
int configuracoesRegiaoValida(const char *id);

/* Verifica se a região "b" faz fronteira com a região "a". Retorna 1 se forem vizinhas. */
int configuracoesSaoVizinhas(const char *a, const char *b);

/* Busca um vizinho do servidor atual pelo ID. Retorna NULL se não for encontrado. */
const Vizinho *buscarVizinho(const char *id);

/* Exibe no terminal a lista de todas as regiões disponíveis e suas portas padrão. */
void listarRegioes(void);

#endif
