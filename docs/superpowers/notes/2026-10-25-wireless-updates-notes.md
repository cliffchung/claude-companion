# Plan 6 (wireless firmware updates): measurements and rulings

Each task appends its section. Host names, addresses and session names are placeholders (`my-host.local`, `you`).

This round covers Tasks 1 to 8 (code and host-side tests only). Every device step is listed under
"Device steps waiting for Task 9" at the end; none was run.

## Task 1: fw_rules

- `ota/fw_rules.c` is pure (only `i18n/i18n.h` and `sys/power_rules.h`); little-endian reads by bytes. It is in
  `test/host`'s `pure` library; `test_fw_rules` has 23 tests and also passes under `-fsanitize=address,undefined`
  (a scratch build: `test/host` has no sanitizer configuration of its own).
- Added beyond the interface: `fw_hdr_name` and `fw_layout_name` (the `FW_HDR_*` / `FW_LAYOUT_*` names for the
  console and `fw_print`), and the `FW_OFF_*` byte offsets, so `fw_update.c` can pin them with `_Static_assert`.
- `fw_image_layout` returns `FW_LAYOUT_TRUNCATED` for an image under 24 bytes; it doesn't check the magic
  (`fw_check_header` does). The checksum is the last byte of the 16-byte block after the segments (ESP-IDF's
  `process_checksum`: `round16(unpadded + 1) - 1`), and `hashed_len` is that block's end.
