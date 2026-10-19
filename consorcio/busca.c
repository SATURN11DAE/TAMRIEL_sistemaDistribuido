#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util/headers/configuracoes.h"
#include "util/headers/persistencia.h"
#include "util/headers/json.h"
#include "http/headers/httpCliente.h"
#include "headers/busca.h"

#define MAX_ITINERARIOS 30
#define MAX_CONSULTAS   40
#define MAX_VISITADOS   16

typedef struct {
    char servidor[MAX_ID];
    char idCarona[64];
    char origem[96];
    char destino[96];
    char horario[32];
    double preco;
    int livres;
} TB;

typedef struct { int n; TB t[MAX_TRECHOS_TX]; } Itin;

typedef struct {
    Itin *v;
    int n;
    int consultas;
    char visitados[MAX_VISITADOS][MAX_ID];
    int nvis;
    char destino[96];
} Busca;

static int jaVisitado(const Busca *b, const char *id) {
    for (int i = 0; i < b->nvis; i++) if (strcmp(b->visitados[i], id) == 0) return 1;
    return 0;
}

static void visitar(Busca *b, const char *id) {
    if (id[0] && !jaVisitado(b, id) && b->nvis < MAX_VISITADOS) {
        snprintf(b->visitados[b->nvis++], MAX_ID, "%s", id);
    }
}

static void adicionar(Busca *b, const TB *pref, int np, const Itin *suf) {
    int ns = suf ? suf->n : 0;
    if (b->n >= MAX_ITINERARIOS || np + ns > MAX_TRECHOS_TX || np + ns == 0) return;

    Itin *it = &b->v[b->n++];
    it->n = 0;
    for (int i = 0; i < np; i++) it->t[it->n++] = pref[i];
    for (int i = 0; i < ns; i++) it->t[it->n++] = suf->t[i];
}

static int cidadeNoCaminho(char cidades[][96], int n, const char *c) {
    for (int i = 0; i < n; i++) if (strcasecmp(cidades[i], c) == 0) return 1;
    return 0;
}

static void csvVisitados(const Busca *b, char *saida, size_t n) {
    saida[0] = 0;
    for (int i = 0; i < b->nvis; i++) {
        size_t u = strlen(saida);
        snprintf(saida + u, n - u, "%s%s", i ? "," : "", b->visitados[i]);
    }
}

static int lerItin(const JVal *it, Itin *saida) {
    const JVal *ts = jsonGet(it, "trechos");
    int n = jsonTam(ts);
    if (n < 1 || n > MAX_TRECHOS_TX) return 0;

    saida->n = 0;
    for (int i = 0; i < n; i++) {
        const JVal *t = jsonIdx(ts, i);
        const char *srv = jsonGetStr(t, "servidor"), *id = jsonGetStr(t, "id_carona");
        if (!srv || !id) return 0;

        TB *d = &saida->t[saida->n++];
        memset(d, 0, sizeof *d);
        snprintf(d->servidor, sizeof d->servidor, "%s", srv);
        snprintf(d->idCarona, sizeof d->idCarona, "%s", id);
        snprintf(d->origem, sizeof d->origem, "%s", jsonGetStr(t, "origem") ? jsonGetStr(t, "origem") : "");
        snprintf(d->destino, sizeof d->destino, "%s", jsonGetStr(t, "destino") ? jsonGetStr(t, "destino") : "");
        snprintf(d->horario, sizeof d->horario, "%s", jsonGetStr(t, "horario") ? jsonGetStr(t, "horario") : "");
        d->preco = jsonGetNum(t, "preco", 0);
        d->livres = jsonGetInt(t, "assentos_livres", 0);
    }
    return 1;
}

static void buscarRemoto(Busca *b, const char *cidade, const TB *pref, int np) {
    char csv[MAX_VISITADOS * (MAX_ID + 1) + 1];
    csvVisitados(b, csv, sizeof csv);

    for (int i = 0; i < servidor.qtdVizinhos; i++) {
        const char *viz = servidor.vizinhos[i].id;

        if (b->n >= MAX_ITINERARIOS || b->consultas >= MAX_CONSULTAS) return;
        if (jaVisitado(b, viz)) continue;
        if (clienteSuspeito(viz)) { printf("[BUSCA]   pulando %s (suspeito de estar fora do ar)\n", viz); continue; }

        b->consultas++;
        Resposta resp;
        int rc = clienteTrechos(viz, cidade, b->destino, csv, &resp);

        if (rc == 0 && resp.codigoHTTP == 200) {
            JVal *j = jsonParse(resp.corpo);
            const JVal *lista = jsonGet(j, "itinerarios");

            for (int k = 0; k < jsonTam(lista); k++) {
                Itin suf;
                if (lerItin(jsonIdx(lista, k), &suf)) adicionar(b, pref, np, &suf);
            }
            jsonLiberar(j);
        }
        respostaLiberar(&resp);
    }
}

