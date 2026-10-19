#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>

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

void bufIni(Buf *b) { b->p = NULL; b->n = 0; b->cap = 0; }

void bufLiberar(Buf *b) { free(b->p); bufIni(b); }

static int bufReservar(Buf *b, size_t extra) {
    if (b->p && b->n + extra + 1 <= b->cap) return 1;

    size_t nova = b->cap ? b->cap : 256;
    while (nova < b->n + extra + 1) nova *= 2;

    char *novo = realloc(b->p, nova);
    if (!novo) return 0;

    if (!b->p) novo[0] = 0;
    b->p = novo;
    b->cap = nova;
    return 1;
}

int bufAdd(Buf *b, const char *fmt, ...) {
    va_list ap, copia;
    va_start(ap, fmt);
    va_copy(copia, ap);

    int len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (len < 0 || !bufReservar(b, (size_t) len)) { va_end(copia); return 0; }

    vsnprintf(b->p + b->n, (size_t) len + 1, fmt, copia);
    va_end(copia);

    b->n += (size_t) len;
    return 1;
}

int bufChar(Buf *b, char c) {
    if (!bufReservar(b, 1)) return 0;
    b->p[b->n++] = c;
    b->p[b->n] = 0;
    return 1;
}

int bufStr(Buf *b, const char *s) {
    if (!s) s = "";
    if (!bufChar(b, '"')) return 0;

    for (const unsigned char *q = (const unsigned char *) s; *q; q++) {
        switch (*q) {
            case '"':  bufAdd(b, "\\\""); break;
            case '\\': bufAdd(b, "\\\\"); break;
            case '\n': bufAdd(b, "\\n");  break;
            case '\r': bufAdd(b, "\\r");  break;
            case '\t': bufAdd(b, "\\t");  break;
            default:
                if (*q < 0x20) bufAdd(b, "\\u%04x", *q);
                else bufChar(b, (char) *q);
        }
    }
    return bufChar(b, '"');
}

char *bufTomar(Buf *b) {
    if (!b->p) {
        char *vazio = malloc(1);
        if (vazio) vazio[0] = 0;
        return vazio;
    }
    char *p = b->p;
    bufIni(b);
    return p;
}

Resp respJSON(unsigned int http, char *json) {
    Resp r = { http, json };
    return r;
}

Resp respErro(unsigned int http, const char *msg) {
    Buf b;
    bufIni(&b);
    bufAdd(&b, "{\"erro\": ");
    bufStr(&b, msg);
    bufAdd(&b, "}\n");
    return respJSON(http, bufTomar(&b));
}

int idValido(const char *s) {
    if (!s || !*s || s[0] == '.' || strlen(s) > 100) return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char) *s;
        if (!(isalnum(c) || c == '-' || c == '_' || c == '.')) return 0;
    }
    return 1;
}

long agoraSegundos(void) { return (long) time(NULL); }

long agoraMonotonicoMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long) ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
