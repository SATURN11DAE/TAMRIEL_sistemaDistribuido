/* =====================================================
   API – cliente HTTP do servidor do consórcio (C + libmicrohttpd)

   Cada função fala com /api/cliente/... do servidor da empresa
   escolhida na tela de login. A interface NÃO decide nada:
   atomicidade, assentos e 2PC são resolvidos no servidor.

   Formato de resposta mantido: { ok, mensagem, ...extra }
   Todas as funções são assíncronas (usar await).
   ===================================================== */
const API = (() => {

  let base = "";        // ex.: http://localhost:3001
  let auth = "";        // "Basic base64(id:senha)"
  let usuario = null;   // { id, nome, tipo, servidor }

  const resposta = (ok, mensagem, extra = {}) => ({ ok, mensagem, ...extra });


  /* ---------- Utilidades ---------- */

  const b64 = s => btoa(String.fromCharCode(...new TextEncoder().encode(s)));

  const normalizarURL = u => {
    u = (u || "").trim().replace(/\/+$/, "");
    return u ? (/^https?:\/\//i.test(u) ? u : "http://" + u) : "";
  };

  /* O servidor não tem campo "data": ela vai dentro do horário ("2026-11-01 08:00"). */
  const temData = h => /^\d{4}-\d{2}-\d{2}/.test(h || "");

  /* Faz a requisição. Nunca lança exceção: devolve { rede, http, json, erro }. */
  async function http(metodo, caminho, corpo, timeoutMs = 10000) {
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), timeoutMs);

    const headers = { Accept: "application/json" };
    if (auth) headers.Authorization = auth;
    if (corpo !== undefined) headers["Content-Type"] = "application/json";

    try {
      const r = await fetch(base + "/api/cliente" + caminho, {
        method: metodo,
        headers,
        signal: ctl.signal,
        body: corpo !== undefined ? JSON.stringify(corpo) : undefined
      });
      let json = {};
      try { json = (await r.json()) || {}; } catch (_) {}
      return { rede: true, http: r.status, json };
    } catch (e) {
      return {
        rede: false, http: 0, json: {},
        erro: e.name === "AbortError"
          ? "O servidor demorou demais para responder."
          : "Servidor inacessível em " + base + "."
      };
    } finally {
      clearTimeout(timer);
    }
  }

  const motivo = r =>
    !r.rede ? r.erro : (r.json.motivo || r.json.erro || "Erro HTTP " + r.http + ".");

  /* Guarda rota/preço da reserva no navegador (o servidor só guarda ids e assentos). */
  const CHAVE_META = "ride-scrolls-reservas";
  const lerMeta = () => { try { return JSON.parse(localStorage.getItem(CHAVE_META)) || {}; } catch (_) { return {}; } };
  const gravarMeta = (id, m) => {
    try { const x = lerMeta(); x[id] = m; localStorage.setItem(CHAVE_META, JSON.stringify(x)); } catch (_) {}
  };


  /* ---------- Usuários ---------- */

  async function login(tipo, id, senha, url) {
    base = normalizarURL(url);
    if (!base) return resposta(false, "Informe o servidor.");
    if (!id || !senha) return resposta(false, "Informe ID e senha.");

    auth = "Basic " + b64(id + ":" + senha);
    const r = await http("GET", "/eu");

    if (!r.rede)          { auth = ""; return resposta(false, r.erro); }
    if (r.http === 401)   { auth = ""; return resposta(false, "ID ou senha incorretos."); }
    if (r.http !== 200)   { auth = ""; return resposta(false, motivo(r)); }

    if (String(r.json.tipo).toUpperCase() !== tipo) {
      auth = "";
      return resposta(false, "Esta conta é de " + r.json.tipo + ". Troque o TIPO.");
    }
    usuario = r.json;
    return resposta(true, "Bem-vindo, " + usuario.nome + "! (servidor " + usuario.servidor + ")", { u: usuario });
  }

  async function cadastro(tipo, nome, id, senha, url) {
    base = normalizarURL(url);
    if (!base) return resposta(false, "Informe o servidor.");
    if (!nome || !id || !senha) return resposta(false, "Preencha todos os campos.");

    auth = "";   // cadastro não usa autenticação
    const r = await http("POST", "/usuarios", { id, nome, tipo: tipo.toLowerCase(), senha });

    if (r.http === 201) return resposta(true, "Cadastro feito em " + (r.json.servidor || "servidor") + ". Faça login.");
    return resposta(false, motivo(r));
  }

  function sair() {
    auth = "";
    usuario = null;
  }


  /* ---------- Motorista ---------- */

  /* Publica UMA carona com vários trechos (POST /api/cliente/rotas), como o terminal. */
  async function publicar(data, trechos) {
    if (!/^\d{4}-\d{2}-\d{2}$/.test(data))
      return resposta(false, "Use data AAAA-MM-DD.");
    if (!trechos.length)
      return resposta(false, "Adicione ao menos um trecho.");

    const r = await http("POST", "/rotas", {
      trechos: trechos.map(t => ({
        origem: t.origem, destino: t.destino,
        horario: data + " " + t.hora,
        preco: t.preco, assentos: t.vagas
      }))
    });

    if (r.http === 201) {
      const id = r.json.id ? " ID: " + r.json.id : "";
      return resposta(true, "Rota publicada com " + trechos.length + " trecho(s)." + id);
    }
    return resposta(false, motivo(r) + (r.rede ? " Nada foi publicado." : ""));
  }

  async function minhasCaronas() {
    const r = await http("GET", "/caronas");
    if (r.http !== 200) return resposta(false, motivo(r), { lista: [] });

    const lista = (r.json.caronas || []).map(c => ({
      id: c.id,
      trechos: (c.trechos || []).map(t => ({
        origem: t.origem, destino: t.destino, horario: t.horario || "",
        preco: t.preco, vagasTotal: t.assentos, vagasDisp: t.assentos_livres
      }))
    }));
    return resposta(true, "", { lista });
  }

  async function cancelarCarona(id) {
    if (!id) return resposta(false, "Selecione uma carona.");
    const r = await http("DELETE", "/caronas/" + encodeURIComponent(id), undefined, 60000);
    if (r.http === 200) return resposta(true, "Carona excluída. Reservas existentes foram canceladas e os passageiros avisados.");
    if (r.http === 202) return resposta(true, "Reservas canceladas, mas uma empresa está fora do ar. Repita a exclusão depois.");
    return resposta(false, motivo(r));
  }


  /* ---------- Passageiro ---------- */

  /* A busca distribuída (vizinhos, ciclos, visitados) é feita pelo servidor. */
  async function buscar(origem, destino, data) {
    if (!origem || !destino || !data)
      return resposta(false, "Informe data, origem e destino.", { lista: [] });

    const q = "?origem=" + encodeURIComponent(origem) + "&destino=" + encodeURIComponent(destino);
    const r = await http("GET", "/itinerarios" + q, undefined, 60000);
    if (r.http !== 200) return resposta(false, motivo(r), { lista: [] });

    const lista = (r.json.itinerarios || [])
      // sem filtro de data no servidor: aceita trechos da data pedida (ou sem data no horário)
      .filter(it => it.trechos.every(t => !temData(t.horario) || t.horario.startsWith(data)))
      .map(it => ({
        id: it.itinerario_id,
        preco: it.preco_total,
        trechos: it.trechos,
        rota: [it.trechos[0].origem, ...it.trechos.map(t => t.destino)].join(" → ")
      }));

    return resposta(
      true,
      lista.length ? lista.length + " itinerário(s) encontrado(s)." : "Nenhum itinerário para esta data.",
      { lista }
    );
  }

  /* O servidor da empresa do passageiro vira coordenador do 2PC. */
  async function reservar(it) {
    if (!it) return resposta(false, "Selecione um itinerário.");

    const r = await http("POST", "/reservas", {
      trechos: it.trechos.map(t => ({
        servidor: t.servidor, id_carona: t.id_carona,
        origem: t.origem, destino: t.destino
      }))
    }, 60000);

    if (!r.rede)
      return resposta(false, r.erro + " Resultado DESCONHECIDO: confira em RESERVAS antes de tentar de novo.");

    if (r.http === 201) {
      gravarMeta(r.json.id_transacao, { rota: it.rota, preco: it.preco });
      return resposta(true, "Reserva confirmada!" +
        (r.json.propagacao_pendente ? " (Alguma empresa registra a confirmação em segundo plano.)" : ""));
    }

    let m = motivo(r);
    if (r.http === 409) m += " Algum assento foi ocupado; faça uma nova busca.";
    else if (r.http >= 502 && r.http <= 504) m += " Empresa do trajeto fora do ar; nada foi reservado.";
    return resposta(false, m);
  }

  async function minhasReservas() {
    const r = await http("GET", "/reservas");
    if (r.http !== 200) return resposta(false, motivo(r), { lista: [] });

    const meta = lerMeta();
    const lista = (r.json.reservas || []).map(x => ({
      id: x.id_transacao,
      estado: x.estado,   // PREPARADO | COMITADO | CANCELANDO
      trechos: (x.itinerario && x.itinerario.length) ? x.itinerario : (x.trechos || []),
      rota: (meta[x.id_transacao] || {}).rota || "",
      preco: (meta[x.id_transacao] || {}).preco
    }));
    return resposta(true, "", { lista });
  }

  async function cancelarReserva(id) {
    if (!id) return resposta(false, "Selecione uma reserva.");

    const r = await http("DELETE", "/reservas/" + encodeURIComponent(id), undefined, 60000);
    if (!r.rede)
      return resposta(false, r.erro + " Resultado DESCONHECIDO: confira em RESERVAS.");
    if (r.http === 200) return resposta(true, "Reserva cancelada em todas as empresas.");
    if (r.http === 202) return resposta(true, "Cancelamento registrado; uma empresa está fora do ar e o servidor conclui quando ela voltar.");
    return resposta(false, motivo(r));
  }


  return {
    login, cadastro, sair,
    publicar, minhasCaronas, cancelarCarona,
    buscar, reservar, minhasReservas, cancelarReserva
  };
})();
