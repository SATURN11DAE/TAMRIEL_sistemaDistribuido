/*
 * Implementação do armazenamento de dados
 *
 * Mantém os dados em memória (arrays dinâmicos) para acesso rápido e 
 * persiste tudo em disco na pasta "json/". Utiliza mutexes para ser 
 * thread-safe, permitindo que múltiplas conexões acessem os dados.
 *
 * Organização em disco: json/<id_do_servidor>/{usuarios,caronas,reservas}/<id>.json
 * Cada gravação é atômica: escreve em "<id>.json.tmp", força o fsync e só então
 * renomeia para "<id>.json", então uma queda no meio nunca deixa arquivo pela metade.
 *
 * REGRA DE OURO DA TRAVA: toda função que mexe nos arrays em memória faz
 * pthread_mutex_lock(&trava) no início e unlock antes de retornar. As funções
 * internas (static) que dizem "chamar com a trava" NÃO travam por conta própria.
 */

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

/* Arrays em memória e controles de capacidade */
static Usuario *usuarios = NULL; 
static int quantidadeUsuarios = 0, capacidadeUsuarios = 0;

static Carona  *caronas  = NULL; 
static int quantidadeCaronas = 0, capacidadeCaronas = 0;

static Reserva *reservas = NULL; 
static int quantidadeReservas = 0, capacidadeReservas = 0;

/* Sincronização entre threads */
static pthread_mutex_t trava = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  mudou = PTHREAD_COND_INITIALIZER;   /* avisa quando uma reserva muda de estado ou some */

/* FUNÇÕES AUXILIARES / INTERNAS */

/*
 * Garante espaço no array dinâmico, dobrando a capacidade se necessário (começa em 16).
 *   - ponteiro: endereço do array (pode ser realocado)
 *   - capacidade: capacidade atual, atualizada se crescer
 *   - quantidadeAtual / tamanhoItem: itens já usados e tamanho de cada item
 * Retorna 0 se há espaço (ou se conseguiu criar), -1 se faltou memória.
 * Use pelo macro GARANTIR(array, capacidade, quantidade).
 */
static int garantirEspaco(void **ponteiro, int *capacidade, int quantidadeAtual, size_t tamanhoItem) {
    if (quantidadeAtual < *capacidade) return 0;
    int novaCapacidade = *capacidade ? *capacidade * 2 : 16;
    void *novoPonteiro = realloc(*ponteiro, (size_t) novaCapacidade * tamanhoItem);
    if (!novoPonteiro) return -1;
    
    *ponteiro = novoPonteiro; 
    *capacidade = novaCapacidade;
    return 0;
}
#define GARANTIR(ptr, cap, n) garantirEspaco((void **) &(ptr), &(cap), (n), sizeof *(ptr))

/*
 * Copia uma string respeitando o tamanho do destino (sempre termina com '\0').
 *   - origem: NULL é tratado como ""
 */
static void copiarStringSegura(char *destino, size_t tamanho, const char *origem) {
    snprintf(destino, tamanho, "%s", origem ? origem : "");
}

/* Utilitários de sistema de arquivos */

/*
 * Monta o caminho "json/<servidor>/<subpasta>".
 *   - subpasta: "usuarios", "caronas" ou "reservas"
 */
static void montarCaminhoDiretorio(char *saida, size_t tamanho, const char *subpasta) {
    snprintf(saida, tamanho, "%s/%s/%s", PASTA_JSON, servidor.id, subpasta);
}

/*
 * Força a gravação do diretório em disco (fsync), para que criações,
 * renomeações e remoções de arquivos sobrevivam a uma queda de energia.
 */
static void sincronizarDiretorio(const char *diretorio) {
    int fd = open(diretorio, O_RDONLY);
    if (fd >= 0) { 
        fsync(fd); 
        close(fd); 
    }
}

/*
 * Grava "<subpasta>/<id>.json" de forma atômica (arquivo temporário + fsync + rename).
 *   - conteudo: texto completo do arquivo
 * Retorna ARMAZENAMENTO_OK, ou ARMAZENAMENTO_ERRO se não conseguiu gravar.
 */
static int gravarEntidadeEmDisco(const char *subpasta, const char *id, const char *conteudo) {
    char diretorio[200], caminho[320], temporario[330];
    montarCaminhoDiretorio(diretorio, sizeof diretorio, subpasta);
    snprintf(caminho, sizeof caminho, "%s/%s.json", diretorio, id);
    snprintf(temporario, sizeof temporario, "%s.tmp", caminho);

    FILE *arquivo = fopen(temporario, "w");
    if (!arquivo) { 
        perror("[ARMAZENAMENTO] fopen"); 
        return ARMAZENAMENTO_ERRO; 
    }

    fputs(conteudo, arquivo);
    fflush(arquivo);
    fsync(fileno(arquivo));
    fclose(arquivo);

    if (rename(temporario, caminho) != 0) { 
        perror("[ARMAZENAMENTO] rename"); 
        return ARMAZENAMENTO_ERRO; 
    }
    
    sincronizarDiretorio(diretorio);
    return ARMAZENAMENTO_OK;
}

/*
 * Apaga "<subpasta>/<id>.json" do disco. Não reclama se o arquivo não existir.
 */
static void apagarEntidadeDoDisco(const char *subpasta, const char *id) {
    char diretorio[200], caminho[320];
    montarCaminhoDiretorio(diretorio, sizeof diretorio, subpasta);
    snprintf(caminho, sizeof caminho, "%s/%s.json", diretorio, id);
    unlink(caminho);
    sincronizarDiretorio(diretorio);
}

/*
 * Lê um arquivo inteiro para a memória.
 * Retorna o texto alocado (quem chama dá free) ou NULL se não conseguiu ler.
 */
static char *lerArquivoCompleto(const char *caminho) {
    FILE *arquivo = fopen(caminho, "r");
    if (!arquivo) return NULL;

    fseek(arquivo, 0, SEEK_END);
    long tamanho = ftell(arquivo);
    fseek(arquivo, 0, SEEK_SET);
    if (tamanho < 0) { 
        fclose(arquivo); 
        return NULL; 
    }

    char *texto = malloc((size_t) tamanho + 1);
    if (!texto) { 
        fclose(arquivo); 
        return NULL; 
    }

    size_t lidos = fread(texto, 1, (size_t) tamanho, arquivo);
    texto[lidos] = '\0';
    fclose(arquivo);
    return texto;
}

/*
 * Cria a pasta se ela ainda não existir.
 * Retorna 0 se a pasta existe ao final, -1 se falhou.
 */
static int criarPastaSeNaoExistir(const char *caminho) {
    if (mkdir(caminho, 0755) != 0 && errno != EEXIST) { 
        perror("[ARMAZENAMENTO] mkdir"); 
        return -1; 
    }
    return 0;
}

/* Geração de JSON usando a nova API do Buffer */

