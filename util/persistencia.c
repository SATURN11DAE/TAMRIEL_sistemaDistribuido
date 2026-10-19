#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>

#include "headers/persistencia.h"
#include "headers/lamport.h"
#include "headers/util.h"
#include "headers/json.h"

static Usuario *usuarios = NULL; static int nUsu = 0, capUsu = 0;
static Carona  *caronas  = NULL; static int nCar = 0, capCar = 0;
static Reserva *reservas = NULL; static int nRes = 0, capRes = 0;

static pthread_mutex_t trava = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  mudou = PTHREAD_COND_INITIALIZER;

static int garantirImpl(void **ptr, int *cap, int n, size_t tam) {
    if (n < *cap) return 0;
    int nv = *cap ? *cap * 2 : 16;
    void *t = realloc(*ptr, (size_t) nv * tam);
    if (!t) return -1;
    *ptr = t; *cap = nv;
    return 0;
}
#define GARANTIR(ptr, cap, n) garantirImpl((void **) &(ptr), &(cap), (n), sizeof *(ptr))

static void caminhoDir(char *saida, size_t n, const char *sub) {
    snprintf(saida, n, "%s/%s/%s", PASTA_JSON, servidor.id, sub);
}

static void sincronizarDir(const char *dir) {
    int fd = open(dir, O_RDONLY);
    if (fd >= 0) { fsync(fd); close(fd); }
}

