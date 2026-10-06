#ifndef CONFIGURACOES_H
#define CONFIGURACOES_H

#define MAX_VIZINHOS 10
#define MAX_ID 50
#define TIMEOUT_REQUISICAO 3000
#define TIMEOUT_CONEXAO 1500
#define PREFIXO_CONSORCIO "/api/consorcio"

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
const Vizinho *buscarVizinho(const char *id);
void listarRegioes(void);

#endif
