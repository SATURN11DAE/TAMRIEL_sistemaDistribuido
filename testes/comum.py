#!/usr/bin/env python3
"""
comum.py - utilitários compartilhados pelos testes de carga e de falhas.
Usa apenas a biblioteca padrão do Python 3.8+.

Contém:
  - http()         requisição HTTP que nunca lança exceção
  - preparar()     cria motoristas, caronas e passageiros de uma rodada
  - gerar_carga()  passageiros virtuais que buscam, reservam e cancelam sem parar
  - estabilizar()  espera a recuperação terminar (sem PREPARADO/CANCELANDO)
  - auditar()      confere as invariantes do consórcio lendo o disco de cada servidor
  - relatorio()    imprime os resultados
"""

import base64
import json
import math
import random
import string
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass

PORTAS = {"skyrim": 3001, "morrowind": 3002, "cyrodiil": 3003}
REGIOES = list(PORTAS)
SENHA = "1234"
ESTADOS_TRANSITORIOS = ("PREPARADO", "CANCELANDO")


# ----------------------------------------------------------------------------
# HTTP
# ----------------------------------------------------------------------------

@dataclass
class Resp:
    rede: bool      # False = sem resposta (queda, timeout, conexão recusada)
    http: int       # código HTTP (0 se não houve resposta)
    json: dict
    erro: str
    ms: float

    @property
    def chave(self):
        return self.http if self.rede else "rede"


def http(metodo, regiao, caminho, corpo=None, usuario=None, timeout=30.0, host="localhost"):
    """Faz UMA requisição. Nunca lança exceção: falhas de rede viram Resp(rede=False)."""
    url = f"http://{host}:{PORTAS[regiao]}{caminho}"
    dados = json.dumps(corpo).encode() if corpo is not None else None
    cabecalhos = {"Accept": "application/json"}
    if dados is not None:
        cabecalhos["Content-Type"] = "application/json"
    if usuario:
        credencial = base64.b64encode(f"{usuario[0]}:{usuario[1]}".encode()).decode()
        cabecalhos["Authorization"] = "Basic " + credencial

    pedido = urllib.request.Request(url, data=dados, headers=cabecalhos, method=metodo)
    inicio = time.monotonic()
    try:
        try:
            with urllib.request.urlopen(pedido, timeout=timeout) as r:
                codigo, texto = r.status, r.read()
        except urllib.error.HTTPError as e:
            codigo, texto = e.code, e.read()
    except Exception as e:  # queda, timeout, reset de conexão...
        return Resp(False, 0, {}, f"{type(e).__name__}: {e}", (time.monotonic() - inicio) * 1000)

    ms = (time.monotonic() - inicio) * 1000
    try:
        corpo_json = json.loads(texto) if texto else {}
    except ValueError:
        corpo_json = {}
    return Resp(True, codigo, corpo_json if isinstance(corpo_json, dict) else {}, "", ms)


def percentil(valores, p):
    if not valores:
        return 0.0
    ordenados = sorted(valores)
    k = max(0, min(len(ordenados) - 1, math.ceil(p / 100 * len(ordenados)) - 1))
    return ordenados[k]


# ----------------------------------------------------------------------------
# Preparação do cenário
# ----------------------------------------------------------------------------

@dataclass
class Ambiente:
    host: str
    tag: str
    capacidade: int
    cidades: list       # [A, B, C, D]
    usuarios: list      # [(id, regiao)]
    caronas: dict       # id_carona -> {"regiao", "capacidades": [por trecho]}
    consultas: dict     # regiao -> [(origem, destino)] que começam numa carona dessa região