static int gravarEntidade(const char *sub, const char *id, const char *conteudo) {
    char dir[200], caminho[320], tmp[330];
    caminhoDir(dir, sizeof dir, sub);
    snprintf(caminho, sizeof caminho, "%s/%s.json", dir, id);
    snprintf(tmp, sizeof tmp, "%s.tmp", caminho);

    FILE *f = fopen(tmp, "w");
    if (!f) { perror("[ARMAZENAMENTO] fopen"); return ARMAZENAMENTO_ERRO; }

    fputs(conteudo, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (rename(tmp, caminho) != 0) { perror("[ARMAZENAMENTO] rename"); return ARMAZENAMENTO_ERRO; }
    sincronizarDir(dir);
    return ARMAZENAMENTO_OK;
}

static void apagarEntidade(const char *sub, const char *id) {
    char dir[200], caminho[320];
    caminhoDir(dir, sizeof dir, sub);
    snprintf(caminho, sizeof caminho, "%s/%s.json", dir, id);
    unlink(caminho);
    sincronizarDir(dir);
}

static char *lerArquivo(const char *caminho) {
    FILE *f = fopen(caminho, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long tam = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (tam < 0) { fclose(f); return NULL; }

    char *txt = malloc((size_t) tam + 1);
    if (!txt) { fclose(f); return NULL; }

    size_t lidos = fread(txt, 1, (size_t) tam, f);
    txt[lidos] = 0;
    fclose(f);
    return txt;
}

static int criarPasta(const char *caminho) {
    if (mkdir(caminho, 0755) != 0 && errno != EEXIST) { perror("[ARMAZENAMENTO] mkdir"); return -1; }
    return 0;
}

static void usuarioJSON(Buf *b, const Usuario *u, int segredo) {
    bufAdd(b, "{\"id\": ");   bufStr(b, u->id);
    bufAdd(b, ", \"nome\": "); bufStr(b, u->nome);
    bufAdd(b, ", \"tipo\": "); bufStr(b, u->tipo);
    if (segredo) {
        bufAdd(b, ", \"salt\": ");  bufStr(b, u->salt);
        bufAdd(b, ", \"hash\": ");  bufStr(b, u->hash);
    }
    bufAdd(b, "}");
}

void caronaJSON(Buf *b, const Carona *c, int incluirLivres) {
    bufAdd(b, "{\"id\": ");          bufStr(b, c->id);
    bufAdd(b, ", \"servidor\": ");   bufStr(b, servidor.id);
    bufAdd(b, ", \"motorista\": ");  bufStr(b, c->motorista);
    bufAdd(b, ", \"origem\": ");     bufStr(b, c->origem);
    bufAdd(b, ", \"destino\": ");    bufStr(b, c->destino);
    bufAdd(b, ", \"horario\": ");    bufStr(b, c->horario);
    bufAdd(b, ", \"preco\": %.2f, \"assentos\": %d", c->preco, c->assentos);
    if (incluirLivres) bufAdd(b, ", \"assentos_livres\": %d", c->livres);
    bufAdd(b, "}");
}

static void trechosJSON(Buf *b, const TrechoRes *t, int n) {
    bufAdd(b, "[");
    for (int i = 0; i < n; i++) {
        bufAdd(b, "%s{\"servidor\": ", i ? ", " : ""); bufStr(b, t[i].servidor);
        bufAdd(b, ", \"id_carona\": ");                bufStr(b, t[i].idCarona);
        bufAdd(b, ", \"assento\": %d}", t[i].assento);
    }
    bufAdd(b, "]");
}

static void reservaJSON(Buf *b, const Reserva *r) {
    bufAdd(b, "{\"id_transacao\": ");      bufStr(b, r->idTransacao);
    bufAdd(b, ", \"estado\": ");           bufStr(b, r->estado);
    bufAdd(b, ", \"coordenador_id\": ");   bufStr(b, r->coordenadorId);
    bufAdd(b, ", \"anterior\": ");         bufStr(b, r->anterior);
    bufAdd(b, ", \"proximo\": ");          bufStr(b, r->proximo);
    bufAdd(b, ", \"passageiro\": ");       bufStr(b, r->passageiro);
    bufAdd(b, ", \"relogio_logico\": %d, \"criado_em\": %ld, \"trechos\": ", r->relogio, r->criadoEm);
    trechosJSON(b, r->locais, r->qtdLocais);
    bufAdd(b, ", \"itinerario\": ");
    trechosJSON(b, r->itin, r->qtdItin);
    bufAdd(b, "}");
}

static void copiar(char *dest, size_t n, const char *orig) {
    snprintf(dest, n, "%s", orig ? orig : "");
}

static int lerTrechos(const JVal *arr, TrechoRes *saida, int max) {
    int n = jsonTam(arr);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        const JVal *t = jsonIdx(arr, i);
        memset(&saida[i], 0, sizeof saida[i]);
        copiar(saida[i].servidor, sizeof saida[i].servidor, jsonGetStr(t, "servidor"));
        copiar(saida[i].idCarona, sizeof saida[i].idCarona, jsonGetStr(t, "id_carona"));
        saida[i].assento = jsonGetInt(t, "assento", 0);
    }
    return n;
}

static void carregarDir(const char *sub, void (*cb)(const JVal *)) {
    char dir[200];
    caminhoDir(dir, sizeof dir, sub);

    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        char caminho[480];
        snprintf(caminho, sizeof caminho, "%s/%.255s", dir, e->d_name);

        if (n > 4 && strcmp(e->d_name + n - 4, ".tmp") == 0) {
            unlink(caminho);
            continue;
        }
        if (n <= 5 || strcmp(e->d_name + n - 5, ".json") != 0) continue;

        char *txt = lerArquivo(caminho);
        JVal *j = txt ? jsonParse(txt) : NULL;
        if (j) cb(j);
        else fprintf(stderr, "[ARMAZENAMENTO] ignorando arquivo ilegivel: %s\n", caminho);

        jsonLiberar(j);
        free(txt);
    }
    closedir(d);
}

static void aoCarregarUsuario(const JVal *j) {
    Usuario u; memset(&u, 0, sizeof u);
    copiar(u.id, sizeof u.id, jsonGetStr(j, "id"));
    copiar(u.nome, sizeof u.nome, jsonGetStr(j, "nome"));
    copiar(u.tipo, sizeof u.tipo, jsonGetStr(j, "tipo"));
    copiar(u.salt, sizeof u.salt, jsonGetStr(j, "salt"));
    copiar(u.hash, sizeof u.hash, jsonGetStr(j, "hash"));
    if (!u.id[0] || GARANTIR(usuarios, capUsu, nUsu)) return;
    usuarios[nUsu++] = u;
}

