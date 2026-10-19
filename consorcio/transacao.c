#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "util/headers/configuracoes.h"
#include "util/headers/lamport.h"
#include "util/headers/persistencia.h"
#include "util/headers/json.h"
#include "http/headers/httpCliente.h"
#include "headers/transacao.h"

static void injetarFalha(const char *ponto) {
    const char *e = getenv("INJETAR_FALHA");
    if (e && strcmp(e, ponto) == 0) {
        printf("[FALHA-INJETADA] processo encerrado em '%s'\n", ponto);
        fflush(stdout);
        _exit(137);
    }
}

typedef struct {
    unsigned int http;
    char motivo[320];
    int falhaRede;
} Falha;

static int contarServidores(const TrechoRes *t, int n) {
    int c = 0;
    for (int i = 0; i < n; i++) if (i == 0 || strcmp(t[i].servidor, t[i - 1].servidor) != 0) c++;
    return c;
}

static void montarCorpoPrepare(Buf *b, const Reserva *r, const TrechoRes *t, int n) {
    bufAdd(b, "{\"id_transacao\": ");    bufStr(b, r->idTransacao);
    bufAdd(b, ", \"coordenador_id\": "); bufStr(b, r->coordenadorId);
    bufAdd(b, ", \"passageiro\": ");     bufStr(b, r->passageiro);
    bufAdd(b, ", \"relogio_logico\": %d, \"trechos_solicitados\": [", r->relogio);
    for (int i = 0; i < n; i++) {
        bufAdd(b, "%s{\"servidor\": ", i ? ", " : ""); bufStr(b, t[i].servidor);
        bufAdd(b, ", \"id_carona\": ");                bufStr(b, t[i].idCarona);
        bufAdd(b, "}");
    }
    bufAdd(b, "]}");
}

static void assentosJSON(Buf *b, const TrechoRes *t, int n) {
    bufAdd(b, "[");
    for (int i = 0; i < n; i++) {
        bufAdd(b, "%s{\"servidor\": ", i ? ", " : ""); bufStr(b, t[i].servidor);
        bufAdd(b, ", \"id_carona\": ");                bufStr(b, t[i].idCarona);
        bufAdd(b, ", \"assento\": %d}", t[i].assento);
    }
    bufAdd(b, "]");
}

static int propagar(const char *tipo, const char *proximo, const char *id) {
    if (!proximo[0]) return 0;

    Resposta resp;
    int rc = (strcmp(tipo, "commit") == 0) ? clienteCommitar(proximo, id, &resp)
                                           : clienteRollback(proximo, id, &resp);
    int entregue = (rc == 0 && resp.codigoHTTP == 200);

    if (!entregue) {
        printf("[2PC] %s de %s NAO entregue a %s (%s); ele resolve na recuperacao\n", tipo, id, proximo,
               rc != 0 ? resp.erro : "resposta != 200");
    }
    respostaLiberar(&resp);
    return entregue ? 0 : -1;
}

static Resp negar(unsigned int http, const char *id, const char *motivo) {
    Buf b; bufIni(&b);
    bufAdd(&b, "{\"id_transacao\": "); bufStr(&b, id);
    bufAdd(&b, ", \"status\": \"NO\", \"servidor\": "); bufStr(&b, servidor.id);
    bufAdd(&b, ", \"motivo\": "); bufStr(&b, motivo);
    bufAdd(&b, "}\n");
    return respJSON(http, bufTomar(&b));
}

static void motivoReserva(char *saida, size_t n, int rc, const Reserva *r) {
    if (rc == ARMAZENAMENTO_NAO_ENCONTRADO) snprintf(saida, n, "carona %s nao existe em %s", r->falha, servidor.id);
    else if (rc == ARMAZENAMENTO_INDISPONIVEL) snprintf(saida, n, "sem assento disponivel na carona %s (%s)", r->falha, servidor.id);
    else snprintf(saida, n, "falha ao gravar a reserva em disco (%s)", servidor.id);
}

