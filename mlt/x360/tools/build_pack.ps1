# Gera o dust2.pak a partir do cache que o Bot Strike ja montou do seu CS2, e confere no PC.
# Rode depois de abrir o Bot Strike pelo menos uma vez (e de novo depois de uma atualizacao do CS2).
#
#   powershell -ExecutionPolicy Bypass -File C:\mlt\x360\tools\build_pack.ps1
param(
	[int]$Tris = 600000,       # orcamento do LOD de longe
	[double]$NearError = 1.0,  # detalhe do LOD de perto (menor = mais triangulos)
	[int]$Tex = 256            # lado maximo das texturas
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$godot = 'C:\mlt\tools\godot\Godot_v4.7.2-stable_win64_console.exe'
$glb = Join-Path $env:APPDATA 'BotStrike\cs2cache\maps\de_dust2.glb'
if (-not (Test-Path $glb)) { throw "Nao achei $glb. Abra o Bot Strike uma vez para ele ler o CS2." }

& $godot --headless --path "$root\tools\converter" -s export_map.gd -- --glb $glb --out "$root\work" --tris $Tris --tex $Tex --near-error $NearError
if ($LASTEXITCODE -ne 0) { throw 'export_map.gd falhou' }
python "$root\tools\pack.py" --work "$root\work" --out "$root\out\dust2.pak"
if ($LASTEXITCODE -ne 0) { throw 'pack.py falhou' }
zig cc -O2 -std=c17 -I "$root\src" "$root\tools\pc\check.c" "$root\src\pack.c" "$root\src\collide.c" "$root\src\player.c" -o "$root\out\check.exe"
& "$root\out\check.exe" "$root\out\dust2.pak"
if ($LASTEXITCODE -ne 0) { throw 'o pacote nao passou na conferencia; nao mande para o console' }
Write-Host "Pronto: $root\out\dust2.pak"
