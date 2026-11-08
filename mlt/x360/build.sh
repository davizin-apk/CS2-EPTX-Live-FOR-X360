#!/bin/bash
# Compila o botstrike.xex. Roda dentro do WSL, com o xsdk em ~/xsdk e a toolchain em ~/xsdk-gcc (a mesma do EPTX).
# Do PowerShell:
#
#   wsl -d Ubuntu-24.04 -e bash /mnt/c/mlt/x360/build.sh
#
# O .xex sai em out/botstrike.xex, com as licencas em out/licenses.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
XSDK="${XSDK:-$HOME/xsdk}"
TOOLCHAIN="${XSDK_TOOLCHAIN:-$HOME/xsdk-gcc}"
# A pasta de build fica no disco do Linux: em /mnt o CMake fica varias vezes mais lento.
BUILD="${BUILD:-$HOME/botstrike-xbox-build}"
OUT="$HERE/out"

cmake -S "$HERE" -B "$BUILD" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$XSDK/cmake/xsdk.cmake" \
	-DXSDK_TOOLCHAIN="$TOOLCHAIN" \
	-DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$BUILD"

mkdir -p "$OUT/licenses"
cp "$BUILD/botstrike.xex" "$OUT/botstrike.xex"
cp "$XSDK/LICENSE" "$OUT/licenses/xsdk-LICENSE.txt"
cp "$XSDK/THIRD_PARTY_NOTICES.md" "$OUT/licenses/xsdk-THIRD_PARTY_NOTICES.md"
echo "Compilado: $OUT/botstrike.xex"
