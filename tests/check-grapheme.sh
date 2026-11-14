#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
if command -v sha256sum >/dev/null 2>&1; then
  sha256sum -c tests/data/unicode.sha256
else
  shasum -a 256 -c tests/data/unicode.sha256
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
set --
if [ "${GRAPHEME_SANITIZE:-0}" = 1 ]; then
  set -- -fsanitize=address,undefined -fno-omit-frame-pointer
fi
cc -std=c11 -Wall -Wextra -Werror "$@" "$ROOT/tests/test_grapheme.c" "$ROOT/src/hjp_unicode.c" -o "$TMP/grapheme"
"$TMP/grapheme" "$ROOT/tests/data/GraphemeBreakTest-18.0.0.txt"
