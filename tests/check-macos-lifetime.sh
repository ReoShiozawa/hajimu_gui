#!/bin/sh
set -eu
if [ "$(uname -s)" != Darwin ]; then
  echo "macOS以外ではARC寿命検査をスキップします"
  exit 0
fi
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
cc -O2 -fobjc-arc -DGL_SILENCE_DEPRECATION -I"$ROOT/src" \
  "$ROOT/tests/test_macos_lifetime.m" -framework Cocoa -framework OpenGL \
  -framework CoreText -framework CoreGraphics -framework ImageIO \
  -o "$TMP/lifetime"
"$TMP/lifetime" "$ROOT/fonts/NotoSansCJKjp-Regular.otf"
cc -O1 -g -fsanitize=address -fno-omit-frame-pointer -fobjc-arc \
  -DGL_SILENCE_DEPRECATION -I"$ROOT/src" "$ROOT/tests/test_render_memory.c" \
  "$ROOT/src/hjp_platform_macos.m" "$ROOT/src/hjp_unicode.c" -framework Cocoa -framework OpenGL \
  -framework CoreText -framework CoreGraphics -framework ImageIO \
  -o "$TMP/render-memory"
"$TMP/render-memory"
