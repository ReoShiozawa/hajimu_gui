#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
mkdir "$TMP/data"
set --
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) directory=$(cygpath -m "$TMP/data"); set -- -D_WIN32_WINNT=0x0601 -static-libgcc ;;
  *) directory="$TMP/data"; if [ "${FILE_SANITIZE:-0}" = 1 ]; then set -- -fsanitize=address,undefined -fno-omit-frame-pointer; fi ;;
esac
${CC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror "$@" \
  "$ROOT/tests/test_files.c" -o "$TMP/files.exe"
HJP_FILE_TEST_DIR="$directory" "$TMP/files.exe"
