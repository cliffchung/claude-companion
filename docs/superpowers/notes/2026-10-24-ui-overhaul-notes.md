# Plan 4 (UI overhaul): measurements and rulings

**Device:** M5Stack Tab5 on USB (`/dev/cu.usbmodem1101`, ruling P4C-1). **Host:** `my-host.local`, 192.168.1.50, LAN Host entry, Tailscale off.
Each task appends its section. Screenshots and raw size reports stay in scratch storage, not in git.

## Task 1: measurement verbs, baseline, Music / Camera / USB MSC removed

### The verbs (every later gate uses them)

- `ui heapmin start` starts `heap_caps_monitor_local_minimum_free_size_start()`. `ui heapmin stop` prints
  `heapmin: internal min <N> B, largest <L> B, now <F> B`. N is `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)`,
  read before the monitor stops. It takes no LVGL lock. A second `start` prints `heapmin: already started`, and a `stop`
  with no `start` prints `heapmin: not started`.
- `ui timers` takes `ui_lock()`, walks `lv_timer_get_next`, and prints `timer <period_ms> ms, last run <age_ms> ms ago, paused=<0|1>`
  per timer, then `timers: <n>, under 300 ms: <k>`. Period and last run come from `src/misc/lv_timer_private.h`, the same
  private-header route `ui_core.c` already takes. LVGL 9.6 has no public getter for them.
- `key ctrl space` posts `KEY_EV_CHAR cp=' '` with `MOD_CTRL`. `key log on` shows it as `type=char cp=' ' key=none mods=ctrl`.

**How to read `ui timers`.** With the screen off, every timer reads `paused=1`, because power.c pauses them in Off (power.c:584).
Read it with the screen on. The 2-minute idle step below ends with the screen off (dim 30 s, off 120 s under `power set defaults`).

**Driving Settings › Network without a tap.** `ui settings`, then `key down` three times and `key enter`. The first arrow
focuses the back button, and the next two reach System and then Network. If the screen has dimmed, the first key only wakes it
(rule 7), so send the keys within 30 s of the last input. Don't wake the screen with `power sim touch` while it is on: that
press lands on the dashboard at (639, y).

### Baseline (Plan 3 build)

This is the Plan 3 code at 8b94efb plus only the three verbs. Tailscale is off, the Host link is on the LAN, and the device is on USB.

| Item | Baseline |
|---|---|
| `idf.py size`, total image | 8,317,249 B |
| DIRAM used | 214,126 B (`.text` 122,390, `.bss` 58,656, `.data` 33,080) |
| `.iram0.text` / `.dram0.data` / `.dram0.bss` / `.dram1.bss` | 122,390 / 33,080 / 9,688 / 48,968 B |
| `ts` after 2 min idle on the dashboard, 3 `companion-test-*` sessions (screen had gone off) | free 123,495 B, min 34,156 B, largest 34,816 B |
| `ts` on the dashboard with the screen on (link up) | free 122,607 to 122,683 B, largest 32,768 B |
| `ui heapmin` over opening Settings › Network | min 101,171 B, largest 34,816 B, now 134,919 B |
| `ui heapmin` over a take-over (`companion takeover`, 60 s, `key ctrl ]`) | min 64,115 B, largest 47,104 B, now 115,971 B |
| `ui timers`, dashboard, screen on | `timers: 8, under 300 ms: 6` (300, 500, 60, 10, and four 16 ms timers, three of them paused) |
| Idle power on USB, 5 min on the dashboard, no input (`power_report.py --log --include-charging`) | 609 samples: tag 0x00 n=6, 8 mW; 0x01 n=90, 9 mW, 0.23 mWh; 0x02 n=513, 9 mW, 1.26 mWh; estimate 1668.2 h. `src=usb-standby`, so the INA226 reads about 1 mA, and the figure says little about battery draw |
| Last check-in awake (`power checkins`) | `hib-lan` 6,354 ms, 1.461 mWh (10-24 01:57:54, the Plan 3 build before the verbs; 6,223 to 8,372 ms across the last 8) |
| Boot (C21) | `boot complete in 2791 ms`; `hib: boot to live: 7989 ms` (kind 0) |

### After the removals (this commit)

| Item | After | Delta |
|---|---|---|
| Total image | 7,439,121 B | −878,128 B (−0.84 MiB) |
| DIRAM used | 199,582 B (`.text` 115,894, `.bss` 55,568, `.data` 28,120) | −14,544 B static internal |
| `.iram0.text` / `.dram0.data` / `.dram0.bss` / `.dram1.bss` | 115,894 / 28,120 / 29,744 / 25,824 B | the linker moved `.bss` between dram0 and dram1; the sum is −3,088 B |
| `ts` after 2 min idle on the dashboard, 3 sessions (screen off) | free 144,395 B, largest 47,104 B | **+20,900 B** |
| `ts` on the dashboard with the screen on | free 143,807 B, largest 47,104 B | +21,124 B or more |
| `ui heapmin` over Settings › Network | min 121,979 B, largest 47,104 B, now 157,407 B | +20,808 B |
| `ui heapmin` over a take-over | min 57,763, 63,699 and 59,771 B (three runs), largest 47,104 B | −352 to −6,352 B against one baseline run (see below) |
| `ui timers`, dashboard, screen on | `timers: 8, under 300 ms: 6` | none |
| Boot | `boot complete in 2792 ms`; boot to live 7,898 and 7,519 ms | about the same |

**Against §7.2.** These three removals' share of §7.2 is Music + codec 1.0, camera 0.5 (+4.4 if the pipeline unlinks),
MSC 5.6, plus the MSC driver task's 4.4. That makes 11.5 KB, or 15.9 KB with the camera pipeline. The camera pipeline did
unlink (below), so the expected figure is 15.9 KB. The measured steady-state gain is about 20.9 KB, about 5 KB more.
The likely source is a second task that §7.2 doesn't count: `usb_mgr.c`'s own `usb_mgr` polling task (4 KB stack plus its
TCB), which existed only to mount and unmount the flash drive. It goes together with the MSC driver's background task.

**Unlinked (`idf.py size-components`).** These are gone from the link:
- `libespressif__esp_video.a` (43.4 KB), `libesp_driver_isp.a` (25.8 KB) and `libesp_driver_cam.a` (14.7 KB): `esp_video`, ISP and CAM unlinked;
- `libesp_audio_codec.a` (580 KB) and `libesp_audio_simple_dec.a` (24.8 KB);
- `libespressif__tinyusb.a` (24.2 KB, 5.3 KB of it internal), `libespressif__esp_tinyusb.a` (13.1 KB) and `libespressif__usb_host_msc.a` (9.6 KB).

`libespressif__esp_codec_dev.a` (13.2 KB) also left the link, because nothing calls it now. The BSP still depends on it,
so the mic work can use it again. `managed_components` no longer holds `espressif__esp_audio_codec`, `espressif__usb_host_msc`
or `espressif__esp_tinyusb`.

**Take-over minimum (open).** The steady-state and Settings minimums rose by about 21 KB, but the take-over minimum didn't
(57.8 to 63.7 KB after, against one 64.1 KB baseline run). The trough during a take-over isn't set by the free total alone,
and one baseline sample can't show the spread. All runs clear B2's 40,960 B. T13's final comparison should take three
take-over runs on each side.

**USB keyboard.** PENDING for the user's visit. No USB keyboard is attached: the boot log has no `USB keyboard connected`.
The HID host is unchanged and still installs at boot.

### Rulings and deviations (Task 1)

- **C1.** `usb_mgr.h` keeps `usb_mgr_status_text()`, now a constant `"USB host"`, for `ui_devinfo.c` until T3 deletes that
  screen. `ui_files.c` loses its `USB_MOUNT` and `usb_mgr_msc_mounted` branches and its audio branch, so it browses the TF
  card only until T2. `USB_MOUNT` is gone from `storage/sd.h`.
