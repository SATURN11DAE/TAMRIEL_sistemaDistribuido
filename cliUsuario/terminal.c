/*
 * terminal.c - Cliente Interativo para Passageiros e Motoristas (Usuário Final)
 *
 * Programa separado do servidor: conversa com um servidor do consórcio só pela
 * API HTTP (apiConsorcio.h). Fluxo: menu inicial (cadastrar / entrar / testar
 * conexão) -> depois do login, menu de motorista ou de passageiro.
 *
 * Uso: terminal [regiao | host:porta | http://host:porta]
 *   O host padrão é "localhost" e pode ser trocado pela variável CLIENTE_HOST.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

#include "headers/configuracoes.h"
#include "headers/json.h"
#include "headers/apiConsorcio.h"

/*
 * Mostra o prompt e lê uma linha do teclado, sem o '\n'.
 * Se a linha for maior que o buffer, descarta o resto.
 * Retorna 1 se leu, 0 se a entrada fechou (EOF).
 */
static int lerLinha(const char *prompt, char *buffer, size_t n) {
    printf("%s", prompt);
    fflush(stdout);
    
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
 * Se a entrada não for um terminal (ex.: redirecionada), lê normalmente.
 * Retorna 1 se leu, 0 se a entrada fechou.
 */
static int lerSenha(const char *prompt, char *buffer, size_t n) {
    struct termios antigo, novo;
    int tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &antigo) == 0;

    printf("%s", prompt);
    fflush(stdout);
    
    if (tty) {
        novo = antigo;
        novo.c_lflag &= ~(tcflag_t) ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &novo);
    }

    char *ok = fgets(buffer, (int) n, stdin);

    if (tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &antigo);
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

/* Devolve "" no lugar de NULL, para poder usar o resultado de jsonObterTexto direto no printf. */
static const char *txt(const char *s) { return s ? s : ""; }

/* Avisa que o servidor não respondeu (falha de rede, não erro HTTP). */
static void semResposta(const ApiResp *resposta) {
    printf("\n!! Sem resposta do servidor %s: %s\n", apiBaseURL(), resposta->erro);
}

/* Mostra o erro HTTP devolvido pelo servidor, com o motivo. */
static void mostrarErro(const ApiResp *resposta) {
    printf("\n!! Erro %ld: %s\n", resposta->http, apiMensagem(resposta));
}

/*
 * Tela de cadastro: pede id, nome, tipo e senha (duas vezes) e chama a API.
 * As senhas digitadas são apagadas da memória depois de usadas.
 */
static void cadastrar(void) {
    char id[64], nome[96], opcaoUsuario[8], senha[80], senhaConfirmada[80];
    
    printf("\n--- Cadastro (neste servidor) ---\n");
    if (!lerLinha("id (letras, numeros, - _ .): ", id, sizeof id)) return;
    if (!lerLinha("nome: ", nome, sizeof nome)) return;
    if (!lerLinha("voce e [1] motorista  [2] passageiro: ", opcaoUsuario, sizeof opcaoUsuario)) return;
    if (!lerSenha("senha (4 a 72 caracteres): ", senha, sizeof senha)) return;
    if (!lerSenha("repita a senha: ", senhaConfirmada, sizeof senhaConfirmada)) return;

    if (strcmp(senha, senhaConfirmada) != 0) { 
        printf("\n!! As senhas nao conferem.\n");
        memset(senha, 0, sizeof senha); 
        return;
    }

    const char *tipoUsuario = atoi(opcaoUsuario) == 1 ? "motorista" : atoi(opcaoUsuario) == 2 ? "passageiro" : NULL;
    if (!tipoUsuario) { 
        printf("Opcao invalida.\n"); 
        return; 
    }

    ApiResp respostaAPI = apiCadastrar(id, nome, tipoUsuario, senha);
    
    memset(senha, 0, sizeof senha); 
    memset(senhaConfirmada, 0, sizeof senhaConfirmada);

    if (!respostaAPI.rede) semResposta(&respostaAPI);
    else if (respostaAPI.http == 201) printf("\nCadastrado como %s. Agora escolha 'Entrar'.\n", tipoUsuario);
    else mostrarErro(&respostaAPI);

    apiLiberar(&respostaAPI);
}

