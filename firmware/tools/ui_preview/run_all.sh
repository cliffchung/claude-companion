#!/bin/sh
# Runs every preview case in the build directory $1 (default: the current directory), then converts
# each <name>.ppm to $1/png/<name>.png with sips (macOS). Exits 1 if any case failed.
PV="${1:-.}"
cd "$PV" || exit 2
[ -x ./preview ] || { echo "run_all.sh: no ./preview in $PV (build it first)" >&2; exit 2; }
rm -f ./*.ppm
fail=0
# An LVGL warning is a finding: it fails the case, and is shown.
PV_ENV=
PV_ENV2=
pv() {
    if [ -n "$PV_ENV2" ]; then env "$PV_ENV" "$PV_ENV2" ./preview "$@"
    elif [ -n "$PV_ENV" ]; then env "$PV_ENV" ./preview "$@"
    else ./preview "$@"; fi
}
run() {
    if pv "$@" >case.log 2>&1; then
        grep -E '^check |passed|ok$|^settings' case.log || true
    else
        echo "FAIL: preview $PV_ENV $*"; tail -5 case.log; fail=1
    fi
    if grep -q '^\[Warn\]' case.log; then
        echo "FAIL: preview $* logged LVGL warnings:"; grep '^\[Warn\]' case.log | head -5; fail=1
    fi
}
run boot
run sleep
run nav
run targets-clip
run softkbd
for i in 0 1 2 3 4 5; do run settings "$i"; done
# Settings with a row clicked (PREVIEW_CLICK, a row label) or the panel scrolled (PREVIEW_SCROLL, px)
runv() { PV_ENV="$1"; shift; run "$@"; PV_ENV=; }
runv "PREVIEW_CLICK=Public key" settings 0
runv "PREVIEW_CLICK=Forget" settings 0
runv "PREVIEW_CLICK=Reset settings" settings 5
runv "PREVIEW_CLICK=Diagnostics" settings 5
runv "PREVIEW_CLICK=Reboot" settings 5
runv "PREVIEW_CLICK=Colour" settings 3
runv "PREVIEW_SCROLL=400" settings 2
runv "PREVIEW_HOST=0" settings 0
runv "PREVIEW_FAIL=notfound" settings 0
runv "PREVIEW_TS=0" settings 1
runv "PREVIEW_TS=3" settings 1
runv "PREVIEW_SCROLL=600" settings 1
runv "PREVIEW_CLICK=Auth key" settings 1
runv "PREVIEW_CLICK=Device name" settings 1
runv "PREVIEW_CLICK=Connection" settings 1
runv "PREVIEW_CLICK=Clientre Wi-Fi" settings 1
runv "PREVIEW_CLICK=Forget this device on the tailnet" settings 1
PV_ENV="PREVIEW_KEY=0"; PV_ENV2="PREVIEW_CLICK=Forget this device on the tailnet"; run settings 1; PV_ENV=; PV_ENV2=
for v in networks advanced peers; do run settings-sub "$v"; done
run settings-sub peers deleted-event
run network-live
run net-sub-open
runv "PREVIEW_HOST=0" settings-sub networks   # join a saved network with no Host set (the device gate's no-Host leg)
run keyboard-settings
for v in live selected-needs-you working loading stale reconnecting no-wifi not-found no-host empty \
         interactive sent low-battery 16 long resort tap-resort resort-scroll keys idle forget; do run sessions "$v"; done
run ctrlk
run keyboard
run gesture
run art
run term-spacing
run terminal
for v in on empty keys deleted-event fail add; do run terminal "$v"; done
runv "PREVIEW_HOST_TSIP=0" terminal add-fqdn
for v in new edit-pin sd-key settings; do run hostform "$v"; done
run takeover
for v in idle nokbd; do run takeover "$v"; done
run ssh
run glyphs >/dev/null
rm -f case.log
mkdir -p png
for f in ./*.ppm; do
    [ -e "$f" ] || continue
    n=$(basename "$f" .ppm)
    sips -s format png "$f" --out "png/$n.png" >/dev/null 2>&1 || { echo "FAIL: sips $f"; fail=1; }
done
[ "$fail" = 0 ] && echo "run_all: every case passed; PNGs in $PV/png"
exit "$fail"
