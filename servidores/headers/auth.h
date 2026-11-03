#ifndef AUTH_H
#define AUTH_H

/*
 * Autenticação de usuários.
 *
 * Gerencia a criação de hashes seguros para senhas (usando PBKDF2 e SHA-256)
 * e a validação do cabeçalho HTTP "Authorization: Basic".
 * As senhas nunca são armazenadas em texto limpo.
 */

#include <stddef.h>
#include "persistencia.h"

#define AUTH_SENHA_MIN 4
#define AUTH_SENHA_MAX 72

#define AUTH_SALT_HEX 33
#define AUTH_HASH_HEX 65

/* Sorteia um salt novo e calcula o hash da senha. Ambos saem em formato hexadecimal. */
int authGerarHash(const char *senha, char salt[AUTH_SALT_HEX], char hash[AUTH_HASH_HEX]);

/* Refaz a conta com o salt guardado e compara com o hash guardado. Retorna 1 se a senha for correta. */
int authConferir(const char *senha, const char *saltHex, const char *hashHex);

/* Extrai id e senha do cabeçalho "Basic dXN1YXJpbzpzZW5oYQ==". Retorna 1 se o formato for válido. */
int authBasic(const char *cabecalho, char *id, size_t tamanhoId, char *senha, size_t tamanhoSenha);

/* Autentica uma requisição inteira e copia os dados para usuarioSaida. Retorna 1 se autenticado. */
int authRequisicao(const char *cabecalho, Usuario *usuarioSaida);

#endif
