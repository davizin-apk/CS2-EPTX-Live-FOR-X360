# EPTX NXE Edition

A dash do EPTX sobre a interface do **Freestyle Dash** (FSD 2.0, TeamFSD), no visual da
NXE. O fundo, as legendas das abas, os blocos em perspectiva, o canto do perfil, os
botões do controle, os painéis e as linhas de lista são as cenas e a arte do próprio FSD;
contas, amizades, Store e multiplayer são os serviços do EPTX.

Abas: **Home, Games, Friends, Store, Profile, Multiplayer**.

---

## Isolamento do EPTX Live

Esta edição mora inteira em `D:\EPTX-NXE-Edition` e não muda nada do EPTX Live:

| O quê | EPTX Live | Esta edição |
|---|---|---|
| Código | `D:\EPTX Live` | `D:\EPTX-NXE-Edition\client` (cópia de `client/core` e `client/xbox`) |
| Build no WSL | `~/eptx-xbox-build`, `~/eptx-preview-build` | `~/eptx-nxe-xbox-build`, `~/eptx-nxe-preview-build`, `~/eptx-nxe-xui-build` |
| Saída | `client/build/xbox/eptx.xex` | `client/build/xbox/eptx_nxe.xex` desta pasta |
| Title ID do pacote | `45505458` ("EPTX") | `4550584E` ("EPXN"): outra pasta em `Hdd1:\Content` |
| Dados no console | `Hdd1:\EPTX\` | `Hdd1:\EPTX-NXE\` (sessão, log) |

Do EPTX Live, esta edição só **lê**: o `eptx.ini` em `Hdd1:\EPTX\` (para achar o servidor,
se `Hdd1:\EPTX-NXE\eptx.ini` não existir), o empacotador `src/tools/stfs-pack.ts` (executado,
gravando aqui) e o código do servidor nos testes (com banco próprio, veja abaixo). O backend
não foi alterado. Ao fim do trabalho, um manifesto SHA-1 de todos os arquivos do EPTX Live
(fora `node_modules` e `data`) bateu com o de antes: 143 arquivos, nenhuma diferença.

---

## Por que o FSD não é compilado como está

O repositório `freestyledash/` é o clone de https://github.com/XboxUnity/freestyledash,
intocado. A interface do FSD é XUI: o `Freestyle.vcproj` linka `xuirun.lib`,
`xuirender.lib`, a ATG e o `xuipkg` — o runtime XUI da Microsoft, que só vem com o **XDK**
oficial (com Visual Studio 2005/2008). Esta máquina tem o xsdk (homebrew, sem XDK), e o
código C++ do FSD não roda sem o XDK. Da liberação de 2011 falta ainda `Tools/Generic/XeCrypt.h`
(o README do FSD avisa que nem tudo foi liberado).

A saída foi reaproveitar a interface onde ela de fato mora: nas cenas `.xui`. Elas são o
XML que o XUI Tool grava — posições, tamanhos, imagens, textos, figuras com degradê,
visuais com estados (Normal, Focus, Press...) e timelines com quadros-chave. O
`client/xbox/xui.cpp` é um engine XUI escrito para o xsdk que lê esses arquivos e os desenha
com o Dear ImGui do imgui-xenon:

- `XuiScene`, `XuiGroup`, `XuiImage`, `XuiText`, `XuiFigure` (sólido, degradê linear e radial,
  com a transformação do pincel: escala, rotação e translação — o modelo foi ajustado até os
  cantos e bordas das molduras dos botões fecharem com a mesma espessura e o fundo dos blocos
  continuar igual ao `preview.png`), `XuiNineGrid`, os presenters e
  os controles com `Visual` da `skin.xui`, com âncoras em cascata quando o controle tem outro
  tamanho que o visual — é assim que as molduras dos botões (cantos e bordas dentro de grupos)
  acompanham a largura da linha;
- `XuiSoundXAudio`: quando a timeline de um estado tocado passa por um quadro-chave com `File`,
  o som toca (o `btn_Focus` do foco, o `a-button` do Press das dicas...);
- timelines a 60 quadros/s, interpolação linear, em degrau e suavizada, quadros nomeados,
  `stop`/`goto`/`gotoandplay`, e a parada no `End...` de cada estado;
- profundidade (`Position` z) em perspectiva — a mesma que põe os blocos onde o
  `preview.png` da skin do FSD mostra.

Fora: mistura aditiva (`BlendMode`), máscaras por canal (`ColorWriteFlags` só de alfa: o
elemento é pulado), HTML e as listas XUI (as linhas são montadas pelo código com os visuais
`Button_*` da skin). A moldura `disabledBox` dos botões fica escondida: ela não tem timeline e
ficaria acesa em toda linha, e aqui nenhum botão é desabilitado.

### Sons

Os efeitos do FSD (`Skins/Default/Audio/*.xma`) são XMA2, que o xsdk não decodifica. O
`pack.mjs` os converte com o **ffmpeg** (instalado com `winget install Gyan.FFmpeg`; ou
`FFMPEG=...`) em PCM de 48 kHz estéreo, `Skin/Audio/*.pcm` — 37 sons, 5,7 MB; a música dos
créditos fica de fora. No console, `sound.c` alimenta o driver de áudio do kernel
(`xsdk_audio_*`) por uma thread própria, misturando até 8 sons nas caixas da frente.

O que toca é o que as cenas do FSD mandam: foco de linha (`btn_Focus`), botões A/B/X/Y/Back
(`a-button`, pelo Press dos visuais `Btn_Button_*`). Nas transições que esta dash faz por
código entram os sons que as timelines do FSD usam nelas: troca de aba (`snd_channeldown`/
`snd_channelup`, do MainScene), blocos (`snd_panelright`/`snd_panelleft`, do ScnMain), painel
(`snd_panelunfold`), Guide (`HUD_open`/`HUD_close`) e avisos (`NotifyPopup`). O preview confere
que esses sons são pedidos; **se eles soam certo só o console diz.**

Validação do engine: `client/xbox/preview/xui_build.sh` desenha o `MainScene` do `main.xui`
original sem nada do EPTX, para comparar com o `preview.png` da skin. As posições dos
blocos batem com a captura do FSD a poucos pixels.

## O que vem do FSD e o que é do EPTX

| Da skin Default do FSD | Do EPTX |
|---|---|
| `CustomBackdrop`: papel de parede, palco, animações de fundo | as abas e o que cada bloco mostra |
| `Tab1Caption` e `BulletFigure`: as legendas e as posições delas (MainScene, `1To2`...) | a ordem e os nomes das abas |
| `Tab2` da `ScnMain`: o bloco 420x320, fundos `Content_Bkg_*`, ícones verdes, reflexo, sombra, `ContentItem_Active` | avatar, segunda linha, barra de download por cima do bloco |
| posições dos blocos (`2To3`, `3To4`...): focado em (100,250,0), +570/+115/+300 por passo | a lógica de navegação (o que a `ScnMain` fazia em C++) |
| `ProfileScene`, `TabCounter`, `Btn_Button_A/B/X/Y/Back` | gamertag, situação da conexão, contagem |
| `Panel_Background`, `Button_Simple`, `Button_TwoLine_Icon`, `Animation_Loading` | o conteúdo dos painéis |

O emblema do canto, o avatar desenhado, o editor de avatar, o aviso de download e o Guide
(Back) vieram da dash do EPTX Live; o Guide ainda está no desenho antigo, não nas cenas do FSD.

---

## Telas

Cima/baixo trocam de aba (dando a volta, como no FSD); esquerda/direita escolhem o bloco;
A seleciona; B volta; Y atualiza; Back abre o Guide.

- **Home**: perfil, convites e pedidos (quando há), downloads, amigos online, dashboard
  original, atualizar, sobre.
- **Games**: biblioteca (o que está no HD), um bloco por jogo, downloads.
- **Friends**: Adicionar Amigo, Solicitações (N), um bloco por amigo, online primeiro.
- **Store**: Pesquisar, um bloco por título do catálogo com a capa.
- **Profile**: perfil e conexão, editor de avatar, servidor, entrar/sair.
- **Multiplayer**: a sala atual (ou Criar Sala), convites, amigos online.

Painéis: detalhe do jogo, resultados da pesquisa, biblioteca, perfil, amigo, solicitações,
adicionar amigo, sala, criar sala, convites, confirmação (remover amigo, sair, fechar sala),
sobre. Cada ação mostra no painel o que o **servidor** respondeu: "Falando com o servidor..."
enquanto espera, depois a confirmação ou o erro dele em português. Nada é simulado localmente.

---

## Perfil do Xbox

A dash só abre com um perfil logado no console (o XAM diz qual, duas vezes por segundo); sem
ele, aparece só o aviso para escolher um pelo botão Xbox. Cada perfil tem a sua conta EPTX: a
sessão fica em `EPTX-NXE\profiles\<XUID>.ini`, e trocar de perfil troca de conta sem apagar a
do anterior. Um perfil sem conta pede login com a gamertag dele já escrita; o `user=` do
`eptx.ini` só entra sozinho no perfil de mesma gamertag.

O pacote do perfil (`Content\<XUID>\FFFE07D1\00010000\<XUID>`, procurado em `hdd:`, `usb:`,
`usb1:`, `usb2:`, `mu:` e `mu1:`) dá o XUID e a **foto do jogador** (a miniatura do cabeçalho).
Ela vai para o servidor como o avatar da conta (`PUT /api/v1/me/avatar`, image/png) sempre que
muda, e os amigos a veem no lugar do boneco. O avatar 3D do Xbox não: só a biblioteca de avatares
do XDK o desenha, e o xsdk não a tem. Sem o pacote no disco, a conta usa a gamertag como chave e
nenhuma foto é enviada.

## Serviços EPTX

Todas as chamadas passam por `api()` em `client/xbox/app.c`: lá um 401 encerra a sessão
("Sua sessão no EPTX expirou") e a falta de resposta apaga o indicador do servidor (o ponto
verde/vermelho no canto do perfil). A sessão salva é só token e segredo
(`Hdd1:\EPTX-NXE\session.ini`); a senha nunca é gravada.

| Função | Rota |
|---|---|
| Login, sessão salva, presença | `POST /auth/login`, `GET /me`, `POST /presence` |
| Amigos com presença e avatar | `GET /friends` (agora com `friendshipId` e `userId`) |
| Pedidos recebidos e enviados | `GET /friends/requests` |
| Enviar pedido | `POST /friends/requests {gamertag}` |
| Aceitar | `POST /friends/requests/:id/accept` |
| Recusar pedido, remover amigo | `DELETE /friends/:id` (novo `eptx_client_delete` no núcleo) |
| Procurar gamertag | `GET /avatar/:gamertag/avatar-body.png` (404 = não existe; senão mostra o avatar) |
| Store e pesquisa | `GET /catalog`, `GET /catalog?q=`, `GET /catalog/:id`, arte, download |
| Salas | `GET /rooms/current`, `GET /rooms/invites`, `POST /rooms`, `/rooms/:id/invite`, `/join`, `/leave` |

Com Friends ou Multiplayer na tela, ou um painel de amigo/pedido/sala aberto, a dash
consulta a cada 5 s; numa sala, a cada 20 s (o servidor tira da sala quem some por 60 s);
fora disso, a cada minuto. Pedidos novos, amigos que entram e quem chega na sala aparecem
sem reiniciar.

### O que o backend não tem (e a dash não inventa)

- **Busca de usuários**: não há rota de busca. Adicionar Amigo usa a gamertag exata; a
  rota pública do avatar confirma que a conta existe e mostra o avatar antes de enviar. A dash
  já avisa se é você mesmo, se já são amigos, se o pedido já foi enviado ou se a pessoa já
  te pediu (aí o botão vira "Aceitar"). O servidor repete essas checagens.
- **Preços e compras**: o catálogo não tem preço nem rota de compra. A Store mostra o que
  existe — capa, tipo, tamanho, Title ID, editora, descrição e disponibilidade (instalável,
  já instalado, aparece em Meus Jogos ou vai para `Hdd1:\Games`, ISO indisponível) — e
  instala com o download que já existia.
- **Multiplayer e crossplay**: o servidor tem salas (até 4, host, convites só para amigos) e
  um relay UDP. A dash cria, convida, entra, sai e mostra quem está conectado ao relay; a
  **partida** é do jogo (hoje, só o Bot Strike usa as salas). Crossplay 360/PC depende de
  cada jogo falar o mesmo protocolo do relay — não é algo que a dash resolva.

---

## Testes

**Integração com o servidor** (`tests/run-integration.ps1`): compila o `app.c` desta edição
em modo PC (com o zig) e roda contra o código do servidor do EPTX Live numa instância de
teste — porta 18360, banco e biblioteca em `tests/.work`. Três contas, sete passos:

```bash
powershell -ExecutionPolicy Bypass -File tests/run-integration.ps1
```

Confere, com respostas reais do servidor: login; busca de gamertag (existe/não existe);
pedido enviado, repetido, para si mesmo e para gamertag inexistente; aceitar; aceitar de
novo (recusado); recusar; criar sala, convidar amigo, convidar quem não é amigo (recusado);
entrar e sair da sala; remover amigo; pesquisa na Store; sessão revogada no servidor
derrubando a dash para "entrar". Último resultado: todos os passos passaram.

**Preview e navegação** (`client/xbox/preview/build.sh`): o mesmo `dash.cpp`, com o xui.cpp e
a skin do FSD, rasterizado no PC. Grava 27 capturas em `client/build/xbox/preview/` e aperta os
botões do controle de verdade (direcional, A, B, X, Y, Back e o teclado do console), conferindo
o que a tela pede a `app.c`: aceitar o id certo, recusar com X, instalar o título certo, convidar
o `userId` certo, remover a amizade certa, criar e entrar na sala. Último resultado: tudo certo.

```bash
wsl -d Ubuntu-24.04 -e bash "/mnt/d/EPTX-NXE-Edition/client/xbox/preview/build.sh"
```

**Console**: o `.xex` compila com a toolchain do xsdk (PowerPC, `-Wall -Wextra -Werror`), e o
pacote XBLA é gerado e relido byte a byte pelo empacotador. **Nada disso rodou num Xbox 360
ainda.** O preview não testa a GPU, a memória nem o desempenho do console.

### O que olhar primeiro no console

- **A skin dentro do pacote**: a dash lê `game:/Skin/...` com caminhos que têm espaço
  (`Images/Interface/Content Tabs/...`). Se aparecer a tela "A skin do Freestyle Dash não foi
  encontrada", o log diz qual arquivo faltou. Use o `.xex` solto (abaixo) para depurar.
- **Memória e tempo de carga**: cada imagem da skin é lida e decodificada na primeira vez que
  uma cena pede (o fundo e o palco têm 1280x720). A `skin.xui` (2,6 MB de XML) é lida no início.
- **Desempenho**: cada quadro avalia as timelines e projeta os vértices na CPU; os blocos
  têm figuras com degradê radial. No PC é folgado; no PowerPC não foi medido.

---

## Compilar, empacotar e instalar

Uma vez, o ambiente do EPTX Live (xsdk em `~/xsdk`, toolchain em `~/xsdk-gcc`, imgui-xenon
em `~/imgui-xenon`) — só lido, nada nele é mudado. Depois:

```bash
wsl -d Ubuntu-24.04 -e bash "/mnt/d/EPTX-NXE-Edition/client/xbox/build.sh"
```

```bash
node D:/EPTX-NXE-Edition/client/xbox/pack.mjs
```

O pacote sai em `client/build/xbox/<content ID>`. Envie por FTP para
`Hdd1:\Content\0000000000000000\4550584E\000D0000\`, crie `Hdd1:\EPTX-NXE\eptx.ini` a partir
de `client/xbox/eptx.ini.example` (ou deixe o do EPTX Live em `Hdd1:\EPTX\`) e reinicie. Ele
aparece em Meus Jogos como "EPTX NXE Edition", ao lado do EPTX Live.

Solto, para o DashLaunch: `client/build/xbox/solto/` vai inteiro para `Hdd1:\EPTX-NXE\`
(`eptx_nxe.xex` com a pasta `Skin` ao lado). Teste por Meus Jogos antes de pôr como dash padrão.

---

## Licenças

As cenas e as imagens da skin são do Freestyle Dash, sob **GPL versão 3**
(`freestyledash/LICENSE.md`), e vão no pacote como arquivos separados, com a licença em
`licenses/freestyledash-GPLv3.md`. Quem distribuir o pacote precisa oferecer também essas
fontes (o repositório do FSD). O resto segue o EPTX Live: xsdk (MIT, com partes BSD), Dear
ImGui (MIT), Selawik (OFL), stb (domínio público).

## Estrutura

```
EPTX-NXE-Edition/
├── freestyledash/            clone do FSD, intocado (a skin sai de Skins/Default)
├── client/
│   ├── core/                 núcleo do EPTX (cópia) + eptx_client_delete
│   ├── xbox/
│   │   ├── xui.h, xui.cpp    engine XUI: lê e desenha as cenas do FSD
│   │   ├── sound.h, sound.c  mixer dos sons do FSD no áudio do xsdk
│   │   ├── dash.cpp          abas, blocos, painéis e controle sobre as cenas do FSD
│   │   ├── app.h, app.c      serviços EPTX: api(), pedidos, busca, salas, sessão
│   │   ├── build.sh          .xex (WSL, build própria)
│   │   ├── pack.mjs          pacote XBLA com a skin
│   │   └── preview/          preview no PC, navegação, e o xui_main da validação do engine
│   └── build/xbox/           .xex, pacote, solto/ e capturas do preview
└── tests/
    ├── app_test.c            passos de integração (app.c em modo PC)
    └── run-integration.ps1   sobe o servidor de teste e roda os passos
```
