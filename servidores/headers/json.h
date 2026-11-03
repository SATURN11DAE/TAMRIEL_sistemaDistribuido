#ifndef JSON_H
#define JSON_H

/*
 * Leitor (parser) de JSON. Transforma um texto JSON em uma árvore de valores
 * na memória, que depois pode ser consultada com as funções jsonObter*.
 *
 *   ValorJSON *raiz = jsonAnalisar(texto);
 *     if (raiz) {
 *        const char *nome = jsonObterTexto(raiz, "nome");
 *        int idade = jsonObterInteiro(raiz, "idade", 0);
 *        jsonLiberar(raiz);     // lembrar de liberar a memória no final
 *     }
 */

/* Os tipos de valor que existem em JSON */
typedef enum {
    JSON_NULO,
    JSON_BOOLEANO,
    JSON_NUMERO,
    JSON_TEXTO,
    JSON_ARRAY,
    JSON_OBJETO     /* objeto: { "chave": valor, ... } */
} TipoJSON;

/* Um nó da árvore JSON. Só o campo correspondente ao "tipo" é válido. */
typedef struct ValorJSON {
    TipoJSON tipo;
    double numero;             /* valor, se tipo == JSON_NUMERO */
    int booleano;              /* 1 ou 0, se tipo == JSON_BOOLEANO */
    char *texto;               /* texto, se tipo == JSON_TEXTO */
    int quantidade;            /* quantidade de itens se for JSON_ARRAY ou JSON_OBJETO */
    struct ValorJSON **itens;  /* os itens em JSON_ARRAY e os valores em JSON_OBJETO */
    char **chaves;             /* nomes dos campos (só em objetos, mesma posição de "itens") */
} ValorJSON;

/* Lê o texto e monta a árvore. Retorna NULL se o JSON for inválido. */
ValorJSON *jsonAnalisar(const char *texto);

/* Libera a árvore inteira (inclusive os filhos). Aceita NULL. */
void jsonLiberar(ValorJSON *valor);

/* Consultas em objetos: devolvem NULL / valor padrão se o campo não existir */
const ValorJSON *jsonObterCampo(const ValorJSON *objeto, const char *chave);
const char      *jsonObterTexto(const ValorJSON *objeto, const char *chave);
int              jsonObterInteiro(const ValorJSON *objeto, const char *chave, int valorPadrao);
double           jsonObterNumero(const ValorJSON *objeto, const char *chave, double valorPadrao);

/* Consultas em arrays */
int              jsonTamanhoArray(const ValorJSON *array);
const ValorJSON *jsonObterElemento(const ValorJSON *array, int posicao);

#endif
