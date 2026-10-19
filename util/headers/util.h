#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

void jsonEscape(const char *entrada, char *saida, size_t tamanho);
int jsonExtrairString(const char *json, const char *chave, char *saida, size_t tamanho);
int jsonExtrairInt(const char *json, const char *chave);
void gerarUUID(char saida[37]);

typedef struct {
    char *p;
    size_t n;
    size_t cap;
} Buf;

void bufIni(Buf *b);
void bufLiberar(Buf *b);
int  bufAdd(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int  bufChar(Buf *b, char c);
int  bufStr(Buf *b, const char *s);
char *bufTomar(Buf *b);

typedef struct {
    unsigned int http;
    char *json;
} Resp;

Resp respJSON(unsigned int http, char *json);
Resp respErro(unsigned int http, const char *msg);

int  idValido(const char *s);
long agoraSegundos(void);
long agoraMonotonicoMs(void);

#endif