static int encaminharPrepare(const Reserva *r, const TrechoRes *resto, int n,
                             TrechoRes *agr, int *nAgr, Falha *f) {
    const char *prox = resto[0].servidor;

    Buf b; bufIni(&b);
    montarCorpoPrepare(&b, r, resto, n);

    long timeout = (long) (TIMEOUT_REQUISICAO + ESPERA_MAX_MS) * contarServidores(resto, n);

    Resposta resp;
    int rc = clientePrepararCorpo(prox, b.p, timeout, &resp);
    bufLiberar(&b);

    memset(f, 0, sizeof *f);

    if (rc != 0) {
        f->http = 502; f->falhaRede = 1;
        snprintf(f->motivo, sizeof f->motivo, "servidor %s indisponivel (%s)", prox, resp.erro);
        respostaLiberar(&resp);
        return -1;
    }

    JVal *j = jsonParse(resp.corpo);

    if (resp.codigoHTTP == 200) {
        const JVal *as = jsonGet(j, "assentos");
        for (int i = 0; i < jsonTam(as) && *nAgr < MAX_TRECHOS_TX; i++) {
            const JVal *a = jsonIdx(as, i);
            TrechoRes *d = &agr[(*nAgr)++];
            memset(d, 0, sizeof *d);
            snprintf(d->servidor, sizeof d->servidor, "%s", jsonGetStr(a, "servidor") ? jsonGetStr(a, "servidor") : "");
            snprintf(d->idCarona, sizeof d->idCarona, "%s", jsonGetStr(a, "id_carona") ? jsonGetStr(a, "id_carona") : "");
            d->assento = jsonGetInt(a, "assento", 0);
        }
        jsonLiberar(j);
        respostaLiberar(&resp);
        return 0;
    }

    const char *m = jsonGetStr(j, "motivo");
    if (!m) m = jsonGetStr(j, "erro");
    f->http = (resp.codigoHTTP == 409 || resp.codigoHTTP == 404) ? (unsigned) resp.codigoHTTP : 502;
    snprintf(f->motivo, sizeof f->motivo, "%s", m ? m : "recusado");
    jsonLiberar(j);
    respostaLiberar(&resp);
    return -1;
}

Resp txPrepare(const char *corpo, const char *remetente) {
    JVal *j = corpo ? jsonParse(corpo) : NULL;
    if (!j) return respErro(400, "Corpo JSON invalido");

    const char *id = jsonGetStr(j, "id_transacao"), *coord = jsonGetStr(j, "coordenador_id");
    const JVal *lista = jsonGet(j, "trechos_solicitados");
    int n = jsonTam(lista);

    if (!id || !idValido(id)) { jsonLiberar(j); return respErro(400, "id_transacao ausente ou invalido"); }
    if (!coord || !*coord) { jsonLiberar(j); return respErro(400, "Corpo sem coordenador_id"); }
    if (n < 1 || n > MAX_TRECHOS_TX) { jsonLiberar(j); return respErro(400, "trechos_solicitados deve ter de 1 a 8 itens"); }

    Reserva r;
    memset(&r, 0, sizeof r);
    snprintf(r.idTransacao, sizeof r.idTransacao, "%s", id);
    snprintf(r.coordenadorId, sizeof r.coordenadorId, "%s", coord);
    snprintf(r.passageiro, sizeof r.passageiro, "%s", jsonGetStr(j, "passageiro") ? jsonGetStr(j, "passageiro") : "");
    snprintf(r.anterior, sizeof r.anterior, "%s", (remetente && *remetente) ? remetente : coord);
    r.relogio = jsonGetInt(j, "relogio_logico", 0);
    r.criadoEm = agoraSegundos();

    TrechoRes todos[MAX_TRECHOS_TX];
    for (int i = 0; i < n; i++) {
        const JVal *t = jsonIdx(lista, i);
        const char *srv = jsonGetStr(t, "servidor"), *idc = jsonGetStr(t, "id_carona");
        if (!srv || !idc || !idValido(idc)) { jsonLiberar(j); return respErro(400, "trecho sem servidor ou com id_carona invalido"); }

        memset(&todos[i], 0, sizeof todos[i]);
        configuracoesNormalizar(srv, todos[i].servidor, sizeof todos[i].servidor);
        snprintf(todos[i].idCarona, sizeof todos[i].idCarona, "%s", idc);
    }
    jsonLiberar(j);
    id = r.idTransacao;

    int k = 0;
    while (k < n && strcmp(todos[k].servidor, servidor.id) == 0) {
        for (int q = 0; q < r.qtdLocais; q++) {
            if (strcmp(r.locais[q].idCarona, todos[k].idCarona) == 0) return respErro(400, "carona repetida na transacao");
        }
        r.locais[r.qtdLocais++] = todos[k++];
    }
    if (r.qtdLocais == 0) return respErro(400, "O primeiro trecho solicitado nao pertence a este servidor");

    for (int q = k; q < n; q++) {
        if (strcmp(todos[q].servidor, servidor.id) == 0) return respErro(400, "Servidor aparece duas vezes na cadeia");
    }
    if (k < n) {
        snprintf(r.proximo, sizeof r.proximo, "%s", todos[k].servidor);
        if (!buscarVizinho(r.proximo)) return respErro(400, "O proximo servidor da cadeia nao faz fronteira com este");
    }

    printf("[2PC] PREPARE tx=%s coordenador=%s relogio=%d trechos_meus=%d proximo=%s\n",
           r.idTransacao, r.coordenadorId, r.relogio, r.qtdLocais, r.proximo[0] ? r.proximo : "-");

    int rc = reservaPreparar(&r, ESPERA_MAX_MS);
    if (rc != ARMAZENAMENTO_OK) {
        char motivo[320];
        motivoReserva(motivo, sizeof motivo, rc, &r);
        return negar(rc == ARMAZENAMENTO_NAO_ENCONTRADO ? 404 : rc == ARMAZENAMENTO_INDISPONIVEL ? 409 : 500, id, motivo);
    }

    TrechoRes agr[MAX_TRECHOS_TX];
    int nAgr = 0;
    for (int q = 0; q < r.qtdLocais && nAgr < MAX_TRECHOS_TX; q++) agr[nAgr++] = r.locais[q];

    if (!r.jaExistia && r.proximo[0]) {
        Falha f;
        if (encaminharPrepare(&r, &todos[k], n - k, agr, &nAgr, &f) != 0) {
            reservaRemover(id);
            if (f.falhaRede) propagar("rollback", r.proximo, id);
            printf("[2PC] PREPARE tx=%s abortado aqui: %s\n", id, f.motivo);
            return negar(f.http, id, f.motivo);
        }
    }

    if (!r.jaExistia) reservaAtivo(id, 0);

    Buf b; bufIni(&b);
    bufAdd(&b, "{\"id_transacao\": "); bufStr(&b, id);
    bufAdd(&b, ", \"status\": \"READY\", \"servidor\": "); bufStr(&b, servidor.id);
    bufAdd(&b, ", \"assentos\": ");
    assentosJSON(&b, agr, nAgr);
    bufAdd(&b, "}\n");
    return respJSON(200, bufTomar(&b));
}

