#!/usr/bin/env bash
# Development helper: runs the demo build (-DVITAIRC_DEMO=ON) in Vita3K and
# screenshots every screen into screenshots/. macOS only; needs Screen Recording
# permission for the terminal/editor running it.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
V3K="${V3K:-$HOME/Vita3K-tools/Vita3K.app/Contents/MacOS/Vita3K}"
FS="$HOME/Library/Application Support/Vita3K/Vita3K/fs"
D="$FS/ux0/data/VitaIRC"
VPK="${1:?uso: tools/vita3k-shots.sh ruta/al/VitaIRC-demo.vpk}"
OUT="$ROOT/screenshots"
WINID="${TMPDIR:-/tmp}/vitairc-winid"

[ -x "$WINID" ] || swiftc -O "$ROOT/tools/winid.swift" -o "$WINID"
mkdir -p "$FS/ux0/app/VIRC00001" "$OUT"
unzip -o -q "$VPK" -d "$FS/ux0/app/VIRC00001"
rm -f "$D/demo_step.txt"

"$V3K" -r VIRC00001 >/dev/null 2>&1 &
PID=$!
last=""
for _ in $(seq 1 400); do
	sleep 0.5
	cur=$(head -1 "$D/demo_step.txt" 2>/dev/null | tr -d '\r\n ' || true)
	if [ -n "$cur" ] && [ "$cur" != "$last" ]; then
		sleep 1.2
		W=$("$WINID" | grep VIRC00001 | head -1 | cut -d' ' -f1)
		screencapture -x -o -l"$W" "$OUT/$cur.png"
		echo "$cur"
		last=$cur
		case "$cur" in *_confirm) break;; esac
	fi
done
sleep 3
kill "$PID" 2>/dev/null || true

# drop the window title bar and scale to the Vita's 960x544
python3 - "$OUT" <<'PY'
import glob, sys
from PIL import Image
for f in glob.glob(sys.argv[1] + "/*.png"):
    im = Image.open(f).convert("RGB")
    w, h = im.size
    s = w / 960
    im.crop((0, h - int(544 * s), w, h)).resize((960, 544), Image.LANCZOS).save(f)
PY
