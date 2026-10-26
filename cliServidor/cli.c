/*
 * cli.c - Utilitário de linha de comando para testar os endpoints e vizinhos do servidor.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

#include "headers/configuracoes.h"
#include "headers/lamport.h"
#include "headers/util.h"
#include "headers/persistencia.h"
#include "headers/httpCliente.h"
#include "headers/json.h"
#include "headers/cadastro.h"
#include "headers/busca.h"
#include "headers/transacao.h"

#include "headers/cli.h"

/*
 * Última transação usada no menu, sugerida como padrão nas opções 5, 6 e 7.
 */
static char ultimaTransacao[64] = "";

/*
 * Mostra o prompt e lê uma linha do teclado, sem o '\n'.
 * Se a linha for maior que o buffer, descarta o resto.
 * Retorna 1 se leu, 0 se a entrada fechou (EOF).
 */
static int lerLinha(const char *prompt, char *buffer, size_t n) {
    printf("%s", prompt);
    fflush(stdout);
    
    buffer[0] = '\0';
    if (!fgets(buffer, (int) n, stdin)) return 0;

    if (strchr(buffer, '\n') == NULL) {
        int c;
        while ((c = fgetc(stdin)) != '\n' && c != EOF);
    }

    buffer[strcspn(buffer, "\r\n")] = '\0';
    return 1;
}

/*
 * Igual a lerLinha, mas sem mostrar o que é digitado (desliga o eco do terminal).
 * Retorna 1 se leu, 0 se a entrada fechou.
 */
static int lerSenha(const char *prompt, char *buffer, size_t n) {
    struct termios antigo, novo;
    int tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &antigo) == 0;

    printf("%s", prompt);
    fflush(stdout);
    
    buffer[0] = '\0';

    if (tty) {
        novo = antigo;
        novo.c_lflag &= ~(tcflag_t) ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &novo); 
    }

    char *ok = fgets(buffer, (int) n, stdin);

    if (tty) { 
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &antigo); 
        printf("\n"); 
    }
    
    if (ok && strchr(buffer, '\n') == NULL) {
        int c;
        while ((c = fgetc(stdin)) != '\n' && c != EOF);
    }
    
    if (!ok) return 0;
    buffer[strcspn(buffer, "\r\n")] = '\0';
    return 1;
}

/*
 * Lista os vizinhos deste servidor e pergunta qual usar.
 * Retorna um ponteiro para servidor.vizinhos[i] (não libere) ou NULL se a opção for inválida.
 */
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
        printf("Opcao invalida.\n"); 
        return NULL;
    }
    
    return &servidor.vizinhos[op - 1];
}

/*
 * Pergunta o id da transação; se o usuário só apertar Enter, usa a última usada.
 *   - destino / n: buffer que recebe o id
 */
static void pedirTransacao(char *destino, size_t n) {
    char prompt[96], linha[64];
    snprintf(prompt, sizeof prompt, "id_transacao [%s]: ", ultimaTransacao[0] ? ultimaTransacao : "nenhuma");

    lerLinha(prompt, linha, sizeof linha);
    snprintf(destino, n, "%s", linha[0] ? linha : ultimaTransacao);
}

/*
 * Mostra o resultado de uma chamada a um vizinho (erro de rede ou HTTP + corpo)
 * e libera a resposta.
 */
static void mostrar(const char *nome, int rc, Resposta *resposta) {
    if (rc != 0) {
        printf("\n>>> %s FALHOU: %s%s\n", nome,
                resposta->erro, resposta->falhaRede ? " [SEM RESPOSTA / servidor fora do ar?]" : "");
    } else {
        printf("\n>>> %s => HTTP %ld\n%s\n", nome, resposta->codigoHTTP, resposta->corpo);
    }

    respostaLiberar(resposta);
}

/*
 * Mostra uma RespostaServidor local (HTTP + JSON) e libera o JSON.
 */
static void mostrarResp(const char *nome, RespostaServidor r) {
    printf("\n>>> %s => HTTP %u\n%s\n", nome, r.http, r.json ? r.json : "");
    free(r.json);
}

/*
 * Opções do menu que rodam AQUI, sem passar por HTTP:
 *   10 cadastrar usuário        11 publicar carona (rota)
 *   12 listar usuários e caronas  13 buscar itinerários (distribuído)
 *   14 reservar itinerário (este servidor coordena o 2PC)
 * Na opção 11, cada trecho da rota é publicado como uma carona separada de 2 cidades
 * (cadastroCarona), diferente do cliente, que publica uma única carona com vários trechos.
 */