/*
 * Escreve um usuário como objeto JSON no buffer.
 *   - incluirSegredo: 1 inclui salt e hash (só para gravar em disco); 0 omite
 *     (usado nas respostas da API, para nunca vazar credenciais)
 */
static void usuarioJSON(Buffer *buffer, const Usuario *u, int incluirSegredo) {
    bufferAdicionar(buffer, "{\"id\": ");   
    bufferAdicionarStringJSON(buffer, u->id);
    
    bufferAdicionar(buffer, ", \"nome\": "); 
    bufferAdicionarStringJSON(buffer, u->nome);
    
    bufferAdicionar(buffer, ", \"tipo\": "); 
    bufferAdicionarStringJSON(buffer, u->tipo);
    
    if (incluirSegredo) {
        bufferAdicionar(buffer, ", \"salt\": ");  
        bufferAdicionarStringJSON(buffer, u->salt);
        bufferAdicionar(buffer, ", \"hash\": ");  
        bufferAdicionarStringJSON(buffer, u->hash);
    }
    bufferAdicionar(buffer, "}");
}

/*
 * Escreve uma carona (rota) como objeto JSON no buffer.
 *   - incluirLivres: 1 inclui "assentos_livres" (use só em cópias que passaram
 *     por preencherLivres); 0 omite (formato gravado em disco)
 *
 * Formato:
 *  - campos de resumo no topo (origem/destino/horario/preco/assentos[_livres]),
 *    onde preco é a SOMA dos trechos e assentos[_livres] é o MÍNIMO entre os trechos;
 *  - "cidades": lista ordenada;
 *  - "trechos": um item por trecho, com horario, preco, assentos e assentos_livres.
 */
void caronaJSON(Buffer *buffer, const Carona *c, int incluirLivres) {
    int n = c->qtdCidades > 0 ? c->qtdCidades : 0;
    int qtdTrechos = n > 1 ? n - 1 : 0;
    double total = 0;
    int minAssentos = 0, minLivres = 0;

    for (int i = 0; i < qtdTrechos; i++) {
        total += c->precos[i];
        if (i == 0 || c->assentos[i] < minAssentos) minAssentos = c->assentos[i];
        if (i == 0 || c->livres[i] < minLivres) minLivres = c->livres[i];
    }

    bufferAdicionar(buffer, "{\"id\": ");          
    bufferAdicionarStringJSON(buffer, c->id);
    bufferAdicionar(buffer, ", \"servidor\": ");   
    bufferAdicionarStringJSON(buffer, servidor.id);
    bufferAdicionar(buffer, ", \"motorista\": ");  
    bufferAdicionarStringJSON(buffer, c->motorista);
    bufferAdicionar(buffer, ", \"origem\": ");     
    bufferAdicionarStringJSON(buffer, n ? c->cidades[0] : "");
    bufferAdicionar(buffer, ", \"destino\": ");    
    bufferAdicionarStringJSON(buffer, n ? c->cidades[n - 1] : "");
    bufferAdicionar(buffer, ", \"horario\": ");    
    bufferAdicionarStringJSON(buffer, qtdTrechos ? c->horarios[0] : "");
    
    bufferAdicionar(buffer, ", \"preco\": %.2f, \"assentos\": %d", total, minAssentos);
    if (incluirLivres) bufferAdicionar(buffer, ", \"assentos_livres\": %d", minLivres);

    bufferAdicionar(buffer, ", \"cidades\": [");
    for (int i = 0; i < n; i++) {
        if (i) bufferAdicionar(buffer, ", ");
        bufferAdicionarStringJSON(buffer, c->cidades[i]);
    }

    bufferAdicionar(buffer, "], \"trechos\": [");
    for (int i = 0; i < qtdTrechos; i++) {
        bufferAdicionar(buffer, "%s{\"origem\": ", i ? ", " : "");
        bufferAdicionarStringJSON(buffer, c->cidades[i]);
        bufferAdicionar(buffer, ", \"destino\": ");
        bufferAdicionarStringJSON(buffer, c->cidades[i + 1]);
        bufferAdicionar(buffer, ", \"horario\": ");
        bufferAdicionarStringJSON(buffer, c->horarios[i]);
        bufferAdicionar(buffer, ", \"preco\": %.2f, \"assentos\": %d", c->precos[i], c->assentos[i]);
        if (incluirLivres) bufferAdicionar(buffer, ", \"assentos_livres\": %d", c->livres[i]);
        bufferAdicionar(buffer, "}");
    }
    bufferAdicionar(buffer, "]}");
}

/*
 * Escreve um vetor de TrechoRes como array JSON no buffer.
 * Usado para "trechos" (locais) e "itinerario" de uma reserva.
 */
static void trechosJSON(Buffer *buffer, const TrechoRes *trechos, int quantidade) {
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
        
        bufferAdicionar(buffer, ", \"de\": %d, \"ate\": %d, \"assento\": %d}", 
                        trechos[i].de, trechos[i].ate, trechos[i].assento);
    }
    bufferAdicionar(buffer, "]");
}

/*
 * Escreve uma reserva como objeto JSON no buffer (formato usado em disco e na API).
 * Os campos "só de execução" (ativo, jaExistia, falha) não são escritos.
 */
static void reservaJSON(Buffer *buffer, const Reserva *r) {
    bufferAdicionar(buffer, "{\"id_transacao\": ");      
    bufferAdicionarStringJSON(buffer, r->idTransacao);
    bufferAdicionar(buffer, ", \"estado\": ");           
    bufferAdicionarStringJSON(buffer, r->estado);
    bufferAdicionar(buffer, ", \"coordenador_id\": ");   
    bufferAdicionarStringJSON(buffer, r->coordenadorId);
    bufferAdicionar(buffer, ", \"anterior\": ");         
    bufferAdicionarStringJSON(buffer, r->anterior);
    bufferAdicionar(buffer, ", \"proximo\": ");          
    bufferAdicionarStringJSON(buffer, r->proximo);
    bufferAdicionar(buffer, ", \"passageiro\": ");       
    bufferAdicionarStringJSON(buffer, r->passageiro);
    
    bufferAdicionar(buffer, ", \"relogio_logico\": %d, \"criado_em\": %ld, \"trechos\": ", r->relogio, r->criadoEm);
    trechosJSON(buffer, r->locais, r->qtdLocais);
    
    bufferAdicionar(buffer, ", \"itinerario\": ");
    trechosJSON(buffer, r->itin, r->qtdItin);
    
    bufferAdicionar(buffer, "}");
}

/*
 * Grava a reserva em "reservas/<idTransacao>.json".
 * Retorna ARMAZENAMENTO_OK ou ARMAZENAMENTO_ERRO.
 */
