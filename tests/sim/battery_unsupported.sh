#!/bin/sh
# Scripted simulator check: the simulator's board has no battery capability, so `battery` must say so and fail.
# usage: battery_unsupported.sh PATH_TO_PALANU_SIM
set -eu
sim="$1"
data=$(mktemp -d)
trap 'rm -rf "$data"' EXIT
# open the launcher from the home screen, the terminal from the launcher, then run battery with its output redirected into the persistent /home
# the boot splash (ui_splash.c) takes the first keys for about 1.5 s: wait for it to hand over to home
PALANU_DATA="$data" "$sim" --wait 2000 --key enter --wait 300 --key enter --wait 500 --cmd "battery > /home/battery.txt" --wait 300
out="$data/home/battery.txt"
if [ ! -f "$out" ]; then
    echo "battery produced no output file" >&2
    exit 1
fi
grep -q "this board cannot measure one" "$out" || {
    echo "unexpected battery output:" >&2
    cat "$out" >&2
    exit 1
}
echo "battery: unsupported message ok"