def preparar(host, n_passageiros, capacidade, tag=None, log=print):
    """
    Monta uma malha pequena com cidades únicas desta rodada (assim rodadas
    antigas não interferem):

        skyrim    : A -> B -> C
        morrowind :      B -> C -> D
        cyrodiil  :           C -> D

    Há vários itinerários de A a D, passando por 1, 2 ou 3 empresas e em ordens
    diferentes. Todos os trechos têm a mesma capacidade, então com mais
    passageiros que assentos as disputas são constantes.
    """
    tag = tag or "".join(random.choices(string.ascii_lowercase + string.digits, k=5))
    A, B, C, D = (f"{letra}-{tag}" for letra in "ABCD")
    rotas = {
        "skyrim": [(A, B), (B, C)],
        "morrowind": [(B, C), (C, D)],
        "cyrodiil": [(C, D)],
    }
    consultas = {
        "skyrim": [(A, D), (A, C), (A, B)],
        "morrowind": [(B, D), (B, C)],
        "cyrodiil": [(C, D)],
    }

    caronas = {}
    for regiao, trechos in rotas.items():
        motorista = (f"mot-{regiao}-{tag}", SENHA)
        r = http("POST", regiao, "/api/cliente/usuarios", host=host, timeout=15,
                 corpo={"id": motorista[0], "nome": motorista[0], "tipo": "motorista", "senha": SENHA})
        if not r.rede:
            raise RuntimeError(f"servidor {regiao} não respondeu em {host}:{PORTAS[regiao]} ({r.erro})")
        if r.http not in (201, 409):
            raise RuntimeError(f"cadastro do motorista em {regiao} falhou: HTTP {r.http} {r.json}")

        corpo = {"trechos": [{"origem": o, "destino": d, "horario": f"{8 + i:02d}:00",
                              "preco": 10.0, "assentos": capacidade}
                             for i, (o, d) in enumerate(trechos)]}
        r = http("POST", regiao, "/api/cliente/rotas", corpo, motorista, host=host, timeout=15)
        if r.http != 201:
            raise RuntimeError(f"publicar rota em {regiao} falhou: HTTP {r.http} {r.json or r.erro}")
        caronas[r.json["id"]] = {"regiao": regiao, "capacidades": [capacidade] * len(trechos)}
        log(f"  carona {r.json['id']} em {regiao}: {' -> '.join([trechos[0][0]] + [d for _, d in trechos])}")

    usuarios = [(f"p{i}-{tag}", REGIOES[i % len(REGIOES)]) for i in range(n_passageiros)]

    def cadastrar(usuario):
        uid, regiao = usuario
        r = http("POST", regiao, "/api/cliente/usuarios", host=host, timeout=30,
                 corpo={"id": uid, "nome": uid, "tipo": "passageiro", "senha": SENHA})
        if r.http not in (201, 409):
            raise RuntimeError(f"cadastro de {uid} em {regiao} falhou: HTTP {r.http} {r.json or r.erro}")

    with ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(cadastrar, usuarios))
    log(f"  {len(usuarios)} passageiros cadastrados ({', '.join(REGIOES)})")

    return Ambiente(host, tag, capacidade, [A, B, C, D], usuarios, caronas, consultas)


# ----------------------------------------------------------------------------
# Carga
# ----------------------------------------------------------------------------

class Metricas:
    """Tudo o que os passageiros virtuais observam (thread-safe)."""

    def __init__(self):
        self.trava = threading.Lock()
        self.amostras = defaultdict(list)   # operação -> [(chave, ms)]
        self.confirmadas = {}               # id_transacao -> regiao (201 e ainda não cancelada)
        self.canceladas = {}                # id_transacao -> 200 | 202
        self.duvidosas = set()              # cancelamento sem resposta: resultado desconhecido
        self.incertas = 0                   # reservas sem resposta conclusiva
        self.sem_itinerario = 0

    def registrar(self, operacao, r):
        with self.trava:
            self.amostras[operacao].append((r.chave, r.ms))


def passageiro_virtual(amb, usuario, regiao, fim, met, rng, p_cancelar, pensar_ms, timeout):
    """Laço de um passageiro: busca, reserva uma opção e, às vezes, cancela."""
    credencial = (usuario, SENHA)
    base = "/api/cliente"

    while time.monotonic() < fim:
        origem, destino = rng.choice(amb.consultas[regiao])
        caminho = f"{base}/itinerarios?origem={urllib.parse.quote(origem)}&destino={urllib.parse.quote(destino)}"
        r = http("GET", regiao, caminho, usuario=credencial, host=amb.host, timeout=timeout)
        met.registrar("busca", r)
        if r.http != 200:
            time.sleep(0.2)
            continue

        itinerarios = r.json.get("itinerarios", [])
        if not itinerarios:
            with met.trava:
                met.sem_itinerario += 1
            time.sleep(0.2)   # tudo lotado: espera cancelamentos liberarem assentos
            continue

        # metade das vezes todos disputam a 1ª opção (ponto quente); no resto, escolha livre
        escolhido = itinerarios[0] if rng.random() < 0.5 else rng.choice(itinerarios)
        corpo = {"trechos": [{k: t[k] for k in ("servidor", "id_carona", "origem", "destino")}
                             for t in escolhido["trechos"]]}
        r = http("POST", regiao, f"{base}/reservas", corpo, credencial, host=amb.host, timeout=timeout)
        met.registrar("reserva", r)

        if r.http == 201:
            id_tx = r.json.get("id_transacao")
            with met.trava:
                met.confirmadas[id_tx] = regiao
            if rng.random() < p_cancelar:
                time.sleep(rng.uniform(0, 0.5))
                c = http("DELETE", regiao, f"{base}/reservas/{id_tx}", usuario=credencial,
                         host=amb.host, timeout=timeout)
                met.registrar("cancelamento", c)
                with met.trava:
                    if c.rede and c.http in (200, 202):
                        met.confirmadas.pop(id_tx, None)
                        met.canceladas[id_tx] = c.http
                    elif not c.rede or c.http >= 500:
                        met.confirmadas.pop(id_tx, None)
                        met.duvidosas.add(id_tx)
        elif not r.rede or (r.http >= 500 and r.json.get("status") != "RECUSADO"):
            # sem resposta, ou erro do servidor que não diz RECUSADO: pode ou não ter confirmado.
            # (400/404/409/502 e qualquer corpo "RECUSADO" são recusas definitivas: nada fica reservado)
            with met.trava:
                met.incertas += 1

        if pensar_ms:
            time.sleep(pensar_ms / 1000)


