#!/bin/bash
# Compila e roda o preview da dash no PC (WSL), gravando PNGs em client/build/xbox/preview
# e conferindo a navegacao pelo controle. Usa a skin do clone do FSD em freestyledash/.
#
#   wsl -d Ubuntu-24.04 -e bash "/mnt/d/EPTX-NXE-Edition/client/xbox/preview/build.sh"
#
# A pasta de build (~/eptx-nxe-preview-build) e desta edicao, separada da do EPTX Live.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
XBOX="$HERE/.."
SKIN="${SKIN:-$XBOX/../../freestyledash/Skins/Default}"
IMGUI_DIR="${IMGUI_DIR:-$HOME/imgui-xenon}"
XSDK="${XSDK:-$HOME/xsdk}"
BUILD="${BUILD:-$HOME/eptx-nxe-preview-build}"
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
[ -f "$BUILD/fonts.o" ] || gcc -O2 -c "$BUILD/fonts.c" -o "$BUILD/fonts.o"
for f in imgui imgui_draw imgui_tables imgui_widgets; do
	[ -f "$BUILD/$f.o" ] || g++ -O2 -std=c++17 "${INCLUDES[@]}" -c "$IMGUI_DIR/$f.cpp" -o "$BUILD/$f.o"
done
gcc -O2 -std=gnu17 "${INCLUDES[@]}" -c "$XBOX/image.c" -o "$BUILD/image.o"
g++ -O2 -std=c++17 -Wall -Wextra -Werror "${INCLUDES[@]}" -c "$XBOX/xui.cpp" -o "$BUILD/xui.o"
g++ -O2 -std=c++17 -Wall -Wextra -Wno-unused-function -Wno-missing-field-initializers "${INCLUDES[@]}" \
	-DIMGUI_DISABLE_DEMO_WINDOWS "$HERE/preview.cpp" "$BUILD/xui.o" "$BUILD/image.o" "$BUILD"/imgui*.o "$BUILD/fonts.o" \
	-o "$BUILD/preview"
"$BUILD/preview" "$OUT" "$SKIN"
