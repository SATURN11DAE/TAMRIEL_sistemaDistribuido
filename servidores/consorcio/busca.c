/*
 * busca.c - Implementação do algoritmo de busca de itinerários
 *
 * Como funciona: uma busca em profundidade que parte da origem.
 *   1. Em cada cidade, olha as caronas LOCAIS que partem dela e oferece todas
 *      as faixas possíveis (cidade -> cada parada seguinte da rota).
 *   2. Se a parada é o destino, o caminho vira um itinerário pronto.
 *      Senão, continua a partir da parada, tanto localmente quanto perguntando
 *      aos servidores vizinhos (clienteTrechos).
 *   3. Os vizinhos recebem a lista de servidores já visitados, o que evita ciclos.
 * A busca tem limites (itinerários, consultas e visitados) para não explodir
 * em redes grandes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "headers/configuracoes.h"
#include "headers/persistencia.h"
#include "headers/json.h"
#include "headers/httpCliente.h"
#include "headers/busca.h"

#define MAX_ITINERARIOS 30   /* para de procurar ao juntar este tanto de itinerários */
#define MAX_CONSULTAS   40   /* máximo de requisições a vizinhos por busca */
#define MAX_VISITADOS   16   /* máximo de servidores na lista de visitados */

/* Um trecho de itinerário: uma faixa de UMA carona, de 'origem' até 'destino'.
 * 'preco' é a soma dos trechos da faixa e 'livres' o mínimo de assentos livres nela. */
typedef struct {
    char servidor[MAX_ID];
    char idCarona[TAM_ID_ENTIDADE];
    char origem[TAM_CIDADE];
    char destino[TAM_CIDADE];
    char horario[TAM_HORARIO];
    double preco;
    int livres;
} TrechoBusca;

/* Um itinerário: sequência de trechos (pode passar por vários servidores). */
typedef struct { 
    int quantidade; 
    TrechoBusca trechos[MAX_TRECHOS_TX]; 
} Itinerario;

/* Estado de UMA busca em andamento (alocado no heap por causa do tamanho). */
typedef struct {
    Itinerario *vetorItinerarios;
    int quantidadeItinerarios;
    int consultas;
    char visitados[MAX_VISITADOS][MAX_ID];
    int quantidadeVisitados;
    char destino[TAM_CIDADE];
} EstadoBusca;

/* Funções Auxiliares de Controle de Estado */

/*
 * Retorna 1 se o servidor 'id' já está na lista de visitados desta busca.
 */
static int jaVisitado(const EstadoBusca *busca, const char *id) {
    for (int i = 0; i < busca->quantidadeVisitados; i++) {
        if (strcmp(busca->visitados[i], id) == 0) return 1;
    }
    return 0;
}

/*
 * Marca um servidor como visitado (ignora id vazio, repetido ou lista cheia).
 */
static void visitar(EstadoBusca *busca, const char *id) {
    if (id[0] && !jaVisitado(busca, id) && busca->quantidadeVisitados < MAX_VISITADOS) {
        snprintf(busca->visitados[busca->quantidadeVisitados++], MAX_ID, "%s", id);
    }
}

/*
 * Devolve o k-ésimo trecho do itinerário formado por prefixo + sufixo, sem copiar nada.
 *   - prefixo / tamanhoPrefixo: trechos já escolhidos nesta busca
 *   - sufixo: trechos trazidos por um vizinho (pode ser NULL)
 */
static const TrechoBusca *trechoDe(const TrechoBusca *prefixo, int tamanhoPrefixo, const Itinerario *sufixo, int k) {
    return k < tamanhoPrefixo ? &prefixo[k] : &sufixo->trechos[k - tamanhoPrefixo];
}

/*
 * Retorna 1 se já existe um itinerário idêntico (mesma sequência de
 * servidor/carona/origem/destino; cidades comparadas sem diferenciar maiúsculas).
 */
