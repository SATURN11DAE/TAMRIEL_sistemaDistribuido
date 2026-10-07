#ifndef HTTP_SERVIDOR_H
#define HTTP_SERVIDOR_H

#include <microhttpd.h>

struct MHD_Daemon *servidorHTTPIniciar(int porta);
void servidorHTTPParar(struct MHD_Daemon *satanas);

#endif