static int gravarReserva(const Reserva *r) {
    Buffer buffer; 
    bufferIniciar(&buffer);
    reservaJSON(&buffer, r);
    bufferAdicionar(&buffer, "\n");
    int rc = gravarEntidadeEmDisco("reservas", r->idTransacao, buffer.p ? buffer.p : "");
    bufferLiberar(&buffer);
    return rc;
}

/* Leitura de JSON (Parsing) usando a nova API ValorJSON */

/*
 * Lê um array JSON de trechos para o vetor 'saida' (operação inversa de trechosJSON).
 *   - maximo: capacidade de 'saida'; itens a mais no array são ignorados
 * Retorna quantos trechos foram lidos.
 */
static int lerTrechosJSON(const ValorJSON *array, TrechoRes *saida, int maximo) {
    int n = jsonTamanhoArray(array);
    if (n > maximo) n = maximo;
    
    for (int i = 0; i < n; i++) {
        const ValorJSON *item = jsonObterElemento(array, i);
        memset(&saida[i], 0, sizeof saida[i]);
        copiarStringSegura(saida[i].servidor, sizeof saida[i].servidor, jsonObterTexto(item, "servidor"));
        copiarStringSegura(saida[i].idCarona, sizeof saida[i].idCarona, jsonObterTexto(item, "id_carona"));
        copiarStringSegura(saida[i].origem, sizeof saida[i].origem, jsonObterTexto(item, "origem"));
        copiarStringSegura(saida[i].destino, sizeof saida[i].destino, jsonObterTexto(item, "destino"));
        saida[i].de = jsonObterInteiro(item, "de", 0);
        saida[i].ate = jsonObterInteiro(item, "ate", 0);
        saida[i].assento = jsonObterInteiro(item, "assento", 0);
    }
    return n;
}

/*
 * Lê todos os "*.json" de uma subpasta e entrega cada um, já analisado, ao callback.
 *   - callback: recebe o JSON (liberado aqui depois, não guarde o ponteiro)
 * Arquivos ".tmp" (sobras de gravações interrompidas) são apagados, e arquivos
 * ilegíveis são ignorados com um aviso no stderr.
 */
static void carregarArquivosDoDiretorio(const char *subpasta, void (*callback)(const ValorJSON *)) {
    char diretorio[200];
    montarCaminhoDiretorio(diretorio, sizeof diretorio, subpasta);

    DIR *d = opendir(diretorio);
    if (!d) return;

    struct dirent *entrada;
    while ((entrada = readdir(d))) {
        size_t tamanhoNome = strlen(entrada->d_name);
        char caminho[480];
        snprintf(caminho, sizeof caminho, "%s/%.255s", diretorio, entrada->d_name);

        /* Limpa arquivos temporários quebrados */
        if (tamanhoNome > 4 && strcmp(entrada->d_name + tamanhoNome - 4, ".tmp") == 0) {
            unlink(caminho);
            continue;
        }
        if (tamanhoNome <= 5 || strcmp(entrada->d_name + tamanhoNome - 5, ".json") != 0) continue;

        char *texto = lerArquivoCompleto(caminho);
        ValorJSON *jsonEncontrado = texto ? jsonAnalisar(texto) : NULL;
        
        if (jsonEncontrado) {
            callback(jsonEncontrado);
        } else {
            fprintf(stderr, "[ARMAZENAMENTO] ignorando arquivo ilegivel: %s\n", caminho);
        }

        jsonLiberar(jsonEncontrado);
        free(texto);
    }
    closedir(d);
}

/* Callbacks para processar cada tipo de entidade lida do disco */

/* Converte o JSON de um usuário e o adiciona ao array em memória (ignora se não tiver id). */
static void aoCarregarUsuario(const ValorJSON *j) {
    Usuario u; memset(&u, 0, sizeof u);
    copiarStringSegura(u.id, sizeof u.id, jsonObterTexto(j, "id"));
    copiarStringSegura(u.nome, sizeof u.nome, jsonObterTexto(j, "nome"));
    copiarStringSegura(u.tipo, sizeof u.tipo, jsonObterTexto(j, "tipo"));
    copiarStringSegura(u.salt, sizeof u.salt, jsonObterTexto(j, "salt"));
    copiarStringSegura(u.hash, sizeof u.hash, jsonObterTexto(j, "hash"));
    
    if (!u.id[0] || GARANTIR(usuarios, capacidadeUsuarios, quantidadeUsuarios)) return;
    usuarios[quantidadeUsuarios++] = u;
}

/*
 * Converte o JSON de uma carona e a adiciona ao array em memória (ignora se não tiver id).
 * Entende dois formatos: o atual (lista "trechos", de onde saem as cidades da
 * rota) e o antigo (carona de um único trecho, com origem/destino no topo).
 */
static void aoCarregarCarona(const ValorJSON *j) {
    Carona c; memset(&c, 0, sizeof c);
    copiarStringSegura(c.id, sizeof c.id, jsonObterTexto(j, "id"));
    copiarStringSegura(c.motorista, sizeof c.motorista, jsonObterTexto(j, "motorista"));

    const ValorJSON *listaTrechos = jsonObterCampo(j, "trechos");
    int nt = jsonTamanhoArray(listaTrechos);
    const char *primeiraOrigem = nt > 0 ? jsonObterTexto(jsonObterElemento(listaTrechos, 0), "origem") : NULL;

    if (nt >= 1 && nt <= MAX_TRECHOS_ROTA && primeiraOrigem) {
        /* As cidades da rota saem de origem/destino dos trechos. */
        c.qtdCidades = nt + 1;
        copiarStringSegura(c.cidades[0], sizeof c.cidades[0], primeiraOrigem);
        for (int i = 0; i < nt; i++) {
            const ValorJSON *t = jsonObterElemento(listaTrechos, i);
            copiarStringSegura(c.cidades[i + 1], sizeof c.cidades[i + 1], jsonObterTexto(t, "destino"));
            copiarStringSegura(c.horarios[i], sizeof c.horarios[i], jsonObterTexto(t, "horario"));
            c.precos[i] = jsonObterNumero(t, "preco", 0);
            c.assentos[i] = jsonObterInteiro(t, "assentos", 0);
        }
    } else {
        /* Formato antigo (carona de um unico trecho) */
        c.qtdCidades = 2;
        copiarStringSegura(c.cidades[0], sizeof c.cidades[0], jsonObterTexto(j, "origem"));
        copiarStringSegura(c.cidades[1], sizeof c.cidades[1], jsonObterTexto(j, "destino"));
        copiarStringSegura(c.horarios[0], sizeof c.horarios[0], jsonObterTexto(j, "horario"));
        c.precos[0] = jsonObterNumero(j, "preco", 0);
        c.assentos[0] = jsonObterInteiro(j, "assentos", 0);
    }
    
    if (!c.id[0] || GARANTIR(caronas, capacidadeCaronas, quantidadeCaronas)) return;
    caronas[quantidadeCaronas++] = c;
}

