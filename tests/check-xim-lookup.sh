#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
set --
if [ "${XIM_SANITIZE:-0}" = 1 ]; then
  set -- "$@" -fsanitize=address,undefined -fno-omit-frame-pointer
fi
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists x11; then
  set -- "$@" $(pkg-config --cflags x11)
elif [ -d /opt/X11/include ]; then
  set -- "$@" -I/opt/X11/include
fi
cc -std=c11 -Wall -Wextra -Werror "$@" "$ROOT/tests/test_xim_lookup.c" \
  "$ROOT/src/hjp_unicode.c" -o "$TMP/xim-lookup"
"$TMP/xim-lookup"
