# Plan 6: Wireless firmware updates: Design

**Date:** 2026-10-25
**Status:** direction set by the user (2026-10-25): after one wired flash, new builds arrive over
Wi-Fi from the Host; pull over the existing SSH link is the suggested route; rollback with
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`; an About row, a confirm, a progress screen. Calls this spec
makes that the inputs don't settle are marked **Ruling** with a reason and the cost if wrong, and
are indexed in §16.
**Builds on:** Plan 5 (voice input, on main at 80ab00e). The whole-project spec is
`docs/superpowers/specs/2026-10-22-claude-tab5-design.md`. This plan moves the camera to Plan 7 and
the release to Plan 8 (§15).

---

## 1. Intent and why

Every new build today means finding the USB-C cable, plugging the Tab5 into the Host and running
`idf.py flash`. The Tab5 lives on the couch; the Host is on the desk. The user builds on the Host
anyway, and the Tab5 already holds an authenticated SSH link to it. So the Tab5 should pull the
build over that link, check it, install it in a second app slot and restart, and go back to the
version it ran before by itself if the new one doesn't come up.

The loop this plan adds:

1. On the Host: `idf.py build`, then `sh tools/stage_update.sh` (or one alias that does both). The
   build is copied to a fixed staging path.
2. On the Tab5: Settings › About › **Update firmware** shows the running version and the one on
   the Host. Tap it (or Enter), confirm.
3. A progress screen: downloading, checking, writing, checking again. About 40 s.
4. The Tab5 restarts into the new version. Once it has joined Wi-Fi and heard from the Host, it
   marks itself good and toasts "Updated to 1.6.1+3f2a9c1". If it doesn't get there, it goes back.

The one-time cost: a wired `idf.py flash` of this plan's first build, which brings the new
partition table and a bootloader with rollback. NVS is kept (§11).

## 2. Goals, non-goals, anti-goals

### Goals
1. Install a build from a file on the Host over the existing SSH link, by touch or keyboard, with no
   USB after the first flash.
2. Nothing new on the Host: two fixed `cc_cmd_*` commands that read a file. No server, no daemon,
   nothing left running.
3. A bad build can't strand the Tab5: the running slot is never written; a new image counts only
   after it has booted, joined Wi-Fi and reached the Host; otherwise the bootloader goes back. A
   build that passed that check but later crash-loops goes back too (§7.6).
4. A corrupt, truncated, oversized, wrong-chip or wrong-project file is refused **before any flash
   write**.
5. A cut-off update (Wi-Fi drop, the Host asleep, cancel, power loss) leaves the running version as
   it was.
6. One update mechanism. The SD-card updater path inherited from Clientre 5 is removed (§10).
7. The image gets smaller, so two app slots leave room to grow (§6).

### Non-goals
- **Updating the bootloader, the partition table or the ESP32-C6 over the air.** All three stay
  USB-only. A wrong bootloader write is the one thing rollback can't undo.
- **Automatic installs or background checks.** The Tab5 checks only when About is shown or the row is
  used, and installs only after a confirm.
- **Download resume.** A failed download starts again from byte 0 (about 8 s on the LAN).
- **Signed images, secure boot, flash encryption, anti-rollback eFuses.** §7.7 says why; no eFuse is
  ever burned by this plan.
- **A release channel** (GitHub releases, an HTTPS manifest). The Host is the only source. Plan 8
  (release) can add one beside it.
- **Delta updates or compressed images.**

### Anti-goals (a failure even if it "works")
- Any write to the running app slot, the bootloader, the partition table, NVS or otadata other than
  through `esp_ota_*` on the other slot.
- An update that starts without the user's confirm, or a restart into an image that wasn't fully
  verified.
- Any Host command line not built by a `cc_cmd_*` function; any login shell; any process left on the
  Host after the channel ends.
- A Tab5 left on a broken build with no way back but USB, when the previous build was good.
- The screen dimming, Radio off or Hibernate during an update (the one Plan 2 I1 exception aside,
  which the battery floor keeps out of reach, §7.4).
- Amber on any update screen (amber means NEEDS YOU only).

## 3. Constraints

### Hard project rules (unchanged; the ones this plan touches)
1. **Never open a login shell on the Host.** The update uses non-pty exec channels.
2. **Every command line sent to the Host comes from a `cc_cmd_*` function.** The firmware path
   passes `cc_path_valid` (`[A-Za-z0-9_./~-]`, 1–63 chars) like the Claude and whisper paths.
3. **No secrets in the repo or in logs.** The update adds none. Logs carry sizes, versions, SHA-256
   prefixes, durations, phases and exit codes. The settings migration wipes the old `ota_url`, which
   could hold a token (§10).
6. **Never flash the C6, erase NVS, or run `sshkey regenerate`.** The migration flash keeps NVS
   (§11). The merged "full flash" image, which would overwrite NVS, is documented as blank-device
   only.
9. **Amber `#f4b545` means NEEDS YOU only.** The progress bar is `fg`, errors red.

### Technical facts
- **Flash:** 16 MB. Bootloader at 0x2000, partition table at 0x8000. Today: `nvs` 0x9000 (24 KB),
  `otadata` 0xF000 (8 KB), `phy_init` 0x11000 (4 KB), `main_app` (ota_0) 0x20000 of 0xF60000
  (~15.4 MB), `factory` (ota_1) 0xF80000 of 0x80000 (512 KB, the SD updater).
- **The image:** `build/claude_companion.bin` is 7,415,488 B (main at 80ab00e). §6.1 has the
  breakdown.
- **PSRAM:** 32 MB. The download goes there whole (§7.1).
- **The link:** one libssh task; exec channels with stdin and stdout streaming (`do_exec`,
  `cc_link_transcribe`); writes are capped at 8 KB (`SSH_WRITE_MAX`). A 937 KB upload took about
  1.2 s on the LAN (Plan 5 notes), so about 800 KB/s.
- **No secure boot, no flash encryption.** Anyone with the Tab5 and a USB cable can flash anything.
- **Settings blob:** new fields at the end of `app_settings_t`. **This plan takes CFG 25.** CFG 22
  stays reserved for Plan 2's Task 10, which would now take 26 (a reserved lower number can't be
  used once a higher one ships, because steps are keyed `old_version < N`).
- **Timers (Plan 4 B1):** no presentation timer under 300 ms. The progress screen uses 300 ms.
- **Strings:** English only, every UI string in `i18n/i18n.h` through `tr()`, "Host" never "Mac",
  sentence case. `ui_lint.py` enforces it.

## 4. Inputs from the user and the controller

| # | Input | Source |
|---|---|---|
| U1 | No USB for each new build: after one wired flash, updates come over Wi-Fi | user |
| U2 | Suggested: pull the image from the Host over the SSH link, from a fixed or configured path | user |
| U3 | Verify before switching: SHA-256 from the Host, the app descriptor and magic, version, project name, size cap | user |
| U4 | `esp_ota_*` with app rollback; mark valid only after boot, Wi-Fi and the Host | user |
| U5 | No update below a battery floor or during a take-over or recording; hold the screen on; never sleep mid-write | user |
| U6 | About row with running and available versions; cheap check; confirm, progress, reboot; touch and keys | user |
| U7 | Console verbs for device tests, dangerous ones behind `CONFIG_COMPANION_DEBUG_COMMANDS` | user |
| U8 | One mechanism; decide what happens to the SD updater, `ota url`, `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP` | user |
| U9 | Document the one wired migration flash: what it must and must not touch | user |
| U10 | Placeholders (`my-host.local`, `you`) in every doc; no private data | user |

## 5. How the image gets to the Tab5

### 5.1 The options

