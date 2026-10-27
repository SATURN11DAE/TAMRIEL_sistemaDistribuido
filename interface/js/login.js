/* =====================================================
   TELA DE LOGIN / CADASTRO
   ===================================================== */

function renderLogin() {
  $("#lform").innerHTML =
    btn("TIPO: " + S.tipo, "b-tipo") +
    (S.cadastro ? fld("NOME:", "l-nome") + fld("CONTATO:", "l-cont") : "") +
    fld("EMAIL:", "l-email") +
    fld("SENHA:", "l-senha", "", "password") +
    btn(S.cadastro ? "CADASTRAR" : "ENTRAR", "b-ok") +
    btn(S.cadastro ? "JÁ TENHO CONTA" : "NOVA CONTA", "b-troca");

  // Alterna PASSAGEIRO <-> MOTORISTA sem perder o que já foi digitado
  $("#b-tipo").onclick = () => {
    S.tipo = S.tipo === "PASSAGEIRO" ? "MOTORISTA" : "PASSAGEIRO";
    const ids = ["l-nome", "l-cont", "l-email"];
    const digitado = ids.map(v);
    renderLogin();
    ids.forEach((id, i) => { if ($("#" + id)) $("#" + id).value = digitado[i]; });
  };

  // Alterna entre "entrar" e "criar conta"
  $("#b-troca").onclick = () => {
    S.cadastro = !S.cadastro;
    $("#status").textContent = "";
    renderLogin();
  };

  $("#b-ok").onclick = enviarLogin;
  $("#l-senha").onkeydown = e => { if (e.key === "Enter") enviarLogin(); };
}


function enviarLogin() {
  const r = S.cadastro
    ? API.cadastro(S.tipo, v("l-nome"), v("l-cont"), v("l-email"), v("l-senha"))
    : API.login(S.tipo, v("l-email"), v("l-senha"));

  $("#status").textContent = r.mensagem;
  if (!r.ok) return;

  // Cadastro ok: volta para o formulário de login mantendo a mensagem
  if (S.cadastro) {
    S.cadastro = false;
    renderLogin();
    $("#status").textContent = r.mensagem;
    return;
  }

  entrarNoApp(r.mensagem);
}


/* Troca a tela de login pela aplicação */
function entrarNoApp(mensagem) {
  const motorista = S.tipo === "MOTORISTA";
  S.tela = motorista ? "motorista" : "passageiro";
  S.sub  = motorista ? "PUBLICAR" : "BUSCAR";

  // limpa seleções da sessão anterior
  S.orig = S.dest = S.oT = S.dT = -1;
  S.itens = [];
  S.sel = -1;
  S.pend = [];

  $("#login").classList.add("hide");
  $("#app").classList.remove("hide");
  $("#titulo").textContent = S.tipo;

  desenharMapa();
  renderPainel();
  msg(mensagem);
}


/* Botão "sair" */
$("#sair").onclick = () => {
  $("#app").classList.add("hide");
  $("#login").classList.remove("hide");
  S.tela = "login";
  renderLogin();
  $("#status").textContent = "";
};
