/* =====================================================
   PAINEL LATERAL
   Cada aba tem sua própria função de desenho:
     Passageiro: BUSCAR | ROTAS | RESERVAS
     Motorista : PUBLICAR | CARONAS
   ===================================================== */

const ABAS_PASSAGEIRO = [["BUSCAR", "BUSCAR"], ["ROTAS", "ROTAS"], ["RESERVAS", "RESERVAS"]];
const ABAS_MOTORISTA  = [["PUBLICAR", "PUBLICAR"], ["CARONAS", "MINHAS CARONAS"]];

/* Desenha as abas e chama a função da aba ativa */
function renderPainel() {
  const motorista = S.tela === "motorista";
  const abas = motorista ? ABAS_MOTORISTA : ABAS_PASSAGEIRO;

  $("#tabs").innerHTML = abas
    .map(([chave, titulo]) =>
      `<button class="btn sm${S.sub === chave ? " on" : ""}" data-k="${chave}" style="flex:1"><span>${titulo}</span></button>`)
    .join("");

  $("#tabs").querySelectorAll("button").forEach(b => {
    b.onclick = () => { S.sub = b.dataset.k; renderPainel(); };
  });

  const conteudo = $("#conteudo");
  if (motorista) {
    if (S.sub === "PUBLICAR") painelPublicar(conteudo);
    else                      painelMinhasCaronas(conteudo);
  } else {
    if (S.sub === "BUSCAR")        painelBuscar(conteudo);
    else if (S.sub === "ROTAS")    painelRotas(conteudo);
    else                           painelReservas(conteudo);
  }
}


/* ================== PASSAGEIRO ================== */

/* Aba BUSCAR: data + origem/destino escolhidos no mapa */
function painelBuscar(c) {
  c.innerHTML =
    fld("DATA:", "p-data", S.data || "2026-11-01") +
    `<p class="sel"><b>ORIGEM:</b>${esc(nomeLocal(S.orig) || "selecione no mapa")}</p>` +
    `<p class="sel"><b>DESTINO:</b>${esc(nomeLocal(S.dest) || "selecione no mapa")}</p><br>` +
    btn("BUSCAR ITINERÁRIO", "p-busca") +
    btn("LIMPAR SELEÇÃO", "p-limpa");

  S.data = v("p-data");
  $("#p-data").oninput = e => { S.data = e.target.value; };

  $("#p-busca").onclick = () => {
    const r = API.buscar(nomeLocal(S.orig) || "", nomeLocal(S.dest) || "", v("p-data"));
    S.itens = r.lista;
    S.sel = -1;
    S.msgItin = r.mensagem;

    // vai para a aba ROTAS se achou algo (ou se a busca era válida mas sem resultado)
    if (r.lista.length || (S.orig >= 0 && S.dest >= 0)) {
      S.sub = "ROTAS";
      renderPainel();
    }
    msg(r.mensagem);
  };

  $("#p-limpa").onclick = () => {
    S.orig = S.dest = -1;
    renderPainel();
    pintar();
  };
}


/* Aba ROTAS: lista de itinerários encontrados, clique para selecionar */
function painelRotas(c) {
  const cartoes = S.itens.map((it, i) => `
    <div class="card pick${S.sel === i ? " on" : ""}" data-i="${i}">
      <div class="h"><span>Itinerário ${i + 1}</span><span>R$ ${it.preco.toFixed(2)}</span></div>
      <div class="r">${esc(it.rota)}</div>
    </div>`).join("");

  c.innerHTML =
    `<p class="mini">ITINERÁRIOS ENCONTRADOS:</p>` +
    (S.itens.length ? cartoes : `<p class="mini">${esc(S.msgItin || "Nenhuma busca feita ainda.")}</p>`) +
    "<br>" +
    btn("RESERVAR SELECIONADO", "p-res");

  c.querySelectorAll(".pick").forEach(el => {
    el.onclick = () => { S.sel = +el.dataset.i; renderPainel(); };
  });

  $("#p-res").onclick = () => {
    const r = API.reservar(S.itens[S.sel]);
    msg(r.mensagem);
    if (r.ok) {
      S.itens = [];
      S.sel = -1;
      S.msgItin = "Reserva feita. Faça uma nova busca.";
      renderPainel();
    }
  };
}