- Strings: the `FW_ERR_*` and `FW_CANCELLED` entries now; **`FW_UPDATED`, `FW_ROLLED_BACK` and `FW_BOOTLOOP` move to
  Task 7**, where Sessions toasts them, because `unused-str` flags an entry nothing uses yet (deviation from the
  plan's Task 1 file list).
- `FW_ERR_BATTERY` ("30 % or") parses as a `% o` printf conversion: `test_i18n`'s plain-percent list pins it
  (`"o"`), and it is only ever shown through `tr()` as a toast, never as a format.

## Task 2: the Host commands, the real-image check and the staging script

- `cc_cmd_fw_info` and `cc_cmd_fw_read` are spec §9.1 byte for byte (one `snprintf` each, `%%%%` for the shell's
  `%%`), in a new section at the end of `cc_cmd.c`/`cc_cmd.h`. `test_cc_cmd` checks the exact strings for the default
  path, every bad path of the plan, a cap one byte short (out cleared), `NULL`/0 caps, and exactly two `'`.
- `cc_cmd_print fwinfo|fwread <path>`; `fw_print check <bin> [chip_rev]` and `fw_print make-test-image <out>`.
  `fw_print` hashes with a small SHA-256 of its own (`test/host/sha256_tiny.c`, test tool only).
- `test_fw_cmd.py` ran both commands under `/bin/sh` and `/bin/zsh` with `PATH=/usr/bin:/bin` and a temp `HOME`: the
  info line equals Python's size and `hashlib.sha256`, the header is the file's first 288 bytes (all 100 for a
  100-byte file), the read is byte-exact for 8 MB, `~/fw.bin` works, exit 120 for a missing file, a directory and a
  mode 000 file, no file and no process left (including a reader that stops after 64 KB, as a cancel does: `cat`
  ends on SIGPIPE).
- The real-image tests ran on main's `firmware/build/claude_companion.bin` (7,340,544 B on this Host's build of
  80ab00e; the spec's 7,415,488 is another build of the same tree): header OK, layout OK, version `1.0`, Python's own
  walk agrees (checksum, appended SHA-256, ends exactly there). Refusals: a flip at `len // 2` (layout CHECKSUM),
  cut by 1,000 B (TRUNCATED), byte 80 to `X` (PROJECT), padded to 9,000,000 B (TOO_BIG), a 84 KB text file (MAGIC).
- `stage_update.sh` checks the magic, the descriptor magic and `claude_companion\0` at 80 with `od`, reads the
  version at 48, copies to `.tmp`, compares the copy's SHA-256 with the source's (a file being rebuilt is refused),
  then `mv -f`. `test_stage_update.py`: byte-equal, no `.tmp`, the printed line, the version from the descriptor,
  a text file / the bootloader's shape (no descriptor) / another project / no version / a missing file refused with
  exit 1 and nothing staged, and a replace while a reader loops: every read is the whole old or the whole new
  file, the inode changes and a reader holding the old file still reads it whole.

## Task 3: the layout, rollback, the image diet and the version

- `partitions.csv` is spec §6.4; `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, `CONFIG_FATFS_CODEPAGE_437=y` (the dynamic
  code page removed). `firmware/sdkconfig` was deleted before the build. The bootloader with rollback is 0x5920 B
  (0x6E0, 7 %, free before the partition table).
- `font_px20.c` deleted; `fonts.c` makes `s_px20` a RAM copy of `font_px24` (the role and its CJK fallback stay).
  `ui_boot.c` and the preview's boot case used `&font_px20` directly (the splash runs before `fonts_init`); both
  now name `font_px24`. `gen_pixel_fonts.sh` writes only `font_px24.c`.
- `PROJECT_VER`: `firmware/CMakeLists.txt` builds `<version.txt>+<hash>[.dirty]` from `git describe --always
  --dirty --abbrev=7` (handles both the tagless `<hash>[-dirty]` and the `<tag>-<n>-g<hash>` forms; `nogit` when git
  fails) before `project()`, unless `PROJECT_VER` is already defined (`-D PROJECT_VER=…`). This build:
  `1.6.0+91ff89d.dirty`.
- The size guard is a custom target `image_size_check` (ALL, depends on `app`) in the project `CMakeLists.txt`,
  not `add_custom_command(TARGET app POST_BUILD)`: `app` is created in ESP-IDF's `esptool_py` directory, and CMake
  only attaches a POST_BUILD command to a target of the current directory. It runs after every build and fails only
  when the image doesn't fit.

### Sizes (this Host's builds)

| | Before (main 80ab00e) | After (Task 3) | Saving |
|---|---|---|---|
| `claude_companion.bin` | 7,340,544 B | **6,291,360 B** | 1,049,184 B |
| `font_px20.c` | 567,531 B | gone | 567,531 B |
| `ffunicode.c` | (dynamic code page, ~480 KB) | 1,336 B | ~480 KB |
| headroom in the 8,323,072 B slot | 982,528 B (11.8 %) | **2,031,712 B (24 %)** | |

The plan expected about 6.38 MB from the spec's 7,415,488 B baseline; this Host's main build was already 7,340,544
B, and the diet saved slightly more than estimated. `idf.py size`: External RAM 6,143,416 B (`.rodata` 4,393,652,
`.text` 1,749,216); DIRAM 193,858 B used.

- Preview after the font change: `run_all.sh` passes (0 FAIL). Pixel diff of all 120 PNGs against the saved
  `t3-before` set: every difference is inside the status bar's clock (x ≥ 1244, y ≤ 38), plus the Display & time
  panel's clock row on `settings-4` (16:10 vs 16:16): the wall clock, which differs between two runs of the same
  build as well. No glyph pixel changed.
- Step 5 (the migration flash) is a device step: waiting for Task 9.

## Task 4: settings (CFG 25) and removing the SD updater path

- **CFG 25 is free on main** (`settings_migrate.h` was at 24, Plan 5), so this plan takes 25 as written. The comment
  now says Plan 2's Task 10 would take 26.
- `app_settings_t` gains `fw_path[64]` at the end. The step `old_version < 25` writes `CC_FW_PATH_DEFAULT` (over
  zeroes) and zeroes all 160 bytes of `ota_url` (reserved). `settings_fw_path_check` (in `settings_migrate.c`, so
  the host tests cover it, as `settings_voice_paths_check` is) NUL-terminates it and falls back unless
  `cc_path_valid`; `settings_init` calls it. `settings_defaults` sets it; `settings_reset_scope` keeps it.
- The migration tests that byte-compare a whole struct after loading an older blob now put back the v25 step's two
  fields (`undo_v25_step`), including the v20 and v23 tests whose compare ranges cover `ota_url` (it sits early in
  the struct). The reset-scope identity test now checks `fw_path` instead of `ota_url`. The newer-blob test uses 26.
- Removed: `firmware/updater/`, `main/ota/ota.c`, `ota.h`, `cmd_ota` (the `ota url` verb) in `power_console.c`,
  `esp_https_ota` from `PRIV_REQUIRES` (`esp_http_client`, `app_update`, `esp_partition`, `esp_app_format` stay),
  `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y`, the Mozgi512 `ota_url` clean-up in `settings_init`, the OTA sentence in
  `storage/sd.h`.
- **`url_mask`: deleted** (`sys/url_mask.c`, `.h`, `test/host/test_url_mask.c` and their CMake lines). After the
  removal, `grep -rn url_mask firmware/main` found no other caller.
- `merge_flash.sh`: no updater, no `update.json`; 0x2000, 0x8000, 0xF000, 0x20000 only; prints the NVS warning first.
- `cc_settings_fw_path` and `cc_link_set_fw_path` (validated, saves) next to the whisper ones in `cc_link.c`.
- `ui_lint.py`: `ota-outside-fw_update` (a call, in code with comments and literals blanked, to `esp_ota_*`,
  `esp_partition_erase_range` or `esp_partition_write*` outside `ota/fw_update.c`) and `fwcmd-outside-cc_cmd` (the
  three literals outside `cc_cmd.c`, `cc_cmd.h`, `i18n.h`). Before the removals they flagged `ota/ota.c` (4 calls)
  and `ota/ota.h` (`claude_companion.bin`); after, the tree is clean.
- The grep of Step 2 (`updater|factory|main_app|ota_install|ota_download|ALLOW_HTTP|esp_https_ota` over
  `*.c *.h *.txt *.csv *.defaults *.sh *.py`, build and managed_components left out) finds only: the
  `partitions.csv` comment ("No factory partition"), the old-layout fixture `main_app` in
  `test_check_image_size.py`, the `ota_url` reserved comment and the v25 step's comment, and unrelated uses
  (`microlink_factory_reset`, the Tab5's factory firmware in `hibernate.c`).
- `THIRD_PARTY_NOTICES.md` doesn't name the updater: unchanged.
- Step 3 (flash, `settings v24 -> v25` on the first boot, none on the second, snapshot S, `help` without `ota url`)
  is a device step: waiting for Task 9.
- FW build after Task 4 (sdkconfig deleted first): passes, image 6,290,784 B, headroom 2,032,288 B (24 %).

## Task 5: the link: firmware info and the binary read

- `req_t` gains `out_len`, `progress`, `cancel` and `direct`. `do_exec` with `direct` reads stdout straight into
  `out` (16 KB per `ssh_channel_read_timeout`, no 512-byte bounce, no NUL), writes `*progress` after each read,
  checks `*cancel` each loop, runs the watch keepalive every second, and once `cap` bytes are in, one more byte is an
  oversize (`ESP_ERR_INVALID_SIZE`). A cancel (`CC_FW_CANCELLED`) or an oversize sets `s_drop`, as a timeout and a
  channel error already did; a park returns `ESP_ERR_INVALID_STATE`. No exit status is read after a cut read.
- `cc_link_fw_info` uses the ordinary exec path with `out_len` (binary-safe: `exec_take` copies with `memcpy`);
  cap `CC_FW_INFO_MAX` 512, 10 s. Both functions build their command from `fw_path` (P5-2's rule), count as
  link-busy, and log only sizes, times, the rate and the exit status. `cc_link_fw_read` logs `fw read: direct` at
  the start (the plan's O1 check).
- `ota/fw_console.c` (`fw_console_init`, registered in `app_main.c` after `voice_console_init`): `ota check`,
  `ota path [<p>]`, debug `ota read-test [cancel-at <pct>]` (a helper task sets the cancel flag when the progress
  passes the percent). The chip revision for the header check is `efuse_hal_chip_revision()` (major × 100 + minor).
- Note for builders: the `main` glob is evaluated at configure time, so a new `.c` file needs `idf.py reconfigure`
  (the first build after adding `fw_console.c` failed to link until then).
- FW build passes: image 6,294,752 B. Steps 2–4 (check, read, cut-offs, Tailscale rate) are device steps: waiting
  for Task 9.

## Task 6: the update service, the boot check and the console verbs

- `ota/fw_update.c` follows spec §12.2 and §12.3. `_Static_assert`s pin every `FW_OFF_*` offset to
  `esp_image_header_t` / `esp_app_desc_t` (+ 32), and `sizeof(esp_app_desc_t) + 32 == FW_HDR_LEN`. At boot the other
  slot's size is compared with `FW_SLOT_SIZE`; a mismatch logs an error and `fwu_start` returns `FW_OUT_WRITE`.
- Checks in order: byte count (5), SHA-256 of the buffer in 64 KB steps with the cancel between them (6), the header
  again on the downloaded bytes (7), the layout walk plus the appended SHA-256 (8; an image without an appended hash
  is refused as damaged), then `esp_ota_begin` (logged `fw: writing <label>`), 16 KB writes with the cancel between
  them, `esp_ota_end` (9), the 16 KB read-back into one PSRAM block (10), `fw/attempt`, `esp_ota_set_boot_partition`
  (11). One `release()` frees both PSRAM blocks, aborts an open handle, ends the hold and clears busy (P6-15). The
  summary line and `fw: heapmin <N> B` (the internal minimum: the open `ui heapmin` window's, else since boot) are
  logged; then 1 s, then `link_down_restart("firmware update")`.
- **Deviation: `fwu_link_started()`.** `cc_poll_subscribe` does nothing before `cc_link_start` (no lock yet), and
  `fwu_boot_init` runs before Wi-Fi, so a pending image subscribes from a second call, made in `app_main.c` right
  after `cc_link_start`. With it there are 4 subscribers (Sessions, hibernate, power, this): exactly `MAX_SUBS`.
- **Deviation: `FWU_NOTE_ROLLING_BACK`.** The 3 s notice ("can't reach the Host. Going back to …") is a boot note
  kind of its own. The 180 s timer is one `esp_timer`: it fires at 177 s of uptime, posts the notice and re-arms for
  the remainder; at 180 s `fw_validity` says ROLLBACK and it calls
  `esp_ota_mark_app_invalid_rollback_and_reboot()`.
- **Addition: `fwu_take_boot_note`.** Sessions' tick takes only boot notes, so it can't take a RESULT that belongs
  to the progress screen or the console.
- `fwu_previous` treats "no otadata entry" (`esp_ota_get_state_partition` fails: a slot written by a wired flash)
  like UNDEFINED, so after the first wireless update the USB-flashed `app0` is offered as Previous version and is a
  boot-loop target. The old updater's bytes left in `app1` by the migration aren't, because their descriptor's
  project isn't `claude_companion`.
- The boot-loop guard: an `RTC_NOINIT` record with a magic (reset on a bad magic or a power-on reset), the count, the
  early flag (cleared with the count by a 60 s timer), a note flag and the test counter. A trip switches with
  `esp_image_verify` + `esp_ota_set_boot_partition` and calls `esp_restart()` directly: it runs before
  `link_down_init` and Wi-Fi, so no SSH session exists to close. The next boot posts `FWU_NOTE_BOOTLOOP`.
- Gates: `session_open` is the TAKEOVER or SSH hold's bit (`power_hold_is_on`, new), `voice_active` is
  `voice_status().phase != VOICE_IDLE`, the source and percent from `power_source` (so `power sim battery` drives it),
  memory from `heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)`. `voice_start` returns `VR_OUT_BUSY` while
  `fwu_busy()`. `POWER_HOLD_UPDATE` counts in `holds_active` and prints `update`.
- `ota update` returns at once and a printer task prints the phases, every 10 % and the end, so `ota cancel` can be
  typed on the next console line (the REPL runs one command at a time).
- `main/CMakeLists.txt` gains `bootloader_support` (for `esp_image_verify`).
- Builds: the default build passes (6,307,872 B); a scratch build with `CONFIG_COMPANION_DEBUG_COMMANDS=y`,
  `CONFIG_COMPANION_OTA_TEST_NO_VALIDATE=y` and `-D PROJECT_VER=1.6.0+novalid` passes (6,335,648 B, version
  `1.6.0+novalid`: the command-line `PROJECT_VER` wins). Steps 2–7 are device steps: waiting for Task 9.

## Task 7: the UI

- New files, to keep out of the sections other branches touch: `ui/ui_settings_fw.c` (the About rows, the Host
  path row, both confirms, the worker job bodies, the boot-note toasts) and `ui/ui_fwupdate.c` (the progress
  screen). `ui_settings.c` gains two calls; `ui_sessions.c` gains the two job types, two dispatch lines,
  `ui_sessions_fw_check` / `ui_sessions_fw_start` and one line in the 300 ms tick.
- **Deviation: no About poll timer.** The worker hands the check's result to the LVGL task with `ui_async`, as the
  voice check does, so About has no timer at all (the preview asserts no 300 ms timer on About). The pending flags
  live in `ui_settings_fw.c`; a second activation while a check runs does nothing.
- **Addition: `ui_modal_layer()`** in `ui_core.c` (the existing dialog layer, made public), so the progress screen
  owns every key and blocks swipes like any dialog. Esc reaches it as `LV_EVENT_CANCEL`; Cancel carries
  `UI_KEYBOARD_SKIP`, so the modal has no keyboard control and Enter, text and Ctrl+P are eaten. The confirms give
  Install / Go back `UI_KEYBOARD_FIRST`, so the first Enter installs and Esc cancels.
- The progress screen's 300 ms timer is bound to the modal and goes with it; `checks.c`'s timer rule excepts it
  while it shows (`ui_fwupdate_timer()`), as it excepts the voice meter.
- The error value is the copy's first sentence, cut with dots inside the row by the value label's existing DOTS
  mode. The default firmware path (52 characters) is also cut with dots in its row; the prompt shows it whole.
- Preview: new cases `settings 5` × `PREVIEW_FW_CHECK` (7 states), `PREVIEW_FW_PREV=1`, the Install confirm (newer,
  older), the Go back confirm, `PREVIEW_CLICK=Firmware file on Host` on `settings 0`, `fwupdate` × 5 phases plus 0 %
  and 100 %, and `fw-notes`. `settings 0` now also asserts Firmware file on Host follows Voice input.
  `run_all.sh`: every case passed (155 checks).
- Strings: the remaining `SET_FW_*` and `FW_*` entries and `SET_HOST_NOTE` as spec §8.6; `test_i18n` pins the
  conversions of the ten format strings.
- Steps 3–6 (touch, keyboard, Go back, gates) are device steps: waiting for Task 9.

## Task 8: the docs

- `README.md`: §1 "Flash and watch the log" without the updater sentence, and a new "Update over Wi-Fi" (the
  migration with its must / must-not lists and the `app-flash` and merged-image warnings, staging with
  `stage_update.sh` and the alias with the placeholder `~/src/claude-companion`, installing, what happens on a
  failure, when USB is still needed); §4 one line; §5 the Host and About rows; §9 `ota`, `ota check`, `ota path`, the
  debug verbs, `ota url` gone; §10 six troubleshooting entries; §12 the update bullet; Status, Known limitations and
  the Roadmap (6 Wireless updates, 7 Camera, 8 Release).
- **Deviation:** the Roadmap row says "built; device gates pending", not "done", and Status says the device gates
  are pending, because Task 9 hasn't run. Task 9 changes both to done.
- `firmware/README.md`: the layout (`ota/` contents, `updater/` gone, the new tools, `version.txt`), the partitions
  line, the Build paragraph (migration flash, version, size guard, CP437, rollback), Release images rewritten with
  the NVS warning (and no updater row), "OTA (dormant)" replaced by "Firmware updates", the two firmware commands in
  the Host link section, the microSD list without the OTA path, the debug verbs, the fonts paragraph.
- The main spec's roadmap gains Plan 6 and renumbers camera (7) and release (8); its command table gains the two
  firmware commands. `THIRD_PARTY_NOTICES.md` names no updater: unchanged.
- Checks: `grep -n "updater\|factory\|ota url\|dormant\|0xF80000" README.md firmware/README.md` finds nothing;
  every `merge_flash` / `full_flash` / `write_flash … 0x0` mention sits next to the NVS warning; no "Mac" was added
  for the Host; placeholders only. The README's staging line ran locally with `STAGE_DIR` in the scratchpad:
  `staged 1.6.0+66fe269.dirty, 6322736 B, sha256 39f50fbd2909…`, one file in the directory, removed after.

## End of Tasks 1–8 (host side)

- Final checks on 5ab222d: `firmware/sdkconfig` deleted, `idf.py reconfigure build` passes; image **6,322,736 B**
  (`1.6.0+5ab222d`), headroom 2,000,336 B (24 %) in the 8,323,072 B slot; bootloader 22,816 B; `idf.py size`: DIRAM
  195,254 B used (+1,396 B static internal over the Task 3 build), External RAM 6,174,592 B. Host tests: 25 of 25
  CTest targets pass (fresh build dir). Python: 94 tests pass, none skipped (the real-image tests ran on this
  build). Lint: clean. Preview `run_all.sh`: every case passed.
- Nothing was flashed and the Tab5, the serial port and the Host's SSH were not touched.

## Device steps waiting for Task 9

Every device step of Tasks 3–7, plus Task 9 itself. Before the first one, the Global Constraints' recovery rule
applies (a good build of this branch flashed over USB in the same session, the cable left in).

1. **Task 3 Step 5, the migration flash:** snapshot S before; `esptool read-flash 0x9000 0x6000` to the scratchpad
   (`chmod 600`); `idf.py -p PORT flash` (log shows bootloader, partition table, `ota_data_initial.bin`, app; nothing
   at 0x9000); boot log shows `app0` and `1.6.0+…`; read back and decode the partition table (five rows of spec
   §6.4); snapshot S after equals before; Wi-Fi joins, the link connects with the same pin, sessions list.
2. **Task 4 Step 3:** first boot logs `settings v24 -> v25`, the second none; snapshot S matches; `help` has no
   `ota url`.
3. **Task 5 Steps 2–4:** `ota check` against a scratch stage dir (rc 0, size and SHA-256 equal to
   `stage_update.sh`'s line, `FW_HDR_OK`); `ota path /nope/x.bin` → rc 120; `ota path "/a b"` refused; `ota
   read-test` ×3 with the watch on (LAN KB/s, O1; `fw read: direct` logged); `cancel-at 40`, a park
   (`power go radio-off`), `wifi test drop`, one read over Tailscale; restore the default path.
4. **Task 6 Steps 2–7:** the first wireless update (build B with `-D PROJECT_VER=1.6.0+testb`, `app0` → `app1`,
   per-phase times, marked valid, `ota status`, snapshot S); the power hold (`power sim battery 50`, `power sim idle
   300` during download, write and pending; 25 % refused); refusals before any write (byte flipped, `corrupt-ram`,
   9 MB, the bootloader, no file: no `fw: writing` line); the no-validate image (debug +
   `CONFIG_COMPANION_OTA_TEST_NO_VALIDATE`, `-D PROJECT_VER=1.6.0+novalid`): reset while pending, then the 177 s
   notice and the 180 s rollback; `ota cancel` during the download and the write; `wifi test drop` during the
   download and the write.
5. **Task 7 Steps 3–6:** by touch (the path by the soft keyboard, About, Install, screenshots of the row, the
   download and the write, the Updated toast); by keyboard (Ctrl+P … Enter, Enter; Esc mid-download; then a full
   update); Go back both ways; the UI gates (take-over, `voice test-bg 10`, `power sim battery 20`).
6. **Task 9 Steps 0–12:** as written in the plan (sizes, both slots by touch and keyboard, every refusal through
   the UI, both rollbacks, the boot-loop guard with `ota test-bootloop 3` and `2`, the power hold, Wi-Fi drops,
   `ui heapmin` with `fw: heapmin`, NVS snapshots after each, nothing left on the Host, clean-up and the visit
   list), then the Roadmap and Status lines in `README.md` and the main spec go from "device gates pending" to done.

Questions for the user's visit (no device needed to list them): whether the About rows and the progress screen read
well at arm's length; whether 30 % is the right battery floor; whether the default staging path and the
`tab5-build` alias suit the workflow.

## Fix round 1 (core and UI reviews)

- Merged `main` (stop-hotkey, busy-pane) first: conflicts only in the `ui_sessions.c` job enum (both sets kept:
  `JOB_STOP, JOB_FW_CHECK, JOB_FW_START`, now also `JOB_FW_GO_BACK`) and `test_i18n.c`'s format list (both blocks).
- Every finding and its fix is listed in spec §17. Points for the builder:
  - **C1.** The `#error`s were checked with a scratch build whose defaults turned rollback off and the code page
    back to dynamic: both fired, with the "delete firmware/sdkconfig" message. The runtime "Rollback off" detection
    is the NEW-state heuristic: it catches a bootloader without rollback the first time it boots an updated image,
    not before (a wired flash leaves no otadata entry to read). The slot-size check already turns updates off on the
    old layout.
  - **Core I1 / M7.** The trusted mark-valid runs in `fwu_boot_early`, the first call in `app_main`, so a target that
    crashes in early init before being marked valid still goes back by the bootloader (a narrow case). Tests:
    `test_boot_state`, `test_bootloop_action` (12 simulated boots: one switch, then give-up; a stable boot resets).
  - **M4.** Watchdog choice: count `ESP_RST_WDT` and `ESP_RST_CPU_LOCKUP` too; `CONFIG_ESP_TASK_WDT_PANIC` stays off
    because the idle-task checks are off and no task subscribes, so it would change nothing.
  - **M13.** The bootloader with rollback is 22,816 B: 1,760 B spare below the partition table (firmware/README).
  - **M11.** `MAX_SUBS` is now 6 (`CC_POLL_MAX_SUBS`), 4 used; `ota status` prints the count.
  - Two new toasts were shortened to fit the one-line toast (it cuts with dots): `FW_ERR_NO_ROLLBACK` and
    `FW_ROLLBACK_FAILED` (spec §17 has the final copy).

