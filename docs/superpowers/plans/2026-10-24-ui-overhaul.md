# UI Overhaul Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the Tab5 firmware into a six-screen, single-purpose Claude Companion: Sessions home, Take-over, Terminal, SSH session, Host form and a six-section Settings. It has one fixed token set where amber means NEEDS YOU only, and every removed feature's code, tasks, timers and internal RAM are gone.

**Architecture:**
- Removals come first, so RAM and surface shrink before anything new is built. Then one pure token header (`ui_tokens`) and one LVGL component module (`ui_comp`) with shared static styles, then each screen rebuilt on them.
- Screen logic that can be pure moves into host-tested modules: the session model, the banner precedence, the tail wrap, hotkeys, keyboard colour, reset scope and host-form rules.
- Navigation is instant: the old screen is deleted before the new one is created (B2).
- The CFG 23 migration, the copy pass and the final device measurement close the plan.

**Tech Stack:** ESP-IDF v5.5.6, LVGL 9.4, libssh 0.11, MicroLink, Unity host tests (cmake/ctest), the host UI preview (`firmware/tools/ui_preview`), Python unittest.

**Spec:** docs/superpowers/specs/2026-10-24-ui-overhaul-design.md. §16 (B1–B6) overrides anything earlier in the spec. The mockups are in docs/superpowers/specs/mockups/ (B4: the spec text wins).

## Global Constraints

**Hard project rules (verbatim from spec §3):**
1. **Never open a login shell on the Host, and never touch the user's tmux.**
   - Sessions open through a pty exec of an explicit `claude attach <id>`.
   - Terminal never lists the Host and refuses to connect to it.
2. **Every command line sent to the Host comes from a `cc_cmd_*` function.** That includes answers, opens and log fetches. No UI code formats a command line.
3. **No secrets in the repo or in logs.** The secrets are:
   - the Tailscale auth key;
   - Wi-Fi passwords;
   - host passwords;
   - the device's private key;
   - typed answer text (the existing masking in the key log stays).

   Secrets are typed masked and shown masked.
4. **No GPL code.** libssh (LGPL-2.1) stays.
5. **Keep `firmware/LICENSE.clientre5`** and the line "Based on Clientre 5 by Mozgi512 (MIT)". About shows that line.
6. **Never flash the C6, erase NVS, or run `sshkey regenerate`.**
7. **A wake input never reaches the Host and never acts on a card.** The first key or tap that wakes the screen from dim or off is swallowed on every screen, terminals included.
8. **Real-session screenshots stay out of git.** Device tests use only `companion-test-*` sessions. Their screenshots go to scratch storage, not the repo.
9. **Amber `#f4b545` means NEEDS YOU only.**

**Added for this plan:**
- Test only against Host sessions you create, named `companion-test-*`. Start one with `~/.local/bin/claude --bg --name companion-test-<x> "Ask me what my favorite color is, then wait for my answer."`. Stop them when the task is done (`~/.local/bin/claude stop <id>`).
- Never change sshd config or `~/.ssh/authorized_keys`.
- Real-session screenshots stay out of git.
- Leave the Tailscale switch as you found it (off by default). A task that runs `ts on` for a gate runs `ts off` before it ends.
- Don't erase the orphaned NVS namespace `music`/`fav` (§6 #101).
- The new screens add no UI dependency to the check-in path (`sys/checkin.c`); a check-in boot builds nothing the old one didn't (§11).

**Project facts:**
- **Base:** this plan is built on main after Plan 3 (`plan3-local-link`, head 55ebebf) merges. Before Task 1, merge main into `plan4-ui-overhaul` in the worktree `/Users/you/workspace/claude-companion-plan4`, and check that `firmware/main/companion/cc_link.h` declares `cc_link_mac_path`. All work happens in that worktree; the top-level checkout stays on main.
- **Settings blob:** `app_settings_t` is one NVS blob. No field is removed or reordered; dropped fields become reserved, with a comment naming Plan 4. **This plan takes CFG 23**, and CFG 22 stays reserved for Plan 2's Task 10.
- **Internal RAM:** LVGL allocations of 512 B or less land in internal RAM. Use the shared static `lv_style_t` styles from `ui_comp` instead of local `lv_obj_set_style_*` calls. Build only the visible Settings section, and update Sessions in place (never rebuild it on a poll).
- **Timers (B1):** no presentation timer runs faster than 300 ms, and the 300 ms one runs only while Sessions is shown. Exempt and unchanged: the key drain (`keyboard.c`, 10 ms) and the pty pump (`ui_terminal.c`, 16 ms, live sessions only). The vitals use one shared 30 s timer.
- **Screen changes (B2):** `lv_screen_load` with the old screen deleted first. No fade or slide.
- **Strings:** every user-visible string goes through `tr(STR_*)` in `firmware/main/i18n/i18n.h` (spec §9.9). Say "Host", never "Mac" (U11). Use sentence case, and put units in values, not labels.
- **Plan 3 surfaces are used as they are:** `tailscale_set_enabled()`, `cc_link_set_lan_fallback()` / `cc_link_lan_fallback()`, `cc_link_mac_path()`, `wifi_mgr_static_ip_get/set()` and `companion hostcache`.

**Commands** (`PV` and `SHOTS` are directories in your scratchpad):
- **Firmware:** `. ~/esp/esp-idf/export.sh && cd firmware && idf.py build`. After editing `sdkconfig.defaults`, delete `firmware/sdkconfig` before building. Size: `idf.py size > $SHOTS/size-<task>.txt`.
- **Flash (P4 only):** `idf.py -p /dev/cu.usbmodem211201 flash`; if that port is gone, use `ls /dev/cu.usbmodem*`. It keeps NVS.
- **Host tests (HOST):** `export IDF_PATH=~/esp/esp-idf && cmake -S firmware/test/host -B firmware/test/host/build && cmake --build firmware/test/host/build && ctest --test-dir firmware/test/host/build --output-on-failure`.
- **Python tests (PY):** `python3 -m unittest discover -s firmware/tools -p 'test_*.py' -v`.
- **Preview (PV):** `cmake -S firmware/tools/ui_preview -B $PV -G Ninja && cmake --build $PV -j 8`, then `cd $PV && ./preview <case> [variant]`. From Task 4 on, every case runs the automated checks and exits nonzero on a failure. `firmware/tools/ui_preview/run_all.sh $PV` runs every case and writes PNGs to `$PV/png`.
- **Console:** `T5=/tmp/tab5.py`, run as `python3 $T5 "cmd" …`. Screenshots: `python3 firmware/tools/screenshot.py --out $SHOTS/<name>.jpg`. Look at every screenshot you take.
- **RAM gate (B2):** `ui heapmin start`, then the action, then `ui heapmin stop`, which prints `heapmin: internal min <N> B, largest <L> B, now <F> B` (Task 1 adds this). The B2 thresholds:
  - Tailscale off: Settings › Network ≥ 66,560 B (65 KB), and a take-over ≥ 40,960 B (40 KB);
  - Tailscale on: a 10-minute take-over ≥ 20,480 B (20 KB) with no `ERR_BUF` and no link drop, and Settings › Network ≥ 20,480 B.
- **Notes:** each task appends its measurements and rulings to `docs/superpowers/notes/2026-10-24-ui-overhaul-notes.md` (Task 1 creates it).

## Review Focus

1. **A poll re-sorts, adds or removes sessions while you type an answer or a send is in flight.** Expected: the selection, the draft and the send stay with the session they were typed for, never the row now at that position. Tests: Task 5 `test_selection_follows_key_across_resort` and `test_draft_follows_key_across_resort`; Task 6 preview `sessions resort`.
2. **The waking tap lands on a footer chip, a row, the mic slot or the session bar's Leave.** Expected: it only wakes; nothing is selected, opened, sent or left. Tests: the Task 6 and Task 10 device steps with `power go off` then `power sim touch`.
3. **Long or unbroken text:** a 95-character session name, a 63-character repo, a 300-character log line with no spaces, or CJK text. Expected: the detail pane wraps it, only list rows show "…", nothing overflows its parent, and confirm titles wrap. Tests: Task 5 `test_wrap_unbroken_line`; Task 6 preview `sessions long` with the fit check.
4. **Bad or empty input in a host form or prompt:** port `0`, `70000` or `abc`; an empty address or user; an address that names the Host; an empty device name; a Claude path with a space. Expected: each is refused with its copy and nothing is saved. Tests: Task 9 `test_hf_validate_*`; the Task 8 preview `settings 1` prompt checks.
5. **A Tailscale event or the switch changing after the Terminal or Peers screen is gone.** Expected: there is no callback into a deleted screen, and the next visit shows the current state. Tests: Task 9 preview `terminal deleted-event`; Task 8 preview `settings-sub peers deleted-event`.

## Plan rulings (gaps the spec leaves open)

- **P4-1, how a RAM gate is measured.** B2 says "the minimum internal free over the action". The plan measures it with ESP-IDF's `heap_caps_monitor_local_minimum_free_size_start/stop` through a new `ui heapmin start|stop` verb.
- **P4-2, deleting the old screen first.** `ui_core` keeps one empty, persistent blank screen. A screen change loads the blank, deletes the old screen, creates the new one and loads it.
- **P4-3, the forget-key fold.** `run_forget` posts its own success toast through `ui_async` when the forget finishes in time. The Sessions tick toasts only a forget that finished later (`cc_link_forget_done` grew). This keeps the 500 ms always-on timer gone, and a forget started from Settings still toasts while Settings is shown.
- **P4-4, banner states the spec doesn't list.**
  - "Waking" (a Hibernate boot with no list yet) and a Radio-off wake both show Reconnecting.
  - A failing poll, or a list too large to load, shows a red banner in the Host-unreachable slot, with [Host settings].
  - A link that is down for less than the 30 s grace shows Loading when there is no list yet, and Reconnecting when there is one.
- **P4-5, a blocked background session with no live process** shows the pill "ENDED", muted. It can't be answered or opened, and §9.4 allows only the five state words. A blocked interactive session shows "waiting on Host" (B3).
- **P4-6, Settings › Host › "Add host…".** The entry it saves becomes the Host.
- **P4-7, SD mounts on demand and stays mounted** until reboot. The card is read rarely, and staying mounted avoids remount churn under libssh.
- **P4-8, drafts.** The draft store keeps 16 entries. A 17th evicts the least recently edited one.
- **P4-9, bridges during the build.** Task 3 adds a temporary Settings › Network row, "Wi-Fi networks", that opens the old Wi-Fi screen, so the device stays recoverable. Task 8 removes it. Ctrl+T and the footer open `ui_hosts_create`, which is the old server list until Task 9.
- **P4-10, the terminal palette.** The terminal reads the token constants directly. The CFG 23 migration still writes the same values into the `term_*` slots (§10 step 1), but nothing reads those slots.
- **P4-11, the Ctrl+K overlay.** It is built from a two-column table, `hk_overlay[]` (`keys`, `what`), so the two columns never wrap.

---

### Task 1: Measurement verbs and baseline; remove Music, Camera and USB MSC

**Files:**
- Modify: `firmware/main/input/key_console.c`: add `ui heapmin start|stop`, `ui timers` and `key ctrl space`.
- Delete: `firmware/main/ui/ui_music.c`, `firmware/main/media/audio_player.c`, `audio_player.h`, `firmware/main/ui/ui_camera.c`, `firmware/main/media/camera.c`, `camera.h`, `firmware/main/ui/ui_usb.c`.
- Modify:
  - `firmware/main/app_main.c`: drop `audio_player_init()`.
  - `firmware/main/ui/ui_core.c`: drop the `ui_music_bar_update()` call in `nav_show_top`, and the mini-player walk of `lv_layer_top()` in `ui_keyboard_route`.
  - `firmware/main/ui/ui.h`: drop `ui_music_create`, `ui_camera_create`, `ui_usb_create` and `ui_music_bar_update`.
  - `firmware/main/ui/ui_home.c`: drop the Music, Camera and USB tiles.
- Modify: `firmware/main/sys/usb_mgr.c`, `usb_mgr.h`: keep the USB host and HID only. Remove MSC, disk mode, format and eject, so `usb_mgr.h` keeps just `usb_mgr_init()`.
- Modify:
  - `firmware/main/CMakeLists.txt`: drop `esp_video` from PRIV_REQUIRES, and `usb_host_msc`, `esp_audio_codec` and `esp_tinyusb` from REQUIRES;
  - `firmware/main/idf_component.yml`: drop `usb_host_msc`, `esp_audio_codec` and `esp_tinyusb`;
  - `firmware/sdkconfig.defaults`: drop the SC202CS camera block (lines 106–110) and the `CONFIG_TINYUSB_*` lines.
  - Keep the BSP `esp_codec_dev` (the mic needs it), and keep the hibernate `camera-rst` rail entry.
