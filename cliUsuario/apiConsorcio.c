/*
 * Implementação das requisições para a API do Consórcio
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "headers/configuracoes.h"
#include "headers/util.h"
#include "headers/apiConsorcio.h"

#define ENDPOINT_API_CLIENTE "/api/cliente"
#define TIMEOUT_CURTO_S  10L
#define TIMEOUT_LONGO_S  60L

/*
 * Estado da conexão e da sessão atual (só existe um usuário logado por vez).
 * A senha fica na memória durante a sessão porque o HTTP Basic a reenvia em
 * toda requisição; apiSair() a apaga.
 */
static char baseURL[256] = "";
static char usuarioId[64] = "";
static char usuarioNome[96] = "";
static char usuarioTipo[16] = "";
static char senhaSessao[80] = "";

/*
 * Retorna 1 se houve resposta de rede e o código HTTP é 2xx.
 */
int apiOk(const ApiResp *r) {
    return r->rede && r->http >= 200 && r->http < 300;
}

/*
 * Texto para mostrar ao usuário quando algo deu errado: o erro de rede ou o
 * campo "motivo"/"erro" do JSON. O texto pertence à resposta, não libere.
 */
const char *apiMensagem(const ApiResp *r) {
    if (!r->rede) return r->erro;

    const char *motivo = jsonObterTexto(r->json, "motivo");
    if (!motivo) motivo = jsonObterTexto(r->json, "erro");
    
    return motivo ? motivo : "(sem detalhes)";
}

/*
 * Libera o JSON da resposta. Chamar depois de cada requisição.
 */
void apiLiberar(ApiResp *r) {
    jsonLiberar(r->json);
    r->json = NULL;
}

/*
 * Callback do cURL: acumula o corpo da resposta no Buffer apontado por 'ud'.
 * Retorna 0 (aborta) se faltar memória.
 */
static size_t aoReceber(void *dados, size_t tamanho, size_t n, void *ud) {
    size_t real = tamanho * n;
    return bufferAdicionar((Buffer *) ud, "%.*s", (int) real, (const char *) dados) ? real : 0;
}

/*
 * Função base: faz UMA requisição HTTP ao servidor e devolve o resultado.
 *   - metodo / caminho: ex. "GET", "/api/cliente/eu" (baseURL é adicionada)
 *   - corpo: JSON a enviar (NULL se não houver)
 *   - timeoutS: tempo total máximo em segundos
 * Se há usuário logado, envia HTTP Basic com id e senha da sessão.
 * O corpo da resposta já vem analisado em ApiResp.json (libere com apiLiberar).
 */
static ApiResp chamar(const char *metodo, const char *caminho, const char *corpo, long timeoutS) {
    ApiResp respostaAPI;
    memset(&respostaAPI, 0, sizeof respostaAPI);

    CURL *curl = curl_easy_init();
    if (!curl) { 
        snprintf(respostaAPI.erro, sizeof respostaAPI.erro, "falha no curl_easy_init"); 
        return respostaAPI; 
    }

    char url[1024];
    snprintf(url, sizeof url, "%s%s", baseURL, caminho);

    Buffer buffer; 
    bufferIniciar(&buffer);
    
    struct curl_slist *headers = curl_slist_append(NULL, "Accept: application/json");
    if (corpo) headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, metodo);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, aoReceber);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutS);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    char credenciais[160] = "";
    if (usuarioId[0]) {
        snprintf(credenciais, sizeof credenciais, "%s:%s", usuarioId, senhaSessao);
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long) CURLAUTH_BASIC);
        curl_easy_setopt(curl, CURLOPT_USERPWD, credenciais);
    }
    
    if (corpo) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, corpo);

    CURLcode resultadoCurl = curl_easy_perform(curl);

    if (resultadoCurl != CURLE_OK) {
        snprintf(respostaAPI.erro, sizeof respostaAPI.erro, "%s", curl_easy_strerror(resultadoCurl));
        bufferLiberar(&buffer);
    } else {
        respostaAPI.rede = 1;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &respostaAPI.http);

        char *textoResposta = bufferExtrairTexto(&buffer);
        if (textoResposta && *textoResposta) respostaAPI.json = jsonAnalisar(textoResposta);
        free(textoResposta);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    memset(credenciais, 0, sizeof credenciais);
    
    return respostaAPI;
}

/*
 * Codifica um texto para uso em URL (ex.: "São Paulo" -> "S%C3%A3o%20Paulo").
 * Retorna uma cópia alocada (quem chama dá free).
 */
static char *escapar(const char *textoOriginal) {
    CURL *tmp = curl_easy_init();
    char *escapado = tmp ? curl_easy_escape(tmp, textoOriginal, 0) : NULL;
    char *retorno = strdup(escapado ? escapado : "");
    
    if (escapado) curl_free(escapado);
    if (tmp) curl_easy_cleanup(tmp);
    
    return retorno;
}

