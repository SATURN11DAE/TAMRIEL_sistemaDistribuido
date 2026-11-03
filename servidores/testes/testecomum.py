#!/usr/bin/env python3
"""
test_comum.py - testes unitários do comum.py. NÃO precisam de servidores.

Por que existe: o auditar() é o "juiz" dos testes de carga e de falhas. Se ele tiver um bug,
os testes aprovam sem conferir nada. Aqui cada violação é fabricada à mão e o teste verifica
que o auditar() a detecta (e que dados corretos NÃO geram falso alarme).

Uso (da raiz do projeto):
    python3 -m unittest testes/test_comum.py -v
    python3 testes/test_comum.py
"""

import http.server
import json
import os
import sys
import threading
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import comum  # noqa: E402

CARONA_SK, CARONA_MO = "skyrim-c1", "morrowind-c1"


def ambiente(capacidade=2):
    """Duas caronas de 2 trechos (índices 0 e 1) e dois passageiros."""
    return comum.Ambiente(
        host="localhost", tag="t", capacidade=capacidade, cidades=["A", "B", "C", "D"],
        usuarios=[("ana", "skyrim"), ("bia", "morrowind")],
        caronas={CARONA_SK: {"regiao": "skyrim", "capacidades": [capacidade] * 2},
                 CARONA_MO: {"regiao": "morrowind", "capacidades": [capacidade] * 2}},
        consultas={})


def trecho(servidor, carona, de, ate, assento):
    return {"servidor": servidor, "id_carona": carona, "origem": "x", "destino": "y",
            "de": de, "ate": ate, "assento": assento}


def reserva(tx, regiao, estado="COMITADO", passageiro="ana", coord="skyrim", locais=None, itin=None):
    return {"id_transacao": tx, "estado": estado, "coordenador_id": coord, "passageiro": passageiro,
            "trechos": locais or [], "itinerario": itin or []}


def itinerario_sk_mo(assento_sk=1, assento_mo=1):
    return [trecho("skyrim", CARONA_SK, 0, 1, assento_sk), trecho("morrowind", CARONA_MO, 0, 1, assento_mo)]


def par_confirmado(tx, passageiro="ana", assento_sk=1, assento_mo=1):
    """A mesma reserva COMITADO nos dois servidores, como o 2PC deixa."""
    itin = itinerario_sk_mo(assento_sk, assento_mo)
    return {
        "skyrim": [reserva(tx, "skyrim", passageiro=passageiro, locais=[itin[0]], itin=itin)],
        "morrowind": [reserva(tx, "morrowind", passageiro=passageiro, locais=[itin[1]])],
    }


def juntar(*partes):
    """Une dicionários {regiao: [reservas]}."""
    saida = {"skyrim": [], "morrowind": [], "cyrodiil": []}
    for p in partes:
        for regiao, lista in p.items():
            saida[regiao].extend(lista)
    return saida


def auditar_com(dados, met=None, amb=None, falhas=()):
    met = met or comum.Metricas()
    amb = amb or ambiente()
    with mock.patch.object(comum, "coletar", return_value=(dados, list(falhas))):
        return comum.auditar(amb, met)


def categorias(resultado):
    return sorted({c for c, _ in resultado["violacoes"]})


def confirmada(met, tx, regiao="skyrim"):
    met.confirmadas[tx] = regiao


# ----------------------------------------------------------------------------

class AuditarSemViolacao(unittest.TestCase):
    def test_estado_vazio_e_consistente(self):
        self.assertEqual(categorias(auditar_com(juntar())), [])

    def test_reserva_confirmada_nas_duas_empresas(self):
        met = comum.Metricas()
        confirmada(met, "tx1")
        r = auditar_com(juntar(par_confirmado("tx1")), met)
        self.assertEqual(r["violacoes"], [])
        self.assertEqual(r["resumo"]["comitadas"], 1)

    def test_mesmo_assento_em_trechos_diferentes_nao_e_conflito(self):
        # assento 1 do trecho 0 e assento 1 do trecho 1: faixas [0,1) e [1,2) não se sobrepõem
        a = par_confirmado("tx1", "ana")
        b = {"skyrim": [reserva("tx2", "skyrim", passageiro="bia",
                                locais=[trecho("skyrim", CARONA_SK, 1, 2, 1)],
                                itin=[trecho("skyrim", CARONA_SK, 1, 2, 1)])]}
        met = comum.Metricas()
        confirmada(met, "tx1")
        confirmada(met, "tx2")
        self.assertEqual(categorias(auditar_com(juntar(a, b), met, ambiente(2))), [])

    def test_reserva_cancelada_pelo_motorista_nao_ocupa_assento(self):
        a = par_confirmado("tx1", "ana", assento_sk=1)
        b = par_confirmado("tx2", "bia", assento_sk=1)      # mesmo assento que tx1
        for lista in b.values():
            for x in lista:
                x["estado"] = "CANCELADA_MOTORISTA"
        r = auditar_com(juntar(a, b), met=_com(["tx1"]))
        self.assertNotIn("exclusao", categorias(r))

    def test_reservas_de_outros_passageiros_sao_ignoradas(self):
        # reserva de um usuário que não é deste ambiente (rodada antiga) não conta
        r = auditar_com(juntar(par_confirmado("tx9", passageiro="estranho", assento_sk=1)))
        self.assertEqual(r["violacoes"], [])