def gerar_carga(amb, duracao, met, p_cancelar=0.3, pensar_ms=0, timeout=30.0, semente=None):
    """Roda um passageiro virtual por usuário durante 'duracao' s. Retorna o tempo real gasto."""
    semente = semente if semente is not None else random.randrange(1 << 30)
    inicio = time.monotonic()
    fim = inicio + duracao
    threads = []
    for i, (uid, regiao) in enumerate(amb.usuarios):
        t = threading.Thread(
            target=passageiro_virtual, daemon=True,
            args=(amb, uid, regiao, fim, met, random.Random(semente + i), p_cancelar, pensar_ms, timeout))
        t.start()
        threads.append(t)
    for t in threads:
        t.join()
    return time.monotonic() - inicio


# ----------------------------------------------------------------------------
# Recuperação e auditoria
# ----------------------------------------------------------------------------

def coletar(host, regioes=REGIOES, timeout=10):
    """Lê as reservas guardadas em cada servidor. Retorna ({regiao: [reservas]}, [regioes sem resposta])."""
    dados, falhas = {}, []
    for regiao in regioes:
        r = http("GET", regiao, "/api/consorcio/reservas", host=host, timeout=timeout)
        if r.rede and r.http == 200:
            dados[regiao] = r.json.get("reservas", [])
        else:
            falhas.append(regiao)
    return dados, falhas


def estabilizar(host, timeout=90, log=print):
    """Espera todos os servidores responderem e não restar reserva PREPARADO/CANCELANDO."""
    inicio = time.monotonic()
    while True:
        dados, falhas = coletar(host)
        pendentes = sum(1 for rs in dados.values() for x in rs if x.get("estado") in ESTADOS_TRANSITORIOS)
        decorrido = time.monotonic() - inicio
        if not falhas and pendentes == 0:
            return True, decorrido
        if decorrido > timeout:
            log(f"  !! sem estabilizar em {timeout:.0f}s (sem resposta: {falhas or '-'}, pendentes: {pendentes})")
            return False, decorrido
        time.sleep(1)


CATEGORIAS = [
    ("servidor", "Todos os servidores responderam à auditoria"),
    ("exclusao", "Exclusão mútua: nenhum assento vendido duas vezes"),
    ("overbooking", "Nenhum trecho com mais reservas que assentos"),
    ("pendente", "Nenhuma reserva presa em PREPARADO/CANCELANDO"),
    ("atomicidade", "Atomicidade: nenhuma reserva confirmada pela metade ou órfã"),
    ("durabilidade", "Durabilidade: toda reserva confirmada ao passageiro existe"),
    ("cancelamento", "Toda reserva cancelada sumiu de todas as empresas"),
    ("fantasma", "Nenhuma confirmação que o passageiro não conhece"),
]


