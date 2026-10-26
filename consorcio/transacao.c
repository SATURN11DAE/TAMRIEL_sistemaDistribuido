/*
 * Implementação das Transações Distribuídas (Two-Phase Commit em cadeia)
 *
 * Como funciona uma reserva que passa por vários servidores:
 *   1. O servidor que recebe o pedido do passageiro é o COORDENADOR (txReservarComo).
 *      Ele reserva os assentos dele (fase PREPARE) e repassa o restante do
 *      itinerário ao próximo servidor da cadeia.
 *   2. Cada participante (txPrepare) faz o mesmo: reserva os seus assentos, repassa
 *      ao seguinte e devolve a lista de assentos de todos os que vêm depois.
 *   3. Se todos disseram "READY", o coordenador DECIDE o commit (grava COMITADO
 *      em disco) e manda o commit pela cadeia (txCommit). Se alguém recusou ou
 *      caiu, o coordenador desfaz tudo (rollback).
 *   4. Quem ficou PREPARADO sem receber decisão é resolvido pela thread de
 *      recuperação, que pergunta o estado ao servidor anterior da cadeia.
 *
 * Estados de uma reserva: PREPARADO -> COMITADO -> CANCELANDO -> (apagada).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "headers/configuracoes.h"
#include "headers/lamport.h"
#include "headers/persistencia.h"
#include "headers/json.h"
#include "headers/httpCliente.h"
#include "headers/transacao.h"

/*
 * Utilitário para testes: se a variável de ambiente INJETAR_FALHA for igual a
 * 'ponto', o processo morre na hora (simula uma queda do servidor nesse ponto).
 * Pontos usados: "antes_decisao", "depois_decisao" e "depois_cancelando".
 */
static void injetarFalha(const char *ponto) {
    const char *envFalha = getenv("INJETAR_FALHA");
    if (envFalha && strcmp(envFalha, ponto) == 0) {
        printf("[FALHA-INJETADA] processo encerrado em '%s'\n", ponto);
        fflush(stdout);
        _exit(137);
    }
}

/* Motivo de uma recusa ou falha ao encaminhar o prepare ao próximo servidor. */
typedef struct {
    unsigned int http;     /* código HTTP a devolver (409, 404 ou 502) */
    char motivo[320];      /* texto explicando a falha */
    int falhaRede;         /* 1 se o próximo servidor não respondeu (timeout, queda...) */
} Falha;

/* Funções Auxiliares de Transação */

/*
 * Conta quantos servidores diferentes aparecem no vetor, considerando só
 * mudanças consecutivas (os trechos do mesmo servidor vêm juntos).
 */
static int contarServidores(const TrechoRes *trechos, int quantidade) {
    int contagem = 0;
    for (int i = 0; i < quantidade; i++) {
        if (i == 0 || strcmp(trechos[i].servidor, trechos[i - 1].servidor) != 0) contagem++;
    }
    return contagem;
}

/*
 * Monta o corpo JSON do prepare enviado ao próximo servidor.
 *   - trechos / quantidade: o que ainda falta reservar (inclui os do próprio destinatário)
 */
static void montarCorpoPrepare(Buffer *buffer, const Reserva *reserva, const TrechoRes *trechos, int quantidade) {
    bufferAdicionar(buffer, "{\"id_transacao\": ");    
    bufferAdicionarStringJSON(buffer, reserva->idTransacao);
    
    bufferAdicionar(buffer, ", \"coordenador_id\": "); 
    bufferAdicionarStringJSON(buffer, reserva->coordenadorId);
    
    bufferAdicionar(buffer, ", \"passageiro\": ");     
    bufferAdicionarStringJSON(buffer, reserva->passageiro);
    
    bufferAdicionar(buffer, ", \"relogio_logico\": %d, \"trechos_solicitados\": [", reserva->relogio);
    
    for (int i = 0; i < quantidade; i++) {
        bufferAdicionar(buffer, "%s{\"servidor\": ", i ? ", " : ""); 
        bufferAdicionarStringJSON(buffer, trechos[i].servidor);
        
        bufferAdicionar(buffer, ", \"id_carona\": ");  
        bufferAdicionarStringJSON(buffer, trechos[i].idCarona);

        bufferAdicionar(buffer, ", \"origem\": ");
        bufferAdicionarStringJSON(buffer, trechos[i].origem);

        bufferAdicionar(buffer, ", \"destino\": ");
        bufferAdicionarStringJSON(buffer, trechos[i].destino);
        
        bufferAdicionar(buffer, "}");
    }
    bufferAdicionar(buffer, "]}");
}

/*
 * Escreve como array JSON os trechos já com o assento escolhido
 * (usado na resposta "READY" do prepare e na confirmação final).
 */
static void assentosJSON(Buffer *buffer, const TrechoRes *trechos, int quantidade) {
    bufferAdicionar(buffer, "[");
    for (int i = 0; i < quantidade; i++) {
        bufferAdicionar(buffer, "%s{\"servidor\": ", i ? ", " : ""); 
        bufferAdicionarStringJSON(buffer, trechos[i].servidor);
        
        bufferAdicionar(buffer, ", \"id_carona\": ");
        bufferAdicionarStringJSON(buffer, trechos[i].idCarona);
        
        bufferAdicionar(buffer, ", \"origem\": ");
        bufferAdicionarStringJSON(buffer, trechos[i].origem);

        bufferAdicionar(buffer, ", \"destino\": ");
        bufferAdicionarStringJSON(buffer, trechos[i].destino);
        
        bufferAdicionar(buffer, ", \"assento\": %d}", trechos[i].assento);
    }
    bufferAdicionar(buffer, "]");
}