static void buscarLocal(Busca *b, const char *origem, char cidades[][96], int ncid, TB *pref, int np) {
    if (b->n >= MAX_ITINERARIOS || np >= MAX_TRECHOS_TX) return;

    Carona *lista = NULL; int n = 0;
    if (caronaListar(origem, &lista, &n) != ARMAZENAMENTO_OK) return;

    for (int i = 0; i < n && b->n < MAX_ITINERARIOS; i++) {
        const Carona *c = &lista[i];
        if (c->livres <= 0 || cidadeNoCaminho(cidades, ncid, c->destino)) continue;

        TB *t = &pref[np];
        memset(t, 0, sizeof *t);
        snprintf(t->servidor, sizeof t->servidor, "%s", servidor.id);
        snprintf(t->idCarona, sizeof t->idCarona, "%s", c->id);
        snprintf(t->origem, sizeof t->origem, "%s", c->origem);
        snprintf(t->destino, sizeof t->destino, "%s", c->destino);
        snprintf(t->horario, sizeof t->horario, "%s", c->horario);
        t->preco = c->preco;
        t->livres = c->livres;

        if (strcasecmp(c->destino, b->destino) == 0) {
            adicionar(b, pref, np + 1, NULL);
            continue;
        }

        snprintf(cidades[ncid], 96, "%s", c->destino);
        buscarLocal(b, c->destino, cidades, ncid + 1, pref, np + 1);
        buscarRemoto(b, c->destino, pref, np + 1);
    }
    free(lista);
}

static Resp montar(const Busca *b) {
    Buf r; bufIni(&r);
    bufAdd(&r, "{\"servidor\": "); bufStr(&r, servidor.id);
    bufAdd(&r, ", \"total\": %d, \"itinerarios\": [", b->n);

    for (int i = 0; i < b->n; i++) {
        const Itin *it = &b->v[i];
        double total = 0;
        for (int k = 0; k < it->n; k++) total += it->t[k].preco;

        bufAdd(&r, "%s{\"itinerario_id\": \"%s-%d\", \"preco_total\": %.2f, \"trechos\": [",
               i ? ", " : "", servidor.id, i + 1, total);

        for (int k = 0; k < it->n; k++) {
            const TB *t = &it->t[k];
            bufAdd(&r, "%s{\"servidor\": ", k ? ", " : "");  bufStr(&r, t->servidor);
            bufAdd(&r, ", \"id_carona\": ");                  bufStr(&r, t->idCarona);
            bufAdd(&r, ", \"origem\": ");                     bufStr(&r, t->origem);
            bufAdd(&r, ", \"destino\": ");                    bufStr(&r, t->destino);
            bufAdd(&r, ", \"horario\": ");                    bufStr(&r, t->horario);
            bufAdd(&r, ", \"preco\": %.2f, \"assentos_livres\": %d}", t->preco, t->livres);
        }
        bufAdd(&r, "]}");
    }
    bufAdd(&r, "]}\n");
    return respJSON(200, bufTomar(&r));
}

static Resp executar(const char *origem, const char *destino, const char *visitadosCsv, int incluirRemotoNaOrigem) {
    Busca *b = calloc(1, sizeof *b);
    if (!b) return respErro(500, "Sem memoria");

    b->v = calloc(MAX_ITINERARIOS, sizeof *b->v);
    if (!b->v) { free(b); return respErro(500, "Sem memoria"); }

    snprintf(b->destino, sizeof b->destino, "%s", destino);

    if (visitadosCsv && *visitadosCsv) {
        char *copia = strdup(visitadosCsv), *ctx = NULL;
        for (char *tok = copia ? strtok_r(copia, ",", &ctx) : NULL; tok; tok = strtok_r(NULL, ",", &ctx)) {
            char norm[MAX_ID];
            while (*tok == ' ') tok++;
            configuracoesNormalizar(tok, norm, sizeof norm);
            visitar(b, norm);
        }
        free(copia);
    }
    visitar(b, servidor.id);

    char cidades[MAX_TRECHOS_TX + 2][96];
    TB pref[MAX_TRECHOS_TX];
    snprintf(cidades[0], 96, "%s", origem);

    printf("[BUSCA]   %s -> %s (incluirRemotoNaOrigem=%d)\n", origem, destino, incluirRemotoNaOrigem);
    buscarLocal(b, origem, cidades, 1, pref, 0);
    if (incluirRemotoNaOrigem) buscarRemoto(b, origem, pref, 0);

    Resp r = montar(b);
    printf("[BUSCA]   %d itinerario(s), %d consulta(s) a vizinhos\n", b->n, b->consultas);

    free(b->v);
    free(b);
    return r;
}

Resp buscaTrechos(const char *origem, const char *destino, const char *visitadosCsv) {
    if (!origem || !*origem || !destino || !*destino) return respErro(400, "Informe ?origem=...&destino=...");
    return executar(origem, destino, visitadosCsv, 0);
}

Resp buscaItinerarios(const char *origem, const char *destino) {
    if (!origem || !*origem || !destino || !*destino) return respErro(400, "Informe ?origem=...&destino=...");
    return executar(origem, destino, NULL, 1);
}
