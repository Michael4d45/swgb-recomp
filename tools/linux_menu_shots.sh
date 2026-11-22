#!/bin/sh
# Headless menu shots for the Linux host: boot, then hover Single Player.
# Writes PNGs under the given OUT dir (default: /tmp/swgb-shots).
set -e
cd "$(dirname "$0")/.."
OUT=${1:-/tmp/swgb-shots}
B=${B:-build-linux/swgbrecomp}
mkdir -p "$OUT"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy

# t≈12s: menu settled. Hover Single Player (~345,250 from docs/testing.md).
"$B" --run --game game --headless --watchdog 25 \
  --shot 12 "$OUT/01-boot.bmp" \
  --keys "14:move:345:250,16:shot=$OUT/02-hover-single.bmp"

for f in "$OUT"/01-boot.bmp "$OUT"/02-hover-single.bmp; do
  [ -f "$f" ] || { echo "missing $f"; exit 1; }
  ffmpeg -y -i "$f" -update 1 "${f%.bmp}.png" >/dev/null 2>&1
done
echo "wrote $OUT/01-boot.png and $OUT/02-hover-single.png"