static void aoCarregarCarona(const JVal *j) {
    Carona c; memset(&c, 0, sizeof c);
    copiar(c.id, sizeof c.id, jsonGetStr(j, "id"));
    copiar(c.motorista, sizeof c.motorista, jsonGetStr(j, "motorista"));
    copiar(c.origem, sizeof c.origem, jsonGetStr(j, "origem"));
    copiar(c.destino, sizeof c.destino, jsonGetStr(j, "destino"));
    copiar(c.horario, sizeof c.horario, jsonGetStr(j, "horario"));
    c.preco = jsonGetNum(j, "preco", 0);
    c.assentos = jsonGetInt(j, "assentos", 0);
    if (!c.id[0] || GARANTIR(caronas, capCar, nCar)) return;
    caronas[nCar++] = c;
}

static void aoCarregarReserva(const JVal *j) {
    Reserva r; memset(&r, 0, sizeof r);
    copiar(r.idTransacao, sizeof r.idTransacao, jsonGetStr(j, "id_transacao"));
    copiar(r.estado, sizeof r.estado, jsonGetStr(j, "estado"));
    copiar(r.coordenadorId, sizeof r.coordenadorId, jsonGetStr(j, "coordenador_id"));
    copiar(r.anterior, sizeof r.anterior, jsonGetStr(j, "anterior"));
    copiar(r.proximo, sizeof r.proximo, jsonGetStr(j, "proximo"));
    copiar(r.passageiro, sizeof r.passageiro, jsonGetStr(j, "passageiro"));
    r.relogio = jsonGetInt(j, "relogio_logico", 0);
    r.criadoEm = (long) jsonGetNum(j, "criado_em", 0);
    r.qtdLocais = lerTrechos(jsonGet(j, "trechos"), r.locais, MAX_TRECHOS_TX);
    r.qtdItin = lerTrechos(jsonGet(j, "itinerario"), r.itin, MAX_TRECHOS_TX);
    r.ativo = 0;
    if (!r.idTransacao[0] || GARANTIR(reservas, capRes, nRes)) return;
    reservas[nRes++] = r;
}

int armazenamentoIniciar(void) {
    char caminho[200];
    snprintf(caminho, sizeof caminho, "%s", PASTA_JSON);
    if (criarPasta(caminho)) return ARMAZENAMENTO_ERRO;

    snprintf(caminho, sizeof caminho, "%s/%s", PASTA_JSON, servidor.id);
    if (criarPasta(caminho)) return ARMAZENAMENTO_ERRO;

    static const char *subs[] = { "usuarios", "caronas", "reservas" };
    for (int i = 0; i < 3; i++) {
        caminhoDir(caminho, sizeof caminho, subs[i]);
        if (criarPasta(caminho)) return ARMAZENAMENTO_ERRO;
    }

    pthread_mutex_lock(&trava);
    carregarDir("usuarios", aoCarregarUsuario);
    carregarDir("caronas", aoCarregarCarona);
    carregarDir("reservas", aoCarregarReserva);

    int maior = 0;
    for (int i = 0; i < nRes; i++) if (reservas[i].relogio > maior) maior = reservas[i].relogio;
    pthread_mutex_unlock(&trava);

    if (maior > 0) lamportAtualizar(maior);

    printf("[ARMAZENAMENTO] json/%s/: %d usuario(s), %d carona(s), %d reserva(s) carregado(s)\n",
           servidor.id, nUsu, nCar, nRes);
    return ARMAZENAMENTO_OK;
}

void armazenamentoEncerrar(void) {
    pthread_mutex_lock(&trava);
    free(usuarios); usuarios = NULL; nUsu = capUsu = 0;
    free(caronas);  caronas = NULL;  nCar = capCar = 0;
    free(reservas); reservas = NULL; nRes = capRes = 0;
    pthread_mutex_unlock(&trava);
}