/*
 * Envia commit ou rollback ao próximo servidor da cadeia.
 *   - tipo: "commit" ou "rollback"
 *   - proximo: id do próximo servidor ("" se este for o último: nada a fazer)
 * Retorna 0 se entregue (ou se não há próximo), -1 se não foi entregue.
 * Quando falha, o próximo servidor resolve sozinho na recuperação.
 */
static int propagarComando(const char *tipo, const char *proximo, const char *idTransacao) {
    if (!proximo[0]) return 0;

    Resposta resp;
    int rc = (strcmp(tipo, "commit") == 0) ? clienteCommitar(proximo, idTransacao, &resp)
                                           : clienteRollback(proximo, idTransacao, &resp);
    
    int entregue = (rc == 0 && resp.codigoHTTP == 200);

    if (!entregue) {
        printf("[2PC] %s de %s NAO entregue a %s (%s); ele resolve na recuperacao\n", 
               tipo, idTransacao, proximo, rc != 0 ? resp.erro : "resposta != 200");
    }
    
    respostaLiberar(&resp);
    return entregue ? 0 : -1;
}

/*
 * Monta a resposta "NO" do prepare (este servidor recusou a transação).
 * Corpo: {"id_transacao", "status": "NO", "servidor", "motivo"}.
 */
static RespostaServidor negarAcesso(unsigned int http, const char *idTransacao, const char *motivo) {
    Buffer buffer; 
    bufferIniciar(&buffer);
    
    bufferAdicionar(&buffer, "{\"id_transacao\": "); 
    bufferAdicionarStringJSON(&buffer, idTransacao);
    
    bufferAdicionar(&buffer, ", \"status\": \"NO\", \"servidor\": "); 
    bufferAdicionarStringJSON(&buffer, servidor.id);
    
    bufferAdicionar(&buffer, ", \"motivo\": "); 
    bufferAdicionarStringJSON(&buffer, motivo);
    bufferAdicionar(&buffer, "}\n");
    
    return criarRespostaJSON(http, bufferExtrairTexto(&buffer));
}

/*
 * Traduz o código de retorno de reservaPreparar para o código HTTP:
 * NAO_ENCONTRADO -> 404, INDISPONIVEL -> 409, INVALIDO -> 400, outros -> 500.
 */
static unsigned int httpDeRc(int rc) {
    return rc == ARMAZENAMENTO_NAO_ENCONTRADO ? 404
         : rc == ARMAZENAMENTO_INDISPONIVEL   ? 409
         : rc == ARMAZENAMENTO_INVALIDO       ? 400 : 500;
}

/*
 * Escreve em 'saida' a mensagem explicando por que reservaPreparar falhou.
 *   - reserva: usa reserva->falha (id da carona responsável)
 */
static void motivoReserva(char *saida, size_t tamanho, int rc, const Reserva *reserva) {
    if (rc == ARMAZENAMENTO_NAO_ENCONTRADO) {
        snprintf(saida, tamanho, "carona %s nao existe em %s", reserva->falha, servidor.id);
    } else if (rc == ARMAZENAMENTO_INDISPONIVEL) {
        snprintf(saida, tamanho, "sem assento disponivel na carona %s (%s)", reserva->falha, servidor.id);
    } else if (rc == ARMAZENAMENTO_INVALIDO) {
        snprintf(saida, tamanho, "origem/destino fora da rota da carona %s (%s)", reserva->falha, servidor.id);
    } else {
        snprintf(saida, tamanho, "falha ao gravar a reserva em disco (%s)", servidor.id);
    }
}

/*
 * Repassa o prepare ao próximo servidor da cadeia e espera a resposta.
 *   - resto / quantidadeRestante: trechos que faltam (o primeiro é do próximo servidor)
 *   - agrupamento / qtdAgrupamento: recebem os assentos devolvidos por ele (e pelos seguintes)
 *   - falha: preenchida com http, motivo e falhaRede quando não deu certo
 * O prazo cresce com a quantidade de servidores restantes, pois a chamada é encadeada.
 * Retorna 0 se o próximo respondeu READY (HTTP 200), -1 se recusou ou não respondeu.
 */
static int encaminharPrepare(const Reserva *reserva, const TrechoRes *resto, int quantidadeRestante,
                             TrechoRes *agrupamento, int *qtdAgrupamento, Falha *falha) {
    
    const char *proximoServidor = resto[0].servidor;

    Buffer buffer; 
    bufferIniciar(&buffer);
    montarCorpoPrepare(&buffer, reserva, resto, quantidadeRestante);

    long timeout = (long) (TIMEOUT_REQUISICAO + ESPERA_MAX_MS) * contarServidores(resto, quantidadeRestante);

    Resposta resp;
    int rc = clientePrepararCorpo(proximoServidor, buffer.p, timeout, &resp);
    bufferLiberar(&buffer);

    memset(falha, 0, sizeof *falha);

    if (rc != 0) {
        falha->http = 502; 
        falha->falhaRede = 1;
        snprintf(falha->motivo, sizeof falha->motivo, "servidor %s indisponivel (%s)", proximoServidor, resp.erro);
        respostaLiberar(&resp);
        return -1;
    }

    ValorJSON *jsonResp = jsonAnalisar(resp.corpo);

    if (resp.codigoHTTP == 200) {
        const ValorJSON *assentosResp = jsonObterCampo(jsonResp, "assentos");
        int numAssentos = jsonTamanhoArray(assentosResp);
        
        for (int i = 0; i < numAssentos && *qtdAgrupamento < MAX_TRECHOS_TX; i++) {
            const ValorJSON *a = jsonObterElemento(assentosResp, i);
            TrechoRes *destino = &agrupamento[(*qtdAgrupamento)++];
            
            memset(destino, 0, sizeof *destino);
            
            const char *serv = jsonObterTexto(a, "servidor");
            snprintf(destino->servidor, sizeof destino->servidor, "%s", serv ? serv : "");
            
            const char *idc = jsonObterTexto(a, "id_carona");
            snprintf(destino->idCarona, sizeof destino->idCarona, "%s", idc ? idc : "");

            const char *org = jsonObterTexto(a, "origem");
            snprintf(destino->origem, sizeof destino->origem, "%s", org ? org : "");

            const char *dst = jsonObterTexto(a, "destino");
            snprintf(destino->destino, sizeof destino->destino, "%s", dst ? dst : "");
            
            destino->assento = jsonObterInteiro(a, "assento", 0);
        }
        
        jsonLiberar(jsonResp);
        respostaLiberar(&resp);
        return 0;
    }

    const char *mensagemErro = jsonObterTexto(jsonResp, "motivo");
    if (!mensagemErro) mensagemErro = jsonObterTexto(jsonResp, "erro");
    
    falha->http = (resp.codigoHTTP == 409 || resp.codigoHTTP == 404) ? (unsigned) resp.codigoHTTP : 502;
    snprintf(falha->motivo, sizeof falha->motivo, "%s", mensagemErro ? mensagemErro : "recusado");
    
    jsonLiberar(jsonResp);
    respostaLiberar(&resp);
    return -1;
}

