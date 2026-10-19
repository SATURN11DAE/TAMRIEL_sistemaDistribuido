#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

#include "util/headers/configuracoes.h"
#include "util/headers/json.h"
#include "cliUsuario/apiConsorcio.h"

static int lerLinha(const char *prompt, char *buf, size_t n) {
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, (int) n, stdin)) return 0;

    if (strchr(buf, '\n') == NULL) {
        int c;
        while ((c = fgetc(stdin)) != '\n' && c != EOF);
    }

    buf[strcspn(buf, "\r\n")] = 0;
    return 1;
}

static int lerSenha(const char *prompt, char *buf, size_t n) {
    struct termios antigo, novo;
    int tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &antigo) == 0;

    printf("%s", prompt);
    fflush(stdout);
    if (tty) {
        novo = antigo;
        novo.c_lflag &= ~(tcflag_t) ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &novo);
    }

    char *ok = fgets(buf, (int) n, stdin);

    if (tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &antigo);
        printf("\n");
    }

    if (ok && strchr(buf, '\n') == NULL) {
        int c;
        while ((c = fgetc(stdin)) != '\n' && c != EOF);
    }

    if (!ok) return 0;
    buf[strcspn(buf, "\r\n")] = 0;
    return 1;
}

static const char *txt(const char *s) { return s ? s : ""; }

static void semResposta(const ApiResp *r) {
    printf("\n!! Sem resposta do servidor %s: %s\n", apiBaseURL(), r->erro);
}

static void mostrarErro(const ApiResp *r) {
    printf("\n!! Erro %ld: %s\n", r->http, apiMensagem(r));
}

static void cadastrar(void) {
    char id[64], nome[96], op[8], senha[80], senha2[80];
    printf("\n--- Cadastro (neste servidor) ---\n");
    if (!lerLinha("id (letras, numeros, - _ .): ", id, sizeof id)) return;
    if (!lerLinha("nome: ", nome, sizeof nome)) return;
    if (!lerLinha("voce e [1] motorista  [2] passageiro: ", op, sizeof op)) return;
    if (!lerSenha("senha (4 a 72 caracteres): ", senha, sizeof senha)) return;
    if (!lerSenha("repita a senha: ", senha2, sizeof senha2)) return;

    if (strcmp(senha, senha2) != 0) { printf("\n!! As senhas nao conferem.\n");
        memset(senha, 0, sizeof senha); return;
    }

    const char *tipo = atoi(op) == 1 ? "motorista" : atoi(op) == 2 ? "passageiro" : NULL;
    if (!tipo) { printf("Opcao invalida.\n"); return; }

    ApiResp r = apiCadastrar(id, nome, tipo, senha);
    memset(senha, 0, sizeof senha); memset(senha2, 0, sizeof senha2);

    if (!r.rede) semResposta(&r);
    else if (r.http == 201) printf("\nCadastrado como %s. Agora escolha 'Entrar'.\n", tipo);
    else mostrarErro(&r);

    apiLiberar(&r);
}

static int entrar(void) {
    char id[64], senha[80];
    if (!lerLinha("\nSeu id: ", id, sizeof id)) return 0;
    if (!lerSenha("Senha: ", senha, sizeof senha)) return 0;

    ApiResp r;
    int ok = apiEntrar(id, senha, &r);
    memset(senha, 0, sizeof senha);

    if (ok) {
        printf("\nBem-vindo(a), %s!\n", apiUsuarioNome());
    } else if (!r.rede) {
        semResposta(&r);
    } else if (r.http == 401) {
        printf("\n!! Id ou senha incorretos.\n");
    } else {
        mostrarErro(&r);
    }

    apiLiberar(&r);
    return ok;
}

static void publicarCarona(void) {
    char o[96], d[96], h[32], p[32], a[16];
    printf("\n--- Publicar carona (um trecho origem -> destino) ---\n");
    if (!lerLinha("origem: ", o, sizeof o) || !lerLinha("destino: ", d, sizeof d)) return;
    if (!lerLinha("horario (texto livre, ex.: 08:30): ", h, sizeof h)) return;
    if (!lerLinha("preco: ", p, sizeof p) || !lerLinha("assentos: ", a, sizeof a)) return;

    ApiResp r = apiPublicarCarona(o, d, h, atof(p), atoi(a));

    if (!r.rede) semResposta(&r);
    else if (r.http == 201) {
        printf("\nCarona publicada! id: %s  (%d assentos)\n",
               jsonGetStr(r.json, "id") ? jsonGetStr(r.json, "id") : "?",
               jsonGetInt(r.json, "assentos", 0));
    } else mostrarErro(&r);

    apiLiberar(&r);
}

