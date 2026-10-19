#ifndef TRANSACAO_H
#define TRANSACAO_H

#include "util/headers/util.h"

//Coordenador
Resp txReservar(const char *corpo);
Resp txReservarComo(const char *passageiro, const char *corpo);
Resp txCancelar(const char *idTransacao, const char *passageiro);

//Participante
Resp txPrepare(const char *corpo, const char *remetente);
Resp txCommit(const char *idTransacao);
Resp txRollback(const char *idTransacao);
Resp txStatus(const char *idTransacao);
Resp txCancel(const char *idTransacao);

void txRecuperacaoIniciar(void);
void txRecuperacaoParar(void);

#endif
