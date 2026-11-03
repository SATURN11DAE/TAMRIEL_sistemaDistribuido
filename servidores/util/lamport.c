/*
 * Implementação do relógio lógico de Lamport
 */

#include <pthread.h>

#include "headers/lamport.h"

/* O valor do relógio deste servidor */
static int valorRelogio = 0;

/* Protege o relógio, já que várias threads (uma por conexão) mexem nele */
static pthread_mutex_t travaRelogio = PTHREAD_MUTEX_INITIALIZER;

/*
 * Evento local: soma 1 ao relógio e devolve o novo valor.
 * Thread-safe (usa travaRelogio).
 */
int lamportIncrementar(void) {
    pthread_mutex_lock(&travaRelogio);
    int novoValor = ++valorRelogio;
    pthread_mutex_unlock(&travaRelogio);

    return novoValor;
}

/*
 * Ao receber uma mensagem: relógio = max(meu, recebido) + 1.
 *   - relogioRecebido: valor do cabeçalho X-Lamport-Clock da mensagem
 * Retorna o novo valor do relógio. Thread-safe.
 */
int lamportSincronizar(int relogioRecebido) {
    pthread_mutex_lock(&travaRelogio);
    /* Pega o maior dos dois relógios e soma 1 */
    valorRelogio = (relogioRecebido > valorRelogio ? relogioRecebido : valorRelogio) + 1;
    int novoValor = valorRelogio;
    pthread_mutex_unlock(&travaRelogio);

    return novoValor;
}

/*
 * Lê o valor atual sem alterá-lo. Thread-safe.
 */
int lamportObterValor(void) {
    pthread_mutex_lock(&travaRelogio);
    int valorAtual = valorRelogio;
    pthread_mutex_unlock(&travaRelogio);

    return valorAtual;
}