/* APIs Públicas do Two-Phase Commit */

/*
 * PARTICIPANTE, fase 1: recebe o prepare (POST /reservas/prepare).
 * Reserva os trechos que pertencem a ESTE servidor, repassa o restante ao
 * próximo servidor da cadeia e responde com os assentos de todos.
 *   - corpo: JSON com id_transacao, coordenador_id, passageiro, relogio_logico
 *     e trechos_solicitados (os deste servidor devem vir primeiro)
 *   - remetente: X-Server-Id de quem enviou (vira o "anterior" da reserva)
 * Idempotente: um prepare repetido não reserva duas vezes.
 * Retorna 200 {"status": "READY", "assentos": [...]} se deu certo, ou o erro
 * (400 corpo inválido, 404 carona inexistente, 409 sem assento, 502 falha adiante).
 * Se algo falha adiante, a reserva local é desfeita antes de responder.
 */
RespostaServidor txPrepare(const char *corpo, const char *remetente) {
    ValorJSON *json = corpo ? jsonAnalisar(corpo) : NULL;
    if (!json) return criarRespostaErro(400, "Corpo JSON invalido");

    const char *idTransacao = jsonObterTexto(json, "id_transacao");
    const char *coordenador = jsonObterTexto(json, "coordenador_id");
    const ValorJSON *listaTrechos = jsonObterCampo(json, "trechos_solicitados");
    int numTrechos = jsonTamanhoArray(listaTrechos);

    if (!idTransacao || !identificadorValido(idTransacao)) { 
        jsonLiberar(json); 
        return criarRespostaErro(400, "id_transacao ausente ou invalido"); 
    }
    if (!coordenador || !*coordenador) { 
        jsonLiberar(json); 
        return criarRespostaErro(400, "Corpo sem coordenador_id"); 
    }
    if (numTrechos < 1 || numTrechos > MAX_TRECHOS_TX) { 
        jsonLiberar(json); 
        return criarRespostaErro(400, "trechos_solicitados deve ter de 1 a 8 itens"); 
    }

    Reserva reservaAtual;
    memset(&reservaAtual, 0, sizeof reservaAtual);
    snprintf(reservaAtual.idTransacao, sizeof reservaAtual.idTransacao, "%s", idTransacao);
    snprintf(reservaAtual.coordenadorId, sizeof reservaAtual.coordenadorId, "%s", coordenador);
    
    const char *passageiro = jsonObterTexto(json, "passageiro");
    snprintf(reservaAtual.passageiro, sizeof reservaAtual.passageiro, "%s", passageiro ? passageiro : "");
    snprintf(reservaAtual.anterior, sizeof reservaAtual.anterior, "%s", (remetente && *remetente) ? remetente : coordenador);
    
    reservaAtual.relogio = jsonObterInteiro(json, "relogio_logico", 0);
    reservaAtual.criadoEm = agoraEmSegundos();

    TrechoRes todosTrechos[MAX_TRECHOS_TX];
    for (int i = 0; i < numTrechos; i++) {
        const ValorJSON *trechoAtual = jsonObterElemento(listaTrechos, i);
        const char *servidorTrecho = jsonObterTexto(trechoAtual, "servidor");
        const char *idCaronaTrecho = jsonObterTexto(trechoAtual, "id_carona");
        
        if (!servidorTrecho || !idCaronaTrecho || !identificadorValido(idCaronaTrecho)) { 
            jsonLiberar(json); 
            return criarRespostaErro(400, "trecho sem servidor ou com id_carona invalido"); 
        }

        memset(&todosTrechos[i], 0, sizeof todosTrechos[i]);
        configuracoesNormalizar(servidorTrecho, todosTrechos[i].servidor, sizeof todosTrechos[i].servidor);
        snprintf(todosTrechos[i].idCarona, sizeof todosTrechos[i].idCarona, "%s", idCaronaTrecho);

        const char *origemTrecho = jsonObterTexto(trechoAtual, "origem");
        const char *destinoTrecho = jsonObterTexto(trechoAtual, "destino");
        if (!origemTrecho || !*origemTrecho || !destinoTrecho || !*destinoTrecho) {
            jsonLiberar(json);
            return criarRespostaErro(400, "trecho sem origem ou destino");
        }
        snprintf(todosTrechos[i].origem, sizeof todosTrechos[i].origem, "%s", origemTrecho);
        snprintf(todosTrechos[i].destino, sizeof todosTrechos[i].destino, "%s", destinoTrecho);
    }
    jsonLiberar(json);

    int k = 0;
    while (k < numTrechos && strcmp(todosTrechos[k].servidor, servidor.id) == 0) {
        for (int q = 0; q < reservaAtual.qtdLocais; q++) {
            if (strcmp(reservaAtual.locais[q].idCarona, todosTrechos[k].idCarona) == 0) {
                return criarRespostaErro(400, "carona repetida na transacao");
            }
        }
        reservaAtual.locais[reservaAtual.qtdLocais++] = todosTrechos[k++];
    }
    
    if (reservaAtual.qtdLocais == 0) return criarRespostaErro(400, "O primeiro trecho solicitado nao pertence a este servidor");

    for (int q = k; q < numTrechos; q++) {
        if (strcmp(todosTrechos[q].servidor, servidor.id) == 0) {
            return criarRespostaErro(400, "Servidor aparece duas vezes na cadeia");
        }
    }
    
    if (k < numTrechos) {
        snprintf(reservaAtual.proximo, sizeof reservaAtual.proximo, "%s", todosTrechos[k].servidor);
        if (!buscarVizinho(reservaAtual.proximo)) return criarRespostaErro(400, "O proximo servidor da cadeia nao faz fronteira com este");
    }

    printf("[2PC] PREPARE tx=%s coordenador=%s relogio=%d trechos_meus=%d proximo=%s\n",
           reservaAtual.idTransacao, reservaAtual.coordenadorId, reservaAtual.relogio, reservaAtual.qtdLocais, reservaAtual.proximo[0] ? reservaAtual.proximo : "-");

    int rc = reservaPreparar(&reservaAtual, ESPERA_MAX_MS);
    if (rc != ARMAZENAMENTO_OK) {
        char motivo[320];
        motivoReserva(motivo, sizeof motivo, rc, &reservaAtual);

        return negarAcesso(httpDeRc(rc), idTransacao, motivo);
    }

    TrechoRes agrupamento[MAX_TRECHOS_TX];
    int qtdAgrupamento = 0;
    for (int q = 0; q < reservaAtual.qtdLocais && qtdAgrupamento < MAX_TRECHOS_TX; q++) {
        agrupamento[qtdAgrupamento++] = reservaAtual.locais[q];
    }

    if (!reservaAtual.jaExistia && reservaAtual.proximo[0]) {
        Falha falhaRede;
        if (encaminharPrepare(&reservaAtual, &todosTrechos[k], numTrechos - k, agrupamento, &qtdAgrupamento, &falhaRede) != 0) {
            reservaRemover(idTransacao);
            if (falhaRede.falhaRede) propagarComando("rollback", reservaAtual.proximo, idTransacao);
            printf("[2PC] PREPARE tx=%s abortado aqui: %s\n", idTransacao, falhaRede.motivo);
            return negarAcesso(falhaRede.http, idTransacao, falhaRede.motivo);
        }
    }

    if (!reservaAtual.jaExistia) reservaAtivo(idTransacao, 0);

    Buffer buffer; 
    bufferIniciar(&buffer);
    bufferAdicionar(&buffer, "{\"id_transacao\": "); 
    bufferAdicionarStringJSON(&buffer, idTransacao);
    
    bufferAdicionar(&buffer, ", \"status\": \"READY\", \"servidor\": "); 
    bufferAdicionarStringJSON(&buffer, servidor.id);
    
    bufferAdicionar(&buffer, ", \"assentos\": ");
    assentosJSON(&buffer, agrupamento, qtdAgrupamento);
    bufferAdicionar(&buffer, "}\n");
    
    return criarRespostaJSON(200, bufferExtrairTexto(&buffer));
}

