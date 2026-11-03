#ifndef CADASTRO_H
#define CADASTRO_H

/*
 * Cadastro de Usuários e Caronas
 *
 * Controla a criação, listagem e exclusão de usuários e caronas,
 * validando regras de negócio e permissões.
 */

#include "util.h"

RespostaServidor cadastroUsuario(const char *corpo);
RespostaServidor cadastroCarona(const char *corpo);
RespostaServidor cadastroRota(const char *motorista, const char *corpo);
RespostaServidor cadastroCaronaComo(const char *motorista, const char *corpo);
RespostaServidor cadastroListarUsuarios(void);
RespostaServidor cadastroListarCaronas(void);
RespostaServidor cadastroListarCaronasDe(const char *motorista);
RespostaServidor cadastroRemoverCarona(const char *motorista, const char *idCarona);

#endif