### New device steps for Task 9 (in addition to the list above)

- `ota status` prints `updates on` after the migration flash (with `rm -f firmware/sdkconfig` first).
- The guard with the Host unreachable: on a validated B with A in the other slot, Wi-Fi off (`wifi leave`), then
  `ota test-bootloop 3`: the fourth boot is A, marked valid at once (log: "switched to by the boot-loop guard or Go
  back, marked valid"), and it stays A after 180 s with no Host.
- The ping-pong cap: `ota test-bootloop 3` right after a guard switch (within 60 s of the boot): the guard gives up
  ("Both versions keep restarting…") instead of switching back.
- Go back with the Host unreachable: the old build is kept (no 180 s rollback, no "Updated to" toast).
- `ota update` then, during the read-back, `ota cancel`: refused with "Too late to cancel…" only from RESTARTING; a
  cancel during the write erases `app1`'s header (log: "header erased after a failed update"), and About shows no
  Previous version.
- Install while a new image is still pending: "The new version is still checking in with the Host…".

## Fix round 2 (re-review)

- Spec §18 lists the changes (N1–N9). New pure rules with tests: `fw_pending_claim` (two claimants, one winner, both
  orders), `fw_go_back_mode`, `fw_switch_trusted`, `fw_previous_ok` (an entry-less complete slot is never offered
  and never trusted), `fw_boot_state(pending, flag, target_match)`.
- `rtc_rec_t` gained the target address, so its magic is now `FWU3` (an older record reads as garbage and resets).
- Preview: the reviewer's 13 failures were not reproduced; `run_all.sh` from a fresh build directory passes (see the
  results below in the report). New case `PREVIEW_FW_LAYOUT=0`.

### More device steps for Task 9

- Go back at about 175 s of a pending image with the Host unreachable: the Tab5 rolls back to the old build, which
  stays (no probation), and `ota status` shows the new slot invalid or aborted and the old one valid.
- A cut (power off) during the read-back of an update: the old build boots and About shows no Previous version.
- `ota status` after the migration and after each update: the `fw/ok_<slot>` marker for the running slot is written
  (Previous version appears for the other slot only once its build has run valid).