/*
 * Guarda a URL base do servidor e inicializa o cURL. Chamar uma vez no começo.
 */
void apiIniciar(const char *url) {
    snprintf(baseURL, sizeof baseURL, "%s", url ? url : "");
    curl_global_init(CURL_GLOBAL_ALL);
}

/*
 * Encerra a sessão e libera o cURL. Chamar uma vez no fim.
 */
void apiEncerrar(void) {
    apiSair();
    curl_global_cleanup();
}

/*
 * Retorna a URL base configurada.
 */
const char *apiBaseURL(void) { return baseURL; }

/*
 * POST /usuarios: cadastra um usuário novo. A senha digitada é apagada da
 * memória logo após o envio.
 */
ApiResp apiCadastrar(const char *id, const char *nome, const char *tipo, const char *senha) {
    Buffer buffer; 
    bufferIniciar(&buffer);
    
    bufferAdicionar(&buffer, "{\"id\": ");     
    bufferAdicionarStringJSON(&buffer, id);
    
    bufferAdicionar(&buffer, ", \"nome\": ");  
    bufferAdicionarStringJSON(&buffer, nome);
    
    bufferAdicionar(&buffer, ", \"tipo\": ");  
    bufferAdicionarStringJSON(&buffer, tipo);
    
    bufferAdicionar(&buffer, ", \"senha\": "); 
    bufferAdicionarStringJSON(&buffer, senha);
    
    bufferAdicionar(&buffer, "}");

    ApiResp resposta = chamar("POST", ENDPOINT_API_CLIENTE "/usuarios", buffer.p, TIMEOUT_CURTO_S);

    if (buffer.p) memset(buffer.p, 0, buffer.n);
    bufferLiberar(&buffer);
    return resposta;
}

/*
 * Faz login: guarda as credenciais e confirma com GET /eu.
 *   - resposta: recebe a resposta da requisição (libere com apiLiberar)
 * Retorna 1 se as credenciais são válidas (e a sessão fica aberta), 0 se não.
 */
int apiEntrar(const char *id, const char *senha, ApiResp *resposta) {
    apiSair();
    snprintf(usuarioId, sizeof usuarioId, "%s", id);
    snprintf(senhaSessao, sizeof senhaSessao, "%s", senha);

    *resposta = chamar("GET", ENDPOINT_API_CLIENTE "/eu", NULL, TIMEOUT_CURTO_S);

    const char *tipoUsuario = apiOk(resposta) ? jsonObterTexto(resposta->json, "tipo") : NULL;
    if (!tipoUsuario) { 
        apiSair(); 
        return 0; 
    }

    const char *nomeUsuario = jsonObterTexto(resposta->json, "nome");
    snprintf(usuarioTipo, sizeof usuarioTipo, "%s", tipoUsuario);
    snprintf(usuarioNome, sizeof usuarioNome, "%s", nomeUsuario ? nomeUsuario : id);
    
    return 1;
}

/*
 * Encerra a sessão: apaga a senha da memória e limpa id, nome e tipo.
 */
void apiSair(void) {
    memset(senhaSessao, 0, sizeof senhaSessao);
    usuarioId[0] = usuarioNome[0] = usuarioTipo[0] = '\0';
}

/*
 * Dados do usuário logado (id, nome e tipo). Retornam "" se não houver sessão.
 */
const char *apiUsuarioId(void)   { return usuarioId; }
const char *apiUsuarioNome(void) { return usuarioNome; }
const char *apiUsuarioTipo(void) { return usuarioTipo; }

/*
 * GET /api/consorcio/health: testa se o servidor está no ar (prazo de 3 s).
 */
ApiResp apiSaude(void) {
    return chamar("GET", "/api/consorcio/health", NULL, 3L);
}

/*
 * POST /rotas (motorista): publica UMA carona com várias paradas.
 *   - numCidades: quantidade de cidades (2 a 10)
 *   - horarios / precos / assentos: (numCidades - 1) itens, um por trecho
 */
ApiResp apiPublicarRota(int numCidades, char (*cidades)[TAM_CIDADE], char (*horarios)[TAM_HORARIO],
                        const double *precos, const int *assentos) {
    Buffer buffer; 
    bufferIniciar(&buffer);

    bufferAdicionar(&buffer, "{\"trechos\": [");
    for (int i = 0; i < numCidades - 1; i++) {
        bufferAdicionar(&buffer, "%s{\"origem\": ", i ? ", " : "");
        bufferAdicionarStringJSON(&buffer, cidades[i]);
        bufferAdicionar(&buffer, ", \"destino\": ");
        bufferAdicionarStringJSON(&buffer, cidades[i + 1]);
        bufferAdicionar(&buffer, ", \"horario\": ");
        bufferAdicionarStringJSON(&buffer, horarios[i]);
        bufferAdicionar(&buffer, ", \"preco\": %.2f, \"assentos\": %d}", precos[i], assentos[i]);
    }
    bufferAdicionar(&buffer, "]}");

    ApiResp resposta = chamar("POST", ENDPOINT_API_CLIENTE "/rotas", buffer.p, TIMEOUT_CURTO_S);
    bufferLiberar(&buffer);

    return resposta;
}

