#ifndef HTTP_SERVIDOR_H
#define HTTP_SERVIDOR_H

/*
 * Servidor HTTP (Usando libmicrohttpd)
 *
 * Responsável por subir o servidor na porta especificada,
 * escutar requisições de clientes e de outros nós do consórcio,
 * e roteá-las para os devidos controladores.
 */

#include <microhttpd.h>

/* Inicia o servidor HTTP em background. Retorna o ponteiro para o daemon criado. */
struct MHD_Daemon *servidorHTTPIniciar(int porta);

/* Encerra a execução do servidor HTTP e libera seus recursos. */
void servidorHTTPParar(struct MHD_Daemon *daemon);

#endif
