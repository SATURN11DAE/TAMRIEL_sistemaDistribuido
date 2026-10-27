/* =====================================================
   MAPA SVG
   Zoom e arrastar funcionam alterando o atributo viewBox.
   ===================================================== */

const NS = "http://www.w3.org/2000/svg";
const IMG_MAPA = "img/mapa.webp";

/* Tamanho da imagem do mapa (pixels) */
const X0 = 0, Y0 = 0, W = 2212, H = 1659;

const svg = $("#map");
let vb = { x: X0, y: Y0, w: W, h: H };   // área visível atual
let arrasto = null;                       // posição do ponteiro durante o arraste
let moveu = false;                        // distingue "clique" de "arraste"


/* ---------- Câmera (viewBox) ---------- */

function aplicarVB() {
  svg.setAttribute("viewBox", `${vb.x} ${vb.y} ${vb.w} ${vb.h}`);
  // mostra mais rótulos conforme aproxima (ver .lbl no CSS)
  svg.classList.toggle("z1", vb.w < 1100);
  svg.classList.toggle("z2", vb.w < 550);
}

/* Encaixa o mapa inteiro na área disponível, centralizado */
function ajustar() {
  const r = $("#mapwrap").getBoundingClientRect();
  const proporcao = r.width / r.height;
  vb.h = W / proporcao > H ? W / proporcao : H;
  vb.w = vb.h * proporcao;
  vb.x = X0 + (W - vb.w) / 2;
  vb.y = Y0 + (H - vb.h) / 2;
  aplicarVB();
}

/* Zoom em torno de um ponto da tela (cx, cy). fator < 1 aproxima. */
function zoom(fator, cx, cy) {
  const r = svg.getBoundingClientRect();
  const mx = vb.x + (cx - r.left) / r.width  * vb.w;   // ponto do mapa sob o cursor
  const my = vb.y + (cy - r.top)  / r.height * vb.h;

  const novaLargura = Math.min(W * 1.6, Math.max(90, vb.w * fator));
  const k = novaLargura / vb.w;

  vb.x = mx - (mx - vb.x) * k;
  vb.y = my - (my - vb.y) * k;
  vb.w = novaLargura;
  vb.h *= k;
  aplicarVB();
}


/* ---------- Desenho ---------- */

function desenharMapa() {
  svg.innerHTML =
    `<rect x="-9000" y="-9000" width="20000" height="20000" fill="#0b1015"/>` +
    `<image href="${IMG_MAPA}" x="0" y="0" width="${W}" height="${H}"/>`;

  LOCAIS.forEach((local, i) => {
    const tipo = TIPOS[local.tipo];
    const g = document.createElementNS(NS, "g");
    g.dataset.i = i;
    g.style.cursor = "pointer";

    g.innerHTML =
      // área de clique maior e invisível
      `<circle cx="${local.x}" cy="${local.y}" r="16" fill="transparent"/>` +
      // anel (colorido só quando é origem/destino)
      `<circle class="ring" cx="${local.x}" cy="${local.y}" r="${tipo.raio * 1.8}" fill="none" stroke-width="4"/>` +
      // ponto do local
      `<circle class="dot" cx="${local.x}" cy="${local.y}" r="${tipo.raio}" fill="${tipo.cor}" stroke="#000" stroke-width="2"/>` +
      // rótulo: capitais (l0) aparecem antes; os demais (l1) só com mais zoom
      `<text class="lbl l${local.tipo === 0 ? 0 : 1}" x="${local.x}" y="${local.y - tipo.raio - 6}">${esc(local.nome)}</text>`;

    svg.appendChild(g);
  });

  ajustar();
  pintar();
}

/* Marca origem (verde) e destino (vermelho) conforme a seleção atual */
function pintar() {
  const motorista = S.tela === "motorista";
  const origem  = motorista ? S.oT : S.orig;
  const destino = motorista ? S.dT : S.dest;

  svg.querySelectorAll("g[data-i]").forEach(g => {
    const i = +g.dataset.i;
    const anel = g.querySelector(".ring");
    const ponto = g.querySelector(".dot");
    const emUso = i === origem || i === destino;

    anel.setAttribute("stroke", emUso ? (i === origem ? "#1fae1f" : "#e02020") : "transparent");
    ponto.setAttribute("fill",  emUso ? (i === origem ? "#7be07b" : "#ff7878") : TIPOS[LOCAIS[i].tipo].cor);
  });
}


