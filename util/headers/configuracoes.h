#ifndef CONFIGURACOES_H
#define CONFIGURACOES_H

#include <stddef.h>

#define MAX_VIZINHOS 10
#define MAX_ID 50
#define TIMEOUT_REQUISICAO 3000
#define TIMEOUT_CONEXAO 1500
#define PREFIXO_CONSORCIO "/api/consorcio"
#define PREFIXO_CLIENTE   "/api/cliente"

#define MAX_TRECHOS_TX        8
#define ESPERA_MAX_MS         800/
#define TIMEOUT_PREPARADO_S   5
#define INTERVALO_RECUPERACAO_S 2
#define SUSPEITA_MS           5000

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

extern Servidor servidor;

int configuracoesCarregar(const char *id, int portaOverride);
void configuracoesNormalizar(const char *entrada, char *saida, size_t tamanho);
int configuracoesRegiaoValida(const char *id);
int configuracoesSaoVizinhas(const char *a, const char *b);
const Vizinho *buscarVizinho(const char *id);
void listarRegioes(void);

#endif
