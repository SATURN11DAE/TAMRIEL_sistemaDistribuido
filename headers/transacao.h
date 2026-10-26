#ifndef TRANSACAO_H
#define TRANSACAO_H

/*
 * Controle de Transações Distribuídas (Two-Phase Commit)
 *
 * Gerencia as reservas de assentos de ponta a ponta na rede.
 * Implementa o protocolo 2PC (Prepare, Commit/Rollback) para garantir 
 * que todos os servidores do itinerário reservem os assentos juntos 
 * ou abortem juntos, mantendo a consistência dos dados.
 */

#include "util.h"

/* FUNÇÕES DO COORDENADOR (Inicia a transação a pedido do cliente) */
RespostaServidor txReservar(const char *corpo);
RespostaServidor txReservarComo(const char *passageiro, const char *corpo);
RespostaServidor txCancelar(const char *idTransacao, const char *passageiro);

/* FUNÇÕES DO PARTICIPANTE (Recebe comandos do coordenador ou de outro nó) */
RespostaServidor txPrepare(const char *corpo, const char *remetente);
RespostaServidor txCommit(const char *idTransacao);
RespostaServidor txRollback(const char *idTransacao);
RespostaServidor txStatus(const char *idTransacao);
RespostaServidor txCancel(const char *idTransacao);

/* THREAD DE RECUPERAÇÃO DE FALHAS */
void txRecuperacaoIniciar(void);
void txRecuperacaoParar(void);

#endif