static void opcaoLocal(int op) {
    char stringA[128]="", stringB[128]="", stringC[128]="", stringD[128]="";
    Buffer buffer; 
    bufferIniciar(&buffer);

    if (op == 10) {
        lerLinha("id: ", stringA, sizeof stringA); 
        lerLinha("nome: ", stringB, sizeof stringB);
        lerLinha("tipo (motorista/passageiro): ", stringC, sizeof stringC);
        lerSenha("senha: ", stringD, sizeof stringD);
        
        bufferAdicionar(&buffer, "{\"id\": "); 
        bufferAdicionarStringJSON(&buffer, stringA); 
        
        bufferAdicionar(&buffer, ", \"nome\": "); 
        bufferAdicionarStringJSON(&buffer, stringB);
        
        bufferAdicionar(&buffer, ", \"tipo\": "); 
        bufferAdicionarStringJSON(&buffer, stringC); 
        
        bufferAdicionar(&buffer, ", \"senha\": "); 
        bufferAdicionarStringJSON(&buffer, stringD); 
        bufferAdicionar(&buffer, "}");
        
        mostrarResp("usuario", cadastroUsuario(buffer.p));
    } else if (op == 11) {
        char motorista[128], bufferCidades[16];
        int numCidades = 0;
        
        lerLinha("motorista (id): ", motorista, sizeof motorista); 

        if (!lerLinha("Quantas cidades compoem a rota (incluindo origem e destino)? (min: 2): ", bufferCidades, sizeof bufferCidades)) {
            bufferLiberar(&buffer);
            return;
        }
        
        numCidades = atoi(bufferCidades);
        if (numCidades < 2) {
            printf("A rota deve ter pelo menos 2 cidades (uma origem e um destino).\n");
            bufferLiberar(&buffer);
            return;
        }
        if (numCidades > 10) {
            printf("Limite maximo de 10 cidades por rota atingido.\n");
            bufferLiberar(&buffer);
            return;
        }

        char cidades[10][96];
        for (int i = 0; i < numCidades; i++) {
            char prompt[64];
            if (i == 0) snprintf(prompt, sizeof prompt, "Cidade 1 (Origem): ");
            else if (i == numCidades - 1) snprintf(prompt, sizeof prompt, "Cidade %d (Destino Final): ", i + 1);
            else snprintf(prompt, sizeof prompt, "Cidade %d (Parada): ", i + 1);

            if (!lerLinha(prompt, cidades[i], sizeof cidades[i])) {
                bufferLiberar(&buffer);
                return;
            }
        }

        char horarios[10][32];
        double precos[10];
        int assentos[10]; // Novo array aqui também

        for (int i = 0; i < numCidades - 1; i++) {
            char precoStr[32], assentosStr[16];
            printf("\n--- Detalhes do trecho %d: %s -> %s ---\n", i + 1, cidades[i], cidades[i+1]);
            if (!lerLinha("Horario de saida (ex.: 08:30): ", horarios[i], sizeof horarios[i])) {
                bufferLiberar(&buffer);
                return;
            }
            if (!lerLinha("Preco apenas deste trecho: ", precoStr, sizeof precoStr)) {
                bufferLiberar(&buffer);
                return;
            }
            precos[i] = atof(precoStr);
            
            if (!lerLinha("Assentos disponiveis NESTE trecho: ", assentosStr, sizeof assentosStr)) {
                bufferLiberar(&buffer);
                return;
            }
            assentos[i] = atoi(assentosStr);
        }

        printf("\nPublicando %d trecho(s) na rede...\n", numCidades - 1);
        for (int i = 0; i < numCidades - 1; i++) {
            bufferLiberar(&buffer);
            bufferIniciar(&buffer);
            
            bufferAdicionar(&buffer, "{\"motorista\": "); 
            bufferAdicionarStringJSON(&buffer, motorista); 
            
            bufferAdicionar(&buffer, ", \"origem\": ");
            bufferAdicionarStringJSON(&buffer, cidades[i]);
            
            bufferAdicionar(&buffer, ", \"destino\": "); 
            bufferAdicionarStringJSON(&buffer, cidades[i+1]); 
            
            bufferAdicionar(&buffer, ", \"horario\": ");
            bufferAdicionarStringJSON(&buffer, horarios[i]);
            
            bufferAdicionar(&buffer, ", \"preco\": %.2f, \"assentos\": %d}", precos[i], assentos[i]); // assentos[i] dinâmico
            
            printf("\n[Trecho %s -> %s]:\n", cidades[i], cidades[i+1]);
            mostrarResp("carona", cadastroCarona(buffer.p));
        }
    } else if (op == 12) {
        mostrarResp("usuarios", cadastroListarUsuarios());
        mostrarResp("caronas", cadastroListarCaronas());
    } else if (op == 13) {
        lerLinha("origem: ", stringA, sizeof stringA); 
        lerLinha("destino: ", stringB, sizeof stringB);
        mostrarResp("itinerarios", buscaItinerarios(stringA, stringB));
    } else {
        lerLinha("passageiro (id): ", stringA, sizeof stringA);
        printf("Trechos na ordem, um por linha, no formato servidor:id_carona (linha vazia termina)\n");
        
        bufferAdicionar(&buffer, "{\"passageiro\": "); 
        bufferAdicionarStringJSON(&buffer, stringA); 
        bufferAdicionar(&buffer, ", \"trechos\": [");
        
        for (int i = 0; i < MAX_TRECHOS_TX; i++) {
            lerLinha("  trecho: ", stringB, sizeof stringB);
            char *dp = strchr(stringB, ':');
            if (!stringB[0] || !dp) break;
            *dp = '\0';
            
            bufferAdicionar(&buffer, "%s{\"servidor\": ", i ? ", " : ""); 
            bufferAdicionarStringJSON(&buffer, stringB);
            bufferAdicionar(&buffer, ", \"id_carona\": "); 
            bufferAdicionarStringJSON(&buffer, dp + 1); 
            bufferAdicionar(&buffer, "}");
        }
        bufferAdicionar(&buffer, "]}");
        mostrarResp("reservar", txReservar(buffer.p));
    }
    bufferLiberar(&buffer);
}

