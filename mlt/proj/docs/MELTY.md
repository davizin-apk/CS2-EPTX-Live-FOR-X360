# Melty

| | |
| --- | --- |
| Título | **Bot Strike: Dust II** |
| Slug | `bot-strike-dust-ii` |
| modId | `ac5faa25-e60e-4213-b9da-56cb4509e38f` |
| Studio | https://melty.gg/studio/ac5faa25-e60e-4213-b9da-56cb4509e38f |
| Página | https://melty.gg/m/bot-strike-dust-ii |
| Status | **rascunho** (não publicado), versão 0.1.0 |
| Jogo | Counter-Strike 2 (`counter-strike-2`), primário |
| Modo | `standalone` – programa próprio que lê a cópia do CS2 do jogador |
| Licença | All rights reserved, remixes permitidos |
| Um clique | sim (`one_click_check`) |
| Jogadores | single player, offline (você + 4 bots contra 5 bots) |

Tagline: *5v5 bomb defusal vs bots on Dust II from your own CS2: play it classic, or build walls, floors and ramps mid-fight.*
Descrição completa: [../LISTING.md](../LISTING.md).

> O token do Melty **não** fica em nenhum arquivo do projeto. Os scripts em `tools/release/` leem de `C:\mlt\.tok`.

## Receita (`melty.json`)

```json
{
  "schemaVersion": 1,
  "mode": "standalone",
  "games": [{"slug": "counter-strike-2", "role": "primary"}],
  "components": [{"id": "main", "fileName": "BotStrike-0.1.0.zip", "label": "Bot Strike", "kind": "main", "required": true}],
  "requirements": [],
  "mappings": [{"component": "main", "from": "", "to": "{managed}"}],
  "launch": {"kind": "exe", "path": "{managed}/BotStrike.exe", "args": ["--cs2={game}"]},
  "test": {"checks": ["prep-done", "map-loaded", "menu-shown", "round-played", "piece-built"], "minutes": 6}
}
```

## Modo de teste

Quando o Melty testa, ele cria `%LOCALAPPDATA%\Melty\test\bot-strike-dust-ii.json`; o jogo então:

- reporta `mod-loads`, `prep-done`, `map-loaded`, `menu-shown`, `round-played`, `piece-built` no arquivo de checks;
- entra sozinho numa partida Build Fight com o seu lugar controlado por um bot (por isso não dá pra controlar durante o teste);
- continua jogando até o Melty fechar (nunca sai sozinho).

## Histórico de testes

| Teste | Resultado |
| --- | --- |
| 9145f377 | não rodou: CS2 atualizando na Steam (24 GB) |
| 338a6ee0 | parado: atualização do CS2 renomeou a colisão do Dust II |
| bf281208 | parado: som do vento mudou de pasta |
| 6267098d | falhou: janela travou (testado antes da correção) |
| d99d53fa | parado: tudo branco (luzes do CS2) – corrigido depois |

**Falta**: um teste do Melty passar (2 execuções) com a build atual, a captura de tela entrar na página, e você dizer que quer publicar.

## Enviar uma nova build

1. Empacotar e zipar (ver [COMO_CONSTRUIR.md](COMO_CONSTRUIR.md)).
2. `start_upload` com nome, tamanho e SHA-256 → recebe `uploadId` (multipart, partes de 64 MB).
3. `python C:/mlt/proj/tools/release/upload_parts.py <uploadId> <sha256>` (divide, envia e chama `finish_upload`).
4. `submit_release` com `modId`, `version`, `uploadIds` e a receita acima.
5. `test_release` (2 execuções) e `test_result` até terminar.
6. `publish` só quando você disser que quer.
