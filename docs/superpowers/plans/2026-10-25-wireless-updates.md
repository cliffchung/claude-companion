# Wireless Firmware Updates Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After one wired flash, new builds reach the Tab5 over Wi-Fi. Settings › About › Update firmware checks a staged build on the Host, and after a confirm the Tab5 pulls it over the existing SSH link into PSRAM, verifies it, writes it to the other app slot, restarts, and keeps it only once it has reached the Host; otherwise the bootloader goes back to the previous build.

**Architecture:**
- Pure parts first, host-tested: `ota/fw_rules.c` (info parse, image header checks, the image layout walk, version compare and offer, progress maths, gates, validity and boot-loop rules, outcome mapping) and the two Host command builders (`cc_cmd_fw_info`, `cc_cmd_fw_read`).
- The build changes next: two 0x7F0000 OTA slots, `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, the image diet (one 24 px font copy, FATFS code page 437), `<version.txt>+<git hash>` versions, a size guard. This is the one wired migration flash.
- One new link request type: a binary exec read with progress and cancel (`cc_link_fw_read`), plus `cc_link_fw_info`. Both build their command from the setting.
- `ota/fw_update.c` owns the whole update: PSRAM buffer, SHA-256, `esp_ota_*`, the boot-time validity check, the 180 s rollback timer, the boot-loop guard and Go back. It is the only file that touches `esp_ota_*`.
- Settings › About gets Update firmware and Previous version; Settings › Host gets the firmware path; a modal progress screen; the `ota` console verbs.
- The SD-card updater path from Clientre 5 is deleted.

**Tech Stack:** ESP-IDF v5.5.6 (`app_update`/`esp_ota_ops`, `esp_partition`, bootloader rollback), mbedTLS hardware SHA-256, LVGL 9.4, libssh 0.11, Unity host tests (cmake/ctest), the host UI preview, Python unittest. On the Host: `/bin/sh`, `wc`, `shasum` (macOS) or `sha256sum` (Linux), `head`, `cat`.

**Spec:** docs/superpowers/specs/2026-10-25-wireless-updates-design.md. Its §16 indexes the rulings; §9.1 has the exact Host commands; §11 is the migration.

## Global Constraints

**Hard project rules (verbatim from the Plan 4 spec §3, unchanged):**
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
- **Flash safety.** Only `ota/fw_update.c` calls `esp_ota_*`, `esp_partition_erase_range` or `esp_partition_write` on an app partition (lint rule P6-10). Nothing writes the running slot, the bootloader, the partition table, otadata (other than through `esp_ota_*`) or NVS (other than the `fw/attempt` key and the settings blob).
- **Wired flashes are `idf.py -p PORT flash` only.** Never `erase-flash`, `erase-region`, `erase-otadata`, `app-flash` for the migration, the merged full image, `espefuse.py`, or anything aimed at the C6. Reading flash (`esptool read-flash`) is allowed; its output goes to the scratchpad only.
- **A bricking test is recoverable by USB.** Before any test that installs a deliberately bad image, the same session has just flashed this branch's good build over USB successfully, and the USB cable stays connected. Recovery is `idf.py -p PORT flash` from that good build (it keeps NVS). If a device doesn't come back after that, stop and ask the user; never erase.
- **The Host commands are exactly spec §9.1.** Non-pty exec channels. Nothing written or left running on the Host.
- **Nothing is installed on the Host by the build or the tests.** Device tests may run `stage_update.sh`, `python3` (to flip bytes in a scratch copy of a build) and `pgrep` **locally** on the Host in a scratch directory; nothing is run on the Host over SSH by hand.
- **Never install without a confirm** in the UI path. Only the debug `ota update` verb skips it.
- Test only against Host sessions you create, named `companion-test-*`. Start one with `~/.local/bin/claude --bg --name companion-test-<x> "Ask me what my favorite color is, then wait for my answer."` and stop it when the task is done (`~/.local/bin/claude stop <id>`).
- Leave the Tailscale switch as you found it (off by default). A task that runs `ts on` for a gate runs `ts off` before it ends.
- Never change sshd config or `~/.ssh/authorized_keys`.
- **Staging during tests uses a scratch directory**, never the user's real staging path: `ota path <scratch>/stage/claude_companion.bin` (≤ 63 characters; use a short scratch path such as `/tmp/cc6/stage/...` created with `mktemp -d /tmp/cc6.XXXX` if the scratchpad path is too long), and `STAGE_DIR=<that dir> sh firmware/tools/stage_update.sh`. Each task that changes the path restores the default (`ota path ~/.local/share/claude-companion/claude_companion.bin`) before it ends.

**Project facts:**
- **Base:** main at 80ab00e (Plan 5 merged, plus the touch-voice follow-up). Work in the worktree `/Users/you/workspace/claude-companion-plan6` on branch `plan6-updates`; the top-level checkout stays on main.
- **Settings blob:** `app_settings_t` is one NVS blob. New fields go at the end. **This plan takes CFG 25**; CFG 22 stays reserved for Plan 2's Task 10, which would now take 26 (update the comment in `settings_migrate.h`). Steps are keyed `old_version < N`.
- **Partitions after Task 3:** `nvs` 0x9000/0x6000, `otadata` 0xF000/0x2000, `phy_init` 0x11000/0x1000, `app0` (ota_0) 0x20000/0x7F0000, `app1` (ota_1) 0x810000/0x7F0000. `FW_SLOT_SIZE` 8,323,072.
- **Image:** 7,415,488 B on main; about 6.38 MB expected after Task 3. Its layout (checked offline on main's build): magic `0xE9`, 9 segments, chip id 18, revisions 0–199, the descriptor at 32 (version at 48, project at 80, ELF SHA-256 at 176), checksum byte, appended SHA-256, no padding.
- **Internal RAM:** LVGL allocations of 512 B or less land in internal RAM; use the shared static styles from `ui_comp`. The update task's stack (6 KB) is internal and exists only during an update.
- **Timers (Plan 4 B1):** no presentation timer under 300 ms. The progress screen's timer is 300 ms.
- **Strings:** every user-visible string is a `tr(STR_*)` entry in `firmware/main/i18n/i18n.h`, copied exactly from spec §8.6. Say "Host", never "Mac".
- **Defaults:** `CC_FW_PATH_DEFAULT "~/.local/share/claude-companion/claude_companion.bin"` in `cc_cmd.h` only.

**Commands** (`PV`, `SHOTS` and `SCRATCH` are directories in your scratchpad; `PORT` is the Tab5's port from `ls /dev/cu.usbmodem*`):
- **Firmware:** `. ~/esp/esp-idf/export.sh && cd firmware && idf.py build`. After editing `sdkconfig.defaults` or `partitions.csv`, delete `firmware/sdkconfig` before building. Size: `idf.py size > $SHOTS/size-<task>.txt`.
- **A second build with test options** (never edit shared files for an experiment): `idf.py -B $SCRATCH/build-<name> -D SDKCONFIG=$SCRATCH/sdkconfig.<name> -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;$SCRATCH/<name>.defaults" build`, where `<name>.defaults` holds only the extra lines.
- **Flash (P4 only):** `idf.py -p PORT flash`. It writes the bootloader, the partition table, the initial otadata and the app, and keeps NVS.
- **Host tests (HOST):** `export IDF_PATH=~/esp/esp-idf && cmake -S firmware/test/host -B firmware/test/host/build && cmake --build firmware/test/host/build && ctest --test-dir firmware/test/host/build --output-on-failure`.
- **Python tests (PY):** `python3 -m unittest discover -s firmware/tools -p 'test_*.py' -v` (build HOST first: `test_fw_cmd.py` uses `cc_cmd_print` and `fw_print`).
- **Lint:** `python3 firmware/tools/ui_lint.py firmware/main` prints `[]`.
- **Preview (PV):** `cmake -S firmware/tools/ui_preview -B $PV -G Ninja && cmake --build $PV -j 8`, then `cd $PV && ./preview <case> [variant]`; `firmware/tools/ui_preview/run_all.sh $PV` runs every case.
- **Console:** the serial helper `T5` from Plan 4 (`python3 $T5 "cmd" …`, a copy in your scratchpad). Screenshots: `python3 firmware/tools/screenshot.py --port PORT --out $SHOTS/<name>.jpg`. Look at every screenshot you take. The console survives a restart: reconnect after `ota update` and read the boot log.
- **RAM gate:** `ui heapmin start`, the action, `ui heapmin stop` (`heapmin: internal min <N> B, largest <L> B, now <F> B`).
- **NVS snapshot (S):** on the console, `sshkey`, `companion status`, `wifi status`, `ts`, `power settings`, `ota path`, `companion claude-path`, and the Terminal host list (`ui hosts` screenshot, or the list verb if one exists), saved to `$SHOTS/nvs-<label>.txt`. Two snapshots match when every line except uptimes, signal levels and timestamps is equal.
- **Notes:** each task appends its measurements and rulings to `docs/superpowers/notes/2026-10-25-wireless-updates-notes.md` (Task 1 creates it). Use placeholders (`my-host.local`, `you`), never the real host name, addresses, user name or session names.

