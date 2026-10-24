#!/bin/sh
# Runs every preview case in the build directory $1 (default: the current directory), then converts
# each <name>.ppm to $1/png/<name>.png with sips (macOS). Exits 1 if any case failed.
PV="${1:-.}"
cd "$PV" || exit 2
[ -x ./preview ] || { echo "run_all.sh: no ./preview in $PV (build it first)" >&2; exit 2; }
rm -f ./*.ppm
fail=0
run() {
    if ./preview "$@" >case.log 2>&1; then
        grep -v '^\[Warn\]' case.log | grep -E '^check |passed|ok$' || true
    else
        echo "FAIL: preview $*"; grep -v '^\[Warn\]' case.log | tail -5; fail=1
    fi
}
run boot
run sleep
run nav
run dashboard
for i in 0 1 2 3 4 5 6; do run settings "$i"; done
run dashboard-reconnecting
run dashboard-keys
run settings-forget
run keyboard
run gesture
run art
run terminal
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
