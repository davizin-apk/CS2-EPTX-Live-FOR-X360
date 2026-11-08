# Como construir e testar

Ferramentas (já baixadas em `C:\mlt\tools`):

- Godot 4.7.2: `C:\mlt\tools\godot\Godot_v4.7.2-stable_win64.exe` (e `_console.exe` para ver a saída)
- Source2Viewer-CLI 20.1: `C:\mlt\tools\vrf\`

## 1. Mudar algo

1. Edite a planilha em `design/*.json` (ex.: dano da AK em `weapons.json`).
2. Rode o preflight – tem que dizer `PREFLIGHT CLEAN`:

```bash
python C:/mlt/proj/tools/preflight.py
```

3. Mude o código em `scripts/` se precisar. Se criou uma `class_name` nova:

```bash
C:/mlt/tools/godot/Godot_v4.7.2-stable_win64_console.exe --headless --path C:/mlt/proj --import
```

4. Se adicionou linhas em `assets.json`, suba `PREP_VERSION` em `scripts/game.gd` (força reexportar do CS2).
   Se mudou o que é assado no mapa, mude o nome em `_baked_path()` de `prep.gd`.

## 2. Testar

Rodar o projeto direto (sem empacotar):

```bash
C:/mlt/tools/godot/Godot_v4.7.2-stable_win64.exe --path C:/mlt/proj
```

Deixar os bots jogarem uma partida Build Fight e salvar capturas + FPS (PowerShell: defina as variáveis com `$env:`):

```bash
BOTSTRIKE_CS2="D:/SteamLibrary/steamapps/common/Counter-Strike Global Offensive" BOTSTRIKE_AUTOPLAY=1 BOTSTRIKE_FP=1 BOTSTRIKE_SHOTS=C:/mlt/shots BOTSTRIKE_Q=low C:/mlt/tools/godot/Godot_v4.7.2-stable_win64.exe --path C:/mlt/proj --windowed --resolution 1600x900
```

O log fica em `%APPDATA%\BotStrike\botstrike.log` (linhas `bench`, `check`, `round`, `portal:`).

Modo Portal Strike com os bots (procure `portal:` no log: compras, portais abertos, quem atravessou, tiros que passaram):

```bash
BOTSTRIKE_CS2="D:/SteamLibrary/steamapps/common/Counter-Strike Global Offensive" BOTSTRIKE_AUTOPLAY=1 BOTSTRIKE_MODE=portal C:/mlt/tools/godot/Godot_v4.7.2-stable_win64_console.exe --headless --path C:/mlt/proj
```

Capturas da construção ou dos portais (troque `build` por `portal`):

```bash
BOTSTRIKE_CS2="D:/SteamLibrary/steamapps/common/Counter-Strike Global Offensive" BOTSTRIKE_AUTOPLAY=1 BOTSTRIKE_FP=1 BOTSTRIKE_Q=medium BOTSTRIKE_MODE=build BOTSTRIKE_DEMO=build BOTSTRIKE_SHOTS=C:/mlt/shots/demo_build C:/mlt/tools/godot/Godot_v4.7.2-stable_win64.exe --path C:/mlt/proj --windowed --resolution 1600x900
```

## 3. Empacotar

```bash
C:/mlt/tools/godot/Godot_v4.7.2-stable_win64_console.exe --headless --path C:/mlt/proj --export-pack "Windows Desktop" C:/mlt/pkg/BotStrike.pck
```

`C:\mlt\pkg` contém:

```
BotStrike.exe        (o Godot 4.7.2 renomeado – carrega BotStrike.pck ao lado)
BotStrike.pck        (o jogo)
vrf/                 (Source2Viewer-CLI: lê os arquivos do CS2)
licenses/            (MIT do Godot e do ValveResourceFormat)
CREDITS.txt, README.txt
```

Zip para o Melty:

```bash
python C:/mlt/proj/tools/release/zipit.py C:/mlt/BotStrike-0.1.0.zip
```

## 4. Enviar ao Melty

Ver [MELTY.md](MELTY.md). Resumo: `start_upload` → `tools/release/upload_parts.py <uploadId> <sha256>` → `submit_release`
com `melty.json` → `test_release`.
