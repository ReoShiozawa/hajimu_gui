#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
HAJIMU_INCLUDE=${HAJIMU_INCLUDE:-"$ROOT/../../jp/include"}
if [ "$(uname -s)" != Darwin ]; then
  echo "This native test currently requires macOS"
  exit 0
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
set --
if [ "${CONTROL_SANITIZE:-0}" = 1 ]; then
  set -- -fsanitize=address,undefined -fno-omit-frame-pointer
fi
cc "$@" -O1 -gline-tables-only -fobjc-arc -DGL_SILENCE_DEPRECATION -DHJP_TEST_NATIVE_QUEUE -I"$ROOT/src" -I"$HAJIMU_INCLUDE" \
  "$ROOT/tests/test_application_controls.c" "$ROOT/src/hjp_render.c" \
  "$ROOT/src/hjp_vnode.c" "$ROOT/src/hjp_frame.c" "$ROOT/src/hjp_hotreload.c" \
  "$ROOT/src/hjp_devtools.c" "$ROOT/src/hjp_unicode.c" "$ROOT/src/hjp_platform_macos.m" \
  -framework Cocoa -framework OpenGL -framework CoreText -framework CoreGraphics \
  -framework ImageIO -lpthread -o "$TMP/controls"
cd "$ROOT"
set +e
"$TMP/controls"
status=$?
set -e
if [ "$status" = 77 ] && [ "${CONTROL_ALLOW_UNSUPPORTED_GL:-0}" = 1 ]; then
  echo "::notice::Native GUI controls NOT TESTED: runner only provides OpenGL below 3.2"
  if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    printf '%s\n' 'Native GUI controls: **NOT TESTED**. The runner only provides OpenGL below 3.2. Run the native ASan/UBSan suite on a graphics-capable macOS host.' >> "$GITHUB_STEP_SUMMARY"
  fi
  exit 0
fi
exit "$status"
