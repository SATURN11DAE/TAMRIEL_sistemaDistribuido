/*
 * Parser de JSON feito à mão porque eu me odeio.
 *
 * Existe um leitor que aponta para a posição atual no texto.
 * Cada função lê um pedaço e avança o ponteiro:
 *     lerValor() olha o primeiro caractere e decide:
 *         '{' -> lerObjeto()      '[' -> lerArray()      '"' -> lerTexto()
 *         true / false / null     ou um número
 * Objetos e arrays chamam lerValor() recursivamente para cada item.
 */

#include <stdlib.h>
#include <string.h>

#include "headers/json.h"
#include "headers/util.h"

/* Limite de aninhamento (objeto dentro de objeto...) para evitar estouro de pilha */
#define PROFUNDIDADE_MAXIMA 32

typedef struct {
    const char *posicao;     /* onde estamos no texto */
    int profundidade;        /* quantos níveis de aninhamento já descemos */
} Leitor;

static ValorJSON *lerValor(Leitor *leitor); /* declarada antes porque objeto/array precisam dela */

/* FUNÇÕES AUXILIARES PARA A LEITURA */

/*
 * Avança o leitor sobre espaços, tabs e quebras de linha.
 */
static void pularEspacos(Leitor *leitor) {
    while (*leitor->posicao == ' ' || *leitor->posicao == '\t' ||
    *leitor->posicao == '\n' || *leitor->posicao == '\r')
           leitor->posicao++;
}

/*
 * Cria um nó novo, zerado, do tipo pedido.
 * Retorna o nó alocado (liberado por jsonLiberar) ou NULL se faltou memória.
 */
static ValorJSON *criarValor(TipoJSON tipo) {
    ValorJSON *valor = calloc(1, sizeof *valor);
    if (valor) valor->tipo = tipo;
    return valor;
}

/*
 * Converte 4 dígitos hexadecimais (a parte XXXX de \uXXXX) em número.
 * Retorna 1 e grava em *resultado se válido, 0 se algum dígito for inválido.
 */
