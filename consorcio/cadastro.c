/*
 * cadastro.c - Implementação das regras de negócio de usuários e caronas
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "headers/configuracoes.h"
#include "headers/persistencia.h"
#include "headers/json.h"
#include "headers/auth.h"
#include "headers/cadastro.h"

/*
 * Cadastra um usuário novo neste servidor (POST /usuarios).
 * Corpo: {"id", "nome", "tipo": "motorista" | "passageiro", "senha"}.
 * A senha é guardada só como hash com salt (authGerarHash).
 * Retorna 201 com os dados públicos do usuário, 400 se algo for inválido,
 * 409 se o id já existe ou 500 em falha interna.
 */
RespostaServidor cadastroUsuario(const char *corpo) {
    ValorJSON *json = corpo ? jsonAnalisar(corpo) : NULL;
    if (!json) return criarRespostaErro(400, "Corpo JSON invalido");

    Usuario u;
    memset(&u, 0, sizeof u);

    const char *id = jsonObterTexto(json, "id");
    const char *nome = jsonObterTexto(json, "nome");
    const char *tipo = jsonObterTexto(json, "tipo");
    const char *senha = jsonObterTexto(json, "senha");

    if (!id || !identificadorValido(id) || strlen(id) >= sizeof u.id) { 
        jsonLiberar(json); 
        return criarRespostaErro(400, "id invalido (use letras, numeros, '-', '_' ou '.')"); 
    }
    if (!nome || !*nome) { 
        jsonLiberar(json); 
        return criarRespostaErro(400, "Informe o nome"); 
    }
    if (!tipo || (strcmp(tipo, TIPO_MOTORISTA) != 0 && strcmp(tipo, TIPO_PASSAGEIRO) != 0)) {
        jsonLiberar(json);
        return criarRespostaErro(400, "tipo deve ser 'motorista' ou 'passageiro'");
    }

    if (!senha || strlen(senha) < AUTH_SENHA_MIN || strlen(senha) > AUTH_SENHA_MAX) {
        jsonLiberar(json);
        return criarRespostaErro(400, "senha obrigatoria, de 4 a 72 caracteres");
    }

    snprintf(u.id, sizeof u.id, "%s", id);
    snprintf(u.nome, sizeof u.nome, "%s", nome);
    snprintf(u.tipo, sizeof u.tipo, "%s", tipo);

    int rcHash = authGerarHash(senha, u.salt, u.hash);
    jsonLiberar(json);
    
    if (rcHash != 0) return criarRespostaErro(500, "Falha ao proteger a senha");

    int rc = usuarioSalvar(&u);
    if (rc == ARMAZENAMENTO_EXISTE) return criarRespostaErro(409, "Usuario ja cadastrado neste servidor");
    if (rc != ARMAZENAMENTO_OK) return criarRespostaErro(500, "Falha ao gravar o usuario em disco");

    Buffer buffer; 
    bufferIniciar(&buffer);
    bufferAdicionar(&buffer, "{\"id\": ");   
    bufferAdicionarStringJSON(&buffer, u.id);
    
    bufferAdicionar(&buffer, ", \"nome\": "); 
    bufferAdicionarStringJSON(&buffer, u.nome);
    
    bufferAdicionar(&buffer, ", \"tipo\": "); 
    bufferAdicionarStringJSON(&buffer, u.tipo);
    
    bufferAdicionar(&buffer, ", \"servidor\": "); 
    bufferAdicionarStringJSON(&buffer, servidor.id);
    bufferAdicionar(&buffer, "}\n");
    
    return criarRespostaJSON(201, bufferExtrairTexto(&buffer));
}

/*
 * Lê e valida a rota do corpo JSON, preenchendo a carona 'c'. Aceita dois formatos:
 *   rota:    {"cidades": ["A","B","C"], "trechos": [{"horario","preco","assentos"}, ...]}
 *   simples: {"origem","destino","horario","preco","assentos"}   (rota de 2 cidades)
 * Regras: os trechos devem se encadear, sem cidades repetidas, com 1 a 60
 * assentos por trecho e preço não negativo.
 * Retorna uma resposta com json == NULL se estiver tudo certo, ou o erro 400.
 */