- Modify: `firmware/tools/ui_preview/CMakeLists.txt`, `preview.c`: drop `ui_camera.c` and the camera, audio and music stubs, and the `camera` case.
- Create: `docs/superpowers/notes/2026-10-24-ui-overhaul-notes.md`.

**Interfaces:**
- Produces, as console verbs used by every later gate:
  - `ui heapmin start` calls `heap_caps_monitor_local_minimum_free_size_start()`. `ui heapmin stop` prints `heapmin: internal min <N> B, largest <L> B, now <F> B`, where N is `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)` read before `heap_caps_monitor_local_minimum_free_size_stop()`. It takes the LVGL lock for nothing.
  - `ui timers` takes `ui_lock()`, walks `lv_timer_get_next`, and prints one line per timer, `timer <period_ms> ms, last run <age_ms> ms ago, paused=<0|1>`, then `timers: <n>, under 300 ms: <k>`.
  - `key ctrl space` posts `KEY_EV_CHAR cp=' '` with `MOD_CTRL`.

- [ ] **Step 1: Add the three console verbs.** Build and flash.
- [ ] **Step 2: Record the baseline on the unchanged firmware.** Tailscale is off. Run each item and write the numbers to the notes under "Baseline (Plan 3 build)":
  - `idf.py size` (flash, and DIRAM/`.dram0`/`.dram1`/`.iram0` used);
  - `ts` (internal free, min and largest) after 2 minutes idle on the dashboard, with 3 `companion-test-*` sessions;
  - `ui heapmin` over opening Settings › Network (`ui settings`, then tap Network);
  - `ui heapmin` over a take-over of a `companion-test-*` session (`companion takeover <id>`, 60 s, `key ctrl ]`);
  - `ui timers` on the dashboard;
  - idle power on USB: `power log on`, 5 minutes on the dashboard with the screen on and no input, then `python3 firmware/tools/power_report.py --port /dev/cu.usbmodem211201 --log --include-charging`;
  - the last check-in's awake time from the `checkin:` log line.
- [ ] **Step 3: Remove Music, Camera and USB MSC** per the Files list. Expected: `grep -rn "audio_player\|ui_music\|camera_\|ui_camera\|usb_mgr_msc\|disk_mode\|tinyusb\|esp_video" firmware/main firmware/tools/ui_preview` finds nothing outside comments.
- [ ] **Step 4: Run the builds and tests.** Commands: FW build; HOST; PV build, then `./preview settings 0 1280 720 1` and `./preview dashboard` (the old case names still apply here). Expected: they all pass, and `managed_components` no longer holds `espressif__esp_audio_codec`, `espressif__usb_host_msc` or `espressif__esp_tinyusb`.
- [ ] **Step 5: Device gate.** Flash and boot to live on USB. A USB keyboard still types: run `key log on` and press a key on a USB keyboard if one is attached; otherwise mark it PENDING for the user's visit.
  - `ts`: internal free at steady state ≥ the baseline. Record the delta against §7.2's ~14.2 KB share for these three removals (Music + codec 1.0, camera 0.5 (+4.4), MSC 5.6 + task 4.4).
  - Record whether `esp_video`, ISP and CAM unlinked (from `idf.py size-components`).
- [ ] **Step 6: Commit.**
```bash
git add -A firmware docs/superpowers/notes/2026-10-24-ui-overhaul-notes.md
git commit -m "feat(ui): ui heapmin/timers verbs and baseline; remove Music, Camera and USB MSC (Plan 4 Task 1)"
```

### Task 2: Remove Files, the viewers, the SD screenshot and Backup

**Files:**
- Delete: `firmware/main/ui/ui_files.c`, `ui_image.c`, `ui_text.c`, `ui_backup.c`, `firmware/main/media/image_loader.*`, `media/exif.*`, `media/tjpgd/` and `firmware/main/storage/backup.*`.
- Modify:
  - `firmware/main/sys/screenshot.c`, `screenshot.h`: remove `screenshot_init`, `screenshot_take`, `write_bmp`, `poll_cb` and the 60 ms timer. Keep `screenshot_dump_usb` and the console `screenshot` command;
  - `firmware/main/app_main.c`: drop the `screenshot_init()` call.
- Modify: `firmware/main/net/wifi_mgr.c`, `wifi_mgr.h`: remove `wifi_mgr_export_json` and `wifi_mgr_import_json`. Modify `firmware/main/servers/server_store.c`, `.h`: remove `server_store_export_json` and `server_store_import_json`.
- Modify: `firmware/main/ui/ui_home.c` (drop the Files and Backup tiles), `ui_settings.c` (drop the Backup category and its row; the rail now has 8 entries) and `ui.h`.
- Modify: `firmware/sdkconfig.defaults`: `CONFIG_LV_USE_LODEPNG`, `_BMP`, `_TJPGD`, `_GIF` and `_FS_STDIO` off, and `CONFIG_LV_CACHE_DEF_SIZE=0`.
- Modify: `firmware/tools/ui_preview/preview.c`: drop the image, fs and backup stubs, and update the settings click loop to 8 categories.
- Modify: the `STR_*` table in `firmware/main/i18n/i18n.h`: delete every entry that only the removed files used.

- [ ] **Step 1: Remove the code** per the Files list. Expected: `grep -rn "ui_files\|image_loader\|tjpgd\|exif\|backup_\|screenshot_take\|screenshot_init\|export_json\|import_json" firmware/main` finds nothing.
- [ ] **Step 2: Run the builds and tests.** Commands: FW build; HOST; PV `./preview settings 0 1280 720 1`. Expected: they all pass.
- [ ] **Step 3: Device gate.**
  - `ui timers` on the dashboard: no timer with a 60 ms period, and the count is one lower than the Task 1 baseline.
  - `screenshot` still prints `@@SHOT jpeg`.
  - `ts`: internal free ≥ Task 1's value. Record the delta (Files 5.1, viewers 0.6).
  - `idf.py size`: record the flash delta against §7.1.
- [ ] **Step 4: Commit.** Message: `feat(ui): remove Files, viewers, LVGL decoders, SD screenshot poll and Backup (Plan 4 Task 2)`.

### Task 3: Remove the More launcher, the fork status bar, rotation, and the Device info, OTA and USB screens

**Files:**
- Delete: `firmware/main/ui/ui_home.c`, `ui_devinfo.c` and `ui_ota.c`. Keep `firmware/main/ota/ota.c` and `storage/sd.c` for Plan 7.
- Modify: `firmware/main/ui/ui_core.c`, `ui.h`: remove `ui_statusbar_attach`, `statusbar_tick`, `statusbar_icon`, `ui_status_text` and the 400 ms timer. Modify `firmware/main/settings/settings.h`: `STATUS_*` is no longer used, and `status_items` gets a "reserved (Plan 4)" comment.
- Modify:
  - `firmware/main/ui/ui_servers.c`: rename `ui_servers_create` to `ui_hosts_create` (the P4-9 bridge). Its header uses `ui_topbar` without the status bar;
  - `firmware/main/ui/ui_terminal.c`: `term_create`'s fallback goes to `ui_hosts_create`.
- Modify: `firmware/main/ui/ui_dashboard.c`, `firmware/main/ui/hotkeys.c`, `hotkeys.h`:
  - the footer's "More" becomes "Terminal", which pushes `ui_hosts_create`;
  - `HK_D_MORE` is renamed `HK_D_TERMINAL` (Ctrl+M) and pushes `ui_hosts_create`.
- Modify: `firmware/main/ui/ui_settings.c`:
  - drop the Status bar category and the rotation row;
  - Display keeps Brightness and Appearance for now;
  - add the P4-9 bridge row "Wi-Fi networks" under Network, which opens `ui_wifi_create`;
  - remove the OTA URL row (it becomes a console verb in Task 7).
- Modify: `firmware/main/app_main.c` (`display_apply_settings`): always `LV_DISPLAY_ROTATION_90`, so it never reads `g_settings.rotation`.
- Modify: the `STR_HOME_*`, `STR_SB_*` and `STR_STATUS_*` entries in `firmware/main/i18n/i18n.h`: delete them.
- Modify: `firmware/tools/ui_preview/preview.c`: drop the `home` case, the tile-fit check, the portrait sizes in the README and the `SCREEN(...)` stubs for deleted screens.
- Test: `firmware/test/host/test_hotkeys.c`.

- [ ] **Step 1: Update the hotkey test.** In `test_dashboard_keys`, Ctrl+M → `HK_D_TERMINAL`. Run HOST and expect a compile failure on `HK_D_TERMINAL`.
- [ ] **Step 2: Remove the code** per the Files list. Expected: `grep -rn "ui_home_create\|ui_statusbar_attach\|ui_status_text\|ui_devinfo\|ui_ota_create\|HK_D_MORE\|STR_HOME_" firmware/main firmware/tools` finds nothing.
- [ ] **Step 3: Run the builds and tests.** Commands: HOST (`test_hotkeys` passes); FW build; PV `./preview dashboard-keys`, with its last block updated so Ctrl+M asserts `ui_nav_is_top(ui_hosts_create)`; PV `./preview settings 0 1280 720 1`.
- [ ] **Step 4: Device gate.**
  - Boot to live.
  - `key ctrl m` opens the server list; `key esc` goes back.
  - `ui settings`, then Network › Wi-Fi networks opens the Wi-Fi screen.
  - `ui timers` on the dashboard: no 400 ms timer.
  - `ts`: internal free ≥ Task 2's value. Record it.
- [ ] **Step 5: Commit.** Message: `feat(ui): remove the More launcher, fork status bar, rotation, Device info/OTA/USB screens; Terminal footer (Plan 4 Task 3)`.

### Task 4: Tokens, components, instant navigation and the preview checks

**Files:**
- Create: `firmware/main/ui/ui_tokens.h`, `ui_tokens.c`. These are pure C with no LVGL.
- Create: `firmware/main/ui/ui_comp.h`, `ui_comp.c`: the shared static styles and components.
- Delete: `firmware/main/ui/ui_theme.c`, `ui_theme.h`, `ui_appearance.c` (it holds `ui_ansi_create` too), `ui_motion.c` and `ui_motion.h`.
- Modify: `firmware/main/ui/ui.h`:
  - the `UI_*` colour macros become compatibility aliases onto the tokens, removed in Task 12: `UI_BG`→bg, `UI_CARD`→panel, `UI_CARD_HI`→panel-2, `UI_ACCENT`→fg, `UI_ACCENT2`→muted, `UI_DANGER`→red, `UI_TEXT`→fg, `UI_MUTED`→muted;
  - drop `ui_theme_reinit_lvgl`, `ui_appearance_create` and `ui_ansi_create`.
- Modify `firmware/main/ui/ui_core.c`:
  - `nav_show_top` follows P4-2: load the blank, `lv_obj_delete` the old screen, create the new one, `lv_screen_load`;
  - `ui_init` calls `ui_comp_init()` and builds the LVGL default theme with fg/bg;
  - the focus ring in `keyboard_focus_set` is 2 px `fg` with a 2 px gap, never a theme accent;
  - `ui_button` defaults to secondary;
  - `ui_toast` is a single line at the bottom centre, `panel`/`fg`, 3 s by default;
  - `ui_confirm` uses `ui_confirm_ex`;
  - `ui_prompt` masks when `password`;
  - `ui_menu` becomes a wrapper for `ui_picker(…, current = -1, …)`;
  - `ui_prompt` and the form fields attach the soft keyboard only when `!keyboard_any_present()`;
  - `ui_busy` is a spinner and a muted label, `ui_sleep_notice` is restyled to the tokens, and `firmware/main/ui/ui_softkbd.c` uses the token styles.
- Modify:
  - `firmware/main/ui/ui_boot.c`: `BOOT_BG` → bg, the logo and "Claude Companion" in fg, "Starting…" muted, no accent;
  - `firmware/main/fonts/fonts.c`, `fonts.h`: drop `fonts_select` and `fonts_set_pixel_icons`. The roles are fixed: `FONT_TITLE`=font_px36b, `FONT_BODY`=font_px24, `FONT_SMALL` (label)=font_px20, `FONT_MONO`=`g_term_font`.
- Modify: `firmware/main/settings/settings.c`:
  - `settings_reset_defaults` writes the token terminal values (`UI_TK_TERM_*`, `ui_tk_term_ansi`), with `term_cursor_blink = 0` (B1) and `term_line_height = UI_TK_TERM_LINE_H`;
  - drop the `ui_theme_set_preset` and `ui_theme_set_terminal_amber` calls;
  - the `theme_*` fields in `settings.h` get "reserved (Plan 4)" comments.