- **C11.** The preview CMake drops `ui_camera.c`.
- **Preview link fix (found here).** The preview didn't link at the base 8b94efb. Plan 3's `ui_settings.c` calls `sip_parse`,
  `wifi_mgr_static_ip_get/set` and `link_down_restart`, and the preview had none of them. The preview now compiles
  `net/fast_rules.c`, a pure-C file, and stubs the other three. The `home` case's tile assert drops from 9 to 6.
- **Left for later tasks.** `sd_unmount_keep_card()` and `sd_remount_after_msc()` in `storage/sd.c` have no callers now.
  T11 owns `sd.c`. The unused `HOME_MUSIC`, `HOME_CAMERA`, `HOME_USB`, `USB_*` and `PLAY`/`PAUSE` strings stay in `i18n.h`
  for T3 and T12. The More launcher still reserves its 56 px bottom strip, which the mini player used to fill. T3 deletes the launcher.

## Task 2: Files, the viewers, the SD screenshot and Backup

**Removed.** `ui_files.c`, `ui_image.c`, `ui_text.c`, `ui_backup.c`, `storage/backup.*`, `media/` (image_loader, exif, tjpgd),
the three-finger SD screenshot (`screenshot_init`, `screenshot_take`, `write_bmp`, the 60 ms poll), `wifi_mgr_export_json` /
`wifi_mgr_import_json`, the Files tile, the Backup category (rail is 8 entries), the "three-finger tap" note in Display, 44 `STR_*`
entries, and LVGL's LODEPNG, BMP, TJPGD, GIF and FS_STDIO (`CONFIG_LV_CACHE_DEF_SIZE=0`). The `screenshot` console verb
(`screenshot_dump_usb`) stays. `ui_files.c`'s stale header comment and its audio-to-text-viewer routing went with the file.

**Ruling C2 applied.** `server_store_export_json` / `server_store_import_json` stay: `save()`, `server_store_init()` and
`set_on_sd` in `server_store.c` persist the host list through them, and T11's `server_store_import_sd_once()` needs the parser.
Only the Backup callers are gone, so the plan's Step 1 grep still finds those two names (expected). `ui_preview/CMakeLists.txt`
(C11) listed none of the removed sources, so it needed no edit. The preview's tile assert drops 6 to 5, and the settings loops go from 9 to 8.

**Measurements** (Task 1 values from the table above).

| Item | Task 1 | Task 2 | Delta |
|---|---|---|---|
| Total image | 7,439,121 B | 7,341,993 B | −97,128 B (−975,256 B against the Plan 3 baseline) |
| `ts` free internal, dashboard, about 1 to 2 min after boot (no sessions on screen) | 144,395 B (after 2 min idle) | 160,899 B | +16,504 B |
| `ui timers` | 8 timers, 6 under 300 ms | 6 timers, 4 under 300 ms (300, 500, 10 ms and the pty pump set); no 60 ms | the 60 ms timer is gone |
| `screenshot` | `@@SHOT jpeg` | `@@SHOT jpeg` | same |

The measured gain (+16.5 KB) is well over the spec's expected 5.7 KB (Files 5.1 + viewers 0.6). The extra is most likely LVGL's
decoders and file-system driver state plus the image cache bookkeeping, now compiled out. Caveat: the Task 1 figure came after
2 min idle with three test sessions and the screen off, and this one after a fresh flash, so the two are not like for like.
After the flash `companion status` is unchanged (connected, LAN 192.168.1.50, host key SHA256:EXAM...), and the heartbeat resumed.

## Task 3: the More launcher, the fork status bar, rotation, Device info, OTA and USB screens

