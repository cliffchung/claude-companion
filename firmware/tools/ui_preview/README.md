# Firmware UI preview

A headless macOS renderer that compiles the actual LVGL, fonts, tokens, components (`ui_comp`),
Sessions and settings sources at 1280 × 720. It uses mock hardware responses and stub screens for
unrelated features. It does not connect to Wi-Fi, Tailscale or a device, and does not verify touch
hardware, networking or display performance. Stub data only: never real sessions.

Requires CMake, Ninja, a C compiler and the managed LVGL component downloaded by the firmware
build. Run from the repository root:

```sh
cmake -S firmware/tools/ui_preview -B "$PV" -G Ninja
cmake --build "$PV" -j 8
cd "$PV" && ./preview <case> [variant]
firmware/tools/ui_preview/run_all.sh "$PV"     # every case; PNGs in $PV/png
```

## Cases

| Case | What it shows and asserts |
|---|---|
| `boot` | The splash before `fonts_init()`/`ui_init()`: logo and name in fg, "Starting…" muted, on bg; no redraw once drawn; the hand-off deletes it |
| `sleep` | The pre-Hibernate notice |
| `nav` | Pushes Settings and a component gallery from home; each probe screen's create function asserts the previous screen is already deleted (P4-2). Writes `nav`, `picker` and `confirm` (a toast, the picker sheet and a destructive confirm over the gallery) |
| `sessions <variant>` | Sessions (spec §5.1) on a host-test fixture (`PREVIEW_FIXTURE`, default `agents_synthetic.json`), one case per state: `live`, `selected-needs-you`, `working`, `loading`, `stale`, `reconnecting`, `no-wifi`, `not-found`, `no-host`, `empty`, `interactive`, `sent`, `low-battery`, `16`, `long`, `resort`, `keys`, `idle`, `forget`, `new-key` (the first-run banner points at Settings › Host). Each asserts its own labels and behaviour (the re-sort keeps the selection and draft, Review Focus 1; `long` checks the CJK cell width and the wrap, Review Focus 3; `screen-log` and `screen-dialog` read a captured `claude logs` stream (`PREVIEW_LOG=screen:<fixture>`) and check that the pane shows the conversation and no footer or status line) before the checks |
| `ctrlk` | The Ctrl+K shortcuts overlay over Sessions: two columns, nothing wrapped |
| `settings <0..6>` | Clicks every rail entry (one visible panel, the same screen object), then shows that section |
| `settings 5` with `PREVIEW_FW_CHECK` | Plan 6: About's Update firmware row for `none` (Check), `checking`, `uptodate`, `newer`, `older`, `nofile` and `notapp` (red, cut with dots inside the row), muted / fg / red as spec §8.1 says; `PREVIEW_FW_PREV=1` adds Previous version (`1.5.9+0c1d2e3 · Go back`). With `PREVIEW_CLICK=Update firmware`: the Install confirm (the older line with `older`), Esc starts nothing; opened by a key Cancel has the focus, so an arrow then Enter starts nothing; Tab then Enter starts it once and opens the progress screen. With `PREVIEW_FW_RESTAGE=1` another (older) build is staged while the confirm is open: Install starts nothing and a new confirm names it, with the older line. `PREVIEW_FW_ROLLBACK=0`: Rollback off on the row and the Version note, no check, a tap refuses. `PREVIEW_FW_LAYOUT=0`: "Old partition table" instead, with its own refusal. `PREVIEW_CLICK=Previous version`: the Go back confirm; Tab, Tab, Enter runs `fwu_go_back` on the worker (the stub fails and toasts) |
| `fwupdate <download\|check\|write\|readback\|restart>` | Plan 6: the progress screen over About at `PREVIEW_FW_PCT` (default 50): title, phase line, the fg bar, `3.2 of 6.4 MB · 50 %`, the hint and Cancel (both hidden at `restart`); one 300 ms timer; text, Ctrl+P, Enter and a tap outside Cancel do nothing; Esc and a tap on Cancel call `fwu_cancel`; once the stub reports IDLE with a cancelled note the screen and its timer are gone and the toast reads `Update cancelled. Nothing was changed.` |
| `fw-notes` | Plan 6: each boot note (Updated, rolled back, boot loop, rolling back) toasts once on Sessions; a RESULT note is left for the progress screen |
| `network-focus` | T13-3: the Tailscale switch's refill, a refill as an event makes it, and a Peers rebuild keep the keyboard focus on the same row; Enter then acts on it |
| `keyboard`, `gesture`, `art`, `terminal`, `glyphs` | Keyboard routing, touch gestures, terminal artwork and row spacing (assertions only); `glyphs` lists the symbols each font draws |

## Checks

Every screen case ends in `pv_run_checks` (`checks.c`), prints `check <name>: ok`, writes
`<name>.ppm` and exits nonzero on a failure:

1. **Amber:** no pixel within CIE76 ΔE ≤ 10 of the NEEDS YOU amber outside a `UI_NEEDS_YOU_PART`
   object; with NEEDS YOU expected, some amber is drawn.
2. **Touch targets:** body controls ≥ 64 px visible and ≥ 80 px hit, `UI_BAR_CONTROL` ≥ 48 and ≥ 80,
   neighbours ≥ 8 px apart.
3. **Fit:** no label extends past its parent; a wrapped label is never clipped.
4. **Idle:** nothing is redrawn over the second after the first draw.
5. **Timers:** none under 300 ms (the pty pump excepted while a terminal is open), and the 300 ms
   tick only while Sessions is shown (the firmware progress screen's own 300 ms timer excepted while it shows).

`PV_SKIP_AMBER` and `PV_SKIP_TARGETS` exist only for the screens not rebuilt yet: the `settings*`
cases pass them until Task 7 rebuilds Settings. A control a scrolled list shows only in part is
skipped by the touch check (it is checked once scrolled into view).

The stubs take their state from the environment: `PREVIEW_FIXTURE` (a file in
`test/host/fixtures`, or `loading` / `none`), `PREVIEW_WIFI=0`, `PREVIEW_HOST=0`,
`PREVIEW_FAIL=notfound`, `PREVIEW_FAIL=newkey` (first run: an unknown key with Trust this key, and with `PREVIEW_CLICK=Trust this key` its confirm), `PREVIEW_BAT=<percent>`, `PREVIEW_CHG=1`, `PREVIEW_LOG=long`, `PREVIEW_FW_CHECK`, `PREVIEW_FW_PREV=1`, `PREVIEW_FW_PCT`, `PREVIEW_FW_RESTAGE=1`, `PREVIEW_FW_ROLLBACK=0`; the
`sessions` cases set them themselves.

`run_all.sh $PV` runs every case and converts each `.ppm` to `$PV/png/<name>.png` with `sips`.
Compare the PNGs by eye with `docs/superpowers/specs/mockups/`. These are drawing checks on the
host, not ESP32-P4 timings: the real panel, RGB565 blending and the device's RAM are checked on
the Tab5.