static int itinerarioJaExiste(const EstadoBusca *busca, const TrechoBusca *prefixo, int tamanhoPrefixo, const Itinerario *sufixo) {
    int total = tamanhoPrefixo + (sufixo ? sufixo->quantidade : 0);

    for (int i = 0; i < busca->quantidadeItinerarios; i++) {
        const Itinerario *existente = &busca->vetorItinerarios[i];
        if (existente->quantidade != total) continue;

        int igual = 1;
        for (int k = 0; k < total && igual; k++) {
            const TrechoBusca *a = &existente->trechos[k];
            const TrechoBusca *b = trechoDe(prefixo, tamanhoPrefixo, sufixo, k);
            igual = strcmp(a->servidor, b->servidor) == 0 &&
                    strcmp(a->idCarona, b->idCarona) == 0 &&
                    strcasecmp(a->origem, b->origem) == 0 &&
                    strcasecmp(a->destino, b->destino) == 0;
        }
        if (igual) return 1;
    }
    return 0;
}

/*
 * Adiciona o itinerário prefixo + sufixo à lista de resultados.
 * Ignora se a lista está cheia, se passa de MAX_TRECHOS_TX trechos, se é vazio
 * ou se for duplicado.
 */
static void adicionarItinerario(EstadoBusca *busca, const TrechoBusca *prefixo, int tamanhoPrefixo, const Itinerario *sufixo) {
    int tamanhoSufixo = sufixo ? sufixo->quantidade : 0;
    
    if (busca->quantidadeItinerarios >= MAX_ITINERARIOS || 
        tamanhoPrefixo + tamanhoSufixo > MAX_TRECHOS_TX || 
        tamanhoPrefixo + tamanhoSufixo == 0) return;

    if (itinerarioJaExiste(busca, prefixo, tamanhoPrefixo, sufixo)) return;

    Itinerario *itinerarioNovo = &busca->vetorItinerarios[busca->quantidadeItinerarios++];
    itinerarioNovo->quantidade = 0;
    
    for (int i = 0; i < tamanhoPrefixo; i++) itinerarioNovo->trechos[itinerarioNovo->quantidade++] = prefixo[i];
    for (int i = 0; i < tamanhoSufixo; i++)  itinerarioNovo->trechos[itinerarioNovo->quantidade++] = sufixo->trechos[i];
}

/*
 * Retorna 1 se a cidade já está no caminho atual (evita voltar a uma cidade já passada).
 */
static int cidadeNoCaminho(char cidades[][TAM_CIDADE], int totalCidades, const char *cidadeAlvo) {
    for (int i = 0; i < totalCidades; i++) {
        if (strcasecmp(cidades[i], cidadeAlvo) == 0) return 1;
    }
    return 0;
}

/*
 * Escreve os servidores visitados separados por vírgula (ex.: "skyrim,cyrodiil").
 *   - saida / tamanho: buffer de destino; o texto é truncado se não couber
 */
static void gerarCsvVisitados(const EstadoBusca *busca, char *saida, size_t tamanho) {
    saida[0] = '\0';
    for (int i = 0; i < busca->quantidadeVisitados; i++) {
        size_t posicaoAtual = strlen(saida);
        snprintf(saida + posicaoAtual, tamanho - posicaoAtual, "%s%s", i ? "," : "", busca->visitados[i]);
    }
}

/*
 * Lê um itinerário do JSON devolvido por um vizinho.
 * Exige 1 a MAX_TRECHOS_TX trechos, cada um com "servidor" e "id_carona".
 * Retorna 1 se válido, 0 se mal formado.
 */
