#ifndef AUTH_H
#define AUTH_H

#include <stddef.h>
#include "persistencia.h"

#define AUTH_SENHA_MIN 4
#define AUTH_SENHA_MAX 72

#define AUTH_SALT_HEX 33
#define AUTH_HASH_HEX 65

int authGerarHash(const char *senha, char salt[AUTH_SALT_HEX], char hash[AUTH_HASH_HEX]);
int authConferir(const char *senha, const char *saltHex, const char *hashHex);
int authBasic(const char *cabecalho, char *id, size_t nId, char *senha, size_t nSenha);
int authRequisicao(const char *cabecalho, Usuario *u);

#endif
