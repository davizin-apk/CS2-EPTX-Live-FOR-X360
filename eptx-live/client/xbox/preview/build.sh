#!/bin/bash
# Compila e roda o preview da dash no PC (WSL), gravando PNGs em client/build/xbox/preview.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
XBOX="$HERE/.."
IMGUI_DIR="${IMGUI_DIR:-$HOME/imgui-xenon}"
XSDK="${XSDK:-$HOME/xsdk}"
BUILD="${BUILD:-$HOME/eptx-preview-build}"
OUT="$XBOX/../build/xbox/preview"
mkdir -p "$BUILD" "$OUT"

# As fontes, do mesmo jeito que o CMakeLists.txt as embute no .xex.
python3 - "$XBOX/third_party/selawik" "$BUILD/fonts.c" <<'EOF'
import sys
src, out = sys.argv[1], sys.argv[2]
with open(out, "w") as f:
    for symbol, name in (("font_regular", "selawk.ttf"), ("font_semibold", "selawksb.ttf"), ("font_light", "selawkl.ttf")):
        data = open(f"{src}/{name}", "rb").read()
        f.write(f"const unsigned char {symbol}[] = {{{','.join(str(b) for b in data)}}};\n")
        f.write(f"const unsigned int {symbol}_size = sizeof({symbol});\n")
EOF

INCLUDES=(-I"$IMGUI_DIR" -I"$IMGUI_DIR/backends" -I"$XSDK/include" -I"$XBOX" -I"$XBOX/../core" -I"$XBOX/third_party/stb")
gcc -O2 -c "$BUILD/fonts.c" -o "$BUILD/fonts.o"
for f in imgui imgui_draw imgui_tables imgui_widgets; do
	[ -f "$BUILD/$f.o" ] || g++ -O2 -std=c++17 "${INCLUDES[@]}" -c "$IMGUI_DIR/$f.cpp" -o "$BUILD/$f.o"
done
g++ -O2 -std=c++17 -Wall -Wextra -Wno-unused-function -Wno-missing-field-initializers "${INCLUDES[@]}" -DIMGUI_DISABLE_DEMO_WINDOWS \
	"$HERE/preview.cpp" "$BUILD"/imgui*.o "$BUILD/fonts.o" -o "$BUILD/preview"
# O segundo caminho recebe o corpo do avatar padrao, que o servidor serve para contas novas.
"$BUILD/preview" "$OUT" "$XBOX/../../src/assets/avatar-default.png"
