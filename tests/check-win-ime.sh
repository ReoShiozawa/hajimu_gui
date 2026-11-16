#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -Wall -Wextra -O1 "$ROOT/tests/test_win_ime.c" -o "$TMP/win-ime.exe" \
  -lopengl32 -lgdi32 -luser32 -lshell32 -lkernel32 -limm32 -static-libgcc
"$TMP/win-ime.exe"
