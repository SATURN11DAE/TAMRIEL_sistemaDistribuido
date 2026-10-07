#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/configuracoes.h"
#include "util/lamport.h"
#include "util/util.h"
#include "util/persistencia.h"
#include "http/httpCliente.h"

#include "cli.h"

static char ultimaTransacao[64] = "";

static int lerLinha(const char *prompt, char *buffer, size_t n) {
    printf("%s", prompt);

    fflush(stdout);
    if (!fgets(buffer, (int) n, stdin)) return 0;

    buffer[strcspn(buffer, "\r\n")] = 0;
    return 1;
}

static const Vizinho *escolherVizinho(void) {
    char linha[32];
    printf("\nVizinhos de %s:\n", servidor.id);

    for (int i = 0; i < servidor.qtdVizinhos; i++) {
        printf("  %d) %-18s %s:%d\n", i + 1, servidor.vizinhos[i].id,
               servidor.vizinhos[i].host, servidor.vizinhos[i].porta);
    }

    if (!lerLinha("Escolha o vizinho: ", linha, sizeof linha)) return NULL;

    int op = atoi(linha);
    if (op < 1 || op > servidor.qtdVizinhos) { 
        printf("Opcao invalida.\n"); return NULL;
    }
    
    return &servidor.vizinhos[op - 1];
}

static void pedirTransacao(char *destino, size_t n) {
    char prompt[96], linha[64];
    snprintf(prompt, sizeof prompt, "id_transacao [%s]: ",
            ultimaTransacao[0] ? ultimaTransacao : "nenhuma");

    lerLinha(prompt, linha, sizeof linha);
    snprintf(destino, n, "%s", linha[0] ? linha : ultimaTransacao);
}

static void mostrar(const char *nome, int rc, Resposta *resposta) {
    if (rc != 0) {
        printf("\n>>> %s FALHOU: %s%s\n", nome,
                resposta->erro, resposta->timeout ? " [TIMEOUT / servidor fora do ar?]" : "");
    } else {
        printf("\n>>> %s => HTTP %ld\n%s\n", nome, resposta->codigoHTTP, resposta->corpo);
    }

    respostaLiberar(resposta);
}

static void menu(void) {
    printf("\n=========== %s (porta %d) | relogio = %d ===========\n",
            servidor.id, servidor.porta, lamportMostrar());

    printf(" 1) GET    health de um vizinho\n");
    printf(" 2) GET    health de TODOS os vizinhos\n");
    printf(" 3) GET    trechos (busca de itinerario)\n");
    printf(" 4) POST   reservas/prepare\n");
    printf(" 5) PUT    reservas/{id}/commit\n");
    printf(" 6) DELETE reservas/{id} (rollback)\n");
    printf(" 7) GET    reservas/{id}/status\n");
    printf(" 8) GET    reservas (lista o que o vizinho tem guardado)\n");
    printf(" 9) Ver as reservas guardadas AQUI (json/%s.json)\n", servidor.id);
    printf(" 0) Sair\n");
}

void cliExecutar(void) {
    char linha[64];
    Resposta resposta;

    for (;;) {
        menu();
        if (!lerLinha("> ", linha, sizeof linha)) {
            printf("[CLI] stdin fechado, servidor continua rodando (Ctrl+C para parar).\n");
            for (;;) pause();
        }

        int op = atoi(linha);
        if (op == 0) {
            if (linha[0] == '0') return;
            printf("Opcao invalida.\n");
            continue;
        }

        if (op == 2) {
            for (int i = 0; i < servidor.qtdVizinhos; i++) {
                int rc = clienteSaude(servidor.vizinhos[i].id, &resposta);
                mostrar(servidor.vizinhos[i].id, rc, &resposta);
            }
            continue;
        }

        if (op == 9) {
            char *json = armazenamentoListarJSON();
            if (json) {
                printf("\n>>> reservas locais\n%s\n", json);
                free(json);
            }
            continue;
        }

        if (op < 1 || op > 8) {printf("Opcao invalida.\n"); continue;}

        const Vizinho *v = escolherVizinho();
        if (!v) continue;
        int rc;

        switch (op) {
            case 1: {
                rc = clienteSaude(v->id, &resposta);
                mostrar("health", rc, &resposta);
                break;

            } case 3: {
                char origem[128], destino[128], visitados[200], todos[300];
                lerLinha("origem: ", origem, sizeof origem);
                lerLinha("destino: ", destino, sizeof destino);
                lerLinha("visitados extras (ex.: skyrim,cyrodiil) [vazio]: ",
                        visitados, sizeof visitados);

                if (visitados[0]) {
                    snprintf(todos, sizeof todos, "%s,%s", servidor.id, visitados);
                } else {
                    snprintf(todos, sizeof todos, "%s", servidor.id);
                }
                
                rc = clienteTrechos(v->id, origem, destino, todos, &resposta);
                mostrar("trechos", rc, &resposta);
                break;

            } case 4: {
                char t[64], carona[64];
                lerLinha("id_transacao [vazio = gerar UUID]: ", t, sizeof t);

                if (!t[0]) gerarUUID(t);
                lerLinha("id_carona [carona-teste]: ", carona, sizeof carona);

                if (!carona[0]) strcpy(carona, "carona-teste");
                snprintf(ultimaTransacao, sizeof ultimaTransacao, "%s", t);

                rc = clientePreparar(v->id, t, carona, &resposta);
                mostrar("prepare", rc, &resposta);
                break;

            } case 8: {
                rc = clienteReservas(v->id, &resposta);
                mostrar("reservas", rc, &resposta);
                break;

            } case 5: case 6: case 7: {
                char t[64];
                pedirTransacao(t, sizeof t);
                if (!t[0]) { printf("Sem id_transacao.\n"); break;}

                if (op == 5) {
                    rc = clienteCommitar(v->id, t, &resposta);
                    mostrar("commit", rc, &resposta);
                } else if (op == 6) {
                    rc = clienteRollback(v->id, t, &resposta);
                    mostrar("rollback", rc, &resposta);
                } else { 
                    rc = clienteStatus(v->id, t, &resposta);
                    mostrar("status", rc, &resposta);
                }
                break;
            }
        }
    }
}