static RespostaServidor lerRota(const ValorJSON *json, Carona *c) {
    RespostaServidor ok = { 0, NULL };

    const ValorJSON *trechos = jsonObterCampo(json, "trechos");
    int nt = jsonTamanhoArray(trechos);

    if (nt == 0) {
        const char *orig = jsonObterTexto(json, "origem");
        const char *dest = jsonObterTexto(json, "destino");
        const char *hor  = jsonObterTexto(json, "horario");

        if (!orig || !*orig || !dest || !*dest) return criarRespostaErro(400, "Informe origem e destino (ou a lista de trechos da rota)");

        c->qtdCidades = 2;
        snprintf(c->cidades[0], sizeof c->cidades[0], "%s", orig);
        snprintf(c->cidades[1], sizeof c->cidades[1], "%s", dest);
        snprintf(c->horarios[0], sizeof c->horarios[0], "%s", hor ? hor : "");
        c->precos[0] = jsonObterNumero(json, "preco", 0);
        c->assentos[0] = jsonObterInteiro(json, "assentos", 0);
    } else {
        /* Cada trecho traz origem e destino; as cidades da rota saem deles. */
        if (nt < 1 || nt > MAX_TRECHOS_ROTA) return criarRespostaErro(400, "A rota deve ter de 2 a 10 cidades (1 a 9 trechos)");

        c->qtdCidades = nt + 1;
        for (int i = 0; i < nt; i++) {
            const ValorJSON *t = jsonObterElemento(trechos, i);
            const char *orig = jsonObterTexto(t, "origem");
            const char *dest = jsonObterTexto(t, "destino");
            const char *hor  = jsonObterTexto(t, "horario");

            if (!orig || !*orig || !dest || !*dest) return criarRespostaErro(400, "Cada trecho precisa de origem e destino");
            if (i == 0) snprintf(c->cidades[0], sizeof c->cidades[0], "%s", orig);
            else if (strcasecmp(c->cidades[i], orig) != 0) return criarRespostaErro(400, "Os trechos devem se encadear: o destino de um e a origem do proximo");

            snprintf(c->cidades[i + 1], sizeof c->cidades[i + 1], "%s", dest);
            snprintf(c->horarios[i], sizeof c->horarios[i], "%s", hor ? hor : "");
            c->precos[i] = jsonObterNumero(t, "preco", 0);
            c->assentos[i] = jsonObterInteiro(t, "assentos", 0);
        }
    }

    /* Cidades nao podem se repetir: origem/destino de uma reserva sao achados pelo nome. */
    for (int i = 0; i < c->qtdCidades; i++) {
        for (int j = i + 1; j < c->qtdCidades; j++) {
            if (strcasecmp(c->cidades[i], c->cidades[j]) == 0) return criarRespostaErro(400, "A rota nao pode repetir cidades");
        }
    }
    for (int i = 0; i < c->qtdCidades - 1; i++) {
        if (c->assentos[i] < 1 || c->assentos[i] > MAX_ASSENTOS) return criarRespostaErro(400, "assentos de cada trecho deve estar entre 1 e 60");
        if (c->precos[i] < 0) return criarRespostaErro(400, "preco invalido");
    }
    return ok;
}

/*
 * Publica uma carona (uma rota com vários trechos).
 *   - forcado: id do motorista vindo da autenticação (ignora o do corpo);
 *     NULL usa o campo "motorista" do corpo
 * O motorista precisa existir neste servidor e ser do tipo motorista.
 * Retorna 201 com a carona criada, 400/403/404 se algo for inválido ou 500 em falha ao gravar.
 */
