/*
 * Autenticação de usuários
 *
 *   Senhas NUNCA são guardadas. Guarda um "hash" (resultado de uma conta
 *   matemática de mão única) junto com um "salt" (valor aleatório único por
 *   usuário), para que duas pessoas com a mesma senha tenham hashes diferentes.
 *   O cliente envia "Authorization: Basic <base64(id:senha)>" e confere.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#include "headers/auth.h"

#define ITERACOES_PBKDF2  100000  /* quantas vezes repetir o cálculo (deixa ataques de força bruta lentos) */
#define TAMANHO_SALT_BYTES 16     /* 128 bits de aleatoriedade */
#define TAMANHO_HASH_BYTES 32     /* 256 bits (SHA-256) */

/* FUNÇÕES AUXILIARES DE CONVERSÃO */

/*
 * Transforma bytes em texto hexadecimal minúsculo.
 *   - bytes / quantidade: dados de entrada
 *   - saidaHex: precisa ter espaço para 2 * quantidade + 1 caracteres
 * O '\0' final é escrito pela função.
 */
static void bytesParaHex(const unsigned char *bytes, size_t quantidade, char *saidaHex) {
    static const char DIGITOS_HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < quantidade; i++) {
        saidaHex[2 * i]     = DIGITOS_HEX[bytes[i] >> 4];   /* 4 bits mais significativos */
        saidaHex[2 * i + 1] = DIGITOS_HEX[bytes[i] & 15];   /* 4 bits menos significativos */
    }
    saidaHex[2 * quantidade] = '\0';
}

/*
 * Converte UM caractere hexadecimal (0-9, a-f, A-F) no seu valor 0..15.
 * Retorna -1 se o caractere não for hexadecimal.
 */
static int valorDoDigitoHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Operação inversa de bytesParaHex.
 *   - hex: texto com exatamente 2 * quantidade dígitos
 *   - saida: recebe 'quantidade' bytes
 * Retorna 1 se o texto era válido, 0 se não (NULL, tamanho errado ou dígito inválido).
 */
static int hexParaBytes(const char *hex, unsigned char *saida, size_t quantidade) {
    if (!hex || strlen(hex) != 2 * quantidade) return 0;
    for (size_t i = 0; i < quantidade; i++) {
        int alto  = valorDoDigitoHex(hex[2 * i]);
        int baixo = valorDoDigitoHex(hex[2 * i + 1]);
        if (alto < 0 || baixo < 0) return 0;
        saida[i] = (unsigned char) ((alto << 4) | baixo);
    }
    return 1;
}

/* FUNÇÕES AUXILIARES DE CRIPTOGRAFIA E BASE64 */

/*
 * Calcula o hash da senha com PBKDF2-HMAC-SHA256 (algoritmo próprio para senhas,
 * propositalmente lento: ITERACOES_PBKDF2 repetições).
 *   - salt: TAMANHO_SALT_BYTES bytes
 *   - hashSaida: recebe TAMANHO_HASH_BYTES bytes
 * Retorna 0 se deu certo, -1 se falhou.
 */
static int derivarHashDaSenha(const char *senha, const unsigned char *salt, unsigned char *hashSaida) {
    int ok = PKCS5_PBKDF2_HMAC(senha, (int) strlen(senha),
                               salt, TAMANHO_SALT_BYTES,
                               ITERACOES_PBKDF2,
                               EVP_sha256(),
                               TAMANHO_HASH_BYTES, hashSaida);
    return ok == 1 ? 0 : -1;
}

/*
 * Decodifica texto Base64 para bytes (para ao encontrar '=' ou o fim do texto).
 * Base64 usa 64 símbolos e cada um carrega 6 bits: juntamos de 6 em 6 até ter
 * 8 bits (1 byte).
 *   - saida / maxBytes: destino e seu limite de tamanho
 *   - quantidadeBytes: recebe quantos bytes foram escritos
 * Retorna 1 se válido, 0 se houver símbolo fora do alfabeto ou estouro de maxBytes.
 */
static int decodificarBase64(const char *texto, unsigned char *saida, size_t maxBytes, size_t *quantidadeBytes) {
    unsigned acumulador = 0;
    int bitsNoAcumulador = 0;
    size_t bytesEscritos = 0;

    for (; *texto && *texto != '='; texto++) {
        int valor6bits;
        char c = *texto;
        if      (c >= 'A' && c <= 'Z') valor6bits = c - 'A';
        else if (c >= 'a' && c <= 'z') valor6bits = c - 'a' + 26;
        else if (c >= '0' && c <= '9') valor6bits = c - '0' + 52;
        else if (c == '+')             valor6bits = 62;
        else if (c == '/')             valor6bits = 63;
        else return 0;                 /* caractere que não existe em Base64 */

        acumulador = ((acumulador << 6) | (unsigned) valor6bits) & 0xFFFFFFu;
        bitsNoAcumulador += 6;

        if (bitsNoAcumulador >= 8) {
            bitsNoAcumulador -= 8;
            if (bytesEscritos >= maxBytes) return 0;
            saida[bytesEscritos++] = (unsigned char) ((acumulador >> bitsNoAcumulador) & 0xFF);
        }
    }
    *quantidadeBytes = bytesEscritos;
    return 1;
}

/* API PÚBLICA */