/*
 * Tela de login: pede id e senha e abre a sessão (apiEntrar).
 * Retorna 1 se entrou, 0 se falhou (credenciais erradas, sem rede ou entrada fechada).
 */
static int entrar(void) {
    char id[64], senha[80];
    if (!lerLinha("\nSeu id: ", id, sizeof id)) return 0;
    if (!lerSenha("Senha: ", senha, sizeof senha)) return 0;

    ApiResp respostaAPI;
    int credenciaisOK = apiEntrar(id, senha, &respostaAPI);
    memset(senha, 0, sizeof senha);

    if (credenciaisOK) {
        printf("\nBem-vindo(a), %s!\n", apiUsuarioNome());
    } else if (!respostaAPI.rede) {
        semResposta(&respostaAPI);
    } else if (respostaAPI.http == 401) {
        printf("\n!! Id ou senha incorretos.\n");
    } else {
        mostrarErro(&respostaAPI);
    }

    apiLiberar(&respostaAPI);
    return credenciaisOK;
}

/*
 * Tela do motorista: publica uma carona com várias paradas.
 * Pergunta as cidades da rota (2 a 10) e, para cada trecho, horário, preço e
 * assentos. Tudo vai numa única carona (apiPublicarRota).
 */
static void publicarCarona(void) {
    char bufferCidades[16];
    int numCidades = 0;

    printf("\n--- Publicar carona (Rotas e Multiplos Trechos) ---\n");
    if (!lerLinha("Quantas cidades compoem a rota (incluindo origem e destino)? (min: 2): ", bufferCidades, sizeof bufferCidades)) return;
    
    numCidades = atoi(bufferCidades);
    if (numCidades < 2) {
        printf("A rota deve ter pelo menos 2 cidades (uma origem e um destino).\n");
        return;
    }
    if (numCidades > 10) {
        printf("Limite maximo de 10 cidades por rota atingido.\n");
        return;
    }

    char cidades[10][96];
    for (int i = 0; i < numCidades; i++) {
        char prompt[64];
        if (i == 0) snprintf(prompt, sizeof prompt, "Cidade 1 (Origem): ");
        else if (i == numCidades - 1) snprintf(prompt, sizeof prompt, "Cidade %d (Destino Final): ", i + 1);
        else snprintf(prompt, sizeof prompt, "Cidade %d (Parada): ", i + 1);

        if (!lerLinha(prompt, cidades[i], sizeof cidades[i])) return;
    }

    char horarios[10][32];
    double precos[10];
    int assentos[10];

    for (int i = 0; i < numCidades - 1; i++) {
        char precoStr[32], assentosStr[16];
        printf("\n--- Detalhes do trecho %d: %s -> %s ---\n", i + 1, cidades[i], cidades[i+1]);
        if (!lerLinha("Horario de saida (ex.: 08:30): ", horarios[i], sizeof horarios[i])) return;

        if (!lerLinha("Preco apenas deste trecho: ", precoStr, sizeof precoStr)) return;
        precos[i] = atof(precoStr);

        if (!lerLinha("Assentos disponiveis NESTE trecho: ", assentosStr, sizeof assentosStr)) return;
        assentos[i] = atoi(assentosStr);
    }

    printf("\nPublicando a carona (%d trecho(s))...\n", numCidades - 1);
    ApiResp respostaAPI = apiPublicarRota(numCidades, cidades, horarios, precos, assentos);

    if (!respostaAPI.rede) {
        semResposta(&respostaAPI);
    } else if (respostaAPI.http == 201) {
        printf(" -> Carona publicada! ID: %s\n"
               "    Os passageiros podem reservar qualquer parte da rota.\n",
               txt(jsonObterTexto(respostaAPI.json, "id")));
    } else {
        mostrarErro(&respostaAPI);
    }
    apiLiberar(&respostaAPI);
}

/*
 * Tela do motorista: lista as caronas dele, com os trechos, preços e assentos livres.
 */