static int lerItinerarioJSON(const ValorJSON *jsonItinerario, Itinerario *saida) {
    const ValorJSON *trechosJSON = jsonObterCampo(jsonItinerario, "trechos");
    int quantidade = jsonTamanhoArray(trechosJSON);
    
    if (quantidade < 1 || quantidade > MAX_TRECHOS_TX) return 0;

    saida->quantidade = 0;
    for (int i = 0; i < quantidade; i++) {
        const ValorJSON *trecho = jsonObterElemento(trechosJSON, i);
        const char *servidorNome = jsonObterTexto(trecho, "servidor");
        const char *idCarona = jsonObterTexto(trecho, "id_carona");
        
        if (!servidorNome || !idCarona) return 0;

        TrechoBusca *destinoTrecho = &saida->trechos[saida->quantidade++];
        memset(destinoTrecho, 0, sizeof *destinoTrecho);
        
        snprintf(destinoTrecho->servidor, sizeof destinoTrecho->servidor, "%s", servidorNome);
        snprintf(destinoTrecho->idCarona, sizeof destinoTrecho->idCarona, "%s", idCarona);
        
        const char *origemTrecho = jsonObterTexto(trecho, "origem");
        snprintf(destinoTrecho->origem, sizeof destinoTrecho->origem, "%s", origemTrecho ? origemTrecho : "");
        
        const char *destinoFinal = jsonObterTexto(trecho, "destino");
        snprintf(destinoTrecho->destino, sizeof destinoTrecho->destino, "%s", destinoFinal ? destinoFinal : "");
        
        const char *horario = jsonObterTexto(trecho, "horario");
        snprintf(destinoTrecho->horario, sizeof destinoTrecho->horario, "%s", horario ? horario : "");
        
        destinoTrecho->preco = jsonObterNumero(trecho, "preco", 0);
        destinoTrecho->livres = jsonObterInteiro(trecho, "assentos_livres", 0);
    }
    return 1;
}

/* Lógicas de Busca */

/*
 * Pergunta aos vizinhos (ainda não visitados e não suspeitos) quais trechos eles
 * têm de 'cidade' até o destino e junta cada resposta ao prefixo atual.
 * Para ao atingir MAX_ITINERARIOS ou MAX_CONSULTAS.
 */
static void buscarRemoto(EstadoBusca *busca, const char *cidade, const TrechoBusca *prefixo, int tamanhoPrefixo) {
    char csv[MAX_VISITADOS * (MAX_ID + 1) + 1];
    gerarCsvVisitados(busca, csv, sizeof csv);

    for (int i = 0; i < servidor.qtdVizinhos; i++) {
        const char *vizinho = servidor.vizinhos[i].id;

        if (busca->quantidadeItinerarios >= MAX_ITINERARIOS || busca->consultas >= MAX_CONSULTAS) return;
        if (jaVisitado(busca, vizinho)) continue;
        if (clienteSuspeito(vizinho)) { 
            printf("[BUSCA]   pulando %s (suspeito de estar fora do ar)\n", vizinho); 
            continue; 
        }

        busca->consultas++;
        Resposta resp;
        int rc = clienteTrechos(vizinho, cidade, busca->destino, csv, &resp);

        if (rc == 0 && resp.codigoHTTP == 200) {
            ValorJSON *jsonRetorno = jsonAnalisar(resp.corpo);
            const ValorJSON *lista = jsonObterCampo(jsonRetorno, "itinerarios");

            for (int k = 0; k < jsonTamanhoArray(lista); k++) {
                Itinerario sufixo;
                if (lerItinerarioJSON(jsonObterElemento(lista, k), &sufixo)) {
                    adicionarItinerario(busca, prefixo, tamanhoPrefixo, &sufixo);
                }
            }
            jsonLiberar(jsonRetorno);
        }
        respostaLiberar(&resp);
    }
}

/*
 * Retorna 1 se a carona já foi usada no prefixo. Uma mesma carona não pode
 * aparecer duas vezes no itinerário (uma faixa por carona).
 */
static int caronaNoPrefixo(const TrechoBusca *prefixo, int tamanhoPrefixo, const char *idCarona) {
    for (int i = 0; i < tamanhoPrefixo; i++) {
        if (strcmp(prefixo[i].idCarona, idCarona) == 0) return 1;
    }
    return 0;
}

/*
 * Para cada carona local que parte de 'origem', oferece TODAS as faixas possíveis
 * (origem -> cada parada seguinte), com preço somado e assentos livres da faixa.
 * Se a parada é o destino, registra o itinerário; senão continua a partir dela
 * (localmente e nos vizinhos). É recursiva.
 *   - cidades / numCidades: caminho já percorrido (evita ciclos)
 *   - prefixo / tamanhoPrefixo: trechos já escolhidos (a função escreve o próximo)
 */