- Modify: `firmware/main/ui/ui_settings.c`: drop the Appearance row. The rail uses `ui_srow(UI_ROW_NAV)`, with the selection in `panel-2` and a 50 % `fg` outline.
- Modify: `firmware/main/ui/ui_dashboard.c` and `ui_vitals.c`: replace their local `C_*` hex defines with `UI_TK_*` and `ui_needs_you_color()`. Drop `ui_motion_plain`.
- Modify: `firmware/sdkconfig.defaults`: drop `CONFIG_LV_FONT_MONTSERRAT_22` and `_30`, and make the default font `CONFIG_LV_FONT_DEFAULT_MONTSERRAT_14`, if `grep -rn "montserrat_22\|montserrat_30" firmware/main` finds nothing.
- Create: `firmware/tools/ui_preview/checks.c`, `checks.h`, `run_all.sh`. Modify: `firmware/tools/ui_preview/preview.c`, `CMakeLists.txt` (add `ui_comp.c` and `ui_tokens.c`; drop theme, motion and appearance), and `README.md`, which describes the case list, the checks and `run_all.sh`.
- Create: `firmware/tools/ui_lint.py`, `firmware/tools/test_ui_lint.py`. Modify: `firmware/test/host/CMakeLists.txt`: `add_test(NAME ui_lint COMMAND python3 ${MAIN}/../tools/ui_lint.py ${MAIN})`, and add `ui_tokens.c` to `pure`.

**Interfaces:**
- Produces, in `ui_tokens.h` (pure):
  - colours: `UI_TK_BG 0x0e1116`, `UI_TK_PANEL 0x161b22`, `UI_TK_PANEL2 0x1b212b`, `UI_TK_LINE 0x2a323d`, `UI_TK_FG 0xe9e7e1`, `UI_TK_MUTED 0x8a94a2`, `UI_TK_GREEN 0x54d08a`, `UI_TK_RED 0xef6f6f`, and `UI_TK_RED_BG_OPA 31` (12 %);
  - sizes: `UI_TK_MARGIN 24`, `UI_TK_GAP_S 8`, `UI_TK_GAP_M 16`, `UI_TK_GAP_L 24`, `UI_TK_ROW_H 64`, `UI_TK_SESSION_ROW_H 72`, `UI_TK_RAIL_ROW_H 72`, `UI_TK_HEADER_H 56`, `UI_TK_SESSION_BAR_H 56`, `UI_TK_FOOTER_H 64`, `UI_TK_BANNER_H 48`, `UI_TK_RADIUS 12`, `UI_TK_RADIUS_CHIP 8`, `UI_TK_TARGET 64`, `UI_TK_HIT 80`, `UI_TK_BAR_CTRL 48`, `UI_TK_LIST_W 440`, `UI_TK_RAIL_W 320`;
  - the terminal: `UI_TK_TERM_FG UI_TK_FG`, `UI_TK_TERM_BG UI_TK_BG`, `UI_TK_TERM_CURSOR UI_TK_FG`, `UI_TK_TERM_LINE_H 32`, and `extern const uint32_t ui_tk_term_ansi[16]` (the VS Code palette from today's `ANSI_STANDARD`);
  - `uint32_t ui_tk_needs_you_hex(void);`. Its 0xf4b545 is the only amber literal in the UI sources.
- Produces, in `ui_comp.h`. Every component uses shared static styles, and its only local styles are geometry.
  - `void ui_comp_init(void);`
  - `lv_color_t ui_needs_you_color(void);`. Its only callers are `ui_state_pill_set`, `ui_session_row_set` and `ui_header_needs_you_set`, and each marks the amber object with `UI_NEEDS_YOU_PART`.
  - `#define UI_NEEDS_YOU_PART LV_OBJ_FLAG_USER_2` and `#define UI_BAR_CONTROL LV_OBJ_FLAG_USER_3`. A bar control is 48 px visible, with an 80 px hit area.
  - `typedef enum { UI_BTN_PRIMARY, UI_BTN_SECONDARY, UI_BTN_DESTRUCTIVE } ui_btn_kind_t;` and `lv_obj_t *ui_btn(lv_obj_t *parent, ui_btn_kind_t kind, const char *text, lv_event_cb_t cb, void *ud);`. Buttons are 64 px. Primary is an `fg` fill with `bg` text; secondary is `panel-2` with a `line` border; destructive is `red` text with a `red` border.
  - `lv_obj_t *ui_chip(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud);`: 48 px, `panel-2`, radius 8, `lv_obj_set_ext_click_area` to reach 80, and `UI_BAR_CONTROL`.
  - `lv_obj_t *ui_header(lv_obj_t *scr, const char *title, const char *back_label);`: 56 px, `panel`, a 1 px `line` underneath. With `back_label` set, a "← <back_label>" bar control calls `ui_nav_back`. The vitals cluster sits at the right.
  - `lv_obj_t *ui_header_slot(lv_obj_t *header);`: the flex slot between the title and the vitals.
  - `lv_obj_t *ui_header_needs_you(lv_obj_t *header);` and `void ui_header_needs_you_set(lv_obj_t *label, int n);`: "● N need you" (or "● 1 needs you"), hidden at 0.
  - `typedef enum { UI_BANNER_MUTED, UI_BANNER_RED } ui_banner_kind_t;`
  - `lv_obj_t *ui_banner(lv_obj_t *scr, int32_t y);`: 48 px, starts hidden.
  - `void ui_banner_set(lv_obj_t *banner, ui_banner_kind_t kind, const char *text, const char *action, lv_event_cb_t cb, void *ud);`: NULL text hides it; it writes styles only on a change.
  - `lv_obj_t *ui_footer(lv_obj_t *scr);`: 64 px, `panel`, with left and right flex groups. `lv_obj_t *ui_footer_group(lv_obj_t *footer, bool right);`
  - `lv_obj_t *ui_state_pill(lv_obj_t *parent);` and `void ui_state_pill_set(lv_obj_t *pill, const cc_session_t *s);`. NEEDS YOU, only when `cc_session_needs_you(s)`, is amber with `bg` text. WORKING is green. The rest are muted on `line`. The text comes from `sm_pill_text` (Task 5); until Task 5 the pill uses today's `pill_text`.
  - `lv_obj_t *ui_session_row(lv_obj_t *parent, lv_event_cb_t cb, void *ud);` and `void ui_session_row_set(lv_obj_t *row, const cc_session_t *s, bool selected, const char *meta);`:
    - 72 px, radius 12, `panel`, a `line` border;
    - NEEDS YOU: a 4 px amber left edge and a 1 px amber border at 60 % (100 % when selected);
    - selected: `panel-2` with a 2 px `fg` outline at 50 %;
    - the name is cut with "…";
    - it does nothing when the content is unchanged.
  - `typedef enum { UI_ROW_VALUE, UI_ROW_SWITCH, UI_ROW_NAV, UI_ROW_ACTION, UI_ROW_INFO } ui_row_kind_t;`
  - `lv_obj_t *ui_srow(lv_obj_t *parent, ui_row_kind_t kind, const char *label, const char *value, lv_event_cb_t cb, void *ud);`: 64 px, and the whole row is the hit target.
  - `lv_obj_t *ui_srow_slider(lv_obj_t *parent, const char *label, int min, int max, int val, lv_event_cb_t on_release, void *ud);`
  - `lv_obj_t *ui_srow_value(lv_obj_t *row);`: the row's value label.
  - `lv_obj_t *ui_section_header(lv_obj_t *parent, const char *text);`: label muted, 24 above, 8 below. `lv_obj_t *ui_footnote(lv_obj_t *parent, const char *text);`
  - `void ui_picker(const char *title, const char *const *items, int count, int current, ui_menu_cb_t cb, void *ud);`: a sheet of 64 px radio rows, with an `fg` dot on `current`.
  - `void ui_confirm_ex(const char *title, const char *body, const char *action, bool destructive, ui_confirm_cb_t cb, void *ud);`: the title wraps; Cancel is secondary.
  - `lv_obj_t *ui_empty_state(lv_obj_t *parent, const char *text, const char *action, lv_event_cb_t cb, void *ud);` and `lv_obj_t *ui_skeleton_row(lv_obj_t *parent);`
- Produces, in `checks.h` (preview only):
  - `int pv_amber_outside(lv_obj_t *scr, const lv_draw_buf_t *snap);`: the count of pixels within CIE76 ΔE ≤ 10 of #f4b545 that fall outside the coords of every `UI_NEEDS_YOU_PART` object.
  - `int pv_touch_targets(lv_obj_t *root, char *why, size_t cap);`: the body needs ≥ 64 visible and ≥ 80 hit; `UI_BAR_CONTROL` needs ≥ 48 and ≥ 80; neighbours are ≥ 8 apart. 0 means OK.
  - `int pv_fit(lv_obj_t *root, char *why, size_t cap);`: no label extends past its parent, and a `LV_LABEL_LONG_WRAP` label is never clipped.
  - `unsigned pv_idle_pixels(lv_display_t *d, unsigned ms);`: the pixels flushed over `ms` after the first draw.
  - `int pv_timers(bool sessions_shown, char *why, size_t cap);`: no timer under 300 ms except the pty pump while a terminal is open, and the 300 ms tick only when `sessions_shown`.
  - `#define PV_SKIP_AMBER 1u` and `#define PV_SKIP_TARGETS 2u`.
  - `int pv_run_checks(lv_display_t *d, lv_obj_t *scr, const char *name, bool needs_you_expected, unsigned skip);`: runs all five checks except the skipped ones, prints `check <name>: ok`, and snapshots to `<name>.ppm`. The skips exist only for the screens that are not rebuilt yet: Task 4 passes them for `dashboard` and `settings *`; Task 6 drops them for Sessions and Task 7 for Settings, and from Task 7 on no case passes a skip.
- Produces, in `ui_lint.py`: `lint(main_dir) -> list[str]`, each entry `"<file>:<line>: <rule>"`. Exit 1 if there are any. The rules:
  - `amber-literal`: `f4b545` (any case) in `firmware/main/ui/**` outside `ui_tokens.c`;
  - `amber-accessor`: `ui_tk_needs_you_hex(` outside `ui_comp.c`, and `ui_needs_you_color(` outside `ui_comp.c`;
  - `exec-outside-cc_cmd`: in `firmware/main/ui/**`, a string literal matching `claude (attach|logs|agents|-p)` or containing `/bin/sh`; or a `cc_link_exec` / `cc_link_exec_tail` call with no `cc_cmd_` call earlier in the same function body. (`ui_terminal.c` gets its command from the caller, which built it with `cc_cmd_attach`, so it has no exec call to flag. The empty-state copy "Start one with `claude --bg`" doesn't match.)

- [ ] **Step 1: Write the failing lint tests** in `firmware/tools/test_ui_lint.py`, each over a temporary tree:
  - `test_amber_literal_flagged`: a `ui/x.c` with `0xF4B545` gives exactly one `amber-literal`;
  - `test_amber_in_tokens_allowed`: the literal in `ui/ui_tokens.c` gives `[]`;
  - `test_accessor_outside_comp_flagged`;
  - `test_exec_literal_flagged`: `cc_link_exec("claude agents", …)` in `ui/y.c` is flagged;
  - `test_clean_tree_passes`.
- [ ] **Step 2: Run PY and expect the import to fail.**
- [ ] **Step 3: Implement `ui_lint.py`, the tokens, `ui_comp`, the `ui_core` changes and the deletions** per the Files list.
  - `ui_comp_init` builds each style once into file-static `lv_style_t` objects: bg, panel, panel-2, the line border, the fg text, muted, label, the focus ring, the primary, secondary and destructive buttons, chip, pill, row and the selected row.
  - No style sets a shadow, a gradient or an opacity animation (§11), and the LVGL default theme's shadows are zeroed.
  - The P4-2 blank screen is created in `ui_init`.
- [ ] **Step 4: Write the preview checks and cases** in `checks.c` and `preview.c`. The new invocation is `./preview <case> [variant]` at 1280 × 720.
  - Remove preset cycling, the portrait sizes, `appearance`, `home`, the tile-fit check and the `motion` sweep comparison. Keep the idle part as `pv_idle_pixels`.
  - Cases in this task:
    - `boot`, with the existing splash assertions;
    - `sleep`;
    - `settings <0..6>`, with the existing click loop and the one-visible-panel assertion over the current rail;
    - `nav`: push Settings from home. A probe screen's create function asserts `!lv_obj_is_valid(prev)`;
    - `dashboard`, with `needs_you_expected = true`; the amber scan runs from Task 6.
  - `run_all.sh $PV` runs every case and converts each `.ppm` to `$PV/png/<name>.png` with `sips`.
- [ ] **Step 5: Run the tests and checks.**
  - PY: the lint tests pass.
  - HOST: `ui_lint` passes on the tree.
  - PV: `./preview nav`, `./preview boot`, `./preview sleep`, `./preview dashboard` and `./preview settings 0` through `settings 6` each print `check <name>: ok`, with the skips above.
  - By eye in `$PV/png`:
    - `boot.png`: logo and title in `#e9e7e1` on `#0e1116`, no orange;
    - `settings-0.png`: no orange anywhere, and the selected rail row is `panel-2` with an outline.
  - FW build.
- [ ] **Step 6: Device gate.**
  - Boot. The splash has no amber; check it with a screenshot taken within 1 s of `reboot`, or mark it PENDING for the visit if the timing can't be caught.
  - `ui settings`, then `key esc`: instant, with no slide. Repeat 10 times; `ts` internal free afterwards is within 1 KB of before (no leak).
  - `ui heapmin` over opening Settings › Network: ≥ 66,560 B.
  - `ui timers`: no `ui_motion` sweep timer.
  - Record the flash delta (fonts, themes) against §7.1.
- [ ] **Step 7: Commit.** Message: `feat(ui): tokens, components, instant navigation, preview checks and lint; remove themes, appearance and motion (Plan 4 Task 4)`.

### Task 5: Sessions model and hotkeys (pure)

**Files:**
- Create: `firmware/main/ui/sessions_model.h`, `sessions_model.c` (pure: `companion/cc_sessions.h` only).
- Modify: `firmware/main/ui/hotkeys.c`, `hotkeys.h`.
- Modify: `firmware/test/host/CMakeLists.txt`: add `sessions_model.c` to `pure`, and the test `test_sessions_model`.
- Create: `firmware/test/host/test_sessions_model.c`. Modify: `firmware/test/host/test_hotkeys.c`.

**Interfaces:**
- Produces (`sessions_model.h`):
  - `#define SM_KEY_MAX 128`, `#define SM_DRAFTS 16`, `#define SM_ANSWER_MAX 480`.
  - `void sm_key(const cc_session_t *s, char *out, size_t cap);`: a background session with an id gives the id. Otherwise it gives `"i:" + name + ":" + started_ms` in decimal (B3).
  - `int sm_select(const cc_session_t *items, int n, const char *sel_key, int prev_index);`: the index whose key equals `sel_key`. Otherwise `prev_index` clamped to `[0, n-1]`, and -1 when `n == 0`.
  - `int sm_next_needs_you(const cc_session_t *items, int n, int sel);`: the next index after `sel` (wrapping) where `cc_session_needs_you`. With none, `(sel + 1) % n`; -1 when `n == 0`.
  - `typedef struct { char key[SM_KEY_MAX]; char text[SM_ANSWER_MAX + 1]; uint32_t used; } sm_draft_t;` and `typedef struct { sm_draft_t d[SM_DRAFTS]; uint32_t clock; } sm_drafts_t;`. The caller allocates it in PSRAM.
  - `const char *sm_draft_get(const sm_drafts_t *ds, const char *key);`: returns `""` when there is none.
  - `void sm_draft_set(sm_drafts_t *ds, const char *key, const char *text);`: empty text clears it; when the store is full, the least recently set entry is evicted (P4-8).
  - `void sm_drafts_prune(sm_drafts_t *ds, const cc_session_t *items, int n);`: drops the drafts whose key matches no item.
  - The sort order is `cc_sessions_parse`'s, unchanged: it already sorts by state, then `started_ms` newest first (B5). The model doesn't re-sort.
  - `const char *sm_pill_text(const cc_session_t *s);`:
    - NEEDS_YOU with `cc_session_needs_you` → "NEEDS YOU";
    - NEEDS_YOU, background, not live → "ENDED" (P4-5);
    - NEEDS_YOU, interactive → "waiting on Host";
    - WORKING → "WORKING", IDLE → "IDLE", DONE → "DONE", ENDED → "ENDED".
  - The banner (§5.1 table, in precedence order, with P4-4):
    - `typedef enum { SM_B_NO_WIFI, SM_B_NO_HOST, SM_B_UNREACHABLE, SM_B_LOW_BATTERY, SM_B_RECONNECTING, SM_B_LOADING, SM_B_STALE, SM_B_EMPTY, SM_B_LIVE } sm_banner_t;`
    - `typedef struct { bool host_set, wifi_up, wifi_connecting, link_up, link_failed, poll_failing, reconnecting, waking, stale, have_list, low_battery_shown; int64_t down_ms; int n; } sm_in_t;`
    - `sm_banner_t sm_banner(const sm_in_t *in);`
    - `#define SM_UNREACHABLE_MS 30000`
  - Low battery: `typedef struct { bool armed, shown, dismissed; } sm_lowbat_t;` and `bool sm_lowbat_step(sm_lowbat_t *st, int pct, bool charging, bool dismiss);`. It shows once per discharge at or below 15 % while not charging, until a dismiss tap. It re-arms on charging or above 15 %. `pct < 0` never shows.
  - `int sm_wrap_tail(const char *in, int cols, int rows, char *out, size_t cap);`: splits on `\n`, wraps each line at `cols` UTF-8 characters with a 2-cell hanging indent on the continuations, and keeps the newest `rows` wrapped rows. Returns the row count.
  - `int sm_tail_options(const char *tail);`: when the last non-empty lines are a numbered list (`^\s*(❯\s*)?\d+\.`) of at least 2 items, the highest number. Otherwise 0.
- Produces (`hotkeys.h`):
  - `typedef enum { HK_D_NONE, HK_D_OPEN, HK_D_SETTINGS, HK_D_TERMINAL, HK_D_KEYS, HK_D_VOICE, HK_D_LOCAL } hk_dash_t;`: t/T and m/M → TERMINAL; `' '` with Ctrl → VOICE; o, p and k as today; any other Ctrl/Meta character → LOCAL.
  - `hk_term_t hk_takeover(const key_event_t *ev, bool takeover);`: Ctrl+] and Ctrl+Q → LEAVE in **both** modes (§4). Shift+↑/↓ scroll only in a take-over; Shift+PgUp/PgDn scroll in both.
  - `typedef struct { const char *keys, *what; } hk_line_t;` and `extern const hk_line_t hk_overlay[];` (NULL-terminated, P4-11).
  - `#define HK_VOICE_TOAST "Voice input comes in a later update"`. `HK_TAKEOVER_TOAST` and `HK_DASH_HINT` are removed.

- [ ] **Step 1: Write the failing tests** in `test_sessions_model.c`, with fixtures built in code:
  - `test_key_background_is_id`: `sm_key` of {background, id "a1b2c3d4"} is `"a1b2c3d4"`.
  - `test_key_interactive`: {interactive, name "desk", started_ms 1700000000123} gives `"i:desk:1700000000123"`.
  - `test_selection_follows_key_across_resort`: items [A,B,C], sel "B" at index 1. After the re-sort [B,C,A], `sm_select(…, "B", 1) == 0`.
  - `test_selection_on_removal_keeps_position`: [A,B,C], sel "B". After the update [A,C], it returns 1. After [A], it returns 0. After `[]`, it returns -1.
  - `test_draft_follows_key_across_resort`: set("B","hi"), re-sort, then `get("B")` is `"hi"` and `get("A")` is `""`.
  - `test_draft_cleared_on_send_and_removal`: set("B","x"), then `set("B","")` gives `""`. Set("C","y"), then prune with [A] gives `get("C") == ""`.
  - `test_drafts_evict_least_recent`: 17 sets of distinct keys leave the first key gone and keys 2–17 present.
  - `test_next_needs_you_wraps`: [W, N, W, N] from sel 3 gives 1. [W, W] from 0 gives 1.
  - `test_pill_text_rules`: all the P4-5 rows above.
  - `test_banner_precedence`, one assertion per row:
    - no Wi-Fi beats everything;
    - `!host_set` → NO_HOST;
    - `link_failed` → UNREACHABLE;
    - `down_ms` = 30000 with Wi-Fi up → UNREACHABLE, and 29999 → LOADING (no list) or RECONNECTING (`have_list`);
    - `poll_failing` → UNREACHABLE;
    - `low_battery_shown` with a live link → LOW_BATTERY, but `link_failed` with `low_battery_shown` → UNREACHABLE;
    - `reconnecting` → RECONNECTING;
    - `waking` → RECONNECTING;
    - `!have_list` → LOADING;
    - `stale` → STALE;
    - `n == 0` → EMPTY;
    - otherwise LIVE.
  - `test_lowbat_once_per_discharge`: 16 → false; 15 → true; a dismiss gives false at 14; charging re-arms; then 15 → true.
  - `test_wrap_hanging_indent`: `"abcdefghij"` with cols 4 gives the rows `"abcd"`, `"  ef"`, `"  gh"`, `"  ij"`.
  - `test_wrap_keeps_newest_rows`: 12 short lines with rows 10 keep lines 3–12.
  - `test_wrap_unbroken_line`: a 300-character line with cols 62 gives no row longer than 62 characters, and every byte appears once.
  - `test_wrap_utf8_counts_chars`: `"ééééé"` with cols 2 splits on characters, never inside a byte sequence.
  - `test_tail_options`: `"Pick\n1. A\n2. B\n3. C\n"` gives 3; `"Pick\n\xe2\x9d\xaf 1. Yes\n  2. No\n"` gives 2; `"1. only\n"` gives 0; `"text\n"` gives 0.
- [ ] **Step 2: Update `test_hotkeys.c`:**
  - `test_dashboard_terminal_keys`: Ctrl+t, Ctrl+T and Ctrl+m → TERMINAL.
  - `test_ctrl_space_is_voice`: Ctrl + `' '` → VOICE; a plain `' '` → NONE.
  - `test_ssh_ctrl_bracket_leaves`: Ctrl+] and Ctrl+q with `takeover = false` → LEAVE. This replaces the old `HK_T_NONE` assertion.
  - `test_terminal_passes_host_keys`: Ctrl+K, Ctrl+T and Ctrl+Space give `HK_T_NONE` in both modes.
  - `test_overlay_lists_every_shortcut`: `hk_overlay` has `keys` entries for ↑/↓, Enter, Ctrl+O, Ctrl+T, Ctrl+P, Ctrl+K, Ctrl+Space, Esc, Ctrl+] and Shift+↑/↓, each with a non-empty `what`.
  - Delete `test_hint_and_toast_strings`.