| | SSH pull from the Host (recommended) | HTTPS (or HTTP) server | SD card + updater (today) |
|---|---|---|---|
| New on the Host | nothing: two fixed commands read a file | a web server (`python -m http.server`, or a launchd job), and a certificate for TLS | nothing, but you carry the card |
| Authentication | the pinned host key and the device key, as for every link command | HTTPS needs a cert the Tab5 trusts; plain HTTP (`ALLOW_HTTP`) lets anyone on the LAN serve an image | physical access |
| Left running on the Host | nothing (the README's security model: no daemon, nothing left running) | a server, unless started by hand each time | nothing |
| Code on the Tab5 | one more link request type, reusing `do_exec` | `esp_https_ota` (well tested, can resume) | `ota.c` + a 512 KB updater app with no screen, no Wi-Fi, no rollback |
| Works over Tailscale | yes, the same link | only if the server listens on the tailnet | n/a |
| Wireless | yes | yes | no: the card moves by hand, which is the chore we are removing |

**Ruling P6-1: pull over SSH.** Why:
- the Host is where the user builds, and it already answers the Tab5's SSH with a pinned key, so the
  trust and the transport exist;
- nothing is installed or left running, which is a project rule the HTTP route would bend;
- the link code already streams binary stdout on non-pty channels;
- `esp_https_ota`'s main advantage, resume, matters little at 800 KB/s (a restart costs ~8 s).

Cost if wrong: a slow link (DERP relay over Tailscale) makes the download long; the timeout scales
with size (§9.3), and an HTTPS route could be added in Plan 8 beside this one.

### 5.2 Where on the Host

**Ruling P6-2: a staging path, set in Settings › Host, default
`~/.local/share/claude-companion/claude_companion.bin`.** `tools/stage_update.sh` copies a build there
atomically (copy to `.tmp`, then `mv`) and prints its version and SHA-256. Why a staging copy and not
`firmware/build/` directly:
- a build in progress, or a build in another worktree, isn't offered by accident;
- the default path is short and the same on every Host (the repo path is personal and may pass
  63 characters);
- `~/.local/share/` is where the whisper model already lives.

The user can still point the setting at `firmware/build/claude_companion.bin` (if it fits 63
characters). A file read mid-write is caught by the checks (§7.1), so that is safe too. Cost if wrong:
one default string.

## 6. Partition layout and image size

### 6.1 What is large (main at 80ab00e, `esp_idf_size`)

| Part | Flash bytes | Note |
|---|---|---|
| `font_cjk.c` (Noto Sans Mono CJK, 24 px, 2 bpp) | 3,094,493 | the CJK and symbol fallback for the terminal and the UI |
| `font_px20.c` | 567,531 | **the same glyph data as `font_px24`** (the file differs only in its name and include guard; `fonts.h` says px20 "now uses the same 24px/2x grid") |
| `font_px24.c` | 567,531 | the UI and terminal face |
| `ffunicode.c` (FATFS) | 483,804 | `CONFIG_FATFS_CODEPAGE_DYNAMIC` links every code page's tables |
| LVGL | 438,461 | |
| alignment gap attributed to `esp_app_desc.c` | 268,225 | padding between the flash text and rodata regions; not reducible here |
| libssh | 253,910 | |
| `ge25519.c` (libssh's ed25519) | 112,806 | |
| `font_px36b.c` | 106,058 | titles |
| the rest | ~1.5 MB | lwIP, mbedTLS, MicroLink, drivers, app code |
| **image** | **7,415,488** | `.text` 1.80 MB, `.rodata` 5.47 MB |

Fonts are 4.3 MB of the 7.4 MB.

### 6.2 The diet (this plan)

1. **Share one copy of the 24 px face.** `fonts.c` makes `s_px20` a RAM copy of `font_px24` (it
   already copies each font into RAM to set fallbacks) and `font_px20.c` is deleted. No pixel
   changes. **−567,531 B.**
2. **`CONFIG_FATFS_CODEPAGE_437=y`** instead of the dynamic code page. Long file names stay UTF-8
   (`CONFIG_FATFS_API_ENCODING_UTF_8`). The OEM code page only shapes the 8.3 short names of
   non-ASCII file names, and the card only holds ASCII names (`authkey.txt`, key files,
   `servers.enc`). **About −470 KB** (measured in Task 3).
3. The SD updater and `ota.c` go (§10): a few KB.

Expected: **about 6.38 MB.** Task 3 measures it.

Not in this plan: trimming `font_cjk` (up to −2.5 MB, but it drops glyphs the terminal may need for
Claude Code output in other scripts) and `-Os` (slower UI). Both stay in reserve (§15, O4).

### 6.3 The layouts

| Layout | App room | Recovery without USB | Verdict |
|---|---|---|---|
| **A. Two OTA slots, 0x7F0000 (8,323,072 B) each** | 7.94 MB per slot: headroom 0.91 MB before the diet, **~1.95 MB (23 %) after** | the bootloader's rollback to the other slot, the previous version kept and offered in About, the boot-loop guard | **chosen** |
| B. One app slot (~13.9 MB) + a ~1.5 MB recovery app that pulls over SSH | ~13.9 MB | the recovery app, if it works | a second firmware with Wi-Fi over the C6 (esp_hosted), libssh, mbedTLS, NVS keys and pins, a display: 1.5–2.5 MB and a copy of the link code to keep in step; an update erases the only app first; no previous version |
| C. Today: one 15.4 MB app + the 512 KB SD updater | 15.4 MB | none (the updater has no screen and no rollback) | not wireless |
| D. One app + a compressed staging partition + a decompressing bootloader | ~10 MB | rollback in a custom bootloader | ESP-IDF doesn't do it; a custom bootloader is the riskiest code to get wrong and can only be fixed by USB |

**Ruling P6-3: layout A, with the diet.** Why: rollback and the previous version come from ESP-IDF's
own `esp_ota_*` and bootloader, tested by Espressif; nothing has to be written twice; 23 % headroom
covers the camera plan (Plan 7, likely +200–400 KB) with room left. Cost if wrong: if the image
outgrows 7.94 MB, the next step is the `font_cjk` trim (−2 MB or more) before any re-layout; a
re-layout costs another wired flash.

### 6.4 The new table

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
otadata,  data, ota,     0xF000,   0x2000,
phy_init, data, phy,     0x11000,  0x1000,
app0,     app,  ota_0,   0x20000,  0x7F0000,
app1,     app,  ota_1,   0x810000, 0x7F0000,
```

- The first three rows are byte-for-byte the same offsets and sizes, so NVS, otadata and phy_init
  stay where they are.
- 0x20000 + 2 × 0x7F0000 = 0x1000000: the flash is used to the end. Both app slots are 64 KB
  aligned.
- 0x12000–0x1FFFF (56 KB) stays unused, as today.
- No `factory` partition, so a blank otadata boots `app0`.
- The names change to `app0` and `app1`. Nothing looks a partition up by name after §10; the lint
  rule `ota-outside-fw_update` (plan P6-10) keeps it so.

### 6.5 The size guard
ESP-IDF's build already fails when the app doesn't fit the smallest app partition.
`tools/check_image_size.py` adds a warning when headroom drops under 1 MB and prints the headroom
after each build (Task 3 hooks it into `idf.py build` as a post-build step). Cost if wrong: noise in
the build log.

## 7. Safety

### 7.1 Download whole into PSRAM, verify, then write

**Ruling P6-4: the image is downloaded into one PSRAM buffer and fully verified before the first
flash write.** The Tab5 has 32 MB of PSRAM and the image is ~6.4 MB. Why:
- a link drop, a timeout or a cancel during the download never touches flash;
- every check, including the image's own appended SHA-256, runs before `esp_ota_begin`;
- the write phase needs no network, so a Wi-Fi drop during it changes nothing (§7.5);
- the link task only fills a buffer; it never waits on flash erases.

Cost if wrong: 8 MB of PSRAM for about 40 s. The gate refuses when the largest free PSRAM block is
smaller than the image plus 1 MB.

The checks, in order (all pure, host-tested in `ota/fw_rules.c`, except the SHA-256 computations,
which use mbedTLS's hardware SHA):

| # | When | Check | Refusal |
|---|---|---|---|
| 1 | the check (§8.2) | the info command's exit code: 0, else 120 (no readable regular file) / 121–122 (the Host's tools failed) | `FW_ERR_NO_FILE`, `FW_ERR_HOST_READ` |
| 2 | the check | the size line: decimal, ≥ 64 KB and ≤ the slot size (8,323,072 B) | `FW_ERR_TOO_BIG`, `FW_ERR_NOT_APP` |
| 3 | the check | the first 288 bytes: image magic `0xE9`, segment count 1–16, chip id `0x0012` (ESP32-P4), this chip's revision within `min_chip_rev_full`..`max_chip_rev_full`, the app descriptor magic `0xABCD5432` at offset 32, project name `claude_companion`, a non-empty NUL-terminated version | `FW_ERR_NOT_APP` |
| 4 | before the download | the gates (§7.4) | their copy |
| 5 | after the download | the byte count equals the size line | `FW_ERR_CHANGED` |
| 6 | after the download | SHA-256 of the buffer equals the Host's | `FW_ERR_CHANGED` (the file changed between the check and the read, or the read was cut) |
| 7 | after the download | checks 2–3 again on the downloaded bytes (they could differ from the check's) | as 2–3 |
| 8 | after the download | the layout walk: 24-byte header, each segment's 8-byte header and data within the file, the XOR checksum byte (seed `0xEF`) at the 16-byte boundary, then the 32-byte SHA-256 of everything before it (`hash_appended`), and the file ending exactly there | `FW_ERR_CORRUPT` |
| 9 | after the write | `esp_ota_end` (ESP-IDF's own `esp_image_verify` of the written slot) | `FW_ERR_WRITE` |
| 10 | after the write | SHA-256 of the written slot's first `size` bytes, read back from flash, equals the Host's | `FW_ERR_WRITE` |
| 11 | at the switch | `esp_ota_set_boot_partition` (verifies once more) | `FW_ERR_WRITE` |

Check 6 protects the transfer and a file replaced mid-read. It does not catch a file that is corrupt
on the Host, because the Host hashes the corrupt file; check 8 catches that (a flipped byte breaks
the appended SHA-256 or the checksum). Checks 9–11 are ESP-IDF's and the read-back's defense in
depth against a bad flash write.

### 7.2 Version and identity

**Ruling P6-5: the version string is `<version.txt>+<git short hash>[.dirty]`, and identity is the
ELF SHA-256 in the app descriptor.**
- `firmware/version.txt` becomes `1.6.0` (minor = the plan number; patch for fixes). CMake sets
  `PROJECT_VER` to `1.6.0+80ab00e` or `1.6.0+80ab00e.dirty` (under 32 characters).
- The version is compared as semver with the build part ignored (`fw_version_cmp`). Since CMake works
  out `PROJECT_VER` at configure time, the hash can be stale after a commit without a reconfigure, so
  the hash is a label only.
- **Same build** means the descriptor's `app_elf_sha256` equals the running one's (bytes 176–207 of
  the 288: descriptor offset 144). That is always right, because every rebuild with a change has a new ELF.
- Offer (`fw_offer`): same ELF → **Up to date** (no Install). Otherwise by version: **newer**;
  **older** (allowed, with an extra line in the confirm: development builds go back and forth); **same
  version, different build** (the usual case while developing); or **unknown** (a version string
  that isn't semver), treated like a different build.

Cost if wrong: labels only; nothing depends on the version string for safety.

### 7.3 Rollback

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. After `esp_ota_set_boot_partition`, the new slot is NEW;
the bootloader boots it as PENDING_VERIFY; if the app restarts for any reason before it calls
`esp_ota_mark_app_valid_cancel_rollback()`, the bootloader marks it ABORTED and boots the previous
slot.

**Ruling P6-6: the new image marks itself valid after the first successful result from the Host**
(the first watch block or poll after boot, through `cc_poll_subscribe`). That one event proves the
image booted, drew the UI (the link starts after `ui_init`; Task 6 confirms the order), joined Wi-Fi, resolved the Host,
passed the host-key pin, authenticated with the device key and parsed Claude Code's output. Why not
sooner: an image that boots but can't reach the Host is the case the user would need USB for. Cost if
wrong: a good image whose Host is unreachable right after the update goes back (§7.3 timer); the user
updates again.

**Ruling P6-7: 180 s to get there, then the image rolls itself back.** While PENDING_VERIFY, a
180 s one-shot `esp_timer` calls `esp_ota_mark_app_invalid_rollback_and_reboot()` after a 3 s notice
("The new version can't reach the Host. Going back to 1.6.0+80ab00e…"). Why: a hung or Host-less
image might otherwise run for hours and only go back at the next reset, and a Hibernate would reset
it at a moment nobody watches. 180 s covers a cold Tailscale connect (about a minute) twice over.
Cost if wrong: a slow network sends a good build back; the user retries.

While PENDING_VERIFY, `POWER_HOLD_UPDATE` keeps the screen Active (no Radio off, no Hibernate), so
the check isn't cut by a sleep that would reset the chip.

After a rollback, the old image finds `esp_ota_get_last_invalid_partition()` and the attempted
version in NVS (namespace `fw`, key `attempt`, written just before the switch) and toasts "The update
to 1.6.1+3f2a9c1 didn't start properly, so the Tab5 went back to 1.6.0+80ab00e." once, then clears
the key.

### 7.4 Gates before starting (`fw_gate`, pure)

| Gate | Refused when | Copy |
|---|---|---|
| link | `cc_link_connected()` is false | `SES_NOT_CONNECTED` (exists) |
| session | a take-over or an SSH session is open | `FW_ERR_BUSY_SESSION` |
| voice | voice isn't idle (recording or transcribing) | `FW_ERR_BUSY_VOICE` |
| busy | an update is already running | `FW_ERR_BUSY` |
| battery | on battery (`PR_SRC_BATTERY`) below **30 %** | `FW_ERR_BATTERY` |
| battery unknown | `PR_SRC_UNKNOWN` (vitals not settled, the first ~10 s after boot) | `FW_ERR_BATTERY_UNKNOWN` |
| memory | the largest free PSRAM block < image size + 1 MB | `FW_ERR_NO_MEMORY` |

**Ruling P6-8: the battery floor is 30 % on battery; on USB there is none.** Why: an update takes
about a minute at a few watts, well under 1 % of the pack; the floor is for margin against a sag
during flash erases (the brownout detector is at 2.6 V) and keeps the update far from the 10 %
Hibernate that beats a hold (Plan 2 I1), with room for the OCV estimate's error. Cost if wrong: a user
at 25 % has to plug in.

While the update runs, `voice_start` returns busy, and the progress screen owns every key and touch,
so no take-over or recording can begin.

### 7.5 Power, sleep and cut-offs

`POWER_HOLD_UPDATE` (counted like `POWER_HOLD_TAKEOVER`) is on from the confirm until the restart,
or until the update fails or is cancelled. The ladder stays Active: no dim, no screen off, no Radio
off, no deep idle, no Hibernate. The download also counts as link-busy.

What happens on each cut-off, by phase:

| Cut-off | Downloading | Checking (RAM) | Writing / read-back | After the switch, before valid |
|---|---|---|---|---|
| Wi-Fi drops, the Host sleeps, a link park | the read fails or times out: abort, buffer freed, `FW_ERR_LINK`; nothing on flash | no effect | **no effect**: the write needs no link and completes | the 180 s timer rolls back unless the link returns |
| Esc or Cancel | abort; `FW_CANCELLED` | abort | `esp_ota_abort`, the slot's first sector erased; the running otadata entry untouched (`esp_ota_begin` erases the inactive one); `FW_CANCELLED`. After the read-back passes: refused, "Too late to cancel" | n/a (the screen is gone) |
| Power loss, a brownout, a crash | the old version boots: nothing was written | same | the old version boots: the running slot and otadata were never touched; the other slot holds a partial image that nothing points to | the bootloader rolls back |
| Battery hits 10 % with the off timeout passed (Plan 2 I1) | as power loss | as power loss | as power loss | as power loss |

**Ruling P6-9: no resume.** A cut download starts again from zero. Why: about 8 s on the LAN, and a
resume needs offsets on the Host (`tail -c +N`) and partial-state bookkeeping for little gain. Cost if
wrong: slower retries over a slow relay.

A cancel or error during the read drops the SSH session (the link reconnects at once), the same as a
timeout today, so a half-read channel never leaves libssh in an odd state. `cat` on the Host gets
SIGPIPE and exits.

### 7.6 Recovery without USB

1. **Not marked valid** (crashes, hangs, can't reach the Host): the bootloader or the 180 s timer goes
   back (§7.3).
2. **Marked valid but broken later, UI still usable:** Settings › About › **Previous version** shows
   the other slot's version and a **Go back** action, if that slot holds a runnable image (its state is
   VALID or UNDEFINED, its descriptor reads, its project is `claude_companion`, and it passes
   `esp_image_verify` when Go back is confirmed).
3. **Marked valid, then crash-loops:** **Ruling P6-11: the boot-loop guard.** An `RTC_NOINIT` record
   holds a count and an "early" flag. Each boot sets "early"; 60 s after boot it is cleared and the
   count reset. At the next boot, a reset reason of panic, interrupt watchdog or task watchdog with
   "early" still set adds one. At 3, `fwu_boot_init` switches to the other slot if it is runnable (2.)
   and restarts, before Wi-Fi or the UI; the old image then toasts "The Tab5 kept restarting, so it
   went back to 1.6.0+80ab00e." Brownouts and power-on resets don't count. Why: a validated build that
   panics 10 s after every boot would otherwise need USB. Cost if wrong: about 40 lines and an RTC word;
   a build that crashes early three times for another reason (a bad SD card?) goes back, which is
   harmless.
4. **Both slots broken, or the bootloader:** USB, `idf.py -p PORT flash` (keeps NVS). The README says
   so.

### 7.7 Trust model

The Host account is the root of trust: whoever can write the file at the firmware path, or edit the
setting, can install firmware. That account already runs Claude Code with the user's privileges and
accepts the Tab5's key, and anyone holding the Tab5 can flash anything over USB because there is no
secure boot. The SSH channel is authenticated by the pinned host key and the device key, and SSH's
MAC protects the bytes in transit.

**Ruling P6-12: no image signing in this plan.** Without secure boot, a signature checked by the app
only stops a remote attacker who already controls the Host account, and that attacker controls the
Claude Code sessions anyway. Signing becomes worth it with a public release channel (Plan 8). Cost if
wrong: a later plan adds an ECDSA check in `fw_update.c` and a signing step in `stage_update.sh`.

## 8. UX

### 8.1 Settings › About

```
Version                  1.6.0+80ab00e · Oct 25 2026
Update firmware          1.6.1+3f2a9c1 available          ← action row
Previous version         1.5.9+0c1d2e3 · Go back          ← only when the other slot is runnable
  Builds come from the Host over SSH (Settings › Host › Firmware file on Host). See README › Update over Wi-Fi.
System
Diagnostics …  Reboot  Reset settings (unchanged)
```

- **Update firmware**'s value: "Checking…" (muted) while the check runs; "Up to date" (muted);
  "1.6.1+3f2a9c1 available" (`fg`); "1.6.1+3f2a9c1 (older) available" (`fg`); or the error in red,
  shortened to the row ("No firmware file on the Host"). Before any check: "Check".
- When About is shown, one check runs in the background (posted to the Sessions worker as the voice
  Check is, P5-5), unless a check is less than 60 s old. Activating the row re-checks when the last
  check is older than 60 s or failed, then opens the confirm when an image is available. "Up to date"
  activations just re-check.
- **Settings › Host** gains **Firmware file on Host** (value row with a prompt; refused with the
  existing path copy unless `cc_path_valid`). Reset settings keeps it, like the other Host paths.

### 8.2 The check (cheap)
One exec, `cc_cmd_fw_info` (§9.1): the Host prints the size, the SHA-256 and the first 288 bytes. On
the Host that is one `wc`, one `shasum` of ~6.4 MB (tens of ms) and a `head`; on the wire, under
400 bytes. The Tab5 parses it (`fw_parse_info`), runs checks 1–3 and `fw_offer`, and caches the result
for 60 s.

### 8.3 The confirm
`ui_confirm_ex` with:
- title "Install 1.6.1+3f2a9c1?";
- body "The Tab5 downloads it from the Host, checks it and restarts. It takes about a minute. If the
  new version doesn't start, the Tab5 goes back to 1.6.0+80ab00e by itself."; for an older build the
  body starts with "This is older than the running version." on its own line;
- buttons Cancel and **Install**, with the shared confirm rule (main's stop-hotkey): opened by a key, Cancel has the
  focus and the arrows never reach Install; Tab (or a tap) reaches it, then Enter installs. Esc cancels. When Install
  is pressed the staged build must still be the one the confirm named (its SHA-256); if another build was staged
  meanwhile, a new confirm names it (with the older line where it applies) and nothing starts (fix round 1, I3).

The gates (§7.4) run when Install is pressed; a refusal toasts its copy and nothing starts.

### 8.4 The progress screen
A full-screen modal on `lv_layer_top`, built once per update:
- title "Updating firmware" (`FONT_TITLE`);
- the phase line: "Downloading from the Host", "Checking the download", "Writing", "Checking what
  was written", "Restarting…";
- an `lv_bar` 600 × 12 px, indicator `fg`, track `line`, the phase's percent (`fw_pct`);
- the bytes line: "3.2 of 6.4 MB · 50 %" (`fw_mb_text`, MB = 10^6 B, one decimal);
- the hint (muted): "Keep the Tab5 on. Esc cancels; nothing changes until the restart.";
- a **Cancel** button (80 px hit area). From "Restarting…" on, the hint and Cancel are hidden.

**Ruling P6-13: per-phase percent, not one weighted bar.** The download and write speeds differ by
link and flash, so a single bar would jump or stall; the phase line says what the bar measures. Cost
if wrong: one function.

A 300 ms LVGL timer refreshes it from `fwu_status()` (B1-compliant) and is deleted with the screen.
Keys: **Esc** cancels; every other key is eaten (including Ctrl+P, Ctrl+K and the footer chips: there
is no footer). Touch: Cancel; everything else is eaten. On a failure the screen closes and the
error toasts (6 s) on About.

After the restart, the new image toasts "Updated to 1.6.1+3f2a9c1" when it marks itself valid.

### 8.5 Touch and keyboard
| Step | Keyboard | Touch |
|---|---|---|
| Reach About | Ctrl+P, then the section list (arrows, Enter) | tap Settings, tap About |
| Check / install | focus **Update firmware**, Enter | tap the row |
| Confirm | Tab to Install, Enter / Esc | tap Install / Cancel |
| Cancel the update | Esc | tap Cancel |
| Go back | focus **Previous version**, Enter, then Tab to Go back, Enter | tap the row, tap Go back |

### 8.6 Copy (exact; all in `i18n.h`)

| Id | English |
|---|---|
| `SET_FW_UPDATE` | Update firmware |
| `SET_FW_CHECK` | Check |
| `SET_FW_CHECKING` | Checking… |
| `SET_FW_UP_TO_DATE` | Up to date |
| `SET_FW_AVAILABLE` | %s available |
| `SET_FW_AVAILABLE_OLDER` | %s (older) available |
| `SET_FW_PREVIOUS` | Previous version |
| `SET_FW_GO_BACK` | Go back |
| `SET_FW_NOTE` | Builds come from the Host over SSH (Settings › Host › Firmware file on Host). See README › Update over Wi-Fi. |
| `SET_FW_PATH` | Firmware file on Host |
| `SET_FW_PATH_PH` | ~/.local/share/claude-companion/claude_companion.bin |
| `FW_CONFIRM_Q` | Install %s? |
| `FW_CONFIRM_BODY` | The Tab5 downloads it from the Host, checks it and restarts. It takes about a minute. If the new version doesn't start, the Tab5 goes back to %s by itself. |
| `FW_CONFIRM_OLDER` | This is older than the running version. |
| `FW_INSTALL` | Install |
| `FW_TITLE` | Updating firmware |
| `FW_PHASE_DOWNLOAD` | Downloading from the Host |
| `FW_PHASE_CHECK` | Checking the download |
| `FW_PHASE_WRITE` | Writing |
| `FW_PHASE_VERIFY` | Checking what was written |
| `FW_PHASE_RESTART` | Restarting… |
| `FW_BYTES` | %s · %d %% |
| `FW_HINT` | Keep the Tab5 on. Esc cancels; nothing changes until the restart. |
| `FW_UPDATED` | Updated to %s |
| `FW_ROLLED_BACK` | The update to %s didn't start properly, so the Tab5 went back to %s. |
| `FW_ROLLING_BACK` | The new version can't reach the Host. Going back to %s… |
| `FW_BOOTLOOP` | The Tab5 kept restarting, so it went back to %s. |
| `FW_GO_BACK_Q` | Go back to %s? |
| `FW_GO_BACK_BODY` | The Tab5 restarts into the version it ran before the last update. |
| `FW_CANCELLED` | Update cancelled. Nothing was changed. |
| `FW_ERR_LINK` | Lost the connection to the Host. Nothing was changed. Try again. |
| `FW_ERR_TIMEOUT` | The Host took too long to send the firmware. Nothing was changed. Try again. |
| `FW_ERR_NO_FILE` | No firmware file on the Host at the firmware path (Settings › Host). |
| `FW_ERR_HOST_READ` | The Host couldn't read the firmware file. |
| `FW_ERR_NOT_APP` | That file isn't Claude Companion firmware for the Tab5. |
| `FW_ERR_TOO_BIG` | That firmware is too large for the Tab5's update slot. |
| `FW_ERR_CORRUPT` | The firmware file is damaged. Build it again on the Host. |
| `FW_ERR_CHANGED` | The file on the Host changed during the download. Try again. |
| `FW_ERR_WRITE` | Writing the update failed. The current version stays installed. |
| `FW_ERR_BATTERY` | Charge the Tab5 to 30 % or plug it in, then update. |
| `FW_ERR_BATTERY_UNKNOWN` | Checking the battery. Try again in a few seconds. |
| `FW_ERR_BUSY_SESSION` | Close the session first, then update. |
| `FW_ERR_BUSY_VOICE` | Wait for voice input to finish, then update. |
| `FW_ERR_BUSY` | An update is already running. |
| `FW_ERR_NO_MEMORY` | Not enough memory for the update. Restart the Tab5 and try again. |
| `SES_NOT_CONNECTED` (exists) | Not connected to the Host |
| `SET_CLAUDE_PATH_BAD` (exists, reused) | Use only letters, digits and _ . / ~ - (1 to 63 characters) |
| `SET_HOST_NOTE` (changed) | The Host is one of your SSH hosts. The Tab5 lists its Claude Code sessions, runs claude from this path, runs whisper-cli for voice input and reads firmware updates from it. |

The row value for an error uses the first sentence of the error copy. `FW_BYTES` takes the
`fw_mb_text` output ("3.2 of 6.4 MB") as `%s`.

### 8.7 Console verbs

| Verb | Build | What it does |
|---|---|---|
| `ota` / `ota status` | always | `ota: running 1.6.0+80ab00e (app0, valid), other 1.5.9+0c1d2e3 (app1, valid), rollback possible: yes, pending: no, bootloop 0` |
| `ota check` | always (read-only on the Host) | runs the check and prints `ota check: host 1.6.1+3f2a9c1, 6374957 B, sha256 3f2a9c1e…, newer` or `ota check: <copy>, rc <n>` |
| `ota path [<path>]` | always | prints or sets the firmware path (like `companion claude-path`) |
| `ota update` | debug | the same service as the UI, without the confirm; prints a line per phase and every 10 %; ends `ota update: restarting` or `ota update: <copy>` |
| `ota update corrupt-ram` | debug | flips one byte of the PSRAM buffer after the download (proves check 6) |
| `ota cancel` | debug | `fwu_cancel()` from the console while an update runs (from a second console line) |
| `ota go-back` | debug | the Previous version action without the confirm |
| `ota test-bootloop <1-3>` | debug | the next n boots panic 5 s after boot (an `RTC_NOINIT` counter; proves §7.6.3) |

`CONFIG_COMPANION_OTA_TEST_NO_VALIDATE` (Kconfig, depends on `CONFIG_COMPANION_DEBUG_COMMANDS`,
default n) builds an image that never marks itself valid, for the rollback gate. It logs
`fw: test build: never marks itself valid` at boot.

The old `ota url` verb is removed (§10).

## 9. The Host side

### 9.1 The commands (exact text; `F` is the validated firmware path)

Info (`cc_cmd_fw_info`):

```sh
exec /bin/sh -c 'F=<path>; [ -f "$F" ] && [ -r "$F" ] || exit 120; S=$(wc -c < "$F") || exit 121; H=$(shasum -a 256 < "$F" 2>/dev/null || sha256sum < "$F") || exit 122; echo $S ${H%% *}; exec head -c 288 "$F"'
```

Read (`cc_cmd_fw_read`):

```sh
exec /bin/sh -c 'F=<path>; [ -f "$F" ] && [ -r "$F" ] || exit 120; exec cat "$F"'
```

- Both run on **non-pty** exec channels: a pty would translate the binary bytes.
- `exec /bin/sh -c '…'` gives POSIX semantics whatever the login shell is (the `cc_cmd_watch` and
  `cc_cmd_transcribe` pattern). `F=~/…` gets tilde expansion in the assignment; `"$F"` is quoted at
  each use. The path can't contain `'`, so the single-quoted argument is safe.
- `$S` unquoted drops the leading spaces macOS `wc` prints. `${H%% *}` keeps the hash and drops the
  ` -` that `shasum` and `sha256sum` print after it. `shasum` is on macOS; `sha256sum` on Linux.
- Exit 120: no readable regular file (missing, a directory, no permission). 121: `wc` failed. 122:
  no hash tool worked. 126/127 are the shell's own.
- **Nothing is written or left running on the Host.** `head` and `cat` end at EOF; if the Tab5
  vanishes, `cat` dies on SIGPIPE at its next write.
- In C, the `%%` of `${H%% *}` is `%%%%` in the `snprintf` format (Task 2).

### 9.2 Output and parsing
Info output: one line `<size> <64 lowercase hex>\n`, then exactly `min(288, size)` raw bytes.
`fw_parse_info` refuses anything else (no line, a non-decimal size, a size over 10 digits, a hash that
isn't 64 hex characters, a header shorter than `min(288, size)`).

Read output: exactly `size` bytes; anything else is `FW_ERR_CHANGED` (check 5).

### 9.3 Timeouts
- Info: 10 s (the link's usual exec budget).
- Read: **20 s + size / 64 KB/s**: about 120 s for 6.4 MB. On the LAN it takes about 8 s; over a DERP
  relay, Plan 5 measured uploads well under that rate's floor. Task 5 records the real rates (O1).

### 9.4 The Host workflow
`firmware/tools/stage_update.sh [path/to/claude_companion.bin]` (default `build/claude_companion.bin`
next to the script's `..`):
1. checks the file starts with `0xE9`, has the descriptor magic at offset 32 and the project name
   `claude_companion` at offset 80;
2. `mkdir -p ~/.local/share/claude-companion`;
3. copies to `claude_companion.bin.tmp` there, then `mv -f` onto `claude_companion.bin` (atomic on one
   file system);
4. prints `staged 1.6.1+3f2a9c1, 6374957 B, sha256 3f2a9c1e…` (version read from the descriptor at
   offset 48).

The README suggests one alias:

```sh
alias tab5-build='(cd ~/src/claude-companion/firmware && idf.py build && sh tools/stage_update.sh)'
```

`STAGE_DIR` overrides the directory for tests. The script writes only inside that directory.

## 10. What happens to the old path

**Ruling P6-14: one mechanism; the SD route is deleted.**

| Thing | Fate | Why |
|---|---|---|
| `firmware/updater/` (the 512 KB factory updater app) | deleted | no partition for it; no screen, no rollback; git history keeps it |
| `firmware/main/ota/ota.c`, `ota.h` | deleted; `ota/` now holds `fw_rules.*`, `fw_update.*`, `fw_console.c` | its HTTP manifest and SD staging have no callers and no place in the new design |
| `ota url` console verb | removed; `ota status|check|path|…` replace it | |
| `settings.ota_url[160]` | stays in place as **reserved**; CFG 25 zeroes it | the blob layout must not move; the field may hold a token (it was printed masked) |
| `url_mask` (`sys/url_mask.*`) and its test | kept only if another caller exists, else deleted with its test | Task 4 decides by grep |
| `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y` | removed from `sdkconfig.defaults` | nothing uses `esp_https_ota` |
| `esp_https_ota` in `main/CMakeLists.txt` | removed from `PRIV_REQUIRES` (`esp_http_client` stays: MicroLink uses it) | |
| `tools/merge_flash.sh` | rewritten: no updater, no `update.json`, the new offsets; prints a warning that the full image overwrites NVS and is for blank devices only | |
| `storage/sd.h`'s OTA comment | updated | |
| README mentions (§1 "factory-updater partition", Status, Known limitations "No firmware updates over the air") and `firmware/README.md` (layout, partitions, Release images, "OTA (dormant)") | rewritten | |
| `THIRD_PARTY_NOTICES.md` | checked for the updater; updated if it is named | |

## 11. Migration: the one wired flash

The first build with this plan has the new partition table and a bootloader built with rollback. Both
can only arrive over USB. Once, with the Tab5 plugged in:

```sh
cd firmware
rm -f sdkconfig                  # a stale one keeps rollback off (it is git-ignored; this build makes a new one)
idf.py reconfigure
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash
```

**Delete `firmware/sdkconfig` first.** ESP-IDF applies `sdkconfig.defaults` only to options an existing
`sdkconfig` doesn't have yet, and an older checkout's `sdkconfig` says `# CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is
not set`: kept, that builds a bootloader that can't go back. The build refuses such a configuration with an
`#error` that says so (`ota/fw_update.c`), and About shows "Rollback off" if a Tab5 ever runs without it.

**It must write:** the bootloader (0x2000: rollback lives there), the partition table (0x8000),
`ota_data_initial.bin` (0xF000: a blank otadata, so the bootloader boots `app0`), and the app
(0x20000). That is exactly what `idf.py flash` writes.

**It must not touch:** NVS (0x9000–0xEFFF: Wi-Fi, the device key, host-key pins, saved hosts,
settings, Tailscale identity), the ESP32-C6, or any eFuse.

**So don't use:**
- `idf.py app-flash` (no bootloader or partition table: the new layout wouldn't be there, and the
  first OTA would fail to find `app1`);
- `idf.py erase-flash`, `esptool erase-flash` or `erase-region` over 0x9000;
- the merged full image at 0x0 (`merge_flash.sh`'s output fills the gaps with 0xFF, which erases NVS);
- `espefuse.py` anything.

**Why it is safe:** the first three partitions keep their offsets and sizes, so NVS is read in place.
The old updater's bytes at 0xF80000 now sit inside `app1` and mean nothing: otadata doesn't point to
`app1`. An update erases only as far as the image it writes (to about 0xE20000), so the updater's bytes past it stay;
nothing points to them (fix round 1, M12).

**Before it** (recommended): read NVS out to a private file, which is read-only for the device:
`python -m esptool --chip esp32p4 -p PORT read-flash 0x9000 0x6000 nvs-backup.bin`. It holds the
device key and Wi-Fi passwords: keep it out of git, `chmod 600`, delete it once the update path is
proven. Writing it back is a restore of last resort only.

From then on every build goes over Wi-Fi. USB is needed again only for a bootloader or partition
change, the C6, or a Tab5 with both slots broken.

## 12. Firmware design

### 12.1 Modules

| Module | Kind | Job |
|---|---|---|
| `companion/cc_cmd.c` | pure, host-tested | `cc_cmd_fw_info`, `cc_cmd_fw_read`, `CC_FW_PATH_DEFAULT`, the exit codes |
| `ota/fw_rules.c` | pure, host-tested | info parse, header checks, layout walk, version compare, offer, progress maths, gates, validity and boot-loop rules, rc → outcome → string |
| `companion/cc_link.c` | firmware | `cc_link_fw_info`, `cc_link_fw_read` (binary out, progress, cancel, the read timeout); the path getter and setter |
| `ota/fw_update.c` | firmware | the update task, the PSRAM buffer, SHA-256, `esp_ota_*`, the boot check, the validity timer, the boot-loop guard, Go back, the result notes. **The only file that calls `esp_ota_*` or writes an app partition.** |
| `ota/fw_console.c` | firmware | the `ota` verbs |
| `ui/ui_settings.c` | firmware (preview-tested) | the About rows, the Host path row, the confirm |
| `ui/ui_fwupdate.c` | firmware (preview-tested) | the progress screen |
| `settings/*` | pure migration + firmware | `fw_path`, CFG 25, `ota_url` wiped |
| `sys/power.*` | firmware | `POWER_HOLD_UPDATE` |
| `fonts/fonts.c`, `sdkconfig.defaults`, `partitions.csv`, `CMakeLists.txt`, `version.txt` | build | the diet, rollback, the layout, the version |

### 12.2 The update task (`fwu_start` → task, 6 KB internal stack)
1. `POWER_HOLD_UPDATE` on. Phase DOWNLOADING.
2. `heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM)`.
3. `cc_link_fw_read(buf, size + 1, &len, &progress, &cancel, &rc, CC_FW_READ_TIMEOUT_MS(size))`.
   ESP_ERR_INVALID_STATE / ESP_FAIL → `FW_ERR_LINK`; ESP_ERR_TIMEOUT → `FW_ERR_TIMEOUT`;
   `CC_FW_CANCELLED` → `FW_CANCELLED`; rc ≠ 0 → `fw_host_rc(rc)`; `len != size` → `FW_ERR_CHANGED`.
4. Phase CHECKING. SHA-256 (mbedTLS), compare (check 6); `fw_check_header` on the buffer (check 7);
   `fw_image_layout` and the appended hash (check 8).
5. Phase WRITING. `esp_ota_get_next_update_partition(NULL)`; `esp_ota_begin(next,
   OTA_WITH_SEQUENTIAL_WRITES, &h)`; `esp_ota_write` in 16 KB chunks, progress after each, cancel
   checked between chunks (`esp_ota_abort`); `esp_ota_end` (check 9).
6. Phase READBACK. `esp_partition_read` in 16 KB chunks into one reused 16 KB PSRAM block, SHA-256,
   compare (check 10).
7. Write NVS `fw/attempt` = the new version. `esp_ota_set_boot_partition(next)` (check 11). Phase
   RESTARTING. Free the buffer (nothing is zeroed: the image isn't secret, P6-15). Wait 1 s for the screen, then `link_down_restart("firmware
   update")`.
8. On every failure: `esp_ota_abort` if begun, free the buffer (one `release()` helper, as voice's
   `wipe_buffer()`), hold off, post the outcome, phase IDLE, the task deletes itself.

Log line, once per update: `fw: 1.6.0+80ab00e -> 1.6.1+3f2a9c1, 6374957 B, download 7.9 s, check
0.4 s, write 21.3 s, readback 1.1 s, <outcome>`.

### 12.3 Boot (`fwu_boot_init`, early in `app_main`, before Wi-Fi)
1. The boot-loop guard (§7.6.3): update the RTC record from `esp_reset_reason()`; at 3, switch and
   restart.
2. The test hook: if `ota test-bootloop` armed a count, arm a 5 s timer that calls `abort()`.
3. `esp_ota_get_state_partition(running)`: PENDING_VERIFY → hold on, the 180 s timer, and subscribe
   to `cc_poll_subscribe` → on the first result, `esp_ota_mark_app_valid_cancel_rollback()`, hold off,
   timer deleted, clear `fw/attempt`, post `FW_UPDATED` for Sessions to toast.
4. Else, if `fw/attempt` is set and `esp_ota_get_last_invalid_partition()` is not NULL: post
   `FW_ROLLED_BACK` with the attempted and running versions; clear `fw/attempt`.
5. Check-in boots (Hibernate) run 1 and 3 too. A check-in boot can't be PENDING_VERIFY in practice
   (the hold keeps an unvalidated image Active), but if it is, it stays awake for the check like any
   other boot.

### 12.4 Memory
- PSRAM: the image buffer (size + 1) for the download through step 7, plus a 16 KB read-back block.
- Internal: the 6 KB task stack, the SHA-256 context, `esp_ota` handles. No LVGL objects outside the
  progress screen, whose small styles come from `ui_comp`'s static set.
- Device gate: `ui heapmin` over a whole update stays ≥ 40,960 B with Tailscale off and ≥ 20,480 B
  with it on (the Plan 4 B2 floors).

### 12.5 Link details
- `cc_link_fw_read` is a `REQ_EXEC` with three new request fields: `out_len` (binary length out),
  `progress` (bytes so far, written after each read) and `cancel` (checked each loop).
- For speed the loop reads straight into the output buffer (up to 16 KB per
  `ssh_channel_read_timeout`) instead of the 512-byte bounce buffer, when the request is a firmware
  read.
- The watch keepalive runs every second during the read, as during a stdin upload, so the watch
  doesn't time out; other requests queue behind it (the session list pauses for the download's
  seconds).
- Both link functions build their own command from the setting (as P5-2): no caller passes a
  command line.

## 13. Failure modes

| What happens | What the user sees | Notes |
|---|---|---|
| No file at the path | the row: "No firmware file on the Host"; Install not offered | exit 120 |
| The path is a directory or unreadable | same | exit 120 |
| No hash tool on the Host | "The Host couldn't read the firmware file." | exit 122 |
| A different project's `.bin`, the bootloader, a text file | "That file isn't Claude Companion firmware for the Tab5." | check 3, before the download |
| An image for another chip | same | chip id |
| Bigger than the slot | "That firmware is too large…" | check 2, before the download |
| A byte flipped on the Host | "The firmware file is damaged. Build it again on the Host." | check 8, before any write |
| Truncated file (a build still writing it) | "damaged" (layout) or "changed" (size) | checks 5, 8 |
| Replaced between the check and the read | "The file on the Host changed during the download." | check 6 |
| Already up to date | the row says "Up to date"; no Install | same ELF SHA-256 |
| An older build | the confirm adds "This is older than the running version." | allowed |
| Wi-Fi drops / the Host sleeps / Radio off parks the link during the download | "Lost the connection to the Host. Nothing was changed." | buffer freed |
| A slow relay | the bar moves slowly; at the timeout "The Host took too long…" | 20 s + size / 64 KB/s |
| Wi-Fi drops during the write | nothing: it completes and restarts; the new image rolls back if the link isn't back in 180 s | |
| Esc / Cancel | "Update cancelled. Nothing was changed." | any phase before Restarting |
| Battery under 30 % on battery | "Charge the Tab5 to 30 % or plug it in, then update." | |
| Battery source not known yet | "Checking the battery. Try again in a few seconds." | first ~10 s after boot |
| A take-over / SSH session open | "Close the session first, then update." | |
| Voice recording or transcribing | "Wait for voice input to finish, then update." | |
| Not enough PSRAM | "Not enough memory for the update…" | |
| Flash write or verify fails | "Writing the update failed. The current version stays installed." | `esp_ota_abort` |
| Power lost at any point before the switch | the old version boots as if nothing happened | the running slot and otadata untouched |
| The new image crashes or hangs before reaching the Host | it goes back by itself (at the next reset, or after 180 s), then "The update to … didn't start properly…" | |
| The new image reaches the Host, then crash-loops | after three early crashes it goes back: "The Tab5 kept restarting…" | §7.6.3 |
| The new image works but has a bug the user doesn't like | About › Previous version › Go back | |
| Both slots broken | USB: `idf.py flash` (keeps NVS) | README |

## 14. Test strategy

### Host unit tests (`firmware/test/host`)
- `test_cc_cmd.c`: the exact info and read strings for the default path; rejection of every bad path
  (space, `'`, `;`, `$`, backtick, newline, `"`, `|`, `&`, empty, 64 characters, NULL) and of a small
  `cap` (out cleared); exactly two `'` in the output.
- `test_fw_rules.c`: info parsing; header checks (each refusal from a synthetic 288-byte header); the
  layout walk on synthetic images (one and three segments; a truncated segment; a bad checksum; extra
  trailing bytes; a segment count of 0 and 17); version compare (semver order, build part ignored,
  pre-release lower, `v` prefix, missing parts, non-semver → UNKNOWN); the offer; progress maths
  (0, 1 byte, 99.9 %, done, total 0, MB text rounding); every gate and its order; the validity rule;
  the boot-loop rule; rc → outcome → `STR_*`, none containing "Mac".
- `test_settings_migrate.c`: v24 → v25 sets the default `fw_path` and zeroes `ota_url`; a v25 blob
  keeps an edited path; an invalid stored path falls back; Reset settings keeps it; a newer (v26) blob
  copies what fits.

### Python tests (`firmware/tools`)
- `test_fw_cmd.py` runs the real §9.1 strings (from `cc_cmd_print fwinfo|fwread <path>`) under
  `/bin/sh -c` and `/bin/zsh -c` with a temp `HOME` and `PATH=/usr/bin:/bin`: the info line matches
  Python's own size and `hashlib.sha256`; the header bytes equal the file's first 288 (and the whole
  file when it is smaller); read output equals the file byte for byte; a tilde path works; exit 120
  for a missing file, a directory and a `chmod 000` file; no process left (`pgrep -f <tmpdir>`); no
  file created under the temp `HOME`.
- The same file runs `fw_print check <bin>` (a host build of `fw_rules.c`) on the real
  `firmware/build/claude_companion.bin` when it exists (skipped otherwise): header OK, layout OK, and
  the appended SHA-256 verified by Python independently; then on copies with one byte flipped in the
  middle (layout or hash refuses), cut by 1,000 bytes (truncated), the project name patched (header
  refuses `PROJECT`) and padded to 9 MB (too big).
- `test_stage_update.py`: `stage_update.sh` with `STAGE_DIR` in a temp dir: the copy is byte-equal;
  no `.tmp` left; a non-image file is refused with exit 1 and nothing staged; the printed version
  matches the descriptor.
- `test_check_image_size.py`: the headroom maths against a fixture `partitions.csv` and the warning
  threshold.
- `test_ui_lint.py`: the new rules (plan P6-10).

### Preview (`firmware/tools/ui_preview`)
`settings 5` (About) variants for each row state, the confirm (`PREVIEW_CLICK=Update firmware`), the
progress screen (`fwupdate download|write|readback|restart`), Esc and Cancel routing, the fit check,
the amber scan, and the timer check (one 300 ms timer while the progress screen is up, none after).
`settings 0` shows the Host path row.

### Device gates (Task 9)
An update from a Host path by touch and by keyboard, and back again (both slots); every refusal;
the no-validate image rolled back (by reset and by the 180 s timer); the boot-loop guard; power holds;
Wi-Fi drops in the download and in the write; heap; NVS kept through the migration, an update, a
rollback and Go back; nothing left on the Host.

## 15. Open items for the builder
- **O1. Rates.** Download speed on the LAN and over Tailscale (DERP); flash write speed with
  sequential erases; total time. If the LAN download is under 500 KB/s, look at the direct-read path
  (§12.5) before anything else.
- **O2. UI during flash writes.** With `CONFIG_SPIRAM_XIP_FROM_PSRAM`, code and rodata run from PSRAM,
  so writes shouldn't stall the display. Confirm the progress screen keeps updating during the write.
- **O3. The layout walk on the real build.** Checked offline on main's `build/claude_companion.bin`
  (2026-10-25, read-only): magic `0xE9`, 9 segments, chip id 18, chip revisions 0–199, descriptor magic
  `0xABCD5432`, project `claude_companion`, version `1.0`; the XOR checksum (`0xE3`) matches, the
  appended SHA-256 matches, and the file ends exactly after it (7,415,488 B, no padding). Task 2's
  real-image test keeps checking it on every build.
- **O4. Image size reserve.** If a later plan pushes the image past 7 MB, the next steps are the
  `font_cjk` range trim (needs the user's call on which scripts the terminal must show) and then `-Os`.
- **Roadmap.** Plan 6 is this plan; the camera becomes Plan 7 and the release Plan 8. The README and
  the main spec's roadmap change in Task 8.

## 16. Rulings (index)
1. P6-1: pull the image over the SSH link (§5.1).
2. P6-2: a staging path in Settings › Host, default `~/.local/share/claude-companion/claude_companion.bin`, filled by `stage_update.sh` (§5.2).
3. P6-3: two 0x7F0000 OTA slots, with the image diet (§6.3).
4. P6-4: download whole into PSRAM; verify everything before the first flash write (§7.1).
5. P6-5: version `<version.txt>+<hash>`; identity by ELF SHA-256 (§7.2).
6. P6-6: valid after the first successful result from the Host (§7.3).
7. P6-7: 180 s to become valid, then roll back (§7.3).
8. P6-8: battery floor 30 % on battery, none on USB (§7.4).
9. P6-9: no resume (§7.5).
10. P6-10 (plan): lint rules keep `esp_ota_*` and app-partition writes in `fw_update.c`, and the firmware commands in `cc_cmd.c`.
11. P6-11: the boot-loop guard (§7.6).
12. P6-12: no image signing yet (§7.7).
13. P6-13: per-phase progress (§8.4).
14. P6-14: one mechanism; the SD updater is deleted (§10).
15. CFG 25 (§3).

## 17. Fix round 1 (two reviews, 2026-10-25)

What changed in the design, in the order of the review findings:

- **C1, a stale `sdkconfig`.** The migration steps (§11, README) delete `firmware/sdkconfig` and run
  `idf.py reconfigure` first. `ota/fw_update.c` has an `#error` when `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is off
  or `CONFIG_FATFS_CODEPAGE_DYNAMIC` is on. At boot, a running image in state NEW means the bootloader has no
  rollback (one with rollback turns NEW into PENDING_VERIFY): the Tab5 logs it, marks the image valid, and turns
  updates off. About's Version row then says "· Rollback off", the Update firmware row reads "Rollback off" (red)
  and runs no check, and Install refuses with `FW_ERR_NO_ROLLBACK`.
- **I1/I2 (UI), the confirm.** The Install and Go back confirms use main's shared rule: opened by a key, Cancel has
  the focus and the arrows never reach the action; Tab or a tap does (§8.3, §8.5).
- **I3 (UI), a stale confirm.** The confirm keeps the SHA-256 of the build it names. Install starts only if the
  cached check still holds that SHA-256 (`fwu_start_expect`, `fw_confirm_matches`). A stale check is re-run; if the
  staged build changed, a new confirm names it and nothing starts.
- **I1 (core), the guard's and Go back's target.** `esp_ota_set_boot_partition` puts any target on probation. A
  switch by the guard or Go back sets an `RTC_NOINIT` flag first; the next boot marks that image valid at once
  (`fw_boot_state`: TRUSTED), with no 180 s check and no "Updated" toast. **M7:** after one guard switch with no
  stable boot (60 s) in between, the guard gives up instead of switching back (`fw_bootloop_action`: GIVE_UP) and
  shows `FW_BOOTLOOP_BOTH`.
- **I2 (core), the holds.** The update task holds `POWER_HOLD_UPDATE`; a pending image holds
  `POWER_HOLD_FW_PENDING`; neither clears the other's. Install refuses while the running image is pending
  (`FW_GATE_PENDING`, `FW_ERR_PENDING`).
- **M1.** The 180 s rollback runs on a task of its own: it closes SSH (`lg_close`), then calls
  `esp_ota_mark_app_invalid_rollback_and_reboot`; if that returns, the hold ends, the image stays pending (the next reset still goes back) and
  `FW_ROLLBACK_FAILED` toasts. (Round 1 left the link parked after a failed rollback, so the claim that a later Host
  result could still mark it valid was wrong; round 2, N3, reopens the link with `lg_reopen`.)
- **M2.** `s_pending` is tested and cleared under the lock by both mark-valid and the rollback.
- **M3.** A failed or cancelled write erases the other slot's first sector, so it is never offered as Previous
  version. (A power cut mid-write can still leave a partial image there; Go back's `esp_image_verify` refuses it.)
- **M4.** `fwu_boot_early` (the guard and the trusted mark-valid) is the first call in `app_main`. The guard also
  counts `ESP_RST_WDT` and `ESP_RST_CPU_LOCKUP`. The task watchdog is not set to panic: the idle-task checks are off
  (`sdkconfig.defaults`) and no task subscribes, so a panic option would change nothing; a hang that never resets
  is left to the user (Reboot) and the 180 s check for a pending image.
- **M5.** "Didn't start properly" shows only when the invalid slot's version equals `fw/attempt`
  (`fw_attempt_rolled_back`).
- **M6.** The phase becomes RESTARTING once the read-back passes, before `fw/attempt` and the switch; a cancel from
  then on is refused with "Too late to cancel…" (`fwu_cancel` returns false).
- **M8.** The firmware path may not start with `-` (`cc_fw_path_valid`): `cat` and `head` would take it as an option.
- **M9.** The flash-write lint also covers `esp_flash_write*`, `esp_flash_erase*`, `spi_flash_write*`,
  `spi_flash_erase*`, `bootloader_flash_write*` and `bootloader_flash_erase*`, and scans `firmware/components`
  (no vendored component writes flash; none is excluded).
- **M1/M10 (UI).** Go back runs on the Sessions worker with Install's gates (busy, a session, voice, the battery).
  **M2:** never two Install confirms. **M3:** the progress screen takes only RESULT notes. **M4:** a console
  `ota update` opens the progress screen too; the console printer only peeks at the note.
- **M11.** `cc_poll_subscribe` returns false and logs a warning when the table is full; `cc_poll_subscribers()`
  reports the count (`ota status`); the table has 6 slots (4 used).
- **Accepted:** UI M7 (a Go back to a pre-CFG-25 build that saves its settings forgets `fw_path`: noted in the
  README) and UI M8 (the `ota_url` wipe is logical: NVS may keep the old page until its garbage collection, which
  matters only to someone who can already read NVS over USB).

New copy: `FW_ERR_PENDING` "The new version is still checking in with the Host. Try again in a few minutes.";
`FW_ERR_NO_ROLLBACK` "Updates are off: this Tab5's bootloader can't go back. Flash it once over USB.";
`SET_FW_ROLLBACK_OFF` "Rollback off"; `FW_TOO_LATE` "Too late to cancel: the Tab5 is switching to the new version.";
`FW_BOOTLOOP_BOTH` "Both versions keep restarting. Flash a good build over USB (README › Update over Wi-Fi).";
`FW_ROLLBACK_FAILED` "The new version can't reach the Host and the Tab5 couldn't go back; it keeps this version.";
`SET_FW_PATH_BAD` "Use only letters, digits and _ . / ~ - (1 to 63 characters), not starting with -".

## 18. Fix round 2 (re-review, 2026-10-25)

- **N1, Go back during the pending window.** `esp_ota_mark_app_*` act on the *active* otadata entry, so a Go back
  that switched to A while the new image N was pending let the 180 s rollback mark A invalid (both entries invalid:
  the bootloader then boots ota_0 unchecked). Now Go back on a pending image *is* the rollback: it claims the
  pending flag under the lock the rollback task and mark-valid use (`fw_pending_claim`: test-and-clear, one winner),
  returns Busy if the rollback has it, stops the timer, closes SSH and calls
  `esp_ota_mark_app_invalid_rollback_and_reboot`; A boots in its existing VALID entry. A rollback task that loses
  the claim does nothing. `switch_to_other` also takes any leftover claim before `esp_ota_set_boot_partition`.
- **N2, the trusted flag and a never-run build.** `esp_ota_begin` erases the other slot's entry, so a power cut in
  the read-back, or between `fw/attempt` and the switch, left a complete build that never ran with no entry. Now:
  Previous version (and the guard's target) needs a VALID entry, a `claude_companion` descriptor, and the
  `fw/ok_<slot>` NVS marker naming the build's ELF SHA-256, which is written only while that build runs valid
  (`fw_previous_ok`). A switch is trusted only to a VALID target (`fw_switch_trusted`), and the RTC flag stores the
  target's address: `fwu_boot_early` trusts only that slot (`fw_boot_state(pending, flag, target_match)`). A
  wired-flash build gets its marker on its first boot (the bootloader writes its entry VALID). The guard reads the
  marker by opening NVS early (`nvs_flash_init`, as `hibernate_boot_early` does) only when it may act.
- **N3.** A failed rollback reopens the link (`lg_reopen`), so a later Host result can still mark the image valid.
- **N4, accepted.** A reset during the 180 s check (a brownout on a weak USB port, while the hold keeps the screen
  on) makes the bootloader abort a good build; the old one boots with "didn't start properly". Not a brick; the
  README says to give a new build's first boot a good power source.
- **N5.** The slot is marked dirty only after `esp_ota_begin` succeeds, so a failed begin can't erase the header of
  the previous good build.
- **N6.** The 177 s notice is posted only while the image is still pending, under the lock.
- **N7, accepted.** A power cut in the milliseconds between the guard's switch and its restart (or a brownout before
  `fwu_boot_early` marks the target valid) loses the trust: the target goes on probation, the old failure mode of
  core I1, in a very narrow window that fails toward probation.
- **N8.** A slot-size mismatch (an old partition table) has its own diagnosis: the row reads "Old partition table"
  and Install refuses with `FW_ERR_LAYOUT` ("Updates are off: the partition table is old. Flash this build once over
  USB."), not "Rollback off".
- **N9.** See §17 C1: the runtime Rollback off check is one update late.

## 19. Fix round 3 (re-review, 2026-10-25)

- **R1, the rollback task wins the claim.** The 180 s rollback clears the in-memory pending flag at 180.0 s, then
  waits up to 2 s in `lg_close`; the entry is still PENDING_VERIFY in otadata. Round 2 decided Go back's mode from
  the flag, so a Go back in that window switched, and the rollback then invalidated the switch's target. Now Go back
  decides from otadata (`fw_go_back_decide(otadata_pending, claim_won)`: pending and won is the rollback, pending and
  lost is "Already going back to the previous version.", never a switch), and `switch_to_other` refuses while the
  running entry is pending. No caller ignores a lost claim. The pending-path rollback needs only
  `esp_ota_check_rollback_is_possible`, not the Previous-version marker.
- **R2.** A failed pending Go back puts the claim back and re-arms the 180 s timer for the remaining time (at once
  if past 180 s), so the rollback still runs and, if it also fails, ends the hold with its note.
- **R3.** `ota status` says "updates off (old partition table)", "off (rollback off)" or "on"
  (`fw_updates_state_text`).
- **R4.** A failed `fw/ok_<slot>` write is logged once per boot.
- **R5.** The guard has two give-up notices: after a fallback, "Both versions keep restarting…"; with nothing to go
  back to (no runnable other slot, as on the first boots after the migration), "Restarting repeatedly; no earlier
  version to go back to. Flash a good build over USB."
- **R6.** A Tab5 coming from a Plan 6 build before round 2 has no marker for its old slot until that slot is
  overwritten: no Previous version and no guard target meanwhile (README).
- **R7.** The rule tests are decision tables: the whole Previous-version table, the Go back table with the race
  replayed step by step (the row that round 2 got wrong), and the status words.
