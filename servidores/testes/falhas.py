#!/usr/bin/env python3
"""
falhas.py - Carga contínua COMBINADA com injeção de falhas.

Para cada cenário, o script:
  1. sobe skyrim, morrowind e cyrodiil num diretório temporário limpo;
  2. cadastra motoristas e passageiros e liga a carga (a mesma do carga.py);
  3. durante a carga, derruba ou trava servidores (SIGKILL, SIGSTOP ou o
     INJETAR_FALHA do próprio servidor);
  4. ao fim da carga, religa tudo, espera a recuperação e confere as invariantes
     lendo o estado de cada servidor.

Cenários:
  participante_cai            SIGKILL no morrowind no meio da carga; ele volta 6 s depois
  participante_trava          SIGSTOP no cyrodiil por 6 s (travado, não caído); depois SIGCONT
  coordenador_antes_decisao   skyrim cai após os votos READY e antes de gravar o COMMIT
  coordenador_depois_decisao  skyrim cai após gravar o COMMIT e antes de avisar os participantes
  cancelamento_interrompido   skyrim cai após gravar CANCELANDO, antes de propagar
  caos                        a cada poucos segundos, um servidor aleatório cai ou trava

Uso (da raiz do projeto, depois de compilar com `make`):
    python3 testes/falhas.py
    python3 testes/falhas.py --cenarios participante_cai caos --duracao 40
    python3 testes/falhas.py --binario ./servidor --saida resultados/falhas.json

Código de saída: 0 se todos os cenários mantiveram as invariantes, 1 caso contrário.
"""

import argparse
import json
import os
import random
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

from comum import (PORTAS, REGIOES, Metricas, auditar, estabilizar, gerar_carga, http,
                   preparar, relatorio)


# ----------------------------------------------------------------------------
# Controle dos processos
# ----------------------------------------------------------------------------

