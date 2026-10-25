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
