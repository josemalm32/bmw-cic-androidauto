#!/bin/sh
# Bench launcher. Run with Android Auto idle and the AAPlugin page open.
set -eu
if [ "$#" -ne 1 ]; then
    echo "Usage: $0 /path/to/accessory.p7b" >&2
    exit 2
fi
base=/mnt/hbdebug/aap
fifo=/tmp/aap.carplay.input
lock=/tmp/aap.carplay.lock
mkdir "$lock" || exit 1
receiver=
cleanup() {
    if [ -n "$receiver" ]; then
        kill "$receiver" 2>/dev/null || :
        wait "$receiver" 2>/dev/null || :
    fi
    rm -f "$fifo"
    rmdir "$lock"
}
trap cleanup 0
trap 'exit 130' INT
trap 'exit 143' TERM
rm -f "$fifo"
umask 077
mkfifo "$fifo"
"$base/cic_carplay" --mfi-cert "$1" &
receiver=$!
wait "$receiver"
