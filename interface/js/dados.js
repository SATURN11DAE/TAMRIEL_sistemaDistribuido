/* DADOS DO MAPA
   Coordenadas em pixels sobre img/mapa.webp (2212 x 1659). */

/* Tipos de local. O campo "tipo" de cada local é o índice desta lista. */
const TIPOS = [
  { cor: "#DDA0DD", raio: 12, nome: "Capital" },             // 0
  { cor: "#DA70D6", raio: 9,  nome: "Cidade" },              // 1
  { cor: "#BA55D3", raio: 8,  nome: "Forte orc" },           // 2
  { cor: "#9932CC", raio: 8,  nome: "Povoado" },             // 3
  { cor: "#9370DB", raio: 8,  nome: "Altar" },               // 4
  { cor: "#4B0082", raio: 7,  nome: "Ponto de interesse" }   // 5
];

/* Todos os locais clicáveis, agrupados por região. */
const LOCAIS = [
  // ----- Skyrim -----
  { nome: "Dawnstar", x: 1140, y: 196, tipo: 0, regiao: "Skyrim" },
  { nome: "Falkreath", x: 1070, y: 547, tipo: 0, regiao: "Skyrim" },
  { nome: "Winterhold", x: 1276, y: 199, tipo: 0, regiao: "Skyrim" },
  { nome: "Windhelm", x: 1375, y: 306, tipo: 0, regiao: "Skyrim" },
  { nome: "Morthal", x: 1047, y: 269, tipo: 0, regiao: "Skyrim" },
  { nome: "Riften", x: 1437, y: 587, tipo: 0, regiao: "Skyrim" },
  { nome: "Whiterun", x: 1175, y: 383, tipo: 0, regiao: "Skyrim" },
  { nome: "Solitude", x: 1006, y: 205, tipo: 0, regiao: "Skyrim" },
  { nome: "Markarth", x: 745, y: 393, tipo: 0, regiao: "Skyrim" },
  { nome: "Rorikstead", x: 962, y: 371, tipo: 1, regiao: "Skyrim" },
  { nome: "Karthwasten", x: 640, y: 291, tipo: 1, regiao: "Skyrim" },
  { nome: "Riverwood", x: 1157, y: 482, tipo: 1, regiao: "Skyrim" },
  { nome: "Helgen", x: 1141, y: 535, tipo: 1, regiao: "Skyrim" },
  { nome: "Dragon Bridge", x: 888, y: 231, tipo: 1, regiao: "Skyrim" },
  { nome: "Ivarstead", x: 1259, y: 506, tipo: 1, regiao: "Skyrim" },
  { nome: "Shor’s Stone", x: 1318, y: 402, tipo: 1, regiao: "Skyrim" },
  { nome: "Mor Khazgul", x: 837, y: 236, tipo: 2, regiao: "Skyrim" },
  { nome: "Dushnikh Yal", x: 853, y: 433, tipo: 2, regiao: "Skyrim" },
  { nome: "Largashbur", x: 1347, y: 593, tipo: 2, regiao: "Skyrim" },
  { nome: "Narzulbur", x: 1415, y: 370, tipo: 2, regiao: "Skyrim" },
  { nome: "Chalé Espinho-Negro", x: 1385, y: 562, tipo: 3, regiao: "Skyrim" },
  { nome: "Fazenda Brandy-Mug", x: 1365, y: 350, tipo: 3, regiao: "Skyrim" },
  { nome: "Santuário da Dark Brotherhood", x: 1038, y: 563, tipo: 3, regiao: "Skyrim" },
  { nome: "Santuário de Dawnstar", x: 1135, y: 176, tipo: 3, regiao: "Skyrim" },
  { nome: "Propriedade Goldenglow", x: 1418, y: 612, tipo: 3, regiao: "Skyrim" },
  { nome: "Hidromelaria Honningbrew", x: 1200, y: 412, tipo: 3, regiao: "Skyrim" },
  { nome: "Kynesgrove", x: 1343, y: 372, tipo: 3, regiao: "Skyrim" },
  { nome: "Estalagem Nightgate", x: 1262, y: 298, tipo: 3, regiao: "Skyrim" },
  { nome: "Velha Hroldan", x: 910, y: 401, tipo: 3, regiao: "Skyrim" },
  { nome: "Embaixada Thalmor", x: 968, y: 171, tipo: 3, regiao: "Skyrim" },
  { nome: "Altar de Peryite", x: 690, y: 350, tipo: 4, regiao: "Skyrim" },
  { nome: "Altar de Meridia", x: 890, y: 205, tipo: 4, regiao: "Skyrim" },
  { nome: "Altar de Meruhnes Dagon", x: 1062, y: 297, tipo: 4, regiao: "Skyrim" },
  { nome: "Altar de Azura", x: 1290, y: 242, tipo: 4, regiao: "Skyrim" },
  { nome: "Santuário de Boethiah", x: 1305, y: 345, tipo: 4, regiao: "Skyrim" },
  { nome: "Altar de Malacath", x: 1300, y: 550, tipo: 4, regiao: "Skyrim" },
  { nome: "Salto do Bardo", x: 820, y: 375, tipo: 5, regiao: "Skyrim" },
  { nome: "Travessia Dwemer", x: 715, y: 310, tipo: 5, regiao: "Skyrim" },
  { nome: "Monumento de Gjukar", x: 930, y: 430, tipo: 5, regiao: "Skyrim" },
  { nome: "Pedra Órfã", x: 1112, y: 500, tipo: 5, regiao: "Skyrim" },
  { nome: "Torre do Vento de Reach", x: 770, y: 430, tipo: 5, regiao: "Skyrim" },
  { nome: "Ruínas de Bthalft", x: 1290, y: 525, tipo: 5, regiao: "Skyrim" },
  { nome: "Ruínas de Rkund", x: 1355, y: 625, tipo: 5, regiao: "Skyrim" },
  { nome: "Templo do Refúgio Celestial", x: 858, y: 372, tipo: 5, regiao: "Skyrim" },
  { nome: "Garganta do Mundo", x: 1210, y: 453, tipo: 5, regiao: "Skyrim" },
  { nome: "Pedras de Weynon", x: 1120, y: 350, tipo: 5, regiao: "Skyrim" },

  // ----- High Rock -----
  { nome: "Wayrest", x: 535, y: 506, tipo: 0, regiao: "High Rock" },
  { nome: "Daggerfall", x: 150, y: 620, tipo: 1, regiao: "High Rock" },
  { nome: "Camlorn", x: 232, y: 480, tipo: 1, regiao: "High Rock" },
  { nome: "Shornhelm", x: 427, y: 326, tipo: 1, regiao: "High Rock" },
  { nome: "Northpoint", x: 525, y: 205, tipo: 1, regiao: "High Rock" },
  { nome: "Evermor", x: 655, y: 410, tipo: 1, regiao: "High Rock" },
  { nome: "Jehanna", x: 629, y: 217, tipo: 1, regiao: "High Rock" },
  { nome: "Farrun", x: 686, y: 199, tipo: 1, regiao: "High Rock" },
  { nome: "Ykalon", x: 212, y: 472, tipo: 1, regiao: "High Rock" },
  { nome: "Alcaire", x: 390, y: 391, tipo: 1, regiao: "High Rock" },

  // ----- Hammerfell -----
  { nome: "Sentinel", x: 275, y: 641, tipo: 0, regiao: "Hammerfell" },
  { nome: "Rihad", x: 678, y: 930, tipo: 1, regiao: "Hammerfell" },
  { nome: "Skaven", x: 660, y: 562, tipo: 1, regiao: "Hammerfell" },
  { nome: "Elinhir", x: 1004, y: 584, tipo: 1, regiao: "Hammerfell" },
  { nome: "Hegathe", x: 305, y: 850, tipo: 1, regiao: "Hammerfell" },
  { nome: "Kozanset", x: 523, y: 632, tipo: 1, regiao: "Hammerfell" },
  { nome: "Dragonstar", x: 727, y: 432, tipo: 1, regiao: "Hammerfell" },
  { nome: "Taneth", x: 609, y: 814, tipo: 1, regiao: "Hammerfell" },
  { nome: "Bergama", x: 469, y: 704, tipo: 1, regiao: "Hammerfell" },
  { nome: "Satakalaam", x: 547, y: 566, tipo: 1, regiao: "Hammerfell" },
  { nome: "Orsinium", x: 886, y: 476, tipo: 1, regiao: "Hammerfell" },

  // ----- Cyrodiil -----
  { nome: "Cidade Imperial", x: 1245, y: 846, tipo: 0, regiao: "Cyrodiil" },
  { nome: "Bravil", x: 1347, y: 1068, tipo: 1, regiao: "Cyrodiil" },
  { nome: "Leyawiin", x: 1420, y: 1303, tipo: 1, regiao: "Cyrodiil" },
  { nome: "Skingrad", x: 1060, y: 987, tipo: 1, regiao: "Cyrodiil" },
  { nome: "Chorrol", x: 1067, y: 771, tipo: 1, regiao: "Cyrodiil" },
  { nome: "Cheydinhal", x: 1408, y: 764, tipo: 1, regiao: "Cyrodiil" },
  { nome: "Bruma", x: 1245, y: 656, tipo: 1, regiao: "Cyrodiil" },
  { nome: "Anvil", x: 748, y: 1088, tipo: 1, regiao: "Cyrodiil" },
  { nome: "Kvatch", x: 865, y: 995, tipo: 1, regiao: "Cyrodiil" },

  // ----- Morrowind -----
  { nome: "Vivec", x: 1738, y: 568, tipo: 0, regiao: "Morrowind" },
  { nome: "Balmora", x: 1695, y: 527, tipo: 1, regiao: "Morrowind" },
  { nome: "Ald’ruhn", x: 1700, y: 428, tipo: 1, regiao: "Morrowind" },
  { nome: "Sadrith Mora", x: 1923, y: 450, tipo: 1, regiao: "Morrowind" },
  { nome: "Caldera", x: 1693, y: 468, tipo: 1, regiao: "Morrowind" },
  { nome: "Mournhold", x: 1812, y: 917, tipo: 1, regiao: "Morrowind" },
  { nome: "Molag Mar", x: 1805, y: 600, tipo: 1, regiao: "Morrowind" },
  { nome: "Suran", x: 1792, y: 589, tipo: 1, regiao: "Morrowind" },
  { nome: "Vos", x: 1830, y: 367, tipo: 1, regiao: "Morrowind" },
  { nome: "Tel Mora", x: 1857, y: 347, tipo: 1, regiao: "Morrowind" },
  { nome: "Gnisis", x: 1590, y: 375, tipo: 1, regiao: "Morrowind" },
  { nome: "Seyda Neen", x: 1660, y: 620, tipo: 1, regiao: "Morrowind" },
  { nome: "Dagon Fel", x: 1778, y: 246, tipo: 1, regiao: "Morrowind" },
  { nome: "Khuul", x: 1600, y: 305, tipo: 1, regiao: "Morrowind" },
  { nome: "Narsis", x: 1769, y: 924, tipo: 1, regiao: "Morrowind" },

  // ----- Solstheim -----
  { nome: "Raven Rock", x: 1490, y: 244, tipo: 1, regiao: "Solstheim" },

  // ----- Black Marsh -----
  { nome: "Helstrom", x: 1735, y: 1266, tipo: 0, regiao: "Black Marsh" },
  { nome: "Gideon", x: 1525, y: 1327, tipo: 1, regiao: "Black Marsh" },
  { nome: "Lilmoth", x: 1805, y: 1593, tipo: 1, regiao: "Black Marsh" },
  { nome: "Archon", x: 1885, y: 1452, tipo: 1, regiao: "Black Marsh" },
  { nome: "Soulrest", x: 1510, y: 1528, tipo: 1, regiao: "Black Marsh" },
  { nome: "Thorn", x: 1945, y: 1075, tipo: 1, regiao: "Black Marsh" },

  // ----- Valenwood -----
  { nome: "Elden Root", x: 970, y: 1393, tipo: 0, regiao: "Valenwood" },
  { nome: "Woodhearth", x: 634, y: 1349, tipo: 1, regiao: "Valenwood" },
  { nome: "Falinesti", x: 800, y: 1177, tipo: 1, regiao: "Valenwood" },
  { nome: "Silvenar", x: 868, y: 1161, tipo: 1, regiao: "Valenwood" },
  { nome: "Southpoint", x: 930, y: 1483, tipo: 1, regiao: "Valenwood" },
  { nome: "Haven", x: 1072, y: 1482, tipo: 1, regiao: "Valenwood" },
  { nome: "Marbruk", x: 826, y: 1318, tipo: 1, regiao: "Valenwood" },

  // ----- Elsweyr -----
  { nome: "Rimmen", x: 1296, y: 1161, tipo: 0, regiao: "Elsweyr" },
  { nome: "Torval", x: 1146, y: 1440, tipo: 1, regiao: "Elsweyr" },
  { nome: "Dune", x: 1114, y: 1095, tipo: 1, regiao: "Elsweyr" },
  { nome: "Rawl’kha", x: 1052, y: 1155, tipo: 1, regiao: "Elsweyr" },
  { nome: "Senchal", x: 1503, y: 1503, tipo: 1, regiao: "Elsweyr" },

  // ----- Summerset -----
  { nome: "Alinor", x: 182, y: 1437, tipo: 0, regiao: "Summerset" },
  { nome: "Cloudrest", x: 290, y: 1285, tipo: 1, regiao: "Summerset" },
  { nome: "Lillandril", x: 127, y: 1308, tipo: 1, regiao: "Summerset" },
  { nome: "Shimmerene", x: 405, y: 1338, tipo: 1, regiao: "Summerset" },
  { nome: "Sunhold", x: 258, y: 1485, tipo: 1, regiao: "Summerset" },
];

/* Procura o índice de um local pelo nome (-1 se não existir). */
const indiceDoLocal = nome => LOCAIS.findIndex(l => l.nome === nome);
