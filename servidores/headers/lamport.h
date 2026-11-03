#ifndef LAMPORT_H
#define LAMPORT_H

/*
 * Relógio Lógico de Lamport.
 *
 * Em sistemas distribuídos os relógios dos computadores não são confiáveis,
 * então usamos um contador que só sobe e que serve para ORDENAR eventos:
 *   - a cada evento local, o contador sobe 1;
 *   - ao receber uma mensagem, o contador vira max(meu, o_da_mensagem) + 1.
 * Todas as funções são seguras para uso com várias threads.
 */

/* Evento local (exemplo, enviar uma mensagem): soma 1 e devolve o novo valor. */
int lamportIncrementar(void);

/* Recebi uma mensagem com o relógio "relogioRecebido": sincroniza e devolve o novo valor. */
int lamportSincronizar(int relogioRecebido);

/* Apenas lê o valor atual, sem alterar. */
int lamportObterValor(void);

#endif
