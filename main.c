#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>

#include "util/configuracoes.h"
#include "util/persistencia.h"
#include "http/httpServidor.h"
#include "cli.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Uso: %s <regiao> [porta]\n\n", argv[0]);
        listarRegioes();
        return 1;
    }

    if (configuracoesCarregar(argv[1], argc >= 3 ? atoi(argv[2]) : 0) != 0) {
        printf("Regiao '%s' desconhecida.\n\n", argv[1]);
        listarRegioes();
        return 1;
    }

    srand((unsigned) time(NULL) ^ (unsigned) getpid());

    if (armazenamentoIniciar() != ARMAZENAMENTO_OK) {
        printf("Erro ao iniciar o armazenamento em '%s/'.\n", PASTA_JSON);
        return 1;
    }
    curl_global_init(CURL_GLOBAL_ALL);

    printf("Iniciando servidor '%s' na porta %d (%d vizinhos)...\n",
           servidor.id, servidor.porta, servidor.qtdVizinhos);

    struct MHD_Daemon *satanas = servidorHTTPIniciar(servidor.porta);
    if (!satanas) {
        printf("Erro ao iniciar o servidor HTTP (porta %d em uso?).\n", servidor.porta);
        curl_global_cleanup();
        return 1;
    }

    cliExecutar();

    servidorHTTPParar(satanas);
    armazenamentoEncerrar();
    curl_global_cleanup();
    return 0;
}
