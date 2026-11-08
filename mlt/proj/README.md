# Bot Strike: Dust II

Um jogo 5v5 contra bots feito em **Godot 4.7.2** que roda a partir da **sua própria cópia do Counter-Strike 2**.
Na primeira vez que abre, ele lê o Dust II, os agentes (SAS e Phoenix), as armas, os sons, os ícones do HUD,
a música do menu e as fontes direto da instalação do CS2 e guarda tudo num cache. Nada do CS2 vai junto no download.

Projeto do Melty: **Bot Strike: Dust II** (rascunho, versão 0.1.0). Detalhes de publicação em [docs/MELTY.md](docs/MELTY.md).

## Modos

| Modo | O que é |
| --- | --- |
| **Competitive vs Bots** | Você + 4 bots contra 5 bots. Bomba no Dust II, primeiro a 7 rounds, troca de lado no intervalo, tempo de compra, dinheiro e menu de compra. |
| **Build Fight vs Bots** | A mesma partida com a construção do Fortnite: grade de 5,12 m, **Q** parede, **X** chão, **C** escada, **V** cone (ou F1–F4), madeira/tijolo/metal, turbo build, edição com **F** e peças que caem quando perdem o apoio. Os bots levantam parede (e às vezes escada) quando você atira neles. |
| **Portal Strike vs Bots** | A partida clássica com uma **Portal Gun** no menu de compra (slot 4). Clique esquerdo abre o portal azul na parede ou no chão; o direito abre o mapa para escolher onde fica o laranja, com as rotas previstas dos inimigos e os lugares recomendados. Jogadores, bots e tiros atravessam os portais. |

Armas: Faca, Glock-18, USP-S, AK-47, M4A1-S, AWP, Portal Gun (só no Portal Strike), mais colete, capacete e kit de desarme.

## Como rodar (no seu PC)

```bash
"C:\mlt\pkg\BotStrike.exe" "--cs2=D:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive"
```

- A primeira abertura (ou depois de uma atualização do CS2) leva ~4–5 min: ele lê os arquivos do CS2 e monta o mapa.
- Depois disso o menu abre em segundos.
- Pelo Melty, o jogador só aperta Play: o Melty passa a pasta do CS2 sozinho (`--cs2={game}`).

## Controles

| Tecla | Ação |
| --- | --- |
| WASD / Espaço / Ctrl / Shift | andar / pular / agachar / andar devagar |
| Mouse esquerdo / direito | atirar / mira da AWP |
| 1 / 2 / 3 / 4 / 5 | primária / pistola / faca / Portal Gun / bomba |
| R | recarregar |
| B | menu de compra (no spawn, durante o tempo de compra) |
| E | desarmar a bomba |
| G | largar a bomba |
| Tab | placar |
| Esc | pausa (gráficos, sensibilidade, sair) |
| Q / X / C / V ou F1–F4 (Build Fight) | parede / chão / escada / cone; clique esquerdo coloca (segurar = turbo build); botão direito troca o material; 1/2/3 volta pras armas |
| F (Build Fight) | editar a peça do seu time que você mira: clique ou arraste nos quadrados para recortar, F confirma, botão direito desfaz a edição |
| Portal Gun (Portal Strike) | clique esquerdo: portal azul onde você mira · botão direito: mapa para o portal laranja (clique numa ★ ou em qualquer lugar; a rodinha gira a saída) |

## Configurações (⚙ no menu principal)

- **Qualidade gráfica**: Low / Medium / High, mais **Render scale**. O portal mostra o outro lado em todas as qualidades (custa uma renderização extra por portal visível; no Low ela é feita em resolução menor).
  No seu PC (Radeon HD 6670, modo OpenGL), use **Low**: ~32–37 FPS a 1600×900.
- **Sensibilidade** (mesma escala do CS2), **FOV**, volume geral e da música, tela cheia.
- **Viewmodel right/left, up/down, back/forward**: posição da arma em primeira pessoa (como `viewmodel_offset_x/y/z` do CS2).
- Contador de FPS no canto superior esquerdo.

## Onde estão as coisas

| Pasta / arquivo | O que tem |
| --- | --- |
| `C:\mlt\proj` | o projeto Godot (código, planilhas de design, ferramentas) |
| `C:\mlt\proj\design\*.json` | as planilhas de design: a fonte da verdade (armas, modos, assets, gráficos…) |
| `C:\mlt\proj\scripts\*.gd` | o código do jogo |
| `C:\mlt\proj\tools\preflight.py` | confere as planilhas e gera `scripts/gen/defs.gd` |
| `C:\mlt\proj\tools\release\` | scripts de empacotar e enviar pro Melty |
| `C:\mlt\pkg` | a build pronta (BotStrike.exe + BotStrike.pck + leitor de arquivos do CS2) |
| `C:\mlt\BotStrike-0.1.0.zip` | o pacote enviado ao Melty |
| `C:\mlt\tools` | Godot 4.7.2 e Source2Viewer-CLI 20.1 usados pra construir |
| `%APPDATA%\BotStrike\cs2cache` | cache com o que foi lido do CS2 (pode apagar; ele refaz) |
| `%APPDATA%\BotStrike\botstrike.log` | log do jogo |
| `%APPDATA%\BotStrike\settings.cfg` | suas configurações |

Mais documentação:

- [docs/ARQUITETURA.md](docs/ARQUITETURA.md) – como o jogo é montado, sistema por sistema
- [docs/DESCOBERTAS.md](docs/DESCOBERTAS.md) – tudo que aprendemos sobre os arquivos do CS2, viewmodel e desempenho
- [docs/COMO_CONSTRUIR.md](docs/COMO_CONSTRUIR.md) – como mudar algo, testar e gerar uma nova build
- [docs/MELTY.md](docs/MELTY.md) – status no Melty, receita, testes
- [CHANGELOG.md](CHANGELOG.md) – o que mudou em cada etapa

## Créditos

Godot Engine 4.7.2 (MIT) e Source 2 Viewer / ValveResourceFormat 20.1 (MIT), incluídos com as licenças em `licenses/`.
Counter-Strike 2 e Dust II são © Valve. Mod feito por fã, sem ligação com a Valve. Licença do mashup: All rights reserved, remixes permitidos.
