/* =====================================================
   ESTADO GLOBAL + FUNÇÕES AUXILIARES DE UI
   ===================================================== */

/* Estado da aplicação (tudo que muda durante o uso fica aqui) */
const S = {
  tela: "login",          // "login" | "passageiro" | "motorista"
  cadastro: false,        // login: false = entrar, true = criar conta
  tipo: "PASSAGEIRO",     // tipo de usuário escolhido no login
  sub: "BUSCAR",          // aba ativa do painel

  // Passageiro: locais selecionados no mapa (índices em LOCAIS, -1 = nenhum)
  orig: -1,
  dest: -1,
  data: "",
  itens: [],              // itinerários encontrados na última busca
  sel: -1,                // itinerário selecionado
  msgItin: "",            // mensagem da última busca

  // Motorista: trecho sendo montado + trechos já adicionados
  oT: -1,
  dT: -1,
  pend: [],
  mdata: "",
  mhora: "",
  mpreco: "",
  mvagas: ""
};


/* ---------- Helpers de DOM ---------- */
const $ = seletor => document.querySelector(seletor);

/* Valor de um <input> pelo id (string vazia se não existir) */
const v = id => ($("#" + id) || {}).value || "";

/* Mostra uma mensagem no rodapé do painel */
const msg = texto => { $("#msg").textContent = texto; };

/* Escapa HTML para evitar injeção ao montar innerHTML */
const esc = s => String(s).replace(/[&<>"]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

/* Nome do local pelo índice (null se não houver seleção) */
const nomeLocal = i => (i < 0 ? null : LOCAIS[i].nome);


/* ---------- Geradores de HTML ---------- */

/* Botão angular */
const btn = (texto, id, ativo) =>
  `<button class="btn${ativo ? " on" : ""}" id="${id}"><span>${texto}</span></button>`;

/* Campo de texto angular (clicar em qualquer parte foca o input) */
const fld = (rotulo, id, valor = "", tipo = "text") =>
  `<div class="btn fld" onclick="this.querySelector('input').focus()">
     <label for="${id}">${rotulo}</label>
     <input id="${id}" type="${tipo}" value="${esc(valor)}" autocomplete="off">
   </div>`;