## Review Focus

1. **A bad build strands the Tab5.** An image that crashes at boot, hangs, can't join Wi-Fi, can't pass the host-key pin, or reaches the Host and then crash-loops. Expected: before it is marked valid, any reset or the 180 s timer goes back to the previous slot; after, three early crashes go back; About › Previous version works while the UI does; USB is needed only when both slots are bad. Tests: Task 1 `test_validity_*`, `test_bootloop_*`; Task 6 steps 5–6; Task 9 steps 4–5.
2. **A wrong or damaged image gets written or installed.** A flipped byte on the Host, a file still being written, a different project's `.bin`, the bootloader, a text file, an image for another chip, a 9 MB file, a file replaced mid-download. Expected: refused before the first flash write, with the right copy; `ota status` unchanged; the running slot never touched. Tests: Task 1 `test_header_*`, `test_layout_*`, `test_info_*`; Task 2 `test_real_image_*`; Task 6 step 4; Task 9 step 3.
3. **A cut-off update leaves the Tab5 worse.** Wi-Fi drops, the Host sleeps, Radio off parks the link, Esc, a power loss, the screen dimming or a Hibernate during the download, the write or the pending check. Expected: nothing switches until every check has passed; a cut before the switch leaves the old version running; the hold keeps the screen Active from the confirm through the pending check; a drop during the write doesn't matter. Tests: Task 1 `test_gate_*`; Task 5 step 4; Task 6 steps 3 and 7; Task 9 steps 6–7.
4. **User data is lost.** The migration flash, an update, a rollback or Go back erases or rewrites NVS, or a downgrade loses settings. The merged full image used by mistake. Expected: every NVS snapshot equal; `fw_path` and the other Host paths kept; the README warns about the merged image and `app-flash`. Tests: Task 3 step 5 (the migration); Task 4 `test_migrate_v24_to_v25_*`; Task 9 step 9; Task 8 step 2.
5. **Host command safety.** A firmware path with a space, `'`, `;`, `$`, a backtick or a newline; a pty channel; a process or file left on the Host; the session left odd after a cancelled read. Expected: refused by `cc_path_valid`; plain exec channels; nothing left; the link reconnects and the watch resumes after a cancel or drop. Tests: Task 2 `test_cmd_fw_*_rejects`, `test_no_process_left`, `test_no_files_left`; Task 5 steps 3–4; Task 9 step 10.

## Plan rulings (gaps the spec leaves open)

