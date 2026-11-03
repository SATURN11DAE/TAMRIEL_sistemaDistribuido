#ifndef UTIL_H
#define UTIL_H

/*
 * Ferramentas usadas pelo projeto inteiro:
 *   - Buffer: um "texto que cresce sozinho" (como um StringBuilder)
 *   - RespostaServidor: o par (código HTTP + JSON) que as rotas devolvem
 *   - validação de ids e funções de tempo
 */

#include <stddef.h>

/* Gera um UUID aleatório no formato "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx". */
void gerarUUID(char saida[37]);

/* BUFFER, TEXTO QUE VAI CRESCENDO AUTOMATICAMENTE */

typedef struct {
    char  *p;     /* o texto em si (NULL enquanto estiver vazio) */
    size_t n;     /* quantos caracteres já foram escritos */
    size_t cap;   /* quantos bytes estão reservados na memória */
} Buffer;

/* Deixa o buffer vazio e pronto para uso. */
void bufferIniciar(Buffer *buffer);

/* Libera a memória do buffer (e o deixa vazio de novo). */
void bufferLiberar(Buffer *buffer);

/* Adiciona texto formatado ao final, igual ao printf. Retorna 1 se deu certo, 0 se faltou memória. */
int  bufferAdicionar(Buffer *buffer, const char *formato, ...) __attribute__((format(printf, 2, 3)));

/* Adiciona um único caractere ao final. */
int  bufferAdicionarCaractere(Buffer *buffer, char caractere);

/* Adiciona "texto" como uma string JSON: entre aspas e com caracteres especiais escapados.
 * abc"def  ->  "abc\"def" */
int  bufferAdicionarStringJSON(Buffer *buffer, const char *texto);

/* Entrega o texto montado e esvazia o buffer.
 * Quem chamar passa a ser dono do texto e deve dar free() nele. */
char *bufferExtrairTexto(Buffer *buffer);

/* RESPOSTA DO SERVIDOR, O QUE AS ROTAS HTTP DEVOLVEM */

typedef struct {
    unsigned int http;   /* código HTTP (200, 404, ...) */
    char *json;          /* corpo da resposta (alocado, quem envia dá free) */
} RespostaServidor;

/* Monta uma resposta com código e JSON já prontos. */
RespostaServidor criarRespostaJSON(unsigned int http, char *json);

/* Monta uma resposta de erro: {"erro": "mensagem"} */
RespostaServidor criarRespostaErro(unsigned int http, const char *mensagem);

/* VALIDAÇÃO E TEMPO */

/* Retorna 1 se o id só tem letras, números, '-', '_' ou '.', não começa com '.'
 * e tem no máximo 100 caracteres. */
int  identificadorValido(const char *id);

/* Hora atual em segundos (relógio de parede). */
long agoraEmSegundos(void);

/* Tempo em milissegundos que nunca volta para trás, ideal para medir intervalos. */
long agoraMonotonicoMs(void);

#endif
