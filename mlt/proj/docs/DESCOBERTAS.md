# Descobertas (o que aprendemos fazendo)

Coisas não óbvias que custaram tempo. Leia antes de mexer.

## Arquivos do CS2

- **Exportação**: Source2Viewer-CLI 20.1 (ValveResourceFormat) exporta mapa, física, modelos e clipes para glTF/glb,
  e sons para wav/mp3. Mapa em metros; Source → Godot = `(x, y, z) → (y, z, x) × 0,0254`.
- **Dust II**: `game/csgo/maps/de_dust2.vpk`. Exportar o `.vmap_c` gera `de_dust2.glb` (render, ~3200 malhas, ~640 texturas)
  e `de_dust2_physics.glb` (gatilhos: bombsites, buyzones).
- **Colisão do mundo**: no CS2 antigo era `maps/de_dust2/world_physics.vphys_c`; na atualização de 29/10/2026 virou
  `world_physics.vmdl_c`, que exporta como `world_physics_physics.glb`. Grupos: `physics_group_*` (sólido),
  `passbullets`/`playerclip` (só bloqueia jogador), `grenadeclip` (ignorar), `sky` (só jogador, fora do navmesh).
- **Entidades** (`default_ents.vents_c`): spawns `info_player_terrorist/counterterrorist`, `light_environment`.
  A atualização mudou o formato dos vetores de `"x y z"` para `[ x, y, z ]` – o parser aceita os dois.
- **Bombsites**: lidos dos `func_bomb_target` do `de_dust2_physics.glb` (unidades Source); caixas em `maps.json` são reserva.
- **Agentes**: na atualização foram de `characters/models/` (hoje só um stub com material laranja = a "caixa laranja")
  para `agents/models/`. As animações saíram do modelo para clipes AnimGraph2 (`.vnmclip_c`) em
  `animation/anims/world/...` (3ª pessoa) e `animation/anims/viewmodel/...` (1ª pessoa).
- **Clipes**: o esqueleto do clipe tem `root_motion` em Z-up; o do agente é identidade. Para a pelve:
  `pos' = R·pos`, `rot' = R·rot`, com `R` = rest do `root_motion` do clipe.
- **Som ambiente**: `sounds/ambient/wind/...` foi para `sounds/ambient/common/wind/...`.
- **Cápsulas**: `models/models/weapons/shared/shell_9mm_hr`, `shell_762_hr`, `shell_50cal_hr`. Saem com ~2 m no glb:
  escalar para 2,1 cm / 3,9 cm / 7 cm.
- **Instalação beta** (`D:\SteamLibrary\steamapps\cs2 beta`) só tem Dust II e é de outro patch; o Melty usa a instalação principal.

## Visual

- **Tudo branco**: o glb do mapa traz as luzes do próprio CS2 (`KHR_lights_punctual`) em unidades do CS2 – enormes no Godot.
  Remover todas as `Light3D` do mapa e usar nosso sol/céu.
- **Formas pretas**: `black_simple` (bloqueadores que o CS2 não desenha) e materiais `tools*` → material invisível.
  Superfícies de blend guardam pesos em cores de vértice → desligar `vertex_color_use_as_albedo`.
  Overlays `transition_overlay` dependem do shader do CS2 → esconder.
- **Câmera do menu** olhava pra parede: raycast em 16 direções e escolher a mais aberta.
- **Malhas re-skinadas** mantêm a AABB antiga → definir `custom_aabb` grande, senão somem por culling.

## Viewmodel (o caminho que funcionou)

1. Braços: re-skin das malhas `firstperson_*` por **nome** de osso (o skin original liga por índice).
   Ossos twist não existem no esqueleto do viewmodel → seguir o ancestral com o offset de repouso; senão os braços rasgam.
2. Raiz do viewmodel: `Basis(UP, PI)` (os clipes esticam os braços em +Z, a câmera olha −Z).
3. Arma: re-skin no esqueleto da arma do próprio clipe (mesmos nomes de osso que o modelo da arma).
4. Esqueleto da arma segue o osso `wpn` via `RemoteTransform3D` com `WPN_TILT = Basis(UP, PI/2) * Basis(BACK, PI/2)`.
   **Não reparentar** o esqueleto: as trilhas do clipe usam o caminho dele e as peças congelam (foi o bug "arma não reage").