RespostaServidor cadastroCaronaComo(const char *forcado, const char *corpo) {
    ValorJSON *json = corpo ? jsonAnalisar(corpo) : NULL;
    if (!json) return criarRespostaErro(400, "Corpo JSON invalido");

    Carona c;
    memset(&c, 0, sizeof c);

    const char *mot = forcado ? forcado : jsonObterTexto(json, "motorista");

    RespostaServidor erroResp = { 0, NULL };
    Usuario u;

    if (!mot || !identificadorValido(mot)) erroResp = criarRespostaErro(400, "Informe o motorista (id valido)");
    else if (usuarioBuscar(mot, &u) != ARMAZENAMENTO_OK) erroResp = criarRespostaErro(404, "Motorista nao cadastrado neste servidor");
    else if (strcmp(u.tipo, TIPO_MOTORISTA) != 0) erroResp = criarRespostaErro(403, "O usuario informado nao e motorista");
    else erroResp = lerRota(json, &c);

    if (erroResp.json) { 
        jsonLiberar(json); 
        return erroResp; 
    }

    char uuid[37];
    gerarUUID(uuid);
    snprintf(c.id, sizeof c.id, "%.20s-%.8s", servidor.id, uuid);
    snprintf(c.motorista, sizeof c.motorista, "%s", mot);
    jsonLiberar(json);

    if (caronaSalvar(&c) != ARMAZENAMENTO_OK) return criarRespostaErro(500, "Falha ao gravar a carona em disco");

    for (int i = 0; i < c.qtdCidades - 1; i++) c.livres[i] = c.assentos[i];

    Buffer buffer; 
    bufferIniciar(&buffer);
    caronaJSON(&buffer, &c, 1);
    bufferAdicionar(&buffer, "\n");
    
    return criarRespostaJSON(201, bufferExtrairTexto(&buffer));
}

/*
 * Atalho de cadastroCaronaComo em que o motorista vem do corpo JSON.
 */
RespostaServidor cadastroCarona(const char *corpo) { 
    return cadastroCaronaComo(NULL, corpo); 
}

/*
 * Publica UMA carona com vários trechos (POST /rotas).
 * Hoje faz o mesmo que cadastroCaronaComo: /rotas e /caronas são equivalentes.
 */
RespostaServidor cadastroRota(const char *motorista, const char *corpo) {
    return cadastroCaronaComo(motorista, corpo);
}

/*
 * Lista os usuários cadastrados (sem salt nem hash).
 */
RespostaServidor cadastroListarUsuarios(void) { 
    return criarRespostaJSON(200, usuariosListarJSON()); 
}

/*
 * Lista as caronas de todos os motoristas.
 */
RespostaServidor cadastroListarCaronas(void) { 
    return criarRespostaJSON(200, caronasListarJSON()); 
}

/*
 * Lista as caronas de um motorista, com os assentos livres de cada trecho.
 */
RespostaServidor cadastroListarCaronasDe(const char *motorista) { 
    return criarRespostaJSON(200, caronasListarJSONDe(motorista)); 
}

/*
 * Exclui uma carona (DELETE /caronas/{id}).
 * Retorna 200 se excluída, 404 se não existe, 403 se é de outro motorista,
 * 409 se já tem reservas ou 400/500 em caso de erro.
 */
RespostaServidor cadastroRemoverCarona(const char *motorista, const char *idCarona) {
    if (!idCarona || !identificadorValido(idCarona)) return criarRespostaErro(400, "id da carona invalido");

    int rc = caronaRemover(idCarona, motorista);

    if (rc == ARMAZENAMENTO_NAO_ENCONTRADO) return criarRespostaErro(404, "Carona nao encontrada");
    if (rc == ARMAZENAMENTO_PROIBIDO)       return criarRespostaErro(403, "Esta carona pertence a outro motorista");
    if (rc == ARMAZENAMENTO_CONFLITO) {
        return criarRespostaErro(409, "A carona tem reservas ativas; so pode ser excluida depois que os passageiros cancelarem");
    }
    if (rc != ARMAZENAMENTO_OK) return criarRespostaErro(500, "Falha ao excluir a carona");

    Buffer buffer; 
    bufferIniciar(&buffer);
    bufferAdicionar(&buffer, "{\"id\": "); 
    bufferAdicionarStringJSON(&buffer, idCarona);
    bufferAdicionar(&buffer, ", \"status\": \"EXCLUIDA\"}\n");
    
    return criarRespostaJSON(200, bufferExtrairTexto(&buffer));
}