static void minhasCaronas(void) {
    ApiResp respostaAPI = apiMinhasCaronas();
    
    if (!respostaAPI.rede) { 
        semResposta(&respostaAPI); 
        apiLiberar(&respostaAPI); 
        return; 
    }
    if (respostaAPI.http != 200) { 
        mostrarErro(&respostaAPI); 
        apiLiberar(&respostaAPI); 
        return; 
    }

    const ValorJSON *listaCaronas = jsonObterCampo(respostaAPI.json, "caronas");
    int contador = 0;

    printf("\n--- Minhas caronas (Trechos Individuais) ---\n");
    for (int i = 0; i < jsonTamanhoArray(listaCaronas); i++) {
        const ValorJSON *carona = jsonObterElemento(listaCaronas, i);
        const char *motorista = jsonObterTexto(carona, "motorista");
        if (!motorista || strcmp(motorista, apiUsuarioId()) != 0) continue;

        printf(" %s\n", txt(jsonObterTexto(carona, "id")));
        const ValorJSON *trechosCarona = jsonObterCampo(carona, "trechos");
        for (int k = 0; k < jsonTamanhoArray(trechosCarona); k++) {
            const ValorJSON *t = jsonObterElemento(trechosCarona, k);
            printf("    %s -> %s | %s | R$ %.2f | %d de %d assentos livres\n",
                   txt(jsonObterTexto(t, "origem")), txt(jsonObterTexto(t, "destino")),
                   txt(jsonObterTexto(t, "horario")), jsonObterNumero(t, "preco", 0),
                   jsonObterInteiro(t, "assentos_livres", 0), jsonObterInteiro(t, "assentos", 0));
        }
        contador++;
    }
    if (!contador) printf(" (nenhuma carona publicada)\n");

    apiLiberar(&respostaAPI);
}

/*
 * Tela do motorista: lista as caronas, pergunta qual excluir e pede confirmação.
 * O servidor recusa (409) se a carona já tem reservas.
 */
static void excluirCarona(void) {
    ApiResp respostaAPI = apiMinhasCaronas();
    
    if (!respostaAPI.rede) { 
        semResposta(&respostaAPI); 
        apiLiberar(&respostaAPI); 
        return; 
    }
    if (respostaAPI.http != 200) { 
        mostrarErro(&respostaAPI); 
        apiLiberar(&respostaAPI); 
        return; 
    }

    const ValorJSON *listaCaronas = jsonObterCampo(respostaAPI.json, "caronas");
    int totalCaronas = jsonTamanhoArray(listaCaronas);

    if (totalCaronas == 0) {
        printf("\nVoce nao tem caronas publicadas.\n");
    } else {
        printf("\n--- Excluir carona (Trecho) ---\n");
        for (int i = 0; i < totalCaronas; i++) {
            const ValorJSON *carona = jsonObterElemento(listaCaronas, i);
            printf(" %d) %s -> %s | %s | %d de %d assentos livres\n", i + 1,
                   txt(jsonObterTexto(carona, "origem")), 
                   txt(jsonObterTexto(carona, "destino")),
                   txt(jsonObterTexto(carona, "horario")),
                   jsonObterInteiro(carona, "assentos_livres", 0), 
                   jsonObterInteiro(carona, "assentos", 0));
        }

        char seletor[16], confirmacao[8];
        if (lerLinha("\nQual excluir? (0 = voltar): ", seletor, sizeof seletor)) {
            int opcao = atoi(seletor);
            
            if (opcao >= 1 && opcao <= totalCaronas) {
                char idParaDeletar[64];
                snprintf(idParaDeletar, sizeof idParaDeletar, "%s", txt(jsonObterTexto(jsonObterElemento(listaCaronas, opcao - 1), "id")));

                if (lerLinha("Confirma a exclusao? [s/N]: ", confirmacao, sizeof confirmacao) &&
                (confirmacao[0] == 's' || confirmacao[0] == 'S')) {
                    
                    ApiResp respostaExclusao = apiExcluirCarona(idParaDeletar);

                    if (!respostaExclusao.rede) semResposta(&respostaExclusao);
                    else if (respostaExclusao.http == 200) printf("\nTrecho excluido.\n");
                    else mostrarErro(&respostaExclusao);

                    apiLiberar(&respostaExclusao);
                }
            }
        }
    }

    apiLiberar(&respostaAPI);
}