/*
 * Converte o JSON de uma reserva e a adiciona ao array em memória (ignora se não tiver id).
 * Reservas carregadas começam com ativo = 0, ou seja, a thread de recuperação
 * pode tratá-las se estiverem PREPARADO há tempo demais.
 */
static void aoCarregarReserva(const ValorJSON *j) {
    Reserva r; memset(&r, 0, sizeof r);
    copiarStringSegura(r.idTransacao, sizeof r.idTransacao, jsonObterTexto(j, "id_transacao"));
    copiarStringSegura(r.estado, sizeof r.estado, jsonObterTexto(j, "estado"));
    copiarStringSegura(r.coordenadorId, sizeof r.coordenadorId, jsonObterTexto(j, "coordenador_id"));
    copiarStringSegura(r.anterior, sizeof r.anterior, jsonObterTexto(j, "anterior"));
    copiarStringSegura(r.proximo, sizeof r.proximo, jsonObterTexto(j, "proximo"));
    copiarStringSegura(r.passageiro, sizeof r.passageiro, jsonObterTexto(j, "passageiro"));
    
    r.relogio = jsonObterInteiro(j, "relogio_logico", 0);
    r.criadoEm = (long) jsonObterNumero(j, "criado_em", 0);
    r.qtdLocais = lerTrechosJSON(jsonObterCampo(j, "trechos"), r.locais, MAX_TRECHOS_TX);
    r.qtdItin = lerTrechosJSON(jsonObterCampo(j, "itinerario"), r.itin, MAX_TRECHOS_TX);
    r.ativo = 0;
    
    if (!r.idTransacao[0] || GARANTIR(reservas, capacidadeReservas, quantidadeReservas)) return;
    reservas[quantidadeReservas++] = r;
}

/* Utilitários de busca de array (chamar com a trava) */

/* Posição da carona no array em memória, ou -1 se não existir. */
static int encontrarIndiceCarona(const char *id) {    
    for (int i = 0; i < quantidadeCaronas; i++) {
        if (strcmp(caronas[i].id, id) == 0) return i;
    }
    return -1;
}

/* Posição da reserva no array em memória, ou -1 se não existir. */
static int encontrarIndiceReserva(const char *id) {
    for (int i = 0; i < quantidadeReservas; i++) {
        if (strcmp(reservas[i].idTransacao, id) == 0) return i;
    }
    return -1;
}

/*
 * Conta quantos trechos de reservas (de qualquer faixa e estado) usam esta carona.
 * Se for maior que zero, a carona não pode ser removida. Chamar com a trava.
 */
static int contarAssentosOcupadosNaCarona(const char *idCarona) {
    int total = 0;
    for (int i = 0; i < quantidadeReservas; i++) {
        for (int k = 0; k < reservas[i].qtdLocais; k++) {
            if (strcmp(reservas[i].locais[k].idCarona, idCarona) == 0) total++;
        }
    }
    return total;
}

/*
 * Índice da cidade na rota da carona, ignorando maiúsculas e minúsculas.
 * Retorna -1 se a cidade for NULL ou não estiver na rota. Não usa trava.
 */
int caronaIndiceCidade(const Carona *c, const char *cidade) {
    if (!cidade) return -1;
    for (int i = 0; i < c->qtdCidades; i++) {
        if (strcasecmp(c->cidades[i], cidade) == 0) return i;
    }
    return -1;
}

/*
 * Desempate entre duas reservas que disputam o mesmo assento.
 * A mais antiga é a de menor relógio de Lamport; se empatar, vence o menor
 * coordenadorId; se ainda empatar, o menor idTransacao. Assim todos os
 * servidores chegam à mesma ordem, sem depender do relógio de parede.
 * Retorna 1 se 'a' é mais antiga que 'b'.
 */
static int reservaEaMaisAntiga(const Reserva *a, const Reserva *b) {
    if (a->relogio != b->relogio) return a->relogio < b->relogio;
    int comparacao = strcmp(a->coordenadorId, b->coordenadorId);
    if (comparacao != 0) return comparacao < 0;
    return strcmp(a->idTransacao, b->idTransacao) < 0;
}

/*
 * Situacao dos assentos numa FAIXA [de, ate) da rota.
 * Um assento so serve para a faixa se estiver livre em TODOS os trechos dela
 * (o passageiro fica sentado no mesmo lugar) e se existir em todos
 * (numero <= menor capacidade da faixa).
 * Reservas que nao se sobrepoem a faixa nao ocupam nada: o mesmo assento pode
 * ser vendido para quem viaja antes/depois.
 */
typedef struct {
    int livres;        /* quantidade de assentos livres na faixa */
    int primeiro;      /* menor numero de assento livre (0 se nenhum) */
    int preparadas;    /* reservas PREPARADO que se sobrepoem a faixa */
    int euMaisAntiga;  /* 1 se 'r' e mais antiga que todas as PREPARADO sobrepostas */
} Intervalo;

/*
 * Analisa a faixa [de, ate) da carona 'c' e devolve o resumo (ver Intervalo).
 *   - r: reserva que está tentando entrar, usada para calcular euMaisAntiga;
 *     NULL se só interessa contar assentos livres
 * Chamar com a trava.
 */
static Intervalo analisarIntervalo(const Carona *c, int de, int ate, const Reserva *r) {
    Intervalo res = { 0, 0, 0, 1 };

    int capacidade = c->assentos[de];
    for (int l = de + 1; l < ate; l++) {
        if (c->assentos[l] < capacidade) capacidade = c->assentos[l];
    }
    if (capacidade > MAX_ASSENTOS) capacidade = MAX_ASSENTOS;

    char usados[MAX_ASSENTOS + 2];
    memset(usados, 0, sizeof usados);

    for (int i = 0; i < quantidadeReservas; i++) {
        for (int k = 0; k < reservas[i].qtdLocais; k++) {
            const TrechoRes *o = &reservas[i].locais[k];
            if (strcmp(o->idCarona, c->id) != 0) continue;
            if (o->ate <= de || o->de >= ate) continue;   /* faixas disjuntas */

            if (o->assento >= 1 && o->assento <= MAX_ASSENTOS) usados[o->assento] = 1;

            if (r && strcmp(reservas[i].estado, "PREPARADO") == 0) {
                res.preparadas++;
                if (!reservaEaMaisAntiga(r, &reservas[i])) res.euMaisAntiga = 0;
            }
        }
    }

    for (int a = 1; a <= capacidade; a++) {
        if (usados[a]) continue;
        res.livres++;
        if (!res.primeiro) res.primeiro = a;
    }
    return res;
}

/* Preenche livres[] (por trecho individual) da copia da carona. Chamar com a trava. */
static void preencherLivres(Carona *c) {
    for (int i = 0; i < c->qtdCidades - 1; i++) {
        c->livres[i] = analisarIntervalo(c, i, i + 1, NULL).livres;
    }
}