/*
 * GET /caronas (motorista): lista as caronas do motorista logado.
 */
ApiResp apiMinhasCaronas(void) {
    return chamar("GET", ENDPOINT_API_CLIENTE "/caronas", NULL, TIMEOUT_CURTO_S);
}

/*
 * DELETE /caronas/{id} (motorista): exclui uma carona sem reservas.
 */
ApiResp apiExcluirCarona(const char *idCarona) {
    char *escapado = escapar(idCarona);
    char caminho[256];
    
    snprintf(caminho, sizeof caminho, ENDPOINT_API_CLIENTE "/caronas/%s", escapado);
    free(escapado);
    
    return chamar("DELETE", caminho, NULL, TIMEOUT_CURTO_S);
}

/*
 * GET /itinerarios (passageiro): busca viagens entre duas cidades na rede inteira.
 * Prazo longo (60 s), pois o servidor consulta os vizinhos.
 */
ApiResp apiBuscarItinerarios(const char *origem, const char *destino) {
    char *escapadoOrigem = escapar(origem);
    char *escapadoDestino = escapar(destino);
    char caminho[512];
    
    snprintf(caminho, sizeof caminho, ENDPOINT_API_CLIENTE "/itinerarios?origem=%s&destino=%s", escapadoOrigem, escapadoDestino);
    
    free(escapadoOrigem); 
    free(escapadoDestino);
    
    return chamar("GET", caminho, NULL, TIMEOUT_LONGO_S);
}

/*
 * POST /reservas (passageiro): reserva um itinerário devolvido pela busca.
 *   - itinerario: o objeto do itinerário escolhido (usa só servidor, carona,
 *     origem e destino de cada trecho)
 * O servidor coordena a confirmação com as outras empresas, por isso o prazo é longo.
 */
ApiResp apiReservar(const ValorJSON *itinerario) {
    const ValorJSON *trechosJSON = jsonObterCampo(itinerario, "trechos");

    Buffer buffer; 
    bufferIniciar(&buffer);
    
    bufferAdicionar(&buffer, "{\"passageiro\": "); 
    bufferAdicionarStringJSON(&buffer, usuarioId);
    
    bufferAdicionar(&buffer, ", \"trechos\": [");
    int quantidade = jsonTamanhoArray(trechosJSON);
    
    for (int k = 0; k < quantidade; k++) {
        const ValorJSON *trechoAtual = jsonObterElemento(trechosJSON, k);
        bufferAdicionar(&buffer, "%s{\"servidor\": ", k ? ", " : ""); 
        bufferAdicionarStringJSON(&buffer, jsonObterTexto(trechoAtual, "servidor"));
        
        bufferAdicionar(&buffer, ", \"id_carona\": ");                
        bufferAdicionarStringJSON(&buffer, jsonObterTexto(trechoAtual, "id_carona"));

        bufferAdicionar(&buffer, ", \"origem\": ");
        bufferAdicionarStringJSON(&buffer, jsonObterTexto(trechoAtual, "origem"));

        bufferAdicionar(&buffer, ", \"destino\": ");
        bufferAdicionarStringJSON(&buffer, jsonObterTexto(trechoAtual, "destino"));
        bufferAdicionar(&buffer, "}");
    }
    bufferAdicionar(&buffer, "]}");

    ApiResp resposta = chamar("POST", ENDPOINT_API_CLIENTE "/reservas", buffer.p, TIMEOUT_LONGO_S);
    bufferLiberar(&buffer);
    
    return resposta;
}

/*
 * GET /reservas (passageiro): lista as reservas do passageiro logado.
 */
ApiResp apiMinhasReservas(void) {
    return chamar("GET", ENDPOINT_API_CLIENTE "/reservas", NULL, TIMEOUT_CURTO_S);
}

/*
 * DELETE /reservas/{id} (passageiro): cancela uma reserva em todas as empresas do trajeto.
 * HTTP 200 = cancelada; 202 = registrada, falta concluir em algum servidor.
 */
ApiResp apiCancelarReserva(const char *idTransacao) {
    char *escapado = escapar(idTransacao);
    char caminho[300];
    
    snprintf(caminho, sizeof caminho, ENDPOINT_API_CLIENTE "/reservas/%s", escapado);
    free(escapado);
    
    return chamar("DELETE", caminho, NULL, TIMEOUT_LONGO_S);
}
