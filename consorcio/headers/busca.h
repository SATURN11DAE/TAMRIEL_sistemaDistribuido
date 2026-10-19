#ifndef BUSCA_H
#define BUSCA_H

#include "util/headers/util.h"

Resp buscaTrechos(const char *origem, const char *destino, const char *visitadosCsv);
Resp buscaItinerarios(const char *origem, const char *destino);

#endif
