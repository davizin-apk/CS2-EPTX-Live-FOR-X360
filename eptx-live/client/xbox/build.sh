#!/bin/bash
# Compila o eptx.xex. Roda dentro do WSL, com o xsdk em ~/xsdk e a toolchain em
# ~/xsdk-gcc (xsdk:docs/GETTING_STARTED.md, passos 1 a 5). Do PowerShell:
#
#   wsl -d Ubuntu-24.04 -e bash "/mnt/d/EPTX Live/client/xbox/build.sh"
#
# O .xex sai em client/build/xbox/eptx.xex, com as licencas em client/build/xbox/licenses.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
XSDK="${XSDK:-$HOME/xsdk}"
TOOLCHAIN="${XSDK_TOOLCHAIN:-$HOME/xsdk-gcc}"
IMGUI_DIR="${IMGUI_DIR:-$HOME/imgui-xenon}"
# O commit do imgui-xenon com que a dash foi feita.
IMGUI_COMMIT=66f7e033
# A pasta de build fica no disco do Linux: em /mnt o CMake fica varias vezes mais lento.
BUILD="${BUILD:-$HOME/eptx-xbox-build}"
OUT="$HERE/../build/xbox"

if [ ! -f "$IMGUI_DIR/imgui.h" ]; then
	git clone -q https://github.com/xenosworks/imgui-xenon.git "$IMGUI_DIR"
	git -C "$IMGUI_DIR" checkout -q "$IMGUI_COMMIT"
fi

cmake -S "$HERE" -B "$BUILD" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$XSDK/cmake/xsdk.cmake" \
	-DXSDK_TOOLCHAIN="$TOOLCHAIN" \
	-DIMGUI_DIR="$IMGUI_DIR" \
	-DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$BUILD"

mkdir -p "$OUT/licenses"
cp "$BUILD/eptx.xex" "$OUT/eptx.xex"
# O que vai dentro do .xex e tem licenca que pede o aviso junto.
cp "$XSDK/LICENSE" "$OUT/licenses/xsdk-LICENSE.txt"
cp "$XSDK/THIRD_PARTY_NOTICES.md" "$OUT/licenses/xsdk-THIRD_PARTY_NOTICES.md"
cp "$IMGUI_DIR/LICENSE.txt" "$OUT/licenses/imgui-LICENSE.txt"
cp "$HERE/third_party/selawik/LICENSE.txt" "$OUT/licenses/selawik-OFL.txt"
echo "Compilado: $OUT/eptx.xex"
