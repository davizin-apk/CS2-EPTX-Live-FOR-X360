# Gera o assets.pak (agentes, armas, viewmodels, animacoes, sons, HUD, navmesh) a partir do Bot Strike do PC.
# Antes: abra o Bot Strike uma vez e jogue uma partida (ele le o CS2 e assa o navmesh dos bots).
# As musicas do CS2 sao MP3 e sao gravadas em tempo real: o script leva uns 4 minutos.
#
#   powershell -ExecutionPolicy Bypass -File C:\mlt\x360\tools\build_assets.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$godot = 'C:\mlt\tools\godot\Godot_v4.7.2-stable_win64_console.exe'
& $godot --headless --path C:\mlt\proj res://tools/x360_bake.tscn -- --out "$root\work\assets" --tex 256 --vm-tex 512
if ($LASTEXITCODE -ne 0) { throw 'x360_bake falhou' }
python "$root\tools\pack_assets.py" --work "$root\work\assets" --out "$root\out\assets.pak"
if ($LASTEXITCODE -ne 0) { throw 'pack_assets.py falhou' }
Write-Host "Pronto: $root\out\assets.pak (e src\defs.h, gerado das planilhas)"
