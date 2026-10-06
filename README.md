# VaiJunto: Consórcio de Caronas

Servidor P2P em C para o consórcio de caronas compartilhadas.
Cada servidor representa uma região do mapa de Tamriel, guarda os próprios dados em disco, além de enviar e receber requisições somente dos vizinhos de fronteira por uma API REST, sem servidor central.

> **Estado atual:** camada de comunicação, relógio de Lamport e persistência estão prontas.
> A lógica de carona/assento e o 2PC ainda estão em desenvolvimento (veja [O que falta](#o-que-falta)).

## Requisitos

```bash
sudo apt install build-essential libmicrohttpd-dev libcurl4-openssl-dev
```

## Compilar

```bash
make clean && make
```

## Rodar

Um terminal por servidor:

```bash
./servidor skyrim          # porta padrão 3001
./servidor morrowind       # porta padrão 3002
./servidor cyrodiil 4000   # porta opcional como segundo argumento
```

Sem argumentos, o programa lista as regiões válidas.

| Região | Porta | Vizinhos |
|---|---|---|
| skyrim | 3001 | high_rock, hammerfell, cyrodiil, morrowind |
| morrowind | 3002 | skyrim, cyrodiil, black_marsh |
| cyrodiil | 3003 | skyrim, hammerfell, morrowind, black_marsh, elsweyr, valenwood, summerset_islands |
| high_rock | 3004 | skyrim, hammerfell |
| hammerfell | 3005 | high_rock, skyrim, cyrodiil, valenwood, summerset_islands |
| black_marsh | 3006 | morrowind, cyrodiil, elsweyr |
| elsweyr | 3007 | valenwood, cyrodiil, black_marsh |
| valenwood | 3008 | elsweyr, cyrodiil, hammerfell, summerset_islands |
| summerset_islands | 3009 | hammerfell, cyrodiil, valenwood |

Por padrão os vizinhos são procurados em `localhost`. Para outra máquina ou container,
use variáveis de ambiente `HOST_<REGIAO>`:

```bash
HOST_SKYRIM=192.168.0.10 ./servidor morrowind
```

## Estrutura

```
main.c               inicialização
cli.c / cli.h        menu de texto para testes
http/
  httpServidor.*     recebe requisições dos vizinhos (libmicrohttpd)
  httpCliente.*      envia requisições aos vizinhos (libcurl, com timeout)
util/
  configuracoes.*    regiões, portas e fronteiras
  lamport.*          relógio lógico de Lamport (thread-safe)
  armazenamento.*    persistência em json/<servidor>.json
  util.*             JSON simples e UUID
json/                criada em tempo de execução (um arquivo por servidor)
```

## API do consórcio

Prefixo: `/api/consorcio`. Toda requisição leva `X-Server-Id` e `X-Lamport-Clock`;
quem recebe atualiza o relógio com `max(local, recebido) + 1` e devolve o seu na resposta.

| Método e rota | Função | Respostas |
|---|---|---|
| `GET /health` | detecção de falha | 200 |
| `GET /trechos?origem=&destino=&visitados=` | busca de trechos | 200, 400 |
| `GET /reservas` | lista as reservas guardadas no servidor | 200 |
| `POST /reservas/prepare` | fase 1 do 2PC (grava `PREPARADO`) | 200 READY, 400, 500 |
| `PUT /reservas/{id}/commit` | fase 2 (marca `COMITADO`) | 200, 404 |
| `DELETE /reservas/{id}` | rollback (remove a reserva) | 200 (idempotente) |
| `GET /reservas/{id}/status` | estado da transação | 200, 404 |

Corpo do `prepare`:

```json
{
  "id_transacao": "uuid",
  "coordenador_id": "morrowind",
  "relogio_logico": 14,
  "trechos_solicitados": [{"servidor": "skyrim", "id_carona": "sk-45"}]
}
```

## Persistência

Cada servidor grava `json/<id>.json` a cada `prepare`, `commit` e `DELETE`, e carrega o arquivo ao iniciar.
A escrita é feita em arquivo temporário seguido de `rename()`, então uma queda no meio não corrompe o JSON.
O relógio de Lamport reinicia a partir do maior valor guardado.

```json
{
  "servidor": "skyrim",
  "reservas": [
    {"id_transacao": "tx-A", "estado": "COMITADO", "coordenador_id": "morrowind", "id_carona": "sk-45", "relogio_logico": 14}
  ]
}
```

A pasta `json/` é relativa ao diretório onde o servidor é executado. Sugestão: colocá-la no `.gitignore`
e, em Docker, montá-la como volume.

## Menu de testes

Ao iniciar, cada servidor abre um menu no terminal:

| Opção | Ação |
|---|---|
| 1 | `GET /health` de um vizinho |
| 2 | `GET /health` de todos os vizinhos |
| 3 | `GET /trechos` |
| 4 | `POST /reservas/prepare` (vazio gera UUID) |
| 5 | `PUT /reservas/{id}/commit` |
| 6 | `DELETE /reservas/{id}` |
| 7 | `GET /reservas/{id}/status` |
| 8 | `GET /reservas` de um vizinho |
| 9 | ver as reservas guardadas neste servidor |
| 0 | sair |

O `id_transacao` do último `prepare` vira o padrão nas opções 5, 6 e 7.

## Testes rápidos com curl

```bash
H=localhost:3001/api/consorcio

curl -i $H/health
curl "$H/trechos?origem=Mournhold&destino=Daggerfall&visitados=morrowind"

curl -X POST $H/reservas/prepare \
  -d '{"id_transacao":"abc-123","coordenador_id":"morrowind","relogio_logico":14,"trechos_solicitados":[{"servidor":"skyrim","id_carona":"sk-45"}]}'
curl -X PUT    $H/reservas/abc-123/commit
curl           $H/reservas/abc-123/status
curl           $H/reservas
curl -X DELETE $H/reservas/abc-123

curl -i -H "X-Server-Id: teste" -H "X-Lamport-Clock: 50" $H/health
```

Teste de falha: dê `Ctrl+C` em um servidor e use a opção 1 ou 2 em outro. Deve aparecer `FALHOU ... [TIMEOUT]`
(timeout de requisição de 3 s e de conexão de 1,5 s). Para simular um servidor travado:
`pkill -STOP -f "servidor skyrim"` e depois `pkill -CONT -f "servidor skyrim"`.

## O que falta

- Modelo de carona/trecho/assento por servidor (`/trechos` ainda devolve um trecho fixo)
- Trancar assento de verdade no `prepare` (hoje sempre responde READY)
- Coordenação do 2PC: repassar `prepare`/`commit`/`DELETE` pela cadeia de participantes
- Critério de precedência em disputas (relógio lógico + `coordenador_id`)
- Recuperação após queda: reservas em `PREPARADO` consultam o coordenador (`/status`)
- Busca de itinerário recursiva usando `visitados`
- Simulador de carga e teste automatizado com injeção de falhas