- [ ] **Step 3: Run HOST and expect the build to fail** (the symbols aren't defined yet).
- [ ] **Step 4: Implement** `sessions_model.c` and the hotkey changes. The existing callers of `HK_TAKEOVER_TOAST` and `hk_overlay_lines` (`ui_terminal.c`, `ui_dashboard.c`) keep compiling: they switch to `hk_overlay` and drop the toast here.
- [ ] **Step 5: Run HOST and expect all tests to pass.** Run the FW build.
- [ ] **Step 6: Commit.** Message: `feat(ui): pure session model (key, selection, drafts, banner, wrap) and Plan 4 hotkeys (Plan 4 Task 5)`.

### Task 6: Sessions screen (master-detail)

**Files:**
- Create: `firmware/main/ui/ui_sessions.c`, `ui_sessions.h`. Delete: `firmware/main/ui/ui_dashboard.c`, `ui_dashboard.h`. Carry over:
  - the worker task and jobs (`JOB_LOGS`, `JOB_PASTE`, `JOB_CHECK`, `JOB_FORGET_KEY`);
  - the notes, the visit and token guard, `CHECK_MAX_AGE_MS`, `cards_view()` and `open_now`.
- Modify:
  - `firmware/main/ui/ui_core.c`: `ui_nav_home` → `ui_sessions_create`, and the overlay check in `ui_keyboard_route`;
  - `firmware/main/ui/ui.h`;
  - `firmware/main/sys/power.c`: `ui_dashboard_selected_center` → `ui_sessions_selected_center`;
  - `firmware/main/companion/cc_console.c`: `companion takeover` calls `ui_sessions_open_test_session`;
  - `firmware/main/ui/ui_settings.c`: the forget call.
- Modify: `firmware/main/ui/ui_vitals.c`, `ui_vitals.h`:
  - one module timer, 30 s, created with the first cluster and deleted with the last, that updates every live cluster (B1);
  - the link dot reads `cc_link_connected()` (green), `cc_link_failure()` (red), or muted;
  - the label is the word "Host";
  - drop `ui_vitals_update`, `ui_vitals_link_dot` and `ui_vitals_link_host` from the header.
- Create: `firmware/test/host/fixtures/agents_16.json` (16 background sessions, 3 blocked) and `agents_long.json`:
  - a 95-character name with no spaces;
  - a 63-character repo;
  - CJK in a name;
  - one interactive blocked session.
- Modify: `firmware/tools/ui_preview/preview.c`:
  - stubs: `PREVIEW_FIXTURE`, `PREVIEW_WIFI`, `PREVIEW_HOST`, `PREVIEW_FAIL`, `PREVIEW_BAT`, `PREVIEW_CHG` and `PREVIEW_LOG`, plus a re-sort hook;
  - add the cases below, and delete the `dashboard`, `dashboard-reconnecting`, `dashboard-keys` and `settings-forget` cases (the new cases cover them).

**Interfaces:**
- Consumes:
  - Task 4: `ui_header`, `ui_header_needs_you(_set)`, `ui_banner(_set)`, `ui_footer(_group)`, `ui_chip`, `ui_btn`, `ui_session_row(_set)`, `ui_state_pill(_set)`, `ui_skeleton_row`, `ui_empty_state` and the `UI_TK_*` values;
  - Task 5: `sm_key`, `sm_select`, `sm_next_needs_you`, `sm_draft_*`, `sm_pill_text`, `sm_banner`, `sm_lowbat_step`, `sm_wrap_tail`, `sm_tail_options`, `hk_dashboard`, `hk_overlay` and `HK_VOICE_TOAST`;
  - Task 3: `ui_hosts_create`.
- Produces (`ui_sessions.h`):
  - `lv_obj_t *ui_sessions_create(void *arg);`
  - `bool ui_sessions_selected_center(int32_t *x, int32_t *y);`
  - `bool ui_sessions_forget_host_key(void);`
  - `int ui_sessions_open_test_session(const char *id);`, returning `UI_SESS_TEST_OK`, `UI_SESS_TEST_NO_CARD` or `UI_SESS_TEST_NOT_SHOWING`
  - `void ui_sessions_keys_overlay(bool show);` and `bool ui_sessions_keys_overlay_shown(void);`
  - `#define FORGET_TOAST_MS 6000`

**Behaviour, from spec §5.1 with B3 and B5:**
- **Built once.** At create the screen builds:
  - the header ("Sessions" and the needs-you count), a banner at y 56, and the footer;
  - a 440 px list holding a pool of 16 `ui_session_row`s, hidden when unused, 8 px apart;
  - a detail pane with 24 px margins: the title row, the log block, the answer row and the action row.
- **Updated in place.** The 300 ms tick only calls setters on a change. Nothing is cleaned or rebuilt on a poll, and the rows' RAM is constant whatever N is.
- **Selection and drafts.** The selection is held as an `sm_key` and re-found with `sm_select` on every snapshot. Drafts are an `sm_drafts_t` allocated with `heap_caps_calloc(…, MALLOC_CAP_SPIRAM)`, pruned on each snapshot, and cleared on a successful send.
- **Log block.** `mono` text. Fetch with `cc_log_tail(buf, 40)`, then `sm_wrap_tail(buf, cols, rows, …)`, where `cols = pane_w / 12` and `rows` comes from the block height ÷ the `FONT_MONO` line height (about 62 × 10). Record the measured cols and rows in the notes.
- **Answer row.** The answer field: 64 px, `panel-2`, radius 12, a 2 px `fg` border while focused. Keys come from the sink, and a tap focuses it. `softkbd_attach_textarea` runs only when `!keyboard_any_present()`. To its right is the mic slot: 64 × 64, a dashed `line` border, a muted glyph, drawn disabled. A tap toasts `HK_VOICE_TOAST`.
- **Action row.** `ui_btn(PRIMARY, "↵ Send")` and `ui_btn(SECONDARY, "⌃O Open")`. On the right, a muted note:
  - "1–N picks an option" when `sm_tail_options` returns N ≥ 2;
  - "Sent ✓" in green for 3 s after a send;
  - an error note in red.
  - Use the glyphs when `./preview glyphs` reports them in `FONT_SMALL`; otherwise the text "Enter" and "Ctrl+O".
- **Interactive sessions (B3).** The pane shows the state, the repo and the start time, and the muted line "Interactive — answer it on the Host". There is no log fetch, no field and no Open.
- **Keys** (`sessions_key` sink):
  - ↑ and ↓ move the selection;
  - Enter sends; on an empty field it opens;
  - Ctrl+O opens;
  - Ctrl+T and Ctrl+M push `ui_hosts_create`;
  - Ctrl+P pushes `ui_settings_create`;
  - Ctrl+K shows the overlay;
  - Ctrl+Space toasts `HK_VOICE_TOAST` and leaves the draft unchanged;
  - digits are typed into the field;
  - Esc clears the draft and never leaves Sessions;
  - the "R" message mode is removed. A non-blocked background session shows the field with the placeholder "Type a message…".
- **Touch.** A tap on a row selects it. A tap on the selected background row opens it. A swipe does nothing.
- **Footer chips.**
  - Left: "↑↓ Select" jumps with `sm_next_needs_you`; "↵ Send" and "⌃O Open".
  - Right: "⌃T Terminal", "⌃P Settings" and "⌃K Keys".
- **Banner** (from `sm_banner`; the text from §5.1 and §9.10):
  - No Wi-Fi: red, with [Wi-Fi settings], which pushes `ui_settings_create` (Task 7 points it at the section). The rows dim to 50 % and go inert.
  - No Host: no banner. The body shows `ui_empty_state("Set up your host to see its Claude sessions.", "Set up host", …)`.
  - Host unreachable: red, with the `cc_link_status_text` reason and [Host settings]. The rows dim and go inert.
  - Low battery: red, "Battery N %. The Tab5 sleeps at 10 %."; a tap dismisses it.
  - Reconnecting: muted, "Reconnecting to host…", and the list at 50 %.
  - Loading: muted, "Loading sessions…", over 3 `ui_skeleton_row`s.
  - Stale: muted, "Updating…".
  - Empty: the muted centred line "No sessions on this host. Start one with `claude --bg`."
- **The forget fold** follows P4-3, and the 500 ms `forget_poll_cb` timer is deleted.

- [ ] **Step 1: Write the failing preview cases.** Each calls `pv_run_checks`. `needs_you_expected` is true only where marked. Each also asserts what is listed:
  - `sessions live` (needs you): the header has `"● 2 need you"`; the selected row is index 0.
  - `sessions selected-needs-you` (needs you).
  - `sessions working`: `PREVIEW_FIXTURE` with no blocked sessions, and the amber scan finds 0 pixels.
  - `sessions loading`: 3 skeleton rows and the label "Loading sessions…".
  - `sessions stale`: "Updating…".
  - `sessions reconnecting`: "Reconnecting to host…", and no "No Wi-Fi".
  - `sessions no-wifi`: "No Wi-Fi" and the action "Wi-Fi settings".
  - `sessions not-found`: `PREVIEW_FAIL=notfound`, with the label "Host not found at my-host.local" and the action "Host settings".
  - `sessions no-host`: the primary button "Set up host", and no list.
  - `sessions empty`: the empty text.
  - `sessions interactive`: the line "Interactive — answer it on the Host"; no textarea in the pane; the pill text "waiting on Host"; the amber scan finds 0 pixels.
  - `sessions sent`: after the stub paste, "Sent ✓".
  - `sessions low-battery`: `PREVIEW_BAT=14`, "Battery 14 %. The Tab5 sleeps at 10 %."
  - `sessions 16` (needs you): 16 rows exist; scrolling the list to its end shows row 16 inside the list.
  - `sessions long`: `agents_long.json`, and the fit check passes (Review Focus 3).
  - `sessions resort` (needs you; Review Focus 1): select B, type "abc" through the sink, re-sort the stub to put B last, tick. Then the detail title is B's name, the field text is "abc", and the selected row is B's.
  - `sessions keys`: Ctrl+K shows the overlay and `z` closes it; Ctrl+T gives `ui_nav_is_top(ui_hosts_create)`; Esc, then Ctrl+Space, leaves the field unchanged with the toast `HK_VOICE_TOAST`; Ctrl+P gives Settings.
  - `ctrlk` (the overlay image).
  - `sessions idle`: `pv_idle_pixels(d, 2000) == 0` after the first draw, with no poll change; `pv_timers(true)` passes.
- [ ] **Step 2: Build PV and run `./preview sessions live`.** Expect it to fail (the case is unknown, or `ui_sessions_create` is undefined).
- [ ] **Step 3: Implement `ui_sessions.c`, the vitals timer and the renames** per the Files list and Behaviour.
- [ ] **Step 4: Run the checks.** Run `run_all.sh $PV`: every `sessions *` and `ctrlk` case prints `check …: ok`. Compare by eye with `mockups/Main.dc.html`:
  - `sessions-live.png`: a 440 px list on the left; a NEEDS YOU row with a 4 px amber edge; the detail pane with the log, the 64 px field, the dashed mic slot, Send and Open; footer chips left and right; amber only in pills, edges and the header count.
  - `sessions-no-wifi.png`: a red banner with a tinted background and dimmed rows.
  - `ctrlk.png`: two columns, nothing wrapped.
  - Then run HOST, PY (`ui_lint` passes) and the FW build.
- [ ] **Step 5: Device gate**, with 3 `companion-test-*` sessions (one blocked):
  - `ts` after 2 minutes on Sessions: internal free ≥ the Task 1 dashboard baseline (§11: a constant 16-row pool).
  - `ui timers` on Sessions: the only presentation timer under 1 s is the 300 ms tick; no 500 ms timer.
  - `ui heapmin` over `companion takeover <id>` for 60 s, then `key ctrl ]`: ≥ 40,960 B.
  - `key ctrl t` shows the server list; `key esc`. `key ctrl m` does the same.
  - `key text abc`, then `key ctrl space`: a screenshot shows the toast and the field "abc" with no space added.
  - `key text 2` then `key enter` answers the blocked test session. `sessions` shows it WORKING, and the pane shows "Sent ✓".
  - Review Focus 2: `power go off`, wait 3 s, `power sim touch`. The screen wakes; `ui timers` shows no terminal pump (nothing opened); `power wakes` shows `swallowed=1`. Repeat with `key down`: the selection is unchanged (screenshot).
  - `power sim battery 14`: the low-battery banner shows; `power sim battery off`.
  - Screenshots go to `$SHOTS/task6-*.jpg` (test sessions only).
- [ ] **Step 6: Stop the test sessions and commit.** Message: `feat(ui): Sessions master-detail on the session model; shared 30 s vitals timer; forget-key fold (Plan 4 Task 6)`.

### Task 7: Settings shell, and the Host, Power, Keyboard light, Display & time and About sections

**Files:**
- Rewrite: `firmware/main/ui/ui_settings.c`. Create: `firmware/main/ui/ui_settings.h`.
- Create: `firmware/main/settings/kbd_colour.h`, `kbd_colour.c` (pure). Modify: `firmware/main/settings/settings_migrate.c`, `.h` (add `settings_reset_scope`), and `firmware/main/settings/settings.c`, `.h` (add `settings_defaults(app_settings_t *out)` and `settings_reset_user()`).
- Modify: `firmware/main/ui/ui_sessions.c`: the banner actions call `ui_settings_open`.
- Modify: `firmware/main/input/key_console.c` (add `key layout stock|jis`) and `firmware/main/sys/power_console.c` (or wherever is nearest: add `ota url [<url>|none]`; a value with `?` is logged as `<scheme>://<host>/…?<masked>`).
- Modify: the `STR_*` table in `firmware/main/i18n/i18n.h`: the section names and every string in this task.
- Test: `firmware/test/host/test_kbd_colour.c` and `test_settings_migrate.c`. Modify: `firmware/test/host/CMakeLists.txt`.
- Modify: `firmware/tools/ui_preview/preview.c`: the `settings <0..5>` cases with `PREVIEW_CLICK` (a row label to click after the section opens) and `PREVIEW_SCROLL` (pixels to scroll the panel), and the sheet cases.

**Interfaces:**
- Consumes: Task 4 `ui_srow*`, `ui_section_header`, `ui_footnote`, `ui_picker`, `ui_confirm_ex`, `ui_btn` and `ui_header`; Task 6 `ui_sessions_forget_host_key`.
- Produces:
  - `typedef enum { UI_SET_HOST, UI_SET_NETWORK, UI_SET_POWER, UI_SET_KBD, UI_SET_DISPLAY, UI_SET_ABOUT, UI_SET_COUNT } ui_set_section_t;`
  - `void ui_settings_open(ui_set_section_t s);`: pushes Settings on section `s`. Plain `ui_settings_create(NULL)` lands on the last used section, which is Host after boot (RAM only).
  - Row builders for Task 8: `void ui_settings_fill_network(lv_obj_t *panel);` is declared in `ui_settings.h`. Until Task 8 its body in `ui_settings.c` keeps today's Network rows (the switch, the fallback, the static address and the P4-9 bridge row); Task 8 moves it to `ui_settings_net.c`.
  - `typedef struct { const char *label; uint32_t rgb; } kbd_swatch_t;` and `extern const kbd_swatch_t kbd_swatches[];`: Blue 0x0080FF, White 0xFFFFFF, Red 0xFF0000, Green 0x00FF00, Magenta 0xFF00FF, Cyan 0x00FFFF, then `{NULL, 0}`.
  - `int kbd_colour_index(uint8_t mode, const uint8_t rgb[3]);`: 0 = "Keyboard's own" (mode 0); `1 + i` for a swatch match in mode 1; -1 for other values (shown as "Custom").
  - `void kbd_colour_apply(int index, uint8_t *mode, uint8_t rgb[3]);`: 0 sets mode 0 and keeps rgb; `1..6` sets mode 1 and the swatch rgb; anything else changes nothing.
  - `void settings_reset_scope(app_settings_t *s, const app_settings_t *defaults);`:
    - it resets every `pw_*` field, `ss_disable_in_ssh`, `kbd_led_mode`, `kbd_led_bright`, `kbd_led_rgb`, `kbd_needs_light`, `kbd_dark_light` and `brightness`;
    - it keeps every other field.
  - `void settings_reset_user(void);`: calls `settings_defaults` and `settings_reset_scope`, saves, then applies the brightness, `power_kbd_light_refresh()` and the power settings.

**Sections** (§5.6–§5.12; rows from `ui_srow` kinds):
- **Shell.**
  - `ui_header(scr, "Settings", "Sessions")`.
  - A 320 px rail of 6 `UI_TK_RAIL_ROW_H` rows. The selected row is `panel-2` with a 50 % `fg` outline.
  - A scrolling panel with a `FONT_TITLE` heading.
  - Only the visible section is built. A rail click cleans and refills the one panel and keeps the screen object.
  - The muted footer hint "↑↓ move · Enter choose · Esc back" shows while a key was used last. It replaces the Ctrl+Tab "UI: Tab / Enter / Esc" toast, which is deleted from `ui_keyboard_route`.
  - ↑/↓ work with no Ctrl+Tab through the existing no-sink path in `ui_keyboard_route`.
- **Host.**
  - Host (nav): the address; opens a picker of the saved hosts, "Add host…" and "None", and sets it through `cc_link_set_server`. "Add host…" pushes `ui_host_form_create` once Task 9 lands; until then `ui_hosts_create`.
  - Address (nav): opens the host form (Task 9). Until then the row is shown with the entry's address and opens `ui_hosts_create`.
  - Link (info): "● connected over home Wi-Fi" in green, or the reason in red. Its second line is `cc_link_mac_path`, then the watch state from `cc_watch_status_text`.
  - Fallback address (value): `cc_link_set_lan_fallback`, with the toast from §5.7.
  - Claude path on host (value): `cc_link_set_claude_path`.
  - Section "Host key": the fingerprint (action), wrapped in full, with a destructive "Forget" chip. Its confirm uses the §5.5 wording and calls `ui_sessions_forget_host_key`.
  - Section "This device's key": the public key (action) "ssh-ed25519 … claude-companion · Show". Show opens a sheet with the whole `device_key_pubkey_line` in mono, wrapped, and the §5.7 note.
  - Footnote: the §5.7 text.
- **Power.** Three groups with footnotes. The values carry their units ("30 s", "Never"). The existing `pw_menu_t` tables stay and their titles lose "(s)". Hibernate after reads "not available yet" when `!hibernate_supported()`. Time zone and the two needs-you light rows leave this section.
- **Keyboard light.**
  - Keyboard (info): Connected / USB keyboard / Not connected.
  - Colour (picker): "Keyboard's own" plus the swatches drawn as 24 px filled circles in their colour (no hex shown), through `kbd_colour_index` and `kbd_colour_apply`.
  - Brightness (slider).
  - Section "Needs you": Show needs-you on keyboard (switch), and Brightness when the screen is dark (picker: Lowest 2 % / Low 5 % / Medium 15 %; any other value shows as "N %").
  - Footnote: "Amber on the keyboard always means a session needs you."
- **Display & time.** Brightness (slider, saved on release). Time zone (picker of `vitals_tz_presets`; a rule that is not a preset shows "Custom (console)"). Clock "YYYY-MM-DD HH:MM" with a "Sync now" chip that calls `sysinfo_start_sntp()`.
- **About.**
  - Version: `sysinfo_app_version()` and the build date from `esp_app_get_description()->date`.
  - Credits (muted): "Based on Clientre 5 by Mozgi512 (MIT)" and "SSH by libssh (LGPL-2.1)".
  - Diagnostics (nav, collapsed): expanding shows the §5.12 list, with a 2 s `lv_timer` that exists only while expanded and is deleted with the panel.
  - Reboot: a confirm.
  - Reset settings: destructive, and the confirm lists the scope from §5.12 including "keeps the time zone".
- **Gone from Settings:** Input (layout → the `key layout` verb), Security (the TF-card hosts, finished in Task 11), System (the OTA URL → the `ota url` verb), the gesture note and the old About alert.

- [ ] **Step 1: Write the failing tests.**
  - `test_kbd_colour.c`:
    - `test_swatches_never_amber`: `!pr_rgb_amber_like(rgb)` for every swatch;
    - `test_index_round_trip`: for i in 0..6, apply then index gives i;
    - `test_custom_is_minus_one`: mode 1 with rgb 0x123456 gives -1;
    - `test_apply_out_of_range_no_change`: indexes 7 and -1 leave mode and rgb unchanged.
  - `test_settings_migrate.c`:
    - `test_reset_scope_resets_power_light_brightness`: s has `pw_dim_s` 600, `kbd_led_bright` 90 and `brightness` 30, against defaults; afterwards they equal the defaults;
    - `test_reset_scope_keeps_identity_and_tz`: `companion_server`, `claude_path`, `mac_lan_ip`, `ts_enable`, `ts_auth_key`, `ts_device_name`, `tz_rule`, `kbd_layout`, `volume` and `ota_url` are unchanged after the reset.
- [ ] **Step 2: Run HOST and expect it to fail** (the symbols are undefined).
- [ ] **Step 3: Implement** `kbd_colour.c`, `settings_reset_scope`, `settings_defaults`, the Settings rewrite and the two console verbs.
- [ ] **Step 4: Run HOST (pass) and the preview cases:**
  - `settings 0` through `settings 5`: each passes `pv_run_checks` with the amber scan, touch targets and fit, and asserts one visible panel and the same screen object across all 6 rail clicks.
  - `settings 0 PREVIEW_CLICK="Public key"`: the sheet has the label prefix `"ssh-ed25519 "`.
  - `settings 0 PREVIEW_CLICK="Forget"`: the confirm title "Forget this host's key?" (wrapped, fit passes); OK calls the forget stub once.
  - `settings 5 PREVIEW_CLICK="Reset settings"`: the confirm body contains "keeps the time zone".
  - `settings 5 PREVIEW_CLICK="Diagnostics"`: one 2000 ms timer exists. Collapse it: no timer.
  - `settings 3 PREVIEW_CLICK="Colour"`: a picker of 7 rows; the amber scan finds 0.
  - `settings 2 PREVIEW_SCROLL=400`: the Battery saver group is visible.
  - `keyboard-settings`: with no sink installed, Down moves the focus from rail row 0 to rail row 1 with no Ctrl+Tab, and Esc gives `ui_nav_is_top(ui_sessions_create)`.
  - Compare `settings-0.png` by eye with `mockups/Settings.dc.html`: a 320 px rail, 64 px rows, the full vitals cluster, no "(macOS)".
- [ ] **Step 5: Device gate.**
  - `ui heapmin` over opening Settings › Host and over each section in turn: each minimum is ≥ 66,560 B with Tailscale off.
  - `key ctrl p` lands on Host; open Power; `key esc`; `key ctrl p` lands on Power.
  - Host › Public key › Show matches `sshkey` output (compare the first 40 characters).
  - Reset settings: set `power set dim 600` first. After the reset, `power` shows dim back at 30, and `clock` shows the time zone unchanged.
  - `key layout jis`, then `key layout stock`.
  - `ota url none`.
- [ ] **Step 6: Commit.** Message: `feat(ui): Settings shell and Host, Power, Keyboard light, Display & time, About; reset scope and keyboard colours (Plan 4 Task 7)`.

### Task 8: Settings › Network

**Files:**
- Create: `firmware/main/ui/ui_settings_net.c`: the Network panel and its sub-pages. Delete: `firmware/main/ui/ui_wifi.c` and `ui_tailscale.c`, and the P4-9 "Wi-Fi networks" bridge row.
- Modify: `firmware/main/net/tailscale.c`, `tailscale.h`:
  - `tailscale_get_auth_key()` reads only `g_settings.ts_auth_key`. Remove the boot fallback to `/sdcard/tailscale/authkey.txt` (U6).
  - `TS_OFF_MESSAGE` is reworded per §9.10 for the Terminal banner.
- Modify: the Tailscale forget path: it **keeps** `ts_auth_key` (U5). That is the old `ui_tailscale.c` `forget_task`, rewritten in `ui_settings_net.c`.
- Modify: `firmware/main/i18n/i18n.h` and `firmware/tools/ui_preview/preview.c`: a peer-list stub (`tailscale_peer_count` / `tailscale_peer`), `tailscale_set_event_cb` recording the last callback, and a wifi scan stub.

**Interfaces:**
- Consumes: Task 7 `ui_settings_fill_network` (implemented here), `ui_settings_open`; Task 4 components.
- Produces:
  - `typedef enum { UI_NET_SUB_NETWORKS, UI_NET_SUB_ADVANCED, UI_NET_SUB_PEERS } ui_net_sub_t;`
  - `lv_obj_t *ui_settings_sub_create(void *arg);`: arg is a `ui_net_sub_t`. It is a pushed screen with `ui_header(scr, <title>, "Settings")`. Back returns to Settings › Network.
  - `void ui_settings_add_peer_host(const ts_peer_t *p);`: opens the host form prefilled (Task 9 fills in the form; until then it toasts "Add it from Terminal").

**Rows** (§5.8, one scrolling column of 64 px rows):
- **Wi-Fi.**
  - The current network (nav): "● connected · strong" (green) or "Not connected" (muted). It opens a sheet with Disconnect (`wifi_mgr_disconnect`, RAM only) and Forget (`wifi_mgr_forget(i)`), both confirmed.
  - Networks (nav) opens `UI_NET_SUB_NETWORKS`:
    - Saved: tap to connect; a trash chip forgets, after a confirm;
    - Available: `wifi_mgr_scan_start` when shown, filled from `WIFI_EV_SCAN_DONE` through `wifi_mgr_set_cb`, and cleared on delete. There is no timer. A lock glyph marks a secured network, and a tap asks for the password in a masked prompt;
    - Join other…: the SSID, then a masked password.
  - Advanced (nav) opens `UI_NET_SUB_ADVANCED`: Static address, with today's `sip_*` code, the note "Applies from the next join", and empty turning it off.
- **Tailscale,** in U10's order. Every row stays visible with the switch off.
  1. Tailscale (switch): `tailscale_set_enabled`; the value reads "off · saved" or "on". Plan 3's toasts, reworded to the Host wording.
  2. Status (info): `"<state> · <ip> · DERP <region> · N peers online"`. TS_ERROR shows `tailscale_last_error()` in red. Off shows "Off: doesn't run, and check-ins don't wait on it."
  3. Auth key (nav): "•••••••• set" or "Not set". The sheet has:
     - Enter key: a masked prompt; never logged;
     - Load from SD card: `sd_mount()`; with no card, toast "No SD card"; read the first line of `/sdcard/tailscale/authkey.txt`; with no file, toast "No key file";
     - Clear key: destructive, confirmed.
  4. Device name (value): an empty name is refused.
  5. Control server (value): "Default (Tailscale)" when blank, with the placeholder `https://headscale.example`.
  6. Connection (picker): Relay only / Try direct, with the note "Applies the next time Tailscale starts".
  7. Peers (nav): "N online ›". It opens `UI_NET_SUB_PEERS`:
     - each row shows the hostname, the IP, online or offline, and direct or relay;
     - a header "Refresh" chip re-reads the list;
     - it updates on `tailscale_set_event_cb`, through `ui_async`, and the callback is cleared on delete;
     - a tap on a peer opens a sheet with "Add as SSH host".
  8. Forget this device on the tailnet (action, destructive, last): the §5.8 confirm text, with "You'll need a new auth key." added when no key is saved. It runs on a worker task as today, and **keeps the auth key**.

- [ ] **Step 1: Write the failing preview cases.** Each runs `pv_run_checks`:
  - `settings 1`: Network, with Tailscale on;
  - `settings 1` with `PREVIEW_TS=0`: the Status row contains "Off: doesn't run";
  - `settings 1 PREVIEW_SCROLL=600`: the Peers and Forget rows are visible;
  - `settings 1 PREVIEW_CLICK="Auth key"`: the sheet has 3 actions, and no label anywhere contains the stub key's text `tskey-auth-preview`;
  - `settings 1 PREVIEW_CLICK="Device name"` with an empty prompt result: nothing saved (`g_settings.ts_device_name` unchanged), and the toast shown;
  - `settings-sub networks`, `settings-sub advanced`, `settings-sub peers`;
  - `settings-sub peers deleted-event` (Review Focus 5): open Peers, go back, and the last `tailscale_set_event_cb` callback is NULL; invoking the stub's saved callback does nothing.
- [ ] **Step 2: Run PV `./preview settings-sub peers` and expect it to fail.**
- [ ] **Step 3: Implement** per the Rows, and remove `ui_wifi.c`, `ui_tailscale.c` and the SD auth-key boot fallback.
- [ ] **Step 4: Run PV `run_all.sh`, HOST, PY and the FW build.** Compare `settings-1.png` by eye with `mockups/Network.dc.html`: the order follows U10, one column, and the auth key is masked.
- [ ] **Step 5: Device gate.**
  - Tailscale off: `ui heapmin` over opening Settings › Network ≥ 66,560 B.
  - `ts on`, wait for `ts` to show connected, then `ui heapmin` over opening Settings › Network ≥ 20,480 B. Peers lists ≥ 1 peer. Then `ts off`.
  - Join Wi-Fi from Settings with no Host set: run `companion host none` first, then rejoin the current network from Networks › Saved. Restore it with `companion host <name>` (record the name before you start).
  - Forget this device: run it only if the user agreed to a tailnet re-registration in the task brief; otherwise mark it PENDING for the visit. Then `ts` shows the auth key still set.
- [ ] **Step 6: Commit.** Message: `feat(ui): Settings › Network: Wi-Fi sub-pages, Tailscale rows in U10 order, Peers; auth key masked and kept on forget (Plan 4 Task 8)`.

### Task 9: Terminal host list and host form

**Files:**
- Create: `firmware/main/ui/ui_hosts.c`, `firmware/main/ui/ui_host_form.c`. Delete: `firmware/main/ui/ui_servers.c` and `ui_server_form.c`.
- Create: `firmware/main/servers/host_rules.c`, `host_rules.h` (pure). Test: `firmware/test/host/test_host_rules.c`, and `firmware/test/host/CMakeLists.txt`.
- Modify: `firmware/main/ssh/ssh_client.c` (`authenticate`, around line 214): B6.
- Modify:
  - `firmware/main/ui/ui_settings.c`: Host › Address and "Add host…" now open `ui_host_form_create`;
  - `firmware/main/ui/ui_settings_net.c`: `ui_settings_add_peer_host` opens the prefilled form;
  - `firmware/main/i18n/i18n.h`;
  - `firmware/tools/ui_preview/preview.c`: a server-store stub holding the Host, a LAN host and a tailnet host, with `ssh_known_host_fingerprint` and `ssh_known_host_forget` stubs.

**Interfaces:**
- Consumes: `cc_is_companion_server`, `cc_link_is_companion_server`, `ts_host_maybe_tailnet` / `tailscale_is_tailnet_ip`, `tailscale_peer*`, `tailscale_set_event_cb`, `ssh_known_host_fingerprint`, `ssh_known_host_forget`, and the Task 4 components.
- Produces:
  - `lv_obj_t *ui_hosts_create(void *arg);`: the same name as the Task 3 bridge.
  - `typedef enum { HF_FROM_TERMINAL, HF_FROM_SETTINGS_HOST, HF_FROM_HOST_PICKER } hf_origin_t;`
  - `typedef struct { int index; hf_origin_t origin; char prefill_name[48], prefill_host[128]; } hf_arg_t;`: `index` is -1 for a new host. The caller keeps the arg static.
  - `lv_obj_t *ui_host_form_create(void *arg);`: arg is `hf_arg_t *`.
  - `typedef enum { HF_OK, HF_NO_ADDRESS, HF_NO_USER, HF_BAD_PORT, HF_IS_HOST } hf_err_t;`
  - `hf_err_t hf_validate(const char *address, const char *port, const char *user, bool is_host_entry, const char *host_address, int *port_out);`: port 1..65535, decimal digits only; the address is not `cc_same_host` as the Host's unless `is_host_entry`.
  - `typedef enum { HF_AUTH_DEVICE, HF_AUTH_PASSWORD, HF_AUTH_SD_FILE } hf_auth_t;`
  - `hf_auth_t hf_auth_of(bool use_key, const char *keypath);`: `use_key` with an empty or `"device"` keypath gives DEVICE; `use_key` with another path gives SD_FILE; `!use_key` gives PASSWORD.
  - `bool hf_tailnet_host(const char *host);`: 100.64.0.0/10 or `*.ts.net`.
  - `typedef enum { HF_KEY_DEVICE, HF_KEY_FILE, HF_KEY_FILE_MISSING } hf_key_src_t;` and `hf_key_src_t hf_key_source(const char *keypath, bool file_exists);`: an empty or `"device"` keypath gives DEVICE; any other path gives FILE when it exists and FILE_MISSING when not. `ssh_client.c`'s `authenticate` uses it (B6).

**Behaviour:**
- **Terminal** (§5.3):
  - `ui_header(scr, "Terminal", "Sessions")`.
  - The banner shows when `!tailscale_enabled()` and any listed host is `hf_tailnet_host`. It reads the §5.3 text, with a primary "Turn on" that calls `tailscale_set_enabled(1)`.
  - Rows are 72 px: a dot (tailnet hosts while Tailscale is on: green when the peer is online, `line` otherwise; LAN hosts have none), the name in body bold, `user@host` in muted, "↵ Connect" on the focused row, and an Edit chip.
  - The Host's entry is never listed (`cc_link_is_companion_server`).
  - The last row is "+ Add host" (dashed). It opens a sheet with "From your tailnet" (online peers, only when the switch is on) and "Enter manually".
  - Footnote: the §5.3 text.
  - The list is refreshed on show and on Tailscale events. The callback is cleared on delete, and there is no timer.
  - Connecting shows a spinner on the row; a failure shows a red note on the row.
- **Host form** (§5.5):
  - Rows: Name, Address, Port, User, then "Sign in with" (a picker of Device key / Password, plus a read-only "Key file on SD card" while `hf_auth_of` is SD_FILE). The Password row shows only for Password, and is masked.
  - Host key: only when a pin exists for host:port. The fingerprint is in mono, with a destructive Forget whose confirm uses the §5.5 text and calls `ssh_known_host_forget`.
  - Delete host: destructive, with a confirm. It is not shown for the Host's entry when opened from Settings.
  - The header has a "Save" primary. Esc with unsaved changes asks "Discard changes?".
  - Saving from `HF_FROM_HOST_PICKER` also calls `cc_link_set_server(name)` (P4-6).
- **The Host refused.** `term_create`'s refusal toast (`TERM_COMPANION_REFUSED`) becomes the footnote's first sentence, "Your Claude host isn't listed here: its sessions are on the Sessions screen."
- **B6.**
  - A host entry with `use_key` and a keypath under `/sdcard/` calls `sd_mount()` before connecting.
  - If the file is then missing, the connect fails with the error "Key file not found on SD card".
  - The `ENOENT` → device-key fallback goes. The device key is used only for an empty or `"device"` keypath.

- [ ] **Step 1: Write the failing host tests** in `test_host_rules.c` (Review Focus 4):
  - `test_hf_validate_ok`: ("box.local", "22", "me", false, "mac.local") gives HF_OK, with port 22.
  - `test_hf_validate_port`: "0", "70000", "abc", "" and "22x" each give HF_BAD_PORT.
  - `test_hf_validate_required`: an empty address gives HF_NO_ADDRESS; an empty user gives HF_NO_USER.
  - `test_hf_validate_is_host`: ("MAC.local.", …, false, "mac.local") gives HF_IS_HOST; with `is_host_entry = true` it gives HF_OK.
  - `test_hf_auth_of`: (true, "") gives DEVICE; (true, "device") gives DEVICE; (true, "/sdcard/id_ed25519") gives SD_FILE; (false, "") gives PASSWORD.
  - `test_hf_tailnet_host`: "100.64.0.1" and "box.tail1234.ts.net" are true; "100.128.0.1" and "192.168.1.5" are false.
  - `test_key_source_never_falls_back`: ("/sdcard/id", false) gives HF_KEY_FILE_MISSING, never DEVICE; ("/sdcard/id", true) gives FILE; ("", false) and ("device", false) give DEVICE.
- [ ] **Step 2: Run HOST and expect it to fail.**
- [ ] **Step 3: Write the failing preview cases:**
  - `terminal` with Tailscale off: the banner contains "Tailscale is off" and "Turn on"; 2 rows; no row named like the Host's entry.
  - `terminal on`: no banner; the tailnet row has a dot and the LAN row has none.
  - `terminal empty`: only "+ Add host" and the footnote.
  - `terminal keys`: with no sink installed, Down moves the focus from row 0 to row 1 with no Ctrl+Tab.
  - `terminal deleted-event` (Review Focus 5): open Terminal, go back, the last event callback is NULL, and the stub's saved callback does nothing.
  - `hostform new`: no Host key and no Delete rows; Save with port "70000" toasts the HF_BAD_PORT copy and saves nothing.
  - `hostform edit-pin`: the fingerprint row and Forget; the Forget confirm calls `ssh_known_host_forget` once after OK.
  - `hostform sd-key`: "Key file on SD card" is shown, read-only.
- [ ] **Step 4: Implement** per the Behaviour.
- [ ] **Step 5: Run HOST, PV `run_all.sh`, PY and the FW build.** Compare `terminal.png` and `hostform-edit-pin.png` by eye with the Terminal mockup's host list.
- [ ] **Step 6: Device gate.**
  - `key ctrl t`: the Terminal list doesn't show the Host's entry.
  - Use a saved non-Host entry the user already has (never the Host, never a new account on any box). Edit → Forget host key; the next connect asks to trust the key, and you accept it.
  - B6: the form can't create an SD key entry, so the rule is proven by `test_key_source_never_falls_back`. On the device, check it only if an existing entry already has an SD keypath, with no card inserted: the connect fails with "Key file not found on SD card". Otherwise mark it PENDING for the visit.
  - `ts on`: the tailnet hosts show dots. `ts off`: the banner shows; Turn on turns it on (`ts`). Then `ts off`.
  - SSH session `ui heapmin` over 60 s of a session to a non-Host box: record it.
- [ ] **Step 7: Commit.** Message: `feat(ui): Terminal host list and host form; per-host Forget host key; SD key file never falls back (B6) (Plan 4 Task 9)`.

### Task 10: Session bar for both terminals

**Files:**
- Modify: `firmware/main/ui/ui_terminal.c`:
  - remove `hotzone()`, `hotzone_cb`, `build_ctrl` (the 21-key side column), `build_status` (the status strip with the red X), `term_long_cb` and its menu, and the "Ctrl+] to leave" toast;
  - add the session bar;
  - the banner lines say "Opening the session on the host…".
- Modify: `firmware/main/term/term_view.c`: create no blink timer when `blink == false`.
- Modify: `firmware/main/i18n/i18n.h` and `firmware/tools/ui_preview/preview.c` (the `takeover` and `ssh` cases render `term_create` with a stub ssh).

**Interfaces:**
- Consumes: Task 5 `hk_takeover` (LEAVE in both modes); Task 4 `ui_chip` and `ui_confirm_ex`; `UI_TK_TERM_*` and `ui_tk_term_ansi` (P4-10).
- Produces, file-local:
  - `static lv_obj_t *session_bar(term_ctx_t *c, const char *name);`: 56 px, `panel`, over the top of the terminal. Left to right:
    - "← Leave" (a bar control);
    - the name;
    - the link dot;
    - "⌨", only when `!keyboard_any_present()`;
    - the chips Esc, Tab, ^C and ↑ ↓, each sending its bytes through `term_keys_encode`.
  - `static void bar_show(term_ctx_t *c, uint32_t ms);`: shows the bar and arms one 3 s one-shot `lv_timer` (no repeat).

**Behaviour:**
- The bar shows for 3 s on open, then hides. A tap in the top 80 px shows it again for 3 s, and no key does.
- The bar states:
  - connecting: "Connecting…", muted;
  - link lost: the bar stays up with a red "Link lost, reconnecting…";
  - session ended: a toast, then back to the screen below.
- **Take-over.** Leave and Ctrl+] close it at once ("Session still running on the host").
- **SSH session.** Leave and Ctrl+] ask "Disconnect from <host>?" (`ui_confirm_ex`, action "Disconnect"). While the confirm is up, keys go to the modal.
- **Palette.** `term_view_set_colors(UI_TK_TERM_FG, UI_TK_TERM_BG)`, `vt_set_palette(ui_tk_term_ansi)`, a block cursor in `UI_TK_TERM_CURSOR`, no blink, and `fonts_set_term_line_height(UI_TK_TERM_LINE_H)`. Claude Code's own colours are drawn as sent.
- The pty pump (16 ms) is created at open and deleted in `scr_delete_cb` (B1). "Stay awake in Terminal" keeps its `POWER_HOLD_SSH`.