/*
 * Menu do motorista logado: publicar, listar e excluir caronas.
 * Retorna quando o usuário escolhe sair (0) ou a entrada fecha.
 */
static void menuMotorista(void) {
    char opcao[16];
    for (;;) {
        printf("\n=========== Motorista: %s ===========\n", apiUsuarioId());
        printf(" 1) Publicar carona (Rotas)\n 2) Minhas caronas (Trechos)\n 3) Excluir carona\n 0) Sair da conta\n");
        if (!lerLinha("> ", opcao, sizeof opcao)) return;

        switch (atoi(opcao)) {
            case 1: publicarCarona(); break;
            case 2: minhasCaronas();  break;
            case 3: excluirCarona();  break;
            case 0: 
                if (opcao[0] == '0') return;
                break;
            default: printf("Opcao invalida.\n");
        }
    }
}

/*
 * Mostra um itinerário da busca: número da opção, preço total e cada trecho
 * (servidor, origem -> destino, horário, preço e assentos livres).
 */
static void mostrarItinerario(int numero, const ValorJSON *itinerario) {
    const ValorJSON *trechosJSON = jsonObterCampo(itinerario, "trechos");
    int numTrechos = jsonTamanhoArray(trechosJSON);
    
    printf("\n %d) R$ %.2f  (%d trecho%s)\n", numero, jsonObterNumero(itinerario, "preco_total", 0),
           numTrechos, numTrechos == 1 ? "" : "s");

    for (int k = 0; k < numTrechos; k++) {
        const ValorJSON *trecho = jsonObterElemento(trechosJSON, k);
        printf("      [%s] %s -> %s  %s  R$ %.2f  (%d livres)\n",
               txt(jsonObterTexto(trecho, "servidor")), 
               txt(jsonObterTexto(trecho, "origem")),
               txt(jsonObterTexto(trecho, "destino")),
               txt(jsonObterTexto(trecho, "horario")),
               jsonObterNumero(trecho, "preco", 0), 
               jsonObterInteiro(trecho, "assentos_livres", 0));
    }
}

/*
 * Procura, dentro de um itinerário, o trecho da carona pedida.
 * Retorna o trecho (pertence ao JSON, não libere) ou NULL se não achar.
 */
static const ValorJSON *trechoDaCarona(const ValorJSON *itinerario, const char *idCaronaBusca) {
    const ValorJSON *trechosJSON = jsonObterCampo(itinerario, "trechos");
    
    for (int k = 0; k < jsonTamanhoArray(trechosJSON); k++) {
        const char *idTrecho = jsonObterTexto(jsonObterElemento(trechosJSON, k), "id_carona");
        if (idTrecho && idCaronaBusca && strcmp(idTrecho, idCaronaBusca) == 0) return jsonObterElemento(trechosJSON, k);
    }
    return NULL;
}

/*
 * Reserva o itinerário escolhido e explica o resultado.
 *   201 -> confirmado: mostra o id da reserva e o assento de cada trecho
 *   409 -> algum assento foi ocupado por outro passageiro
 *   502/503/504 -> alguma empresa do trajeto não respondeu
 *   sem rede -> resultado DESCONHECIDO: manda conferir em "Minhas reservas"
 */
