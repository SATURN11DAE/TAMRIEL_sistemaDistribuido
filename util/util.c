/* 
 * util.c - Funções de apoio usadas pelo projeto todo
 *   - Gerar UUIDs
 *   - Buffer de texto que cresce sozinho (para montar respostas JSON)
 *   - Respostas HTTP padronizadas, validação de id e funções de tempo
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>

#include "headers/util.h"

/*
 * Gera um UUID versão 4 (ex.: 3f2b8c1e-9a4d-4e7b-8c21-5d6e7f8a9b0c).
 *   - saida: buffer de 37 bytes (36 caracteres + '\0'), preenchido pela função
 * Usa rand(), então o programa deve chamar srand() uma vez no início.
 * Serve para identificar entidades, não para segredos.
 */
void gerarUUID(char saida[37]) {
    unsigned char bytes[16];

    for (int i = 0; i < 16; i++) {
        bytes[i] = (unsigned char) (rand() & 0xFF);
    }
    
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    snprintf(saida, 37,
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
            bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

/*  BUFFER DE TEXTO  */

/*
 * Deixa o buffer vazio e pronto para uso (nenhuma memória alocada ainda).
 */
void bufferIniciar(Buffer *buffer) {
    buffer->p = NULL;
    buffer->n = 0;
    buffer->cap = 0;
}

/*
 * Libera a memória do buffer e o deixa vazio de novo. Aceita buffer já vazio.
 */
void bufferLiberar(Buffer *buffer) {
    free(buffer->p);
    bufferIniciar(buffer);
}

/*
 * Garante espaço para mais 'extra' caracteres (+1 do '\0'), dobrando a
 * capacidade a cada vez (começa em 256 bytes).
 * Retorna 1 se deu certo, 0 se faltou memória (o conteúdo antigo é preservado).
 */
static int bufferReservar(Buffer *buffer, size_t extra) {
    if (buffer->p && buffer->n + extra + 1 <= buffer->cap) return 1;

    size_t novaCapacidade = buffer->cap ? buffer->cap : 256;
    while (novaCapacidade < buffer->n + extra + 1) novaCapacidade *= 2;

    char *novaMemoria = realloc(buffer->p, novaCapacidade);
    if (!novaMemoria) return 0;

    if (!buffer->p) novaMemoria[0] = '\0';
    buffer->p = novaMemoria;
    buffer->cap = novaCapacidade;
    return 1;
}

/*
 * Adiciona texto formatado ao final, igual ao printf.
 * Mede o tamanho com um vsnprintf(NULL, 0, ...), reserva e só então escreve.
 * Retorna 1 se deu certo, 0 se o formato for inválido ou faltou memória.
 */
int bufferAdicionar(Buffer *buffer, const char *formato, ...) {
    va_list args, copiaArgs;
    va_start(args, formato);
    va_copy(copiaArgs, args);

    int tamanhoNecessario = vsnprintf(NULL, 0, formato, args);
    va_end(args);

    if (tamanhoNecessario < 0 || !bufferReservar(buffer, (size_t) tamanhoNecessario)) { 
        va_end(copiaArgs); 
        return 0; 
    }

    vsnprintf(buffer->p + buffer->n, (size_t) tamanhoNecessario + 1, formato, copiaArgs);
    va_end(copiaArgs);

    buffer->n += (size_t) tamanhoNecessario;
    return 1;
}

/*
 * Adiciona um único caractere ao final e mantém o '\0' no fim.
 * Retorna 1 se deu certo, 0 se faltou memória.
 */
int bufferAdicionarCaractere(Buffer *buffer, char caractere) {
    if (!bufferReservar(buffer, 1)) return 0;
    buffer->p[buffer->n++] = caractere;
    buffer->p[buffer->n] = '\0';
    return 1;
}

/*
 * Adiciona 'texto' como string JSON: entre aspas, com " \ \n \r \t escapados
 * e demais caracteres de controle (< 0x20) como \uXXXX.
 *   - texto: NULL é tratado como ""
 * Bytes acima de 0x7F (UTF-8) passam direto, sem escape.
 * Retorna 1 se deu certo, 0 se faltou memória.
 */
int bufferAdicionarStringJSON(Buffer *buffer, const char *texto) {
    if (!texto) texto = "";
    if (!bufferAdicionarCaractere(buffer, '"')) return 0;

    for (const unsigned char *c = (const unsigned char *) texto; *c; c++) {
        switch (*c) {
            case '"':  bufferAdicionar(buffer, "\\\""); break;
            case '\\': bufferAdicionar(buffer, "\\\\"); break;
            case '\n': bufferAdicionar(buffer, "\\n");  break;
            case '\r': bufferAdicionar(buffer, "\\r");  break;
            case '\t': bufferAdicionar(buffer, "\\t");  break;
            default:
                if (*c < 0x20) bufferAdicionar(buffer, "\\u%04x", *c);
                else bufferAdicionarCaractere(buffer, (char) *c);
        }
    }
    return bufferAdicionarCaractere(buffer, '"');
}

/*
 * Entrega o texto montado e esvazia o buffer.
 * Quem chamar passa a ser dono do texto e deve dar free() nele.
 * Se nada foi escrito, devolve uma string vazia alocada (ou NULL sem memória).
 */
char *bufferExtrairTexto(Buffer *buffer) {
    if (!buffer->p) {
        char *vazio = malloc(1);
        if (vazio) vazio[0] = '\0';
        return vazio;
    }
    char *textoFinal = buffer->p;
    bufferIniciar(buffer);
    return textoFinal;
}

/*  RESPOSTAS HTTP  */

/*
 * Monta uma resposta com código HTTP e JSON já prontos.
 *   - json: texto alocado; a posse passa para a resposta (quem envia dá free)
 */
RespostaServidor criarRespostaJSON(unsigned int http, char *json) {
    RespostaServidor resposta = { http, json };
    return resposta;
}

/*
 * Monta uma resposta de erro no formato {"erro": "mensagem"}.
 *   - mensagem: texto livre, escapado automaticamente para JSON
 */
RespostaServidor criarRespostaErro(unsigned int http, const char *mensagem) {
    Buffer buffer;
    bufferIniciar(&buffer);
    bufferAdicionar(&buffer, "{\"erro\": ");
    bufferAdicionarStringJSON(&buffer, mensagem);
    bufferAdicionar(&buffer, "}\n");
    return criarRespostaJSON(http, bufferExtrairTexto(&buffer));
}

/* VALIDAÇÃO E TEMPO */

/*
 * Valida um id usado em URLs e nomes de arquivo (evita path traversal).
 * Retorna 1 se o id é não vazio, tem até 100 caracteres, não começa com '.'
 * e só contém letras, números, '-', '_' ou '.'. Caso contrário, 0.
 */
int identificadorValido(const char *id) {
    if (!id || !*id || id[0] == '.' || strlen(id) > 100) return 0;
    for (; *id; id++) {
        unsigned char c = (unsigned char) *id;
        if (!(isalnum(c) || c == '-' || c == '_' || c == '.')) return 0;
    }
    return 1;
}

/*
 * Hora atual em segundos desde a época Unix (relógio de parede).
 * Pode pular se o relógio do sistema for ajustado.
 */
long agoraEmSegundos(void) { 
    return (long) time(NULL); 
}

/*
 * Tempo em milissegundos que nunca volta para trás (CLOCK_MONOTONIC).
 * Só serve para medir intervalos, o valor absoluto não tem significado.
 */
long agoraMonotonicoMs(void) {
    struct timespec instante;
    clock_gettime(CLOCK_MONOTONIC, &instante);
    return (long) instante.tv_sec * 1000L + instante.tv_nsec / 1000000L;
}