/*
 * Resultado de avaliarReserva:
 *   OK: achou assento livre
 *   ESPERAR: sem assento, mas há reserva PREPARADO mais nova que pode ser
 *            desfeita, então vale esperar
 *   NAO: sem assento e sem esperança
 *   INEXISTENTE: a carona não existe neste servidor
 *   INVALIDO: origem/destino fora da rota ou na ordem errada
 */
typedef enum { AVALIACAO_OK, AVALIACAO_ESPERAR, AVALIACAO_NAO, AVALIACAO_INEXISTENTE, AVALIACAO_INVALIDO } StatusAvaliacao;

/*
 * Resolve a faixa (de/ate) do trecho pedido e escolhe um assento livre nela.
 *   - r: reserva em preparação (usada no desempate por antiguidade)
 *   - t: trecho pedido; recebe os índices 'de' e 'ate' resolvidos
 *   - assentoDestino: recebe o número do assento escolhido (só se retornar AVALIACAO_OK)
 * Chamar com a trava.
 */
static StatusAvaliacao avaliarReserva(const Reserva *r, TrechoRes *t, int *assentoDestino) {
    int indiceCarona = encontrarIndiceCarona(t->idCarona);
    if (indiceCarona < 0) return AVALIACAO_INEXISTENTE;

    const Carona *c = &caronas[indiceCarona];
    int de = caronaIndiceCidade(c, t->origem);
    int ate = caronaIndiceCidade(c, t->destino);
    if (de < 0 || ate < 0 || de >= ate) return AVALIACAO_INVALIDO;

    t->de = de;
    t->ate = ate;

    Intervalo iv = analisarIntervalo(c, de, ate, r);
    if (iv.livres > 0) {
        *assentoDestino = iv.primeiro;
        return AVALIACAO_OK;
    }

    if (iv.preparadas > 0 && iv.euMaisAntiga) return AVALIACAO_ESPERAR;
    
    return AVALIACAO_NAO;
}

/* API PÚBLICA DE ARMAZENAMENTO GERAL */

/*
 * Cria as pastas json/<servidor>/{usuarios,caronas,reservas} (se não existirem),
 * carrega tudo do disco para a memória e sincroniza o relógio de Lamport com o
 * maior relógio encontrado nas reservas (para nunca voltar no tempo após reiniciar).
 * Chamar uma vez, depois de configuracoesCarregar.
 * Retorna ARMAZENAMENTO_OK ou ARMAZENAMENTO_ERRO se não conseguiu criar as pastas.
 */
int armazenamentoIniciar(void) {
    char caminho[200];
    snprintf(caminho, sizeof caminho, "%s", PASTA_JSON);
    if (criarPastaSeNaoExistir(caminho)) return ARMAZENAMENTO_ERRO;

    snprintf(caminho, sizeof caminho, "%s/%s", PASTA_JSON, servidor.id);
    if (criarPastaSeNaoExistir(caminho)) return ARMAZENAMENTO_ERRO;

    static const char *subpastas[] = { "usuarios", "caronas", "reservas" };
    for (int i = 0; i < 3; i++) {
        montarCaminhoDiretorio(caminho, sizeof caminho, subpastas[i]);
        if (criarPastaSeNaoExistir(caminho)) return ARMAZENAMENTO_ERRO;
    }

    pthread_mutex_lock(&trava);
    
    carregarArquivosDoDiretorio("usuarios", aoCarregarUsuario);
    carregarArquivosDoDiretorio("caronas", aoCarregarCarona);
    carregarArquivosDoDiretorio("reservas", aoCarregarReserva);

    int maiorRelogio = 0;
    for (int i = 0; i < quantidadeReservas; i++) {
        if (reservas[i].relogio > maiorRelogio) maiorRelogio = reservas[i].relogio;
    }
    
    pthread_mutex_unlock(&trava);

    if (maiorRelogio > 0) lamportSincronizar(maiorRelogio);

    printf("[ARMAZENAMENTO] json/%s/: %d usuario(s), %d carona(s), %d reserva(s) carregado(s)\n",
           servidor.id, quantidadeUsuarios, quantidadeCaronas, quantidadeReservas);
    return ARMAZENAMENTO_OK;
}

/*
 * Libera todos os arrays da memória. Não apaga nada do disco.
 */
void armazenamentoEncerrar(void) {
    pthread_mutex_lock(&trava);
    
    free(usuarios); usuarios = NULL; quantidadeUsuarios = capacidadeUsuarios = 0;
    free(caronas);  caronas = NULL;  quantidadeCaronas = capacidadeCaronas = 0;
    free(reservas); reservas = NULL; quantidadeReservas = capacidadeReservas = 0;
    
    pthread_mutex_unlock(&trava);
}

/*
 * Retorna quantas reservas existem em memória (em qualquer estado).
 */
int armazenamentoQuantidade(void) {
    pthread_mutex_lock(&trava);
    int quantidade = quantidadeReservas;
    pthread_mutex_unlock(&trava);
    return quantidade;
}

/*
 * JSON com todas as reservas deste servidor.
 * Retorna texto alocado (quem chama dá free).
 */
char *armazenamentoListarJSON(void) {
    return reservasListarJSON(NULL, 0);
}

/* API PÚBLICA DE USUÁRIOS */

/*
 * Salva um usuário novo (em memória e em disco).
 * Retorna ARMAZENAMENTO_OK, ARMAZENAMENTO_EXISTE se o id já está em uso ou
 * ARMAZENAMENTO_ERRO se faltou memória ou falhou a gravação.
 */
int usuarioSalvar(const Usuario *u) {
    pthread_mutex_lock(&trava);

    for (int i = 0; i < quantidadeUsuarios; i++) {
        if (strcmp(usuarios[i].id, u->id) == 0) { 
            pthread_mutex_unlock(&trava); 
            return ARMAZENAMENTO_EXISTE; 
        }
    }
    
    if (GARANTIR(usuarios, capacidadeUsuarios, quantidadeUsuarios)) { 
        pthread_mutex_unlock(&trava); 
        return ARMAZENAMENTO_ERRO; 
    }

    Buffer buffer; 
    bufferIniciar(&buffer);
    usuarioJSON(&buffer, u, 1);
    bufferAdicionar(&buffer, "\n");
    int rc = gravarEntidadeEmDisco("usuarios", u->id, buffer.p ? buffer.p : "");
    bufferLiberar(&buffer);

    if (rc == ARMAZENAMENTO_OK) usuarios[quantidadeUsuarios++] = *u;
    
    pthread_mutex_unlock(&trava);
    return rc;
}

/*
 * Busca um usuário pelo id.
 *   - saida: recebe uma cópia do usuário (pode ser NULL se só interessa saber se existe)
 * Retorna ARMAZENAMENTO_OK ou ARMAZENAMENTO_NAO_ENCONTRADO.
 */
