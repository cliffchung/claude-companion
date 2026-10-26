# Claude Companion firmware (source)

Source tree of Claude Companion: firmware for the M5Stack Tab5 (ESP32-P4) that shows and controls
the Claude Code sessions on a computer (the Host) over SSH, by default on the home LAN, with
Tailscale as an optional switch. It is forked from Clientre 5 (MIT) and built on ESP-IDF v5.5
(tested with v5.5.6) and LVGL 9. This document covers the internals. Setup, daily use and the
console commands are in the [top-level README](../README.md).

> Status: verified on a physical Tab5 that the board support package reports as board version 2
> (upstream Clientre 5 was tested on v3): display, touch, keyboard, Wi-Fi, the Host link over the
> LAN, take-over, Tailscale on and off, and Hibernate check-ins on USB power. Not yet verified on
> this build: a Terminal SSH session to a real machine, the microSD paths, a USB keyboard, battery
> measurements on the pack, and firmware updates over Wi-Fi (Plan 6: built and host-tested, device gates
> pending).

## Layout

```
firmware/
├── main/
│   ├── app_main.c        boot: BSP, display, boot splash, settings, secure store, servers, input, Wi-Fi, UI
│   ├── companion/        the Host link (cc_link: one libssh session, exec channels, host-key pin carry-over),
│   │                     command builders (cc_cmd), the session list parser (cc_sessions), the long-poll
│   │                     watch parser (cc_watch), the debug console verbs (cc_console)
│   ├── crypto/           AES-256-GCM secure store (device-local key in NVS) for saved passwords
│   ├── fonts/            generated LVGL fonts: MaruMinya pixel faces, the Noto/DejaVu fallback, 16 px icons
│   ├── i18n/             the one English string table (tr())
│   ├── input/            Tab5 I2C keyboard (0x6D) + USB HID keyboard, key routing, the `key`/`ui` console verbs
│   ├── net/              Wi-Fi manager over esp_wifi_remote (ESP32-C6 via SDIO), the Tailscale switch and
│   │                     MicroLink wrapper (tailscale.c), tailnet host rules, fast check-in rules
│   ├── ota/              firmware updates: fw_rules.c (pure checks, host-tested), fw_update.c (the service; the
│   │                     only file that calls esp_ota_* or writes an app partition), fw_console.c (`ota`)
│   ├── servers/          the saved SSH hosts (NVS) and the host form's rules
│   ├── settings/         the settings blob, its migrations, keyboard light colours
│   ├── ssh/              libssh client task for the Terminal (device key, password, keyboard-interactive),
│   │                     host-key pins in NVS, the device's ed25519 key (`sshkey`)
│   ├── storage/          microSD mount on demand (reference-counted; never at boot)
│   ├── sys/              power ladder, Hibernate, check-ins, vitals, INA226 power monitor, RX8130 RTC, SNTP,
│   │                     screenshot, USB host manager, the debug console, the close-SSH-before-link-down guard
│   ├── voice/            voice input: mic.c (the microphone), voice.c (the record and transcribe service),
│   │                     voice_rules.c (pure rules, host-tested), voice_console.c (the `voice` console verbs)
│   ├── term/             VT100/xterm-256color emulator (wide characters, scrollback) and its LVGL view
│   └── ui/               Sessions, take-over / SSH session, Terminal host list, host form, Settings,
│                         shared components and design tokens, soft keyboard, boot splash
├── components/libssh/    libssh 0.11 (LGPL-2.1) from ewpa/LibSSH-ESP32, no Arduino dependency
├── components/microlink/ MicroLink (MIT), an unofficial Tailscale client
├── components/wireguard_lwip/  WireGuard for lwIP (BSD-3-Clause), used by MicroLink
├── test/host/            Unity host tests for the pure modules (CMake/CTest)
├── tools/                font and icon generators, ui_lint.py, screenshot.py, power_report.py,
│                         authorize_device_key.sh, merge_flash.sh, stage_update.sh, check_image_size.py,
│                         the host UI preview (ui_preview/)
├── partitions.csv        two OTA app slots: app0 @0x20000 and app1 @0x810000, 0x7F0000 each
├── version.txt           the version's first part (the build adds +<git hash>[.dirty])
└── sdkconfig.defaults
```

## Build

Requirements: ESP-IDF **v5.5** with the esp32p4 toolchain, and network access for the component
registry.