static void buscarLocal(EstadoBusca *busca, const char *origem, char cidades[][TAM_CIDADE], int numCidades, TrechoBusca *prefixo, int tamanhoPrefixo) {
    if (busca->quantidadeItinerarios >= MAX_ITINERARIOS || tamanhoPrefixo >= MAX_TRECHOS_TX) return;

    Carona *listaCaronas = NULL; 
    int totalCaronas = 0;
    
    if (caronaListar(origem, &listaCaronas, &totalCaronas) != ARMAZENAMENTO_OK) return;

    for (int i = 0; i < totalCaronas && busca->quantidadeItinerarios < MAX_ITINERARIOS; i++) {
        const Carona *carona = &listaCaronas[i];
        if (caronaNoPrefixo(prefixo, tamanhoPrefixo, carona->id)) continue;

        int de = caronaIndiceCidade(carona, origem);
        if (de < 0) continue;

        double preco = 0;
        for (int ate = de + 1; ate < carona->qtdCidades && busca->quantidadeItinerarios < MAX_ITINERARIOS; ate++) {
            preco += carona->precos[ate - 1];

            const char *cidadeDestino = carona->cidades[ate];
            if (cidadeNoCaminho(cidades, numCidades, cidadeDestino)) continue;

            int livres = caronaLivresIntervalo(carona->id, de, ate);
            if (livres <= 0) continue;

            TrechoBusca *trecho = &prefixo[tamanhoPrefixo];
            memset(trecho, 0, sizeof *trecho);
            
            snprintf(trecho->servidor, sizeof trecho->servidor, "%s", servidor.id);
            snprintf(trecho->idCarona, sizeof trecho->idCarona, "%s", carona->id);
            snprintf(trecho->origem, sizeof trecho->origem, "%s", carona->cidades[de]);
            snprintf(trecho->destino, sizeof trecho->destino, "%s", cidadeDestino);
            snprintf(trecho->horario, sizeof trecho->horario, "%s", carona->horarios[de]);
            
            trecho->preco = preco;
            trecho->livres = livres;

            if (strcasecmp(cidadeDestino, busca->destino) == 0) {
                adicionarItinerario(busca, prefixo, tamanhoPrefixo + 1, NULL);
                continue;
            }

            snprintf(cidades[numCidades], TAM_CIDADE, "%s", cidadeDestino);
            buscarLocal(busca, cidadeDestino, cidades, numCidades + 1, prefixo, tamanhoPrefixo + 1);
            buscarRemoto(busca, cidadeDestino, prefixo, tamanhoPrefixo + 1);
        }
    }
    free(listaCaronas);
}

/*
 * Monta o JSON final com todos os itinerários encontrados e o preço total de cada um.
 * O "itinerario_id" é "<servidor>-<posição>".
 */
static RespostaServidor montarRespostaJSON(const EstadoBusca *busca) {
    Buffer buffer; 
    bufferIniciar(&buffer);
    
    bufferAdicionar(&buffer, "{\"servidor\": "); 
    bufferAdicionarStringJSON(&buffer, servidor.id);
    bufferAdicionar(&buffer, ", \"total\": %d, \"itinerarios\": [", busca->quantidadeItinerarios);

    for (int i = 0; i < busca->quantidadeItinerarios; i++) {
        const Itinerario *itinerario = &busca->vetorItinerarios[i];
        double precoTotal = 0;
        
        for (int k = 0; k < itinerario->quantidade; k++) {
            precoTotal += itinerario->trechos[k].preco;
        }

        bufferAdicionar(&buffer, "%s{\"itinerario_id\": \"%s-%d\", \"preco_total\": %.2f, \"trechos\": [",
               i ? ", " : "", servidor.id, i + 1, precoTotal);

        for (int k = 0; k < itinerario->quantidade; k++) {
            const TrechoBusca *trecho = &itinerario->trechos[k];
            bufferAdicionar(&buffer, "%s{\"servidor\": ", k ? ", " : "");  
            bufferAdicionarStringJSON(&buffer, trecho->servidor);
            
            bufferAdicionar(&buffer, ", \"id_carona\": ");                  
            bufferAdicionarStringJSON(&buffer, trecho->idCarona);
            
            bufferAdicionar(&buffer, ", \"origem\": ");                     
            bufferAdicionarStringJSON(&buffer, trecho->origem);
            
            bufferAdicionar(&buffer, ", \"destino\": ");                    
            bufferAdicionarStringJSON(&buffer, trecho->destino);
            
            bufferAdicionar(&buffer, ", \"horario\": ");                    
            bufferAdicionarStringJSON(&buffer, trecho->horario);
            
            bufferAdicionar(&buffer, ", \"preco\": %.2f, \"assentos_livres\": %d}", trecho->preco, trecho->livres);
        }
        bufferAdicionar(&buffer, "]}");
    }
    bufferAdicionar(&buffer, "]}\n");
    return criarRespostaJSON(200, bufferExtrairTexto(&buffer));
}

