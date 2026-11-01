/* =====================================================
   TELA DE LOGIN / CADASTRO
   Cada passageiro/motorista conecta no servidor da SUA empresa:
   o campo SERVIDOR recebe a URL dele (ex.: http://localhost:3001).
   O login usa o ID (letras, números, - _ .), não e-mail.
   ===================================================== */

const SERVIDOR_PADRAO = "http://localhost:3001";
let enviando = false;   // evita enviar duas vezes enquanto espera o servidor

function renderLogin() {
  $("#lform").innerHTML =
    btn("TIPO: " + S.tipo, "b-tipo") +
    fld("SERVIDOR:", "l-srv", S.srv || SERVIDOR_PADRAO) +
    (S.cadastro ? fld("NOME:", "l-nome") : "") +
    fld("ID:", "l-id") +
    fld("SENHA:", "l-senha", "", "password") +
    btn(S.cadastro ? "CADASTRAR" : "ENTRAR", "b-ok") +
    btn(S.cadastro ? "JÁ TENHO CONTA" : "NOVA CONTA", "b-troca");

  // Alterna PASSAGEIRO <-> MOTORISTA sem perder o que já foi digitado
  $("#b-tipo").onclick = () => {
    S.tipo = S.tipo === "PASSAGEIRO" ? "MOTORISTA" : "PASSAGEIRO";
    const ids = ["l-srv", "l-nome", "l-id"];
    const digitado = ids.map(v);
    renderLogin();
    ids.forEach((id, i) => { if ($("#" + id)) $("#" + id).value = digitado[i]; });
  };

  // Alterna entre "entrar" e "criar conta"
  $("#b-troca").onclick = () => {
    S.srv = v("l-srv");
    S.cadastro = !S.cadastro;
    $("#status").textContent = "";
    renderLogin();
  };

  $("#b-ok").onclick = enviarLogin;
  $("#l-senha").onkeydown = e => { if (e.key === "Enter") enviarLogin(); };
}


async function enviarLogin() {
  if (enviando) return;
  enviando = true;

  S.srv = v("l-srv");
  $("#status").textContent = "Conectando...";

  const r = S.cadastro
    ? await API.cadastro(S.tipo, v("l-nome"), v("l-id"), v("l-senha"), S.srv)
    : await API.login(S.tipo, v("l-id"), v("l-senha"), S.srv);

  enviando = false;
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
  S.selRes = "";
  S.selCar = "";

  $("#login").classList.add("hide");
  $("#app").classList.remove("hide");
  $("#titulo").textContent = S.tipo;

  desenharMapa();
  renderPainel();
  msg(mensagem);
}


/* Botão "sair" */
$("#sair").onclick = () => {
  API.sair();
  $("#app").classList.add("hide");
  $("#login").classList.remove("hide");
  S.tela = "login";
  renderLogin();
  $("#status").textContent = "";
};
