#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
set --
if [ "${TASK_SANITIZE:-0}" = 1 ]; then
  set -- -fsanitize=address,undefined -fno-omit-frame-pointer
fi
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
  "$@" \
  "$ROOT/tests/test_tasks.c" -o "$TMP/tasks"
"$TMP/tasks"