Resp txCommit(const char *id) {
    printf("[2PC] COMMIT tx=%s\n", id);

    Reserva r;
    if (reservaBuscar(id, &r) != ARMAZENAMENTO_OK) {
        return respErro(404, "Transacao desconhecida (nao houve prepare)");
    }

    if (reservaDecidir(id, "COMITADO", NULL, 0) != ARMAZENAMENTO_OK) {
        return respErro(500, "Falha ao gravar a reserva em disco");
    }

    int pendente = (propagar("commit", r.proximo, id) != 0);

    Buf b; bufIni(&b);
    bufAdd(&b, "{\"id_transacao\": "); bufStr(&b, id);
    bufAdd(&b, ", \"mensagem\": \"Transacao COMMITTED com sucesso.\", \"propagacao_pendente\": %s}\n",
           pendente ? "true" : "false");
    return respJSON(200, bufTomar(&b));
}

Resp txRollback(const char *id) {
    printf("[2PC] ROLLBACK tx=%s\n", id);

    Reserva r;
    int tinha = (reservaBuscar(id, &r) == ARMAZENAMENTO_OK);

    int rc = reservaRemover(id);
    if (rc == ARMAZENAMENTO_CONFLITO) return respErro(409, "Transacao ja COMITADA: rollback recusado");
    if (rc == ARMAZENAMENTO_ERRO) return respErro(500, "Falha ao gravar a remocao em disco");

    if (tinha) propagar("rollback", r.proximo, id);

    Buf b; bufIni(&b);
    bufAdd(&b, "{\"id_transacao\": "); bufStr(&b, id);
    bufAdd(&b, ", \"mensagem\": \"Transacao ABORTED com sucesso.\"}\n");
    return respJSON(200, bufTomar(&b));
}

Resp txStatus(const char *id) {
    Reserva r;
    if (reservaBuscar(id, &r) != ARMAZENAMENTO_OK) return respErro(404, "Transacao desconhecida");

    Buf b; bufIni(&b);
    bufAdd(&b, "{\"id_transacao\": "); bufStr(&b, id);
    bufAdd(&b, ", \"status\": ");      bufStr(&b, r.estado);
    bufAdd(&b, ", \"coordenador_id\": "); bufStr(&b, r.coordenadorId);
    bufAdd(&b, "}\n");
    return respJSON(200, bufTomar(&b));
}