```sh
cd firmware
idf.py set-target esp32p4
idf.py build
idf.py -p PORT flash monitor
```

`idf.py flash` writes the bootloader (`0x2000`), the partition table (`0x8000`), a blank `otadata`
(`0xF000`) and the app (`0x20000`, `app0`), and keeps NVS. That is all a working device needs, and
the one wired flash a device on the old layout needs before updates over Wi-Fi work (README ›
Update over Wi-Fi). Delete `firmware/sdkconfig` and run `idf.py reconfigure` first: a stale one keeps
`# CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is not set` (defaults never override an option an existing `sdkconfig` has),
and `ota/fw_update.c` refuses to build that (`#error`), or the dynamic FATFS code page. Never `app-flash` for that
migration, `erase-flash`, or the merged full image.

The bootloader with rollback is 22,816 B of the 24,576 B between 0x2000 and 0x8000 (1,760 B spare). A bootloader
option that grows it past that fails the build; moving the partition table to make room would need another wired
flash.

The app version is `<version.txt>+<git short hash>[.dirty]` (worked out at configure time; a
`-D PROJECT_VER=…` wins, for test builds). After each build `tools/check_image_size.py` prints the
app slot's headroom and warns under 1 MB.

Board-specific settings in `sdkconfig.defaults` that matter:

- `CONFIG_ESP32P4_TAB5_C6_BOARD=y` together with `espressif/esp_hosted` pinned to `==3.0.10`
  (`main/idf_component.yml`). Without them esp_hosted defaults to the P4 Function-EV board's SDIO
  pins and Wi-Fi fails at boot. The C6 reports firmware `0.0.0`; scanning and joining work, and
  the C6 is never reflashed.
- `CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES=y`: lwIP's own mDNS query support, so the Host's `.local`
  name resolves without the full mDNS component.
