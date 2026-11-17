#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -O1 -g -Wall -Wextra -Werror -D_WIN32_WINNT=0x0601 -municode \
  "$ROOT/tests/test_win_tasks.c" -static-libgcc -o "$TMP/tasks.exe"
"$TMP/tasks.exe"
