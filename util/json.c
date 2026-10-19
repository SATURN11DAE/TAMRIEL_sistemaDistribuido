#include <stdlib.h>
#include <string.h>

#include "headers/json.h"
#include "headers/util.h"

#define PROF_MAX 32

typedef struct { const char *s; int prof; } Leitor;

static JVal *valor(Leitor *l);

static void espacos(Leitor *l) {
    while (*l->s == ' ' || *l->s == '\t' || *l->s == '\n' || *l->s == '\r') l->s++;
}

static JVal *novo(JTipo tipo) {
    JVal *v = calloc(1, sizeof *v);
    if (v) v->tipo = tipo;
    return v;
}

static int hex4(const char *s, unsigned *saida) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned) (c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned) (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned) (c - 'A' + 10);
        else return 0;
    }
    *saida = v;
    return 1;
}

static void utf8(Buf *b, unsigned cp) {
    if (cp < 0x80) bufChar(b, (char) cp);
    else if (cp < 0x800) { bufChar(b, (char) (0xC0 | (cp >> 6))); bufChar(b, (char) (0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
        bufChar(b, (char) (0xE0 | (cp >> 12)));
        bufChar(b, (char) (0x80 | ((cp >> 6) & 0x3F)));
        bufChar(b, (char) (0x80 | (cp & 0x3F)));
    } else {
        bufChar(b, (char) (0xF0 | (cp >> 18)));
        bufChar(b, (char) (0x80 | ((cp >> 12) & 0x3F)));
        bufChar(b, (char) (0x80 | ((cp >> 6) & 0x3F)));
        bufChar(b, (char) (0x80 | (cp & 0x3F)));
    }
}

static char *texto(Leitor *l) {
    Buf b; bufIni(&b);
    l->s++;

    while (*l->s && *l->s != '"') {
        unsigned char c = (unsigned char) *l->s++;
        if (c == '\\') {
            char e = *l->s++;
            switch (e) {
                case 'n': bufChar(&b, '\n'); break;
                case 't': bufChar(&b, '\t'); break;
                case 'r': bufChar(&b, '\r'); break;
                case 'b': bufChar(&b, '\b'); break;
                case 'f': bufChar(&b, '\f'); break;
                case '"': case '\\': case '/': bufChar(&b, e); break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(l->s, &cp)) { bufLiberar(&b); return NULL; }
                    l->s += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && l->s[0] == '\\' && l->s[1] == 'u') {
                        unsigned lo;
                        if (hex4(l->s + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            l->s += 6;
                        }
                    }
                    utf8(&b, cp);
                    break;
                }
                default: bufLiberar(&b); return NULL;
            }
        } else {
            bufChar(&b, (char) c);
        }
    }

    if (*l->s != '"') { bufLiberar(&b); return NULL; }
    l->s++;
    return bufTomar(&b);
}

static int empilhar(JVal *pai, char *chave, JVal *filho) {
    JVal **it = realloc(pai->itens, (size_t) (pai->n + 1) * sizeof *it);
    if (!it) return 0;
    pai->itens = it;

    if (pai->tipo == J_OBJ) {
        char **ch = realloc(pai->chaves, (size_t) (pai->n + 1) * sizeof *ch);
        if (!ch) return 0;
        pai->chaves = ch;
        pai->chaves[pai->n] = chave;
    }
    pai->itens[pai->n++] = filho;
    return 1;
}

static JVal *objeto(Leitor *l) {
    JVal *o = novo(J_OBJ);
    if (!o) return NULL;
    l->s++;
    espacos(l);

    if (*l->s == '}') { l->s++; return o; }

    for (;;) {
        espacos(l);
        if (*l->s != '"') goto erro;

        char *chave = texto(l);
        if (!chave) goto erro;

        espacos(l);
        if (*l->s != ':') { free(chave); goto erro; }
        l->s++;

        JVal *v = valor(l);
        if (!v) { free(chave); goto erro; }
        if (!empilhar(o, chave, v)) { free(chave); jsonLiberar(v); goto erro; }

        espacos(l);
        if (*l->s == ',') { l->s++; continue; }
        if (*l->s == '}') { l->s++; return o; }
        goto erro;
    }
erro:
    jsonLiberar(o);
    return NULL;
}

static JVal *vetor(Leitor *l) {
    JVal *a = novo(J_ARR);
    if (!a) return NULL;
    l->s++;
    espacos(l);

    if (*l->s == ']') { l->s++; return a; }

    for (;;) {
        JVal *v = valor(l);
        if (!v) goto erro;
        if (!empilhar(a, NULL, v)) { jsonLiberar(v); goto erro; }

        espacos(l);
        if (*l->s == ',') { l->s++; continue; }
        if (*l->s == ']') { l->s++; return a; }
        goto erro;
    }
erro:
    jsonLiberar(a);
    return NULL;
}

static JVal *valor(Leitor *l) {
    if (++l->prof > PROF_MAX) return NULL;
    espacos(l);

    JVal *v = NULL;
    char c = *l->s;

    if (c == '{') v = objeto(l);
    else if (c == '[') v = vetor(l);
    else if (c == '"') {
        char *s = texto(l);
        if (s) { v = novo(J_STR); if (v) v->str = s; else free(s); }
    } else if (strncmp(l->s, "true", 4) == 0) {
        v = novo(J_BOOL); if (v) { v->booleano = 1; l->s += 4; }
    } else if (strncmp(l->s, "false", 5) == 0) {
        v = novo(J_BOOL); if (v) l->s += 5;
    } else if (strncmp(l->s, "null", 4) == 0) {
        v = novo(J_NULO); if (v) l->s += 4;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char *fim;
        double d = strtod(l->s, &fim);
        if (fim != l->s) { v = novo(J_NUM); if (v) { v->num = d; l->s = fim; } }
    }

    l->prof--;
    return v;
}

JVal *jsonParse(const char *txt) {
    if (!txt) return NULL;

    Leitor l = { txt, 0 };
    JVal *v = valor(&l);
    if (!v) return NULL;

    espacos(&l);
    if (*l.s != 0) { jsonLiberar(v); return NULL; }
    return v;
}

void jsonLiberar(JVal *v) {
    if (!v) return;
    for (int i = 0; i < v->n; i++) {
        jsonLiberar(v->itens[i]);
        if (v->chaves) free(v->chaves[i]);
    }
    free(v->itens);
    free(v->chaves);
    free(v->str);
    free(v);
}

const JVal *jsonGet(const JVal *obj, const char *chave) {
    if (!obj || obj->tipo != J_OBJ) return NULL;
    for (int i = 0; i < obj->n; i++) {
        if (strcmp(obj->chaves[i], chave) == 0) return obj->itens[i];
    }
    return NULL;
}

const char *jsonGetStr(const JVal *obj, const char *chave) {
    const JVal *v = jsonGet(obj, chave);
    return (v && v->tipo == J_STR) ? v->str : NULL;
}

double jsonGetNum(const JVal *obj, const char *chave, double pad) {
    const JVal *v = jsonGet(obj, chave);
    return (v && v->tipo == J_NUM) ? v->num : pad;
}

int jsonGetInt(const JVal *obj, const char *chave, int pad) {
    const JVal *v = jsonGet(obj, chave);
    return (v && v->tipo == J_NUM) ? (int) v->num : pad;
}

int jsonTam(const JVal *arr) { return (arr && arr->tipo == J_ARR) ? arr->n : 0; }

const JVal *jsonIdx(const JVal *arr, int i) {
    return (arr && arr->tipo == J_ARR && i >= 0 && i < arr->n) ? arr->itens[i] : NULL;
}