- [ ] **Step 1: Write the failing preview cases:**
  - `takeover`: the bar is visible with "Leave" and the name; after `step(d, 3100)` the bar is hidden; a pointer tap at y = 20 shows it again; `pv_timers` allows only the 16 ms pump; the amber scan finds 0; touch targets pass for the bar chips.
  - `takeover idle`: after the bar hides, `pv_idle_pixels(d, 2000) == 0` (no cursor blink).
  - `ssh`: Ctrl+] through the sink shows the confirm "Disconnect from box?"; Cancel stays; OK leaves.
- [ ] **Step 2: Run PV `./preview takeover` and expect it to fail.**
- [ ] **Step 3: Implement** per the Behaviour and the removals.
- [ ] **Step 4: Run PV `run_all.sh`, HOST, PY and the FW build.** Compare `takeover.png` by eye with `mockups/Terminal.dc.html`: a 56 px bar, chips 48 px tall, `#0e1116` background, no amber chrome.
- [ ] **Step 5: Device gate.**
  - Take-over of a `companion-test-*` session: `ui heapmin` over 60 s ≥ 40,960 B.
  - `ui timers` while it is open: the 16 ms pump and the 10 ms drain only under 300 ms, and no 550 ms blink. After `key ctrl ]`: no 16 ms timer.
  - In the take-over, `key ctrl k` and `key ctrl t` reach the Host. The session shows `^K`/`^T` handling, and the device shows no overlay and no Terminal list.
  - SSH to a non-Host box: `key ctrl ]` shows the confirm; `key enter` disconnects.
  - Review Focus 2: in a take-over, `power go off`, then `power sim touch`. The screen wakes, the terminal stays open, and `power wakes` shows `swallowed=1`.