static Resp recusado(unsigned int http, const char *motivo) {
    Buf b; bufIni(&b);
    bufAdd(&b, "{\"status\": \"RECUSADO\", \"motivo\": ");
    bufStr(&b, motivo);
    bufAdd(&b, "}\n");
    return respJSON(http, bufTomar(&b));
}

Resp txReservarComo(const char *forcado, const char *corpo) {
    JVal *j = corpo ? jsonParse(corpo) : NULL;
    if (!j) return respErro(400, "Corpo JSON invalido");

    char pass[64] = "";
    if (jsonGetStr(j, "passageiro")) snprintf(pass, sizeof pass, "%s", jsonGetStr(j, "passageiro"));
    if (forcado) snprintf(pass, sizeof pass, "%s", forcado);
    const JVal *lista = jsonGet(j, "trechos");
    int n = jsonTam(lista);

    Usuario u;
    if (!pass[0] || !idValido(pass)) { jsonLiberar(j); return respErro(400, "Informe o passageiro"); }
    if (usuarioBuscar(pass, &u) != ARMAZENAMENTO_OK) { jsonLiberar(j); return respErro(404, "Passageiro nao cadastrado neste servidor"); }
    if (strcmp(u.tipo, "passageiro") != 0) { jsonLiberar(j); return respErro(403, "O usuario informado nao e passageiro"); }
    if (n < 1 || n > MAX_TRECHOS_TX) { jsonLiberar(j); return respErro(400, "trechos deve ter de 1 a 8 itens"); }

    TrechoRes t[MAX_TRECHOS_TX];
    for (int i = 0; i < n; i++) {
        const JVal *it = jsonIdx(lista, i);
        const char *srv = jsonGetStr(it, "servidor"), *idc = jsonGetStr(it, "id_carona");
        if (!srv || !idc || !idValido(idc)) { jsonLiberar(j); return respErro(400, "Trecho sem servidor ou com id_carona invalido"); }

        memset(&t[i], 0, sizeof t[i]);
        configuracoesNormalizar(srv, t[i].servidor, sizeof t[i].servidor);
        snprintf(t[i].idCarona, sizeof t[i].idCarona, "%s", idc);

        if (!configuracoesRegiaoValida(t[i].servidor)) { jsonLiberar(j); return respErro(400, "Servidor desconhecido em um dos trechos"); }
        for (int q = 0; q < i; q++) {
            if (strcmp(t[q].idCarona, t[i].idCarona) == 0 && strcmp(t[q].servidor, t[i].servidor) == 0) {
                jsonLiberar(j); return respErro(400, "Carona repetida no itinerario");
            }
        }
    }
    jsonLiberar(j);

    const char *prev = servidor.id;
    char vistos[MAX_TRECHOS_TX][MAX_ID];
    int nv = 0;
    for (int i = 0; i < n; i++) {
        if (i > 0 && strcmp(t[i].servidor, t[i - 1].servidor) == 0) continue;

        for (int q = 0; q < nv; q++) if (strcmp(vistos[q], t[i].servidor) == 0) return respErro(400, "Servidor repetido de forma nao consecutiva no itinerario");
        snprintf(vistos[nv++], MAX_ID, "%s", t[i].servidor);

        if (strcmp(t[i].servidor, servidor.id) == 0) {
            if (i != 0) return respErro(400, "Os trechos deste servidor devem vir primeiro no itinerario");
        } else if (!configuracoesSaoVizinhas(prev, t[i].servidor)) {
            char m[260];
            snprintf(m, sizeof m, "%s e %s nao fazem fronteira: a cadeia de participantes deve seguir o mapa", prev, t[i].servidor);
            return respErro(400, m);
        }
        prev = t[i].servidor;
    }

    Reserva r;
    memset(&r, 0, sizeof r);
    gerarUUID(r.idTransacao);
    snprintf(r.coordenadorId, sizeof r.coordenadorId, "%s", servidor.id);
    snprintf(r.passageiro, sizeof r.passageiro, "%s", pass);
    r.relogio = lamportTique();
    r.criadoEm = agoraSegundos();

    int k = 0;
    while (k < n && strcmp(t[k].servidor, servidor.id) == 0) r.locais[r.qtdLocais++] = t[k++];
    if (k < n) snprintf(r.proximo, sizeof r.proximo, "%s", t[k].servidor);
    for (int i = 0; i < n; i++) r.itin[r.qtdItin++] = t[i];

    printf("[2PC] RESERVAR tx=%s passageiro=%s relogio=%d trechos=%d (locais=%d)\n",
           r.idTransacao, pass, r.relogio, n, r.qtdLocais);

    int rc = reservaPreparar(&r, ESPERA_MAX_MS);
    if (rc != ARMAZENAMENTO_OK) {
        char motivo[320];
        motivoReserva(motivo, sizeof motivo, rc, &r);
        return recusado(rc == ARMAZENAMENTO_NAO_ENCONTRADO ? 404 : rc == ARMAZENAMENTO_INDISPONIVEL ? 409 : 500, motivo);
    }

    TrechoRes agr[MAX_TRECHOS_TX];
    int nAgr = 0;
    for (int q = 0; q < r.qtdLocais; q++) agr[nAgr++] = r.locais[q];

    if (k < n) {
        Falha f;
        if (encaminharPrepare(&r, &t[k], n - k, agr, &nAgr, &f) != 0) {
            reservaRemover(r.idTransacao);
            if (f.falhaRede) propagar("rollback", r.proximo, r.idTransacao);
            printf("[2PC] tx=%s ABORTADA: %s\n", r.idTransacao, f.motivo);
            return recusado(f.http, f.motivo);
        }
    }

    injetarFalha("antes_decisao");

    if (reservaDecidir(r.idTransacao, "COMITADO", agr, nAgr) != ARMAZENAMENTO_OK) {
        reservaRemover(r.idTransacao);
        propagar("rollback", r.proximo, r.idTransacao);
        return recusado(500, "Falha ao gravar a decisao em disco");
    }
    reservaAtivo(r.idTransacao, 0);
    printf("[2PC] tx=%s DECIDIDA: COMMIT\n", r.idTransacao);

    injetarFalha("depois_decisao");

    int pendente = (propagar("commit", r.proximo, r.idTransacao) != 0);

    Buf b; bufIni(&b);
    bufAdd(&b, "{\"id_transacao\": "); bufStr(&b, r.idTransacao);
    bufAdd(&b, ", \"status\": \"CONFIRMADO\", \"passageiro\": "); bufStr(&b, pass);
    bufAdd(&b, ", \"relogio_logico\": %d, \"propagacao_pendente\": %s, \"trechos\": ",
           r.relogio, pendente ? "true" : "false");
    assentosJSON(&b, agr, nAgr);
    bufAdd(&b, "}\n");
    return respJSON(201, bufTomar(&b));
}

