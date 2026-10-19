#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "util/headers/util.h"
#include "apiConsorcio.h"

#define API "/api/cliente"
#define TIMEOUT_CURTO_S  10L
#define TIMEOUT_LONGO_S  60L

static char baseURL[256] = "";
static char usuarioId[64] = "", usuarioNome[96] = "", usuarioTipo[16] = "", senhaSessao[80] = "";

int apiOk(const ApiResp *r) {
    return r->rede && r->http >= 200 && r->http < 300;
}

const char *apiMensagem(const ApiResp *r) {
    if (!r->rede) return r->erro;

    const char *m = jsonGetStr(r->json, "motivo");
    if (!m) m = jsonGetStr(r->json, "erro");
    return m ? m : "(sem detalhes)";
}

void apiLiberar(ApiResp *r) {
    jsonLiberar(r->json);
    r->json = NULL;
}

static size_t aoReceber(void *dados, size_t t, size_t n, void *ud) {
    size_t real = t * n;
    return bufAdd((Buf *) ud, "%.*s", (int) real, (const char *) dados) ? real : 0;
}

static ApiResp chamar(const char *metodo, const char *caminho, const char *corpo,
long timeoutS) {
    ApiResp r;
    memset(&r, 0, sizeof r);

    CURL *c = curl_easy_init();
    if (!c) { snprintf(r.erro, sizeof r.erro, "falha no curl_easy_init"); return r; }

    char url[1024];
    snprintf(url, sizeof url, "%s%s", baseURL, caminho);

    Buf b; bufIni(&b);
    struct curl_slist *h = curl_slist_append(NULL, "Accept: application/json");
    if (corpo) h = curl_slist_append(h, "Content-Type: application/json");

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, metodo);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, aoReceber);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeoutS);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    char cred[160] = "";
    if (usuarioId[0]) {
        snprintf(cred, sizeof cred, "%s:%s", usuarioId, senhaSessao);
        curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long) CURLAUTH_BASIC);
        curl_easy_setopt(c, CURLOPT_USERPWD, cred);
    }
    if (corpo) curl_easy_setopt(c, CURLOPT_POSTFIELDS, corpo);

    CURLcode res = curl_easy_perform(c);

    if (res != CURLE_OK) {
        snprintf(r.erro, sizeof r.erro, "%s", curl_easy_strerror(res));
        bufLiberar(&b);
    } else {
        r.rede = 1;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.http);

        char *txt = bufTomar(&b);
        if (txt && *txt) r.json = jsonParse(txt);
        free(txt);
    }

    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    memset(cred, 0, sizeof cred);
    return r;
}

static char *escapar(const char *s) {
    CURL *t = curl_easy_init();
    char *e = t ? curl_easy_escape(t, s, 0) : NULL;
    char *r = strdup(e ? e : "");
    if (e) curl_free(e);
    if (t) curl_easy_cleanup(t);
    return r;
}

void apiIniciar(const char *url) {
    snprintf(baseURL, sizeof baseURL, "%s", url ? url : "");
    curl_global_init(CURL_GLOBAL_ALL);
}

void apiEncerrar(void) {
    apiSair();
    curl_global_cleanup();
}

const char *apiBaseURL(void) { return baseURL; }

ApiResp apiCadastrar(const char *id, const char *nome, const char *tipo, const char *senha) {
    Buf b; bufIni(&b);
    bufAdd(&b, "{\"id\": ");     bufStr(&b, id);
    bufAdd(&b, ", \"nome\": ");  bufStr(&b, nome);
    bufAdd(&b, ", \"tipo\": ");  bufStr(&b, tipo);
    bufAdd(&b, ", \"senha\": "); bufStr(&b, senha);
    bufAdd(&b, "}");

    ApiResp r = chamar("POST", API "/usuarios", b.p, TIMEOUT_CURTO_S);

    if (b.p) memset(b.p, 0, b.n);
    bufLiberar(&b);
    return r;
}