static int lerHexadecimal4(const char *texto, unsigned *resultado) {
    unsigned valor = 0;
    for (int i = 0; i < 4; i++) {
        char c = texto[i];
        valor <<= 4;
        if (c >= '0' && c <= '9') valor |= (unsigned) (c - '0');
        else if (c >= 'a' && c <= 'f') valor |= (unsigned) (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') valor |= (unsigned) (c - 'A' + 10);
        else return 0;
    }
    *resultado = valor;
    return 1;
}

/*
 * Escreve um caractere Unicode no buffer, codificado em UTF-8
 * (1 a 4 bytes, dependendo do tamanho do número).
 *   - codePoint: valor Unicode (U+0000 a U+10FFFF)
 */
static void adicionarUTF8(Buffer *buffer, unsigned codePoint) {
    if (codePoint < 0x80) {
        bufferAdicionarCaractere(buffer, (char) codePoint);
    } else if (codePoint < 0x800) {
        bufferAdicionarCaractere(buffer, (char) (0xC0 | (codePoint >> 6)));
        bufferAdicionarCaractere(buffer, (char) (0x80 | (codePoint & 0x3F)));
    } else if (codePoint < 0x10000) {
        bufferAdicionarCaractere(buffer, (char) (0xE0 | (codePoint >> 12)));
        bufferAdicionarCaractere(buffer, (char) (0x80 | ((codePoint >> 6) & 0x3F)));
        bufferAdicionarCaractere(buffer, (char) (0x80 | (codePoint & 0x3F)));
    } else {
        bufferAdicionarCaractere(buffer, (char) (0xF0 | (codePoint >> 18)));
        bufferAdicionarCaractere(buffer, (char) (0x80 | ((codePoint >> 12) & 0x3F)));
        bufferAdicionarCaractere(buffer, (char) (0x80 | ((codePoint >> 6) & 0x3F)));
        bufferAdicionarCaractere(buffer, (char) (0x80 | (codePoint & 0x3F)));
    }
}

/* LEITURA DE CADA TIPO DE VALOR */

/*
 * Lê uma string JSON a partir da aspa de abertura e avança até depois da aspa de fechamento.
 * Trata escapes como \n, \" e \uXXXX (inclusive pares substitutos).
 * Retorna uma cópia alocada (quem chama dá free), ou NULL se a string for
 * inválida (sem aspa final, escape desconhecido ou \u malformado).
 */
static char *lerTexto(Leitor *leitor) {
    Buffer buffer; bufferIniciar(&buffer);
    leitor->posicao++;   /* pula a aspa de abertura */

    while (*leitor->posicao && *leitor->posicao != '"') {
        unsigned char caractere = (unsigned char) *leitor->posicao++;

        if (caractere == '\\') {
            char escape = *leitor->posicao++;
            switch (escape) {
                case 'n': bufferAdicionarCaractere(&buffer, '\n'); break;
                case 't': bufferAdicionarCaractere(&buffer, '\t'); break;
                case 'r': bufferAdicionarCaractere(&buffer, '\r'); break;
                case 'b': bufferAdicionarCaractere(&buffer, '\b'); break;
                case 'f': bufferAdicionarCaractere(&buffer, '\f'); break;
                case '"': case '\\': case '/': bufferAdicionarCaractere(&buffer, escape); break;
                case 'u': {
                    unsigned codePoint;
                    if (!lerHexadecimal4(leitor->posicao, &codePoint)) { bufferLiberar(&buffer); return NULL; }
                    leitor->posicao += 4;

                    /* Caracteres fora do plano básico vêm como "par substituto":
                     * \uD83D\uDE00 (alto + baixo). Aqui juntamos os dois em um só. */
                    if (codePoint >= 0xD800 && codePoint <= 0xDBFF &&
                        leitor->posicao[0] == '\\' && leitor->posicao[1] == 'u') {
                        unsigned parteBaixa;
                        if (lerHexadecimal4(leitor->posicao + 2, &parteBaixa) &&
                            parteBaixa >= 0xDC00 && parteBaixa <= 0xDFFF) {
                            codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (parteBaixa - 0xDC00);
                            leitor->posicao += 6;
                        }
                    }
                    adicionarUTF8(&buffer, codePoint);
                    break;
                }
                default: bufferLiberar(&buffer); return NULL;   /* escape desconhecido */
            }
        } else {
            bufferAdicionarCaractere(&buffer, (char) caractere);
        }
    }

    /* Se o texto acabou sem fechar as aspas, é inválido */
    if (*leitor->posicao != '"') { bufferLiberar(&buffer); return NULL; }
    leitor->posicao++;   /* pula a aspa de fechamento */
    return bufferExtrairTexto(&buffer);
}

/*
 * Adiciona um item à lista de filhos de um pai (serve para arrays e objetos).
 *   - chave: nome do campo se o pai for objeto (a posse passa para o pai); ignorada em arrays
 * Retorna 1 se deu certo, 0 se faltou memória (nesse caso a posse NÃO passou).
 */
static int adicionarFilho(ValorJSON *pai, char *chave, ValorJSON *filho) {
    ValorJSON **novosItens = realloc(pai->itens, (size_t) (pai->quantidade + 1) * sizeof *novosItens);
    if (!novosItens) return 0;
    pai->itens = novosItens;

    if (pai->tipo == JSON_OBJETO) {
        char **novasChaves = realloc(pai->chaves, (size_t) (pai->quantidade + 1) * sizeof *novasChaves);
        if (!novasChaves) return 0;
        pai->chaves = novasChaves;
        pai->chaves[pai->quantidade] = chave;
    }
    pai->itens[pai->quantidade++] = filho;
    return 1;
}

/*
 * Lê um objeto: { "chave": valor, "chave2": valor2, ... }
 * Retorna o objeto montado ou NULL se a sintaxe for inválida (tudo que já
 * tinha sido lido é liberado).
 */
static ValorJSON *lerObjeto(Leitor *leitor) {
    ValorJSON *objeto = criarValor(JSON_OBJETO);
    if (!objeto) return NULL;
    leitor->posicao++;   /* pula o '{' */
    pularEspacos(leitor);

    if (*leitor->posicao == '}') { leitor->posicao++; return objeto; }   /* objeto vazio {} */

    for (;;) {
        pularEspacos(leitor);
        if (*leitor->posicao != '"') goto erro;   /* toda chave começa com aspas */

        char *chave = lerTexto(leitor);
        if (!chave) goto erro;

        pularEspacos(leitor);
        if (*leitor->posicao != ':') { free(chave); goto erro; }
        leitor->posicao++;

        ValorJSON *valor = lerValor(leitor);
        if (!valor) { free(chave); goto erro; }
        if (!adicionarFilho(objeto, chave, valor)) { free(chave); jsonLiberar(valor); goto erro; }

        pularEspacos(leitor);
        if (*leitor->posicao == ',') { leitor->posicao++; continue; }        /* tem mais campos */
        if (*leitor->posicao == '}') { leitor->posicao++; return objeto; }   /* terminou */
        goto erro;
    }

erro:
    jsonLiberar(objeto);
    return NULL;
}

/*
 * Lê um array: [ valor, valor2, ... ]
 * Retorna o array montado ou NULL se a sintaxe for inválida (tudo que já
 * tinha sido lido é liberado).
 */
static ValorJSON *lerArray(Leitor *leitor) {
    ValorJSON *array = criarValor(JSON_ARRAY);
    if (!array) return NULL;
    leitor->posicao++;   /* pula o '[' */
    pularEspacos(leitor);

    if (*leitor->posicao == ']') { leitor->posicao++; return array; }   /* array vazio [] */

    for (;;) {
        ValorJSON *valor = lerValor(leitor);
        if (!valor) goto erro;
        if (!adicionarFilho(array, NULL, valor)) { jsonLiberar(valor); goto erro; }

        pularEspacos(leitor);
        if (*leitor->posicao == ',') { leitor->posicao++; continue; }
        if (*leitor->posicao == ']') { leitor->posicao++; return array; }
        goto erro;
    }

erro:
    jsonLiberar(array);
    return NULL;
}

/*
 * Lê qualquer valor JSON: olha o 1º caractere para decidir o que é e chama a
 * função certa. Objetos e arrays chamam esta função de volta, por isso existe
 * o limite PROFUNDIDADE_MAXIMA de aninhamento.
 * Retorna o valor ou NULL se for inválido ou fundo demais.
 */
static ValorJSON *lerValor(Leitor *leitor) {
    if (++leitor->profundidade > PROFUNDIDADE_MAXIMA) return NULL;
    pularEspacos(leitor);

    ValorJSON *valor = NULL;
    char primeiro = *leitor->posicao;

    if (primeiro == '{') {
        valor = lerObjeto(leitor);

    } else if (primeiro == '[') {
        valor = lerArray(leitor);

    } else if (primeiro == '"') {
        char *texto = lerTexto(leitor);
        if (texto) {
            valor = criarValor(JSON_TEXTO);
            if (valor) valor->texto = texto; else free(texto);
        }

    } else if (strncmp(leitor->posicao, "true", 4) == 0) {
        valor = criarValor(JSON_BOOLEANO);
        if (valor) { valor->booleano = 1; leitor->posicao += 4; }

    } else if (strncmp(leitor->posicao, "false", 5) == 0) {
        valor = criarValor(JSON_BOOLEANO);
        if (valor) leitor->posicao += 5;

    } else if (strncmp(leitor->posicao, "null", 4) == 0) {
        valor = criarValor(JSON_NULO);
        if (valor) leitor->posicao += 4;

    } else if (primeiro == '-' || (primeiro >= '0' && primeiro <= '9')) {
        char *fimDoNumero;
        double numero = strtod(leitor->posicao, &fimDoNumero);

        if (fimDoNumero != leitor->posicao) {
            valor = criarValor(JSON_NUMERO);
            if (valor) { valor->numero = numero; leitor->posicao = fimDoNumero; }
        }
    }

    leitor->profundidade--;
    return valor;
}

/* API PÚBLICA */

/*
 * Lê o texto e monta a árvore de valores.
 *   - texto: JSON completo (NULL é aceito e devolve NULL)
 * Não pode sobrar nada depois do valor, exceto espaços.
 * Retorna a raiz (quem chama libera com jsonLiberar) ou NULL se o JSON for inválido.
 */
ValorJSON *jsonAnalisar(const char *texto) {
    if (!texto) return NULL;

    Leitor leitor = { texto, 0 };
    ValorJSON *raiz = lerValor(&leitor);
    if (!raiz) return NULL;

    /* Depois do valor não pode sobrar lixo no texto */
    pularEspacos(&leitor);
    if (*leitor.posicao != 0) { jsonLiberar(raiz); return NULL; }
    return raiz;
}

/*
 * Libera a árvore inteira, inclusive os filhos, as chaves e os textos.
 * Aceita NULL.
 */
void jsonLiberar(ValorJSON *valor) {
    if (!valor) return;
    for (int i = 0; i < valor->quantidade; i++) {
        jsonLiberar(valor->itens[i]);
        if (valor->chaves) free(valor->chaves[i]);
    }
    free(valor->itens);
    free(valor->chaves);
    free(valor->texto);
    free(valor);
}

/*
 * Busca um campo de um objeto pelo nome.
 * Retorna o valor (pertence à árvore, não libere) ou NULL se 'objeto' não for
 * um objeto ou o campo não existir.
 */
const ValorJSON *jsonObterCampo(const ValorJSON *objeto, const char *chave) {
    if (!objeto || objeto->tipo != JSON_OBJETO) return NULL;
    for (int i = 0; i < objeto->quantidade; i++) {
        if (strcmp(objeto->chaves[i], chave) == 0) return objeto->itens[i];
    }
    return NULL;
}

/*
 * Lê um campo do tipo texto.
 * Retorna o texto (pertence à árvore, não libere) ou NULL se o campo não
 * existir ou não for texto.
 */
const char *jsonObterTexto(const ValorJSON *objeto, const char *chave) {
    const ValorJSON *campo = jsonObterCampo(objeto, chave);
    return (campo && campo->tipo == JSON_TEXTO) ? campo->texto : NULL;
}

/*
 * Lê um campo numérico.
 * Retorna o valor, ou 'valorPadrao' se o campo não existir ou não for número.
 */
double jsonObterNumero(const ValorJSON *objeto, const char *chave, double valorPadrao) {
    const ValorJSON *campo = jsonObterCampo(objeto, chave);
    return (campo && campo->tipo == JSON_NUMERO) ? campo->numero : valorPadrao;
}

/*
 * Lê um campo numérico como inteiro (a parte decimal é descartada).
 * Retorna o valor, ou 'valorPadrao' se o campo não existir ou não for número.
 */
int jsonObterInteiro(const ValorJSON *objeto, const char *chave, int valorPadrao) {
    const ValorJSON *campo = jsonObterCampo(objeto, chave);
    return (campo && campo->tipo == JSON_NUMERO) ? (int) campo->numero : valorPadrao;
}

/*
 * Retorna a quantidade de itens do array, ou 0 se não for um array.
 */
int jsonTamanhoArray(const ValorJSON *array) {
    return (array && array->tipo == JSON_ARRAY) ? array->quantidade : 0;
}

/*
 * Retorna o item na posição pedida (começa em 0; pertence à árvore, não libere).
 * Retorna NULL se não for um array ou se a posição estiver fora dos limites.
 */
const ValorJSON *jsonObterElemento(const ValorJSON *array, int posicao) {
    return (array && array->tipo == JSON_ARRAY && posicao >= 0 && posicao < array->quantidade)
           ? array->itens[posicao] : NULL;
}