**Removed.** `ui_home.c` (the More launcher), `ui_devinfo.c`, `ui_ota.c` (`ota/ota.c` and `storage/sd.c` stay for Plan 7),
`ui_statusbar_attach` / `statusbar_tick` / `statusbar_icon` and its 400 ms timer, `ui_status_text` (and, per C3, its call in
`ui_terminal.c`'s `build_status`, with the label it filled; the rest of `build_status` waits for T10), `usb_mgr_status_text()`
(C1), the `STATUS_*` bitmask, the Status bar category (rail is 7: System, Network, Display, Power, Input, Security, About),
the rotation row, the System › OTA URL row (T7 adds `ota url`), and the `STR_HOME_*`, `STR_SB_*` and `STR_STATUS_*` strings.
`app_settings_t.rotation` and `status_items` are marked reserved (Plan 4); the blob layout is unchanged.
`display_apply_settings` always sets `LV_DISPLAY_ROTATION_90`.

**Added.** The dashboard footer's "More" is now "Terminal", and Ctrl+M is `HK_D_TERMINAL`; both push `ui_hosts_create`
(the P4-9 bridge: `ui_servers_create` renamed in `ui_servers.c`, and `term_create`'s fallback uses it). The overlay lines say
"Ctrl+M  terminal" and "Esc  back (Settings, Terminal)". Network gets the bridge row "Wi-Fi networks" (value: the SSID),
which opens `ui_wifi_create`; T8 deletes it.

**Implementer ruling T3-a (hard rule 5).** "Based on Clientre 5 by Mozgi512 (MIT)" lived only in Device info. So that About
keeps showing it until T7 rebuilds About, the line is appended to `STR_ABOUT_TEXT` (Settings › About alert), and
`tools/check_trim.sh`'s allowlist moves from `ui_devinfo.c` to `i18n/i18n.h`. Cost if wrong: one line T7 rewrites anyway.

**Preview.** `CMakeLists.txt` drops `ui_home.c` (C11). The `home` case, the home tile keyboard walk in `keyboard`, the tile-fit
check, the `ui_ota_create` / `ui_devinfo_create` stubs and `status_items` are gone; an unknown case now exits 2. The settings
loops and the category assert use 7 (C13). `dashboard-keys` asserts `ui_nav_is_top(ui_hosts_create)` after Ctrl+M. The README
drops `home`, the portrait (720 × 1280) commands and table row, and the Backup / Status bar categories. `font-policy` still
aborts on its FONT_SMALL == FONT_BODY assert, as at the base commit e57d44d (C8: T4 deletes that case).

**Device (C18).** The old check "no 400 ms timer on the dashboard" asserted nothing, since only the launcher attached the
status bar. Replaced by the grep (no `ui_statusbar_attach` anywhere) plus `ui timers` on the dashboard and after `key ctrl m`.

| Item | Task 2 | Task 3 | Delta |
|---|---|---|---|
| Total image | 7,341,993 B | 7,282,043 B | −59,950 B |
| `ts` free internal, dashboard, a few min after boot, Tailscale off | 160,899 B | 161,283 B | +384 B |
| `ui timers`, dashboard | 6 timers (300, 500, 10, 16 ×3) | same 6; no 400 ms | none |
| `ui timers`, after `key ctrl m` (server list) | n/a | 2000, 500, 10, 16 ×3; no 400 ms | |

`key ctrl m` opens the server list and `key esc` returns to the dashboard (timers 2000 ms → 300 ms). `ui settings`, then
Network › Wi-Fi networks opens the Wi-Fi screen. The first key after the screen dims is swallowed as the wake key, as designed,
so device scripts must check the screen before relying on a key. No test sessions were created, and no card was selected or opened.

## Task 4: tokens, components, instant navigation and the preview checks

**Added.** `ui_tokens.h/.c` (pure C: the §8.1 colours and sizes, the terminal values and `ui_tk_term_ansi`, and
`ui_tk_needs_you_hex()`, the one amber literal in `ui/`), and `ui_comp.h/.c` (every component in the brief, on
file-static styles built once by `ui_comp_init()`). `ui_lint.py` (rules `amber-literal`, `amber-accessor`,
`exec-outside-cc_cmd`) runs as the host test `ui_lint`; `ui_tokens.c` joins the host `pure` library.

**Removed.** `ui_theme.c/.h` (presets, density, glow, the widget theme), `ui_appearance.c` (Appearance and the ANSI
editor), `ui_motion.c/.h` (the 33 ms phosphor sweep and scan lines), `fonts_select` / `fonts_set_pixel_icons`, the
Display › Appearance row, the `cli-colors` and `font-policy` preview cases (C8), the preset cycling, the `motion` case and
the width/height/preset arguments. Montserrat 22 and 30 are off; the LVGL default font is Montserrat 14.

**Navigation (B2, P4-2).** `nav_show_top` loads a blank screen, deletes the old one, then creates and loads the new one.
The splash is deleted the same way at the first `ui_nav_home`.

**Implementer rulings (T4-a to T4-f).**
- T4-a: `ui_comp.h` has five calls beyond the brief, so the rail and `ui_core.c` use shared styles too:
  `ui_srow_set_selected` (the rail's panel-2 + 50 % fg outline), `ui_comp_install_theme`, `ui_comp_focus_ring`,
  `ui_comp_screen` and `ui_comp_sheet`. Cost if wrong: names to fold later.
- T4-b: `ui_picker` and `ui_confirm_ex` are declared in `ui_comp.h` but live in `ui_core.c`, beside the modal overlay
  that `ui_keyboard_route` recognises. Cost if wrong: a move.
- T4-c: the `ui_theme_radius(x)` shim keeps 0 as 0 (bars stay square) and maps anything else to `UI_TK_RADIUS` (C5
  says `UI_TK_RADIUS`; a literal reading would round the terminal's bars).
- T4-d: `UI_ROW_SWITCH` starts on when `value` is non-NULL and non-empty; a tap on the row toggles the switch and sends
  `LV_EVENT_VALUE_CHANGED` to it, so existing switch callbacks (`lv_event_get_target_obj` = the switch) keep working.
- T4-e: `CONFIG_LV_THEME_DEFAULT_TRANSITION_TIME=0` and `CONFIG_LV_THEME_DEFAULT_GROW=n` (and the same in the preview's
  `lv_conf.h`): the default theme's 80 ms transitions animate `BG_OPA`, which §11 rules out.
- T4-f: the soft keyboard attaches as before; `softkbd_attach_textarea` already shows the keyboard only when
  `!keyboard_any_present()`, checked each time the field is focused (a keyboard plugged in later still counts).
- The dashboard and the vitals now write a style or a HIDDEN flag only on a change. Before, every 300 ms tick re-set the
  summary colour and the link dot, and every 900 ms the vitals, which redrew a static dashboard (the idle check caught it).

**Preview.** `./preview <case> [variant]` at 1280 × 720. `boot`, `sleep`, `nav` (also writes `picker` and `confirm`),
`dashboard` and `settings 0`–`6` print `check <name>: ok`; `dashboard*` and `settings*` pass `PV_SKIP_AMBER |
PV_SKIP_TARGETS` until Tasks 6 and 7. `nav` pushes Settings and a component gallery from home through probe screens whose
create functions assert that the previous screen is no longer valid. `run_all.sh $PV` runs every case and writes the PNGs.
Negative check: an amber label without `UI_NEEDS_YOU_PART` in the gallery failed the amber scan (392 px), and an
overflowing footnote failed the fit check.

**Glyphs.** MaruMinya (body, label and title) draws ← ↑ ↓ › ● … ↵ ⌃ ·, so the header's "← Sessions", the count
"● N need you", the rail's "›" and the boot "Starting…" need no fallback. Only ⏺ is missing outside the terminal.

**Device** (P4 flashed; Tailscale off, LAN Host link):

| Item | Task 3 | Task 4 | Delta |
|---|---|---|---|
| Total image | 7,282,043 B | 7,217,227 B | −64,816 B (`.rodata` −61,552, `.text` −3,200) |
| DIRAM | 192,400 B | 192,520 B | +120 B (`.bss` +184: the static styles; `.data` −64) |
| `ui heapmin` over opening Settings › Network | 121,979 B | 157,935 B (largest 63,488) | gate 66,560: pass |
| `ts` internal free on the dashboard, before and after 10 × (`ui settings`, `key esc`) | | 160,107 → 159,991 B | −116 B: no leak |
| `ui timers`, dashboard | 300, 500, 10, 16 ×3 | the same | no sweep timer |

§7.1 expected 8.2 KB of code and 70 KB of fonts for rows 23–25; the measured drop is 63.3 KB in all, most of it the two
Montserrat sizes. Static internal RAM rose 0.1 KB instead of falling 0.2 KB, because the component styles are static.
The 1 s boot splash can't be captured: the console starts after `ui_nav_home`, so the splash check is the preview's
`boot.png` (fg on bg, no orange) and stays PENDING for the visit. `ui settings` then `key esc` is instant. A key sent while
the screen has dimmed is swallowed as the wake key (rule 7), so scripted Esc loops must keep the screen awake; one early loop
lost two Escs that way. No test sessions were made, and no card was selected or opened. Final state: `ts off`,
`power set defaults`, static address off (DHCP), Host link connected on the LAN.

**Task 4 fix round 1 (ruling P4C-5, amends spec §8.1).** A bar control's hit area is the bar's full height (header 56,
footer 64, banner 48) and ≥ 80 px wide; rows keep 64 visible / 80 hit. `pv_touch_targets` now clips each control's
extended area to every ancestor the way `lv_indev_search_obj` does (a parent's children are searched only on its coords,
grown by its ext draw size when `OVERFLOW_VISIBLE`). The header slot, footer groups and the slider row are made
`OVERFLOW_VISIBLE` with an ext draw size so their children's hit area isn't clipped; the bar itself still clips.
New preview cases `targets-clip` (fails a clipped button, passes the real bars) and `softkbd` (which found that
`softkbd_create` dropped its own size: `lv_obj_remove_style_all` must come first). `run_all.sh` fails on LVGL warnings.
Deviation kept: dialog titles use the regular 24 px body face; the family has no bold 24 px face.
Device after the fix: Settings › Network heapmin min 157,735 B.

## Task 6: Sessions (master-detail)

`ui_sessions.c` replaces `ui_dashboard.c`: a 440 px list with a constant pool of 16 `ui_session_row`s, a detail pane
(title, log block, answer field + mic slot, Send/Open + note), header/banner/footer from `ui_comp`, all built once
per visit and updated by setters on change from the 300 ms tick. Vitals: one shared 30 s timer in `ui_vitals.c`
(created with the first cluster, deleted with the last); the link dot is `cc_link_connected()` green /
`cc_link_failure()` red / muted, with the word "Host". The 500 ms `forget_poll_cb` is gone (P4-3, below).

**Measured** (device: P4 flashed, Tailscale off, LAN Host link; preview at 1280 × 720):

| Item | Value | Gate |
|---|---|---|
| Log block (§5.1 "about 62 × 10") | 62 cols × 9 rows (device log `sessions: log block: 62 cols x 9 rows`; preview: 750 px content = 62 cells) | |
| `ts` on Sessions, > 2 min after boot, 2 test + about 10 real sessions (12 rows) | internal free 149,047 B, largest 63,488 B | ≥ 123,495 B (Task 1 dashboard): pass |
| `ui heapmin` over `companion takeover` of a test session (~90 s) then `key ctrl ]` | min 98,727 B, largest 63,488, now 144,799 B | ≥ 40,960 B: pass |
| `ui timers`, Sessions, screen on | 300 (tick), 30000 (vitals), 10 (key drain), 16 × 3 paused (LVGL) | only the 300 ms tick under 1 s; no 500 ms: pass |
| `ui timers`, screen Off | every timer `paused=1`, the 30 s vitals timer included | B1: pass |
| `ui timers`, take-over | 16 (pty pump), 550, 10, 16 × 3 paused; no 300, no 30000 | |
| `ui timers`, after Ctrl+T / Ctrl+M (server list) | 2000, 10, 16 × 3 paused; Esc brings back the 300 ms tick | |
| Review Focus 2: `power go off`, `power sim touch` | woke; `power wakes`: `touch swallowed=1`; no pump (the one new 16 ms timer is the sim pointer's paused event-mode read timer) | pass |
| Review Focus 2: `power go off`, `key down` | `key swallowed=1`; the selection stayed on the test session (screenshot) | pass |
| `key text abc`, then Ctrl+Space × 11 | the field still reads "abc"; toast "Voice input comes in a later update" | pass |
| `key text 2`, `key enter` on the blocked test session | `sessions: answer d0000004: ESP_OK`; the session replied about "2" and asked again (WORKING only for a moment) | |
| `power sim battery 14` | red banner "Battery 14 %. The Tab5 sleeps at 10 %." with [Dismiss] | pass |
| `idf.py size` (Task 4 → Task 6) | image 7,217,227 → 7,227,251 B (+10,024); DIRAM 192,520 → 188,144 B (−4,376: the old dashboard's static text buffers and notes are PSRAM now) | |

C25: the 16-row RAM budget is proven by the constant pool and the preview `sessions 16` (16 rows, row 16 inside the
list after a scroll); the device run showed 12 rows. "Sent ✓" lasts 3 s and the device screenshot transfer is lossy
(below), so on the device it rests on the log line; the preview `sessions sent` asserts it.

**CJK and symbol widths (Review Focus 3, T5 carry).** In `FONT_MONO` U+65E5 advances 24 px (two 12 px cells), and so
do several Claude Code TUI symbols when drawn by a label (… ← ↓ █ ░ ─ ╭ ⎿ ● ✽); ✓ advances 16 px. The terminal draws
them in its own grid, so only the Sessions log label is affected. Rulings:
- T6-1: `sm_wrap_tail` counts terminal cells (`vt_wcwidth`, moved unchanged from `vt.c` into the pure
  `term/wcwidth.c`): wide characters take 2 and never split. `sm_wrap_tail_fn` takes a width function, and Sessions
  passes the label's own glyph advances (`ceil(adv / 12)`), so the label never wraps a row again. The label is
  content-sized, so a row the wrap got wrong is clipped by the block on the device and fails the preview fit check.
  `sessions long` checks every row's pixel width against the block (a mutant using terminal cells fails: "log row 4 is
  952 px, the block 750").

**Rulings and deviations:**
- T6-2: low battery is dismissed with a [Dismiss] chip. A whole-banner tap target would be a 48 px body control, which
  fails §8.1 / P4C-5. Its input is `power_source()` (the rules' view, so `power sim battery` counts; USB re-arms).
- T6-3: Stale shows only the "Updating…" banner (§5.1); the rows dim to 50 % and go inert for No Wi-Fi, Host
  unreachable, Reconnecting and any time the link is down.
- T6-4: Host unreachable shows `cc_link_status_text` cut at its first " (" and capitalised ("Host not found at
  my-host.local"); down 30 s with no failure reason: "Can't reach <host>. Is it awake?"; failing polls: "Can't
  load sessions from the host…"; too large: "Session list too large to load". A Radio-off wake counts as Wi-Fi
  connecting, so it shows Reconnecting, never No Wi-Fi (P4-4).
- T6-5: the footer chip reads "⌃T/⌃M Terminal" (the T3 minor: Ctrl+M named as the alias).
- T6-6: the Ctrl+K sheet closes on a press, and the release is swallowed (`lv_indev_wait_release`), so the closing tap
  never lands on a row. LVGL already drops that release when the pressed object is deleted; the call is kept as
  belt-and-braces. Built on demand; its keys column is as wide as the widest key, so neither column wraps.
- T6-7: C24 kept: Esc clears the draft and never leaves Sessions. Enter on an empty field opens the session.
- T6-8: the detail title uses `body` (24 px); the family has no 26 px bold face. The repo and age sit on a muted line
  under the title, because a long name wraps. B3 interactive panes show the age, not a clock time.
- T6-9: the header count shows only with the list (not over "Set up host").
- T6-10: the pill text comes from `sm_pill_text` (C7 closed); `ui_session_row_set` refits the name whenever the
  name's laid-out width changes (T4 M12); `pv_bar_hits_clear` asserts that no bar control's hit area meets row 1's
  (T4 re-review). The touch check skips controls that a scrolled list shows only in part.
- T6-11: the textarea cursor is steady (`anim_duration` 0 on `LV_PART_CURSOR`): the default theme blinks it every
  400 ms, which is an idle redraw. `sessions idle` holds 0 px redrawn over 2 s and again over 31 s (past a log
  re-fetch, an age refresh and the vitals timer).
- P4-3 (forget fold): `run_forget` posts its success through `ui_async`, and the toast is gated on
  `cc_link_forget_done` changing, so a forget done in time toasts once on any screen and a late one is toasted by the
  Sessions tick. The job only needs Sessions' worker, which lives for the app, so it finishes on any screen.
- New fixture `agents_working.json` (no blocked session) for `sessions working`, besides `agents_16` and
  `agents_long`.
- `settings-forget` was deleted as the brief says; `sessions forget` covers the toast.

**Device notes.** P4C-4: at boot the selection is row 0 of the first snapshot. On the first flash that was a real,
stopped session (my test session didn't exist yet), and the selection followed it. I moved it with one `key up` onto my
own test session (the first key was swallowed as a wake key). Nothing was sent or opened: Sessions fetched that row's
log tail read-only, as the old dashboard did at boot. Later boots had a test session first. When the test sessions
were stopped, the selection moved to the row at the same position (§5.1 rule). The device screenshot transfer drops
64-byte USB packets on 130–150 KB frames ("length mismatch"); 1 in 3 to 1 in 20 tries succeed (tooling, not changed).
Screenshots with real session names are in the scratchpad only. Final state: test sessions stopped, `ts off`,
`power set defaults`, DHCP (static off), Host link connected on the LAN.

## Task 7: Settings shell, and Host, Power, Keyboard light, Display & time, About

**Shape.** One screen: `ui_header` ("← Sessions", "Settings", the shared vitals; no timer of its own), a 320 px rail
of six 72 px `ui_rail_row`s (transparent until selected: panel-2 with a 50 % fg outline) and one scrolling panel
with a `FONT_TITLE` heading. A rail click cleans and refills the one panel (`panel_fill`); the screen object stays
(the preview asserts the body has exactly two children, the rail and the panel, and the same screen after every
click). Ctrl+P lands on the last used section, Host after a boot (RAM only); `ui_settings_open(s)` is what the
Sessions banners call (No Wi-Fi → Network; Host settings and Set up host → Host). The only extra timer is the 2 s
Diagnostics refresh, bound to its label, so a collapse or a section change deletes it.

**Device gate (Tailscale off, LAN Host link).**

| Check | Result | |
|---|---|---|
| `ui heapmin` over `key ctrl p` (Sessions → Settings › Host) | internal min **141,675 B** (gate 66,560) | pass |
| … then each section by rail (keys): Network / Power / Keyboard light / Display & time / About | min **157,295 / 158,791 / 161,223 / 164,483 / 163,943 B** | pass |
| `key ctrl p` after a boot | lands on Host (screenshot) | pass |
| Power by rail, `key esc`, `key ctrl p` | lands on Power (screenshot) | pass |
| Host › Public key › Show vs `sshkey` | the sheet's line and `@@PUBKEY` agree past 50 characters (`ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIEXAMPLEPUBKEY0…`) | pass |
| `power set dim 600`, About › Reset settings › Reset | `power settings` dim=30 off=120; `clock` tz=PST8PDT,M3.2.0,M11.1.0 unchanged; keyboard light bright 0 → 20 (its default); Host link, pin and fallback unchanged | pass |
| `key layout jis`, `key layout stock` | `key layout: jis`, then `stock` | pass |
| `ota url <url with ?token>`, `ota url`, `ota url none` | `ota url: https://updates.example.com/…?<masked>`; `none` | pass |
| About › Diagnostics expanded, `ui timers` | one 2000 ms timer; collapsed: none | pass |
| Host › Forget on the device | not pressed (ruling): the preview drives it against the stub | PENDING |
| `idf.py size` (Task 6 → Task 7) | image 7,227,251 → 7,236,475 B (+9,224); DIRAM 188,144 → 188,352 B (+208) | |

User state recorded before the reset and put back after it: brightness 80, keyboard light mode 0 bright 0 rgb
0080ff needs-you 1 dark 5 (`key light 0 0 0080ff 1 5`), layout stock, OTA URL none. Final state: test session
stopped, `ts off`, `power set defaults`, DHCP, Host link connected on the LAN.

**Rulings and deviations:**
- T7-1: the Host row shows the entry's address (§5.7), and until Task 9's host form the Address row shows
  `user@address[:port]` and opens `ui_hosts_create`. With no entry the Host row reads the configured name or "Not set".
- T7-2: action rows keep the whole row as the hit target (§5.6); their chips ("Forget", "Sync now") are drawn by
  `ui_srow_chip` and are not controls of their own (a `ui_chip` is a bar control and would fail P4C-5 in a 64 px
  body row).
- T7-3: Network keeps the Plan 3 rows unchanged, the fallback address included, until Task 8 (C14); the Host
  section also has Fallback address (§5.7). `settings 1` and `keyboard-settings` (which ends on Network) keep
  `PV_SKIP_TARGETS`; every other settings case runs with no skips.
- T7-4: short dialog buttons (T4 carry, P4C-5): `dialog_buttons` pads the row 8 px sideways as well as up and down,
  so the end button's 80 px hit area is never clipped by the row. `targets-clip` asserts it on an OK alert and on a
  one-letter confirm whose button sits at its 64 px minimum (it fails, 72x80, with the padding removed).
- T7-5: the touch check counts a control's hit area (coords + ext click) when it decides a scrolled container shows
  it only in part. A row a scroll leaves at the panel's edge (`settings 2 PREVIEW_SCROLL=400`: Hibernate after at
  y 652, its 8 px below cut off) is checked once scrolled into view, where LVGL keeps it inside the 24 px padding.
- T7-6: the footer hint is `ui_keyboard_hint(label)` in ui_core: shown while the last input was a key (set in
  `ui_keyboard_route`), hidden on a pointer press (an indev `LV_EVENT_PRESSED` callback). No timer. It sits at the
  bottom of the rail. The Ctrl+Tab "UI: Tab / Enter / Esc" / "SSH input" toasts are gone (Task 10 gives Terminal the
  same hint).
- T7-7: keyboard order is tree order: the back chip, the six rail rows, then the panel's rows. From a fresh
  screen the second Down reaches rail row 0. Enter on a rail row refills the panel and keeps the focus on the rail.
- T7-8: `settings_reset_user` resets through `settings_reset_scope`, then `power_settings_defaults()` (which now
  notifies the power task, so the ladder takes the default timings at once) and the brightness.
- T7-9: console read forms added for the restore rule: `key layout` (no argument prints it), `key light [<mode>
  <bright> <rrggbb> [<needs> <dark>]]`, `ui brightness [5..100]` and `ota url` (prints the masked value).
- T7-10: Diagnostics reads the Wi-Fi channel once when it opens (an SDIO RPC) and the cached RSSI on each 2 s tick.
  The IDF line shows `esp_get_idf_version()`, which is a git hash on this checkout.
- T7-11 (T6 N1): a Sessions row clears its press key on `LV_EVENT_PRESS_LOST` too.
- The public key row reads "ssh-ed25519 … claude-companion · Show": the key's comment up to its '@'.

**Task 7 fix round 1.**
- I1: `scrolled_out` counts an edge only while the scroller can still scroll toward it, so a clip no scroll undoes
  (sides, the top at scroll 0) fails again. `targets-clip` adds an unpadded 10-row scroller that the check must
  report (`"Row 0" ... hits 600x72`). T7-5 now reads that way.
- I2: cc_link's no-host status says "no Host set up". The Host › Link row and the Sessions banner share
  `ui_sessions_link_down_text`: "No Host set up", "Can't reach <name>. Is it awake?", or the failure up to its
  parenthetical, so no frozen "(retry in Ns)". Preview `settings 0` with `PREVIEW_HOST=0` / `PREVIEW_FAIL=notfound`
  asserts no "Mac" and no "retry in".
- M1–M6: the Diagnostics and reset-reason text is `STR_*`. `ota url` masks userinfo too (pure `sys/url_mask.c`,
  host-tested). `settings_reset_user` writes NVS once, and the extra `notify()` is gone (`power_kbd_light_refresh`
  already wakes the task). Preview Diagnostics covers Esc while expanded. The reset test covers the reserved pw
  fields. `key light` and `ui brightness` parse strictly.

## Task 8: Settings › Network

**Shape.** `ui_settings_net.c` owns the Network panel (`ui_settings_fill_network`) and the three pushed sub-pages
(`ui_settings_sub_create`, arg a `ui_net_sub_t`; header "← Settings", Back lands on Settings › Network). One column of
64 px rows: Wi-Fi (the current network, Networks, Advanced), then Tailscale in U10's order (switch, Status, Auth key,
Device name, Control server, Connection, Peers, Forget), every row shown with the switch off. `ui_wifi.c`,
`ui_tailscale.c` and the P4-9 bridge row are gone; the SD auth-key boot fallback is gone (`load_auth_key` reads
`g_settings.ts_auth_key` only). No timer: scan results come by `WIFI_EV_SCAN_DONE`, peer changes by
`tailscale_set_event_cb`, both through `ui_async`, both slots cleared on delete.

**Device gate (Tab5, LAN Host link).**

| Check | Result | |
|---|---|---|
| Tailscale off: `ui heapmin` over `ui settings network` (from Sessions) | internal min **144,007 B** (gate 66,560) | pass |
| Tailscale off: over `ui net networks` / `advanced` / `peers` | **165,531 / 163,907 / 164,535 B**; `ui timers` the same 5 as Settings (30 s vitals, 10 ms key drain, 3 paused) | pass |
| `ts on`, connected: over `ui settings network` | internal min **71,123 B** (gate 20,480) | pass |
| `ts on`: over `ui net peers`; the page | **100,287 B**; 6 peers listed (4 online, 2 offline), the same counts as `ts` | pass |
| Status row with `ts on` | "connected · <ip> · DERP <code> · 4 peers online" (screenshot, scratch only) | pass |
| `ts off` | `ts: off`; link back on the LAN path | pass |
| Rejoin from Networks › Saved (`wifi leave`, then Enter on the saved row) | the scan filled by event; rejoined; Host link connected again | pass |
| The same with no Host set (`companion host none`) | not run on the device (T8-9); preview `PREVIEW_HOST=0 settings-sub networks` | PENDING |
| Forget this device on the tailnet | not run (no re-registration agreed); preview: forget once, auth key kept | PENDING |
| `ts` after the gates | `auth key: set` | |
| `idf.py size` (Task 7 fix round → Task 8) | image 7,240,043 B; DIRAM 187,616 B (−736 B vs Task 7's 188,352) | |

**Rulings and deviations:**
- T8-1: the Network panel reads its values when built; a row that changes one rebuilds the panel in place keeping
  its scroll (`ui_settings_refill`, new in ui_settings.c). Only the current-network row follows Wi-Fi events (it
  holds `wifi_mgr_set_cb` while shown). Status does not follow Tailscale events: the Tailscale slot is Peers' (the
  brief's deleted-event case requires it NULL after Back to Network), so Status refreshes on re-entry or any row change.
- T8-2: the current-network row with Wi-Fi down opens Networks (there is nothing to disconnect); connected, it opens
  an action sheet: Disconnect (confirmed, RAM only) and Forget (destructive, confirmed; only when the network is saved).
- T8-3: new `ui_action_sheet(title, items, n, destructive_mask, cb, ud)` (ui_core; `ui_picker` shares its body): the
  rows have no radio dots, and destructive ones read red. Used by the Wi-Fi, Auth key and peer sheets.
- T8-4: Saved rows are a 64 px row (tap: join with the saved password) and a sibling 64×64 trash button 8 px to its
  right (destructive style, `LV_SYMBOL_TRASH`), in a line with `ui_comp_pass_hits` so both keep 80 px of hit. Not a
  drawn chip (T7-2): a drawn chip can't be a second target on a row. Tapping the network it is already on doesn't
  rejoin (that would drop the Host link): it toasts "Connected to …".
- T8-5: the lock glyph is a new 8×8 pictogram at U+F023 in `font_icons16/32` (`gen_ui_icons.py` `EXTRA_CPS`,
  `UI_GLYPH_LOCK`); the face had none. Available hides hidden SSIDs and a second access point of a listed network.
  A header "Scan" chip restarts the scan (the mockup's "Scan").
- T8-6: Auth key sheet: Enter key (masked prompt that starts empty, trimmed, empty refused), Load from SD card
  (`sd_mount()` on demand, first line trimmed; "No SD card" / "No key file"; the card stays mounted afterwards), Clear
  key (destructive, confirmed; shown only while a key is set, so the sheet has 3 actions with a key, 2 without).
- T8-7: Forget keeps the auth key (U5): the old `forget_task` without the key wipe. The confirm adds "You'll need a
  new auth key." only when none is saved. In the preview the work runs inline (no tasks).
- T8-8: C15: `TS_HOST_OFF_MESSAGE` stays the connect refusal, reworded to "Tailscale is off. Turn it on in Settings
  › Network › Tailscale." (§9 rule 5 path); the Terminal banner text is `STR_TS_OFF_BANNER` / `STR_TS_TURN_ON` for
  Task 9. `tailscale_home_derp_code()` (new) gives "DERP sea" (the preview stub) from the DERP map's RegionCode, the number if none.
- T8-9: the no-Host rejoin leg was not run on the device: `companion host none` calls `cc_link_set_server`, which for
  another name clears the pin carry-over (`carry_store(NULL)`, "tailnet pin source"), and `companion host Mac` can't
  bring it back (only a tailnet connect captures it). That breaks the "restore it exactly" rule and would leave the
  Host link refusing a new LAN address with an unknown key. The rejoin ran with the Host set; the no-Host leg is the
  preview's `PREVIEW_HOST=0 settings-sub networks`. PENDING for the visit, with the user's agreement to lose the carry.
- T8-10: `ts` prints "auth key: set|not set; peers: N, M online" (never the key). Console `ui settings network` and
  `ui net networks|advanced|peers` drive the gates.
- T8-11: shared fixes found by the new cases: an INFO row's value has a set 60 % width (a content-width label with a
  max width was clipped to one line), and a toast deleted by anything else (a cleaned layer) clears its pointer and
  timer (it was a dangling pointer).
- T8-12: C5: with ui_wifi.c and ui_tailscale.c gone, `ui_terminal.c` is the only `ui_theme_*` shim user. C11: the
  preview builds `ui_settings_net.c` (the two deleted files were never in it). C14 ends: `settings 1` and
  `keyboard-settings` run every check.

**Task 8 fix round 1.**
- I1 (replaces T8-1's Status clause): the Network panel holds `tailscale_set_event_cb` while its Status row exists
  and gives it back in that row's DELETE event; events coalesce to one `ui_async` and update the switch (state set
  without VALUE_CHANGED), its "on" / "off · saved" label, Status (text and tone) and Peers' "N online" in place, with
  no rebuild and no timer. Peers takes the slot over by B2's delete-then-create order. Device: on Network, `ts on`
  took Status from "connecting" to "connected · <ip> · DERP <n> · 4 peers online", and `ts off` turned the switch and
  Status back, with no rebuild (screenshots, scratch only). The DERP region shows its number until the DERP map's
  code arrives with a later event.
- M1: Saved rows resolve their network by the row's SSID at tap time (the trash button by its line's row).
- M2: the dead `STR_WIFI_*`, `STR_TS_*`, `STR_TAILSCALE` and `STR_POWER_NOTE` entries are deleted (grep: no users).
- T8-13 (M3, controller ruling, a deviation from §5.8 row 8): with no key saved, the forget confirm reads "Removes this
  device's tailnet identity. You'll need a new auth key to register it again." (the spec's appended sentence
  contradicted the "saved auth key" one).
- M4: `ui_settings_column()` (a shared style; Settings' panel and the sub-pages) and `ui_srow_line()` (a row and a
  trailing button) in ui_comp; the switch label's extra margin is gone (the row's 16 px gap spaces it).
- M5: `refill()` in ui_settings.c goes through `ui_settings_refill()`: every panel keeps its scroll on a change
  (a rail click still starts at the top).
- M6: Load from SD runs the mount and the read on a one-shot worker task (the forget's pattern), the result by
  `ui_async`; a first line without `tskey-` ("The key file doesn't hold a Tailscale auth key") or longer than the
  setting ("The key in the file is too long") is refused whole.
- M7: `ui net <sub>` with Settings on top switches its section in place (one Back returns to Settings).
- M8: the `ui_comp_pass_hits` comment no longer claims ui_core and ui_softkbd are its only users.

## Task 9: Terminal host list and host form

**Shape.** `ui_hosts.c` (Terminal, §5.3) and `ui_host_form.c` (§5.5) replace `ui_servers.c` and `ui_server_form.c`.
Terminal: `ui_header("Terminal", "Sessions")`, a 48 px banner under it (Tailscale off and a listed tailnet host: the
§5.3 text and a primary Turn on), then one column of 72 px host rows (dot, name, `user@host[:port]`, a trailing label,
and a 64 px Edit button), "+ Add host", the footnote and the §5.13 key hint. The Host's entry is never listed
(`cc_link_is_companion_server`). No timer: built on show; the dots and the banner follow Tailscale events in place
(one coalesced `ui_async`, a NULL-handle check in the handler, the slot cleared in the screen's DELETE). The pure rules
are `servers/host_rules.c` (host-tested); `hf_tailnet_host` is `ts_host_needs_tailscale` (C16).

**Device gate (Tab5, LAN Host link, Tailscale off unless noted).** Run twice: after the first flash, and again after the
heap move of the form state (fix on top, same session); the second figures are shown.

| Check | Result | |
|---|---|---|
| `key ctrl t`: the Host's entry isn't listed | the store holds 1 entry (the Host; counted in the Host picker); Terminal lists 0 rows, "+ Add host" and the footnote | pass |
| `ui heapmin` over `key ctrl t` (Sessions → Terminal) | internal min **144,211 B** (gate 40,960; first flash 141,095 B); `ui timers`: the 30 s vitals and the 10 ms drain only (no 300 ms tick) | pass |
| Host form on an existing entry (Settings › Host › Address, the only entry), read only, Esc | Edit host, "← Settings", Save; Name, Address, Port 22, User, Sign in with · Device key; no Host key row, no Delete host; Esc left at once (no change). Heapmin over the open **166,307 B** | pass |
| `ts on`: tailnet hosts show dots | no tailnet entry exists to dot (0 listed). Terminal with `ts on`: heapmin **109,139 B** (gate 20,480); "+ Add host" sheet had 2 actions; "From your tailnet" listed 4 peers, the `ts` count of online peers | PENDING (dots), pass (sheet) |
| `ts off`: the banner shows; Turn on | no listed tailnet host, so no banner (correct by §5.3). Console equivalent `ts on` reported "on (Connected …)"; `ts off` reported off and the link came back on the LAN path | PENDING (banner, tap) |
| Edit → Forget host key → reconnect → accept | not run (P4C-8): preview `hostform edit-pin` (forget once, after OK) | PENDING |
| B6 on the device | no entry has an SD key path (the one entry signs in with the device key) | PENDING; `test_key_source_never_falls_back` |
| 60 s SSH session `ui heapmin` | not run (P4C-8) | PENDING |
| `idf.py size` (Task 8 fix round → Task 9) | image 7,240,043 → 7,245,083 B (+5,040); DIRAM 187,616 → 188,928 B (+1,312) | |

Final state: `ts off`, `power set defaults`, DHCP, Host link connected on the LAN, brightness 80 and keyboard light
`0 0 0080ff 1 5` (as found), test sessions stopped. Screenshots in the scratchpad `shots9/` only.

**Rulings and deviations:**
- T9-1: C16 checked: `ts_host_needs_tailscale` gives the brief's values (100.64.0.1 and box.tail1234.ts.net true;
  100.128.0.1 and 192.168.1.5 false). No mismatch.
- T9-2: C12: the old `terminal` spacing case is `term-spacing` (its PNG `term-spacing.png`); `terminal [variant]` is the
  new one. C11: the preview builds ui_hosts.c, ui_host_form.c and host_rules.c; the two deleted files were never in it
  (they were `SCREEN()` stubs, now gone; `ui_terminal_create` is a recording stub).
- T9-3: the Edit control is a 64×64 secondary `ui_btn` 8 px right of the row in a `ui_comp_pass_hits` line (T8-4's
  pattern), not the spec's 48 px chip: a 48 px body control fails P4C-5. It is touch only (`UI_KEYBOARD_SKIP`); the
  keyboard edits with Ctrl+Enter on the row (its LONG_PRESSED, which a long touch also gives). So Down goes row to row
  (`terminal keys`), and the focused row's hint reads "↵ Connect · ⌃↵ Edit" (§5.3 says "↵ Connect"; the second half
  tells the keyboard user how to reach Edit). The long-press menu is gone.
- T9-4: the name is in `body` (FONT_BODY): there is no bold body face, as on the Sessions rows.
- T9-5: "+ Add host" has a 1 px line border, not a dashed one: LVGL draws no dashed borders.
- T9-6: Connecting: a tap checks the refusals first (the Host, no Wi-Fi, a tailnet host with the switch off:
  `ts_host_term_refusal`) and shows the reason in red on the row (two lines at most), with no session screen. Otherwise
  the row shows a spinner and "Connecting…" and the session screen is pushed on the next LVGL cycle; the session
  screen shows the progress (Task 10 rebuilds it). A session that ends in SSH_ST_ERROR calls
  `ui_hosts_note_failure(idx, msg)` (ui_terminal.c), and Terminal shows that reason in red on the entry's row (by
  name, host and port) when it shows again; the next tap clears it.
- T9-7: "+ Add host" opens an action sheet: "From your tailnet" (only with the switch on) and "Enter manually". "From
  your tailnet" opens a second sheet of the online peers (a toast "No tailnet peers online" if none); a peer
  prefills Name with its hostname and Address with its tailnet IP (so `hf_tailnet_host` dots it). Settings › Network ›
  Peers › Add as SSH host pushes Terminal and then the prefilled form, so Back lands on Terminal.
- T9-8: rows, Edit and Delete resolve their entry by name, host and port at tap time (`terminal keys`, `hostform
  edit-pin` removes an earlier entry under the form and Delete still removes the right one). Save on an entry that
  went toasts "This host was removed".
- T9-9: Esc, ← and a swipe all go through `ui_nav_back`, which now asks the screen's back guard first
  (`ui_nav_set_back_guard`, cleared on the screen's DELETE). The form's guard asks "Discard changes?" (Cancel /
  Discard, destructive) while the draft differs from what was opened.
- T9-10: Host key and Delete host are shown only from Terminal: from Settings (the Host's entry, or a new Host from
  the picker) the pin is Settings › Host's own row, which forgets through the link (`ui_sessions_forget_host_key`,
  pin carry-over aware), and Delete would remove the Host. Saving from Settings › Host › Address or the picker calls
  `cc_link_set_server(name)` (P4-6); with the name unchanged that keeps the pin carry-over and only reconfigures the
  link; a rename counts as another Host and clears it (L3, the conservative side).
- T9-11: Sign in with: Device key or Password; an entry whose key path isn't empty or "device" also offers the
  read-only "Key file on SD card" (its path is never shown or edited) until another is saved. Password starts empty,
  masked; an empty answer keeps the saved one; the row shows "•••••••• set" or "Not set". Choosing Device key keeps
  any saved password (no silent wipe). Name shows the `user@host` it will save as while empty (C19).
- T9-12: `hf_validate` order: address, port, user, then the Host check; "MAC" from Terminal is refused with "That's
  your Claude host: its sessions are on the Sessions screen." `TERM_COMPANION_REFUSED` is now
  `tr(STR_HOSTS_NOT_LISTED)`, the footnote's first sentence.
- T9-13: B6 in `ssh_client.c`: the ENOENT → device-key fallback is gone. `ssh_task`, before any socket, mounts the card
  for a `/sdcard/` key path and fails with "Key file not found on SD card" (`HF_KEY_FILE_MISSING_MSG`) when the file
  is missing; `authenticate` uses `hf_key_source` for the same rule (the race after the check). The device key is
  used only for an empty or "device" path.
- T9-14 (Task 8 carry): `ui_async` returns bool (false: no memory, the lock, or `lv_async_call` failed; the wrapper is
  freed). The SD-key worker frees its job and clears its flag when the post fails; the forget worker clears its
  flag. Both flags are volatile. The dead `if (!s_sdk_running)` in `key_sd_done_async` is gone.
- T9-15: the form state and the add-peer list are malloc'd while their screen shows (PSRAM-sized), not .bss: the first
  build had DIRAM +3.8 KB; now +1.3 KB.
- T9-16: C10 left as ruled: Ctrl+] in an SSH session still leaves without a confirm until Task 10.
- T9-17: the old server strings with no users left (`STR_SERVERS`, `ADD_SERVER`, `EDIT_SERVER`, `DEL_SERVER(_Q)`,
  `NO_SERVERS`, `NAME`, `HOST`, `PORT`, `USERNAME`, `PASSWORD`, `AUTH_KEY`, `KEY_PATH`, `HOST_USER_REQ`, `SAVED`,
  `SERVER_SAVE_FAILED`, `ERROR`, `WIFI_NOT_CONNECTED`, `NET_ADD_FROM_TERMINAL`) are deleted.
- T9-18 (device finding, not this task's code): in an action sheet the overlay is the first keyboard control, so a
  first Down lands on the overlay and Enter there closes the sheet; a second Down reaches the first item. ui_core's
  modal walk; left for Task 12's keyboard pass.
- Incident: during the gate a Shift+Tab then Enter on Terminal took the back chip to Sessions, and two Downs then
  moved the Sessions selection onto a real card (P4C-4). It was moved back to the test session with Up only, within a
  minute; nothing was opened, sent or expanded (the detail pane loaded that card's recent output, a read).

**Task 9 fix round 1.**
- C1 (hard rule 1): `cc_link_host_addresses` (cc_link.c) collects every address the link knows the Host by. That is
  the entry's host and its MagicDNS IP, the LAN fallback, `mac_ts_ip`, the carry IP, the verified and pinned
  addresses, the last LAN address and the host cache. It is empty with no Host set. `cc_link_is_companion_server`
  matches an entry's host (and its MagicDNS IP) against all of them through the pure `hf_is_host`
  (`cc_same_host`, host-tested), and it fails closed when it can't allocate. As a result:
  - Terminal can't list it.
  - `row_cb` and `term_create` refuse it with the footnote's sentence.
  - The form's Save uses `hf_validate_addrs` (HF_IS_HOST for the fallback or the tailnet IP typed by hand).
  - "From your tailnet" drops any peer whose IP or name matches.

  Preview: `terminal add` (the Host's tailnet peer is not offered) and `hostform new` (the fallback and the tailnet
  address are refused). Invented addresses only. The preview's stubs now say `studio-host` / 192.168.77.20 instead of
  the user's real name and fallback.
- I1: Save refuses a name another entry already has (case-insensitive, excluding the edited entry): HF_NAME_TAKEN,
  "Another host already has that name". It applies from Terminal and from the Host picker (`hf_name_taken`,
  host-tested).
- M1: new `cc_link_rename_server(old, new)`. It follows a rename of the Host's entry without clearing the pin
  carry-over. Settings' Save calls it before the same-name `cc_link_set_server` (which keeps the carry). This
  replaces T9-10's last clause.
- M3: the host row's column, hint and failure note, and the fingerprint row, now use shared static styles.
  `ui_srow_fingerprint` serves Settings › Host and the form.
- M4: the prompt placeholders and the Name row's user/host fallback are `STR_*`.
- M5: Save failure, list full (checked before an add) and delete failure each have their own string.
- M6: the connect post and Terminal's Tailscale post check their results. A failed connect post shows "Couldn't
  start the connection. Try again." on the row; a failed event post clears the queued flag.
- M7: the back guard covers every way off the screen (`ui_nav_back`, `ui_nav_home`, `ui_nav_push`,
  `ui_nav_replace`). A blocked move waits as pending: Discard runs it (`ui_nav_proceed`) and Cancel drops it. This
  replaces T9-9's guard clause. Preview `hostform new`: `ui_nav_home` asks first.
- M8: `hf_arg_t` keeps the identity the form opened (opened_*). A re-create (`ui refresh`) finds that entry by
  identity, or toasts "This host was removed" and falls back a level. Preview `hostform edit-pin` inserts an entry
  before it and refreshes.
- M9: a key path outside `/sdcard/`, or with a `..` part, is HF_KEY_BAD_PATH: "The key file must be on the SD card",
  before connecting and in `authenticate`.
- M10: a key entry never tries a password or keyboard-interactive answer, so an SD key's passphrase is never sent as
  a password. A refused key fails with "The host refused the key", and `ssh_task` shows the client's own reason.
  Saved passwords on device-key entries are no longer sent either.
- M11: `authenticate` decides through the pure `hf_auth_plan(use_key, keypath, exists)`, which is host-tested
  (`test_auth_plan`). It is a seam, not an integration test: there is no libssh on the host. The device-side wiring
  stays a visit item.
- M2 is deferred to Task 12.
- `idf.py size`: image 7,247,675 B, DIRAM 189,728 B (+800 B against the first Task 9 build).
- Flashed. Nothing was saved on the device and no navigation keys were sent (P4C-9). The final state is unchanged.

**Task 9 fix round 2 (P4C-10: the Host is known by its SSH host key).**
- **Primary (C1, I-new-1): key deny-list.**
  - `ssh_task` checks the server's key on an interactive session (no `cmd`): after the key exchange, before the
    known-hosts prompt and any authentication. It refuses any key the companion link ever pinned, with the footnote's
    sentence (`STR_HOSTS_NOT_LISTED`); the done path disconnects. It fails closed when the list or the server key
    can't be read.
  - The list is NVS "cc"/"hostkeys": n x SHA256, a FIFO of 8 (`hf_hostkeys_add` / `hf_hostkey_is_host`,
    host-tested). `cc_link` appends the key it authenticated on at every connect, which covers pins, re-pins and
    carried keys. Nothing removes from it, so it survives Settings › Host › Forget and Host switches.
  - `host_keys_migrate()` at `cc_link_start` adds the pins found for the Host's entry host, `mac_lan_ip`,
    `mac_ts_ip`, the carry IP and the host-cache IP, plus the carry key. The list is never empty while a pin exists.
  - Logs carry a count only ("host keys: N known for the Host"), never a key.
  - Device: after the flash the boot log read "host keys: 1 known for the Host" and the link connected on the LAN.
  - Key type: neither `cc_link` nor `ssh_client` sets `SSH_OPTIONS_HOSTKEYS`. Both negotiate with libssh's default
    order against the same sshd, so the Host presents the same key type to both, and the deny-list holds that type.
    Gap: an sshd whose key set changes, so that a new type is preferred, is refused only after the link has met the
    new key once. That is the same connect, since the link reconnects first after a boot.
- **Convenience (list and save).**
  - `cc_link_host_addresses` adds the Host entry host's first label (`hf_host_addrs_add_name`: "studio" from
    "studio.local", host-tested) and `tailscale_resolve(<label>)`.
  - The tailnet sheet and Settings › Network › Peers › Add as SSH host drop a peer whose IP or MagicDNS name matches.
    For example, `studio.<tailnet>.ts.net` matches by the short-name rule. Peers toasts the footnote sentence.
  - Preview `terminal add-fqdn` (`PREVIEW_HOST_TSIP=0`) covers a peer named studio-host.tail1234.ts.net with no
    tailnet IP known. The peer is not offered, Peers doesn't add it, and the FQDN typed by hand is refused at Save.
  - Remaining gap: a Tailscale machine name unlike the Host's `.local` name. The key check closes it at connect.
- **Optional `getpeername` check: not done.** The key check covers every address, alias, spelling and IPv6 address.
  An address check would add nothing, and it can't know an IPv6 or unresolved LAN address anyway.
- **M-new-1:** `authenticate` clears `c->err` first, so a refused password shows "Authentication failed: …".
- **M-new-2:** a way out while "Discard changes?" is up stacks no second dialog, and the first move stays pending
  (`guarded` keeps it). Preview `hostform new` covers it.
- **Preview:**
  - The terminal stub applies `hf_hostkey_is_host` as `ssh_task` does. In `terminal fail`, "alias-box" (10.20.30.40,
    an address the link doesn't know) is listed, connects, meets the Host's key, and its row shows the refusal.
  - The stub Host entry is now studio-host.local.
- **Parked, as instructed:** real names in older test files, the repair of duplicates saved before the fix, two forms
  sharing an arg, and the boot-millisecond cache window.
- **Size:** image 7,249,499 B, DIRAM 189,824 B.
- **Boot:** the P4 booted unaided after `idf.py flash` (its RTS hard reset); no esptool reset was needed this time.

**Task 9 fix round 3.**
- I-new-2: the deny-list read has three results, NONE / FOUND / ERROR (`hk_read`, raw NVS: a missing namespace or key
  is NONE, any other open or read failure and a blob that isn't a multiple of 32 bytes or longer than 8 keys is
  ERROR; no allocation). The pure `hf_shell_gate` decides: an interactive session is refused with "Couldn't check
  this host against the Host's keys" on ERROR or an unreadable server key, refused with the footnote sentence on a
  Host key, and allowed on NONE (no list yet) or FOUND without a match; a take-over (cmd) is never gated.
  `ssh_host_keys_add` writes only after a NONE or FOUND read (`hf_hostkeys_may_write`), so an unreadable list is never
  overwritten. This replaces round 2's "It fails closed when the list or the server key can't be read", which only
  held for the server key and the buffer.
- M-new-3 (correcting round 2's key-type note): a Host whose key changed (regenerated, or a newly preferred type met on
  a pinned address) makes the link refuse with KH_CHANGED and append nothing; the new key joins the deny-list only
  after Settings › Host › Forget and the link's re-pin. Until then the Terminal's key check doesn't know it (only the
  address checks apply). The same holds for a second sshd on the Host (another port, a container, Tailscale SSH),
  and for the time before the link's first authenticated connect (an empty list).
- Key-type hardening: the link records the type of the key it authenticated on (NVS "cc"/"hostktype",
  `ssh_host_key_type_note`); an interactive Terminal session sets `SSH_OPTIONS_HOSTKEYS` to that type's algorithms
  first and then libssh 0.11's DEFAULT_HOSTKEYS (`hf_hostkeys_pref`, host-tested: RSA is rsa-sha2-512,rsa-sha2-256;
  ed25519 is the default order unchanged). So a Host that adds a newer preferred type still presents its deny-listed
  key. Gaps: a regenerated key and a second sshd (above). Side effect to watch on the visit: when the Host's type is
  not ed25519, a Terminal host with several key types pinned under the default order may present another type once
  and ask about a changed key.
- M-new-4: the deny-list mutex is made once by `ssh_host_keys_init()` at `cc_link_start` (before the migration and
  the link task); before it, a read is ERROR and an add is skipped. No FreeRTOS call in a critical section.
- M-new-5: first labels are kept apart from addresses (`hf_host_addrs_t.label`) and match only a `*.ts.net` name with
  that first label or the bare name (one trailing dot allowed): with a Host at web.local, web.example.com and web.lan
  are not refused; web.tail1234.ts.net and "web" are. `hf_first_label` is the one helper (cc_link uses it too).
- Order seam: `ssh_task` runs the steps `hf_conn_steps(interactive)` gives (DENY_CHECK, VERIFY_HOST, AUTH for an
  interactive session; VERIFY_HOST, AUTH for a take-over) in a loop, so the host test pins "deny check before the
  known-hosts check and before any authentication". What stays untestable on the host: that libssh's `ssh_connect`
  sends no userauth request (read in client.c; the device visit covers the live refusal).
- Boot log: `cc_link: host keys: N known for the Host` after the migration (a count). Device after the flash: "host
  keys: 1 known for the Host", link connected on the LAN, booted unaided after `idf.py flash`; final state as found.
- `idf.py size`: image 7,251,867 B; DIRAM 189,824 B.
