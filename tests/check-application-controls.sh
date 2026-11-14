#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ "$(uname -s)" != Darwin ]; then
  echo "This native test currently requires macOS"
  exit 0
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
cc -O1 -g -fobjc-arc -DGL_SILENCE_DEPRECATION -DHJP_TEST_NATIVE_QUEUE -I"$ROOT/src" -I"$ROOT/../../jp/include" \
  "$ROOT/tests/test_application_controls.c" "$ROOT/src/hjp_render.c" \
  "$ROOT/src/hjp_vnode.c" "$ROOT/src/hjp_frame.c" "$ROOT/src/hjp_hotreload.c" \
  "$ROOT/src/hjp_devtools.c" "$ROOT/src/hjp_unicode.c" "$ROOT/src/hjp_platform_macos.m" \
  -framework Cocoa -framework OpenGL -framework CoreText -framework CoreGraphics \
  -framework ImageIO -lpthread -o "$TMP/controls"
cd "$ROOT"
"$TMP/controls"
