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