int apiEntrar(const char *id, const char *senha, ApiResp *r) {
    apiSair();
    snprintf(usuarioId, sizeof usuarioId, "%s", id);
    snprintf(senhaSessao, sizeof senhaSessao, "%s", senha);

    *r = chamar("GET", API "/eu", NULL, TIMEOUT_CURTO_S);

    const char *tipo = apiOk(r) ? jsonGetStr(r->json, "tipo") : NULL;
    if (!tipo) { apiSair(); return 0; }

    const char *nome = jsonGetStr(r->json, "nome");
    snprintf(usuarioTipo, sizeof usuarioTipo, "%s", tipo);
    snprintf(usuarioNome, sizeof usuarioNome, "%s", nome ? nome : id);
    return 1;
}

void apiSair(void) {
    memset(senhaSessao, 0, sizeof senhaSessao);
    usuarioId[0] = usuarioNome[0] = usuarioTipo[0] = 0;
}

int         apiLogado(void)      { return usuarioId[0] && usuarioTipo[0]; }
const char *apiUsuarioId(void)   { return usuarioId; }
const char *apiUsuarioNome(void) { return usuarioNome; }
const char *apiUsuarioTipo(void) { return usuarioTipo; }

ApiResp apiSaude(void) {
    return chamar("GET", "/api/consorcio/health", NULL, 3L);
}

ApiResp apiPublicarCarona(const char *origem, const char *destino, const char *horario,
                          double preco, int assentos) {
    Buf b; bufIni(&b);
    bufAdd(&b, "{\"origem\": ");   bufStr(&b, origem);
    bufAdd(&b, ", \"destino\": ");  bufStr(&b, destino);
    bufAdd(&b, ", \"horario\": ");  bufStr(&b, horario);
    bufAdd(&b, ", \"preco\": %.2f, \"assentos\": %d}", preco, assentos);

    ApiResp r = chamar("POST", API "/caronas", b.p, TIMEOUT_CURTO_S);
    bufLiberar(&b);
    return r;
}

ApiResp apiMinhasCaronas(void) {
    return chamar("GET", API "/caronas", NULL, TIMEOUT_CURTO_S);
}

ApiResp apiExcluirCarona(const char *idCarona) {
    char *e = escapar(idCarona), caminho[256];
    snprintf(caminho, sizeof caminho, API "/caronas/%s", e);
    free(e);
    return chamar("DELETE", caminho, NULL, TIMEOUT_CURTO_S);
}

ApiResp apiBuscarItinerarios(const char *origem, const char *destino) {
    char *eo = escapar(origem), *ed = escapar(destino), caminho[512];
    snprintf(caminho, sizeof caminho, API "/itinerarios?origem=%s&destino=%s", eo, ed);
    free(eo); free(ed);
    return chamar("GET", caminho, NULL, TIMEOUT_LONGO_S);
}

ApiResp apiReservar(const JVal *itinerario) {
    const JVal *ts = jsonGet(itinerario, "trechos");

    Buf b; bufIni(&b);
    bufAdd(&b, "{\"passageiro\": "); bufStr(&b, usuarioId);
    bufAdd(&b, ", \"trechos\": [");
    for (int k = 0; k < jsonTam(ts); k++) {
        const JVal *t = jsonIdx(ts, k);
        bufAdd(&b, "%s{\"servidor\": ", k ? ", " : ""); bufStr(&b, jsonGetStr(t, "servidor"));
        bufAdd(&b, ", \"id_carona\": ");                bufStr(&b, jsonGetStr(t, "id_carona"));
        bufAdd(&b, "}");
    }
    bufAdd(&b, "]}");

    ApiResp r = chamar("POST", API "/reservas", b.p, TIMEOUT_LONGO_S);
    bufLiberar(&b);
    return r;
}

ApiResp apiMinhasReservas(void) {
    return chamar("GET", API "/reservas", NULL, TIMEOUT_CURTO_S);
}

ApiResp apiCancelarReserva(const char *idTransacao) {
    char *e = escapar(idTransacao), caminho[300];
    snprintf(caminho, sizeof caminho, API "/reservas/%s", e);
    free(e);
    return chamar("DELETE", caminho, NULL, TIMEOUT_LONGO_S);
}
