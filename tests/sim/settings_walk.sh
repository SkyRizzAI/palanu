#!/bin/sh
# Scripted simulator walk through the Settings pages: each page is opened with `uishow settings PAGE` (a fixed clock, no key
# presses after the route), captured, and checked for a full 320x240 frame. A page that crashes or does not open fails.
# usage: settings_walk.sh PATH_TO_PALANU_SIM [OUTPUT_DIR]
set -eu
sim="$1"
out="${2:-$(mktemp -d)}"
mkdir -p "$out"
pages="root display appearance desktop-setting controls keys keymap wifi wifi-detail wifi-scan bluetooth remote remote-link link-clients device sound led battery power camera touch sensors secure notifications notify-apps storage storage-detail apps app-detail app-terminal services service-detail datetime profile pin updates developer about about-detail about-notices"
fails=0
for page in $pages; do
    data=$(mktemp -d)
    # The launcher, then the Terminal (its first entry), then home: the same route the golden scenes use. The shell types the
    # uishow command; the frame is saved once the page is up.
    if ! PALANU_DATA="$data" "$sim" --wait 2000 --key enter --wait 300 --key enter --wait 500 \
        --cmd "cat /home/golden-cursor.esc" --wait 200 --cmd "date 2026-01-01 10:00:00" --wait 300 \
        --key home-app --wait 400 --key enter --wait 300 --key enter --wait 300 \
        --cmd "uishow settings $page" --wait 700 --shot "$out/settings-$page.ppm" >/dev/null 2>&1; then
        echo "FAIL $page: the simulator exited with an error" >&2
        fails=$((fails + 1))
    elif [ "$(wc -c < "$out/settings-$page.ppm" | tr -d ' ')" -lt 230400 ]; then
        echo "FAIL $page: no full frame" >&2
        fails=$((fails + 1))
    else
        echo "ok   settings $page"
    fi
    rm -rf "$data"
done
if [ "$fails" -ne 0 ]; then
    echo "settings walk: $fails page(s) failed" >&2
    exit 1
fi
echo "settings walk: all pages open"