- [ ] **Step 6: Stop the test sessions and commit.** Message: `feat(ui): session bar for take-over and SSH; token terminal palette, no blink; hot zones and side column removed (Plan 4 Task 10)`.

### Task 11: CFG 23 migration, the TF-card hosts and SD on demand

**Files:**
- Modify:
  - `firmware/main/settings/settings_migrate.c`, `.h`: `CFG_VERSION 23`, with the pure steps keyed `old_version < 23`;
  - `firmware/main/settings/settings.c`: the post-load ESP-IDF step for `creds_on_sd`;
  - `firmware/main/settings/settings.h`: the reserved comments on `theme_*`, `status_items`, `creds_on_sd`, `ssh_bell`, `pw_light_sleep` and `term_font` ("reserved (Plan 4)"); `volume`, `kbd_layout` and `ota_url` stay live.
- Modify: `firmware/main/servers/server_store.c`, `.h`:
  - remove `server_store_set_on_sd`, `server_store_reload` and the SD path;
  - add `int server_store_import_sd_once(void)`, which returns the count imported, or -1 when the card or file can't be read. It is used only by the migration.
- Modify: `firmware/main/app_main.c`: no `sd_mount()` at boot (P4-7). `server_store_init()` reads NVS only.
- Test: `firmware/test/host/test_settings_migrate.c`.

