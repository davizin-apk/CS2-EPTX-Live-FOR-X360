# Arquitetura

O Bot Strike é um programa Godot próprio que **lê** o conteúdo do CS2 instalado no PC do jogador.
Ele nunca abre nem modifica o CS2 (sem risco de VAC). No Melty é um mashup em modo `standalone`.

```
BotStrike.exe --cs2=<pasta do CS2>
   │
   ├─ boot (prep.gd)    acha o CS2 → exporta assets com Source2Viewer-CLI → monta o mapa → cache
   ├─ menu (menu.gd)    Dust II de fundo, agente com arma, painel PLAY, configurações
   └─ match (match.gd)  5v5: rounds, bomba, economia, construção, bots, HUD
```

## Planilhas de design (`design/*.json`) – a fonte da verdade

Cada planilha é uma tabela: cada linha é uma coisa, cada coluna uma propriedade.
`tools/preflight.py` cruza todas, acusa célula vazia ou referência quebrada, e gera
`scripts/gen/defs.gd` (um dicionário constante por planilha). **Mude a planilha antes do código.**

| Planilha | Linhas | Usada por |
| --- | --- | --- |
| `weapons.json` | knife, glock, usp, ak47, m4a1s, awp, portalgun – dano, pente, preço, velocidade, conjunto de animação, cápsula, `only_mode` (portalgun só no modo portal) | fighter, player, bot, hud |
| `equipment.json` | colete, colete+capacete, kit | fighter, hud |
| `modes.json` | classic, build, portal – rounds, tempos, economia, `building`, `portals` | match, menu |
| `build_pieces.json` | wall, floor, ramp (escada), cone – custo, vida, tempo, tecla, tecla F1–F4, grade de edição (3 ou 2) | build, player, bot, hud |
| `build_materials.json` | wood, brick, metal – multiplicador de vida e tempo, cor, textura do Dust II, quantidade inicial e por abate | build, match, hud |
| `maps.json` | de_dust2 – assets do mapa, caixas dos bombsites (reserva) | world |
| `teams.json` | CT, T – agente, classe de spawn, pistola inicial, nomes dos bots | match, fighter |
| `bot_skills.json` | easy, normal, hard – reação, erro de mira, FOV, chance de construir | bot |
| `graphics.json` | low, medium, high – sombras, SSAO, glow, MSAA, render scale, distâncias, LOD, `portal_view` (escala da vista pelo portal; 0 = só o redemoinho) | game, world, portal |
| `assets.json` | tudo que é lido do CS2: mapa, física, entidades, agentes, armas, cápsulas, clipes de animação, sons, ícones, fontes | prep, game |
| `systems.json` | cada sistema do jogo, seu script e o check de teste do Melty | preflight |

`assets.json` aceita **alternativas** separadas por `|` em `path` e `out`: atualizações do CS2 renomeiam
arquivos (ver [DESCOBERTAS.md](DESCOBERTAS.md)); o primeiro arquivo real vence.

## Scripts

| Script | Papel |
| --- | --- |
| `game.gd` (autoload `Game`) | caminhos (CS2, cache, Source2Viewer), configurações, carregar glb/wav/svg/ttf em runtime, gráficos, animação dos agentes, modo de teste do Melty, log, modo de captura |
| `prep.gd` | tela de boot: exporta os assets com o Source2Viewer-CLI em grupos, assa o mapa (`de_dust2_baked_v3.scn`), gera LODs |
| `world.gd` | instancia o mapa, tira as luzes do CS2 e malhas bloqueadoras, cria colisão (trimesh), occluder, spawns, bombsites, sol/céu, navmesh dos bots (cacheado por patch) |
| `menu.gd` | menu estilo CS2, câmera que procura espaço aberto, configurações |
| `match.gd` | rounds, bomba (plantar/desarmar/explodir), economia, meio-tempo, construção (origem da grade, apoio, desabamento), portais (abrir, atravessar, atalhos dos bots), efeitos (tracer, impacto, cápsulas, flash do cano), espectador |
| `fighter.gd` | base de jogador e bot: vida, colete, dinheiro, armas, tiro (raycast), partes do corpo, modelo do agente em 3ª pessoa |
| `player.gd` | câmera em 1ª pessoa, movimento estilo Source (aceleração/atrito/air strafe), compra, plantar/desarmar, construir |
| `viewmodel.gd` | braços e arma em 1ª pessoa com os clipes do próprio CS2 (ver abaixo) |
| `bot.gd` | IA: compra, rota no navmesh até o site, ver/mirar/atirar, plantar, desarmar, construir parede quando leva tiro, sair de travamento |
| `build.gd` | peças do Fortnite (parede/chão/escada/cone) na grade de 5,12 × 3,84 m, materiais, crescem até a vida máxima, edição (recortar quadrados, girar escada), holograma azul |
| `portal.gd` | portal (oval na superfície ou de pé no chão), transformação de um portal para o gêmeo, vista pelo portal (SubViewport), modelo da Portal Gun |
| `tacmap.gd` | mapa do botão direito: radar desenhado do navmesh, rotas previstas dos inimigos, recomendações para o portal laranja |
| `hud.gd` | HUD estilo CS2, menu de compra, placar, pausa (gráficos/sensibilidade), FPS |
| `ui.gd` | fábrica de UI (cores, botões, painéis) com as fontes do próprio CS2 |
| `testmode.gd` | vigia do teste do Melty (loga e falha checks que nunca acontecem) |

## Fluxo de assets

