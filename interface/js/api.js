/* =====================================================
   API – "servidor" simulado em memória
   Todos os dados somem ao recarregar a página.
   Para usar um backend de verdade, troque o corpo destes
   métodos por fetch()/WebSocket mantendo o mesmo formato
   de resposta: { ok, mensagem, ...extra }
   ===================================================== */
const API = (() => {

  /* ---------- Dados iniciais ---------- */
  const usuarios = [
    { tipo: "PASSAGEIRO", nome: "Demo",  contato: "-", email: "p@p.com", senha: "1" },
    { tipo: "MOTORISTA",  nome: "Lydia", contato: "-", email: "m@m.com", senha: "1" }
  ];

  let caronas  = [];
  let reservas = [];
  let proxCarona  = 1;     // próximo id de carona
  let proxReserva = 1;     // próximo id de reserva
  let usuarioLogado = null;

  /* Um trecho = um pedaço da viagem (origem → destino) com preço e vagas. */
  const criarTrecho = (origem, destino, preco, vagas) =>
    ({ origem, destino, preco, vagasTotal: vagas, vagasDisp: vagas, pax: 0 });

  caronas.push({
    id: proxCarona++, motorista: "m@m.com", data: "2026-11-01", horario: "08:00", ativa: true,
    trechos: [criarTrecho("Whiterun", "Riverwood", 10, 3), criarTrecho("Riverwood", "Falkreath", 15, 3)]
  });
  caronas.push({
    id: proxCarona++, motorista: "m@m.com", data: "2026-11-01", horario: "09:30", ativa: true,
    trechos: [criarTrecho("Whiterun", "Windhelm", 30, 2), criarTrecho("Windhelm", "Riften", 25, 2)]
  });

  /* Formato padrão de resposta */
  const resposta = (ok, mensagem, extra = {}) => ({ ok, mensagem, ...extra });


  /* ---------- Usuários ---------- */
  function login(tipo, email, senha) {
    const u = usuarios.find(u => u.email === email && u.senha === senha && u.tipo === tipo);
    if (!u) return resposta(false, "Credenciais inválidas.");
    usuarioLogado = u;
    return resposta(true, "Bem-vindo, " + u.nome + "!", { u });
  }

  function cadastro(tipo, nome, contato, email, senha) {
    if (!nome || !email || !senha) return resposta(false, "Preencha todos os campos.");
    if (usuarios.some(u => u.email === email)) return resposta(false, "E-mail já cadastrado.");
    usuarios.push({ tipo, nome, contato, email, senha });
    return resposta(true, "Cadastro feito. Faça login.");
  }


  /* ---------- Motorista ---------- */
  function publicar(data, hora, trechos) {
    if (!/^\d{4}-\d{2}-\d{2}$/.test(data) || !/^\d{2}:\d{2}$/.test(hora))
      return resposta(false, "Use data AAAA-MM-DD e hora HH:MM.");
    if (!trechos.length)
      return resposta(false, "Adicione ao menos um trecho.");

    caronas.push({
      id: proxCarona++,
      motorista: usuarioLogado.email,
      data,
      horario: hora,
      ativa: true,
      trechos: trechos.map(t => criarTrecho(t.origem, t.destino, t.preco, t.vagas))
    });
    return resposta(true, "Carona #" + (proxCarona - 1) + " publicada!");
  }

  function minhasCaronas() {
    return caronas.filter(c => c.motorista === usuarioLogado.email);
  }

  function cancelarCarona(id) {
    const carona = caronas.find(c => c.id == id && c.motorista === usuarioLogado.email);
    if (!carona) return resposta(false, "Carona não encontrada.");

    carona.ativa = false;
    // cancela também as reservas que usavam esta carona
    reservas.forEach(r => {
      if (r.ativa && r.partes.some(p => p.c === carona.id)) cancelarReserva(r.id, true);
    });
    return resposta(true, "Carona #" + id + " cancelada.");
  }


  /* ---------- Passageiro ---------- */

  /* Busca itinerários de `origem` até `destino` numa data.
     Faz uma busca em profundidade (DFS) combinando trechos de caronas
     diferentes, com no máximo 3 trechos, e ordena do mais barato ao mais caro. */
  function buscar(origem, destino, data) {
    if (!origem || !destino || !data)
      return resposta(false, "Informe data, origem e destino.", { lista: [] });

    const resultados = [];
    const caronasDoDia = caronas.filter(c => c.ativa && c.data === data);

    function explorar(local, visitados, percurso, preco) {
      if (local === destino) {
        resultados.push({
          partes: percurso.slice(),
          preco,
          rota: [origem, ...percurso.map(p => p.t.destino)].join(" → ")
        });
        return;
      }
      if (percurso.length >= 3) return;   // limite de 3 trechos

      for (const c of caronasDoDia) {
        for (const t of c.trechos) {
          const serve = t.origem === local && t.vagasDisp > 0 && !visitados.includes(t.destino);
          if (!serve) continue;
          percurso.push({ c: c.id, t });
          explorar(t.destino, [...visitados, t.destino], percurso, preco + t.preco);
          percurso.pop();
        }
      }
    }

    explorar(origem, [origem], [], 0);
    resultados.sort((a, b) => a.preco - b.preco);

    return resposta(
      true,
      resultados.length ? resultados.length + " itinerário(s) encontrado(s)." : "Nenhum itinerário para esta data.",
      { lista: resultados }
    );
  }

  function reservar(itinerario) {
    if (!itinerario) return resposta(false, "Selecione um itinerário.");
    if (itinerario.partes.some(p => p.t.vagasDisp <= 0)) return resposta(false, "Sem vagas em algum trecho.");

    itinerario.partes.forEach(p => { p.t.vagasDisp--; p.t.pax++; });
    reservas.push({
      id: proxReserva++,
      email: usuarioLogado.email,
      ativa: true,
      preco: itinerario.preco,
      rota: itinerario.rota,
      partes: itinerario.partes.map(p => ({ c: p.c, t: p.t }))
    });
    return resposta(true, "Reserva #" + (proxReserva - 1) + " confirmada!");
  }

  function minhasReservas() {
    return reservas.filter(r => r.email === usuarioLogado.email);
  }

  /* `forcar` = true quando o cancelamento vem do motorista (ignora o dono da reserva) */
  function cancelarReserva(id, forcar) {
    const r = reservas.find(r => r.id == id && r.ativa && (forcar || r.email === usuarioLogado.email));
    if (!r) return resposta(false, "Reserva não encontrada.");

    r.ativa = false;
    r.partes.forEach(p => { p.t.vagasDisp++; p.t.pax--; });   // devolve as vagas
    return resposta(true, "Reserva #" + id + " cancelada.");
  }


  return {
    login, cadastro,
    publicar, minhasCaronas, cancelarCarona,
    buscar, reservar, minhasReservas, cancelarReserva
  };
})();