class Frota:
    """Os três servidores deste cenário, com controle de queda, trava e religamento."""

    def __init__(self, binario, diretorio):
        self.binario = os.path.abspath(binario)
        self.diretorio = diretorio
        self.processos = {}
        self.parados = set()
        self.trava = threading.Lock()

    def iniciar(self, regiao, injetar=None):
        env = dict(os.environ)
        env.pop("INJETAR_FALHA", None)
        if injetar:
            env["INJETAR_FALHA"] = injetar
        log = open(os.path.join(self.diretorio, f"{regiao}.log"), "ab")
        with self.trava:
            self.processos[regiao] = subprocess.Popen(
                [self.binario, regiao], cwd=self.diretorio, env=env,
                stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
            self.parados.discard(regiao)
        log.close()

    def vivo(self, regiao):
        p = self.processos.get(regiao)
        return p is not None and p.poll() is None

    def matar(self, regiao):
        with self.trava:
            p = self.processos.get(regiao)
            if p and p.poll() is None:
                p.send_signal(signal.SIGKILL)
                p.wait()
            self.parados.discard(regiao)

    def travar(self, regiao):
        with self.trava:
            if self.vivo(regiao):
                self.processos[regiao].send_signal(signal.SIGSTOP)
                self.parados.add(regiao)

    def destravar(self, regiao):
        with self.trava:
            if self.vivo(regiao) and regiao in self.parados:
                self.processos[regiao].send_signal(signal.SIGCONT)
            self.parados.discard(regiao)

    def aguardar_saude(self, regiao, timeout=20):
        limite = time.monotonic() + timeout
        while time.monotonic() < limite:
            if self.vivo(regiao) and http("GET", regiao, "/api/consorcio/health", timeout=1).http == 200:
                return True
            time.sleep(0.2)
        return False

    def curar(self):
        """Deixa os três servidores rodando, sem injeção e sem estar travados."""
        for regiao in REGIOES:
            self.destravar(regiao)
            if not self.vivo(regiao):
                self.iniciar(regiao)
        return all(self.aguardar_saude(r) for r in REGIOES)

    def encerrar(self):
        for regiao in REGIOES:
            self.destravar(regiao)
            p = self.processos.get(regiao)
            if p and p.poll() is None:
                p.kill()
                p.wait()


# ----------------------------------------------------------------------------
# Cenários: cada ação roda numa thread durante a carga
# ----------------------------------------------------------------------------

def espera(segundos, parar):
    """Dorme até 'segundos' ou até a carga acabar. Retorna False se a carga acabou."""
    return not parar.wait(segundos)


def acao_participante_cai(frota, parar, duracao, rng, log):
    if espera(duracao * 0.25, parar):
        log("  >> SIGKILL em morrowind")
        frota.matar("morrowind")
        if espera(6, parar):
            log("  >> morrowind volta")
            frota.iniciar("morrowind")
            return {"disparou": True}
        return {"disparou": True}   # a queda aconteceu; a carga acabou antes do religamento
    return {"disparou": False}


def acao_participante_trava(frota, parar, duracao, rng, log):
    if espera(duracao * 0.25, parar):
        log("  >> SIGSTOP em cyrodiil")
        frota.travar("cyrodiil")
        espera(6, parar)
        log("  >> cyrodiil destravado (SIGCONT)")
        frota.destravar("cyrodiil")
        return {"disparou": True}
    return {"disparou": False}


def fabrica_acao_injecao(regiao):
    """O servidor encerra sozinho (INJETAR_FALHA); esta ação o religa 3 s depois, sem injeção."""
    def acao(frota, parar, duracao, rng, log):
        while not parar.is_set():
            if not frota.vivo(regiao):
                log(f"  >> {regiao} caiu pela injeção; religando em 3 s")
                espera(3, parar)
                frota.iniciar(regiao)
                return {"disparou": True}
            time.sleep(0.1)
        return {"disparou": False}
    return acao


def acao_caos(frota, parar, duracao, rng, log):
    """Um servidor por vez cai (SIGKILL + religa) ou trava (SIGSTOP + SIGCONT)."""
    ocorrencias = 0
    while espera(rng.uniform(3, 6), parar):
        regiao = rng.choice(REGIOES)
        pausa = rng.uniform(2, 5)
        if rng.random() < 0.5:
            log(f"  >> SIGKILL em {regiao} por {pausa:.1f}s")
            frota.matar(regiao)
            espera(pausa, parar)
            frota.iniciar(regiao)
        else:
            log(f"  >> SIGSTOP em {regiao} por {pausa:.1f}s")
            frota.travar(regiao)
            espera(pausa, parar)
            frota.destravar(regiao)
        ocorrencias += 1
    return {"disparou": ocorrencias > 0}


CENARIOS = {
    "participante_cai": {
        "descricao": "SIGKILL no morrowind (participante) no meio da carga",
        "acao": acao_participante_cai, "injetar": {}, "cancelar": 0.5},
    "participante_trava": {
        "descricao": "SIGSTOP no cyrodiil: servidor travado, não caído",
        "acao": acao_participante_trava, "injetar": {}, "cancelar": 0.5},
    "coordenador_antes_decisao": {
        "descricao": "skyrim cai depois dos votos READY e antes de gravar o COMMIT",
        "acao": fabrica_acao_injecao("skyrim"), "injetar": {"skyrim": "antes_decisao"}, "cancelar": 0.5},
    "coordenador_depois_decisao": {
        "descricao": "skyrim cai depois de gravar o COMMIT e antes de avisar os participantes",
        "acao": fabrica_acao_injecao("skyrim"), "injetar": {"skyrim": "depois_decisao"}, "cancelar": 0.5},
    "cancelamento_interrompido": {
        "descricao": "skyrim cai depois de gravar CANCELANDO, antes de propagar",
        "acao": fabrica_acao_injecao("skyrim"), "injetar": {"skyrim": "depois_cancelando"}, "cancelar": 0.9},
    "caos": {
        "descricao": "servidores aleatórios caem ou travam ao longo da carga",
        "acao": acao_caos, "injetar": {}, "cancelar": 0.5},
}


# ----------------------------------------------------------------------------
# Execução de um cenário
# ----------------------------------------------------------------------------

def executar_cenario(nome, args, semente):
    cenario = CENARIOS[nome]
    rng = random.Random(semente)
    diretorio = tempfile.mkdtemp(prefix=f"vaijunto-{nome}-")
    frota = Frota(args.binario, diretorio)
    print(f"\n{'=' * 78}\nCENÁRIO {nome}: {cenario['descricao']}\n  dados e logs em {diretorio}")

    try:
        for regiao in REGIOES:
            frota.iniciar(regiao, cenario["injetar"].get(regiao))
        if not all(frota.aguardar_saude(r) for r in REGIOES):
            return {"titulo": nome, "aprovado": False, "motivo": "servidores não subiram", "diretorio": diretorio}

        # prepara sem a injeção atrapalhar: a falha só vale depois que a carga começa
        amb = preparar("localhost", args.passageiros, args.capacidade)
        met = Metricas()

        parar = threading.Event()
        retorno = {}

        def rodar_acao():
            retorno.update(cenario["acao"](frota, parar, args.duracao, rng, print) or {})

        thread = threading.Thread(target=rodar_acao, daemon=True)
        thread.start()
        duracao = gerar_carga(amb, args.duracao, met, cenario["cancelar"], 0, args.timeout, semente)
        parar.set()
        thread.join(timeout=30)

        print("Carga encerrada; religando o que sobrou e esperando a recuperação ...")
        if not frota.curar():
            print("  !! algum servidor não voltou")
        estabilizou, tempo = estabilizar("localhost", args.espera)
        resultado = auditar(amb, met)

        disparou = retorno.get("disparou", False)
        if not disparou:
            resultado["avisos"].append("a falha deste cenário NÃO chegou a acontecer durante a carga "
                                       "(aumente --duracao/--passageiros); o teste não provou nada sobre ela")
        dados = relatorio(f"{nome}", met, duracao, estabilizou, tempo, resultado)
        dados["falha_disparou"] = disparou
        dados["diretorio"] = diretorio
        return dados
    finally:
        frota.encerrar()
        if not args.manter and "dados" in locals() and dados.get("aprovado"):
            shutil.rmtree(diretorio, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binario", default="./servidor", help="caminho do executável do servidor (padrão: ./servidor)")
    ap.add_argument("--cenarios", nargs="+", choices=list(CENARIOS), default=list(CENARIOS),
                    help="cenários a rodar (padrão: todos)")
    ap.add_argument("--passageiros", type=int, default=20, help="passageiros virtuais (padrão: 20)")
    ap.add_argument("--duracao", type=float, default=30, help="duração da carga por cenário, em s (padrão: 30)")
    ap.add_argument("--capacidade", type=int, default=8, help="assentos por trecho (padrão: 8)")
    ap.add_argument("--timeout", type=float, default=20, help="timeout de cada requisição, em s (padrão: 20)")
    ap.add_argument("--espera", type=float, default=90, help="máximo de s esperando a recuperação (padrão: 90)")
    ap.add_argument("--semente", type=int, default=None, help="semente aleatória (para repetir uma execução)")
    ap.add_argument("--manter", action="store_true", help="não apaga os diretórios de dados dos cenários aprovados")
    ap.add_argument("--saida", default=None, help="grava os resultados em um arquivo JSON")
    args = ap.parse_args()

    if not os.path.isfile(args.binario) or not os.access(args.binario, os.X_OK):
        print(f"Executável '{args.binario}' não encontrado. Compile com `make` ou use --binario.")
        return 2
    for regiao in REGIOES:
        if http("GET", regiao, "/api/consorcio/health", timeout=1).rede:
            print(f"A porta {PORTAS[regiao]} ({regiao}) já está em uso. Encerre os servidores antes: este teste sobe os seus.")
            return 2

    semente = args.semente if args.semente is not None else random.randrange(1 << 30)
    print(f"Semente: {semente} (use --semente {semente} para repetir)")

    resultados = [executar_cenario(nome, args, semente + i) for i, nome in enumerate(args.cenarios)]

    print(f"\n{'=' * 78}\nRESUMO\n{'-' * 78}")
    print(f"{'cenário':<30}{'falha ocorreu':>15}{'confirmadas':>13}{'violações':>11}   resultado")
    for r in resultados:
        confirmadas = r.get("operacoes", {}).get("reserva", {}).get("codigos", {}).get("201", 0)
        print(f"{r['titulo']:<30}{('sim' if r.get('falha_disparou') else 'NÃO'):>15}{confirmadas:>13}"
              f"{len(r.get('violacoes', [])):>11}   {'APROVADO' if r['aprovado'] else 'REPROVADO'}")
        if not r["aprovado"]:
            print(f"    logs dos servidores: {r.get('diretorio')}")

    if args.saida:
        os.makedirs(os.path.dirname(os.path.abspath(args.saida)), exist_ok=True)
        with open(args.saida, "w", encoding="utf-8") as f:
            json.dump({"semente": semente, "cenarios": resultados}, f, ensure_ascii=False, indent=2)
        print(f"\nResultados gravados em {args.saida}")

    return 0 if all(r["aprovado"] for r in resultados) else 1


if __name__ == "__main__":
    sys.exit(main())
    