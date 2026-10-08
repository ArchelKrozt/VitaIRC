#!/usr/bin/env bash
# Builds VitaIRC.vpk. vita-pack-vpk can't handle spaces in paths, so the
# build runs through a space-free symlink when needed.
set -euo pipefail

: "${VITASDK:?Set VITASDK first (e.g. export VITASDK=\$HOME/vitasdk)}"
export PATH="$VITASDK/bin:$PATH"

SRC="$(cd "$(dirname "$0")" && pwd)"
if [[ "$SRC" == *" "* ]]; then
	LINK="$HOME/.vitairc-src"
	ln -sfn "$SRC" "$LINK"
	SRC="$LINK"
fi
BUILD="${BUILD_DIR:-$HOME/.vitairc-build}"

mkdir -p "$BUILD"
cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" -j4

mkdir -p "$SRC/dist"
cp "$BUILD/VitaIRC.vpk" "$SRC/dist/VitaIRC.vpk"
echo "Done: dist/VitaIRC.vpk"