- `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`: the debug console on the USB-C port.
- `CONFIG_FATFS_LFN_HEAP=y`: long file names on the microSD card (key files such as `id_ed25519`).
  `CONFIG_FATFS_CODEPAGE_437=y`: one OEM code page (the dynamic one linked every code page, ~470 KB);
  long names stay UTF-8.
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`: a new image boots pending and the bootloader goes back
  unless it marks itself valid (Firmware updates, below).
- `espressif/esp_lvgl_port` is pinned to `==2.8.0~1`, because `sys/display_ctl.c` mirrors its
  private driver-data layout.

### Release images

```sh
cd firmware
idf.py build
sh tools/merge_flash.sh            # -> firmware/release/{partitions,full}
```

**The merged full image is for a blank Tab5 only:** it fills every gap with `0xFF`, so it
overwrites NVS (Wi-Fi, the device key, the host-key pins, the saved hosts, the settings).
`merge_flash.sh` prints that warning first. On a blank device, flash it at `0x0`:

```sh
python -m esptool --chip esp32p4 -p PORT --baud 1500000 write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 release/full/claude_companion_full_flash.bin
```

Or flash the parts individually (what `idf.py flash` does, keeping NVS):

| Offset | File |
|---|---|
| `0x2000` | `partitions/bootloader.bin` |
| `0x8000` | `partitions/partition_table.bin` |
| `0xF000` | `partitions/ota_data_initial.bin` |
| `0x20000` | `partitions/claude_companion.bin` |

## Product identity

The product name is **Claude Companion** in the UI, the boot splash and the startup logs. The
ESP-IDF project (and so the app descriptor) is `claude_companion`. The default Tailscale node name
is `claude-companion`, and the device key's comment is `claude-companion@<device name>`. The UI is
English only, and calls the machine running Claude Code "Host" everywhere (`tools/ui_lint.py`
enforces it).

## How the Host link works

`companion/cc_link.c` owns one libssh session to the Host and signs in with the device key only.
Every command line comes from a `cc_cmd_*` builder (`companion/cc_cmd.c`), and only validated parts
go into one: the Claude path and session ids. The link never opens a shell.

- **The session list.** A long-poll exec channel runs a fixed `/bin/sh` loop on the Host
  (`cc_cmd_watch`). It runs `<claude> agents --json --all` every few seconds (3 s with the screen
  on, 15 s with it off), prints the output only when it changes, sends a heartbeat when quiet, and
  exits on stdin EOF, after 60 s without the Tab5's keepalive, or after 30 minutes. If the loop
  can't start, the link falls back to plain polls. The poll buffer is 256 KB.
- **Previews.** `claude logs <id> | tail -c 524288`, kept on the Tab5 from the stream's latest full clear
  (`ESC[2J`) on and rendered on a virtual screen in PSRAM; the pane shows the conversation rows above the
  input box or a dialog's hint, never the footer, status line or spinner (`cc_logview`). **Limit:** the bytes
  since that clear must fit 512 KB. Claude Code clears on start, attach and resize, so a session that runs
  long without being attached can pass it; its pane then falls back to the escape-stripped tail
  (`cc_log_tail`), which can show footer lines. A stateful streaming replay (a vt per session fed only the new
  bytes) would lift the limit; it isn't built.
- **Quick answers.** A pty exec channel runs `claude attach <id>`. The text goes in as a bracketed
  paste (`cc_paste_build` drops ESC, DEL and the other control bytes), then Enter, then Ctrl+Z
  detaches. The device types only when the last `❯` line on screen is an empty input line
  (`cc_screen_input_ready`); a permission dialog (`cc_screen_is_permission_prompt`) or anything
  else is refused.
- **Transcribing.** `cc_cmd_transcribe` is a plain (non-pty) exec channel that runs `whisper-cli`
  on the Host with the WAV on the channel's stdin and reads the text from stdout. The command is
  `exec /bin/sh -c 'W=<whisper>; M=<model>; [ -x "$W" ] || { cat >/dev/null; exit 120; }; [ -r "$M" ] || { cat >/dev/null; exit 121; }; exec "$W" -m "$M" -l en -nt -np -otxt -of - -f -'`.
  The paths are checked by `cc_path_valid`. A failed pre-check drains stdin first, so the Tab5's
  upload isn't cut off; exit 120 means no executable at the Whisper path and 121 means no readable
  model. `cc_link_transcribe` is the only public way to run an exec with stdin, and it builds the
  command itself from the settings. It queues behind other link work and gives up with "Lost the
  connection to the Host" if the link parks mid-upload.
- **Voice check.** `cc_cmd_voice_check` runs the same two tests and exits 0, 120 or 121, with no
  audio. Voice input starts it when a recording starts and Settings › Host › Voice input › Check
  runs it on demand.
- **Firmware.** Two plain (non-pty) exec channels read the file at the firmware path (Settings ›
  Host › Firmware file on Host, `cc_path_valid`). `cc_cmd_fw_info`:
  `exec /bin/sh -c 'F=<path>; [ -f "$F" ] && [ -r "$F" ] || exit 120; S=$(wc -c < "$F") || exit 121; H=$(shasum -a 256 < "$F" 2>/dev/null || sha256sum < "$F") || exit 122; echo $S ${H%% *}; exec head -c 288 "$F"'`
  prints the size, the SHA-256 and the first 288 bytes; `cc_cmd_fw_read`:
  `exec /bin/sh -c 'F=<path>; [ -f "$F" ] && [ -r "$F" ] || exit 120; exec cat "$F"'` sends the file,
  read straight into PSRAM 16 KB at a time (`cc_link_fw_read`, with progress and cancel). Nothing is
  written or left running on the Host.
- **Take-over.** A pty exec channel runs `claude attach <id>` and the terminal view draws it.
- **Resolving the Host.** A literal IPv4 address as is; MagicDNS while Tailscale is connected;
  `getaddrinfo` (lwIP sends an mDNS query for `*.local`, DNS for the rest); and only if all of
  that fails, the fallback LAN address. The last good address is cached for fast check-ins.
- **Host keys.** Pins are per resolved address, in NVS. The first contact on a tailnet address,
  or the first connection after the user's Forget, pins the key it meets. On a LAN address with no
  pin, a key equal to the same entry's earlier tailnet pin is carried over silently; any other
  unknown key is refused until the user trusts it in Settings › Host (`cc_link_trust_host_key`: only
  while nothing is pinned, and only the fingerprint the user confirmed, `kh_forget_applies`) or
  forgets (`companion/kh_carry.c`, host-tested). A changed key is always refused.
- **Never on the Host as a shell.** The Terminal client refuses a server whose host key is in a
  deny-list of every key ever pinned for the link (NVS, up to 8), checked after key exchange and
  before authentication, besides hiding the Host's entry from the list.
- **Closing before going down.** Every path that takes the link down (Wi-Fi off, sleep, restart)
  closes SSH first (`sys/link_guard.c`, `sys/link_down.c`).

## Voice input

`voice/` records on the Tab5 and has the Host transcribe (README › Daily use › Voice input).
Ctrl+Space (`HK_D_VOICE`) or the mic button toggles it on Sessions.

- **Buffer.** One PSRAM buffer, a 44-byte WAV header slot then 16 kHz mono 16-bit samples, up to
  30 s (960,044 B). The header is written at stop and the whole buffer is the stdin payload. The
  buffer is zeroed and freed on every exit path. No sample or transcript text goes to a log, the
  console, NVS or a file; logs show lengths, durations, dBFS levels and exit codes.
- **Mic notes.** `mic.c` opens its own RX-only I2S channel (I2S 1, which drives MCLK) with the
  ES7210 as a slave, every time a recording starts, and deletes it when the recording ends. Nothing
  holds the mic open, and the codec returns to its power-down state at close. MIC1 and MIC2 are
  averaged at 30 dB gain, and the first 250 ms after each open are dropped to skip a power-up pop.
  The open costs about 7.4 KB of internal RAM, all returned at close. Block levels under -50 dBFS
  count as silence ("No speech heard"), and nothing is uploaded.
- **Tasks.** A 4 KB voice task records and uploads. A 3 KB helper task runs the voice check at
  start. The 100 ms meter timer exists only while recording. All are gone after stop, cancel, an
  error, leaving Sessions or a link park.
- **Result.** One result slot with a generation counter. Sessions takes it on its 300 ms tick, and
  puts the text only in the draft of the session key captured at start. A result not taken within
  60 s is wiped. Nothing calls the paste path with transcript text, so only Enter sends.
- **Console.** `voice rec <s>`, `voice status`, `voice test <s>`, `voice test-bg <s>`,
  `voice test-short` and `voice test-cancel <s>` drive the service without the UI (`help` has the
  details). They use the same code path and print only lengths and timings. All but
  `voice status` are built only with `CONFIG_COMPANION_DEBUG_COMMANDS=y` (see below).
- **Settings.** `whisper_path` and `whisper_model` are the last fields of the settings blob
  (CFG 24). Their defaults, `CC_WHISPER_DEFAULT` and `CC_WHISPER_MODEL_DEFAULT`, live only in
  `companion/cc_cmd.h`; `tools/ui_lint.py` flags `whisper-cli` or `-otxt` anywhere else under `main/`.

## Boot splash

A static pixel terminal mark, "Claude Companion" and "Starting…" are drawn as soon as LCD and
touch setup are done, before settings, secure storage, the RTC, server data, input and the
network start. It is a 32-byte monochrome mark drawn once with flash fonts: no SD asset, image
decoder or animation timer. The splash stays up for at least 1 s, then Sessions replaces it
without waiting for Wi-Fi. The serial log reports `boot logo submitted in ... ms` and
`boot complete in ... ms`.

## Tailscale (tailnet SSH)

The Tailscale support uses the vendored, unofficial [MicroLink](https://github.com/Csontikka/microlink)
client (`components/microlink`, MIT): ts2021 control protocol, WireGuard, DERP relay, DISCO/STUN.
The WireGuard netif owns `100.64.0.0/10` in lwIP, so libssh connects to peers with ordinary
sockets. Server hosts may be tailnet names (`nas`, `nas.tailxxxx.ts.net`); they are resolved from
the peer list at connect time.

**The switch.** `net/tailscale.c` is the only owner of MicroLink. `tailscale_set_enabled()` saves
`ts_enable` to NVS first, then starts or stops MicroLink, and `tailscale_start` refuses while the
switch is off, so no path (boot, Wi-Fi event, check-in, reconnect) can start it while it's off.
Switching off stops and frees the instance, which returns its internal RAM. MicroLink's node keys
stay in NVS, so switching on again reuses the registered node without a new auth key. *Forget this
device on the tailnet* wipes the node keys but keeps the auth key. With the switch off, a
connection to a tailnet address or `*.ts.net` name fails at once (`net/ts_host.c`). Settings for
this are under Settings › Network; the auth key can also be read once from
`/tailscale/authkey.txt` on the microSD card.

**Relay-only by default.** MicroLink's direct-path (DISCO) upgrade can black-hole traffic behind a
strict NAT (pings answered, WireGuard data not), so the firmware keeps every peer on DERP unless
*Connection* is set to *Try direct*. Expect about 50–150 ms of extra latency. Before opening an SSH
socket the firmware moves its DERP connection to the peer's home region, wakes the WireGuard
session and waits for it; the first connection to a peer can take 5–30 s, and a cold connect after
boot about a minute.

**Tailscale SSH targets** (`SSH-2.0-Tailscale` banner) authenticate by tailnet identity: the
client's `none` authentication succeeds before any key or password is tried. Their default ACL
rule is `"action": "check"`, which needs a browser confirmation the Tab5 can't do. Tag the device
and add an `accept` rule, e.g.:

```jsonc
"tagOwners": { "tag:tab5": ["autogroup:admin"], "tag:server": ["autogroup:admin"] },
"ssh": [
  { "action": "accept", "src": ["tag:tab5"], "dst": ["tag:server"], "users": ["autogroup:nonroot", "root"] },
  { "action": "check",  "src": ["autogroup:member"], "dst": ["autogroup:self"], "users": ["autogroup:nonroot", "root"] }
]
```
then apply `tag:tab5` to the Tab5 and `tag:server` to the target on the Machines page (SSH rules
can't use `autogroup:member` or a user as `dst` when the source is a tag).

## Saved hosts and the microSD card

The saved SSH hosts live in NVS (`servers/server_store.c`), with passwords encrypted by the
secure store. The microSD card is never mounted at boot; `storage/sd.c` mounts it on demand and
unmounts after the last user. It is used for:

- *Load from SD card* (the Tailscale auth key, `/tailscale/authkey.txt`);
- Terminal hosts whose key file is under `/sdcard/`;
- a one-time import of the host list an older build kept on the card (`/sdcard/.tab5/servers.enc`,
  encrypted with this device's key), on devices that had that option on. The import adds entries,
  skips duplicates and invalid ones, and leaves the card file unchanged.

## Firmware updates

`ota/fw_update.c` (Plan 6) installs a build from the Host into the app slot that isn't running.
About › Update firmware runs the check on the Sessions worker; Install starts the update task:

1. `POWER_HOLD_UPDATE` on (no dim, no Radio off, no Hibernate); the whole image is downloaded into one
   PSRAM buffer (`cc_link_fw_read`).
2. Before any flash write: the byte count, the SHA-256 against the Host's, the header again (magic,
   segments, chip `0x0012` and its revision range, the descriptor magic, the project
   `claude_companion`, a version), and the layout walk (`fw_rules.c`: segments, the XOR checksum, the
   appended SHA-256, the file ending exactly there). A failure is refused with its copy; nothing is
   written.
3. `esp_ota_begin` on the other slot, 16 KB writes, `esp_ota_end` (ESP-IDF's verify), then a read-back
   of the written bytes hashed against the Host's SHA-256.
4. NVS `fw/attempt` = the new version, `esp_ota_set_boot_partition`, restart.

At boot (`fwu_boot_init`, before Wi-Fi): a new image is PENDING_VERIFY; it marks itself valid on the
first result from the Host (`cc_poll_subscribe`), else a 180 s timer shows a notice and rolls back
(`esp_ota_mark_app_invalid_rollback_and_reboot`). The old image toasts the rollback from `fw/attempt`.
A boot-loop guard (an `RTC_NOINIT` count of early panic and watchdog resets) switches to the other
slot after three in a row. About › Previous version › Go back verifies the other slot and switches to
it. Bootloader, partition table and C6 updates stay USB-only.

`tools/ui_lint.py` keeps every `esp_ota_*`, `esp_partition_erase_range` and `esp_partition_write*`
call in `fw_update.c` (`ota-outside-fw_update`) and the firmware command literals in `cc_cmd`
(`fwcmd-outside-cc_cmd`). `CONFIG_COMPANION_OTA_TEST_NO_VALIDATE` (debug builds only) builds an image
that never marks itself valid, for the rollback test. On the Host, `tools/stage_update.sh` copies a
build to the staging path atomically.

## Regenerating fonts

`gen_pixel_fonts.sh` reads `x12y12pxMaruMinya.ttf` from the repository root by default
(not committed: the font's terms allow embedding but the TTF itself is obtained from its
author, hicc), or accepts a TTF path. `gen_fonts.sh` needs `NotoSansMonoCJKjp-Regular.otf` and
`DejaVuSansMono.ttf` in the supplied directory.

```sh
npm i lv_font_conv                                  # once
pip3 install fonttools                              # once
sh tools/gen_pixel_fonts.sh              # the UI and terminal faces
sh tools/gen_fonts.sh       <font dir>   # the CJK/symbol fallback (font_cjk)
python3 tools/gen_ui_icons.py            # the 16 px pictogram font (font_icons16)
```

The UI and the terminal use **x12y12pxMaruMinya** by hicc, rendered on its 12 px grid at exact
integer scales: 24 px for body, label text and the terminal (12 px cells), and 36 px for titles,
at 1 bpp. The label role (`FONT_SMALL`) is a RAM copy of `font_px24` (Plan 6: `font_px20.c` was a
byte-for-byte duplicate); the C names `font_px24` and `font_px36b` are kept from the fork. Missing
glyphs fall back to `font_cjk`, then to the icons.

`cjk_ranges.py` intersects the wanted blocks with the font's cmap and routes whatever Noto Sans
CJK lacks to DejaVu Sans Mono, which `mono_match.py` rescales so a fallback glyph still advances
exactly one terminal cell. The title face is a subset taken from `i18n_chars.py`, which walks
all of `main/` and (with `--font`) drops the characters the face does not have: run
`gen_pixel_fonts.sh` again after adding strings. LVGL draws a hollow rectangle for any character
missing from the whole fallback chain, so a box on screen means a gap in these ranges.
`gen_ui_icons.py` builds the pictograms from bit masks in the script, with no external font.

## UI structure

One visual system: fixed design tokens (`ui/ui_tokens.c`) and shared components (`ui/ui_comp.c`),
with no runtime themes. Amber is reserved for NEEDS YOU; `tools/ui_lint.py` and the preview's amber
check enforce it. Six screens: Sessions (home), Take-over, Terminal (host list), SSH session, the
host form, and Settings with six sections. Only the visible Settings section is built.

Gestures are taken from the input device rather than from the widget tree: LVGL only walks up from
the touched object while every object in between has `LV_OBJ_FLAG_GESTURE_BUBBLE`, which does not
survive cards, lists and buttons. `ui_core.c` installs one handler on the pointer indev and
dispatches by active screen. The default is swipe right to go back; screens that need their own
handling (Sessions, the terminal) install one with `ui_screen_set_gesture()`. A swipe that wakes
the screen does nothing else.

The [UI preview](tools/ui_preview/README.md) renders the real screens on macOS with stub data and
checks them (amber, touch targets, fit, idle redraws, timers).

## Licenses

| Component | License |
|---|---|
| libssh | LGPL-2.1 |
| MicroLink | MIT |
| wireguard-lwip | BSD-3-Clause |
| LVGL | MIT |
| ESP-IDF | Apache-2.0 |
| Clientre5 (upstream fork base, see [LICENSE.clientre5](LICENSE.clientre5)) | MIT |

Fonts: MaruMinya (source TTF copyright © 2023 hicc), Noto Sans CJK (OFL-1.1), DejaVu Sans Mono
(Bitstream Vera / Arev, permissive). Application code in `main/`: MIT. The full
list, including the components the build downloads, is in
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## Test-only console commands

`CONFIG_COMPANION_DEBUG_COMMANDS` (Kconfig menu "Claude Companion", `main/Kconfig.projbuild`) is off
by default and in `sdkconfig.defaults`. It builds the bench commands that change state behind the
user's back: `companion hostkey corrupt` (and `ssh_known_host_set_debug`, which writes the wrong
pin), `companion takeover|kick|hostcache set|watch test-fallback|voice-upload-test`,
`wifi test drop`, `power bench|sim|go|light test|light raw|checkin hibernate`,
`power hib test|test-wake|probe|backstop <s> <n>`, `power rail <n> on|off`, `power imu suspend`
`voice rec|test|test-bg|test-short|test-cancel`, and `ota read-test|update [corrupt-ram]|cancel|go-back|test-bootloop <1-3>`. Set it to `y` in a development device's local
`sdkconfig` (`idf.py menuconfig`); `sdkconfig` is git-ignored, so the default stays off.