/*
 * PARTICIPANTE, fase 2: confirma a reserva (PUT /reservas/{id}/commit).
 * Grava COMITADO em disco e repassa o commit ao próximo servidor da cadeia.
 * Retorna 200 (com "propagacao_pendente" = true se o próximo não recebeu;
 * ele se resolve na recuperação), 404 se não houve prepare ou 500 em falha de disco.
 */
RespostaServidor txCommit(const char *idTransacao) {
    printf("[2PC] COMMIT tx=%s\n", idTransacao);

    Reserva reservaAtual;
    if (reservaBuscar(idTransacao, &reservaAtual) != ARMAZENAMENTO_OK) {
        return criarRespostaErro(404, "Transacao desconhecida (nao houve prepare)");
    }

    if (reservaDecidir(idTransacao, "COMITADO", NULL, 0) != ARMAZENAMENTO_OK) {
        return criarRespostaErro(500, "Falha ao gravar a reserva em disco");
    }

    int propagacaoPendente = (propagarComando("commit", reservaAtual.proximo, idTransacao) != 0);

    Buffer buffer; 
    bufferIniciar(&buffer);
    bufferAdicionar(&buffer, "{\"id_transacao\": "); 
    bufferAdicionarStringJSON(&buffer, idTransacao);
    bufferAdicionar(&buffer, ", \"mensagem\": \"Transacao COMMITTED com sucesso.\", \"propagacao_pendente\": %s}\n",
           propagacaoPendente ? "true" : "false");
           
    return criarRespostaJSON(200, bufferExtrairTexto(&buffer));
}

/*
 * PARTICIPANTE, fase 2 (abortar): desfaz a reserva preparada
 * (DELETE /reservas/{id}) e repassa o rollback ao próximo servidor.
 * Retorna 200 (também se a transação não existia: o rollback é idempotente),
 * 409 se a reserva já foi COMITADA, ou 500 em falha de disco.
 */