static void minhasCaronas(void) {
    ApiResp r = apiMinhasCaronas();
    if (!r.rede) { semResposta(&r); apiLiberar(&r); return; }
    if (r.http != 200) { mostrarErro(&r); apiLiberar(&r); return; }

    const JVal *lista = jsonGet(r.json, "caronas");
    int n = 0;

    printf("\n--- Minhas caronas ---\n");
    for (int i = 0; i < jsonTam(lista); i++) {
        const JVal *c = jsonIdx(lista, i);
        const char *mot = jsonGetStr(c, "motorista");
        if (!mot || strcmp(mot, apiUsuarioId()) != 0) continue;

        printf(" %s\n    %s -> %s  | %s | R$ %.2f | %d de %d assentos livres\n",
               txt(jsonGetStr(c, "id")), txt(jsonGetStr(c, "origem")),
               txt(jsonGetStr(c, "destino")),
               txt(jsonGetStr(c, "horario")),
               jsonGetNum(c, "preco", 0), jsonGetInt(c, "assentos_livres", 0),
               jsonGetInt(c, "assentos", 0));
        n++;
    }
    if (!n) printf(" (nenhuma carona publicada)\n");

    apiLiberar(&r);
}

static void excluirCarona(void) {
    ApiResp r = apiMinhasCaronas();
    if (!r.rede) { semResposta(&r); apiLiberar(&r); return; }
    if (r.http != 200) { mostrarErro(&r); apiLiberar(&r); return; }

    const JVal *lista = jsonGet(r.json, "caronas");
    int n = jsonTam(lista);

    if (n == 0) {
        printf("\nVoce nao tem caronas publicadas.\n");
    } else {
        printf("\n--- Excluir carona ---\n");
        for (int i = 0; i < n; i++) {
            const JVal *c = jsonIdx(lista, i);
            printf(" %d) %s -> %s | %s | %d de %d assentos livres\n", i + 1,
                   txt(jsonGetStr(c, "origem")), txt(jsonGetStr(c, "destino")),
                   txt(jsonGetStr(c, "horario")),
                   jsonGetInt(c, "assentos_livres", 0), jsonGetInt(c, "assentos", 0));
        }

        char sel[16], conf[8];
        if (lerLinha("\nQual excluir? (0 = voltar): ", sel, sizeof sel)) {
            int op = atoi(sel);
            if (op >= 1 && op <= n) {
                char id[64];
                snprintf(id, sizeof id, "%s", txt(jsonGetStr(jsonIdx(lista, op - 1), "id")));

                if (lerLinha("Confirma a exclusao? [s/N]: ", conf, sizeof conf) &&
                (conf[0] == 's' || conf[0] == 'S')) {
                    ApiResp d = apiExcluirCarona(id);

                    if (!d.rede) semResposta(&d);
                    else if (d.http == 200) printf("\nCarona excluida.\n");
                    else mostrarErro(&d);

                    apiLiberar(&d);
                }
            }
        }
    }

    apiLiberar(&r);
}

static void menuMotorista(void) {
    char op[16];
    for (;;) {
        printf("\n=========== Motorista: %s ===========\n", apiUsuarioId());
        printf(" 1) Publicar carona\n 2) Minhas caronas\n 3) Excluir carona\n 0) Sair da conta\n");
        if (!lerLinha("> ", op, sizeof op)) return;

        switch (atoi(op)) {
            case 1: publicarCarona(); break;
            case 2: minhasCaronas();  break;
            case 3: excluirCarona();  break;
            case 0: if (op[0] == '0') return;
            default: printf("Opcao invalida.\n");
        }
    }
}

static void mostrarItinerario(int numero, const JVal *it) {
    const JVal *ts = jsonGet(it, "trechos");
    printf("\n %d) R$ %.2f  (%d trecho%s)\n", numero, jsonGetNum(it, "preco_total", 0),
           jsonTam(ts), jsonTam(ts) == 1 ? "" : "s");

    for (int k = 0; k < jsonTam(ts); k++) {
        const JVal *t = jsonIdx(ts, k);
        printf("      [%s] %s -> %s  %s  R$ %.2f  (%d livres)\n",
               txt(jsonGetStr(t, "servidor")), txt(jsonGetStr(t, "origem")),
               txt(jsonGetStr(t, "destino")),
               txt(jsonGetStr(t, "horario")),
               jsonGetNum(t, "preco", 0), jsonGetInt(t, "assentos_livres", 0));
    }
}