5. Faca: no osso `wpn` com `KNIFE_FIX = Basis(RIGHT, PI)`.
6. Não escalar o viewmodel para imitar o FOV 68 do CS2 – deixou a arma gigante.

## Desempenho (PC do criador: AMD Radeon HD 6670)

- A placa não tem Vulkan/D3D12 → Godot cai para **OpenGL 3 (Compatibility)**.
- Medido: 17 FPS no Low antigo. O vilão era o próprio preset: **render scale < 1 e MSAA** no OpenGL
  criam cópias de tela inteira – mais caro que os pixels economizados. Agora desligados quando o renderer é `gl_compatibility`.
- LODs gerados (`ImporterMesh.generate_lods`) para mapa, agentes e armas: 360k → ~212k triângulos.
- Low: céu de cor sólida, luz por vértice sem normal map nos materiais do mapa, props pequenos param de desenhar a 35 m,
  fog até a distância de visão, occluder com os triângulos grandes da colisão.
- Resultado: ~32–37 FPS no Low a 1600×900. Cena vazia do Godot nesse PC: 75 FPS (limite do monitor).
- `Performance.TIME_PROCESS` inclui espera da GPU no OpenGL – não confiar nele sozinho; comparar FPS e draws.

## Construção e portais

- `PhysicsShapeQueryParameters3D` não tem `max_results`: é o segundo argumento de `intersect_shape(params, 1)`.
- O navmesh assado da colisão também cobre o **topo dos prédios** (dá para ficar em pé lá). No radar eles cobriam as ruas:
  o mapa desenha só os polígonos ligados aos spawns (polígonos vizinhos compartilham índices de vértice).
- Com Dust II cheio de desníveis, uma grade vertical global deixaria paredes meio enterradas; por isso o nível 0 vem da estrutura vizinha ou do chão.
- Texturas dos materiais vêm do próprio Dust II (`wood_planks`, `brick_ground`, `metal_panels_01_color`), mapeadas em triplanar no mundo.
- A vista pelo portal renderiza a cena de novo; no OpenGL da HD 6670 isso pesa, por isso o Low a renderiza em 35% da resolução. O near da câmera do portal vem do ponto mais próximo da borda do portal gêmeo; cortar pelo centro apagava o chão do outro lado (triângulo preto).

## Testes e capturas

- Variáveis de ambiente de desenvolvimento (inertes para jogadores):
  `BOTSTRIKE_CS2` (pasta), `BOTSTRIKE_AUTOPLAY=1` (bots jogam sozinhos), `BOTSTRIKE_FP=1` (assento em 1ª pessoa no autoplay),
  `BOTSTRIKE_SHOTS=<pasta>` (salva capturas e loga FPS a cada 2 s), `BOTSTRIKE_Q=low|medium|high`,
  `BOTSTRIKE_MODE=classic|build|portal` (modo do autoplay; padrão build), `BOTSTRIKE_DEMO=build|portal` (com `BOTSTRIKE_FP=1` e `BOTSTRIKE_SHOTS`: o jogador constrói/edita ou abre portais, abre o mapa e atravessa, e salva capturas `build_*`/`portal_*`),
  `BOTSTRIKE_NOVSYNC`, `BOTSTRIKE_NOMAP`, `BOTSTRIKE_NOAGENT`, `BOTSTRIKE_NOBOTMODEL`, `BOTSTRIKE_NOOCC`, `BOTSTRIKE_NOENV`, `BOTSTRIKE_NOUI`, `BOTSTRIKE_PLAINVP`, `BOTSTRIKE_PICK=x,y`.
- Headless não desenha nada: bugs visuais (branco, preto, agentes) só aparecem com janela.
- Uma nova `class_name` só registra depois de `godot --headless --path . --import`.
- Não rodar uma instância local durante o teste do Melty: as duas escrevem no mesmo log e no arquivo de checks.
