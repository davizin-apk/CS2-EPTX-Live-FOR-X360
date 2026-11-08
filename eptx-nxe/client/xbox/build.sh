#!/bin/bash
# Compila o eptx_nxe.xex. Roda dentro do WSL, com o xsdk em ~/xsdk e a toolchain em
# ~/xsdk-gcc (os mesmos do EPTX Live, so lidos). Do PowerShell:
#
#   wsl -d Ubuntu-24.04 -e bash "/mnt/d/EPTX-NXE-Edition/client/xbox/build.sh"
#
# O .xex sai em client/build/xbox/eptx_nxe.xex desta edicao, com as licencas ao lado.
# A pasta de build e propria (~/eptx-nxe-xbox-build): a do EPTX Live (~/eptx-xbox-build)
# nao e tocada.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
XSDK="${XSDK:-$HOME/xsdk}"
TOOLCHAIN="${XSDK_TOOLCHAIN:-$HOME/xsdk-gcc}"
IMGUI_DIR="${IMGUI_DIR:-$HOME/imgui-xenon}"
# O commit do imgui-xenon com que a dash foi feita.
IMGUI_COMMIT=66f7e033
# A pasta de build fica no disco do Linux: em /mnt o CMake fica varias vezes mais lento.
BUILD="${BUILD:-$HOME/eptx-nxe-xbox-build}"
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
cp "$BUILD/eptx_nxe.xex" "$OUT/eptx_nxe.xex"
# O que vai dentro do .xex (e a skin do FSD, que vai junto) e tem licenca que pede o aviso.
cp "$XSDK/LICENSE" "$OUT/licenses/xsdk-LICENSE.txt"
cp "$XSDK/THIRD_PARTY_NOTICES.md" "$OUT/licenses/xsdk-THIRD_PARTY_NOTICES.md"
cp "$IMGUI_DIR/LICENSE.txt" "$OUT/licenses/imgui-LICENSE.txt"
cp "$HERE/third_party/selawik/LICENSE.txt" "$OUT/licenses/selawik-OFL.txt"
cp "$HERE/../../freestyledash/LICENSE.md" "$OUT/licenses/freestyledash-GPLv3.md"
echo "Compilado: $OUT/eptx_nxe.xex"
