#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util/headers/configuracoes.h"
#include "util/headers/persistencia.h"
#include "util/headers/json.h"
#include "util/headers/auth.h"
#include "headers/cadastro.h"

Resp cadastroUsuario(const char *corpo) {
    JVal *j = corpo ? jsonParse(corpo) : NULL;
    if (!j) return respErro(400, "Corpo JSON invalido");

    Usuario u;
    memset(&u, 0, sizeof u);

    const char *id = jsonGetStr(j, "id"), *nome = jsonGetStr(j, "nome"), *tipo = jsonGetStr(j, "tipo");
    const char *senha = jsonGetStr(j, "senha");

    if (!id || !idValido(id) || strlen(id) >= sizeof u.id) { jsonLiberar(j); return respErro(400, "id invalido (use letras, numeros, '-', '_' ou '.')"); }
    if (!nome || !*nome) { jsonLiberar(j); return respErro(400, "Informe o nome"); }
    if (!tipo || (strcmp(tipo, "motorista") != 0 && strcmp(tipo, "passageiro") != 0)) {
        jsonLiberar(j);
        return respErro(400, "tipo deve ser 'motorista' ou 'passageiro'");
    }

    if (!senha || strlen(senha) < AUTH_SENHA_MIN || strlen(senha) > AUTH_SENHA_MAX) {
        jsonLiberar(j);
        return respErro(400, "senha obrigatoria, de 4 a 72 caracteres");
    }

    snprintf(u.id, sizeof u.id, "%s", id);
    snprintf(u.nome, sizeof u.nome, "%s", nome);
    snprintf(u.tipo, sizeof u.tipo, "%s", tipo);

    int rcHash = authGerarHash(senha, u.salt, u.hash);
    jsonLiberar(j);
    if (rcHash != 0) return respErro(500, "Falha ao proteger a senha");

    int rc = usuarioSalvar(&u);
    if (rc == ARMAZENAMENTO_EXISTE) return respErro(409, "Usuario ja cadastrado neste servidor");
    if (rc != ARMAZENAMENTO_OK) return respErro(500, "Falha ao gravar o usuario em disco");

    Buf b; bufIni(&b);
    bufAdd(&b, "{\"id\": ");   bufStr(&b, u.id);
    bufAdd(&b, ", \"nome\": "); bufStr(&b, u.nome);
    bufAdd(&b, ", \"tipo\": "); bufStr(&b, u.tipo);
    bufAdd(&b, ", \"servidor\": "); bufStr(&b, servidor.id);
    bufAdd(&b, "}\n");
    return respJSON(201, bufTomar(&b));
}

Resp cadastroCaronaComo(const char *forcado, const char *corpo) {
    JVal *j = corpo ? jsonParse(corpo) : NULL;
    if (!j) return respErro(400, "Corpo JSON invalido");

    Carona c;
    memset(&c, 0, sizeof c);

    const char *mot = forcado ? forcado : jsonGetStr(j, "motorista"), *orig = jsonGetStr(j, "origem"), *dest = jsonGetStr(j, "destino");
    const char *hor = jsonGetStr(j, "horario");
    c.preco = jsonGetNum(j, "preco", 0);
    c.assentos = jsonGetInt(j, "assentos", 0);

    Resp erroResp = { 0, NULL };
    Usuario u;

    if (!mot || !idValido(mot)) erroResp = respErro(400, "Informe o motorista (id valido)");
    else if (usuarioBuscar(mot, &u) != ARMAZENAMENTO_OK) erroResp = respErro(404, "Motorista nao cadastrado neste servidor");
    else if (strcmp(u.tipo, "motorista") != 0) erroResp = respErro(403, "O usuario informado nao e motorista");
    else if (!orig || !*orig || !dest || !*dest) erroResp = respErro(400, "Informe origem e destino");
    else if (strcasecmp(orig, dest) == 0) erroResp = respErro(400, "Origem e destino devem ser diferentes");
    else if (c.assentos < 1 || c.assentos > MAX_ASSENTOS) erroResp = respErro(400, "assentos deve estar entre 1 e 60");
    else if (c.preco < 0) erroResp = respErro(400, "preco invalido");

    if (erroResp.json) { jsonLiberar(j); return erroResp; }

    char uuid[37];
    gerarUUID(uuid);
    snprintf(c.id, sizeof c.id, "%.20s-%.8s", servidor.id, uuid);
    snprintf(c.motorista, sizeof c.motorista, "%s", mot);
    snprintf(c.origem, sizeof c.origem, "%s", orig);
    snprintf(c.destino, sizeof c.destino, "%s", dest);
    snprintf(c.horario, sizeof c.horario, "%s", hor ? hor : "");
    jsonLiberar(j);

    if (caronaSalvar(&c) != ARMAZENAMENTO_OK) return respErro(500, "Falha ao gravar a carona em disco");

    c.livres = c.assentos;
    Buf b; bufIni(&b);
    caronaJSON(&b, &c, 1);
    bufAdd(&b, "\n");
    return respJSON(201, bufTomar(&b));
}

Resp cadastroCarona(const char *corpo) { return cadastroCaronaComo(NULL, corpo); }

Resp cadastroListarUsuarios(void) { return respJSON(200, usuariosListarJSON()); }
Resp cadastroListarCaronas(void)  { return respJSON(200, caronasListarJSON()); }
Resp cadastroListarCaronasDe(const char *motorista) { return respJSON(200, caronasListarJSONDe(motorista)); }

Resp cadastroRemoverCarona(const char *motorista, const char *idCarona) {
    if (!idCarona || !idValido(idCarona)) return respErro(400, "id da carona invalido");

    Carona c;
    int rc = caronaRemover(idCarona, motorista);

    if (rc == ARMAZENAMENTO_NAO_ENCONTRADO) return respErro(404, "Carona nao encontrada");
    if (rc == ARMAZENAMENTO_PROIBIDO)       return respErro(403, "Esta carona pertence a outro motorista");

    if (rc == ARMAZENAMENTO_CONFLITO) {
        char msg[200] = "A carona tem reservas ativas; so pode ser excluida depois que os passageiros cancelarem";
        if (caronaBuscar(idCarona, &c) == ARMAZENAMENTO_OK) {
            snprintf(msg, sizeof msg, "A carona tem %d assento(s) reservado(s); so pode ser excluida depois que "
                     "os passageiros cancelarem", c.assentos - c.livres);
        }
        return respErro(409, msg);
    }
    if (rc != ARMAZENAMENTO_OK) return respErro(500, "Falha ao excluir a carona");

    Buf b; bufIni(&b);
    bufAdd(&b, "{\"id\": "); bufStr(&b, idCarona);
    bufAdd(&b, ", \"status\": \"EXCLUIDA\"}\n");
    return respJSON(200, bufTomar(&b));
}
