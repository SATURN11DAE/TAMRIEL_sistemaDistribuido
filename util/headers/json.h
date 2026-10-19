#ifndef JSON_H
#define JSON_H

typedef enum { J_NULO, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JTipo;

typedef struct JVal {
    JTipo tipo;
    double num;
    int booleano;
    char *str;
    int n;
    struct JVal **itens;
    char **chaves;
} JVal;

JVal *jsonParse(const char *texto);
void  jsonLiberar(JVal *v);

const JVal *jsonGet(const JVal *obj, const char *chave);
const char *jsonGetStr(const JVal *obj, const char *chave);
int         jsonGetInt(const JVal *obj, const char *chave, int pad);
double      jsonGetNum(const JVal *obj, const char *chave, double pad);

int         jsonTam(const JVal *arr);
const JVal *jsonIdx(const JVal *arr, int i);

#endif
