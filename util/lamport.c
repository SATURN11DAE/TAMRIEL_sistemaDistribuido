#include <pthread.h>

#include "headers/lamport.h"

static int lamport = 0;
static pthread_mutex_t trava = PTHREAD_MUTEX_INITIALIZER;

int lamportTique(void) {
    pthread_mutex_lock(&trava);
    int relogio = ++lamport;
    pthread_mutex_unlock(&trava);

    return relogio;
}

int lamportAtualizar(int atual) {
    pthread_mutex_lock(&trava);
    lamport = (atual > lamport ? atual : lamport) + 1;
    int relogio = lamport;
    pthread_mutex_unlock(&trava);

    return relogio;
}

int lamportMostrar(void) {
    pthread_mutex_lock(&trava);
    int relogio = lamport;
    pthread_mutex_unlock(&trava);

    return relogio;
}
