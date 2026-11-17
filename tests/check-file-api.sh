#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
cd "$ROOT"
HJP_FILE_API_TEST_DIR="$TMP" "${HAJIMU_BIN:-$ROOT/../../jp/nihongo}" tests/test_file_progress.jp