static const JVal *trechoDaCarona(const JVal *it, const char *idCarona) {
    const JVal *ts = jsonGet(it, "trechos");
    for (int k = 0; k < jsonTam(ts); k++) {
        const char *id = jsonGetStr(jsonIdx(ts, k), "id_carona");
        if (id && idCarona && strcmp(id, idCarona) == 0) return jsonIdx(ts, k);
    }
    return NULL;
}

static void reservar(const JVal *it) {
    printf("\nReservando (o servidor coordena a confirmacao com as outras empresas)...\n");

    ApiResp r = apiReservar(it);

    if (!r.rede) {
        semResposta(&r);
        printf("   O resultado e DESCONHECIDO: a reserva pode ter sido concluida ou nao.\n"
               "   Confira em 'Minhas reservas' antes de tentar de novo.\n");
        apiLiberar(&r);
        return;
    }

    if (r.http == 201) {
        printf("\n*** VIAGEM CONFIRMADA ***\n id da reserva: %s\n",
            txt(jsonGetStr(r.json, "id_transacao")));
        const JVal *as = jsonGet(r.json, "trechos");
        for (int i = 0; i < jsonTam(as); i++) {
            const JVal *a = jsonIdx(as, i);
            const JVal *t = trechoDaCarona(it, jsonGetStr(a, "id_carona"));
            printf("   [%s] %s -> %s   assento %d\n", txt(jsonGetStr(a, "servidor")),
                   t ? txt(jsonGetStr(t, "origem")) : "?", t ? txt(jsonGetStr(t, "destino")) : "?",
                   jsonGetInt(a, "assento", 0));
        }
        printf(" total: R$ %.2f\n", jsonGetNum(it, "preco_total", 0));

        const JVal *pend = jsonGet(r.json, "propagacao_pendente");
        if (pend && pend->tipo == J_BOOL && pend->booleano) {
            printf(" (a reserva ja esta garantida; alguma empresa vai registrar a confirmacao em segundo plano)\n");
        }
    } else {
        printf("\n!! Reserva NAO realizada (HTTP %ld): %s\n", r.http, apiMensagem(&r));
        if (r.http == 409) printf("   Algum assento foi ocupado por outro passageiro. Nada foi reservado; busque de novo.\n");
        else if (r.http == 502 || r.http == 503 || r.http == 504)
            printf("   Alguma empresa do trajeto nao respondeu. Nada foi reservado; tente de novo ou escolha outro itinerario.\n");
    }

    apiLiberar(&r);
}

static void buscarEReservar(void) {
    char o[96], d[96], esc[16];
    printf("\n--- Buscar viagem ---\n");
    if (!lerLinha("origem: ", o, sizeof o) || !lerLinha("destino: ", d, sizeof d)) return;

    printf("\nBuscando nas empresas do consorcio (pode levar alguns segundos)...\n");

    ApiResp r = apiBuscarItinerarios(o, d);
    if (!r.rede) { semResposta(&r); apiLiberar(&r); return; }
    if (r.http != 200) { mostrarErro(&r); apiLiberar(&r); return; }

    const JVal *lista = jsonGet(r.json, "itinerarios");
    int n = jsonTam(lista);

    if (n == 0) {
        printf("\nNenhum itinerario com assentos livres de '%s' ate '%s'.\n", o, d);
    } else {
        for (int i = 0; i < n; i++) mostrarItinerario(i + 1, jsonIdx(lista, i));

        if (lerLinha("\nReservar qual itinerario? (0 = voltar): ", esc, sizeof esc)) {
            int op = atoi(esc);
            if (op >= 1 && op <= n) reservar(jsonIdx(lista, op - 1));
        }
    }

    apiLiberar(&r);
}