static void reservar(const ValorJSON *itinerario) {
    printf("\nReservando (o servidor coordena a confirmacao com as outras empresas)...\n");

    ApiResp respostaAPI = apiReservar(itinerario);

    if (!respostaAPI.rede) {
        semResposta(&respostaAPI);
        printf("   O resultado e DESCONHECIDO: a reserva pode ter sido concluida ou nao.\n"
               "   Confira em 'Minhas reservas' antes de tentar de novo.\n");
        apiLiberar(&respostaAPI);
        return;
    }

    if (respostaAPI.http == 201) {
        printf("\n*** VIAGEM CONFIRMADA ***\n id da reserva: %s\n",
            txt(jsonObterTexto(respostaAPI.json, "id_transacao")));
            
        const ValorJSON *trechosReservados = jsonObterCampo(respostaAPI.json, "trechos");
        
        for (int i = 0; i < jsonTamanhoArray(trechosReservados); i++) {
            const ValorJSON *trechoAssento = jsonObterElemento(trechosReservados, i);
            const ValorJSON *trechoDetalhe = trechoDaCarona(itinerario, jsonObterTexto(trechoAssento, "id_carona"));
            
            printf("   [%s] %s -> %s   assento %d\n", 
                   txt(jsonObterTexto(trechoAssento, "servidor")),
                   trechoDetalhe ? txt(jsonObterTexto(trechoDetalhe, "origem")) : "?", 
                   trechoDetalhe ? txt(jsonObterTexto(trechoDetalhe, "destino")) : "?",
                   jsonObterInteiro(trechoAssento, "assento", 0));
        }
        printf(" total: R$ %.2f\n", jsonObterNumero(itinerario, "preco_total", 0));

        const ValorJSON *pendencia = jsonObterCampo(respostaAPI.json, "propagacao_pendente");
        if (pendencia && pendencia->tipo == JSON_BOOLEANO && pendencia->booleano) {
            printf(" (a reserva ja esta garantida; alguma empresa vai registrar a confirmacao em segundo plano)\n");
        }
    } else {
        printf("\n!! Reserva NAO realizada (HTTP %ld): %s\n", respostaAPI.http, apiMensagem(&respostaAPI));
        
        if (respostaAPI.http == 409) {
            printf("   Algum assento foi ocupado por outro passageiro. Nada foi reservado; busque de novo.\n");
        } else if (respostaAPI.http == 502 || respostaAPI.http == 503 || respostaAPI.http == 504) {
            printf("   Alguma empresa do trajeto nao respondeu. Nada foi reservado; tente de novo ou escolha outro itinerario.\n");
        }
    }

    apiLiberar(&respostaAPI);
}

/*
 * Tela do passageiro: pergunta origem e destino, busca os itinerários na rede
 * inteira, lista as opções e reserva a escolhida.
 */
static void buscarEReservar(void) {
    char origem[96], destino[96], escolha[16];
    printf("\n--- Buscar viagem ---\n");
    if (!lerLinha("origem: ", origem, sizeof origem) || !lerLinha("destino: ", destino, sizeof destino)) return;

    printf("\nBuscando nas empresas do consorcio (pode levar alguns segundos)...\n");

    ApiResp respostaAPI = apiBuscarItinerarios(origem, destino);
    if (!respostaAPI.rede) { 
        semResposta(&respostaAPI); 
        apiLiberar(&respostaAPI); 
        return; 
    }
    if (respostaAPI.http != 200) { 
        mostrarErro(&respostaAPI); 
        apiLiberar(&respostaAPI); 
        return; 
    }

    const ValorJSON *listaItinerarios = jsonObterCampo(respostaAPI.json, "itinerarios");
    int totalItinerarios = jsonTamanhoArray(listaItinerarios);

    if (totalItinerarios == 0) {
        printf("\nNenhum itinerario com assentos livres de '%s' ate '%s'.\n", origem, destino);
    } else {
        for (int i = 0; i < totalItinerarios; i++) {
            mostrarItinerario(i + 1, jsonObterElemento(listaItinerarios, i));
        }

        if (lerLinha("\nReservar qual itinerario? (0 = voltar): ", escolha, sizeof escolha)) {
            int opcao = atoi(escolha);
            if (opcao >= 1 && opcao <= totalItinerarios) reservar(jsonObterElemento(listaItinerarios, opcao - 1));
        }
    }

    apiLiberar(&respostaAPI);
}

/*
 * Tela do passageiro: lista as reservas dele, com o estado de cada uma
 * (CONFIRMADA, EM ANDAMENTO, CANCELANDO) e os assentos.
 *   - tentarCancelar: 0 só lista; 1 também pergunta qual cancelar e pede confirmação
 * Na hora de cancelar, 200 = cancelada, 202 = registrada (alguma empresa está
 * fora do ar e o servidor conclui depois) e sem rede = resultado desconhecido.
 */
