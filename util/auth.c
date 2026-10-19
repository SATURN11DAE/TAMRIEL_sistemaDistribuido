#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#include "headers/auth.h"

#define ITERACOES  100000
#define SALT_BYTES 16
#define HASH_BYTES 32

static void paraHex(const unsigned char *b, size_t n, char *saida) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { saida[2 * i] = H[b[i] >> 4]; saida[2 * i + 1] = H[b[i] & 15]; }
    saida[2 * n] = 0;
}

static int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int deHex(const char *hex, unsigned char *saida, size_t n) {
    if (!hex || strlen(hex) != 2 * n) return 0;
    for (size_t i = 0; i < n; i++) {
        int a = nibble(hex[2 * i]), b = nibble(hex[2 * i + 1]);
        if (a < 0 || b < 0) return 0;
        saida[i] = (unsigned char) (a << 4 | b);
    }
    return 1;
}

static int derivar(const char *senha, const unsigned char *salt, unsigned char *saida) {
    return PKCS5_PBKDF2_HMAC(senha, (int) strlen(senha), salt, SALT_BYTES, ITERACOES,
                             EVP_sha256(), HASH_BYTES, saida) == 1 ? 0 : -1;
}

int authGerarHash(const char *senha, char salt[AUTH_SALT_HEX], char hash[AUTH_HASH_HEX]) {
    unsigned char s[SALT_BYTES], h[HASH_BYTES];
    if (RAND_bytes(s, sizeof s) != 1) return -1;
    if (derivar(senha, s, h) != 0) return -1;
    paraHex(s, sizeof s, salt);
    paraHex(h, sizeof h, hash);
    return 0;
}

int authConferir(const char *senha, const char *saltHex, const char *hashHex) {
    unsigned char s[SALT_BYTES], esperado[HASH_BYTES], obtido[HASH_BYTES];
    if (!deHex(saltHex, s, sizeof s) || !deHex(hashHex, esperado, sizeof esperado)) return 0;
    if (derivar(senha, s, obtido) != 0) return 0;
    return CRYPTO_memcmp(esperado, obtido, HASH_BYTES) == 0;   /* tempo constante */
}

static int decodificar64(const char *s, unsigned char *saida, size_t max, size_t *n) {
    unsigned val = 0;
    int bits = 0;
    size_t k = 0;

    for (; *s && *s != '='; s++) {
        int d;
        char c = *s;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+') d = 62;
        else if (c == '/') d = 63;
        else return 0;

        val = ((val << 6) | (unsigned) d) & 0xFFFFFFu;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (k >= max) return 0;
            saida[k++] = (unsigned char) ((val >> bits) & 0xFF);
        }
    }
    *n = k;
    return 1;
}

int authBasic(const char *cab, char *id, size_t nId, char *senha, size_t nSenha) {
    if (!cab || strncasecmp(cab, "Basic ", 6) != 0) return 0;
    cab += 6;
    while (*cab == ' ') cab++;

    unsigned char bruto[200];
    size_t n = 0;
    if (!decodificar64(cab, bruto, sizeof bruto - 1, &n)) return 0;
    bruto[n] = 0;

    char *dp = strchr((char *) bruto, ':');
    int ok = 0;
    if (dp) {
        *dp = 0;
        const char *i = (char *) bruto, *s = dp + 1;
        if (*i && strlen(i) < nId && strlen(s) < nSenha) {
            snprintf(id, nId, "%s", i);
            snprintf(senha, nSenha, "%s", s);
            ok = 1;
        }
    }
    OPENSSL_cleanse(bruto, sizeof bruto);
    return ok;
}

int authRequisicao(const char *cabecalho, Usuario *u) {
    char id[64], senha[AUTH_SENHA_MAX + 8];
    Usuario achado;
    int ok = 0;
    memset(&achado, 0, sizeof achado);

    if (authBasic(cabecalho, id, sizeof id, senha, sizeof senha)) {
        if (usuarioBuscar(id, &achado) == ARMAZENAMENTO_OK && achado.hash[0]) {
            ok = authConferir(senha, achado.salt, achado.hash);
        } else {
            authConferir(senha, "00000000000000000000000000000000",
                         "0000000000000000000000000000000000000000000000000000000000000000");
        }
        if (ok && u) *u = achado;
    }

    OPENSSL_cleanse(senha, sizeof senha);
    if (!ok) usleep(300000);
    return ok;
}