int armazenamentoQuantidade(void) {
    pthread_mutex_lock(&trava);
    int n = nRes;
    pthread_mutex_unlock(&trava);
    return n;
}

int usuarioSalvar(const Usuario *u) {
    pthread_mutex_lock(&trava);

    for (int i = 0; i < nUsu; i++) {
        if (strcmp(usuarios[i].id, u->id) == 0) { pthread_mutex_unlock(&trava); return ARMAZENAMENTO_EXISTE; }
    }
    if (GARANTIR(usuarios, capUsu, nUsu)) { pthread_mutex_unlock(&trava); return ARMAZENAMENTO_ERRO; }

    Buf b; bufIni(&b);
    usuarioJSON(&b, u, 1);
    bufAdd(&b, "\n");
    int rc = gravarEntidade("usuarios", u->id, b.p ? b.p : "");
    bufLiberar(&b);

    if (rc == ARMAZENAMENTO_OK) usuarios[nUsu++] = *u;
    pthread_mutex_unlock(&trava);
    return rc;
}

int usuarioBuscar(const char *id, Usuario *saida) {
    int rc = ARMAZENAMENTO_NAO_ENCONTRADO;
    pthread_mutex_lock(&trava);
    for (int i = 0; i < nUsu; i++) {
        if (strcmp(usuarios[i].id, id) == 0) { if (saida) *saida = usuarios[i]; rc = ARMAZENAMENTO_OK; break; }
    }
    pthread_mutex_unlock(&trava);
    return rc;
}

char *usuariosListarJSON(void) {
    Buf b; bufIni(&b);
    pthread_mutex_lock(&trava);
    bufAdd(&b, "{\"servidor\": "); bufStr(&b, servidor.id);
    bufAdd(&b, ", \"total\": %d, \"usuarios\": [", nUsu);
    for (int i = 0; i < nUsu; i++) { if (i) bufAdd(&b, ", "); usuarioJSON(&b, &usuarios[i], 0); }
    bufAdd(&b, "]}\n");
    pthread_mutex_unlock(&trava);
    return bufTomar(&b);
}

static int ocupadosDe(const char *idCarona) {
    int total = 0;
    for (int i = 0; i < nRes; i++) {
        for (int k = 0; k < reservas[i].qtdLocais; k++) {
            if (strcmp(reservas[i].locais[k].idCarona, idCarona) == 0) total++;
        }
    }
    return total;
}

static int acharCarona(const char *id) {    
    for (int i = 0; i < nCar; i++) if (strcmp(caronas[i].id, id) == 0) return i;
    return -1;
}

int caronaSalvar(const Carona *c) {
    pthread_mutex_lock(&trava);

    if (acharCarona(c->id) >= 0) { pthread_mutex_unlock(&trava); return ARMAZENAMENTO_EXISTE; }
    if (GARANTIR(caronas, capCar, nCar)) { pthread_mutex_unlock(&trava); return ARMAZENAMENTO_ERRO; }

    Buf b; bufIni(&b);
    caronaJSON(&b, c, 0);
    bufAdd(&b, "\n");
    int rc = gravarEntidade("caronas", c->id, b.p ? b.p : "");
    bufLiberar(&b);

    if (rc == ARMAZENAMENTO_OK) caronas[nCar++] = *c;
    pthread_mutex_unlock(&trava);
    return rc;
}

int caronaBuscar(const char *id, Carona *saida) {
    pthread_mutex_lock(&trava);
    int i = acharCarona(id);
    if (i >= 0 && saida) {
        *saida = caronas[i];
        saida->livres = caronas[i].assentos - ocupadosDe(id);
    }
    pthread_mutex_unlock(&trava);
    return i >= 0 ? ARMAZENAMENTO_OK : ARMAZENAMENTO_NAO_ENCONTRADO;
}

