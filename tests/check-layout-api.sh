#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN=${HAJIMU_BIN:-"$ROOT/../../jp/nihongo"}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
cd "$ROOT"
"$BIN" tests/test_layout_widths.jp
GUI_LAYOUT_CAPTURE="$TMP/clip.ppm" "$BIN" tests/test_child_clipping.jp
node tests/check-child-clipping.mjs "$TMP/clip.ppm"