int usuarioBuscar(const char *id, Usuario *saida) {
    int rc = ARMAZENAMENTO_NAO_ENCONTRADO;
    
    pthread_mutex_lock(&trava);
    for (int i = 0; i < quantidadeUsuarios; i++) {
        if (strcmp(usuarios[i].id, id) == 0) { 
            if (saida) *saida = usuarios[i]; 
            rc = ARMAZENAMENTO_OK; 
            break; 
        }
    }
    pthread_mutex_unlock(&trava);
    
    return rc;
}

/*
 * JSON com todos os usuários, SEM salt e hash.
 * Retorna texto alocado (quem chama dá free).
 */
char *usuariosListarJSON(void) {
    Buffer buffer; 
    bufferIniciar(&buffer);
    
    pthread_mutex_lock(&trava);
    bufferAdicionar(&buffer, "{\"servidor\": "); 
    bufferAdicionarStringJSON(&buffer, servidor.id);
    bufferAdicionar(&buffer, ", \"total\": %d, \"usuarios\": [", quantidadeUsuarios);
    
    for (int i = 0; i < quantidadeUsuarios; i++) { 
        if (i) bufferAdicionar(&buffer, ", "); 
        usuarioJSON(&buffer, &usuarios[i], 0); 
    }
    bufferAdicionar(&buffer, "]}\n");
    pthread_mutex_unlock(&trava);
    
    return bufferExtrairTexto(&buffer);
}

/* API PÚBLICA DE CARONAS */

/*
 * Salva uma carona nova (em memória e em disco).
 * Retorna ARMAZENAMENTO_OK, ARMAZENAMENTO_EXISTE se o id já está em uso ou
 * ARMAZENAMENTO_ERRO se faltou memória ou falhou a gravação.
 */
int caronaSalvar(const Carona *c) {
    pthread_mutex_lock(&trava);

    if (encontrarIndiceCarona(c->id) >= 0) { 
        pthread_mutex_unlock(&trava); 
        return ARMAZENAMENTO_EXISTE; 
    }
    if (GARANTIR(caronas, capacidadeCaronas, quantidadeCaronas)) { 
        pthread_mutex_unlock(&trava); 
        return ARMAZENAMENTO_ERRO; 
    }

    Buffer buffer; 
    bufferIniciar(&buffer);
    caronaJSON(&buffer, c, 0);
    bufferAdicionar(&buffer, "\n");
    int rc = gravarEntidadeEmDisco("caronas", c->id, buffer.p ? buffer.p : "");
    bufferLiberar(&buffer);

    if (rc == ARMAZENAMENTO_OK) caronas[quantidadeCaronas++] = *c;
    
    pthread_mutex_unlock(&trava);
    return rc;
}

/*
 * Busca uma carona pelo id.
 *   - saida: recebe uma cópia, já com livres[] calculado (pode ser NULL)
 * Retorna ARMAZENAMENTO_OK ou ARMAZENAMENTO_NAO_ENCONTRADO.
 */
int caronaBuscar(const char *id, Carona *saida) {
    pthread_mutex_lock(&trava);
    int indice = encontrarIndiceCarona(id);
    
    if (indice >= 0 && saida) {
        *saida = caronas[indice];
        preencherLivres(saida);
    }
    
    pthread_mutex_unlock(&trava);
    return indice >= 0 ? ARMAZENAMENTO_OK : ARMAZENAMENTO_NAO_ENCONTRADO;
}

/*
 * Quantos assentos estão livres em TODOS os trechos da faixa [de, ate) da carona.
 * Retorna -1 se a carona não existe ou se a faixa for inválida.
 */
int caronaLivresIntervalo(const char *id, int de, int ate) {
    pthread_mutex_lock(&trava);

    int indice = encontrarIndiceCarona(id);
    int livres = -1;

    if (indice >= 0 && de >= 0 && ate > de && ate < caronas[indice].qtdCidades) {
        livres = analisarIntervalo(&caronas[indice], de, ate, NULL).livres;
    }

    pthread_mutex_unlock(&trava);
    return livres;
}

/*
 * Lista cópias das caronas, já com livres[] calculado.
 *   - origem: se != NULL, só as caronas que PARTEM dessa cidade (a última
 *     parada da rota não conta como partida)
 *   - lista / qtd: recebem o vetor alocado e a quantidade (quem chama dá free na lista)
 * Retorna ARMAZENAMENTO_OK ou ARMAZENAMENTO_ERRO se faltou memória.
 */
int caronaListar(const char *origem, Carona **lista, int *qtd) {
    pthread_mutex_lock(&trava);

    Carona *vetor = malloc((size_t) (quantidadeCaronas ? quantidadeCaronas : 1) * sizeof *vetor);
    if (!vetor) { 
        pthread_mutex_unlock(&trava); 
        return ARMAZENAMENTO_ERRO; 
    }

    int n = 0;
    for (int i = 0; i < quantidadeCaronas; i++) {
        if (origem) {
            /* a carona so interessa se PARTE da cidade (a ultima parada nao conta) */
            int pos = caronaIndiceCidade(&caronas[i], origem);
            if (pos < 0 || pos >= caronas[i].qtdCidades - 1) continue;
        }
        vetor[n] = caronas[i];
        preencherLivres(&vetor[n]);
        n++;
    }

    pthread_mutex_unlock(&trava);
    *lista = vetor; 
    *qtd = n;
    
    return ARMAZENAMENTO_OK;
}

/*
 * JSON com as caronas de um motorista (inclui assentos_livres).
 *   - motorista: NULL lista as caronas de todos
 * Retorna texto alocado (quem chama dá free).
 */
char *caronasListarJSONDe(const char *motorista) {
    Carona *vetor = NULL; 
    int totalEncontrado = 0;
    Buffer buffer; 
    bufferIniciar(&buffer);

    caronaListar(NULL, &vetor, &totalEncontrado);

    int quantidadeFiltrada = 0;
    for (int i = 0; i < totalEncontrado; i++) {
        if (!motorista || strcmp(vetor[i].motorista, motorista) == 0) quantidadeFiltrada++;
    }

    bufferAdicionar(&buffer, "{\"servidor\": "); 
    bufferAdicionarStringJSON(&buffer, servidor.id);
    bufferAdicionar(&buffer, ", \"total\": %d, \"caronas\": [", quantidadeFiltrada);

    int iterador = 0;
    for (int i = 0; i < totalEncontrado; i++) {
        if (motorista && strcmp(vetor[i].motorista, motorista) != 0) continue;
        if (iterador++) bufferAdicionar(&buffer, ", ");
        caronaJSON(&buffer, &vetor[i], 1);
    }
    bufferAdicionar(&buffer, "]}\n");

    free(vetor);
    return bufferExtrairTexto(&buffer);
}

/*
 * JSON com as caronas de todos os motoristas.
 * Retorna texto alocado (quem chama dá free).
 */