static void reservasInterativo(int tentarCancelar) {
    ApiResp respostaAPI = apiMinhasReservas();
    
    if (!respostaAPI.rede) { 
        semResposta(&respostaAPI); 
        apiLiberar(&respostaAPI); 
        return; 
    }
    if (respostaAPI.http != 200) { 
        mostrarErro(&respostaAPI); 
        apiLiberar(&respostaAPI); 
        return; 
    }

    const ValorJSON *listaReservas = jsonObterCampo(respostaAPI.json, "reservas");
    int totalReservas = jsonTamanhoArray(listaReservas);

    printf("\n--- %s ---\n", tentarCancelar ? "Cancelar reserva" : "Minhas reservas");
    
    for (int i = 0; i < totalReservas; i++) {
        const ValorJSON *reservaAtual = jsonObterElemento(listaReservas, i);
        const char *estadoTransacao = jsonObterTexto(reservaAtual, "estado");
        
        const char *rotuloEstado = !estadoTransacao ? "?" 
                                 : strcmp(estadoTransacao, "COMITADO") == 0 ? "CONFIRMADA"
                                 : strcmp(estadoTransacao, "PREPARADO") == 0 ? "EM ANDAMENTO"
                                 : strcmp(estadoTransacao, "CANCELANDO") == 0 ? "CANCELANDO" : estadoTransacao;

        const ValorJSON *trechosReserva = jsonTamanhoArray(jsonObterCampo(reservaAtual, "itinerario")) 
                                          ? jsonObterCampo(reservaAtual, "itinerario") 
                                          : jsonObterCampo(reservaAtual, "trechos");
                                          
        if (tentarCancelar) printf("\n %d) [%s] %s\n", i + 1, rotuloEstado, txt(jsonObterTexto(reservaAtual, "id_transacao")));
        else                printf("\n [%s] %s\n", rotuloEstado, txt(jsonObterTexto(reservaAtual, "id_transacao")));

        for (int k = 0; k < jsonTamanhoArray(trechosReserva); k++) {
            const ValorJSON *trechoAtual = jsonObterElemento(trechosReserva, k);
            int assentoPoltrona = jsonObterInteiro(trechoAtual, "assento", 0);
            
            printf("      %s : %s", txt(jsonObterTexto(trechoAtual, "servidor")), txt(jsonObterTexto(trechoAtual, "id_carona")));
            if (assentoPoltrona > 0) printf("   assento %d", assentoPoltrona);
            printf("\n");
        }
    }
    if (!totalReservas) printf(" (nenhuma reserva)\n");

    if (tentarCancelar && totalReservas > 0) {
        char seletor[16], confirmacao[8];
        if (lerLinha("\nQual cancelar? (0 = voltar): ", seletor, sizeof seletor)) {
            int opcaoSelecionada = atoi(seletor);
            
            if (opcaoSelecionada >= 1 && opcaoSelecionada <= totalReservas) {
                char idTransacaoCancelamento[128];
                snprintf(idTransacaoCancelamento, sizeof idTransacaoCancelamento, "%s", txt(jsonObterTexto(jsonObterElemento(listaReservas, opcaoSelecionada - 1), "id_transacao")));

                if (lerLinha("Confirma o cancelamento? [s/N]: ", confirmacao, sizeof confirmacao) && (confirmacao[0] == 's' || confirmacao[0] == 'S')) {
                    printf("\nCancelando (o servidor avisa as outras empresas do trajeto)...\n");

                    ApiResp respostaCancelamento = apiCancelarReserva(idTransacaoCancelamento);
                    
                    if (!respostaCancelamento.rede) {
                        semResposta(&respostaCancelamento);
                        printf("   O resultado e DESCONHECIDO: confira em 'Minhas reservas'.\n");
                    } else if (respostaCancelamento.http == 200) {
                        printf("\nReserva cancelada. Os assentos foram liberados em todas as empresas.\n");
                    } else if (respostaCancelamento.http == 202) {
                        printf("\nCancelamento REGISTRADO, mas alguma empresa do trajeto esta fora do ar agora.\n"
                               "O servidor conclui sozinho assim que ela voltar (veja 'Minhas reservas').\n");
                    } else {
                        mostrarErro(&respostaCancelamento);
                    }
                    apiLiberar(&respostaCancelamento);
                }
            }
        }
    }

    apiLiberar(&respostaAPI);
}