int caronaListar(const char *origem, Carona **lista, int *qtd) {
    pthread_mutex_lock(&trava);

    Carona *v = malloc((size_t) (nCar ? nCar : 1) * sizeof *v);
    if (!v) { pthread_mutex_unlock(&trava); return ARMAZENAMENTO_ERRO; }

    int n = 0;
    for (int i = 0; i < nCar; i++) {
        if (origem && strcasecmp(caronas[i].origem, origem) != 0) continue;
        v[n] = caronas[i];
        v[n].livres = caronas[i].assentos - ocupadosDe(caronas[i].id);
        n++;
    }

    pthread_mutex_unlock(&trava);
    *lista = v; *qtd = n;
    return ARMAZENAMENTO_OK;
}

char *caronasListarJSONDe(const char *motorista) {
    Carona *v = NULL; int n = 0;
    Buf b; bufIni(&b);

    caronaListar(NULL, &v, &n);

    int total = 0;
    for (int i = 0; i < n; i++) if (!motorista || strcmp(v[i].motorista, motorista) == 0) total++;

    bufAdd(&b, "{\"servidor\": "); bufStr(&b, servidor.id);
    bufAdd(&b, ", \"total\": %d, \"caronas\": [", total);

    int k = 0;
    for (int i = 0; i < n; i++) {
        if (motorista && strcmp(v[i].motorista, motorista) != 0) continue;
        if (k++) bufAdd(&b, ", ");
        caronaJSON(&b, &v[i], 1);
    }
    bufAdd(&b, "]}\n");

    free(v);
    return bufTomar(&b);
}

char *caronasListarJSON(void) { return caronasListarJSONDe(NULL); }

int caronaRemover(const char *id, const char *motorista) {
    pthread_mutex_lock(&trava);

    int i = acharCarona(id), rc;
    if (i < 0) {
        rc = ARMAZENAMENTO_NAO_ENCONTRADO;
    } else if (motorista && strcmp(caronas[i].motorista, motorista) != 0) {
        rc = ARMAZENAMENTO_PROIBIDO;
    } else if (ocupadosDe(id) > 0) {
        rc = ARMAZENAMENTO_CONFLITO;
    } else {
        memmove(&caronas[i], &caronas[i + 1], (size_t) (nCar - i - 1) * sizeof *caronas);
        nCar--;
        apagarEntidade("caronas", id);
        rc = ARMAZENAMENTO_OK;
    }

    pthread_mutex_unlock(&trava);
    return rc;
}

static int procurarRes(const char *id) {
    for (int i = 0; i < nRes; i++) if (strcmp(reservas[i].idTransacao, id) == 0) return i;
    return -1;
}

static int gravarReserva(const Reserva *r) {
    Buf b; bufIni(&b);
    reservaJSON(&b, r);
    bufAdd(&b, "\n");
    int rc = gravarEntidade("reservas", r->idTransacao, b.p ? b.p : "");
    bufLiberar(&b);
    return rc;
}

static int maisAntiga(const Reserva *a, const Reserva *b) {
    if (a->relogio != b->relogio) return a->relogio < b->relogio;
    int c = strcmp(a->coordenadorId, b->coordenadorId);
    if (c != 0) return c < 0;
    return strcmp(a->idTransacao, b->idTransacao) < 0;
}

typedef enum { A_OK, A_ESPERAR, A_NAO, A_INEXISTENTE } Aval;

static Aval avaliar(const Reserva *r, const TrechoRes *t, int *assento) {
    int ic = acharCarona(t->idCarona);
    if (ic < 0) return A_INEXISTENTE;

    int total = caronas[ic].assentos;
    char usado[MAX_ASSENTOS + 2];
    memset(usado, 0, sizeof usado);

    int livres = total, preparados = 0, euMaisAntiga = 1;

    for (int i = 0; i < nRes; i++) {
        for (int k = 0; k < reservas[i].qtdLocais; k++) {
            if (strcmp(reservas[i].locais[k].idCarona, t->idCarona) != 0) continue;

            int a = reservas[i].locais[k].assento;
            if (a >= 1 && a <= MAX_ASSENTOS) usado[a] = 1;
            livres--;

            if (strcmp(reservas[i].estado, "PREPARADO") == 0) {
                preparados++;
                if (!maisAntiga(r, &reservas[i])) euMaisAntiga = 0;
            }
        }
    }

    if (livres > 0) {
        for (int a = 1; a <= total && a <= MAX_ASSENTOS; a++) {
            if (!usado[a]) { *assento = a; return A_OK; }
        }
        return A_NAO;
    }

    if (preparados > 0 && euMaisAntiga) return A_ESPERAR;
    return A_NAO;
}