char *caronasListarJSON(void) { 
    return caronasListarJSONDe(NULL); 
}

/*
 * Remove uma carona (da memória e do disco).
 *   - motorista: se != NULL, só remove se a carona for dele
 * Retorna ARMAZENAMENTO_OK, ARMAZENAMENTO_NAO_ENCONTRADO,
 * ARMAZENAMENTO_PROIBIDO se pertence a outro motorista ou
 * ARMAZENAMENTO_CONFLITO se a carona já tem reservas.
 */
int caronaRemover(const char *id, const char *motorista) {
    pthread_mutex_lock(&trava);

    int indice = encontrarIndiceCarona(id);
    int rc;
    
    if (indice < 0) {
        rc = ARMAZENAMENTO_NAO_ENCONTRADO;
    } else if (motorista && strcmp(caronas[indice].motorista, motorista) != 0) {
        rc = ARMAZENAMENTO_PROIBIDO;
    } else if (contarAssentosOcupadosNaCarona(id) > 0) {
        rc = ARMAZENAMENTO_CONFLITO;
    } else {
        memmove(&caronas[indice], &caronas[indice + 1], (size_t) (quantidadeCaronas - indice - 1) * sizeof *caronas);
        quantidadeCaronas--;
        apagarEntidadeDoDisco("caronas", id);
        rc = ARMAZENAMENTO_OK;
    }

    pthread_mutex_unlock(&trava);
    return rc;
}

/* API PÚBLICA DE RESERVAS */

/*
 * Fase 1 do 2PC: escolhe e guarda um assento para cada trecho em r->locais e
 * grava a reserva como PREPARADO.
 *   - r: reserva a preparar; recebe de/ate/assento de cada trecho, o estado e
 *     ativo = 1. Se o id já existir, é substituída pela reserva guardada, com
 *     jaExistia = 1 (idempotência: um prepare repetido não reserva duas vezes)
 *   - esperaMs: tempo máximo de espera se uma reserva PREPARADO mais NOVA
 *     disputa o assento (a thread dorme em 'mudou' até alguém decidir)
 * Em caso de recusa, r->falha recebe o id da carona responsável.
 * Retorna ARMAZENAMENTO_OK, _NAO_ENCONTRADO (carona inexistente), _INVALIDO
 * (cidade fora da rota), _INDISPONIVEL (sem assento ou espera esgotada) ou
 * _ERRO (memória ou disco).
 */
int reservaPreparar(Reserva *r, int esperaMs) {
    struct timespec limiteTempo;
    clock_gettime(CLOCK_REALTIME, &limiteTempo);
    limiteTempo.tv_sec += esperaMs / 1000;
    limiteTempo.tv_nsec += (long) (esperaMs % 1000) * 1000000L;
    
    if (limiteTempo.tv_nsec >= 1000000000L) { 
        limiteTempo.tv_sec++; 
        limiteTempo.tv_nsec -= 1000000000L; 
    }

    pthread_mutex_lock(&trava);

    int indiceExistente = encontrarIndiceReserva(r->idTransacao);
    if (indiceExistente >= 0) {
        *r = reservas[indiceExistente];
        r->jaExistia = 1;
        pthread_mutex_unlock(&trava);
        return ARMAZENAMENTO_OK;
    }

    r->jaExistia = 0;
    r->falha[0] = '\0';
    int assentosAlocados[MAX_TRECHOS_TX];

    for (;;) {
        int deveEsperar = 0;
        int rc = ARMAZENAMENTO_OK;

        for (int k = 0; k < r->qtdLocais; k++) {
            StatusAvaliacao status = avaliarReserva(r, &r->locais[k], &assentosAlocados[k]);
            
            if (status == AVALIACAO_INEXISTENTE) { 
                copiarStringSegura(r->falha, sizeof r->falha, r->locais[k].idCarona); 
                rc = ARMAZENAMENTO_NAO_ENCONTRADO; 
                break; 
            }
            if (status == AVALIACAO_INVALIDO) { 
                copiarStringSegura(r->falha, sizeof r->falha, r->locais[k].idCarona); 
                rc = ARMAZENAMENTO_INVALIDO; 
                break; 
            }
            if (status == AVALIACAO_NAO) { 
                copiarStringSegura(r->falha, sizeof r->falha, r->locais[k].idCarona); 
                rc = ARMAZENAMENTO_INDISPONIVEL; 
                break; 
            }
            if (status == AVALIACAO_ESPERAR) { 
                copiarStringSegura(r->falha, sizeof r->falha, r->locais[k].idCarona); 
                deveEsperar = 1; 
            }
        }

        if (rc != ARMAZENAMENTO_OK) { 
            pthread_mutex_unlock(&trava); 
            return rc; 
        }
        if (!deveEsperar) break;

        if (pthread_cond_timedwait(&mudou, &trava, &limiteTempo) == ETIMEDOUT) {
            pthread_mutex_unlock(&trava);
            return ARMAZENAMENTO_INDISPONIVEL;
        }
    }

    for (int k = 0; k < r->qtdLocais; k++) {
        r->locais[k].assento = assentosAlocados[k];
    }
    
    copiarStringSegura(r->estado, sizeof r->estado, "PREPARADO");
    r->ativo = 1;

    if (GARANTIR(reservas, capacidadeReservas, quantidadeReservas)) { 
        pthread_mutex_unlock(&trava); 
        return ARMAZENAMENTO_ERRO; 
    }
    
    reservas[quantidadeReservas++] = *r;

    int rc = gravarReserva(r);
    if (rc != ARMAZENAMENTO_OK) quantidadeReservas--;

    pthread_mutex_unlock(&trava);
    return rc;
}

/*
 * Busca uma reserva pelo id.
 *   - saida: recebe uma cópia (pode ser NULL)
 * Retorna ARMAZENAMENTO_OK ou ARMAZENAMENTO_NAO_ENCONTRADO.
 */
int reservaBuscar(const char *id, Reserva *saida) {
    pthread_mutex_lock(&trava);
    int indice = encontrarIndiceReserva(id);
    if (indice >= 0 && saida) *saida = reservas[indice];
    pthread_mutex_unlock(&trava);
    
    return indice >= 0 ? ARMAZENAMENTO_OK : ARMAZENAMENTO_NAO_ENCONTRADO;
}

/*
 * Muda o estado da reserva e grava em disco. Acorda quem espera em reservaPreparar.
 *   - estado: ESTADO_PREPARADO, ESTADO_COMITADO ou ESTADO_CANCELANDO
 *   - itin: se != NULL, guarda também o itinerário completo (até MAX_TRECHOS_TX)
 * Retorna ARMAZENAMENTO_OK, ARMAZENAMENTO_NAO_ENCONTRADO ou ARMAZENAMENTO_ERRO.
 */
