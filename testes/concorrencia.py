#!/usr/bin/env python3
"""
concorrencia.py - Testes de concorrência DIRIGIDOS (corridas montadas de propósito).

Diferente do carga.py (carga aleatória) e do falhas.py (quedas de servidor), aqui cada
cenário monta uma disputa específica e solta todas as requisições no mesmo instante
(uma barreira libera todas as threads juntas). Durante todo o cenário, um monitor lê o
estado dos três servidores a cada 100 ms e confere, em cada leitura, que nenhum assento
foi dado a duas reservas e que nenhum trecho passou da capacidade (vale também para
reservas ainda PREPARADO, não só no fim). No final, a auditoria de sempre (comum.auditar)
confere atomicidade, durabilidade e cancelamentos.

Cenários:
  ultimo_assento      N passageiros nos 3 servidores disputam, ao mesmo tempo, os mesmos
                      assentos de um trecho: nunca pode vender mais do que existe
  cruzada             reservas que seguram assentos em ordens OPOSTAS (skyrim->morrowind e
                      morrowind->skyrim): precisa resolver sem travar (deadlock)
  cancelamento_duplo  vários DELETE da MESMA reserva ao mesmo tempo: nada de erro 5xx e o
                      assento tem de ficar livre em todas as empresas
  churn_um_assento    reservar e cancelar sem parar disputando UM assento
  exclusao_carona     o motorista exclui a carona enquanto passageiros reservam nela

Cada cenário roda --rodadas vezes, cada uma com cidades e caronas novas.

Pré-requisito: os três servidores rodando (skyrim, morrowind, cyrodiil), como no carga.py.

Uso:
    python3 testes/concorrencia.py
    python3 testes/concorrencia.py --cenarios cruzada churn_um_assento --rodadas 5
    python3 testes/concorrencia.py --passageiros 24 --duracao 20 --semente 123 --verbose

Código de saída: 0 se tudo passou, 1 se algum cenário falhou, 2 se os servidores não respondem.
"""

import argparse
import json
import os
import random
import sys
import threading
import time
from collections import defaultdict

from comum import (REGIOES, SENHA, Metricas, Resp, auditar, coletar, estabilizar, http,
                   preparar, relatorio)


# ----------------------------------------------------------------------------
# Peças comuns
# ----------------------------------------------------------------------------

def trecho(amb, regiao, i, j):
    """Trecho da carona de 'regiao' entre as cidades de índice i e j (A=0, B=1, C=2, D=3)."""
    id_carona = next(c for c, info in amb.caronas.items() if info["regiao"] == regiao)
    return {"servidor": regiao, "id_carona": id_carona,
            "origem": amb.cidades[i], "destino": amb.cidades[j]}


def ordenar(regiao, trechos):
    """Os trechos do próprio servidor coordenador vêm primeiro (regra do txReservarComo);
    a ordem relativa dos demais é mantida."""
    return sorted(trechos, key=lambda t: t["servidor"] != regiao)


def usuarios_de(amb, regiao):
    return [u for u, r in amb.usuarios if r == regiao]


def reservar(amb, met, usuario, regiao, trechos, timeout):
    """POST /reservas anotando o resultado nas métricas, igual ao passageiro_virtual."""
    r = http("POST", regiao, "/api/cliente/reservas", {"trechos": trechos}, (usuario, SENHA),
             host=amb.host, timeout=timeout)
    met.registrar("reserva", r)
    if r.http == 201:
        with met.trava:
            met.confirmadas[r.json.get("id_transacao")] = regiao
    elif not r.rede or (r.http >= 500 and r.json.get("status") != "RECUSADO"):
        with met.trava:
            met.incertas += 1
    return r


def cancelar(amb, met, usuario, regiao, id_tx, timeout):
    """DELETE /reservas/{id} anotando o resultado nas métricas, igual ao passageiro_virtual."""
    c = http("DELETE", regiao, f"/api/cliente/reservas/{id_tx}", usuario=(usuario, SENHA),
             host=amb.host, timeout=timeout)
    met.registrar("cancelamento", c)
    with met.trava:
        if c.rede and c.http in (200, 202):
            met.confirmadas.pop(id_tx, None)
            met.canceladas[id_tx] = c.http
        elif not c.rede or c.http >= 500:
            met.confirmadas.pop(id_tx, None)
            met.duvidosas.add(id_tx)
    return c