/*
 * Imprime o menu principal e o relógio de Lamport atual.
 */
static void menu(void) {
    printf("\n=========== %s (porta %d) | relogio = %d ===========\n",
            servidor.id, servidor.porta, lamportObterValor());

    printf(" 1) GET    health de um vizinho\n");
    printf(" 2) GET    health de TODOS os vizinhos\n");
    printf(" 3) GET    trechos (busca de itinerario)\n");
    printf(" 4) POST   reservas/prepare\n");
    printf(" 5) PUT    reservas/{id}/commit\n");
    printf(" 6) DELETE reservas/{id} (rollback)\n");
    printf(" 7) GET    reservas/{id}/status\n");
    printf(" 8) GET    reservas (lista o que o vizinho tem guardado)\n");
    printf(" 9) Ver as reservas guardadas AQUI (json/%s.json)\n", servidor.id);
    printf("10) Cadastrar usuario (motorista/passageiro) AQUI\n");
    printf("11) Publicar carona AQUI (multiplos trechos/rotas)\n");
    printf("12) Listar usuarios e caronas AQUI\n");
    printf("13) Buscar itinerarios (distribuido)\n");
    printf("14) Reservar itinerario (2PC; este servidor coordena)\n");
    printf(" 0) Sair\n");
}

/*
 * Laço principal do menu do servidor (bloqueia até o usuário escolher 0).
 * Se o stdin fechar, o servidor continua rodando e a thread espera em pause().
 */
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
            char *jsonLocal = armazenamentoListarJSON();
            if (jsonLocal) {
                printf("\n>>> reservas locais\n%s\n", jsonLocal);
                free(jsonLocal);
            }
            continue;
        }

        if (op >= 10 && op <= 14) { 
            opcaoLocal(op); 
            continue; 
        }

        if (op < 1 || op > 8) {
            printf("Opcao invalida.\n"); 
            continue;
        }

        const Vizinho *vizinho = escolherVizinho();
        if (!vizinho) continue;
        int rc;

        switch (op) {
            case 1: {
                rc = clienteSaude(vizinho->id, &resposta);
                mostrar("health", rc, &resposta);
                break;
            } case 3: {
                char origem[128], destino[128], visitados[200], todos[300];
                lerLinha("origem: ", origem, sizeof origem);
                lerLinha("destino: ", destino, sizeof destino);
                lerLinha("visitados extras (ex.: skyrim,cyrodiil) [vazio]: ", visitados, sizeof visitados);

                if (visitados[0]) {
                    snprintf(todos, sizeof todos, "%s,%s", servidor.id, visitados);
                } else {
                    snprintf(todos, sizeof todos, "%s", servidor.id);
                }
                
                rc = clienteTrechos(vizinho->id, origem, destino, todos, &resposta);
                mostrar("trechos", rc, &resposta);
                break;
            } case 4: {
                char transacao_id[64], carona[64];
                lerLinha("id_transacao [vazio = gerar UUID]: ", transacao_id, sizeof transacao_id);

                if (!transacao_id[0]) gerarUUID(transacao_id);
                lerLinha("id_carona [carona-teste]: ", carona, sizeof carona);

                if (!carona[0]) strcpy(carona, "carona-teste");
                snprintf(ultimaTransacao, sizeof ultimaTransacao, "%s", transacao_id);

                rc = clientePreparar(vizinho->id, transacao_id, carona, &resposta);
                mostrar("prepare", rc, &resposta);
                break;
            } case 8: {
                rc = clienteReservas(vizinho->id, &resposta);
                mostrar("reservas", rc, &resposta);
                break;
            } case 5: case 6: case 7: {
                char transacao_id[64];
                pedirTransacao(transacao_id, sizeof transacao_id);
                if (!transacao_id[0]) { 
                    printf("Sem id_transacao.\n"); 
                    break;
                }

                if (op == 5) {
                    rc = clienteCommitar(vizinho->id, transacao_id, &resposta);
                    mostrar("commit", rc, &resposta);
                } else if (op == 6) {
                    rc = clienteRollback(vizinho->id, transacao_id, &resposta);
                    mostrar("rollback", rc, &resposta);
                } else { 
                    rc = clienteStatus(vizinho->id, transacao_id, &resposta);
                    mostrar("status", rc, &resposta);
                }
                break;
            }
        }
    }
}