int reservaDecidir(const char *id, const char *estado, const TrechoRes *itin, int qtdItin) {
    pthread_mutex_lock(&trava);

    int indice = encontrarIndiceReserva(id);
    int rc;
    
    if (indice < 0) {
        rc = ARMAZENAMENTO_NAO_ENCONTRADO;
    } else {
        copiarStringSegura(reservas[indice].estado, sizeof reservas[indice].estado, estado);
        
        if (itin) {
            if (qtdItin > MAX_TRECHOS_TX) qtdItin = MAX_TRECHOS_TX;
            memcpy(reservas[indice].itin, itin, (size_t) qtdItin * sizeof *itin);
            reservas[indice].qtdItin = qtdItin;
        }
        
        rc = gravarReserva(&reservas[indice]);
        pthread_cond_broadcast(&mudou);
    }

    pthread_mutex_unlock(&trava);
    return rc;
}

/*
 * Rollback: remove a reserva (memória e disco) e libera os assentos.
 * Retorna ARMAZENAMENTO_OK, ARMAZENAMENTO_NAO_ENCONTRADO ou
 * ARMAZENAMENTO_CONFLITO se a reserva já está COMITADO ou CANCELANDO.
 */
int reservaRemover(const char *id) {
    pthread_mutex_lock(&trava);

    int indice = encontrarIndiceReserva(id);
    int rc;
    
    if (indice < 0) {
        rc = ARMAZENAMENTO_NAO_ENCONTRADO;
    } else if (strcmp(reservas[indice].estado, "COMITADO") == 0 || strcmp(reservas[indice].estado, "CANCELANDO") == 0) {
        rc = ARMAZENAMENTO_CONFLITO;
    } else {
        memmove(&reservas[indice], &reservas[indice + 1], (size_t) (quantidadeReservas - indice - 1) * sizeof *reservas);
        quantidadeReservas--;
        apagarEntidadeDoDisco("reservas", id);
        rc = ARMAZENAMENTO_OK;
        pthread_cond_broadcast(&mudou);
    }

    pthread_mutex_unlock(&trava);
    return rc;
}

/*
 * Remove a reserva sem checar o estado (usado ao concluir um cancelamento).
 * Retorna ARMAZENAMENTO_OK ou ARMAZENAMENTO_NAO_ENCONTRADO.
 */
int reservaApagar(const char *id) {
    pthread_mutex_lock(&trava);

    int indice = encontrarIndiceReserva(id);
    int rc;
    
    if (indice < 0) {
        rc = ARMAZENAMENTO_NAO_ENCONTRADO;
    } else {
        memmove(&reservas[indice], &reservas[indice + 1], (size_t) (quantidadeReservas - indice - 1) * sizeof *reservas);
        quantidadeReservas--;
        apagarEntidadeDoDisco("reservas", id);
        rc = ARMAZENAMENTO_OK;
        pthread_cond_broadcast(&mudou);
    }

    pthread_mutex_unlock(&trava);
    return rc;
}

/*
 * Lista cópias das reservas que estão em um determinado estado.
 *   - lista / qtd: recebem o vetor alocado e a quantidade (quem chama dá free na lista)
 * Retorna ARMAZENAMENTO_OK ou ARMAZENAMENTO_ERRO se faltou memória.
 */
int reservaListarPorEstado(const char *estado, Reserva **lista, int *qtd) {
    pthread_mutex_lock(&trava);

    Reserva *vetor = malloc((size_t) (quantidadeReservas ? quantidadeReservas : 1) * sizeof *vetor);
    if (!vetor) { 
        pthread_mutex_unlock(&trava); 
        return ARMAZENAMENTO_ERRO; 
    }

    int n = 0;
    for (int i = 0; i < quantidadeReservas; i++) {
        if (strcmp(reservas[i].estado, estado) == 0) vetor[n++] = reservas[i];
    }

    pthread_mutex_unlock(&trava);
    *lista = vetor; 
    *qtd = n;
    
    return ARMAZENAMENTO_OK;
}

/*
 * Marca se uma requisição está tratando a transação (campo Reserva.ativo).
 * Enquanto ativo = 1, a thread de recuperação ignora a reserva.
 * Não faz nada se o id não existir.
 */
void reservaAtivo(const char *id, int ativo) {
    pthread_mutex_lock(&trava);
    int indice = encontrarIndiceReserva(id);
    if (indice >= 0) reservas[indice].ativo = ativo;
    pthread_mutex_unlock(&trava);
}

/*
 * Lista cópias das reservas PREPARADO, inativas e com pelo menos 'idadeMinS'
 * segundos de vida: as candidatas à recuperação de falhas.
 *   - lista / qtd: recebem o vetor alocado e a quantidade (quem chama dá free na lista)
 * Retorna ARMAZENAMENTO_OK ou ARMAZENAMENTO_ERRO se faltou memória.
 */
int reservaPendentes(int idadeMinS, Reserva **lista, int *qtd) {
    pthread_mutex_lock(&trava);

    Reserva *vetor = malloc((size_t) (quantidadeReservas ? quantidadeReservas : 1) * sizeof *vetor);
    if (!vetor) { 
        pthread_mutex_unlock(&trava); 
        return ARMAZENAMENTO_ERRO; 
    }

    long tempoAgora = agoraEmSegundos();
    int n = 0;
    
    for (int i = 0; i < quantidadeReservas; i++) {
        if (strcmp(reservas[i].estado, "PREPARADO") != 0 || reservas[i].ativo) continue;
        if (tempoAgora - reservas[i].criadoEm < idadeMinS) continue;
        vetor[n++] = reservas[i];
    }

    pthread_mutex_unlock(&trava);
    *lista = vetor; 
    *qtd = n;
    
    return ARMAZENAMENTO_OK;
}

/*
 * JSON com as reservas deste servidor.
 *   - passageiro: se != NULL, só as desse passageiro
 *   - soCoordenador: se 1, só as que este servidor coordena
 * Retorna texto alocado (quem chama dá free).
 */
char *reservasListarJSON(const char *passageiro, int soCoordenador) {
    Buffer buffer; 
    bufferIniciar(&buffer);
    
    pthread_mutex_lock(&trava);

    bufferAdicionar(&buffer, "{\"servidor\": "); 
    bufferAdicionarStringJSON(&buffer, servidor.id);
    bufferAdicionar(&buffer, ", \"reservas\": [");

    int iterador = 0;
    for (int i = 0; i < quantidadeReservas; i++) {
        if (soCoordenador && strcmp(reservas[i].coordenadorId, servidor.id) != 0) continue;
        if (passageiro && strcmp(reservas[i].passageiro, passageiro) != 0) continue;
        
        if (iterador++) bufferAdicionar(&buffer, ", ");
        reservaJSON(&buffer, &reservas[i]);
    }
    bufferAdicionar(&buffer, "], \"total\": %d}\n", iterador);

    pthread_mutex_unlock(&trava);
    return bufferExtrairTexto(&buffer);
}