int reservaPreparar(Reserva *r, int esperaMs) {
    struct timespec limite;
    clock_gettime(CLOCK_REALTIME, &limite);
    limite.tv_sec += esperaMs / 1000;
    limite.tv_nsec += (long) (esperaMs % 1000) * 1000000L;
    if (limite.tv_nsec >= 1000000000L) { limite.tv_sec++; limite.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&trava);

    int existente = procurarRes(r->idTransacao);
    if (existente >= 0) {
        *r = reservas[existente];
        r->jaExistia = 1;
        pthread_mutex_unlock(&trava);
        return ARMAZENAMENTO_OK;
    }

    r->jaExistia = 0;
    r->falha[0] = 0;
    int assentos[MAX_TRECHOS_TX];

    for (;;) {
        int esperar = 0, rc = ARMAZENAMENTO_OK;

        for (int k = 0; k < r->qtdLocais; k++) {
            Aval a = avaliar(r, &r->locais[k], &assentos[k]);
            if (a == A_INEXISTENTE) { copiar(r->falha, sizeof r->falha, r->locais[k].idCarona); rc = ARMAZENAMENTO_NAO_ENCONTRADO; break; }
            if (a == A_NAO)         { copiar(r->falha, sizeof r->falha, r->locais[k].idCarona); rc = ARMAZENAMENTO_INDISPONIVEL; break; }
            if (a == A_ESPERAR)     { copiar(r->falha, sizeof r->falha, r->locais[k].idCarona); esperar = 1; }
        }

        if (rc != ARMAZENAMENTO_OK) { pthread_mutex_unlock(&trava); return rc; }
        if (!esperar) break;

        if (pthread_cond_timedwait(&mudou, &trava, &limite) == ETIMEDOUT) {
            pthread_mutex_unlock(&trava);
            return ARMAZENAMENTO_INDISPONIVEL;
        }
    }

    for (int k = 0; k < r->qtdLocais; k++) r->locais[k].assento = assentos[k];
    copiar(r->estado, sizeof r->estado, "PREPARADO");
    r->ativo = 1;

    if (GARANTIR(reservas, capRes, nRes)) { pthread_mutex_unlock(&trava); return ARMAZENAMENTO_ERRO; }
    reservas[nRes++] = *r;

    int rc = gravarReserva(r);
    if (rc != ARMAZENAMENTO_OK) nRes--;

    pthread_mutex_unlock(&trava);
    return rc;
}

int reservaBuscar(const char *id, Reserva *saida) {
    pthread_mutex_lock(&trava);
    int i = procurarRes(id);
    if (i >= 0 && saida) *saida = reservas[i];
    pthread_mutex_unlock(&trava);
    return i >= 0 ? ARMAZENAMENTO_OK : ARMAZENAMENTO_NAO_ENCONTRADO;
}

int reservaDecidir(const char *id, const char *estado, const TrechoRes *itin, int qtdItin) {
    pthread_mutex_lock(&trava);

    int i = procurarRes(id), rc;
    if (i < 0) {
        rc = ARMAZENAMENTO_NAO_ENCONTRADO;
    } else {
        copiar(reservas[i].estado, sizeof reservas[i].estado, estado);
        if (itin) {
            if (qtdItin > MAX_TRECHOS_TX) qtdItin = MAX_TRECHOS_TX;
            memcpy(reservas[i].itin, itin, (size_t) qtdItin * sizeof *itin);
            reservas[i].qtdItin = qtdItin;
        }
        rc = gravarReserva(&reservas[i]);
        pthread_cond_broadcast(&mudou);
    }

    pthread_mutex_unlock(&trava);
    return rc;
}