def _com(ids):
    met = comum.Metricas()
    for i in ids:
        confirmada(met, i)
    return met


class AuditarDetectaViolacoes(unittest.TestCase):
    def test_assento_vendido_duas_vezes(self):
        dados = juntar(par_confirmado("tx1", "ana", assento_sk=1), par_confirmado("tx2", "bia", assento_sk=1))
        r = auditar_com(dados, _com(["tx1", "tx2"]))
        self.assertIn("exclusao", categorias(r))

    def test_overbooking_mais_reservas_que_assentos(self):
        # capacidade 1, duas reservas no mesmo trecho com assentos diferentes (1 e 2)
        dados = juntar(par_confirmado("tx1", "ana", assento_sk=1), par_confirmado("tx2", "bia", assento_sk=2))
        r = auditar_com(dados, _com(["tx1", "tx2"]), ambiente(1))
        self.assertIn("overbooking", categorias(r))

    def test_assento_acima_da_capacidade(self):
        r = auditar_com(juntar(par_confirmado("tx1", assento_sk=5)), _com(["tx1"]), ambiente(2))
        self.assertIn("overbooking", categorias(r))

    def test_reserva_presa_em_preparado(self):
        dados = par_confirmado("tx1")
        dados["morrowind"][0]["estado"] = "PREPARADO"
        r = auditar_com(juntar(dados))
        self.assertIn("pendente", categorias(r))

    def test_reserva_presa_em_cancelando(self):
        dados = par_confirmado("tx1")
        dados["skyrim"][0]["estado"] = "CANCELANDO"
        self.assertIn("pendente", categorias(auditar_com(juntar(dados))))

    def test_reserva_orfa_participante_sem_coordenador(self):
        # é exatamente o que o use-after-free do txPrepare produzia
        itin = itinerario_sk_mo()
        dados = {"morrowind": [reserva("tx1", "morrowind", estado="PREPARADO", coord="skyrim", locais=[itin[1]])]}
        r = auditar_com(juntar(dados))
        self.assertIn("atomicidade", categorias(r))
        self.assertIn("pendente", categorias(r))

    def test_confirmada_pela_metade(self):
        dados = par_confirmado("tx1")
        dados["morrowind"] = []                             # o participante perdeu a reserva
        r = auditar_com(juntar(dados), _com(["tx1"]))
        self.assertIn("atomicidade", categorias(r))

    def test_participante_ainda_nao_comitado(self):
        dados = par_confirmado("tx1")
        dados["morrowind"][0]["estado"] = "PREPARADO"
        self.assertIn("atomicidade", categorias(auditar_com(juntar(dados), _com(["tx1"]))))

    def test_copia_fora_do_itinerario(self):
        dados = par_confirmado("tx1")
        dados["cyrodiil"] = [reserva("tx1", "cyrodiil")]
        self.assertIn("atomicidade", categorias(auditar_com(juntar(dados), _com(["tx1"]))))

    def test_201_ao_passageiro_mas_nada_no_disco(self):
        r = auditar_com(juntar(), _com(["tx-sumiu"]))
        self.assertIn("durabilidade", categorias(r))

    def test_cancelada_mas_ainda_existe(self):
        met = comum.Metricas()
        met.canceladas["tx1"] = 200
        r = auditar_com(juntar(par_confirmado("tx1")), met)
        self.assertIn("cancelamento", categorias(r))

    def test_confirmacao_que_o_passageiro_nao_conhece(self):
        # comitada em disco, nenhuma requisição ficou sem resposta: não há explicação para ela existir
        r = auditar_com(juntar(par_confirmado("tx1")), comum.Metricas())
        self.assertIn("fantasma", categorias(r))

    def test_confirmacao_sem_resposta_e_explicada_por_incerta(self):
        met = comum.Metricas()
        met.incertas = 1                                    # houve uma reserva sem resposta (timeout)
        r = auditar_com(juntar(par_confirmado("tx1")), met)
        self.assertEqual(r["violacoes"], [])
        self.assertTrue(r["avisos"])

    def test_servidor_fora_do_ar_na_auditoria(self):
        dados = {"skyrim": [], "morrowind": []}
        r = auditar_com(dados, falhas=["cyrodiil"])
        self.assertIn("servidor", categorias(r))