/*
 * Menu do passageiro logado: buscar e reservar, ver reservas e cancelar.
 * Retorna quando o usuário escolhe sair (0) ou a entrada fecha.
 */
static void menuPassageiro(void) {
    char opcao[16];
    for (;;) {
        printf("\n=========== Passageiro: %s ===========\n", apiUsuarioId());
        printf(" 1) Buscar viagem e reservar\n 2) Minhas reservas\n 3) Cancelar reserva\n 0) Sair da conta\n");
        if (!lerLinha("> ", opcao, sizeof opcao)) return;

        switch (atoi(opcao)) {
            case 1: buscarEReservar(); break;
            case 2: reservasInterativo(0); break;
            case 3: reservasInterativo(1); break;
            case 0: 
                if (opcao[0] == '0') return;
                break;
            default: printf("Opcao invalida.\n");
        }
    }
}

/* Testa a conexão com o servidor (GET /health) e mostra o resultado. */
static void verSaude(void) {
    ApiResp respostaAPI = apiSaude();
    if (!respostaAPI.rede) {
        semResposta(&respostaAPI);
    } else {
        printf("\nServidor %s respondeu HTTP %ld: %s\n", apiBaseURL(), respostaAPI.http, txt(jsonObterTexto(respostaAPI.json, "status")));
    }
    apiLiberar(&respostaAPI);
}

/*
 * Ponto de entrada. O argumento escolhe o servidor:
 *   - nome de região (ex.: "skyrim"): usa a porta padrão do mapa e o host de CLIENTE_HOST
 *   - "http://host:porta" ou "host:porta": usa o endereço informado
 *   - sem argumento: "skyrim"
 * "-h" ou "--help" mostra o uso e as regiões disponíveis.
 */
int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *alvoConexao = argc >= 2 ? argv[1] : "skyrim";
    const char *hostAmbiente = getenv("CLIENTE_HOST") ? getenv("CLIENTE_HOST") : "localhost";
    char urlBaseConexao[256];

    if (strcmp(alvoConexao, "-h") == 0 || strcmp(alvoConexao, "--help") == 0) {
        printf("Uso: %s [regiao | host:porta | http://host:porta]\n\n", argv[0]);
        listarRegioes();
        return 0;
    }

    if (strstr(alvoConexao, "://")) {
        snprintf(urlBaseConexao, sizeof urlBaseConexao, "%s", alvoConexao);
    } else if (configuracoesCarregar(alvoConexao, 0) == 0) {
        snprintf(urlBaseConexao, sizeof urlBaseConexao, "http://%s:%d", hostAmbiente, servidor.porta);
    } else {
        snprintf(urlBaseConexao, sizeof urlBaseConexao, "http://%s", alvoConexao);
    }

    apiIniciar(urlBaseConexao);
    printf("Cliente do consorcio de caronas -> %s\n", apiBaseURL());

    char opcaoEscolhida[16];
    for (;;) {
        printf("\n=========== Consorcio de Caronas ===========\n");
        printf(" 1) Cadastrar-se\n 2) Entrar\n 3) Testar conexao com o servidor\n 0) Sair\n");
        if (!lerLinha("> ", opcaoEscolhida, sizeof opcaoEscolhida)) break;

        int numSelecionado = atoi(opcaoEscolhida);
        if (numSelecionado == 0 && opcaoEscolhida[0] == '0') break;

        if (numSelecionado == 1) {
            cadastrar();
        } else if (numSelecionado == 2) {
            if (entrar()) {
                if (strcmp(apiUsuarioTipo(), "motorista") == 0) menuMotorista(); else menuPassageiro();
                apiSair();
            }
        } else if (numSelecionado == 3) {
            verSaude();
        } else {
            printf("Opcao invalida.\n");
        }
    }

    apiEncerrar();
    return 0;
}