static void reservasInterativo(int cancelar) {
    ApiResp r = apiMinhasReservas();
    if (!r.rede) { semResposta(&r); apiLiberar(&r); return; }
    if (r.http != 200) { mostrarErro(&r); apiLiberar(&r); return; }

    const JVal *lista = jsonGet(r.json, "reservas");
    int n = jsonTam(lista);

    printf("\n--- %s ---\n", cancelar ? "Cancelar reserva" : "Minhas reservas");
    for (int i = 0; i < n; i++) {
        const JVal *x = jsonIdx(lista, i);
        const char *estado = jsonGetStr(x, "estado");
        const char *rotulo = !estado ? "?" : strcmp(estado, "COMITADO") == 0 ? "CONFIRMADA"
                                           : strcmp(estado, "PREPARADO") == 0 ? "EM ANDAMENTO"
                                           : strcmp(estado, "CANCELANDO") == 0 ? "CANCELANDO" : estado;

        const JVal *ts = jsonTam(jsonGet(x, "itinerario")) ? jsonGet(x, "itinerario") : jsonGet(x, "trechos");
        if (cancelar) printf("\n %d) [%s] %s\n", i + 1, rotulo, txt(jsonGetStr(x, "id_transacao")));
        else          printf("\n [%s] %s\n", rotulo, txt(jsonGetStr(x, "id_transacao")));

        for (int k = 0; k < jsonTam(ts); k++) {
            const JVal *t = jsonIdx(ts, k);
            int a = jsonGetInt(t, "assento", 0);
            printf("      %s : %s", txt(jsonGetStr(t, "servidor")), txt(jsonGetStr(t, "id_carona")));
            if (a > 0) printf("   assento %d", a);
            printf("\n");
        }
    }
    if (!n) printf(" (nenhuma reserva)\n");

    if (cancelar && n > 0) {
        char sel[16], conf[8];
        if (lerLinha("\nQual cancelar? (0 = voltar): ", sel, sizeof sel)) {
            int op = atoi(sel);
            if (op >= 1 && op <= n) {
                char id[128];
                snprintf(id, sizeof id, "%s", txt(jsonGetStr(jsonIdx(lista, op - 1), "id_transacao")));

                if (lerLinha("Confirma o cancelamento? [s/N]: ", conf, sizeof conf) && (conf[0] == 's' || conf[0] == 'S')) {
                    printf("\nCancelando (o servidor avisa as outras empresas do trajeto)...\n");

                    ApiResp d = apiCancelarReserva(id);
                    if (!d.rede) {
                        semResposta(&d);
                        printf("   O resultado e DESCONHECIDO: confira em 'Minhas reservas'.\n");
                    } else if (d.http == 200) {
                        printf("\nReserva cancelada. Os assentos foram liberados em todas as empresas.\n");
                    } else if (d.http == 202) {
                        printf("\nCancelamento REGISTRADO, mas alguma empresa do trajeto esta fora do ar agora.\n"
                               "O servidor conclui sozinho assim que ela voltar (veja 'Minhas reservas').\n");
                    } else mostrarErro(&d);
                    apiLiberar(&d);
                }
            }
        }
    }

    apiLiberar(&r);
}

static void menuPassageiro(void) {
    char op[16];
    for (;;) {
        printf("\n=========== Passageiro: %s ===========\n", apiUsuarioId());
        printf(" 1) Buscar viagem e reservar\n 2) Minhas reservas\n 3) Cancelar reserva\n 0) Sair da conta\n");
        if (!lerLinha("> ", op, sizeof op)) return;

        switch (atoi(op)) {
            case 1: buscarEReservar(); break;
            case 2: reservasInterativo(0); break;
            case 3: reservasInterativo(1); break;
            case 0: if (op[0] == '0') return;
            default: printf("Opcao invalida.\n");
        }
    }
}

static void verSaude(void) {
    ApiResp r = apiSaude();
    if (!r.rede) semResposta(&r);
    else printf("\nServidor %s respondeu HTTP %ld: %s\n", apiBaseURL(), r.http,
                txt(jsonGetStr(r.json, "status")));
    apiLiberar(&r);
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *alvo = argc >= 2 ? argv[1] : "skyrim";
    const char *host = getenv("CLIENTE_HOST") ? getenv("CLIENTE_HOST") : "localhost";
    char base[256];

    if (strcmp(alvo, "-h") == 0 || strcmp(alvo, "--help") == 0) {
        printf("Uso: %s [regiao | host:porta | http://host:porta]\n\n", argv[0]);
        listarRegioes();
        return 0;
    }

    if (strstr(alvo, "://")) snprintf(base, sizeof base, "%s", alvo);
    else if (configuracoesCarregar(alvo, 0) == 0) snprintf(base, sizeof base, "http://%s:%d", host, servidor.porta);
    else snprintf(base, sizeof base, "http://%s", alvo);

    apiIniciar(base);
    printf("Cliente do consorcio de caronas -> %s\n", apiBaseURL());

    char op[16];
    for (;;) {
        printf("\n=========== Consorcio de Caronas ===========\n");
        printf(" 1) Cadastrar-se\n 2) Entrar\n 3) Testar conexao com o servidor\n 0) Sair\n");
        if (!lerLinha("> ", op, sizeof op)) break;

        int n = atoi(op);
        if (n == 0 && op[0] == '0') break;

        if (n == 1) cadastrar();
        else if (n == 2) {
            if (entrar()) {
                if (strcmp(apiUsuarioTipo(), "motorista") == 0) menuMotorista(); else menuPassageiro();
                apiSair();
            }
        } else if (n == 3) verSaude();
        else printf("Opcao invalida.\n");
    }

    apiEncerrar();
    return 0;
}