RespostaServidor txRollback(const char *idTransacao) {
    printf("[2PC] ROLLBACK tx=%s\n", idTransacao);

    Reserva reservaAtual;
    int transacaoExistia = (reservaBuscar(idTransacao, &reservaAtual) == ARMAZENAMENTO_OK);

    int rc = reservaRemover(idTransacao);
    if (rc == ARMAZENAMENTO_CONFLITO) return criarRespostaErro(409, "Transacao ja COMITADA: rollback recusado");
    if (rc == ARMAZENAMENTO_ERRO) return criarRespostaErro(500, "Falha ao gravar a remocao em disco");

    if (transacaoExistia) propagarComando("rollback", reservaAtual.proximo, idTransacao);

    Buffer buffer; 
    bufferIniciar(&buffer);
    bufferAdicionar(&buffer, "{\"id_transacao\": "); 
    bufferAdicionarStringJSON(&buffer, idTransacao);
    bufferAdicionar(&buffer, ", \"mensagem\": \"Transacao ABORTED com sucesso.\"}\n");
    
    return criarRespostaJSON(200, bufferExtrairTexto(&buffer));
}

/*
 * Informa o estado de uma transação (GET /reservas/{id}/status).
 * É a pergunta que a recuperação faz ao servidor anterior da cadeia.
 * Retorna 200 com {"status": <estado>, "coordenador_id"} ou 404 se desconhecida.
 */
RespostaServidor txStatus(const char *idTransacao) {
    Reserva reservaAtual;
    if (reservaBuscar(idTransacao, &reservaAtual) != ARMAZENAMENTO_OK) return criarRespostaErro(404, "Transacao desconhecida");

    Buffer buffer; 
    bufferIniciar(&buffer);
    
    bufferAdicionar(&buffer, "{\"id_transacao\": "); 
    bufferAdicionarStringJSON(&buffer, idTransacao);
    
    bufferAdicionar(&buffer, ", \"status\": ");      
    bufferAdicionarStringJSON(&buffer, reservaAtual.estado);
    
    bufferAdicionar(&buffer, ", \"coordenador_id\": "); 
    bufferAdicionarStringJSON(&buffer, reservaAtual.coordenadorId);
    bufferAdicionar(&buffer, "}\n");
    
    return criarRespostaJSON(200, bufferExtrairTexto(&buffer));
}

/*
 * Monta a resposta de recusa enviada ao passageiro: {"status": "RECUSADO", "motivo"}.
 */
static RespostaServidor respostaRecusada(unsigned int http, const char *motivo) {
    Buffer buffer; 
    bufferIniciar(&buffer);
    
    bufferAdicionar(&buffer, "{\"status\": \"RECUSADO\", \"motivo\": ");
    bufferAdicionarStringJSON(&buffer, motivo);
    bufferAdicionar(&buffer, "}\n");
    
    return criarRespostaJSON(http, bufferExtrairTexto(&buffer));
}

/*
 * COORDENADOR: reserva um itinerário completo para um passageiro (POST /reservas).
 *   - forcado: id do passageiro vindo da autenticação (sobrescreve o do corpo);
 *     NULL usa o campo "passageiro" do corpo
 *   - corpo: {"passageiro", "trechos": [{"servidor", "id_carona", "origem", "destino"}, ...]}
 * Valida o itinerário (trechos deste servidor primeiro, cadeia de servidores
 * seguindo o mapa de fronteiras, sem caronas repetidas), reserva os assentos
 * locais, encaminha o prepare, decide o commit e o propaga.
 * Retorna 201 {"status": "CONFIRMADO", ...} se deu certo; senão 400/403/404 (pedido
 * inválido) ou a resposta RECUSADO com 404/409/502 conforme o motivo. Nesse caso
 * nada fica reservado.
 */
