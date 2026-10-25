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