/*
 * Prepara e executa uma busca completa.
 *   - visitadosCsv: servidores já visitados, vindos de outro servidor (pode ser NULL)
 *   - incluirRemotoNaOrigem: 1 também pergunta aos vizinhos a partir da origem
 *     (busca do cliente); 0 só olha as caronas locais primeiro (busca entre servidores)
 * Retorna a resposta JSON (ou erro 500 se faltar memória).
 */
static RespostaServidor executarBusca(const char *origem, const char *destino, const char *visitadosCsv, int incluirRemotoNaOrigem) {
    EstadoBusca *busca = calloc(1, sizeof *busca);
    if (!busca) return criarRespostaErro(500, "Sem memoria");

    busca->vetorItinerarios = calloc(MAX_ITINERARIOS, sizeof *busca->vetorItinerarios);
    if (!busca->vetorItinerarios) { 
        free(busca); 
        return criarRespostaErro(500, "Sem memoria"); 
    }

    snprintf(busca->destino, sizeof busca->destino, "%s", destino);

    if (visitadosCsv && *visitadosCsv) {
        char *copiaStr = strdup(visitadosCsv);
        char *contexto = NULL;
        
        for (char *token = copiaStr ? strtok_r(copiaStr, ",", &contexto) : NULL; token; token = strtok_r(NULL, ",", &contexto)) {
            char cidadeNormalizada[MAX_ID];
            while (*token == ' ') token++;
            configuracoesNormalizar(token, cidadeNormalizada, sizeof cidadeNormalizada);
            visitar(busca, cidadeNormalizada);
        }
        free(copiaStr);
    }
    visitar(busca, servidor.id);

    char cidadesCaminho[MAX_TRECHOS_TX + 2][TAM_CIDADE];
    TrechoBusca prefixoAcumulado[MAX_TRECHOS_TX];
    snprintf(cidadesCaminho[0], TAM_CIDADE, "%s", origem);

    printf("[BUSCA]   %s -> %s (incluirRemotoNaOrigem=%d)\n", origem, destino, incluirRemotoNaOrigem);
    
    buscarLocal(busca, origem, cidadesCaminho, 1, prefixoAcumulado, 0);
    if (incluirRemotoNaOrigem) buscarRemoto(busca, origem, prefixoAcumulado, 0);

    RespostaServidor resposta = montarRespostaJSON(busca);
    printf("[BUSCA]   %d itinerario(s), %d consulta(s) a vizinhos\n", busca->quantidadeItinerarios, busca->consultas);

    free(busca->vetorItinerarios);
    free(busca);
    return resposta;
}

/* API Pública */

/*
 * Busca usada na comunicação entre servidores (GET /trechos).
 * Retorna 400 se origem ou destino faltarem.
 */
RespostaServidor buscaTrechos(const char *origem, const char *destino, const char *visitadosCsv) {
    if (!origem || !*origem || !destino || !*destino) return criarRespostaErro(400, "Informe ?origem=...&destino=...");
    return executarBusca(origem, destino, visitadosCsv, 0);
}

/*
 * Busca de ponta a ponta usada pelos passageiros (GET /itinerarios).
 * Consulta também os vizinhos a partir da origem.
 * Retorna 400 se origem ou destino faltarem.
 */
RespostaServidor buscaItinerarios(const char *origem, const char *destino) {
    if (!origem || !*origem || !destino || !*destino) return criarRespostaErro(400, "Informe ?origem=...&destino=...");
    return executarBusca(origem, destino, NULL, 1);
}
