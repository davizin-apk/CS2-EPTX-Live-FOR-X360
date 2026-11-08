# Changelog

Tudo ainda é a versão 0.1.0 (rascunho no Melty, reenviada a cada etapa).

## 30/10/2026

### Construção igual à do Fortnite (Build Fight)
- Grade de 5,12 m × 3,84 m; parede (Q/F1), chão (X/F2), escada (C/F3) e cone (V/F4) com holograma azul de prévia (vermelho onde não dá).
- Madeira, tijolo e metal (botão direito troca), com vida e tempo de construção próprios e texturas do Dust II.
- Turbo build segurando o clique (ramp rush funciona).
- Edição com F: parede 3×3, chão e cone 2×2, escada gira arrastando; botão direito desfaz.
- Só constrói encostado no chão ou em outra peça; quebrar o apoio derruba o que estava pendurado.
- Bots usam o material que têm mais e às vezes levantam escada atrás da parede.

### Novo modo: Portal Strike vs Bots
- Portal Gun ($1000, slot 4) no menu de compra só nesse modo.
- Clique esquerdo: portal azul na parede/chão/teto (na parede ele assenta no chão se você mirar baixo).
- Botão direito: mapa de Dust II (radar do navmesh) com as rotas previstas dos inimigos e até 3 lugares recomendados (★ AMBUSH, CUT OFF, SITE);
  clique para abrir o portal laranja ali, a rodinha gira para onde você sai olhando.
- Jogador, bots e tiros (seus e dos bots) atravessam; sai com impulso.
- Bots compram a arma, abrem seus pares, usam qualquer portal como atalho e enxergam/atiram através deles.
- O portal mostra o outro lado em todas as qualidades (no Low em resolução menor), e dá pra atirar através dele: o tiro sai pelo outro portal.

## 29/10/2026 – noite

### Primeira pessoa
- Viewmodel um pouco mais pra trás; controles **Viewmodel right/left, up/down, back/forward** nas configurações.
- Peças da arma reagem às animações do CS2 em todas as armas: ferrolho da AK, slide da Glock/USP, pente saindo no reload
  (o esqueleto da arma agora segue o osso `wpn` sem mudar de lugar na árvore).
- Cápsulas no tamanho real (9 mm 2,1 cm, 7.62 3,9 cm, .50 7 cm) – antes saíam com ~2 m.
- Cada arma com seus clipes do CS2: AK-47, Glock-18, AWP próprios; USP-S e M4A1-S nos padrões.
- Arma montada no esqueleto da arma do clipe (tamanho e pegada corretos), faca com lâmina pra cima.
- Flash no cano real e cápsula do calibre certo a cada tiro.
- Braços do próprio agente em 1ª pessoa com os clipes draw/idle/shoot/reload do CS2.

### Desempenho (Radeon HD 6670 / OpenGL)
- 17 → ~32–37 FPS no Low: sem render scale/MSAA no OpenGL, LODs no mapa/agentes/armas, props somem à distância,
  occluder, fog, céu sólido e luz por vértice no Low. Contador de FPS.
- Presets Low / Medium / High + render scale (⚙ e Esc).

## 29/10/2026 – tarde

### Visual
- Fim do "tudo branco": removidas as luzes do CS2 que vinham no mapa.
- Agentes de volta (CS2 mudou para `agents/models/`) com animações idle/run/shoot remapeadas; tombam ao morrer.
- Armas nas mãos dos bots; câmera do menu procura espaço aberto; formas pretas escondidas; câmera do espectador não entra na parede;
  bots miram no corpo e às vezes na cabeça.

### Atualização do CS2 (patch 1.41.9.3)
- Colisão do Dust II renomeada (`world_physics.vmdl_c`), vetores das entidades em `[x, y, z]`, som do vento em nova pasta:
  o jogo aceita os dois formatos; sons/ícones/fontes faltando não impedem mais de abrir.
- Navmesh refeito a cada patch; bots desviam quando travam.

## 29/10/2026 – manhã

- Projeto criado: Godot 4.7.2 lendo Dust II, agentes, armas, sons, ícones, música e fontes do CS2 do jogador via Source2Viewer-CLI.
- Modos Competitive vs Bots e Build Fight vs Bots (5v5, bomba, economia, meio-tempo, compra, construção Q/X/C).
- Menu estilo CS2, HUD, placar, pausa, configurações.
- Rascunho no Melty, receita standalone, um clique: sim.