RespostaServidor txReservarComo(const char *forcado, const char *corpo) {
    ValorJSON *json = corpo ? jsonAnalisar(corpo) : NULL;
    if (!json) return criarRespostaErro(400, "Corpo JSON invalido");

    char passageiroFinal[64] = "";
    const char *passJSON = jsonObterTexto(json, "passageiro");
    if (passJSON) snprintf(passageiroFinal, sizeof passageiroFinal, "%s", passJSON);
    if (forcado) snprintf(passageiroFinal, sizeof passageiroFinal, "%s", forcado);
    
    const ValorJSON *listaTrechos = jsonObterCampo(json, "trechos");
    int numTrechos = jsonTamanhoArray(listaTrechos);

    Usuario usuarioPassageiro;
    if (!passageiroFinal[0] || !identificadorValido(passageiroFinal)) { 
        jsonLiberar(json); 
        return criarRespostaErro(400, "Informe o passageiro"); 
    }
    if (usuarioBuscar(passageiroFinal, &usuarioPassageiro) != ARMAZENAMENTO_OK) { 
        jsonLiberar(json); 
        return criarRespostaErro(404, "Passageiro nao cadastrado neste servidor"); 
    }
    if (strcmp(usuarioPassageiro.tipo, "passageiro") != 0) { 
        jsonLiberar(json); 
        return criarRespostaErro(403, "O usuario informado nao e passageiro"); 
    }
    if (numTrechos < 1 || numTrechos > MAX_TRECHOS_TX) { 
        jsonLiberar(json); 
        return criarRespostaErro(400, "trechos deve ter de 1 a 8 itens"); 
    }

    TrechoRes trechosParsed[MAX_TRECHOS_TX];
    for (int i = 0; i < numTrechos; i++) {
        const ValorJSON *itemTrecho = jsonObterElemento(listaTrechos, i);
        const char *servidorAlvo = jsonObterTexto(itemTrecho, "servidor");
        const char *idCaronaAlvo = jsonObterTexto(itemTrecho, "id_carona");
        
        if (!servidorAlvo || !idCaronaAlvo || !identificadorValido(idCaronaAlvo)) { 
            jsonLiberar(json); 
            return criarRespostaErro(400, "Trecho sem servidor ou com id_carona invalido"); 
        }

        memset(&trechosParsed[i], 0, sizeof trechosParsed[i]);
        configuracoesNormalizar(servidorAlvo, trechosParsed[i].servidor, sizeof trechosParsed[i].servidor);
        snprintf(trechosParsed[i].idCarona, sizeof trechosParsed[i].idCarona, "%s", idCaronaAlvo);

        const char *origemTrecho = jsonObterTexto(itemTrecho, "origem");
        const char *destinoTrecho = jsonObterTexto(itemTrecho, "destino");
        if (!origemTrecho || !*origemTrecho || !destinoTrecho || !*destinoTrecho) {
            jsonLiberar(json);
            return criarRespostaErro(400, "Trecho sem origem ou destino");
        }
        snprintf(trechosParsed[i].origem, sizeof trechosParsed[i].origem, "%s", origemTrecho);
        snprintf(trechosParsed[i].destino, sizeof trechosParsed[i].destino, "%s", destinoTrecho);

        if (!configuracoesRegiaoValida(trechosParsed[i].servidor)) { 
            jsonLiberar(json); 
            return criarRespostaErro(400, "Servidor desconhecido em um dos trechos"); 
        }
        for (int q = 0; q < i; q++) {
            if (strcmp(trechosParsed[q].idCarona, trechosParsed[i].idCarona) == 0 && strcmp(trechosParsed[q].servidor, trechosParsed[i].servidor) == 0) {
                jsonLiberar(json); 
                return criarRespostaErro(400, "Carona repetida no itinerario");
            }
        }
    }
    jsonLiberar(json);

    const char *servidorAnterior = servidor.id;
    char servidoresVisitados[MAX_TRECHOS_TX][MAX_ID];
    int qtdVisitados = 0;
    
    for (int i = 0; i < numTrechos; i++) {
        if (i > 0 && strcmp(trechosParsed[i].servidor, trechosParsed[i - 1].servidor) == 0) continue;

        for (int q = 0; q < qtdVisitados; q++) {
            if (strcmp(servidoresVisitados[q], trechosParsed[i].servidor) == 0) {
                return criarRespostaErro(400, "Servidor repetido de forma nao consecutiva no itinerario");
            }
        }
        snprintf(servidoresVisitados[qtdVisitados++], MAX_ID, "%s", trechosParsed[i].servidor);

        if (strcmp(trechosParsed[i].servidor, servidor.id) == 0) {
            if (i != 0) return criarRespostaErro(400, "Os trechos deste servidor devem vir primeiro no itinerario");
        } else if (!configuracoesSaoVizinhas(servidorAnterior, trechosParsed[i].servidor)) {
            char mensagem[260];
            snprintf(mensagem, sizeof mensagem, "%s e %s nao fazem fronteira: a cadeia de participantes deve seguir o mapa", servidorAnterior, trechosParsed[i].servidor);
            return criarRespostaErro(400, mensagem);
        }
        servidorAnterior = trechosParsed[i].servidor;
    }

    Reserva reservaAtual;
    memset(&reservaAtual, 0, sizeof reservaAtual);
    gerarUUID(reservaAtual.idTransacao);
    snprintf(reservaAtual.coordenadorId, sizeof reservaAtual.coordenadorId, "%s", servidor.id);
    snprintf(reservaAtual.passageiro, sizeof reservaAtual.passageiro, "%s", passageiroFinal);
    reservaAtual.relogio = lamportIncrementar();
    reservaAtual.criadoEm = agoraEmSegundos();

    int k = 0;
    while (k < numTrechos && strcmp(trechosParsed[k].servidor, servidor.id) == 0) reservaAtual.locais[reservaAtual.qtdLocais++] = trechosParsed[k++];
    if (k < numTrechos) snprintf(reservaAtual.proximo, sizeof reservaAtual.proximo, "%s", trechosParsed[k].servidor);
    for (int i = 0; i < numTrechos; i++) reservaAtual.itin[reservaAtual.qtdItin++] = trechosParsed[i];

    printf("[2PC] RESERVAR tx=%s passageiro=%s relogio=%d trechos=%d (locais=%d)\n",
           reservaAtual.idTransacao, passageiroFinal, reservaAtual.relogio, numTrechos, reservaAtual.qtdLocais);

    int rc = reservaPreparar(&reservaAtual, ESPERA_MAX_MS);
    if (rc != ARMAZENAMENTO_OK) {
        char motivo[320];
        motivoReserva(motivo, sizeof motivo, rc, &reservaAtual);
        return respostaRecusada(httpDeRc(rc), motivo);
    }

    TrechoRes agrupamento[MAX_TRECHOS_TX];
    int qtdAgrupamento = 0;
    for (int q = 0; q < reservaAtual.qtdLocais; q++) agrupamento[qtdAgrupamento++] = reservaAtual.locais[q];

    if (k < numTrechos) {
        Falha falhaRede;
        if (encaminharPrepare(&reservaAtual, &trechosParsed[k], numTrechos - k, agrupamento, &qtdAgrupamento, &falhaRede) != 0) {
            reservaRemover(reservaAtual.idTransacao);
            if (falhaRede.falhaRede) propagarComando("rollback", reservaAtual.proximo, reservaAtual.idTransacao);
            printf("[2PC] tx=%s ABORTADA: %s\n", reservaAtual.idTransacao, falhaRede.motivo);
            return respostaRecusada(falhaRede.http, falhaRede.motivo);
        }
    }

    injetarFalha("antes_decisao");

    if (reservaDecidir(reservaAtual.idTransacao, "COMITADO", agrupamento, qtdAgrupamento) != ARMAZENAMENTO_OK) {
        reservaRemover(reservaAtual.idTransacao);
        propagarComando("rollback", reservaAtual.proximo, reservaAtual.idTransacao);
        return respostaRecusada(500, "Falha ao gravar a decisao em disco");
    }
    reservaAtivo(reservaAtual.idTransacao, 0);
    printf("[2PC] tx=%s DECIDIDA: COMMIT\n", reservaAtual.idTransacao);

    injetarFalha("depois_decisao");

    int propagacaoPendente = (propagarComando("commit", reservaAtual.proximo, reservaAtual.idTransacao) != 0);

    Buffer buffer; 
    bufferIniciar(&buffer);
    bufferAdicionar(&buffer, "{\"id_transacao\": "); 
    bufferAdicionarStringJSON(&buffer, reservaAtual.idTransacao);
    
    bufferAdicionar(&buffer, ", \"status\": \"CONFIRMADO\", \"passageiro\": "); 
    bufferAdicionarStringJSON(&buffer, passageiroFinal);
    
    bufferAdicionar(&buffer, ", \"relogio_logico\": %d, \"propagacao_pendente\": %s, \"trechos\": ",
           reservaAtual.relogio, propagacaoPendente ? "true" : "false");
           
    assentosJSON(&buffer, agrupamento, qtdAgrupamento);
    bufferAdicionar(&buffer, "}\n");
    return criarRespostaJSON(201, bufferExtrairTexto(&buffer));
}

