#ifndef BUSCA_H
#define BUSCA_H

/*
 * Busca de Itinerários e Trechos
 *
 * Realiza buscas locais e distribuídas (consultando os servidores vizinhos)
 * para encontrar caminhos entre a origem e o destino solicitados.
 */

#include "util.h"

/* Busca trechos diretos disponíveis na rede (usado pela comunicação entre servidores) */
RespostaServidor buscaTrechos(const char *origem, const char *destino, const char *visitadosCsv);

/* Busca itinerários completos de ponta a ponta (usado pelos clientes/passageiros) */
RespostaServidor buscaItinerarios(const char *origem, const char *destino);

#endif
