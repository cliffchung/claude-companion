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