/* Aba RESERVAS: histórico + cancelamento por id */
function painelReservas(c) {
  const lista = API.minhasReservas();

  const cartoes = lista.map(r => `
    <div class="card ${r.ativa ? "a" : "c"}">
      <div class="h">
        <span>Reserva #${r.id} – R$ ${r.preco.toFixed(2)}</span>
        <span class="${r.ativa ? "ok" : "bad"}">${r.ativa ? "ATIVA" : "CANCELADA"}</span>
      </div>
      <div class="r">${esc(r.rota)}</div>
    </div>`).join("");

  c.innerHTML =
    `<p class="mini">MINHAS RESERVAS:</p>` +
    (lista.length ? cartoes : `<p class="mini">Você ainda não tem reservas.</p>`) +
    "<br>" +
    fld("ID RESERVA:", "p-id") +
    btn("CANCELAR RESERVA", "p-canc");

  $("#p-canc").onclick = () => {
    msg(API.cancelarReserva(v("p-id")).mensagem);
    renderPainel();
  };
}


/* ================== MOTORISTA ================== */

/* Aba PUBLICAR: monta uma carona com vários trechos */
function painelPublicar(c) {
  const trechosPendentes = S.pend.map(t =>
    `<div class="ok trecho-pendente">${esc(t.origem)} → ${esc(t.destino)} · R$ ${t.preco.toFixed(2)} · ${t.vagas} vagas</div>`
  ).join("");

  c.innerHTML =
    fld("DATA:", "m-data", S.mdata || "2026-11-01") +
    fld("HORA:", "m-hora", S.mhora || "08:00") +
    `<p class="mini">TRECHO NO MAPA:</p>` +
    `<p class="sel">${esc(nomeLocal(S.oT) || "ORIGEM")} → ${esc(nomeLocal(S.dT) || "DESTINO")}</p>` +
    fld("PREÇO:", "m-preco", S.mpreco) +
    fld("VAGAS:", "m-vagas", S.mvagas) +
    btn("ADICIONAR TRECHO", "m-add") +
    trechosPendentes +
    "<br>" +
    btn("PUBLICAR CARONA", "m-pub");

  // guarda o que foi digitado para não perder quando o painel é redesenhado
  $("#m-data").oninput  = e => { S.mdata  = e.target.value; };
  $("#m-hora").oninput  = e => { S.mhora  = e.target.value; };
  $("#m-preco").oninput = e => { S.mpreco = e.target.value; };
  $("#m-vagas").oninput = e => { S.mvagas = e.target.value; };

  $("#m-add").onclick = () => {
    const preco = parseFloat(v("m-preco").replace(",", "."));
    const vagas = parseInt(v("m-vagas"));

    if (S.oT < 0 || S.dT < 0)    return msg("Selecione origem e destino no mapa.");
    if (!(preco > 0) || !(vagas > 0)) return msg("Verifique preço e vagas.");

    S.pend.push({ origem: nomeLocal(S.oT), destino: nomeLocal(S.dT), preco, vagas });
    S.oT = S.dT = -1;
    S.mpreco = S.mvagas = "";
    msg("Trecho adicionado!");
    renderPainel();
    pintar();
  };

  $("#m-pub").onclick = () => {
    const r = API.publicar(v("m-data"), v("m-hora"), S.pend);
    msg(r.mensagem);
    if (r.ok) {
      S.pend = [];
      renderPainel();
    }
  };
}


/* Aba CARONAS: caronas publicadas + cancelamento por id */
function painelMinhasCaronas(c) {
  const lista = API.minhasCaronas();

  // cor da linha conforme a lotação do trecho
  const classeVagas = t =>
    t.vagasDisp <= 0 ? "bad" : t.vagasDisp < t.vagasTotal ? "warn" : "ok";

  const cartoes = lista.map(x => `
    <div class="card ${x.ativa ? "a" : "c"}">
      <div class="h">
        <span>Carona #${x.id} – ${x.data} ${x.horario}</span>
        <span class="${x.ativa ? "ok" : "bad"}">${x.ativa ? "ATIVA" : "CANCELADA"}</span>
      </div>
      ${x.trechos.map(t => `
        <div class="r ${classeVagas(t)}">
          ${esc(t.origem)} → ${esc(t.destino)} · R$${t.preco.toFixed(2)} · ${t.vagasDisp}/${t.vagasTotal} vagas${t.pax ? ` (${t.pax} pax)` : ""}
        </div>`).join("")}
    </div>`).join("");

  c.innerHTML =
    `<p class="mini">MINHAS CARONAS:</p>` +
    (lista.length ? cartoes : `<p class="mini">Nenhuma carona publicada.</p>`) +
    "<br>" +
    fld("ID CARONA:", "m-id") +
    btn("CANCELAR CARONA", "m-canc");

  $("#m-canc").onclick = () => {
    msg(API.cancelarCarona(v("m-id")).mensagem);
    renderPainel();
  };
}