def disparar(tarefas):
    """Roda as funções ao mesmo tempo (todas esperam numa barreira) e devolve os
    resultados na ordem. Uma exceção numa tarefa vira o próprio resultado dela."""
    barreira = threading.Barrier(len(tarefas))
    resultados = [None] * len(tarefas)

    def rodar(i, tarefa):
        try:
            barreira.wait(30)
            resultados[i] = tarefa()
        except Exception as e:  # noqa: BLE001 - o teste precisa sobreviver e reportar
            resultados[i] = e

    threads = [threading.Thread(target=rodar, args=(i, t), daemon=True) for i, t in enumerate(tarefas)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return resultados


def classificar(resultados):
    """Separa as reservas confirmadas (201) das recusas normais (409) e das respostas inesperadas."""
    ganhou, anomalias = [], []
    for r in resultados:
        if not isinstance(r, Resp):
            anomalias.append(f"exceção no teste: {r!r}")
        elif r.http == 201:
            ganhou.append(r)
        elif r.http != 409:
            anomalias.append(f"HTTP {r.http}" if r.rede else f"sem resposta ({r.erro})")
    return ganhou, anomalias


# ----------------------------------------------------------------------------
# Monitor: confere os assentos DURANTE o cenário
# ----------------------------------------------------------------------------

def conferir_assentos(amb, reservas, meus):
    """Confere exclusão mútua e capacidade num retrato de UM servidor. Devolve textos de violação."""
    achados = []
    faixas = defaultdict(list)   # (carona, assento) -> [(de, ate, id_tx)]
    ocupacao = {}                # carona -> ocupação por trecho

    for x in reservas:
        if x.get("passageiro") not in meus or x.get("estado") == "CANCELADA_MOTORISTA":
            continue
        for t in x.get("trechos", []):
            info = amb.caronas.get(t.get("id_carona"))
            if not info:
                continue
            caps = info["capacidades"]
            de, ate = t.get("de", 0), t.get("ate", 0)
            if not (0 <= de < ate <= len(caps)):
                continue
            ocup = ocupacao.setdefault(t["id_carona"], [0] * len(caps))
            for i in range(de, ate):
                ocup[i] += 1
            faixas[(t["id_carona"], t.get("assento", 0))].append((de, ate, x["id_transacao"]))

    for carona, ocup in ocupacao.items():
        caps = amb.caronas[carona]["capacidades"]
        for i, n in enumerate(ocup):
            if n > caps[i]:
                achados.append(f"{carona}: trecho {i} com {n} reservas para {caps[i]} assentos")

    for (carona, assento), lista in faixas.items():
        fim, dono = -1, None
        for de, ate, id_tx in sorted(lista):
            if de < fim:
                achados.append(f"{carona}: assento {assento} dado a {dono[:8]} e a {id_tx[:8]} ao mesmo tempo")
            if ate > fim:
                fim, dono = ate, id_tx
    return achados


class Monitor(threading.Thread):
    """Lê os três servidores sem parar enquanto o cenário roda e guarda as violações."""

    def __init__(self, amb, intervalo=0.1):
        super().__init__(daemon=True)
        self.amb = amb
        self.intervalo = intervalo
        self.parar = threading.Event()
        self.violacoes = []
        self.leituras = 0

    def run(self):
        meus = {u for u, _ in self.amb.usuarios}
        while not self.parar.is_set():
            for regiao in REGIOES:
                r = http("GET", regiao, "/api/consorcio/reservas", host=self.amb.host, timeout=3)
                if r.rede and r.http == 200:
                    for texto in conferir_assentos(self.amb, r.json.get("reservas", []), meus):
                        if texto not in self.violacoes:
                            self.violacoes.append(texto)
            self.leituras += 1
            self.parar.wait(self.intervalo)


# ----------------------------------------------------------------------------
# Cenários: cada um devolve (problemas, avisos)
# ----------------------------------------------------------------------------

def cenario_ultimo_assento(amb, met, args, rng):
    """Todos os passageiros pedem o MESMO trecho no mesmo instante, de servidores diferentes."""
    alvo = [trecho(amb, "skyrim", 0, 2)]
    res = disparar([(lambda u=u, r=r: reservar(amb, met, u, r, alvo, args.timeout))
                    for u, r in amb.usuarios])

    ganhou, anomalias = classificar(res)
    cap = amb.capacidade
    problemas, avisos = [], []

    if len(ganhou) > cap:
        problemas.append(f"{len(ganhou)} reservas confirmadas para {cap} assento(s) (overbooking)")
    assentos = [t.get("assento") for r in ganhou for t in r.json.get("trechos", [])]
    if len(assentos) != len(set(assentos)):
        problemas.append(f"assento repetido entre as reservas confirmadas: {sorted(assentos)}")
    if len(ganhou) < cap:
        (avisos if anomalias else problemas).append(
            f"só {len(ganhou)} de {cap} assento(s) foram vendidos com {len(res)} interessados")
    if anomalias:
        avisos.append("respostas inesperadas: " + ", ".join(sorted(set(anomalias))))
    return problemas, avisos


def cenario_cruzada(amb, met, args, rng):
    """Dois grupos pedem os MESMOS dois assentos (um no skyrim, um no morrowind) em ordens opostas.
    Sem uma regra de desempate, cada grupo seguraria um assento esperando o outro (deadlock)."""
    sk, mo = trecho(amb, "skyrim", 0, 1), trecho(amb, "morrowind", 1, 2)
    tarefas = []
    for k, (u, r) in enumerate(amb.usuarios):
        inverso = r == "morrowind" or (r == "cyrodiil" and (k // 3) % 2 == 1)
        itin = ordenar(r, [mo, sk] if inverso else [sk, mo])
        tarefas.append(lambda u=u, r=r, itin=itin: reservar(amb, met, u, r, itin, args.timeout))

    res = disparar(tarefas)
    ganhou, anomalias = classificar(res)
    problemas, avisos = [], []

    if len(ganhou) > amb.capacidade:
        problemas.append(f"{len(ganhou)} reservas confirmadas para {amb.capacidade} assento(s)")
    travadas = [r for r in res if isinstance(r, Resp) and (not r.rede or r.ms > args.timeout * 900)]
    if travadas:
        problemas.append(f"{len(travadas)} requisição(ões) travaram ou estouraram o prazo (possível deadlock)")
    if not ganhou:
        avisos.append("ninguém ganhou: as reservas se recusaram mutuamente (seguro, mas ninguém viajou)")
    if anomalias:
        avisos.append("respostas inesperadas: " + ", ".join(sorted(set(anomalias))))
    return problemas, avisos


def cenario_cancelamento_duplo(amb, met, args, rng):
    """Uma reserva nas 3 empresas é cancelada por vários DELETE simultâneos."""
    itin = [trecho(amb, "skyrim", 0, 1), trecho(amb, "morrowind", 1, 2), trecho(amb, "cyrodiil", 2, 3)]
    u1, u2 = usuarios_de(amb, "skyrim")[:2]

    r = reservar(amb, met, u1, "skyrim", itin, args.timeout)
    if r.http != 201:
        return [f"a reserva inicial falhou: HTTP {r.http} {r.json or r.erro}"], []
    id_tx = r.json["id_transacao"]

    res = disparar([(lambda: cancelar(amb, met, u1, "skyrim", id_tx, args.timeout)) for _ in range(8)])
    codigos = [x.http if isinstance(x, Resp) and x.rede else "erro" for x in res]
    problemas, avisos = [], []

    inesperados = [str(c) for c in codigos if c not in (200, 202, 404)]
    if inesperados:
        problemas.append(f"cancelamentos simultâneos devolveram {sorted(set(inesperados))}")
    if not any(c in (200, 202) for c in codigos):
        problemas.append("nenhum dos cancelamentos foi aceito")

    # o assento tem de estar livre nas três empresas: outro passageiro reserva o mesmo itinerário
    for tentativa in range(6):
        r2 = reservar(amb, met, u2, "skyrim", itin, args.timeout)
        if r2.http == 201:
            if tentativa > 0:
                avisos.append(f"o assento só ficou livre após {tentativa} nova(s) tentativa(s)")
            break
        time.sleep(1)
    else:
        problemas.append("o assento não foi liberado em todas as empresas depois do cancelamento")
    return problemas, avisos


def cenario_churn(amb, met, args, rng):
    """Todos reservam e cancelam logo em seguida, o tempo todo, disputando UM assento por trecho."""
    sk, cy = trecho(amb, "skyrim", 0, 1), trecho(amb, "cyrodiil", 2, 3)
    fim = time.monotonic() + args.duracao
    falhas_cancelamento = []

    def passageiro(uid, regiao, semente):
        sorteio = random.Random(semente)
        itin = ordenar(regiao, [sk, cy])
        ganhos = 0
        while time.monotonic() < fim:
            r = reservar(amb, met, uid, regiao, itin, args.timeout)
            if r.http == 201:
                ganhos += 1
                c = cancelar(amb, met, uid, regiao, r.json.get("id_transacao"), args.timeout)
                if not (c.rede and c.http in (200, 202)):
                    falhas_cancelamento.append(f"HTTP {c.http}" if c.rede else "sem resposta")
            else:
                time.sleep(sorteio.uniform(0, 0.05))
        return ganhos

    semente = rng.randrange(1 << 30)
    res = disparar([(lambda u=u, r=r, i=i: passageiro(u, r, semente + i))
                    for i, (u, r) in enumerate(amb.usuarios)])
    excecoes = [x for x in res if not isinstance(x, int)]
    total = sum(x for x in res if isinstance(x, int))
    problemas, avisos = [], []

    if excecoes:
        problemas.append(f"exceção no teste: {excecoes[0]!r}")
    if total == 0:
        problemas.append("nenhuma reserva foi confirmada durante o cenário (o assento nunca foi liberado?)")
    if falhas_cancelamento:
        problemas.append(f"{len(falhas_cancelamento)} cancelamento(s) de reserva confirmada falharam: "
                         f"{sorted(set(falhas_cancelamento))}")
    avisos.append(f"{total} reservas confirmadas e canceladas no assento disputado")
    return problemas, avisos


def cenario_exclusao_carona(amb, met, args, rng):
    """Passageiros reservam sem parar na carona do skyrim enquanto o motorista a exclui."""
    sk, mo = trecho(amb, "skyrim", 0, 1), trecho(amb, "morrowind", 1, 2)
    id_carona = sk["id_carona"]
    motorista = (f"mot-skyrim-{amb.tag}", SENHA)
    fim = time.monotonic() + args.duracao
    estado = {"t_exclusao": None}
    tardias = []   # reservas confirmadas DEPOIS de a exclusão ter sido concluída

    def passageiro(uid, regiao):
        itin = ordenar(regiao, [sk, mo])
        while time.monotonic() < fim:
            t0 = time.monotonic()
            r = reservar(amb, met, uid, regiao, itin, args.timeout)
            if r.http == 201 and estado["t_exclusao"] and t0 > estado["t_exclusao"]:
                tardias.append(r.json.get("id_transacao"))
            if r.http != 201:
                time.sleep(0.05)

    def excluir():
        time.sleep(args.duracao * 0.3)
        ultima = None
        limite = time.monotonic() + 30
        while time.monotonic() < limite:
            ultima = http("DELETE", "skyrim", f"/api/cliente/caronas/{id_carona}", usuario=motorista,
                          host=amb.host, timeout=60)
            if ultima.rede and ultima.http == 200:
                estado["t_exclusao"] = time.monotonic()
                break
            time.sleep(0.5)   # 202 (empresa fora do ar) ou 409 (reserva entrou no meio): tenta de novo
        return ultima

    res = disparar([(lambda u=u, r=r: passageiro(u, r)) for u, r in amb.usuarios] + [excluir])
    exclusao = res[-1]
    problemas, avisos = [], []

    if not (isinstance(exclusao, Resp) and exclusao.rede and exclusao.http == 200):
        problemas.append(f"a exclusão da carona não terminou com 200: {exclusao!r}")
    if tardias:
        problemas.append(f"{len(tardias)} reserva(s) confirmada(s) depois de a carona ser excluída")

    # toda reserva que usa a carona excluída deve estar CANCELADA_MOTORISTA em TODAS as empresas
    estabilizar(args.host, args.espera, log=lambda *a, **k: None)
    dados, _ = coletar(amb.host)
    meus = {u for u, _ in amb.usuarios}
    estados = defaultdict(dict)
    envolvidas = set()
    for regiao, reservas in dados.items():
        for x in reservas:
            if x.get("passageiro") not in meus:
                continue
            estados[x["id_transacao"]][regiao] = x.get("estado")
            partes = list(x.get("trechos", [])) + list(x.get("itinerario") or [])
            if any(t.get("id_carona") == id_carona for t in partes):
                envolvidas.add(x["id_transacao"])

    for id_tx in sorted(envolvidas):
        if any(e != "CANCELADA_MOTORISTA" for e in estados[id_tx].values()):
            problemas.append(f"reserva {id_tx[:8]} da carona excluída ficou {dict(estados[id_tx])}")
    for id_tx in met.confirmadas:
        if id_tx not in estados:
            problemas.append(f"reserva {id_tx[:8]} confirmada ao passageiro sumiu depois da exclusão da carona")
    avisos.append(f"{len(envolvidas)} reserva(s) usavam a carona quando ela foi excluída")
    return problemas, avisos


CENARIOS = {
    "ultimo_assento": {
        "descricao": "N passageiros disputam o mesmo trecho ao mesmo tempo",
        "fn": cenario_ultimo_assento, "passageiros": None, "capacidade": None},
    "cruzada": {
        "descricao": "reservas segurando assentos em ordens opostas (risco de deadlock)",
        "fn": cenario_cruzada, "passageiros": 6, "capacidade": 1},
    "cancelamento_duplo": {
        "descricao": "vários DELETE simultâneos da mesma reserva",
        "fn": cenario_cancelamento_duplo, "passageiros": 4, "capacidade": 1},
    "churn_um_assento": {
        "descricao": "reservar e cancelar sem parar disputando um assento",
        "fn": cenario_churn, "passageiros": None, "capacidade": 1},
    "exclusao_carona": {
        "descricao": "motorista exclui a carona enquanto passageiros reservam",
        "fn": cenario_exclusao_carona, "passageiros": None, "capacidade": 6,
        "ignorar": ("durabilidade",)},   # reservas confirmadas viram CANCELADA_MOTORISTA de propósito
}


# ----------------------------------------------------------------------------
# Execução
# ----------------------------------------------------------------------------

def rodar_rodada(nome, cfg, rodada, args, semente):
    linhas = []
    log = linhas.append
    rng = random.Random(semente)
    passageiros = cfg["passageiros"] or args.passageiros
    capacidade = cfg["capacidade"] or (1 + rodada % 3)

    try:
        amb = preparar(args.host, passageiros, capacidade, log=log)
    except RuntimeError as e:
        print(f"  rodada {rodada}: FALHOU\n    [PROBLEMA] preparação: {e}")
        return {"cenario": nome, "rodada": rodada, "aprovado": False, "problemas": [f"preparação: {e}"]}

    met = Metricas()
    monitor = Monitor(amb)
    monitor.start()
    inicio = time.monotonic()
    try:
        problemas, avisos = cfg["fn"](amb, met, args, rng)
    except Exception as e:  # noqa: BLE001
        problemas, avisos = [f"exceção no cenário: {type(e).__name__}: {e}"], []
    finally:
        monitor.parar.set()
        monitor.join(5)
    duracao = time.monotonic() - inicio

    estabilizou, tempo = estabilizar(args.host, args.espera, log=log)
    resultado = auditar(amb, met)
    ignorar = cfg.get("ignorar", ())
    resultado["violacoes"] = [v for v in resultado["violacoes"] if v[0] not in ignorar]
    dados = relatorio(f"{nome} · rodada {rodada} · {passageiros} passageiros, {capacidade} assento(s)/trecho",
                      met, duracao, estabilizou, tempo, resultado, log=log)

    problemas = list(problemas) + [f"monitor: {t}" for t in monitor.violacoes]
    aprovado = dados["aprovado"] and not problemas
    dados.update(cenario=nome, rodada=rodada, aprovado=aprovado, problemas=problemas,
                 avisos=avisos, leituras_monitor=monitor.leituras)

    print(f"  rodada {rodada}: {'OK' if aprovado else 'FALHOU'}  "
          f"({monitor.leituras} leituras do monitor, {duracao:.1f}s)")
    if not aprovado or args.verbose:
        print("\n".join(linhas))
    for p in problemas:
        print(f"    [PROBLEMA] {p}")
    for a in avisos:
        print(f"    aviso: {a}")
    return dados


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="localhost", help="host dos servidores (padrão: localhost)")
    ap.add_argument("--cenarios", nargs="+", choices=list(CENARIOS), default=list(CENARIOS),
                    help="cenários a rodar (padrão: todos)")
    ap.add_argument("--rodadas", type=int, default=3, help="rodadas por cenário, cada uma com ambiente novo (padrão: 3)")
    ap.add_argument("--passageiros", type=int, default=12,
                    help="passageiros nos cenários de tamanho variável (padrão: 12)")
    ap.add_argument("--duracao", type=float, default=10, help="duração dos cenários contínuos, em s (padrão: 10)")
    ap.add_argument("--timeout", type=float, default=20, help="timeout de cada requisição, em s (padrão: 20)")
    ap.add_argument("--espera", type=float, default=60, help="máximo de s esperando a recuperação (padrão: 60)")
    ap.add_argument("--semente", type=int, default=None, help="semente aleatória (para repetir uma execução)")
    ap.add_argument("--saida", default=None, help="grava os resultados em um arquivo JSON")
    ap.add_argument("--verbose", action="store_true", help="imprime o relatório completo de todas as rodadas")
    args = ap.parse_args()

    if args.rodadas < 1 or args.passageiros < 4:
        ap.error("valores inválidos (use --rodadas >= 1 e --passageiros >= 4)")

    for regiao in REGIOES:
        if not http("GET", regiao, "/api/consorcio/health", host=args.host, timeout=3).rede:
            print(f"O servidor {regiao} não responde em {args.host}. Suba os três antes de rodar este teste.")
            return 2

    semente = args.semente if args.semente is not None else random.randrange(1 << 30)
    print(f"Semente: {semente} (use --semente {semente} para repetir)")

    resultados = []
    for n, nome in enumerate(args.cenarios):
        print(f"\n{'=' * 78}\nCENÁRIO {nome}: {CENARIOS[nome]['descricao']}")
        for rodada in range(1, args.rodadas + 1):
            resultados.append(rodar_rodada(nome, CENARIOS[nome], rodada, args, semente + n * 1000 + rodada))

    print(f"\n{'=' * 78}\nRESUMO\n{'-' * 78}")
    print(f"{'cenário':<24}{'rodadas':>9}{'aprovadas':>11}   resultado")
    for nome in args.cenarios:
        rs = [r for r in resultados if r["cenario"] == nome]
        ok = sum(1 for r in rs if r["aprovado"])
        print(f"{nome:<24}{len(rs):>9}{ok:>11}   {'APROVADO' if ok == len(rs) else 'REPROVADO'}")

    if args.saida:
        os.makedirs(os.path.dirname(os.path.abspath(args.saida)), exist_ok=True)
        with open(args.saida, "w", encoding="utf-8") as f:
            json.dump({"semente": semente, "rodadas": resultados}, f, ensure_ascii=False, indent=2)
        print(f"\nResultados gravados em {args.saida}")

    return 0 if all(r["aprovado"] for r in resultados) else 1


if __name__ == "__main__":
    sys.exit(main())
    