/* ---------- Seleção de origem/destino ---------- */

/* 1º clique = origem, 2º = destino, 3º recomeça com nova origem */
function clicarLocal(i) {
  if (S.tela === "motorista") {
    if (S.oT < 0)                      S.oT = i;
    else if (S.dT < 0 && i !== S.oT)   S.dT = i;
    else { S.oT = i; S.dT = -1; }
    if (S.sub === "PUBLICAR") renderPainel();
  } else {
    if (S.orig < 0)                        S.orig = i;
    else if (S.dest < 0 && i !== S.orig)   S.dest = i;
    else { S.orig = i; S.dest = -1; }
    if (S.sub === "BUSCAR") renderPainel();
  }
  pintar();
}


/* ---------- Eventos: arrastar, tooltip, zoom ---------- */

svg.addEventListener("pointerdown", e => {
  arrasto = { x: e.clientX, y: e.clientY };
  moveu = false;
  svg.setPointerCapture(e.pointerId);
});

svg.addEventListener("pointermove", e => {
  // tooltip com nome · tipo · região
  const g = e.target.closest && e.target.closest("g[data-i]");
  const tip = $("#tip");
  if (g && !arrasto) {
    const l = LOCAIS[g.dataset.i];
    const r = $("#mapwrap").getBoundingClientRect();
    tip.textContent = l.nome + " · " + TIPOS[l.tipo].nome + " · " + l.regiao;
    tip.style.left = (e.clientX - r.left) + "px";
    tip.style.top  = (e.clientY - r.top)  + "px";
    tip.classList.remove("hide");
  } else {
    tip.classList.add("hide");
  }

  // arrastar o mapa
  if (!arrasto) return;
  const r = svg.getBoundingClientRect();
  const dx = e.clientX - arrasto.x;
  const dy = e.clientY - arrasto.y;
  if (Math.abs(dx) + Math.abs(dy) > 3) moveu = true;

  vb.x -= dx * vb.w / r.width;
  vb.y -= dy * vb.h / r.height;
  arrasto = { x: e.clientX, y: e.clientY };
  aplicarVB();
});

svg.addEventListener("pointerup", e => {
  arrasto = null;
  if (moveu) return;   // foi arraste, não clique

  const alvo = document.elementFromPoint(e.clientX, e.clientY);
  const g = alvo && alvo.closest && alvo.closest("g[data-i]");
  if (g) clicarLocal(+g.dataset.i);
});

svg.addEventListener("wheel", e => {
  e.preventDefault();
  zoom(e.deltaY < 0 ? 0.85 : 1.18, e.clientX, e.clientY);
}, { passive: false });


/* ---------- Botões do mapa ---------- */

/* Zoom pelos botões + / − (centro do mapa) */
function zoomCentro(fator) {
  const r = svg.getBoundingClientRect();
  zoom(fator, r.left + r.width / 2, r.top + r.height / 2);
}
$("#zi").onclick = () => zoomCentro(0.7);
$("#zo").onclick = () => zoomCentro(1.4);

/* Botão ☰: esconde/mostra o painel mantendo o centro e a escala do mapa */
$("#tg").onclick = () => {
  const antes = svg.getBoundingClientRect();
  const escala = vb.w / antes.width;
  const cx = vb.x + vb.w / 2;
  const cy = vb.y + vb.h / 2;

  const oculto = $("#app").classList.toggle("oculto");
  $("#tg").setAttribute("aria-label", oculto ? "Mostrar painel" : "Esconder painel");

  const depois = svg.getBoundingClientRect();
  vb.w = escala * depois.width;
  vb.h = escala * depois.height;
  vb.x = cx - vb.w / 2;
  vb.y = cy - vb.h / 2;
  aplicarVB();
};

addEventListener("resize", () => { if (S.tela !== "login") ajustar(); });
