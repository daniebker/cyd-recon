#!/usr/bin/env bash
# Render UI previews on a Linux desktop (no hardware needed).
# Usage: tools/preview/run.sh [outdir]   (needs g++, python3 + Pillow for PNGs)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."
LGFX="${LGFX:-$ROOT/.pio/libdeps/fnk0104n/LovyanGFX/src}"
OUT="${1:-$HERE/out}"
BUILD="$HERE/build"
mkdir -p "$OUT" "$BUILD"
SRCS=$(find "$LGFX/lgfx" \( -name '*.cpp' -o -name '*.c' \) \
  | grep -v '/platforms/' ; find "$LGFX/lgfx/v1/platforms/framebuffer" -name '*.cpp')
FLAGS="-std=gnu++17 -O1 -w -DLGFX_LINUX_FB -I$HERE/shim -I$LGFX -I$ROOT/src"
objs=()
for s in $SRCS; do
  o="$BUILD/$(echo "$s" | md5sum | cut -c1-12).o"
  [ -f "$o" ] || { case "$s" in *.c) gcc -O1 -w -DLGFX_LINUX_FB -I$LGFX -c "$s" -o "$o";; *) g++ $FLAGS -c "$s" -o "$o";; esac; }
  objs+=("$o")
done
g++ $FLAGS "$HERE/preview.cpp" "$ROOT/src/ui.cpp" "$ROOT/src/devices.cpp" "$ROOT/src/signatures.cpp" "$ROOT/src/battery.cpp" \
  "${objs[@]}" -o "$BUILD/preview" -lpthread
"$BUILD/preview" "$OUT"
python3 - "$OUT" <<'PY'
import sys, glob, os
try:
    from PIL import Image
except ImportError:
    sys.exit(0)
for p in sorted(glob.glob(os.path.join(sys.argv[1], "*.ppm"))):
    Image.open(p).save(p[:-4] + ".png"); os.remove(p)
PY
echo "previews in $OUT"
