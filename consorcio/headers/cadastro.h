#ifndef CADASTRO_H
#define CADASTRO_H

#include "util/headers/util.h"

Resp cadastroUsuario(const char *corpo);
Resp cadastroCarona(const char *corpo);
Resp cadastroCaronaComo(const char *motorista, const char *corpo);
Resp cadastroListarUsuarios(void);
Resp cadastroListarCaronas(void);
Resp cadastroListarCaronasDe(const char *motorista);
Resp cadastroRemoverCarona(const char *motorista, const char *idCarona);

#endif