/*
 * Sorteia um salt novo (RAND_bytes) e calcula o hash da senha com ele.
 *   - salt / hash: recebem os valores em hexadecimal (já com '\0'), prontos
 *     para guardar no Usuario
 * Retorna 0 se deu certo, -1 se falhou (atenção: o inverso de authConferir).
 */
int authGerarHash(const char *senha, char salt[AUTH_SALT_HEX], char hash[AUTH_HASH_HEX]) {
    unsigned char saltBytes[TAMANHO_SALT_BYTES];
    unsigned char hashBytes[TAMANHO_HASH_BYTES];

    if (RAND_bytes(saltBytes, sizeof saltBytes) != 1) return -1;
    if (derivarHashDaSenha(senha, saltBytes, hashBytes) != 0) return -1;

    bytesParaHex(saltBytes, sizeof saltBytes, salt);
    bytesParaHex(hashBytes, sizeof hashBytes, hash);
    return 0;
}

/*
 * Refaz a conta com o salt guardado e compara com o hash guardado.
 * A comparação usa CRYPTO_memcmp (tempo constante), evitando ataques de tempo.
 * Retorna 1 se a senha for correta, 0 se errada ou se salt/hash forem inválidos.
 */
int authConferir(const char *senha, const char *saltHex, const char *hashHex) {
    unsigned char saltBytes[TAMANHO_SALT_BYTES];
    unsigned char hashEsperado[TAMANHO_HASH_BYTES];
    unsigned char hashCalculado[TAMANHO_HASH_BYTES];

    if (!hexParaBytes(saltHex, saltBytes, sizeof saltBytes)) return 0;
    if (!hexParaBytes(hashHex, hashEsperado, sizeof hashEsperado)) return 0;
    if (derivarHashDaSenha(senha, saltBytes, hashCalculado) != 0) return 0;

    /* CRYPTO_memcmp previne ataques de tempo medindo diferenças iniciais no hash */
    return CRYPTO_memcmp(hashEsperado, hashCalculado, TAMANHO_HASH_BYTES) == 0;
}

/*
 * Extrai id e senha do cabeçalho "Basic dXN1YXJpbzpzZW5oYQ==" (base64 de "id:senha").
 *   - id / senha: buffers de saída, com seus tamanhos
 * O texto decodificado é apagado da memória (OPENSSL_cleanse) antes de retornar.
 * Retorna 1 se o formato for válido e tudo coube nos buffers, 0 caso contrário.
 */
int authBasic(const char *cabecalho, char *id, size_t tamanhoId, char *senha, size_t tamanhoSenha) {
    if (!cabecalho || strncasecmp(cabecalho, "Basic ", 6) != 0) return 0;
    cabecalho += 6;
    while (*cabecalho == ' ') cabecalho++;

    unsigned char textoDecodificado[200];
    size_t tamanhoDecodificado = 0;
    if (!decodificarBase64(cabecalho, textoDecodificado, sizeof textoDecodificado - 1, &tamanhoDecodificado)) return 0;
    textoDecodificado[tamanhoDecodificado] = '\0';

    char *doisPontos = strchr((char *) textoDecodificado, ':');
    int valido = 0;
    if (doisPontos) {
        *doisPontos = '\0';
        const char *idLido    = (char *) textoDecodificado;
        const char *senhaLida = doisPontos + 1;

        if (*idLido && strlen(idLido) < tamanhoId && strlen(senhaLida) < tamanhoSenha) {
            snprintf(id, tamanhoId, "%s", idLido);
            snprintf(senha, tamanhoSenha, "%s", senhaLida);
            valido = 1;
        }
    }
    
    OPENSSL_cleanse(textoDecodificado, sizeof textoDecodificado);
    return valido;
}

/*
 * Autentica uma requisição inteira: lê o cabeçalho, busca o usuário e confere a senha.
 *   - cabecalho: valor de "Authorization" (pode ser NULL)
 *   - usuarioSaida: recebe uma cópia do usuário se autenticado (pode ser NULL)
 * Se o usuário não existe, faz uma conferência falsa para gastar o mesmo tempo.
 * Falhas esperam 0,3 s, o que atrasa tentativas de força bruta.
 * Retorna 1 se autenticado, 0 caso contrário.
 */
int authRequisicao(const char *cabecalho, Usuario *usuarioSaida) {
    char id[64];
    char senha[AUTH_SENHA_MAX + 8];
    Usuario usuarioEncontrado;
    int autenticado = 0;
    
    memset(&usuarioEncontrado, 0, sizeof usuarioEncontrado);

    if (authBasic(cabecalho, id, sizeof id, senha, sizeof senha)) {
        if (usuarioBuscar(id, &usuarioEncontrado) == ARMAZENAMENTO_OK && usuarioEncontrado.hash[0]) {
            autenticado = authConferir(senha, usuarioEncontrado.salt, usuarioEncontrado.hash);
        } else {
            /* Conferência falsa para manter o mesmo tempo de resposta perante ataques */
            authConferir(senha, "00000000000000000000000000000000",
                         "0000000000000000000000000000000000000000000000000000000000000000");
        }
        if (autenticado && usuarioSaida) *usuarioSaida = usuarioEncontrado;
    }

    OPENSSL_cleanse(senha, sizeof senha);
    if (!autenticado) usleep(300000); /* Atraso de 0.3s para mitigar força bruta */
    return autenticado;
}