**Interfaces:**
- Produces: `uint32_t settings_blob_load(app_settings_t *s, const void *blob, size_t len);`, the same signature, plus for `version < 23`:
  1. `term_fg` = `UI_TK_TERM_FG`, `term_bg` = `UI_TK_TERM_BG`, `term_cursor` = `UI_TK_TERM_CURSOR`, `term_ansi` = `ui_tk_term_ansi`, `term_cursor_style` = 0, `term_cursor_blink` = 0, `term_line_height` = 32;
  2. `rotation` = 1;
  3. step 3 (the card) is not pure: see Step 3 below;
  4. an amber-like `kbd_led_rgb` → 0x0080FF (the existing load-time rule; also restated in the pure function so the test covers it).
- Consumes: Task 4 `ui_tokens.c` (already in `pure`).

- [ ] **Step 1: Write the failing tests** in `test_settings_migrate.c`:
  - `test_migrate_v21_to_v23_terminal_and_rotation`: a v21 blob with `term_fg` 0xFF9800, `term_cursor_blink` 1, `term_line_height` 26 and `rotation` 0 loads as the token values, blink 0, height 32, rotation 1, version returned 21, and `companion_server` unchanged.
  - `test_migrate_v20_and_v22_to_v23`: the same assertions from v20 (which also keeps the v21 Tailscale-off step) and from v22.
  - `test_v23_blob_unchanged`: a v23 blob with `term_line_height` 30 loads with 30 (no step runs).
  - `test_migrate_amber_kbd_to_blue`: `kbd_led_rgb` {0xf4,0xb5,0x45} from v21 gives {0,0x80,0xff}.
  - `test_newer_blob_takes_newer_path`: a v24 blob copies what fits and returns 24 (the downgrade path is unchanged).
