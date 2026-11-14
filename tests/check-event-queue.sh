#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
set --
if [ "${EVENT_SANITIZE:-0}" = 1 ]; then
  set -- -fsanitize=address,undefined -fno-omit-frame-pointer
fi
cc -std=c11 -Wall -Wextra -Werror "$@" "$ROOT/tests/test_event_queue.c" -o "$TMP/events"
"$TMP/events"
cc -std=c11 -Wall -Wextra -Werror "$@" "$ROOT/tests/test_native_queue.c" -o "$TMP/native-events"
"$TMP/native-events"
