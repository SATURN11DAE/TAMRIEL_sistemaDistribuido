#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

void jsonEscape(const char *entrada, char *saida, size_t tamanho);
int jsonExtrairString(const char *json, const char *chave, char *saida, size_t tamanho);
int jsonExtrairInt(const char *json, const char *chave);
void gerarUUID(char saida[37]);

#endif