- **P6-10, lint.** `ui_lint.py` gains `ota-outside-fw_update` (any `esp_ota_`, `esp_partition_erase_range` or `esp_partition_write` call under `firmware/main` outside `ota/fw_update.c`) and `fwcmd-outside-cc_cmd` (the literals `claude_companion.bin`, `head -c 288` or `exec cat` outside `companion/cc_cmd.c`, `cc_cmd.h` and `i18n/i18n.h`).
- **P6-15, one buffer, one free.** `fw_update.c` has one `release()` helper that zeroes nothing (the image is not secret) but frees the PSRAM buffer, ends the hold and aborts an open OTA handle; every exit of the task goes through it.
- **P6-16, the check job.** The About check and the Install press post jobs to the Sessions worker (as P5-5's voice Check), so no link call ever runs on the LVGL task. `ui_sessions_fw_check()` returns false when a job is already pending.
- **P6-17, outcome delivery.** `fw_update.c` keeps one outcome slot with a generation counter (as voice's P5-3). The progress screen polls it; on its close the About panel toasts it. Boot notes (`FW_UPDATED`, `FW_ROLLED_BACK`, `FW_BOOTLOOP`) sit in the same slot until Sessions' 300 ms tick toasts them once.
- **P6-18, `fw/attempt`.** NVS namespace `fw`, key `attempt`, the new version string, written just before `esp_ota_set_boot_partition`, cleared when the new image marks itself valid or when the old image reports the rollback.
- **P6-19, no auto-check outside About.** No timer and no boot path runs the check. Only About and the console do.

---

### Task 1: `fw_rules`, the pure update logic

**Files:**
- Create: `firmware/main/ota/fw_rules.h`, `fw_rules.c` (no ESP-IDF or LVGL headers; `i18n/i18n.h` and `sys/power_rules.h` only).
- Create: `firmware/test/host/test_fw_rules.c`.
- Modify: `firmware/test/host/CMakeLists.txt`: add `ota/fw_rules.c` to `pure` and the test executable.
- Modify: `firmware/main/i18n/i18n.h`: the `FW_ERR_*`, `FW_CANCELLED`, `FW_UPDATED`, `FW_ROLLED_BACK`, `FW_BOOTLOOP` strings (copy from spec §8.6).
- Create: `docs/superpowers/notes/2026-10-25-wireless-updates-notes.md` (header plus "Task 1").

**Interfaces:**
- Produces:
```c
#define FW_HDR_LEN          288          /* image header 24 + first segment header 8 + app descriptor 256 */
#define FW_IMG_MAGIC        0xE9
#define FW_DESC_MAGIC       0xABCD5432u
#define FW_CHIP_ESP32P4     0x0012
#define FW_PROJECT          "claude_companion"
#define FW_SLOT_SIZE        0x7F0000u    /* == app0/app1 size in partitions.csv (Task 3 asserts it) */
#define FW_MIN_SIZE         (64u * 1024u)
#define FW_MAX_SEGMENTS     16
#define FW_BATTERY_FLOOR    30
#define FW_PSRAM_MARGIN     (1024u * 1024u)
#define FW_VALIDATE_MS      180000
#define FW_BOOTLOOP_N       3
#define FW_BOOTLOOP_EARLY_MS 60000
#define FW_CHECK_TTL_MS     60000
typedef struct {
    char version[33], project[33], date[17], time[17], idf[33];
    uint8_t elf_sha[32];
    uint16_t chip_id, min_rev, max_rev;          /* full revisions: 100 = v1.0 */
    uint8_t segments;
    bool hash_appended;
} fw_desc_t;
typedef enum { FW_HDR_OK, FW_HDR_SHORT, FW_HDR_MAGIC, FW_HDR_SEGMENTS, FW_HDR_CHIP, FW_HDR_CHIP_REV,
               FW_HDR_DESC_MAGIC, FW_HDR_PROJECT, FW_HDR_NO_VERSION, FW_HDR_TOO_SMALL, FW_HDR_TOO_BIG } fw_hdr_t;
/* b: the first n bytes of an image of image_size bytes. Checks in this order: SHORT (n < FW_HDR_LEN and
   n < image_size), TOO_SMALL, TOO_BIG (> slot_size), MAGIC, SEGMENTS (0 or > FW_MAX_SEGMENTS), CHIP,
   CHIP_REV (chip_rev outside min..max), DESC_MAGIC, PROJECT (!= FW_PROJECT), NO_VERSION (empty or not
   NUL-terminated within 32). out (may be NULL) is filled on OK, NUL-terminating every string. */
fw_hdr_t fw_check_header(const uint8_t *b, size_t n, uint32_t image_size, uint32_t slot_size,
                         uint16_t chip_rev, fw_desc_t *out);
typedef struct { uint32_t hashed_len; uint8_t checksum_calc, checksum_stored; bool hash_appended; } fw_layout_t;
typedef enum { FW_LAYOUT_OK, FW_LAYOUT_TRUNCATED, FW_LAYOUT_SEGMENTS, FW_LAYOUT_CHECKSUM,
               FW_LAYOUT_TRAILING } fw_layout_res_t;
/* Walks header (24), each segment (8 + data_len, inside len), pads to the 16-byte boundary where the XOR
   checksum (seed 0xEF, over every segment's data) sits, then the 32-byte SHA-256 when hash_appended. The
   walk must end exactly at len (TRAILING otherwise). The caller hashes img[0, hashed_len) and compares it
   with img[hashed_len, +32). */
fw_layout_res_t fw_image_layout(const uint8_t *img, size_t len, fw_layout_t *out);
/* "<size> <64 lowercase hex>\n" then min(288, size) bytes. False for anything else. */
bool fw_parse_info(const uint8_t *buf, size_t len, uint32_t *size, char sha_hex[65],
                   const uint8_t **hdr, size_t *hdr_len);
typedef enum { FW_VER_OLDER = -1, FW_VER_SAME = 0, FW_VER_NEWER = 1, FW_VER_UNKNOWN = 2 } fw_ver_t;
/* host relative to running. "v"/"V" prefix allowed; MAJOR[.MINOR[.PATCH]][-pre][+build]; missing parts are
   0; the build part is ignored; a pre-release is lower than the same release; pre-releases compare as
   strings. Anything else (a bare git hash) is UNKNOWN. */
fw_ver_t fw_version_cmp(const char *host, const char *running);
typedef enum { FW_OFFER_UP_TO_DATE, FW_OFFER_NEWER, FW_OFFER_OLDER, FW_OFFER_OTHER_BUILD } fw_offer_t;
/* same ELF SHA-256 -> UP_TO_DATE; else NEWER / OLDER by fw_version_cmp; SAME or UNKNOWN -> OTHER_BUILD */
fw_offer_t fw_offer(const fw_desc_t *host, const char *run_version, const uint8_t run_elf_sha[32]);
int  fw_pct(uint64_t done, uint64_t total);       /* floor; 0 for total 0; 99 until done >= total, then 100 */
/* "3.2 of 6.4 MB" (MB = 10^6, one decimal, rounded half up); "0.0 of 0.0 MB" for zeros. */
void fw_mb_text(uint64_t done, uint64_t total, char *out, size_t cap);
typedef struct {
    bool link_up, session_open, voice_active, updating;
    pr_src_t src; int battery_pct;
    size_t psram_largest; uint32_t image_size;
} fw_gate_in_t;
typedef enum { FW_GATE_OK, FW_GATE_BUSY, FW_GATE_NO_LINK, FW_GATE_SESSION, FW_GATE_VOICE,
               FW_GATE_BATTERY_UNKNOWN, FW_GATE_BATTERY, FW_GATE_MEMORY } fw_gate_t;
fw_gate_t fw_gate(const fw_gate_in_t *in);       /* first failing in the enum's order */
typedef enum { FW_V_NONE, FW_V_WAIT, FW_V_MARK_VALID, FW_V_ROLLBACK } fw_validity_t;
/* not pending -> NONE; host_ok -> MARK_VALID; uptime >= FW_VALIDATE_MS -> ROLLBACK; else WAIT */
fw_validity_t fw_validity(bool pending, bool host_ok, int64_t uptime_ms);
/* The count after this boot: crash_reset && was_early -> count + 1, else 0. */
int  fw_bootloop_next(int count, bool crash_reset, bool was_early);
bool fw_bootloop_trip(int count, bool other_runnable);   /* count >= FW_BOOTLOOP_N && other_runnable */
typedef enum { FW_OUT_OK, FW_OUT_CANCELLED, FW_OUT_NO_LINK, FW_OUT_LINK_LOST, FW_OUT_TIMEOUT, FW_OUT_NO_FILE,
               FW_OUT_HOST_READ, FW_OUT_NOT_APP, FW_OUT_TOO_BIG, FW_OUT_CORRUPT, FW_OUT_CHANGED, FW_OUT_WRITE,
               FW_OUT_BATTERY, FW_OUT_BATTERY_UNKNOWN, FW_OUT_SESSION, FW_OUT_VOICE, FW_OUT_BUSY,
               FW_OUT_NO_MEMORY } fw_out_t;
fw_out_t fw_host_rc(int rc);                     /* 0 OK; 120 NO_FILE; 121, 122, 126, 127 and others HOST_READ */
fw_out_t fw_hdr_out(fw_hdr_t h);                 /* TOO_BIG -> TOO_BIG; SHORT -> CHANGED; the rest -> NOT_APP */
fw_out_t fw_gate_out(fw_gate_t g);
str_id_t fw_out_text(fw_out_t o);                /* STR_COUNT for OK */
```
- `fw_out_text` maps: CANCELLED `FW_CANCELLED`, NO_LINK `SES_NOT_CONNECTED`, LINK_LOST `FW_ERR_LINK`, TIMEOUT `FW_ERR_TIMEOUT`, NO_FILE `FW_ERR_NO_FILE`, HOST_READ `FW_ERR_HOST_READ`, NOT_APP `FW_ERR_NOT_APP`, TOO_BIG `FW_ERR_TOO_BIG`, CORRUPT `FW_ERR_CORRUPT`, CHANGED `FW_ERR_CHANGED`, WRITE `FW_ERR_WRITE`, BATTERY `FW_ERR_BATTERY`, BATTERY_UNKNOWN `FW_ERR_BATTERY_UNKNOWN`, SESSION `FW_ERR_BUSY_SESSION`, VOICE `FW_ERR_BUSY_VOICE`, BUSY `FW_ERR_BUSY`, NO_MEMORY `FW_ERR_NO_MEMORY`.
- Header byte offsets used (and asserted against `esp_image_header_t`/`esp_app_desc_t` with `_Static_assert(offsetof(...))` in `fw_update.c`, Task 6): magic 0, segment count 1, chip id 12 (u16), min rev full 15 (u16), max rev full 17 (u16), hash appended 23; descriptor magic 32, version 48 (32), project 80 (32), time 112 (16), date 128 (16), idf 144 (32), ELF SHA-256 176 (32).

- [ ] **Step 1: Write the failing tests** in `test_fw_rules.c`. A helper `make_image(segments, seg_len, out, cap)` builds a synthetic image (header with chip 18, revs 0–199, the descriptor with project `claude_companion` and version `1.6.0+abc`, segments of `0x5A` bytes, padding, checksum, and an appended hash of 32 `0x11` bytes, since the pure walk doesn't hash):
  - `test_info_ok`: `"6374957 3f2a…(64 hex)\n"` + 288 bytes → true, size, hash, `hdr_len` 288; a file of 100 bytes → `hdr_len` 100.
  - `test_info_rejects`: no newline; a size with a letter; 11 digits; a 63- and a 65-character hash; uppercase hex; a missing space; 287 header bytes for a 7 MB size; 289 bytes.
  - `test_header_ok`: the synthetic header → OK; every `fw_desc_t` field as built; `hash_appended` true.
  - `test_header_each_refusal`: one test per `fw_hdr_t` value, each from the good header with one change (magic `0xE8`; 0 and 17 segments; chip 0x0009; chip_rev 300 with max 199; descriptor magic off by one; project `claude_companioN`; project not NUL-terminated within 32 is PROJECT; an empty version; a version filling 32 bytes with no NUL; image_size 65,535; image_size `FW_SLOT_SIZE + 1`; n 100 with image_size 7 MB is SHORT). Also: `image_size == FW_SLOT_SIZE` is OK.
  - `test_layout_ok_one_and_nine_segments`: OK, `hashed_len` = total − 32, checksums equal.
  - `test_layout_truncated`: the image cut inside a segment, inside the checksum block and inside the hash → TRUNCATED.
  - `test_layout_bad_checksum`: one data byte flipped (checksum stays) → CHECKSUM.
  - `test_layout_trailing`: one extra `0x00` byte → TRAILING.
  - `test_layout_segment_count`: 0 and 17 → SEGMENTS; a segment `data_len` of `0xFFFFFFF0` → TRUNCATED (no overflow: sanitizer build).
  - `test_version_cmp`: `1.6.1` vs `1.6.0` NEWER; `1.6.0+abc` vs `1.6.0+def` SAME; `v1.7` vs `1.6.9` NEWER; `1.6` vs `1.6.0` SAME; `1.6.0-rc1` vs `1.6.0` OLDER; `1.6.0-rc2` vs `1.6.0-rc1` NEWER; `1.10.0` vs `1.9.0` NEWER; `80ab00e` vs `1.6.0` UNKNOWN; `""` UNKNOWN.
  - `test_offer`: same ELF → UP_TO_DATE even with different versions; different ELF and NEWER/OLDER/SAME/UNKNOWN → NEWER/OLDER/OTHER_BUILD/OTHER_BUILD.
  - `test_pct`: (0, 0) 0; (0, 100) 0; (1, 6374957) 0; (6374956, 6374957) 99; (6374957, 6374957) 100; (7, 3) 100.
  - `test_mb_text`: (3_150_000, 6_374_957) → `"3.2 of 6.4 MB"`; (0, 0) → `"0.0 of 0.0 MB"`; (49_999, 50_000) → `"0.0 of 0.1 MB"`; a cap of 4 truncates without overflow.
  - `test_gate_each` and `test_gate_order`: each refusal on its own; with every input bad, BUSY first; USB at 5 % OK; battery at 29 % BATTERY, 30 % OK; UNKNOWN source BATTERY_UNKNOWN; `psram_largest == image_size + FW_PSRAM_MARGIN - 1` MEMORY.
  - `test_validity`: not pending NONE; pending and host_ok MARK_VALID (also at 200 s); pending at 179,999 WAIT; at 180,000 ROLLBACK.
  - `test_bootloop`: crash and early → +1; crash not early → 0; early, not a crash → 0; trip at 3 only with `other_runnable`; 2 doesn't trip.
  - `test_host_rc`, `test_hdr_out`, `test_gate_out`: every value.
  - `test_out_text_ids`: every `fw_out_t` but OK maps to a non-empty `tr()` string without "Mac".
- [ ] **Step 2: Run HOST and expect it to fail** (no `fw_rules.c`).
- [ ] **Step 3: Implement `fw_rules.c` and the strings.** Little-endian reads by bytes (no struct casts). Run HOST (also its ASan/UBSan configuration if `test/host` has one): pass. Run the lint: `[]` (the new strings are used by `fw_out_text`, so `unused-str` stays clean).
- [ ] **Step 4: Commit.** Message: `feat(ota): fw_rules: image header and layout checks, version compare, progress, gates, rollback rules (Plan 6 Task 1)`.

### Task 2: The Host commands, the real-image check and the staging script

**Files:**
- Modify: `firmware/main/companion/cc_cmd.h`, `cc_cmd.c`: the two builders, the default, the exit codes.
- Modify: `firmware/test/host/test_cc_cmd.c`, `firmware/test/host/cc_cmd_print.c` (subcommands `fwinfo <path>` and `fwread <path>`).
- Create: `firmware/test/host/fw_print.c` (a host tool: `fw_print check <bin>` prints `header <FW_HDR_*>`, `layout <FW_LAYOUT_*>`, `version <v>`, `project <p>`, `hashed_len <n>` and exits 0 only when both are OK) and its target in `CMakeLists.txt`.
- Create: `firmware/tools/test_fw_cmd.py`, `firmware/tools/stage_update.sh`, `firmware/tools/test_stage_update.py`.

**Interfaces:**
- Produces (in `cc_cmd.h`):
```c
#define CC_FW_PATH_DEFAULT  "~/.local/share/claude-companion/claude_companion.bin"
#define CC_FW_RC_NO_FILE    120   /* not a readable regular file */
#define CC_FW_RC_SIZE       121   /* wc failed */
#define CC_FW_RC_HASH       122   /* no hash tool worked */
/* spec §9.1: prints "<size> <sha256>\n" then the first 288 bytes. false for an invalid path or a small cap (out cleared). */
bool cc_cmd_fw_info(const char *path, char *out, size_t cap);
/* spec §9.1: prints the file. */
bool cc_cmd_fw_read(const char *path, char *out, size_t cap);
```
- The exact strings (C literals in `cc_cmd.c`, one `snprintf` each, `%s` for the path; note `%%%%` for the shell's `%%`):
  - `exec /bin/sh -c 'F=%s; [ -f "$F" ] && [ -r "$F" ] || exit 120; S=$(wc -c < "$F") || exit 121; H=$(shasum -a 256 < "$F" 2>/dev/null || sha256sum < "$F") || exit 122; echo $S ${H%%%% *}; exec head -c 288 "$F"'`
  - `exec /bin/sh -c 'F=%s; [ -f "$F" ] && [ -r "$F" ] || exit 120; exec cat "$F"'`
- `stage_update.sh [bin]`: as spec §9.4. Exit 0 and one line `staged <version>, <size> B, sha256 <first 12 hex>…` on success; exit 1 with `stage_update: <reason>` and nothing staged otherwise. `STAGE_DIR` (default `~/.local/share/claude-companion`) is the only directory it writes, and only `claude_companion.bin` and `claude_companion.bin.tmp` in it.

- [ ] **Step 1: Write the failing C tests** in `test_cc_cmd.c`:
  - `test_cmd_fw_info_exact` / `test_cmd_fw_read_exact`: with `CC_FW_PATH_DEFAULT`, byte-for-byte the strings above with the path substituted (and a single `%%` in the info output).
  - `test_cmd_fw_info_rejects` / `test_cmd_fw_read_rejects`: false and `out[0] == 0` for `NULL`, `""`, 64 characters, `"/a b"`, `"/a'b"`, `"/a;b"`, `"/a$b"`, `` "/a`b" ``, `"/a\nb"`, `"/a\"b"`, `"/a|b"`, `"/a&b"`, and a `cap` one byte short.
  - `test_cmd_fw_no_quote_inside`: exactly two `'` in each output.
- [ ] **Step 2: Run HOST and expect it to fail.**
- [ ] **Step 3: Implement the builders, the `cc_cmd_print` subcommands and `fw_print`.** Run HOST: pass.
- [ ] **Step 4: Write `test_fw_cmd.py`.** It gets each command from `cc_cmd_print fwinfo|fwread <path>` and runs it with `subprocess.run([shell, '-c', cmd], env={'HOME': tmp_home, 'PATH': '/usr/bin:/bin'}, capture_output=True, timeout=30)` for `shell` in `/bin/sh` and `/bin/zsh`. Files live in `tempfile.mkdtemp(prefix='ccf-', dir='/tmp')` (real path, ≤ 63 characters with the file name). Tests:
  - `test_info_line_and_header`: a 1,000,000-byte random file: the first line is `f"{size} {hashlib.sha256(data).hexdigest()}"`, then exactly `data[:288]`, nothing after; exit 0.
  - `test_info_small_file`: a 100-byte file: header is all 100 bytes.
  - `test_read_exact`: stdout equals the file byte for byte (8 MB random); exit 0.
  - `test_tilde_path`: `~/fw.bin` under the temp `HOME` works for both commands.
  - `test_missing_dir_unreadable_120`: a missing file, a directory, a `chmod 000` file → 120, empty stdout, for both commands.
  - `test_no_files_left` and `test_no_process_left`: the temp dirs' file sets unchanged after every run; `pgrep -f <tmpdir>` empty.
  - `test_real_image_ok` (skipped unless `firmware/build/claude_companion.bin` exists): `fw_print check` exits 0 with `header FW_HDR_OK`, `layout FW_LAYOUT_OK`, project `claude_companion`; Python independently walks the segments and checks the checksum and `sha256(data[:hashed_len]) == data[hashed_len:hashed_len+32]`.
  - `test_real_image_refusals` (same skip): scratch copies with one byte flipped at `len // 2` (layout CHECKSUM or a hash mismatch reported by `fw_print` as `hash bad`: `fw_print` computes the hash itself with a small SHA-256 in the test tool, not in `fw_rules.c`), cut by 1,000 bytes (TRUNCATED), byte 80 changed to `X` (header PROJECT), padded to 9,000,000 bytes (header TOO_BIG), and a text file (header MAGIC). Every copy is deleted at the end.
- [ ] **Step 5: Write `stage_update.sh` and `test_stage_update.py`**: with `STAGE_DIR` in a temp dir, a synthetic image from `fw_print make-test-image <out>` (a `fw_print` subcommand that writes Task 1's synthetic image): staged byte-equal, no `.tmp` left, the version printed; a text file → exit 1, nothing in `STAGE_DIR`; a missing file → exit 1; an existing staged file is replaced atomically (the inode changes, never a partial file: check by staging twice and reading during the second).
- [ ] **Step 6: Run PY.** Expected: all pass (the real-image tests run on this Host when main's build exists in this worktree; build it first). Commit. Message: `feat(ota): cc_cmd_fw_info and cc_cmd_fw_read, real-image checks and stage_update.sh (Plan 6 Task 2)`.

### Task 3: The partition layout, rollback, the image diet and the migration flash

**Files:**
- Modify: `firmware/partitions.csv` (spec §6.4), `firmware/sdkconfig.defaults` (add `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` and `CONFIG_FATFS_CODEPAGE_437=y`; remove `CONFIG_FATFS_CODEPAGE_DYNAMIC=y`).
- Modify: `firmware/main/fonts/fonts.c`, `fonts.h`: `s_px20 = font_px24` (the role and its fallback stay); drop `LV_FONT_DECLARE(font_px20)`. Delete `firmware/main/fonts/font_px20.c`. Update `tools/gen_pixel_fonts.sh` so it no longer writes `font_px20.c` (and the `fonts.h` comment).
- Modify: `firmware/CMakeLists.txt`: `PROJECT_VER` from `version.txt` plus `git describe --always --dirty --abbrev=7` (spec P6-5), set before `project()`, unless `PROJECT_VER` is already defined (a `-D PROJECT_VER=…` on the command line wins, for test builds); fall back to `<version.txt>+nogit` when git fails.
- Modify: `firmware/version.txt`: `1.6.0`.
- Create: `firmware/tools/check_image_size.py` (reads `partitions.csv` and a `.bin`; prints `image <n> B, slot <s> B, headroom <h> B (<p> %)`; exit 1 if the image doesn't fit; prints `WARNING: under 1 MB of headroom` below 1,048,576 B), `firmware/tools/test_check_image_size.py`.
- Modify: `firmware/main/CMakeLists.txt` or the project `CMakeLists.txt`: run `check_image_size.py` after the app binary is built (`add_custom_command(TARGET app POST_BUILD …)` or the ESP-IDF equivalent; the step must not fail the build except when the image doesn't fit, which ESP-IDF already enforces).
- Test: the font change is covered by the preview (`run_all.sh`: pixels unchanged).

- [ ] **Step 1: Preview baseline.** Before changing fonts, run PV `run_all.sh` and save the screenshots it writes (or `./preview sessions` and `./preview settings 5` PNGs) to `$SHOTS/t3-before/`.
- [ ] **Step 2: Write the failing Python test** `test_check_image_size.py`: a fixture CSV with two 0x7F0000 slots: a 6,374,957-byte file → headroom 1,948,115 B (23 %), exit 0, no warning; 7,500,000 → warning; 8,323,073 → exit 1; a CSV with one app slot of 0xF60000 uses that slot. Run PY: fails.
- [ ] **Step 3: Implement** the CSV, sdkconfig, font, version and size-guard changes. Delete `firmware/sdkconfig`, build. Expected: `check_image_size.py` prints a headroom over 1.5 MB; `idf.py size` shows `font_px20.c` gone and `ffunicode.c` under 20 KB. Record the image size, each saving and the version string in the notes. `_Static_assert`-style check: `fw_rules.h`'s `FW_SLOT_SIZE` equals the CSV's size (a line in `test_check_image_size.py` reads both and compares).
- [ ] **Step 4: Preview after.** PV `run_all.sh` passes; compare the same screenshots with `t3-before` pixel for pixel (`cmp` of the PNGs, or a Python pixel diff): identical.
- [ ] **Step 5: The migration flash (device).** This is spec §11, done once:
  - Take snapshot **S** → `$SHOTS/nvs-before-migration.txt`.
  - Read NVS out: `python -m esptool --chip esp32p4 -p PORT read-flash 0x9000 0x6000 $SCRATCH/nvs-backup.bin`, then `chmod 600` it. It never leaves the scratchpad; Task 9 deletes it.
  - `idf.py -p PORT flash`. The log shows the bootloader, the partition table, `ota_data_initial.bin` and the app written; nothing at 0x9000.
  - The boot log shows `app0` running and the new version string. On the console, `esp_ota` state isn't available yet (no `ota` verb until Task 6): check the partition table with `python -m esptool --chip esp32p4 -p PORT read-flash 0x8000 0x1000 $SCRATCH/pt.bin` and `python $IDF_PATH/components/partition_table/gen_esp32part.py $SCRATCH/pt.bin`: it lists the five rows of spec §6.4.
  - Snapshot **S** → `$SHOTS/nvs-after-migration.txt`: matches the before snapshot. Wi-Fi joins without a password prompt, the Host link connects with the same pinned fingerprint, sessions list.
  - If anything in S differs: stop and report to the user with both snapshots. Don't write `nvs-backup.bin` back without the user's go-ahead.
- [ ] **Step 6: Commit.** Message: `feat(build): two OTA slots with rollback, one 24 px font copy, FATFS CP437, build versions and a size guard (Plan 6 Task 3)`.

### Task 4: Settings (CFG 25) and removing the SD updater path

**Files:**
- Modify: `firmware/main/settings/settings.h`: append `char fw_path[64];` (comment "v25, Plan 6: must pass cc_path_valid"); `ota_url`'s comment becomes "reserved (Plan 6): zeroed by CFG 25".
- Modify: `firmware/main/settings/settings_migrate.h`, `.c`: `CFG_VERSION 25`; the pure step `old_version < 25` sets `fw_path` to `CC_FW_PATH_DEFAULT` and zeroes `ota_url`; `settings_reset_scope` leaves `fw_path` alone (comment it); the reservation comment says Plan 2's Task 10 would take 26.
- Modify: `firmware/main/settings/settings.c`: `settings_defaults` sets `fw_path`; `settings_init` NUL-terminates it and falls back to the default when `!cc_path_valid`; the Mozgi512 `ota_url` line goes.
- Delete: `firmware/updater/` (all), `firmware/main/ota/ota.c`, `firmware/main/ota/ota.h`.
- Modify: `firmware/main/sys/power_console.c`: remove `cmd_ota` and its registration (Task 6 adds the new `ota` command in `ota/fw_console.c`).
- Delete or keep: `firmware/main/sys/url_mask.*` and `firmware/test/host/test_url_mask.c`: `grep -rn url_mask firmware/main` after the removal; delete with their CMake lines if nothing else uses them; record which in the notes.
- Modify: `firmware/main/CMakeLists.txt`: drop `esp_https_ota` from `PRIV_REQUIRES` (keep `esp_http_client`: `components/microlink/src/ml_coord.c` uses it; keep `app_update`, `esp_partition`, `esp_app_format`).
- Modify: `firmware/sdkconfig.defaults`: remove `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y`.
- Modify: `firmware/main/storage/sd.h`: drop the OTA comment.
- Modify: `firmware/tools/merge_flash.sh`: no updater and no `update.json`; offsets 0x2000, 0x8000, 0xF000, 0x20000 only; prints `WARNING: the full image overwrites NVS (Wi-Fi, keys, settings). For a blank Tab5 only; use idf.py flash otherwise.` first.
- Modify: `firmware/main/companion/cc_link.h`, `cc_link.c`: `cc_settings_fw_path(char *out, size_t cap)` and `bool cc_link_set_fw_path(const char *path)` (false unless `cc_path_valid`; saves).
- Modify: `firmware/tools/ui_lint.py`, `test_ui_lint.py`: P6-10's two rules.
- Test: `firmware/test/host/test_settings_migrate.c`, `firmware/tools/test_ui_lint.py`.

- [ ] **Step 1: Write the failing tests.**
  - `test_migrate_v24_to_v25_fw_path`: a v24 blob (tail padding 0xAA over the new field's offset, and `ota_url` holding `https://u:tok@example.com/m.json?k=s`) loads with `fw_path` the default, `ota_url` all zero, and returns 24.
  - `test_v25_blob_keeps_fw_path`: a v25 blob with `~/src/claude-companion/firmware/build/claude_companion.bin` keeps it.
  - `test_reset_scope_keeps_fw_path`.
  - `test_newer_blob_takes_newer_path` updated: a v26 blob copies what fits and returns 26.
  - Every existing test that asserted `ota_url` survives a migration (around `test_settings_migrate.c:454`) is updated: below 25 it is zeroed.
  - Lint: `test_esp_ota_outside_fw_update_flagged` (`esp_ota_begin(` in `ui/ui_settings.c` flagged; in `ota/fw_update.c` not), `test_partition_write_flagged`, `test_fwcmd_literal_flagged` (`"exec cat"` in `ota/fw_update.c` flagged; in `companion/cc_cmd.c` not).
  - Run HOST and PY; expect failures.
- [ ] **Step 2: Implement** the settings, migration, removals, link setters and lint rules. Run HOST, PY, the lint (`[]`) and the FW build. `grep -rn "updater\|factory\|main_app\|ota_install\|ota_download\|ALLOW_HTTP\|esp_https_ota" firmware/ --include=*.c --include=*.h --include=*.txt --include=*.csv --include=*.defaults --include=*.sh --include=*.py` finds nothing outside git history and the notes (record the command output).
- [ ] **Step 3: Device step.** Flash (`idf.py -p PORT flash`; the layout is already in place, NVS kept). The first boot logs `settings v24 -> v25`; the second logs no migration. Snapshot S matches the after-migration one. `help` no longer lists `ota url`.
- [ ] **Step 4: Commit.** Message: `refactor(ota): remove the SD updater path; fw_path setting (CFG 25) and the flash-safety lint rules (Plan 6 Task 4)`.

### Task 5: The link: firmware info and the binary read

**Files:**
- Modify: `firmware/main/companion/cc_link.h`, `cc_link.c`: the two requests below; the `req_t` fields `out_len`, `progress`, `cancel`, `direct`; `do_exec`'s direct-read path, cancel and keepalive.
- Create: `firmware/main/ota/fw_console.c`: the `ota` command with `check`, `path [<p>]` and (debug) `read-test [cancel-at <pct>]`. Task 6 adds the rest. Register it from `app_main.c` (or `power_console_init`, where `ota` lived).
- Modify: `firmware/main/CMakeLists.txt` only if needed (the glob picks up `ota/*.c`).

**Interfaces:**
- Produces (in `cc_link.h`):
```c
#define CC_FW_INFO_MAX        512
#define CC_FW_INFO_TIMEOUT_MS 10000
#define CC_FW_CANCELLED       ESP_ERR_NOT_FINISHED
#define CC_FW_READ_TIMEOUT_MS(size) (20000 + (int)((uint64_t)(size) * 1000 / (64 * 1024)))
/* cc_cmd_fw_info for the fw_path setting on a non-pty exec channel. out gets the raw output (binary: *len is its
   length; out is NUL-terminated after it). ESP_OK with *exit_status; ESP_ERR_INVALID_STATE (not connected or
   parked); ESP_ERR_TIMEOUT; ESP_ERR_INVALID_SIZE (over cap). Blocks: never from the LVGL task. Counts as link-busy. */
esp_err_t cc_link_fw_info(uint8_t *out, size_t cap, size_t *len, int *exit_status);
/* cc_cmd_fw_read for the fw_path setting on a non-pty exec channel, reading stdout straight into buf (up to 16 KB per
   read), *progress updated after each read, *cancel checked each loop. ESP_OK with *len and *exit_status;
   ESP_ERR_INVALID_SIZE (more than cap bytes: the read stops there); CC_FW_CANCELLED; ESP_ERR_INVALID_STATE (not
   connected, or a park mid-read); ESP_FAIL (a channel error); ESP_ERR_TIMEOUT. A cancel, an error, an oversize or a
   timeout drops the session (the link reconnects at once). The watch keepalive runs every second meanwhile. Counts as
   link-busy. Logs only sizes, the duration, the rate and the exit status. Blocks: never from the LVGL task. */
esp_err_t cc_link_fw_read(uint8_t *buf, size_t cap, size_t *len, volatile uint32_t *progress,
                          volatile bool *cancel, int *exit_status, int timeout_ms);
```
- Console (no debug gate, read-only on the Host): `ota check` prints `ota check: rc <n>, <size> B, sha256 <12 hex>…, header <FW_HDR_*>, version <v>` (Task 6 replaces it with the full check). `ota path [<p>]`: prints the path, or sets it (`ota path: ok` or the path copy). Debug: `ota read-test [cancel-at <pct>]` allocates `size + 1` B of PSRAM, calls `cc_link_fw_read`, hashes the buffer with mbedTLS, frees it, and prints `ota read-test: <result>, <len> B in <ms> ms (<KB/s> KB/s), sha256 <match|MISMATCH>`; `cancel-at` sets the cancel flag from a helper task when `progress` passes that percent.

- [ ] **Step 1: Implement.** FW build; HOST still passes.
- [ ] **Step 2: Device step: the check.** Flash. Stage this build into the scratch stage dir (`STAGE_DIR=… sh firmware/tools/stage_update.sh`) and `ota path <that>/claude_companion.bin`. `ota check`: rc 0, the size and SHA-256 prefix equal `stage_update.sh`'s line, `header FW_HDR_OK`, the version equals the running one. `ota path /nope/x.bin`, `ota check`: rc 120. `ota path "/a b"`: refused, unchanged.
- [ ] **Step 3: Device step: the read.** `ota read-test` three times with the watch on: `ESP_OK`, the length equals the size, `sha256 match`; record the KB/s on the LAN (O1). After each, `companion status` is connected and the watch running (`watch: on …` with a recent block). If the rate is under 500 KB/s, check that the direct-read path is taken (a log line at start says `fw read: direct`) before going on, and record the finding.
- [ ] **Step 4: Device step: cut-offs.**
  - `ota read-test cancel-at 40`: `CC_FW_CANCELLED` within 1 s of 40 %; the link reconnects within 5 s (`companion status`); locally on the Host, `pgrep -fl "cat .*claude_companion"` is empty within 2 s.
  - A park: start `ota read-test`, and within 2 s `power go radio-off`: `ESP_ERR_INVALID_STATE` within 1 s. `power go active`; the link and the watch come back.
  - A Wi-Fi drop: start `ota read-test`, then `wifi test drop` (debug) at about 30 %: `ESP_FAIL`, `ESP_ERR_INVALID_STATE` or `ESP_ERR_TIMEOUT` (record which and how long it took). `wifi join` if needed; the link comes back.
  - Over Tailscale: `ts on`, the Host entry on its tailnet address if that is how the user's setup tests Tailscale (Plan 3 notes), `ota read-test` once: record the KB/s and whether it beats `CC_FW_READ_TIMEOUT_MS`; `ts off`.
  - Restore the default path: `ota path ~/.local/share/claude-companion/claude_companion.bin`.
- [ ] **Step 5: Commit.** Message: `feat(link): cc_link_fw_info and cc_link_fw_read: binary exec reads with progress and cancel (Plan 6 Task 5)`.

### Task 6: The update service, the boot check and the console verbs

**Files:**
- Create: `firmware/main/ota/fw_update.h`, `fw_update.c`.
- Modify: `firmware/main/ota/fw_console.c`: `ota status`, the full `ota check`; debug `ota update [corrupt-ram]`, `ota cancel`, `ota go-back`, `ota test-bootloop <1-3>`.
- Modify: `firmware/main/Kconfig.projbuild`: `COMPANION_OTA_TEST_NO_VALIDATE` (bool, default n, `depends on COMPANION_DEBUG_COMMANDS`, help: "Builds an image that never marks itself valid, for the rollback test. Never for a build you keep.").
- Modify: `firmware/main/sys/power.h`, `power.c`: `POWER_HOLD_UPDATE` (counted like `POWER_HOLD_TAKEOVER`; `power_holds_text` prints `update`); `bool power_hold_is_on(power_hold_t h)` if no such query exists.
- Modify: `firmware/main/voice/voice.c`: `voice_start` returns `VR_OUT_BUSY` while `fwu_busy()`.
- Modify: `firmware/main/app_main.c`: `fwu_boot_init()` right after NVS and settings init, before Wi-Fi.

**Interfaces:**
- Produces:
```c
typedef enum { FWU_IDLE, FWU_DOWNLOADING, FWU_CHECKING, FWU_WRITING, FWU_READBACK, FWU_RESTARTING } fwu_phase_t;
typedef struct { fwu_phase_t phase; uint64_t done, total; } fwu_status_t;
typedef struct {
    fw_out_t out; int rc; uint32_t size; char sha[65]; fw_desc_t desc; fw_offer_t offer; int64_t at_ms;
} fwu_check_t;
/* Blocking (never the LVGL task): cc_link_fw_info, fw_parse_info, fw_check_header (this chip's revision), fw_offer.
   Caches the result. */
void fwu_check(fwu_check_t *out);
bool fwu_last_check(fwu_check_t *out);          /* the cached check and its age (at_ms); false if none */
/* Runs fw_gate on the live inputs; on OK starts the update task (6 KB internal) with the cached check's size and
   SHA-256 and returns FW_OUT_OK; otherwise the gate's outcome. Needs a cached OK check younger than
   FW_CHECK_TTL_MS whose offer isn't UP_TO_DATE (else FW_OUT_CHANGED: the caller re-checks). */
fw_out_t fwu_start(void);
void fwu_cancel(void);                          /* any phase before RESTARTING */
void fwu_status(fwu_status_t *out);
bool fwu_busy(void);
/* One outcome slot (P6-17): the update's end (CANCELLED, errors) or a boot note (UPDATED, ROLLED_BACK, BOOTLOOP),
   with the version strings it needs. True once per outcome. */
typedef enum { FWU_NOTE_RESULT, FWU_NOTE_UPDATED, FWU_NOTE_ROLLED_BACK, FWU_NOTE_BOOTLOOP } fwu_note_kind_t;
typedef struct { uint32_t gen; fwu_note_kind_t kind; fw_out_t out; char ver_a[33], ver_b[33]; } fwu_note_t;
bool fwu_take_note(fwu_note_t *out);
void fwu_boot_init(void);                       /* spec §12.3 */
/* The other slot holds a runnable image (VALID or UNDEFINED state, descriptor readable, project FW_PROJECT). */
bool fwu_previous(fw_desc_t *out);
/* esp_image_verify on the other slot, then esp_ota_set_boot_partition and link_down_restart. ESP_ERR_NOT_FOUND
   when not runnable; never returns on success. */
esp_err_t fwu_go_back(void);
typedef struct { const char *running_label, *other_label; int running_state, other_state; bool pending, rollback_possible; int bootloop; } fwu_info_t;
void fwu_info(fwu_info_t *out);                 /* for `ota status` and About */
```
- The task follows spec §12.2 exactly; `release()` (P6-15) is the only free. It logs `fw: writing <label>` at `esp_ota_begin` (Task 6 Step 4 and Task 9 Step 3 look for its absence), the spec §12.2 summary line at the end, and, when a `ui heapmin` window is open, the window's internal minimum at RESTARTING (`fw: heapmin <N> B`), since the restart ends the window. `_Static_assert`s pin the Task 1 byte offsets to `offsetof(esp_image_header_t, …)` and `offsetof(esp_app_desc_t, …)` (+ 32), and `FW_SLOT_SIZE` is checked against `esp_ota_get_next_update_partition(NULL)->size` at boot (a mismatch logs an error and disables updates: `fwu_start` returns `FW_OUT_WRITE`).
- The validity path: `fwu_boot_init` subscribes with `cc_poll_subscribe`; the callback (poll task) calls `fw_validity(pending, true, uptime)`, and on MARK_VALID runs `esp_ota_mark_app_valid_cancel_rollback()`, clears `fw/attempt`, ends the hold and posts `FWU_NOTE_UPDATED`. The 180 s `esp_timer` evaluates `fw_validity(pending, false, uptime)` and on ROLLBACK posts the notice (`FW_ROLLING_BACK`, shown by Sessions as a toast), waits 3 s, then `esp_ota_mark_app_invalid_rollback_and_reboot()`. With `CONFIG_COMPANION_OTA_TEST_NO_VALIDATE`, the callback never marks valid (it logs `fw: test build: not marking valid`).
- Console output:
  - `ota status`: `ota: running <ver> (<label>, <state>), other <ver|none> (<label>, <state>), rollback possible: <yes|no>, pending: <yes|no>, bootloop <n>`.
  - `ota check`: `ota check: host <ver>, <size> B, sha256 <12 hex>…, <newer|older|other build|up to date>` or `ota check: <tr(fw_out_text)>, rc <n>`.
  - `ota update`: runs `fwu_check`, then `fwu_start`, then prints `ota update: <phase> <pct> %` on each phase change and every 10 %, and finally `ota update: restarting` or `ota update: <tr(fw_out_text)>`. `corrupt-ram` sets a debug flag that XORs byte `size / 2` of the buffer after the download.
  - `ota test-bootloop <n>`: stores n in an `RTC_NOINIT` word and restarts; each boot with n > 0 decrements it and arms a 5 s timer that calls `abort()`.

- [ ] **Step 1: Implement.** FW build; HOST and PY still pass; the lint gives `[]` (the new rules allow `fw_update.c`).
- [ ] **Step 2: Device step: the first wireless update.** Flash this build over USB (`idf.py -p PORT flash`). `ota status`: running `app0`, other none. Make a second build without editing any file: build the same tree into `$SCRATCH/build-b` with `-D PROJECT_VER=1.6.0+testb` (Task 3's CMake keeps a `PROJECT_VER` given on the command line). Its ELF differs only by the version string, which is enough: `fw_offer` compares ELF SHA-256s. Stage `build-b`'s bin to the scratch stage dir, `ota path` to it, `ota check`: `other build`. `ui heapmin start`, `ota update`. Record each phase's time. The Tab5 restarts; the boot log shows `app1` running, pending verify, then within about 15 s `marked valid`; `ui heapmin stop` is lost with the restart, so run the heap gate in Task 9 instead. `ota status`: running `1.6.0+testb (app1, valid)`, other `app0` with its version, rollback possible yes. Snapshot S matches.
- [ ] **Step 3: Device step: power hold.** On battery simulation (`power sim battery 50`): during `ota update`, at the download and again at the write, `power sim idle 300`: `power` shows `active` with holds including `update`. After the restart and before it marks valid, `power` shows the `update` hold. After it is valid, `power sim idle 300` dims as usual. `power sim battery 25`: `ota update` prints the battery copy and starts nothing. `power sim battery -1` at the end.
- [ ] **Step 4: Device step: refusals before any write.** For each, `ota status` is unchanged after, and the log shows no `esp_ota_begin` line (log one at begin: `fw: writing <label>`):
  - a scratch copy with a byte flipped at the middle → `The firmware file is damaged…`;
  - `ota update corrupt-ram` → `The file on the Host changed during the download…`;
  - the copy padded to 9,000,000 B → `That firmware is too large…` at the check;
  - `firmware/build/bootloader/bootloader.bin` staged as is (copied by hand into the scratch dir, not through the script, which refuses it) → `That file isn't Claude Companion firmware…`;
  - no file → `No firmware file on the Host…`.
- [ ] **Step 5: Device step: a pending image reset.** Build the no-validate image into `$SCRATCH/build-novalid` with `$SCRATCH/novalid.defaults` = `CONFIG_COMPANION_DEBUG_COMMANDS=y` and `CONFIG_COMPANION_OTA_TEST_NO_VALIDATE=y`, `PROJECT_VER=1.6.0+novalid`. The USB cable stays in (Global Constraints). Stage, `ota update`. It boots `1.6.0+novalid`, `ota status` shows pending yes. `reboot`: the boot log shows the bootloader rolling back; the previous version runs; the toast "The update to 1.6.0+novalid didn't start properly…" shows (screenshot `$SHOTS/t6-rolledback.jpg`, read it); `ota status` shows the other slot invalid or aborted.
- [ ] **Step 6: Device step: the 180 s timer.** `ota update` to the novalid image again (the slot is rewritten). Don't touch it. At about 177 s the notice toasts; at 180 s it restarts into the previous version, which toasts the rolled-back note. Record the times.
- [ ] **Step 7: Device step: a cancel and a drop.** `ota update` and `ota cancel` from a second console line at about 30 % of the download, then again during the write: both print `Update cancelled…`; `ota status` unchanged; PSRAM free (`ts` or the heap line) back within 4 KB of before. `ota update` with `wifi test drop` at about 30 % of the download → `Lost the connection…`, unchanged. `ota update` with `wifi test drop` during the write → the write completes, it restarts, and the new image marks itself valid once Wi-Fi is back (or rolls back after 180 s if it doesn't come back; record which).
- [ ] **Step 8: Commit.** Message: `feat(ota): the update service: PSRAM download, full verification, esp_ota write, rollback, boot-loop guard and console verbs (Plan 6 Task 6)`. Restore the default path and put the main build back over USB if the device ended on a test image.

### Task 7: The UI: About, Settings › Host and the progress screen

**Files:**
- Modify: `firmware/main/ui/ui_settings.c`: in `fill_about`, after Version: **Update firmware** (action row, value from `fwu_last_check`), **Previous version** (only when `fwu_previous`), the footnote `SET_FW_NOTE`; the confirm (`ui_confirm_ex`, Install focused); the Go back confirm; in `fill_host`, **Firmware file on Host** after the whisper rows (value row, prompt, `cc_link_set_fw_path`, refused with `SET_CLAUDE_PATH_BAD`); the About background check on show (P6-16) with a 300 ms poll of the job's result to refill the row.
- Modify: `firmware/main/ui/ui_sessions.c`, `ui_sessions.h`: `JOB_FW_CHECK` and `JOB_FW_START`; `bool ui_sessions_fw_check(void)` and `bool ui_sessions_fw_start(void)`; the 300 ms tick takes boot notes with `fwu_take_note` and toasts them (spec §8.6 copy, 6 s).
- Create: `firmware/main/ui/ui_fwupdate.c`, `ui_fwupdate.h`: `void ui_fwupdate_show(void)` (the modal on `lv_layer_top`, spec §8.4), its 300 ms timer, key and touch routing, and the close path that toasts the result note.
- Modify: `firmware/main/i18n/i18n.h`: the remaining `SET_FW_*` and `FW_*` UI strings, `SET_HOST_NOTE` changed (spec §8.6).
- Modify: `firmware/tools/ui_preview/preview.c`, `run_all.sh`, the preview README: stubs for `fwu_*` and the new `cc_settings_fw_path` and setter (scriptable: `PREVIEW_FW_CHECK=none|checking|uptodate|newer|older|nofile|notapp`, `PREVIEW_FW_PREV=1`, `PREVIEW_FW_PHASE=download|check|write|readback|restart` with `PREVIEW_FW_PCT`), and the cases below.

**Interfaces:**
- Consumes: Task 6's `fwu_*`, Task 1's `fw_out_text`, `fw_pct`, `fw_mb_text`.
- Internal: `static void fw_row_cb(lv_event_t *e)`: no check or a stale/failed one → `ui_sessions_fw_check()`, the row shows "Checking…"; a fresh NEWER/OLDER/OTHER_BUILD → the confirm; UP_TO_DATE → re-check. `static void fw_install_yes(bool yes, void *ud)`: yes → `ui_sessions_fw_start()`; the job calls `fwu_start()` and, on `FW_OUT_OK`, `ui_async(ui_fwupdate_show)`; on another outcome toasts `tr(fw_out_text(o))`; on `FW_OUT_CHANGED` it re-checks first.

- [ ] **Step 1: Preview cases first** (they fail until Step 2):
  - `settings 5` with each `PREVIEW_FW_CHECK`: the row value reads `Check`, `Checking…`, `Up to date`, `1.6.1+3f2a9c1 available`, `1.6.1+3f2a9c1 (older) available`, the red `No firmware file on the Host…` cut to the row by the fit check (dots or wrap inside the row, never overflow), and the red `That file isn't…`. No amber. With `PREVIEW_FW_PREV=1` the Previous version row shows `1.5.9+0c1d2e3 · Go back`.
  - `PREVIEW_CLICK=Update firmware settings 5` with `newer`: the confirm shows `Install 1.6.1+3f2a9c1?`, the body names the running version, Install has the focus; `key enter` calls the start stub once; with `older` the body starts with the older line; Esc closes it and nothing starts.
  - `PREVIEW_CLICK=Previous version settings 5`: the Go back confirm; Enter calls the `fwu_go_back` stub.
  - `fwupdate` with each `PREVIEW_FW_PHASE`: title, phase line, bar at `PREVIEW_FW_PCT`, the bytes line (`3.2 of 6.4 MB · 50 %`), the hint and Cancel (both hidden at `restart`); the fit check; no amber; `ui timers` lists one 300 ms timer; Esc calls `fwu_cancel`; `key text x`, `key ctrl p` and Enter change nothing; a tap outside Cancel does nothing; a tap on Cancel calls `fwu_cancel`. After the stub reports IDLE with a CANCELLED note, the screen is gone, no timer remains and the toast reads `Update cancelled. Nothing was changed.`
  - `settings 0`: **Firmware file on Host** follows **Whisper model on Host** (and Voice input) with the default value; `PREVIEW_CLICK=Firmware file on Host` opens the prompt; entering `/a b` shows the path copy.
  - `sessions` with a queued `FWU_NOTE_UPDATED` note: the toast `Updated to 1.6.1+3f2a9c1`; with ROLLED_BACK and BOOTLOOP: their copy.
- [ ] **Step 2: Implement.** PV `run_all.sh` passes; HOST, PY and the lint (`[]`) pass; FW build.
- [ ] **Step 3: Device step: by touch.** Flash over USB. Stage build B (Task 6 Step 2's method) to the scratch dir, set the path in Settings › Host by the prompt (soft keyboard), then: tap Settings, tap About: the row shows "Checking…" then `1.6.0+testb available` (screenshot `$SHOTS/t7-about.jpg`, read it). Tap it, tap Install. The progress screen runs through its phases (screenshots during download and write: `$SHOTS/t7-download.jpg`, `$SHOTS/t7-write.jpg`; read them: the bar moves, the bytes line is right). It restarts; "Updated to 1.6.0+testb" toasts on Sessions (screenshot).
- [ ] **Step 4: Device step: by keyboard.** Stage the main build back. `key ctrl p`, arrows to About, Enter on the section, arrows to Update firmware, Enter, Enter (Install). Mid-download, Esc: the screen closes and "Update cancelled. Nothing was changed." toasts. Again, without Esc, to the end: it updates.
- [ ] **Step 5: Device step: Go back.** About › Previous version shows the other slot's version; Enter, Enter: it restarts into it; `ota status` agrees. Go back again to end on the main build.
- [ ] **Step 6: Device step: gates in the UI.**
  - A take-over: with a `companion-test-*` session open full-screen, Settings can't be opened (Ctrl+P goes to the session, like every key but the leave and voice keys), so no update can start from the UI; `ota update` from the console prints "Close the session first, then update."
  - Voice: `voice test-bg 10` on the console (it records in the background), then Settings › About › Update firmware › Install within those 10 s: "Wait for voice input to finish, then update." and nothing starts.
  - Battery: `power sim battery 20`: Install toasts the battery copy; `power sim battery -1`.
- [ ] **Step 7: Commit.** Message: `feat(ui): Update firmware and Previous version in About, the firmware path in Settings › Host, the progress screen (Plan 6 Task 7)`. Restore the default path.

### Task 8: README, firmware README and the main spec

**Files:**
- Modify: `README.md`:
  - §1 Build and flash: "Flash and watch the log" drops the factory-updater sentence; a new `### Update over Wi-Fi` after it: the one-time migration (spec §11, with the must/must-not lists and the `app-flash` and merged-image warnings), staging (`sh tools/stage_update.sh`, the alias from spec §9.4 with `~/src/claude-companion` as the placeholder repo path), installing (About › Update firmware), what happens if it fails (rollback, the 180 s check, Previous version, the boot-loop guard), and when USB is still needed.
  - §4 Daily use: one line pointing to Update over Wi-Fi.
  - §5 Settings: the Host row lists the firmware path; the About row lists Update firmware and Previous version.
  - §9 Console: `ota`, `ota check`, `ota path` rows; the debug paragraph lists `ota update`, `ota cancel`, `ota go-back`, `ota test-bootloop`; `ota url` gone.
  - §10 Troubleshooting: "No firmware file on the Host…", "That file isn't Claude Companion firmware…", "The firmware file is damaged…", "The update to … didn't start properly…", "The Tab5 kept restarting…", and "Both versions are broken" (USB, `idf.py flash`, keeps NVS).
  - §12 Security model: a bullet: updates come only from the firmware path on the Host over the pinned SSH link, through two fixed commands; the image is checked (project, chip, size, the Host's SHA-256, its own appended hash) before anything is written; the running version is never overwritten; rollback; no signing yet, because without secure boot the Host account and USB access are already the trust boundary.
  - Status, Known limitations (drop "No firmware updates over the air"; add "The bootloader, the partition table and the C6 still need USB"), Roadmap (6 Wireless updates done; 7 Camera next; 8 Release).
- Modify: `firmware/README.md`: the layout (`ota/` new contents; `updater/` gone); the partitions line; the Build section's `idf.py flash` paragraph; "Release images" rewritten (merge_flash's NVS warning; the table without the updater); "OTA (dormant)" replaced by "Firmware updates" (the flow of spec §12.2–12.3, the lint rule, the Kconfig test option); the Host link section gains the two commands; the microSD list drops "the dormant OTA path"; the test-only list gains the `ota` debug verbs.
- Modify: `docs/superpowers/specs/2026-10-22-claude-tab5-design.md`: the roadmap gains Plan 6 (this plan, done) and renumbers camera and release; the command table gains the two firmware commands.
- Modify: `THIRD_PARTY_NOTICES.md` only if it names the updater.

- [ ] **Step 1: Write the docs.** Every command in them is copied from the code or the spec, not retyped. Run the README's staging line locally with `STAGE_DIR` in the scratchpad (allowed: local, scratch only).
- [ ] **Step 2: Check:** `grep -n "updater\|factory\|ota url\|dormant\|0xF80000" README.md firmware/README.md` finds nothing stale; `grep -n "merge_flash\|full_flash\|write_flash.*0x0" README.md firmware/README.md` shows every mention next to the NVS warning; no "Mac" added for the Host; no real host names, addresses, user names or session names (`my-host.local`, `you` only).
- [ ] **Step 3: Commit.** Message: `docs: wireless firmware updates: migration, staging, use, recovery and security model (Plan 6 Task 8)`.

### Task 9: Final device gates

**Files:**
- Modify: `docs/superpowers/notes/2026-10-25-wireless-updates-notes.md`: the final table.
- No product code changes. A failed gate gets a fix commit in the task that owns the code, and this task's step is re-run.

- [ ] **Step 0: Recovery ready.** Build this branch's head into `firmware/build` and flash it over USB (`idf.py -p PORT flash`); keep the cable in for the whole task. Snapshot S → `$SHOTS/nvs-t9-start.txt`. Build B (`PROJECT_VER=1.6.0+gateb`) and the no-validate image into scratch build dirs.
- [ ] **Step 1: Sizes.** `idf.py size` and `check_image_size.py`: record the image, the headroom (expected ≥ 1.5 MB) and static internal RAM against the Plan 5 final figures (expected: within +2 KB).
- [ ] **Step 2: An update from a Host path, both slots.** By touch: main → B (`app0` → `app1`). By keyboard: B → main (`app1` → `app0`). Record per phase: download (KB/s), check, write, read-back, restart to valid, total (expected total ≤ 60 s on the LAN). Once with Tailscale on (`ts on`, then `ts off`): the download completes within its timeout; record the rate.
- [ ] **Step 3: Corrupt and wrong images refused.** Each through the UI (About), with a screenshot of the row or toast, and `ota status` unchanged after: a byte flipped mid-image; the file cut by 1,000 bytes; byte 80 changed (project); padded to 9 MB; a text file; the bootloader; `ota update corrupt-ram` (console); a missing file. The log shows no `fw: writing` line for any of them.
- [ ] **Step 4: A bad-boot image rolled back.** The no-validate image: (a) install, then `reboot` while pending → back on the previous version, the rolled-back toast, `ota status` other slot invalid/aborted; (b) install, wait → the 177 s notice and the 180 s rollback, the toast. Both end on the main build; record times. If the Tab5 doesn't come back within 2 minutes at any point: USB `idf.py -p PORT flash` (keeps NVS), note it, and stop the task to report.
- [ ] **Step 5: The boot-loop guard.** On a validated build B with main in the other slot: `ota test-bootloop 3`. Three boots each panic at 5 s; the fourth boot is the other slot, with the "kept restarting" toast; `ota status` agrees; bootloop 0. Then `ota test-bootloop 2` on main with B in the other slot: two panics, then main again (no switch), bootloop resets after 60 s up.
- [ ] **Step 6: The power hold.** `power sim battery 50`: through a whole update, `power sim idle 300` at the download, the write and the pending check: never dims (`power` shows the `update` hold each time). `power go radio-off` during the download: "Lost the connection…", unchanged, `power go active`. `power sim battery 29`: refused; `30`: allowed. `power sim battery -1`. On USB: no floor (an update at a simulated 5 % on USB goes through, if `power sim battery` can simulate USB; else note it and rely on Task 1's test).
- [ ] **Step 7: Wi-Fi drops mid-update.** `wifi test drop` at about 30 % of the download: "Lost the connection…", nothing changed; retry succeeds. `wifi test drop` during the write: it installs; the new image becomes valid once the link is back (record the time) or rolls back after 180 s (record which).
- [ ] **Step 8: Heap during the update.** `ui heapmin start` before Install, and read `fw: heapmin <N> B` from the log just before the restart: internal minimum ≥ 40,960 B with Tailscale off and ≥ 20,480 B with it on. A cancelled update: PSRAM free back within 4 KB. No `ERR_BUF` in the console log.
- [ ] **Step 9: NVS survives.** Snapshot S after Step 2's two updates, after Step 4's rollback, and after Step 5's boot-loop switch: each matches `nvs-t9-start.txt` (and Task 3's `nvs-before-migration.txt`, apart from the new `ota path` line). The custom scratch `ota path` set during the task survives each update (it lives in the settings blob).
- [ ] **Step 10: Nothing left on the Host.** After every step above, locally on the Host: `pgrep -fl "claude_companion.bin"` is empty; the real staging directory (`~/.local/share/claude-companion`) was never written by the tests (its listing, if it exists, is unchanged from the task start; record it).
- [ ] **Step 11: Clean up and the user's visit.** Delete `$SCRATCH/nvs-backup.bin` and the scratch stage dirs; restore the default `ota path`; check `ts` is off; stop all `companion-test-*` sessions. Write the batched visit list in the notes: whether the About rows and the progress screen read clearly at arm's length, whether 30 % is the right floor, whether the default staging path and the alias suit the user's workflow, and every PENDING item above.
- [ ] **Step 12: Commit.** Message: `docs(plan6): final measurements, gates and the visit list (Plan 6 Task 9)`.

---

Execution: subagent-driven development (superpowers:subagent-driven-development), one fresh implementer and reviewer per task.
