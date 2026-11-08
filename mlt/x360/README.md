# Bot Strike 360

O porte do Bot Strike para o Xbox 360 destravado (RGH/JTAG), feito com o [xsdk](https://github.com/xenosworks/xsdk).
O Godot não roda no 360, então o jogo é reescrito em C sobre o xsdk. Os assets continuam vindo **da sua cópia do
CS2**: o PC converte o que o Bot Strike já extrai em dois pacotes para o console (`dust2.pak` e `assets.pak`), e nada
do CS2 vai em download nenhum.

## Estado

| Parte | Estado |
| --- | --- |
| Dust II, colisão, movimento estilo Source | **rodou no 360** a 60 fps (etapa 1) |
| Menu principal (modo, lado, dificuldade, configurações) | testado no PC |
| Agentes SAS e Phoenix animados (idle, corrida, tiro, queda), com a arma na mão | testado no PC |
| Armas em primeira pessoa com os clipes do CS2 (draw, idle, tiro, recarga), brilho do cano, cápsulas | testado no PC |
| Tiro por raio (cabeça, peito, barriga, pernas, colete, capacete), tracos, marcas, sangue | testado no PC |
| Bots: compra, rota no navmesh, visão, mira com erro, plantar, desarmar, sair de travamento | testado no PC |
| Partida: rodadas, bomba, economia, meio-tempo, kill feed, placar, menu de compra, pausa | testado no PC |
| Build Fight: parede, chão, escada, cone, materiais, edição, queda sem apoio, paredes dos bots | testado no PC |
| Portal Strike: Portal Gun, portais, atravessar, tiros pelos portais, mapa tático, atalhos dos bots | testado no PC |
| Sons e músicas do CS2 | testado no PC |
| `.xex` completo | compila (`out/botstrike.xex`, 676 KB); **ainda não rodou no 360** |

Os testes no PC usam o mesmo código do jogo (só a tela, o controle e o áudio mudam): `out/botstrike_pc.exe`.

## Online (EPTX Live)

O jogo usa a conta do EPTX. No 360 ele lê a mesma sessão que a dash do EPTX salvou (`Hdd1:\EPTX` ou `Usb0:\EPTX`,
`eptx.ini` e `session.ini`): entre pela dash uma vez e o jogo já abre logado. No PC, se não houver sessão, o menu
ONLINE pede gamertag e senha e salva a sessão em `eptx\` ao lado do jogo (com o `eptx.ini` apontando o servidor).

- Abrindo, o jogo mostra **Logando...**; se não der, segue só local, contra bots, como antes.
- O menu ONLINE mostra os amigos do EPTX e o que estão jogando, os convites e a sala, e o ping UDP até o servidor
  (é o teste de UDP do console: "UDP 23 ms (5/5)").
- A presença vai para o servidor: os amigos (na dash do EPTX, no celular e no jogo) veem "Bot Strike 360 - Build
  Fight - CT 3 x 2 T".
- **Sala:** crie (escolhendo o modo), convide amigos online, e o host começa. Até 4 humanos, no mesmo time, contra os
  bots, em qualquer modo. A partida roda no host; os outros mandam o próprio movimento e as escolhas, e recebem o
  estado 20 vezes por segundo. Tudo passa pelo relay UDP do servidor do EPTX (porta do EPTX + 1, a 8361 por padrão):
  ninguém abre porta no roteador e o console só fala com o seu servidor.
- Os dois lados precisam do mesmo `dust2.pak` e `assets.pak` (cada um gera do próprio CS2): a sala avisa "PACOTE
  DIFERENTE" se não bater.

Servidor: o `D:\EPTX Live` ganhou `src/routes/rooms.ts` (salas, convites e o relay UDP). Reinicie o servidor do EPTX
para ele subir, e libere a porta UDP 8361 no firewall do PC do servidor.

Testado no PC: dois jogos (Host1 e Guest1) contra um servidor de teste, sala criada, convite aceito, Build Fight
sincronizado (relógio, placar, vivos, kill feed, mortes). **Ainda não testado no 360.**

## Gerar os pacotes

Antes: abra o Bot Strike no PC uma vez e jogue uma partida (ele lê o CS2 e assa o navmesh dos bots).

```bash
powershell -ExecutionPolicy Bypass -File C:\mlt\x360\tools\build_pack.ps1
```

```bash
powershell -ExecutionPolicy Bypass -File C:\mlt\x360\tools\build_assets.ps1
```

O primeiro faz o `dust2.pak` (mapa, colisão, spawns) e confere o movimento em todos os spawns. O segundo faz o
`assets.pak` (agentes, armas, viewmodels, animações, sons, HUD, navmesh) rodando o `proj/tools/x360_bake.tscn` dentro
do projeto do Bot Strike, para os modelos saírem montados exatamente como no PC, e gera o `src/defs.h` das planilhas.

## Jogar no PC (teste)

```bash
C:\mlt\x360\out\botstrike_pc.exe
```

Teclado e mouse como o Bot Strike original, ou um controle de Xbox (XInput). Para ver os bots jogando sozinhos:

```bash
set BS_AUTOPLAY=build && C:\mlt\x360\out\botstrike_pc.exe
```

(`classic`, `build` ou `portal`; `BS_FP=1` põe a câmera em primeira pessoa no lugar do jogador; `BS_SHOTS=pasta`
salva um quadro a cada 5 s; `BS_QUIT=segundos` fecha sozinho.)

## Compilar o .xex

```bash
wsl -d Ubuntu-24.04 -e bash /mnt/c/mlt/x360/build.sh
```

## No console

Arquivos: `botstrike.xex`, `dust2.pak` e `assets.pak` juntos em `Hdd1:\BotStrike\` (ou no pendrive, em
`Usb0:\BotStrike\`). O log sai ao lado do `.xex`, em `botstrike.log`.

| Controle | Ação |
| --- | --- |
| Analógico esquerdo / direito | andar / olhar |
| RT | atirar; no Build Fight, construir (segurar = turbo); editando, marcar quadrados |
| LT | mira da AWP; Portal Gun: mapa tático; construindo: troca o material |
| A / B / clique do esquerdo | pular / agachar / andar devagar |
| X | recarregar; segurar perto da bomba: desarmar |
| Y / LB | próxima / arma anterior |
| RB | Build Fight: entra no modo de construção e troca a peça |
| Clique do direito | Build Fight: editar a peça (de novo confirma) |
| Cruz para cima / para baixo | menu de compra / largar a bomba |
| Back / Start | placar / pausa (configurações, sair para o menu) |

No mapa tático: o analógico esquerdo move o ponteiro, A põe o portal laranja (numa estrela ou onde der para andar),
LB/RB giram a saída, B fecha.

### Por que não estraga o console

- Ele só **lê** arquivos. Não mexe em NAND, kernel, configurações, perfis nem saves. O único arquivo que escreve é o
  próprio log.
- Os dois pacotes são conferidos inteiros antes de qualquer byte ir para a GPU: índices, faixas, tamanhos, texturas,
  ossos, animações, navmesh. Se algo não bate, aparece uma tela com o motivo e Back volta para a dashboard.
- A memória da GPU tem um teto (320 MB); os vértices animados pela CPU vão para três buffers que se revezam, e a
  CPU nunca escreve num buffer que a GPU ainda está lendo.
- Os quadros ficam presos a 60 Hz pelo vblank. Se a GPU parar de responder, o jogo desiste em 5 s e volta para a
  dashboard. No pior caso, desliga no botão e nada fica gravado.
- Primeiro teste: tenha o FTP do DashLaunch ligado e mande o `botstrike.log` se algo der errado.

## Estrutura

```
src/main.c            o laço: carga, menu, partida, câmeras, modo automático
src/game.h            as estruturas do jogo
src/fighter.c         lutador, jogador, bots, viewmodel (fighter.gd, player.gd, bot.gd, viewmodel.gd)
src/match.c           rodadas, bomba, economia, efeitos, sons (match.gd)
src/build.c           Build Fight (build.gd)
src/portal.c          Portal Strike e o cálculo do mapa tático (portal.gd, tacmap.gd)
src/ui.c              comandos, HUD, compra, placar, pausa, mapa tático, menu (hud.gd, menu.gd)
src/world.c           raios, bombsites, navegação dos bots (A* + funil no navmesh do Godot)
src/anim.c            animação e pele na CPU
src/assets.c, pack.c  os leitores dos pacotes, com as conferências
src/collide.c         colisão e raios contra o mapa e as peças
src/player.c          movimento estilo Source
src/audio.c           mixer de software
src/gfx_xbox.c        desenho no 360 (xsdk)        src/gfx_gl.c   desenho no PC (OpenGL, para testar)
src/plat_xbox.c       controle, áudio, arquivos no 360   src/plat_pc.c  no PC
shaders/              map, model e fx (GLSL, compilados pelo xost do xsdk)
tools/                conversores (Godot e Python), check e viewer do mapa, scripts
```

## O que falta

- Rodar no 360 de verdade: é a primeira vez que modelos animados, efeitos com blend e a HUD passam pela GPU do console.
- A vista através do portal (no PC original, a renderização do outro lado): aqui o portal mostra só o redemoinho, como
  o `portal_view = 0` da planilha.
- Transparência do mapa (poeira, vapor), sombra do sol, e a textura `grape_leaves_01` (PNG corrompido no cache).
