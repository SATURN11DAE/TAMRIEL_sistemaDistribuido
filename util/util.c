#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "headers/util.h"

void jsonEscape(const char *entrada, char *saida, size_t tamanho) {
    size_t j = 0;
    for (size_t i = 0; entrada[i] && j + 7 < tamanho; i++) {
        unsigned char xar = (unsigned char) entrada[i];

        if (xar == '"' || xar == '\\') {
            saida[j++] = '\\';
            saida[j++] = (char) xar;
        } else if (xar == '\n') {
            saida[j++] = '\\';
            saida[j++] = 'n';
        } else if (xar < 0x20) {
            j += (size_t) snprintf(saida + j, tamanho - j, "\\u%04x", xar);
        } else {
            saida[j++] = (char) xar;
        }
    }
    saida[j] = 0;
}

int jsonExtrairString(const char *json, const char *chave, char *saida, size_t tamanho) {
    char padrao[96];
    snprintf(padrao, sizeof padrao, "\"%s\"", chave);

    const char *p = strstr(json, padrao);
    if (!p) return 0;

    p = strchr(p + strlen(padrao), ':');
    if (!p) return 0;
    p++;

    while(*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (*p != '"') return 0;
    p++;

    size_t i = 0;
    while (*p && *p != '"' && i < tamanho - 1) saida[i++] = *p++;
    saida[i] = 0;

    return 1;
}

int jsonExtrairInt(const char *json, const char *chave) {
    char padrao[96];
    snprintf(padrao, sizeof padrao, "\"%s\"", chave);

    const char *p = strstr(json, padrao);
    if (!p) return 0;

    p = strchr(p + strlen(padrao), ':');
    if (!p) return 0;
    p++;

    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    return atoi(p);
}

void gerarUUID(char saida[37]) {
    unsigned char id[16];

    for (int i = 0; i < 16; i++) {
        id[i] = (unsigned char) (rand() & 0xFF);
    }
    id[6] = (id[6] & 0x0F) | 0x40;
    id[8] = (id[8] & 0x3F) | 0x80;

    snprintf(saida, 37,
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7],
            id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15]);
}
