#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
BIN=${HAJIMU_BIN:-../../jp/nihongo}
TMP=$(mktemp -d)
holder=
trap '[ -z "$holder" ] || kill "$holder" 2>/dev/null || true; rm -rf "$TMP"' EXIT
export GUI_LOCK_TEST_PATH="$TMP/write.lock"
GUI_LOCK_TEST_MODE=holder "$BIN" tests/test_file_lock.jp &
holder=$!
i=0
while [ ! -f "$GUI_LOCK_TEST_PATH.ready" ]; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || exit 1
    sleep 0.02
done
GUI_LOCK_TEST_MODE=probe "$BIN" tests/test_file_lock.jp
wait "$holder"
holder=
GUI_LOCK_TEST_MODE=reopen "$BIN" tests/test_file_lock.jp
echo "cross-process lock tests passed"
