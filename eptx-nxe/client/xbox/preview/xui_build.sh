#!/bin/bash
# Compila o xui_main (o engine XUI sozinho) no WSL e desenha uma cena do FSD.
#
#   wsl -d Ubuntu-24.04 -e bash "/mnt/d/EPTX-NXE-Edition/client/xbox/preview/xui_build.sh" [cena.xui] [Id] [segundos]
#
# Grava em client/build/xbox/preview/xui-<Id>.png. A pasta de build e propria desta
# edicao (~/eptx-nxe-xui-build), separada da do EPTX Live.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
XBOX="$HERE/.."
EDITION="$XBOX/../.."
SKIN="$EDITION/freestyledash/Skins/Default"
IMGUI_DIR="${IMGUI_DIR:-$HOME/imgui-xenon}"
BUILD="${BUILD:-$HOME/eptx-nxe-xui-build}"
OUT="$XBOX/../build/xbox/preview"
mkdir -p "$BUILD" "$OUT"

python3 - "$XBOX/third_party/selawik" "$BUILD/fonts.c" <<'EOF'
import sys
src, out = sys.argv[1], sys.argv[2]
with open(out, "w") as f:
    for symbol, name in (("font_regular", "selawk.ttf"), ("font_semibold", "selawksb.ttf"), ("font_light", "selawkl.ttf")):
        data = open(f"{src}/{name}", "rb").read()
        f.write(f"const unsigned char {symbol}[] = {{{','.join(str(b) for b in data)}}};\n")
        f.write(f"const unsigned int {symbol}_size = sizeof({symbol});\n")
EOF

INCLUDES=(-I"$IMGUI_DIR" -I"$XBOX" -I"$XBOX/third_party/stb")
[ -f "$BUILD/fonts.o" ] || gcc -O2 -c "$BUILD/fonts.c" -o "$BUILD/fonts.o"
for f in imgui imgui_draw imgui_tables imgui_widgets; do
	[ -f "$BUILD/$f.o" ] || g++ -O2 -std=c++17 "${INCLUDES[@]}" -c "$IMGUI_DIR/$f.cpp" -o "$BUILD/$f.o"
done
g++ -O2 -std=c++17 -Wall -Wextra -Werror "${INCLUDES[@]}" -c "$XBOX/xui.cpp" -o "$BUILD/xui.o"
g++ -O2 -std=c++17 -Wall -Wextra -Wno-unused-function -Wno-missing-field-initializers "${INCLUDES[@]}" \
	"$HERE/xui_main.cpp" "$BUILD/xui.o" "$BUILD"/imgui*.o "$BUILD/fonts.o" -o "$BUILD/xui_main"

FILE="${1:-main.xui}"
SCENE="${2:-}"
SECONDS_="${3:-1}"
"$BUILD/xui_main" "$SKIN" "$OUT/xui-${SCENE:-root}.png" "$FILE" "$SCENE" "$SECONDS_"