1. `prep.gd` agrupa as linhas de `assets.json` por VPK e tipo e chama o Source2Viewer-CLI:
   - mapa/física → `--gltf_export_format glb --gltf_export_materials --gltf_textures_adapt`
   - agentes/armas/cápsulas → glb (+ lista de animações para versões antigas do CS2)
   - clipes de animação (`.vnmclip_c`) → glb com animação
   - sons, ícones svg, entidades → `-d` (decompilar)
2. Só **mapa, física, entidades, gatilhos e modelos** são obrigatórios; som/ícone/fonte faltando só gera aviso.
3. O mapa é assado uma vez: tira materiais "tools", texturas reduzidas (1024 cor / 512 normal, S3TC), descarta ORM, gera LODs.
4. `stamp.json` guarda versão do prep (`PREP_VERSION`) + patch do CS2; se mudar, exporta de novo.

## Agentes em 3ª pessoa

- Modelos em `agents/models/<agente>/` (CS2 novo) ou `characters/models/` (antigo).
- Animações: no CS2 novo vêm em clipes separados (`animation/anims/world/rifle/_default_rifle/idle_rifle` etc.).
  `Game.animate_agent()` copia as trilhas por **nome de osso** pro esqueleto do agente e gira a pelve pro referencial do agente.
- Arma na mão: montada num ponto fixo do corpo (`Game.grip_offset`), porque as poses de rifle seguram as mãos no peito.
- Morte: o agente tomba (não há clipe de morte exportado).

## Viewmodel (1ª pessoa)

Cada clipe de viewmodel do CS2 tem **dois esqueletos**: braços (`viewmodel.vnmskel`) e arma (`<arma>.vnmskel`).

- Braços: as malhas `firstperson_*` do agente são re-skinadas por nome de osso no esqueleto dos braços.
  Ossos que faltam (twist, clavícula) seguem o ancestral mais próximo mantendo o offset de repouso.
- Arma: as malhas do modelo da arma são re-skinadas no esqueleto da arma do clipe, então ferrolho, slide,
  pente, cão e gatilho se mexem com o clipe.
- O esqueleto da arma **não pode mudar de lugar na árvore** (as trilhas do clipe apontam pro caminho dele):
  ele segue o osso `wpn` dos braços por um `RemoteTransform3D`, com a rotação `WPN_TILT`.
- Faca: vai direto no osso `wpn` com `KNIFE_FIX` (lâmina pra cima).
- Conjuntos: AK-47, Glock-18, AWP têm clipes próprios; USP-S e M4A1-S usam os padrões (feitos pra elas).
- A cada tiro: clipe `shoot`, flash no osso `muzzle`, cápsula do calibre certo saindo da janela de ejeção.
- Posição: `vm_x / vm_y / vm_z` nas configurações.

## Bots

- Navmesh assado em runtime a partir da colisão do mundo (sem o céu), salvo em `nav_de_dust2_<patch>_v3.res`.
- T escolhem um site por round; um carrega a bomba, planta quando está no site sem inimigo à vista.
- CT se dividem entre A e B; quando a bomba é plantada, o mais perto vai desarmar.
- Visão: FOV/alcance da dificuldade, raycast até peito/cabeça; tempo de reação; erro de mira que diminui; mira no corpo e às vezes na cabeça.
- Build Fight: ao levar tiro, levantam parede na direção do atirador (com cooldown), às vezes com uma escada atrás, usando o material que têm mais.
- Portal Strike: metade compra a Portal Gun quando sobra dinheiro; alguns segundos depois do início abrem o azul numa parede perto e o laranja no lugar recomendado perto do objetivo.
  A cada novo caminho (2,5 s) comparam o caminho direto com "até um portal + do gêmeo até o objetivo" e usam o portal se for bem mais curto.
  Enxergam através dos portais e miram na posição "virtual" do inimigo (o inimigo trazido pelo portal de volta), então o tiro atravessa e acerta.

## Construção (Fortnite)

- Grade de 5,12 m na horizontal (alinhada ao mundo) e 3,84 m de altura. O nível 0 é o da estrutura mais próxima (para as peças se alinharem) ou o chão sob os pés.
- Onde a peça vai: parede na borda do seu quadrado para onde você olha (olhar para cima sobe um nível), chão à frente (olhando para baixo fica embaixo, para cima vira teto),
  escada à frente subindo para longe (passar do meio de uma escada conta como o próximo nível, por isso o "ramp rush" funciona segurando o clique), cone sobre a cabeça.
- Cada lugar tem nome (`key`): uma borda vale pelos dois quadrados, então não dá para pôr duas paredes no mesmo lugar.
- Só dá para construir encostado no chão ou em outra peça; quando uma peça quebra, `check_support()` derruba tudo que ficou sem caminho até uma peça apoiada no chão.
- Edição: parede 3×3, chão/cone 2×2, escada 2×2 (arrastar de um quadrado para outro gira a escada). Remover todos os quadrados não vale (como no Fortnite).

## Portais

- Cada dono tem um par azul/laranja; o par só funciona com os dois abertos. Todos atravessam qualquer par.
- `through()` = gêmeo × giro de 180° em Y × inverso do portal: leva ponto/direção que entra pela frente de um para a saída pela frente do outro.
- Atravessar: um ponto do corpo (pé, centro, cabeça) dentro do oval e a menos de 45 cm da frente, sem estar se afastando. Sai com pelo menos 3 m/s para fora e 0,4 s sem outra viagem.
- Tiros: `_trace` procura o primeiro portal no segmento antes da parede (`portal_on_segment`) e continua do gêmeo com o alcance que sobrou (até 3 portais seguidos).
- Vista pelo portal (Medium/High): uma câmera num SubViewport fica em `through() * câmera do jogador`; o oval mostra a textura pelas coordenadas de tela.
  Portais e viewmodel ficam em camadas de render que essa câmera não vê.