int reservaRemover(const char *id) {
    pthread_mutex_lock(&trava);

    int i = procurarRes(id), rc;
    if (i < 0) {
        rc = ARMAZENAMENTO_NAO_ENCONTRADO;
    } else if (strcmp(reservas[i].estado, "COMITADO") == 0 || strcmp(reservas[i].estado, "CANCELANDO") == 0) {
        rc = ARMAZENAMENTO_CONFLITO;
    } else {
        memmove(&reservas[i], &reservas[i + 1], (size_t) (nRes - i - 1) * sizeof *reservas);
        nRes--;
        apagarEntidade("reservas", id);
        rc = ARMAZENAMENTO_OK;
        pthread_cond_broadcast(&mudou);
    }

    pthread_mutex_unlock(&trava);
    return rc;
}

int reservaApagar(const char *id) {
    pthread_mutex_lock(&trava);

    int i = procurarRes(id), rc;
    if (i < 0) {
        rc = ARMAZENAMENTO_NAO_ENCONTRADO;
    } else {
        memmove(&reservas[i], &reservas[i + 1], (size_t) (nRes - i - 1) * sizeof *reservas);
        nRes--;
        apagarEntidade("reservas", id);
        rc = ARMAZENAMENTO_OK;
        pthread_cond_broadcast(&mudou);
    }

    pthread_mutex_unlock(&trava);
    return rc;
}

int reservaListarPorEstado(const char *estado, Reserva **lista, int *qtd) {
    pthread_mutex_lock(&trava);

    Reserva *v = malloc((size_t) (nRes ? nRes : 1) * sizeof *v);
    if (!v) { pthread_mutex_unlock(&trava); return ARMAZENAMENTO_ERRO; }

    int n = 0;
    for (int i = 0; i < nRes; i++) if (strcmp(reservas[i].estado, estado) == 0) v[n++] = reservas[i];

    pthread_mutex_unlock(&trava);
    *lista = v; *qtd = n;
    return ARMAZENAMENTO_OK;
}

void reservaAtivo(const char *id, int ativo) {
    pthread_mutex_lock(&trava);
    int i = procurarRes(id);
    if (i >= 0) reservas[i].ativo = ativo;
    pthread_mutex_unlock(&trava);
}

int reservaPendentes(int idadeMinS, Reserva **lista, int *qtd) {
    pthread_mutex_lock(&trava);

    Reserva *v = malloc((size_t) (nRes ? nRes : 1) * sizeof *v);
    if (!v) { pthread_mutex_unlock(&trava); return ARMAZENAMENTO_ERRO; }

    long agora = agoraSegundos();
    int n = 0;
    for (int i = 0; i < nRes; i++) {
        if (strcmp(reservas[i].estado, "PREPARADO") != 0 || reservas[i].ativo) continue;
        if (agora - reservas[i].criadoEm < idadeMinS) continue;
        v[n++] = reservas[i];
    }

    pthread_mutex_unlock(&trava);
    *lista = v; *qtd = n;
    return ARMAZENAMENTO_OK;
}

char *reservasListarJSON(const char *passageiro, int soCoordenador) {
    Buf b; bufIni(&b);
    pthread_mutex_lock(&trava);

    bufAdd(&b, "{\"servidor\": "); bufStr(&b, servidor.id);
    bufAdd(&b, ", \"reservas\": [");

    int n = 0;
    for (int i = 0; i < nRes; i++) {
        if (soCoordenador && strcmp(reservas[i].coordenadorId, servidor.id) != 0) continue;
        if (passageiro && strcmp(reservas[i].passageiro, passageiro) != 0) continue;
        if (n++) bufAdd(&b, ", ");
        reservaJSON(&b, &reservas[i]);
    }
    bufAdd(&b, "], \"total\": %d}\n", n);

    pthread_mutex_unlock(&trava);
    return bufTomar(&b);
}

char *armazenamentoListarJSON(void) {
    return reservasListarJSON(NULL, 0);
}