/*
 * COORDENADOR: atalho de txReservarComo em que o passageiro vem do corpo JSON.
 */
RespostaServidor txReservar(const char *corpo) { 
    return txReservarComo(NULL, corpo); 
}

/*
 * Pede ao próximo servidor que cancele a reserva (PUT /reservas/{id}/cancel).
 * Retorna 0 se concluído (ou se não há próximo), -1 se não foi possível.
 */
static int enviarCancelamento(const char *proximo, const char *idTransacao) {
    if (!proximo[0]) return 0;

    Resposta resp;
    int rc = clienteCancelar(proximo, idTransacao, &resp);
    int entregue = (rc == 0 && resp.codigoHTTP == 200);

    if (!entregue) {
        printf("[2PC] cancel de %s NAO concluido em %s (%s)\n", idTransacao, proximo, rc != 0 ? resp.erro : "resposta != 200");
    }
    respostaLiberar(&resp);
    return entregue ? 0 : -1;
}

/*
 * Cancela a reserva na cadeia: primeiro os servidores à frente, depois apaga a
 * cópia local (só apaga se os da frente confirmaram, para nunca perder o rastro).
 * Retorna 0 se tudo foi cancelado, -1 se algum servidor não respondeu
 * (a reserva fica em CANCELANDO e a recuperação tenta de novo).
 */
static int cancelarCadeia(const Reserva *reserva) {
    if (enviarCancelamento(reserva->proximo, reserva->idTransacao) != 0) return -1;
    
    int rc = reservaApagar(reserva->idTransacao);
    return (rc == ARMAZENAMENTO_OK || rc == ARMAZENAMENTO_NAO_ENCONTRADO) ? 0 : -1;
}

/*
 * PARTICIPANTE: cancela uma reserva já confirmada (PUT /reservas/{id}/cancel).
 * Passa o cancelamento adiante e depois apaga a reserva local.
 * Idempotente: se a reserva não existe, responde 200 ("Ja cancelada aqui").
 * Retorna 503 se não conseguiu concluir adiante (o chamador pode tentar de novo).
 */
RespostaServidor txCancel(const char *idTransacao) {
    printf("[2PC] CANCEL tx=%s\n", idTransacao);

    Reserva reservaAtual;
    Buffer buffer; 
    bufferIniciar(&buffer);
    
    bufferAdicionar(&buffer, "{\"id_transacao\": "); 
    bufferAdicionarStringJSON(&buffer, idTransacao);

    if (reservaBuscar(idTransacao, &reservaAtual) != ARMAZENAMENTO_OK) {
        bufferAdicionar(&buffer, ", \"mensagem\": \"Ja cancelada aqui.\"}\n");
        return criarRespostaJSON(200, bufferExtrairTexto(&buffer));
    }

    if (cancelarCadeia(&reservaAtual) != 0) {
        bufferLiberar(&buffer);
        return criarRespostaErro(503, "Nao consegui concluir o cancelamento adiante; tente de novo");
    }

    bufferAdicionar(&buffer, ", \"mensagem\": \"Reserva CANCELADA.\"}\n");
    return criarRespostaJSON(200, bufferExtrairTexto(&buffer));
}

/*
 * COORDENADOR: cancelamento pedido pelo passageiro (DELETE /reservas/{id}).
 * Só o servidor coordenador e o dono da reserva podem cancelar. Primeiro grava
 * CANCELANDO em disco (a decisão), depois propaga pela cadeia.
 * Retorna 200 {"status": "CANCELADA"} se todos cancelaram, 202 {"status": "CANCELANDO"}
 * se algum servidor está fora do ar (a recuperação conclui depois), 404 se a reserva
 * não é dele, 409 se a reserva ainda está em andamento ou 500 em falha de disco.
 */
RespostaServidor txCancelar(const char *idTransacao, const char *passageiro) {
    Reserva reservaAtual;

    if (!idTransacao || !passageiro || reservaBuscar(idTransacao, &reservaAtual) != ARMAZENAMENTO_OK ||
        strcmp(reservaAtual.coordenadorId, servidor.id) != 0 || strcmp(reservaAtual.passageiro, passageiro) != 0) {
        return criarRespostaErro(404, "Reserva nao encontrada");
    }

    if (strcmp(reservaAtual.estado, "PREPARADO") == 0) {
        return criarRespostaErro(409, "Reserva ainda em andamento; tente de novo em instantes");
    }

    if (strcmp(reservaAtual.estado, "COMITADO") == 0) {
        if (reservaDecidir(idTransacao, "CANCELANDO", NULL, 0) != ARMAZENAMENTO_OK) {
            return criarRespostaErro(500, "Falha ao gravar o cancelamento em disco");
        }
        printf("[2PC] tx=%s CANCELAMENTO decidido (CANCELANDO)\n", idTransacao);
        injetarFalha("depois_cancelando");
    }

    Buffer buffer; 
    bufferIniciar(&buffer);
    bufferAdicionar(&buffer, "{\"id_transacao\": "); 
    bufferAdicionarStringJSON(&buffer, idTransacao);

    if (cancelarCadeia(&reservaAtual) == 0) {
        printf("[2PC] tx=%s CANCELADA\n", idTransacao);
        bufferAdicionar(&buffer, ", \"status\": \"CANCELADA\"}\n");
        return criarRespostaJSON(200, bufferExtrairTexto(&buffer));
    }

    bufferAdicionar(&buffer, ", \"status\": \"CANCELANDO\", \"mensagem\": \"Alguma empresa do trajeto esta fora do ar. "
               "O cancelamento esta registrado e sera concluido automaticamente.\"}\n");
    return criarRespostaJSON(202, bufferExtrairTexto(&buffer));
}

