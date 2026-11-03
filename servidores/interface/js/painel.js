/* =====================================================
   PAINEL LATERAL
   Cada aba tem sua própria função de desenho:
     Passageiro: BUSCAR | ROTAS | RESERVAS
     Motorista : PUBLICAR | CARONAS
   As abas que dependem do servidor são assíncronas.
   ===================================================== */

const ABAS_PASSAGEIRO = [["BUSCAR", "BUSCAR"], ["ROTAS", "ROTAS"], ["RESERVAS", "RESERVAS"]];
const ABAS_MOTORISTA  = [["PUBLICAR", "PUBLICAR"], ["CARONAS", "MINHAS CARONAS"]];

const MAX_TRECHOS_ROTA = 9;   // 10 cidades = 9 trechos (mesmo limite do servidor)
const MAX_VAGAS = 60;

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

/* Marca visualmente o cartão selecionado sem buscar tudo de novo no servidor */
function marcarSelecao(c, id) {
  c.querySelectorAll(".pick").forEach(el => el.classList.toggle("on", el.dataset.id === id));
}

/* ================== PASSAGEIRO ================== */

/* Aba BUSCAR: data + origem/destino escolhidos no mapa */
function painelBuscar(c) {
  c.innerHTML =
    fld("DATA:", "p-data", S.data || "2026-11-01") +
    `<p class="sel"><b>ORIGEM:</b>${esc(nomeComMapa(S.orig) || "selecione no mapa")}</p>` +
    `<p class="sel"><b>DESTINO:</b>${esc(nomeComMapa(S.dest) || "selecione no mapa")}</p><br>` +
    btn("BUSCAR ITINERÁRIO", "p-busca") +
    btn("LIMPAR SELEÇÃO", "p-limpa");

  S.data = v("p-data");
  $("#p-data").oninput = e => { S.data = e.target.value; };

  $("#p-busca").onclick = async () => {
    msg("Buscando nas empresas do consórcio...");
    const r = await API.buscar(nomeLocal(S.orig) || "", nomeLocal(S.dest) || "", v("p-data"));
    S.itens = r.lista;
    S.sel = -1;
    S.msgItin = r.mensagem;

    // vai para a aba ROTAS se a busca foi válida (com ou sem resultado)
    if (r.ok) {
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


/* Aba ROTAS: itinerários encontrados, clique para selecionar */
function painelRotas(c) {
  const cartoes = S.itens.map((it, i) => `
    <div class="card pick${S.sel === i ? " on" : ""}" data-i="${i}">
      <div class="h"><span>Itinerário ${i + 1}</span><span>R$ ${Number(it.preco).toFixed(2)}</span></div>
      ${it.trechos.map(t => `
        <div class="r">[${esc(t.servidor)}] ${esc(t.origem)} → ${esc(t.destino)}
          · ${esc(t.horario)} · R$ ${Number(t.preco).toFixed(2)} · ${t.assentos_livres} livres</div>`).join("")}
    </div>`).join("");

  c.innerHTML =
    `<p class="mini">ITINERÁRIOS ENCONTRADOS:</p>` +
    (S.itens.length ? cartoes : `<p class="mini">${esc(S.msgItin || "Nenhuma busca feita ainda.")}</p>`) +
    "<br>" +
    btn("RESERVAR SELECIONADO", "p-res");

  c.querySelectorAll(".pick").forEach(el => {
    el.onclick = () => {
      S.sel = +el.dataset.i;
      c.querySelectorAll(".pick").forEach(x => x.classList.toggle("on", +x.dataset.i === S.sel));
    };
  });

  $("#p-res").onclick = async () => {
    if (S.sel < 0) return msg("Selecione um itinerário.");
    msg("Reservando (o servidor coordena a confirmação com as outras empresas)...");

    const r = await API.reservar(S.itens[S.sel]);
    msg(r.mensagem);

    if (r.ok) {
      S.itens = [];
      S.sel = -1;
      S.msgItin = "Reserva feita. Faça uma nova busca.";
      renderPainel();
    }
  };
}


/* Aba RESERVAS: lista do servidor, clique para selecionar e cancelar */
const ESTADOS_RESERVA = {
  COMITADO:   { rotulo: "CONFIRMADA",   cor: "ok",   borda: "a" },
  PREPARADO:  { rotulo: "EM ANDAMENTO", cor: "warn", borda: "a" },
  CANCELANDO: { rotulo: "CANCELANDO",   cor: "warn", borda: "c" },
  CANCELADA_MOTORISTA: { rotulo: "CANCELADA PELO MOTORISTA", cor: "bad", borda: "c" }
};

async function painelReservas(c) {
  c.innerHTML = `<p class="mini">Carregando reservas...</p>`;
  const r = await API.minhasReservas();
  if (S.sub !== "RESERVAS" || S.tela !== "passageiro") return;   // trocou de aba enquanto esperava

  if (!r.ok) {
    c.innerHTML = `<p class="mini bad">${esc(r.mensagem)}</p>`;
    return;
  }

  const cartoes = r.lista.map(x => {
    const e = ESTADOS_RESERVA[x.estado] || { rotulo: x.estado, cor: "warn", borda: "a" };
    const titulo = "Reserva " + x.id.slice(0, 8) + (x.preco != null ? " – R$ " + Number(x.preco).toFixed(2) : "");
    return `
      <div class="card pick ${e.borda}${S.selRes === x.id ? " on" : ""}" data-id="${esc(x.id)}">
        <div class="h"><span>${esc(titulo)}</span><span class="${e.cor}">${e.rotulo}</span></div>
        ${x.rota ? `<div class="r">${esc(x.rota)}</div>` : ""}
        ${x.trechos.map(t => `
          <div class="r">${esc(t.servidor)} : ${esc(t.id_carona)}${t.assento > 0 ? " · assento " + t.assento : ""}</div>`).join("")}
      </div>`;
  }).join("");

  c.innerHTML =
    `<p class="mini">MINHAS RESERVAS (clique para selecionar):</p>` +
    (r.lista.length ? cartoes : `<p class="mini">Você ainda não tem reservas.</p>`) +
    "<br>" +
    btn("CANCELAR SELECIONADA", "p-canc");

  c.querySelectorAll(".pick").forEach(el => {
    el.onclick = () => { S.selRes = el.dataset.id; marcarSelecao(c, S.selRes); };
  });

  $("#p-canc").onclick = async () => {
    if (!S.selRes) return msg("Selecione uma reserva.");
    msg("Cancelando (o servidor avisa as outras empresas do trajeto)...");

    const rc = await API.cancelarReserva(S.selRes);
    msg(rc.mensagem);
    if (rc.ok) S.selRes = "";
    renderPainel();
  };
}

/* ================== MOTORISTA ================== */

/* Aba PUBLICAR: monta uma rota (trechos encadeados) e publica de uma vez */

function painelPublicar(c) {
  const trechosPendentes = S.pend.map(t =>
    `<div class="ok trecho-pendente">${esc(t.origem)} → ${esc(t.destino)} · ${esc(t.hora)} · R$ ${t.preco.toFixed(2)} · ${t.vagas} vagas</div>`
  ).join("");

  c.innerHTML =
    fld("DATA:", "m-data", S.mdata || "2026-11-01") +
    `<p class="mini">TRECHO NO MAPA:</p>` +
    `<p class="sel">${esc(nomeComMapa(S.oT) || "ORIGEM")} → ${esc(nomeComMapa(S.dT) || "DESTINO")}</p>` +
    fld("HORA:", "m-hora", S.mhora || "08:00") +
    fld("PREÇO:", "m-preco", S.mpreco) +
    fld("VAGAS:", "m-vagas", S.mvagas) +
    btn("ADICIONAR TRECHO", "m-add") +
    trechosPendentes +
    (S.pend.length ? btn("REMOVER ÚLTIMO TRECHO", "m-undo") : "") +
    "<br>" +
    btn("PUBLICAR ROTA", "m-pub");

  $("#m-data").oninput  = e => { S.mdata  = e.target.value; };
  $("#m-hora").oninput  = e => { S.mhora  = e.target.value; };
  $("#m-preco").oninput = e => { S.mpreco = e.target.value; };
  $("#m-vagas").oninput = e => { S.mvagas = e.target.value; };

  $("#m-add").onclick = () => {
    const preco = parseFloat(v("m-preco").replace(",", "."));
    const vagas = parseInt(v("m-vagas"), 10);
    const hora  = v("m-hora").trim();
    const ultimo = S.pend[S.pend.length - 1];

    if (S.pend.length >= MAX_TRECHOS_ROTA) return msg("Máximo de " + MAX_TRECHOS_ROTA + " trechos por rota.");
    if (S.oT < 0 || S.dT < 0)              return msg("Selecione origem e destino no mapa.");
    if (ultimo && ultimo.destino !== nomeLocal(S.oT))
      return msg("O trecho deve partir de " + ultimo.destino + " (destino do anterior).");
    if (S.pend.some(t => t.origem === nomeLocal(S.dT) || t.destino === nomeLocal(S.dT)))
      return msg("A rota não pode repetir cidades.");
    if (!/^\d{2}:\d{2}$/.test(hora))       return msg("Use hora HH:MM.");
    if (!(preco >= 0))                     return msg("Verifique o preço.");
    if (!(vagas >= 1 && vagas <= MAX_VAGAS)) return msg("Vagas deve estar entre 1 e " + MAX_VAGAS + ".");

    S.pend.push({ origem: nomeLocal(S.oT), destino: nomeLocal(S.dT), hora, preco, vagas });

    S.oT = S.dT;
    S.dT = -1;
    S.mpreco = S.mvagas = "";
    msg("Trecho adicionado! Escolha o próximo destino no mapa.");
    renderPainel();
    pintar();
  };

  if (S.pend.length) {
    $("#m-undo").onclick = () => {
      const removido = S.pend.pop();
      S.oT = indiceDoLocal(removido.origem);
      S.dT = -1;
      renderPainel();
      pintar();
    };
  }

  $("#m-pub").onclick = async () => {
    msg("Publicando a rota (tudo ou nada)...");
    const r = await API.publicar(v("m-data"), S.pend);
    msg(r.mensagem);

    if (r.ok) {
      S.pend = [];
      S.oT = S.dT = -1;
      renderPainel();
      pintar();
    }
  };
}


async function painelMinhasCaronas(c) {
  c.innerHTML = `<p class="mini">Carregando caronas...</p>`;
  const r = await API.minhasCaronas();
  if (S.sub !== "CARONAS" || S.tela !== "motorista") return;

  if (!r.ok) {
    c.innerHTML = `<p class="mini bad">${esc(r.mensagem)}</p>`;
    return;
  }

  const classeVagas = t =>
    t.vagasDisp <= 0 ? "bad" : t.vagasDisp < t.vagasTotal ? "warn" : "ok";

  const cartoes = r.lista.map(x => {
    const primeiro = x.trechos[0], ultimo = x.trechos[x.trechos.length - 1];
    const titulo = primeiro ? primeiro.origem + " → " + ultimo.destino : "(sem trechos)";
    return `
      <div class="card pick a${S.selCar === x.id ? " on" : ""}" data-id="${esc(x.id)}">
        <div class="h"><span>${esc(titulo)}</span><span>${x.trechos.length} trecho(s)</span></div>
        ${x.trechos.map(t => `
          <div class="r ${classeVagas(t)}">
            ${esc(t.origem)} → ${esc(t.destino)} · ${esc(t.horario)} ·
            R$ ${Number(t.preco).toFixed(2)} · ${t.vagasDisp}/${t.vagasTotal} livres
          </div>`).join("")}
      </div>`;
  }).join("");

  c.innerHTML =
    `<p class="mini">MINHAS CARONAS (clique para selecionar):</p>` +
    (r.lista.length ? cartoes : `<p class="mini">Nenhuma carona publicada.</p>`) +
    "<br>" +
    btn("EXCLUIR SELECIONADA", "m-canc");

  c.querySelectorAll(".pick").forEach(el => {
    el.onclick = () => { S.selCar = el.dataset.id; marcarSelecao(c, S.selCar); };
  });

  $("#m-canc").onclick = async () => {
    if (!S.selCar) return msg("Selecione uma carona.");
    if (!confirm("Se houver reservas, elas serão canceladas e os passageiros avisados. Continuar?")) return;
    const rc = await API.cancelarCarona(S.selCar);
    msg(rc.mensagem);
    if (rc.ok) S.selCar = "";
    renderPainel();
  };
}