def auditar(amb, met):
    """
    Lê o estado de todos os servidores e confere as invariantes. Deve ser chamada
    depois de estabilizar(). Retorna {"violacoes": [(categoria, texto)], "avisos": [...], "resumo": {...}}.
    """
    violacoes, avisos = [], []
    dados, falhas = coletar(amb.host)
    for regiao in falhas:
        violacoes.append(("servidor", f"{regiao} não respondeu na auditoria"))

    meus = {uid for uid, _ in amb.usuarios}
    copias = defaultdict(dict)   # id_transacao -> {regiao: reserva}
    for regiao, reservas in dados.items():
        for x in reservas:
            if x.get("passageiro") in meus:
                copias[x["id_transacao"]][regiao] = x

    # --- exclusão mútua e overbooking, por carona ---
    por_carona = defaultdict(list)
    for id_tx, por_regiao in copias.items():
        for x in por_regiao.values():
            if x.get("estado") == "CANCELADA_MOTORISTA":
                continue
            for t in x.get("trechos", []):
                por_carona[t["id_carona"]].append((id_tx, t.get("de", 0), t.get("ate", 0), t.get("assento", 0)))

    for id_carona, itens in por_carona.items():
        info = amb.caronas.get(id_carona)
        if not info:
            continue
        caps = info["capacidades"]
        ocupacao = [0] * len(caps)
        por_assento = defaultdict(list)

        for id_tx, de, ate, assento in itens:
            if not (0 <= de < ate <= len(caps)):
                violacoes.append(("overbooking", f"{id_carona}: faixa inválida [{de},{ate}) na reserva {id_tx[:8]}"))
                continue
            if assento < 1 or assento > min(caps[de:ate]):
                violacoes.append(("overbooking", f"{id_carona}: assento {assento} fora da capacidade na reserva {id_tx[:8]}"))
            for i in range(de, ate):
                ocupacao[i] += 1
            por_assento[assento].append((de, ate, id_tx))

        for i, ocupados in enumerate(ocupacao):
            if ocupados > caps[i]:
                violacoes.append(("overbooking", f"{id_carona}: trecho {i} com {ocupados} reservas para {caps[i]} assentos"))

        for assento, faixas in por_assento.items():
            fim_maximo, dono = -1, None
            for de, ate, id_tx in sorted(faixas):
                if de < fim_maximo:
                    violacoes.append(("exclusao", f"{id_carona}: assento {assento} vendido para {dono[:8]} e {id_tx[:8]} ao mesmo tempo"))
                if ate > fim_maximo:
                    fim_maximo, dono = ate, id_tx

    # --- pendências, atomicidade ---
    comitadas = set()
    for id_tx, por_regiao in copias.items():
        for regiao, x in por_regiao.items():
            if x["estado"] in ESTADOS_TRANSITORIOS:
                violacoes.append(("pendente", f"reserva {id_tx[:8]} ainda {x['estado']} em {regiao}"))

        coordenador = next(iter(por_regiao.values())).get("coordenador_id")
        copia_coord = por_regiao.get(coordenador)
        if copia_coord is None:
            violacoes.append(("atomicidade", f"reserva {id_tx[:8]} existe em {sorted(por_regiao)} mas não no coordenador {coordenador} (órfã)"))
            continue

        estados = {regiao: x["estado"] for regiao, x in por_regiao.items()}
        if "COMITADO" not in estados.values():
            continue
        comitadas.add(id_tx)

        itinerario = copia_coord.get("itinerario") or []
        if not itinerario:
            avisos.append(f"reserva {id_tx[:8]} sem itinerário gravado no coordenador; participantes não conferidos")
            continue
        esperados = {t["servidor"] for t in itinerario} | {coordenador}
        for regiao in sorted(esperados):
            if estados.get(regiao) != "COMITADO":
                violacoes.append(("atomicidade", f"reserva {id_tx[:8]} confirmada pela metade: {regiao} está {estados.get(regiao, 'sem cópia')}"))
        for regiao in sorted(set(estados) - esperados):
            violacoes.append(("atomicidade", f"reserva {id_tx[:8]} tem cópia em {regiao}, fora do itinerário"))

    # --- o que os passageiros observaram x o que ficou gravado ---
    for id_tx in met.confirmadas:
        if id_tx not in comitadas:
            violacoes.append(("durabilidade", f"reserva {id_tx[:8]} foi confirmada ao passageiro (201) mas não está COMITADO"))
    for id_tx, codigo in met.canceladas.items():
        if id_tx in copias:
            violacoes.append(("cancelamento", f"reserva {id_tx[:8]} cancelada (HTTP {codigo}) ainda existe em {sorted(copias[id_tx])}"))

    desconhecidas = comitadas - set(met.confirmadas) - set(met.canceladas) - met.duvidosas
    if len(desconhecidas) > met.incertas:
        violacoes.append(("fantasma", f"{len(desconhecidas)} reservas confirmadas que o passageiro não conhece, "
                                      f"mas só {met.incertas} reservas ficaram sem resposta conclusiva"))
    elif desconhecidas:
        avisos.append(f"{len(desconhecidas)} reserva(s) confirmada(s) sem resposta ao passageiro "
                      f"(explicadas pelas {met.incertas} requisições sem resposta conclusiva)")

    resumo = {
        "reservas_em_disco": len(copias),
        "comitadas": len(comitadas),
        "confirmadas_ao_passageiro": len(met.confirmadas),
        "canceladas": len(met.canceladas),
        "sem_resposta_conclusiva": met.incertas,
    }
    return {"violacoes": violacoes, "avisos": avisos, "resumo": resumo}