- [ ] **Step 2: Run HOST and expect it to fail.**
- [ ] **Step 3: Implement the migration.** In `settings.c`, post-load, when `old_version < 23 && g_settings.creds_on_sd == 1`:
  - call `sd_mount()`, then `server_store_import_sd_once()`;
  - on ≥ 0, log `hosts on SD imported: <n>`; on -1, log `hosts on SD not imported: no card` and keep the NVS list; leave the SD file untouched in both cases;
  - set `creds_on_sd = 0` and save.
- [ ] **Step 4: Run HOST (pass), the FW build and PV `run_all.sh`** (the migration changes no render).
- [ ] **Step 5: Device gate.**
  - Flash. The first boot logs `settings v21 -> v23` (or `v22`); the second boot logs no migration.
  - `companion status` shows the same Host and the link connects. The Terminal list is unchanged.
  - `ui heapmin` and `ts` at steady state: record them (no mount at boot means no SD driver RAM).
  - Boot time from `boot complete in <ms>`: record it against Task 10.
- [ ] **Step 6: Commit.** Message: `feat(settings): CFG 23 migration; TF-card hosts imported once; SD mounted on demand (Plan 4 Task 11)`.

### Task 12: Copy pass: Host, never Mac, and one string table

**Files:**
- Modify: every user-visible string still outside `tr()` in `firmware/main/ui/**` moves into `firmware/main/i18n/i18n.h`. Delete every unused `STR_*` (the lint finds them).
- Modify: the user-visible and console text in
  - `firmware/main/companion/cc_link.c`, `cc_link.h` (the status texts, `CC_LINK_TS_OFF_TEXT`, "no Mac set up" → "no host set up");
  - `firmware/main/companion/cc_console.c` ("Mac entry host" → "Host entry host", "New Mac key pinned" → "New host key pinned", "No Mac set up" → "No host set up");
  - `firmware/main/sys/checkin.c` (its log lines);
  - `firmware/main/ui/ui_terminal.c`;
  - `firmware/main/ui/ui_sessions.c` and the comments in their headers.
  - Code identifiers such as `mac_lan_ip`, `cc_link_mac_path` and `companion_server` stay.
- Modify: `firmware/main/ui/ui.h`: delete the `UI_*` compatibility aliases from Task 4 and `ui_button_colored`. Callers use `UI_TK_*` or `ui_comp`.
- Modify: `firmware/tools/ui_lint.py`, `test_ui_lint.py`: the rules `mac-word`, `literal-outside-tr` and `ui-alias`.
- Modify: any host test or preview assertion that matched an old string, such as `"Reconnecting to Mac…"`.

**Interfaces:**
- Produces, as lint rules:
  - `mac-word`: `\bMac\b` inside a string literal in `firmware/main/ui/**`, `i18n/i18n.h`, `companion/cc_link.c`, `cc_link.h`, `cc_console.c` or `sys/checkin.c`;
  - `literal-outside-tr`: a string literal passed to `lv_label_set_text`, `ui_toast*`, `ui_srow*`, `ui_btn`, `ui_chip`, `ui_banner_set`, `ui_confirm_ex`, `ui_prompt` or `ui_picker` in `firmware/main/ui/**` that is not `tr(...)`, not `""`, and not purely a format such as `"%s"`;
  - `ui-alias`: the word-bounded names `UI_BG`, `UI_CARD`, `UI_CARD_HI`, `UI_ACCENT`, `UI_ACCENT2`, `UI_DANGER`, `UI_WARN`, `UI_TEXT`, `UI_MUTED` or `ui_button_colored` anywhere under `firmware/main` (`UI_TK_*` doesn't match).

- [ ] **Step 1: Write the failing lint tests:**
  - `test_mac_word_flagged`: `ui_toast("Can't reach your Mac")` is flagged; the identifier `mac_lan_ip` and the string `"MacBook"` are not;
  - `test_literal_outside_tr_flagged`;
  - `test_tr_and_format_allowed`;
  - `test_ui_alias_flagged`.
  - Run PY and expect them to fail.
- [ ] **Step 2: Run `python3 firmware/tools/ui_lint.py firmware/main` with the new rules.** Expect a list of findings. Save it to `$SHOTS/lint-before.txt`.
- [ ] **Step 3: Fix every finding** with the §9.10 rewrites, sentence case, units in values, and "…" for work in progress.
- [ ] **Step 4: Run the checks.** The lint gives `[]`. PY, HOST (with `ui_lint`) and PV `run_all.sh` pass, with every assertion updated to the new strings. `grep -rn '"[^"]*\bMac\b' firmware/main --include=*.c --include=*.h` finds nothing.
- [ ] **Step 5: Device gate.**
  - `companion status` prints no "Mac".
  - `companion host none`: Sessions shows "Set up host". Restore it with `companion host <name>`.
  - Screenshots of Sessions, Settings › Host and Terminal contain no "Mac" (read them).
- [ ] **Step 6: Commit.** Message: `feat(ui): copy pass: Host not Mac, one tr() table, lint for words, literals and aliases (Plan 4 Task 12)`.

### Task 13: Final device check and measurement

**Files:**
- Modify: `docs/superpowers/notes/2026-10-24-ui-overhaul-notes.md`: the final table.
- Modify: `docs/superpowers/specs/2026-10-22-claude-tab5-design.md`: the roadmap table (§15): Plan 3 is "Local link"; Plan 4 folds in the home screen and the design pass; voice is next; camera is last.
- No product code changes. A failed gate gets a fix commit in the task that owns the code, and this task's step is re-run.

- [ ] **Step 1: Sizes.** Run `idf.py size` and `idf.py size-components` and record them against the Task 1 baseline and §7.1/§7.2: flash (expected about −0.82 MB, or −0.9 MB with the camera pipeline unlinked), static internal RAM, and the task count from the boot log (`MSC` task absent).
- [ ] **Step 2: Heap gates (B2).** Tailscale off, then on. Record N and L:
  - TS off: steady state on Sessions ≥ the Task 1 dashboard baseline; Settings › Network ≥ 66,560 B; a take-over (60 s) ≥ 40,960 B.
  - TS on (`ts on`): Settings › Network ≥ 20,480 B. Then a **10-minute** take-over of a `companion-test-*` session ≥ 20,480 B. `grep -c "err=-2\|ERR_BUF"` over the captured console log is 0, and `companion status` shows connected throughout (check it every 2 minutes from the T5 helper). Compare with L28's 8,116 B. Then `ts off`.
- [ ] **Step 3: Timers.** `ui timers` on Sessions, Settings, Terminal and a take-over matches B1. Then `power go off`, wait 60 s, `power go active`, and run `ui timers` immediately: every UI timer's "last run" age is ≥ 60,000 ms, except the key drain. No 60 ms timer exists.
- [ ] **Step 4: Keys.**
  - In both layouts (`key layout stock`, then `key layout jis`, then back to stock), `key log on` then `key ctrl t`, `key ctrl m` and `key ctrl space` log `cp` with `MOD_CTRL`, and Ctrl+Space logs `cp=' '`.
  - `key ctrl ]` leaves both terminals.
  - For each of Sessions, Settings and a take-over: `power go dim`, then `key down`; then `power go off`, then `power sim touch`. Each is swallowed (`power wakes` shows `swallowed` ≥ 1 per wake), and nothing reaches the Host (the test session's log is unchanged).
- [ ] **Step 5: Screens.** Take a console `screenshot` of each of the six screens and every Settings section, with only `companion-test-*` sessions, to `$SHOTS/final-*.jpg`, outside git. Read each one against its mockup.
- [ ] **Step 6: Flows** (§12 step 6):
  - join Wi-Fi with no Host set;
  - Tailscale on and off from Network and from Terminal's Turn on;
  - Forget node keeps the key (PENDING if the user hasn't agreed to a re-registration);
  - Forget host key on a Terminal host;
  - the Reset settings scope;
  - the device key Show matches `sshkey`.
- [ ] **Step 7: Power.** On USB, run the Task 1 idle measurement on Sessions: mean W ≤ the baseline. The check-in awake time is ≤ the Plan 3 figure.
- [ ] **Step 8: The user's visit.** Write the batched list in the notes: real-finger touch targets, two-thumb reach for the footer and the session bar, the typeface question (§15), a look at every screen, and every PENDING item above.
- [ ] **Step 9: Stop all `companion-test-*` sessions, check `ts` is off, and commit.** Message: `docs(plan4): final measurements, gates and the visit list; roadmap table (Plan 4 Task 13)`.

---

Execution: subagent-driven development (superpowers:subagent-driven-development), one fresh implementer and reviewer per task.
