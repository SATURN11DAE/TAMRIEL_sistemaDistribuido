#!/usr/bin/env python3
"""
carga.py - Simulador de passageiros com carga alta e contínua.

Sobe N passageiros virtuais, espalhados pelos três servidores (skyrim, morrowind
e cyrodiil). Cada um fica em laço durante --duracao segundos: busca itinerários,
reserva um deles e, com probabilidade --cancelar, cancela. Como há mais
passageiros que assentos, as disputas pelos últimos assentos são constantes,
e os cancelamentos devolvem assentos para a briga continuar.

Ao final espera a recuperação e confere as invariantes lendo o estado de cada
servidor (exclusão mútua, overbooking, atomicidade, durabilidade, cancelamentos).

Pré-requisito: os três servidores já rodando (veja o README):
    ./servidor skyrim ; ./servidor morrowind ; ./servidor cyrodiil

Uso:
    python3 testes/carga.py
    python3 testes/carga.py --passageiros 60 --duracao 60 --capacidade 15
    python3 testes/carga.py --host 192.168.0.10 --saida resultados/carga.json

Código de saída: 0 se todas as invariantes se mantiveram, 1 caso contrário.
"""

import argparse
import json
import os
import sys

from comum import estabilizar, auditar, gerar_carga, Metricas, preparar, relatorio


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="localhost", help="host dos servidores (padrão: localhost)")
    ap.add_argument("--passageiros", type=int, default=30, help="passageiros virtuais simultâneos (padrão: 30)")
    ap.add_argument("--duracao", type=float, default=30, help="duração da carga em segundos (padrão: 30)")
    ap.add_argument("--capacidade", type=int, default=10, help="assentos por trecho de cada carona (padrão: 10)")
    ap.add_argument("--cancelar", type=float, default=0.5, help="chance de cancelar após reservar, 0 a 1 (padrão: 0.5)")
    ap.add_argument("--pensar", type=int, default=0, help="pausa entre operações de um passageiro, em ms (padrão: 0)")
    ap.add_argument("--timeout", type=float, default=30, help="timeout de cada requisição, em s (padrão: 30)")
    ap.add_argument("--espera", type=float, default=60, help="máximo de s esperando a recuperação (padrão: 60)")
    ap.add_argument("--semente", type=int, default=None, help="semente aleatória (para repetir uma execução)")
    ap.add_argument("--saida", default=None, help="grava o resultado também neste arquivo JSON")
    args = ap.parse_args()

    if not 0 <= args.cancelar <= 1 or args.passageiros < 1 or args.capacidade < 1:
        ap.error("valores inválidos")

    print(f"Preparando a rodada em {args.host} ...")
    try:
        amb = preparar(args.host, args.passageiros, args.capacidade)
    except RuntimeError as e:
        print(f"\nERRO na preparação: {e}\nOs três servidores estão rodando?")
        return 2

    print(f"\nCarga: {args.passageiros} passageiros por {args.duracao:.0f}s "
          f"({args.capacidade} assentos por trecho, cancelar={args.cancelar})")
    met = Metricas()
    duracao = gerar_carga(amb, args.duracao, met, args.cancelar, args.pensar, args.timeout, args.semente)

    print("Carga encerrada; esperando a recuperação e conferindo o estado ...")
    estabilizou, tempo = estabilizar(args.host, args.espera)
    resultado = auditar(amb, met)
    dados = relatorio(f"Carga contínua ({args.passageiros} passageiros, {args.capacidade} assentos/trecho)",
                      met, duracao, estabilizou, tempo, resultado)

    if args.saida:
        os.makedirs(os.path.dirname(os.path.abspath(args.saida)), exist_ok=True)
        with open(args.saida, "w", encoding="utf-8") as f:
            json.dump(dados, f, ensure_ascii=False, indent=2)
        print(f"\nResultado gravado em {args.saida}")

    return 0 if dados["aprovado"] else 1


if __name__ == "__main__":
    sys.exit(main())