/* Thread de Recuperação (Recovery) */

static pthread_t threadRecuperacao;
static volatile int sistemaRecuperando = 0;   /* 1 enquanto a thread deve continuar rodando */

/*
 * Resolve UMA reserva que ficou PREPARADO por tempo demais (sem decisão).
 *   - Se este servidor é o coordenador: não há decisão de commit gravada, então
 *     aborta (rollback local e adiante).
 *   - Se é participante: pergunta o estado ao servidor anterior da cadeia.
 *       404 -> o anterior não conhece: aborta aqui e adiante
 *       COMITADO -> confirma aqui e adiante
 *       sem resposta ou outro estado -> não faz nada, tenta no próximo ciclo
 */
static void resolverTransacaoPendente(const Reserva *reserva) {
    const char *idTransacao = reserva->idTransacao;

    if (strcmp(reserva->coordenadorId, servidor.id) == 0) {
        printf("[RECUPERACAO] tx=%s: sou o coordenador e nao ha decisao COMMIT -> abortando\n", idTransacao);
        if (reservaRemover(idTransacao) == ARMAZENAMENTO_OK) propagarComando("rollback", reserva->proximo, idTransacao);
        return;
    }

    Resposta resp;
    int rc = clienteStatus(reserva->anterior, idTransacao, &resp);

    if (rc != 0) {
        printf("[RECUPERACAO] tx=%s: %s sem resposta (%s); tento de novo depois\n", idTransacao, reserva->anterior, resp.erro);
        respostaLiberar(&resp);
        return;
    }

    if (resp.codigoHTTP == 404) {
        printf("[RECUPERACAO] tx=%s: %s nao conhece a transacao -> abortando aqui\n", idTransacao, reserva->anterior);
        if (reservaRemover(idTransacao) == ARMAZENAMENTO_OK) propagarComando("rollback", reserva->proximo, idTransacao);
    } else if (resp.codigoHTTP == 200) {
        ValorJSON *jsonResp = jsonAnalisar(resp.corpo);
        const char *estadoRemoto = jsonObterTexto(jsonResp, "status");

        if (estadoRemoto && strcmp(estadoRemoto, "COMITADO") == 0) {
            printf("[RECUPERACAO] tx=%s: %s informa COMITADO -> confirmando aqui\n", idTransacao, reserva->anterior);
            if (reservaDecidir(idTransacao, "COMITADO", NULL, 0) == ARMAZENAMENTO_OK) propagarComando("commit", reserva->proximo, idTransacao);
        }
        jsonLiberar(jsonResp);
    }
    respostaLiberar(&resp);
}

/*
 * Corpo da thread de recuperação. A cada INTERVALO_RECUPERACAO_S segundos:
 *   1. resolve as reservas PREPARADO pendentes (idade >= TIMEOUT_PREPARADO_S);
 *   2. conclui os cancelamentos que este servidor coordena e ficaram em CANCELANDO.
 * Dorme em fatias de 0,1 s para encerrar rápido em txRecuperacaoParar.
 */
static void *cicloRecuperacao(void *arg) {
    (void) arg;

    while (sistemaRecuperando) {
        for (int i = 0; i < INTERVALO_RECUPERACAO_S * 10 && sistemaRecuperando; i++) usleep(100000);
        if (!sistemaRecuperando) break;

        Reserva *listaReservas = NULL; 
        int qtdReservas = 0;
        
        if (reservaPendentes(TIMEOUT_PREPARADO_S, &listaReservas, &qtdReservas) == ARMAZENAMENTO_OK) {
            for (int i = 0; i < qtdReservas && sistemaRecuperando; i++) {
                resolverTransacaoPendente(&listaReservas[i]);
            }
            free(listaReservas);
        }

        listaReservas = NULL; 
        qtdReservas = 0;
        
        if (reservaListarPorEstado("CANCELANDO", &listaReservas, &qtdReservas) == ARMAZENAMENTO_OK) {
            for (int i = 0; i < qtdReservas && sistemaRecuperando; i++) {
                if (strcmp(listaReservas[i].coordenadorId, servidor.id) != 0) continue;
                if (cancelarCadeia(&listaReservas[i]) == 0) {
                    printf("[RECUPERACAO] tx=%s: cancelamento concluido\n", listaReservas[i].idTransacao);
                }
            }
            free(listaReservas);
        }
    }
    return NULL;
}

/*
 * Inicia a thread de recuperação de falhas. Chamar depois de armazenamentoIniciar.
 */
void txRecuperacaoIniciar(void) {
    sistemaRecuperando = 1;
    pthread_create(&threadRecuperacao, NULL, cicloRecuperacao, NULL);
}

/*
 * Para a thread de recuperação e espera ela terminar. Não faz nada se não estava rodando.
 */
void txRecuperacaoParar(void) {
    if (!sistemaRecuperando) return;
    sistemaRecuperando = 0;
    pthread_join(threadRecuperacao, NULL);
}