Resp txReservar(const char *corpo) { return txReservarComo(NULL, corpo); }

static int enviarCancel(const char *proximo, const char *id) {
    if (!proximo[0]) return 0;

    Resposta resp;
    int rc = clienteCancelar(proximo, id, &resp);
    int entregue = (rc == 0 && resp.codigoHTTP == 200);

    if (!entregue) {
        printf("[2PC] cancel de %s NAO concluido em %s (%s)\n", id, proximo,
               rc != 0 ? resp.erro : "resposta != 200");
    }
    respostaLiberar(&resp);
    return entregue ? 0 : -1;
}

static int cancelarCadeia(const Reserva *r) {
    if (enviarCancel(r->proximo, r->idTransacao) != 0) return -1;
    int rc = reservaApagar(r->idTransacao);
    return (rc == ARMAZENAMENTO_OK || rc == ARMAZENAMENTO_NAO_ENCONTRADO) ? 0 : -1;
}

Resp txCancel(const char *id) {
    printf("[2PC] CANCEL tx=%s\n", id);

    Reserva r;
    Buf b; bufIni(&b);
    bufAdd(&b, "{\"id_transacao\": "); bufStr(&b, id);

    if (reservaBuscar(id, &r) != ARMAZENAMENTO_OK) {
        bufAdd(&b, ", \"mensagem\": \"Ja cancelada aqui.\"}\n");
        return respJSON(200, bufTomar(&b));
    }

    if (cancelarCadeia(&r) != 0) {
        bufLiberar(&b);
        return respErro(503, "Nao consegui concluir o cancelamento adiante; tente de novo");
    }

    bufAdd(&b, ", \"mensagem\": \"Reserva CANCELADA.\"}\n");
    return respJSON(200, bufTomar(&b));
}