# ----------------------------------------------------------------------------
# Relatório
# ----------------------------------------------------------------------------

def estatisticas(met, duracao):
    saida = {}
    for operacao, amostras in met.amostras.items():
        tempos = [ms for _, ms in amostras]
        codigos = defaultdict(int)
        for chave, _ in amostras:
            codigos[str(chave)] += 1
        saida[operacao] = {
            "total": len(amostras),
            "por_segundo": round(len(amostras) / duracao, 2) if duracao else 0,
            "codigos": dict(sorted(codigos.items())),
            "p50_ms": round(percentil(tempos, 50), 1),
            "p95_ms": round(percentil(tempos, 95), 1),
            "p99_ms": round(percentil(tempos, 99), 1),
            "max_ms": round(max(tempos), 1) if tempos else 0,
        }
    return saida


def relatorio(titulo, met, duracao, estabilizou, tempo_estabilizar, resultado, log=print):
    """Imprime o relatório e devolve um dicionário serializável em JSON."""
    estat = estatisticas(met, duracao)
    log(f"\n=== {titulo} ===")
    log(f"Duração da carga: {duracao:.1f}s | recuperação até estabilizar: {tempo_estabilizar:.1f}s"
        f"{'' if estabilizou else '  (NÃO estabilizou)'}")
    log(f"\n{'operação':<14}{'total':>8}{'req/s':>8}{'p50':>9}{'p95':>9}{'p99':>9}{'máx':>9}   códigos")
    for operacao in ("busca", "reserva", "cancelamento"):
        e = estat.get(operacao)
        if not e:
            continue
        codigos = " ".join(f"{k}:{v}" for k, v in e["codigos"].items())
        log(f"{operacao:<14}{e['total']:>8}{e['por_segundo']:>8}{e['p50_ms']:>8.0f}ms{e['p95_ms']:>7.0f}ms"
            f"{e['p99_ms']:>7.0f}ms{e['max_ms']:>7.0f}ms   {codigos}")
    log(f"buscas sem itinerário (tudo lotado): {met.sem_itinerario}")

    reservas = estat.get("reserva", {}).get("codigos", {})
    if reservas:
        confirmadas = reservas.get("201", 0)
        log(f"reservas confirmadas: {confirmadas}/{sum(reservas.values())}  ({confirmadas / duracao:.2f}/s)")

    log("\nInvariantes:")
    por_categoria = defaultdict(list)
    for categoria, texto in resultado["violacoes"]:
        por_categoria[categoria].append(texto)
    for categoria, descricao in CATEGORIAS:
        ocorrencias = por_categoria.get(categoria, [])
        log(f"  [{'OK ' if not ocorrencias else 'ERRO'}] {descricao}" + (f"  ({len(ocorrencias)})" if ocorrencias else ""))
        for texto in ocorrencias[:5]:
            log(f"         - {texto}")
        if len(ocorrencias) > 5:
            log(f"         ... e mais {len(ocorrencias) - 5}")
    for aviso in resultado["avisos"]:
        log(f"  aviso: {aviso}")
    log(f"\nResumo: {resultado['resumo']}")
    log(f"RESULTADO: {'APROVADO' if not resultado['violacoes'] and estabilizou else 'REPROVADO'}")

    return {
        "titulo": titulo,
        "duracao_s": round(duracao, 2),
        "estabilizou": estabilizou,
        "tempo_estabilizar_s": round(tempo_estabilizar, 2),
        "operacoes": estat,
        "buscas_sem_itinerario": met.sem_itinerario,
        "violacoes": [{"categoria": c, "texto": t} for c, t in resultado["violacoes"]],
        "avisos": resultado["avisos"],
        "resumo": resultado["resumo"],
        "aprovado": not resultado["violacoes"] and estabilizou,
    }