class Estabilizar(unittest.TestCase):
    def test_estabiliza_quando_nao_ha_pendentes(self):
        with mock.patch.object(comum, "coletar", return_value=({"skyrim": []}, [])):
            ok, _ = comum.estabilizar("localhost", timeout=2, log=lambda *a: None)
        self.assertTrue(ok)

    def test_espera_a_recuperacao_terminar(self):
        pendente = ({"skyrim": [reserva("tx1", "skyrim", estado="PREPARADO")]}, [])
        limpo = ({"skyrim": []}, [])
        with mock.patch.object(comum, "coletar", side_effect=[pendente, pendente, limpo]), \
                mock.patch.object(comum.time, "sleep"):
            ok, _ = comum.estabilizar("localhost", timeout=60, log=lambda *a: None)
        self.assertTrue(ok)

    def test_desiste_no_timeout_com_pendente(self):
        pendente = ({"skyrim": [reserva("tx1", "skyrim", estado="CANCELANDO")]}, [])
        relogio = iter([0, 0, 5, 5, 11, 11, 20, 20])
        with mock.patch.object(comum, "coletar", return_value=pendente), \
                mock.patch.object(comum.time, "monotonic", side_effect=lambda: next(relogio)), \
                mock.patch.object(comum.time, "sleep"):
            ok, _ = comum.estabilizar("localhost", timeout=10, log=lambda *a: None)
        self.assertFalse(ok)

    def test_servidor_sem_resposta_nao_estabiliza(self):
        with mock.patch.object(comum, "coletar", return_value=({"skyrim": []}, ["cyrodiil"])), \
                mock.patch.object(comum.time, "sleep"), \
                mock.patch.object(comum.time, "monotonic", side_effect=[0, 0, 100, 100]):
            ok, _ = comum.estabilizar("localhost", timeout=10, log=lambda *a: None)
        self.assertFalse(ok)


class Estatistica(unittest.TestCase):
    def test_percentil(self):
        v = list(range(1, 101))
        self.assertEqual(comum.percentil(v, 50), 50)
        self.assertEqual(comum.percentil(v, 95), 95)
        self.assertEqual(comum.percentil(v, 100), 100)
        self.assertEqual(comum.percentil([], 50), 0.0)
        self.assertEqual(comum.percentil([7], 99), 7)

    def test_estatisticas_contam_codigos(self):
        met = comum.Metricas()
        for chave, ms in [(201, 10), (201, 20), (409, 30), ("rede", 40)]:
            met.amostras["reserva"].append((chave, ms))
        e = comum.estatisticas(met, 2.0)["reserva"]
        self.assertEqual(e["total"], 4)
        self.assertEqual(e["por_segundo"], 2.0)
        self.assertEqual(e["codigos"], {"201": 2, "409": 1, "rede": 1})
        self.assertEqual(e["max_ms"], 40)


class FalsoServidor(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _responder(self, codigo, corpo):
        dados = json.dumps(corpo).encode() if corpo is not None else b"nao-e-json"
        self.send_response(codigo)
        self.end_headers()
        self.wfile.write(dados)

    def do_GET(self):
        if self.path.endswith("/ok"):
            self._responder(200, {"a": 1})
        elif self.path.endswith("/404"):
            self._responder(404, {"erro": "nao"})
        elif self.path.endswith("/lixo"):
            self._responder(200, None)
        else:
            self._responder(200, [1, 2])                    # JSON que não é objeto

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        corpo = json.loads(self.rfile.read(n))
        auth = self.headers.get("Authorization", "")
        self._responder(201, {"eco": corpo, "auth": auth})


class ClienteHttp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.servidor = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FalsoServidor)
        threading.Thread(target=cls.servidor.serve_forever, daemon=True).start()
        cls.porta = cls.servidor.server_address[1]
        comum.PORTAS["teste"] = cls.porta

    @classmethod
    def tearDownClass(cls):
        cls.servidor.shutdown()
        comum.PORTAS.pop("teste", None)

    def pedir(self, *a, **k):
        return comum.http(*a, host="127.0.0.1", timeout=3, **k)

    def test_200_com_json(self):
        r = self.pedir("GET", "teste", "/ok")
        self.assertTrue(r.rede)
        self.assertEqual((r.http, r.json), (200, {"a": 1}))

    def test_codigo_de_erro_nao_vira_excecao(self):
        r = self.pedir("GET", "teste", "/404")
        self.assertTrue(r.rede)
        self.assertEqual((r.http, r.json), (404, {"erro": "nao"}))

    def test_corpo_que_nao_e_json(self):
        r = self.pedir("GET", "teste", "/lixo")
        self.assertEqual((r.http, r.json), (200, {}))

    def test_json_que_nao_e_objeto_vira_dicionario_vazio(self):
        self.assertEqual(self.pedir("GET", "teste", "/outro").json, {})

    def test_post_envia_json_e_basic_auth(self):
        r = self.pedir("POST", "teste", "/x", {"k": "v"}, usuario=("ana", "1234"))
        self.assertEqual(r.http, 201)
        self.assertEqual(r.json["eco"], {"k": "v"})
        self.assertEqual(r.json["auth"], "Basic YW5hOjEyMzQ=")      # base64("ana:1234")

    def test_conexao_recusada_vira_rede_falsa(self):
        comum.PORTAS["morto"] = 1                                   # porta onde ninguém escuta
        try:
            r = self.pedir("GET", "morto", "/ok")
        finally:
            comum.PORTAS.pop("morto")
        self.assertFalse(r.rede)
        self.assertEqual(r.http, 0)
        self.assertEqual(r.chave, "rede")
        self.assertTrue(r.erro)


if __name__ == "__main__":
    unittest.main(verbosity=2)
    