Resp txCancelar(const char *id, const char *passageiro) {
    Reserva r;

    if (!id || !passageiro || reservaBuscar(id, &r) != ARMAZENAMENTO_OK ||
        strcmp(r.coordenadorId, servidor.id) != 0 || strcmp(r.passageiro, passageiro) != 0) {
        return respErro(404, "Reserva nao encontrada");
    }

    if (strcmp(r.estado, "PREPARADO") == 0) {
        return respErro(409, "Reserva ainda em andamento; tente de novo em instantes");
    }

    if (strcmp(r.estado, "COMITADO") == 0) {
        if (reservaDecidir(id, "CANCELANDO", NULL, 0) != ARMAZENAMENTO_OK) {
            return respErro(500, "Falha ao gravar o cancelamento em disco");
        }
        printf("[2PC] tx=%s CANCELAMENTO decidido (CANCELANDO)\n", id);
        injetarFalha("depois_cancelando");
    }

    Buf b; bufIni(&b);
    bufAdd(&b, "{\"id_transacao\": "); bufStr(&b, id);

    if (cancelarCadeia(&r) == 0) {
        printf("[2PC] tx=%s CANCELADA\n", id);
        bufAdd(&b, ", \"status\": \"CANCELADA\"}\n");
        return respJSON(200, bufTomar(&b));
    }

    bufAdd(&b, ", \"status\": \"CANCELANDO\", \"mensagem\": \"Alguma empresa do trajeto esta fora do ar. "
               "O cancelamento esta registrado e sera concluido automaticamente.\"}\n");
    return respJSON(202, bufTomar(&b));
}

static pthread_t thRecuperacao;
static volatile int recuperando = 0;

static void resolver(const Reserva *r) {
    const char *id = r->idTransacao;

    if (strcmp(r->coordenadorId, servidor.id) == 0) {
        printf("[RECUPERACAO] tx=%s: sou o coordenador e nao ha decisao COMMIT -> abortando\n", id);
        if (reservaRemover(id) == ARMAZENAMENTO_OK) propagar("rollback", r->proximo, id);
        return;
    }

    Resposta resp;
    int rc = clienteStatus(r->anterior, id, &resp);

    if (rc != 0) {
        printf("[RECUPERACAO] tx=%s: %s sem resposta (%s); tento de novo depois\n", id, r->anterior, resp.erro);
        respostaLiberar(&resp);
        return;
    }

    if (resp.codigoHTTP == 404) {
        printf("[RECUPERACAO] tx=%s: %s nao conhece a transacao -> abortando aqui\n", id, r->anterior);
        if (reservaRemover(id) == ARMAZENAMENTO_OK) propagar("rollback", r->proximo, id);
    } else if (resp.codigoHTTP == 200) {
        JVal *j = jsonParse(resp.corpo);
        const char *st = jsonGetStr(j, "status");

        if (st && strcmp(st, "COMITADO") == 0) {
            printf("[RECUPERACAO] tx=%s: %s informa COMITADO -> confirmando aqui\n", id, r->anterior);
            if (reservaDecidir(id, "COMITADO", NULL, 0) == ARMAZENAMENTO_OK) propagar("commit", r->proximo, id);
        }
        jsonLiberar(j);
    }
    respostaLiberar(&resp);
}

static void *cicloRecuperacao(void *arg) {
    (void) arg;

    while (recuperando) {
        for (int i = 0; i < INTERVALO_RECUPERACAO_S * 10 && recuperando; i++) usleep(100000);
        if (!recuperando) break;

        Reserva *lista = NULL; int n = 0;
        if (reservaPendentes(TIMEOUT_PREPARADO_S, &lista, &n) == ARMAZENAMENTO_OK) {
            for (int i = 0; i < n && recuperando; i++) resolver(&lista[i]);
            free(lista);
        }

        lista = NULL; n = 0;
        if (reservaListarPorEstado("CANCELANDO", &lista, &n) == ARMAZENAMENTO_OK) {
            for (int i = 0; i < n && recuperando; i++) {
                if (strcmp(lista[i].coordenadorId, servidor.id) != 0) continue;
                if (cancelarCadeia(&lista[i]) == 0) {
                    printf("[RECUPERACAO] tx=%s: cancelamento concluido\n", lista[i].idTransacao);
                }
            }
            free(lista);
        }
    }
    return NULL;
}

void txRecuperacaoIniciar(void) {
    recuperando = 1;
    pthread_create(&thRecuperacao, NULL, cicloRecuperacao, NULL);
}

void txRecuperacaoParar(void) {
    if (!recuperando) return;
    recuperando = 0;
    pthread_join(thRecuperacao, NULL);
}
