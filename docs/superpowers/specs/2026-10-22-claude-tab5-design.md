# Claude Companion on the M5Stack Tab5 — Design

Status: direction agreed with the user on 2026-10-22. This is the third revision of the day; the
"Claude API direct" revision is in git history and is superseded.

## Goal

Do your main work in Claude Code on the Mac, then pick up the Tab5 on the couch and see, answer and
fully take over those same sessions. No phone, no API key: it runs on your existing Claude subscription.

## Requirements (from the user)

- **R1** Control existing Mac sessions from the Tab5: see them all, see which ones need you, answer
  quickly, and fully take one over (type into the same session).
- **R2** No phone needed; works at home or away.
- **R3** Runs on the existing Claude subscription. Claude Code on the Mac does all of the Claude
  work: no API key, and no imitating Claude's own apps.
- **R4** Keyboard-first: the A164 is the primary input; touch is secondary.
- **R5** A beautiful, functional UI we control: a native dashboard, with a terminal only when you
  open a session.
- **R6** Power-efficient: sleeps on its own. Shows battery %, charging, Wi-Fi and the clock.
- **R7** Later: voice input with the mic, so you don't have to type on the small keyboard (the
  user's priority, 2026-10-23). A camera photo into a session comes last.
- **R8** Shareable open source (MIT).
- **R9** Not a security or hacking tool. Never propose Evil-M5Project or anything like it.

## How it works

```
 Mac                                                    Tab5
 Claude Code background sessions                        dashboard + terminal (LVGL)
 sshd (Remote Login, key auth)  ◀── SSH over Tailscale ──  libssh + MicroLink (Tailscale)
```

- **Background sessions are what the Tab5 controls.** You start them with `claude --bg`, from
  agent view (`claude agents`), or move a running session with `/bg`. At the desk you keep using
  `claude attach <name>` as usual.
- **The Tab5 holds one SSH connection to the Mac over Tailscale** and runs ordinary Claude Code
  commands. These were verified in Claude Code 2.1.309 on the user's Mac:

  | Need | Command run on the Mac |
  |---|---|
  | List sessions and their state | `claude agents --json --all` (`state: "blocked"` = needs you; `--all` keeps finished ones) |
  | Preview a session | `claude logs <id>` |
  | Quick answer | attach-paste: `claude attach <id>` in a pty, bracketed paste of the text, Enter, then Ctrl+Z (about 1.3–3.3 s; `-p --resume` is refused while the session runs). Only for a running background session; no shell sees the text. |
  | Take over | `claude attach <id>` in a terminal (pty) channel. ← returns to agent view and Ctrl+Z to the shell; the session keeps running. |
  | Transcribe a recording (Plan 5) | `cc_cmd_transcribe`: a plain exec channel (no pty) that runs `whisper-cli -m <model> -l en -nt -np -otxt -of - -f -` with the WAV on stdin and the text on stdout; exit 120 = no whisper-cli at the path, 121 = no model. Nothing is stored on the Host. See `docs/superpowers/specs/2026-10-24-voice-input-design.md` §7.1. |
  | Check voice input (Plan 5) | `cc_cmd_voice_check`: the same two path tests, no audio; exits 0, 120 or 121 (§7.2). |
  | Stop | `claude stop <id>` (verified on the Mac; the device has no Stop feature in Plan 1) |

- **Nothing extra runs on the Mac:** no daemon, no helper, no port forwarding; the watch loop
  runs only inside the companion's exec channel and exits on its own (Plan 2). Both machines
  connect outward to Tailscale.

## Decisions

- **D1 — SSH over Tailscale to the Mac.** Alternatives that were rejected:
  - Claude API direct: it can't reach Mac sessions and needs an API key.
  - Reusing the Claude login inside our firmware: against the terms, and it risks the account.
  - Remote Control: only the official apps can use it, and there's no device interface.
  - A bridge daemon on the Mac: unnecessary, because plain SSH exec does the job.

  The earlier `bridge/` and `web/` stay in the repo as optional extras.
- **D2 — Only background sessions can be taken over.** `claude agents --json` also lists plain
  interactive sessions (`kind: "interactive"`). The Tab5 shows those read-only with the hint
  "Run /bg in this session on your Mac to take it over here."
- **D3 — Absolute path to `claude`.** SSH exec gets a shell that can't find `claude`. Verified: it's
  installed at `~/.local/bin/claude` and only `.zshrc` puts it on the PATH, so neither a login nor a
  non-login non-interactive zsh finds it. A device setting "Claude path" defaults to
  `~/.local/bin/claude`, Claude Code's native install location; the remote shell expands the `~`.
  Every command uses that setting.
- **D4 — Safe command lines.** Every command line is built by a `cc_cmd_*` function, and only from
  validated parts: the Claude path (`[A-Za-z0-9_./~-]`, 1–63 characters) and session ids (short ids
  against `^[0-9a-f]{8}$`, session UUIDs against the standard UUID form). The user's answer text is
  never part of a command line: a quick answer reaches the session as pty bytes inside a bracketed
  paste on a `claude attach <id>` channel, after `cc_paste_build` drops ESC, DEL and the other
  control bytes, so no shell ever parses it. The quoting rule stays for a command that would carry
  text: one single-quoted argument with `'` escaped as `'\''`, after a `--` end-of-options marker
  so text starting with `-` is a message, not a flag (verified on Claude Code 2.1.309:
  `-p --resume <sid> -- '--help me'` delivered the literal text). Only `cc_cmd_reply` uses it; it is
  reserved for a later reply to a stopped session, and only the host tests call it today.
- **D5 — Input versus permission prompts.** Claude Code 2.1.309 has no `waitingFor` field, so the
  Tab5 can't tell a permission prompt from an input request. Every blocked background session
  (`state: "blocked"`) gets the answer field, plus **Open** (Ctrl+O) to answer inside the session. The
  logs preview shows what it's waiting for. Answers are pasted; Ctrl+U and the final CR go
  outside the paste, so the device types only when the screen's last `❯` line is the empty input
  prompt (fail-closed, R19), and otherwise offers Open. A real Bash permission dialog from
  2.1.309 is captured (`firmware/test/host/fixtures/screen_permission_dialog.bin`, taken with
  `firmware/tools/capture_attach_screen.py`); the gate refuses it and the dialog matcher knows it. Only running background sessions can be answered: a stopped card shows
  "Stopped" with no answer field, and the console `reply` refuses it. The device never runs
  `claude -p --resume` (it is refused while a session runs, and a stopped-session reply is not
  built yet). The recipe and timings are in `docs/superpowers/notes/2026-10-22-couch-path-check.md`.
- **D6 — Fork Clientre5** (MIT, © 2026 Mozgi512), vendored in `firmware/` at upstream `252e07a`.
  - **Keep:** `ssh/` + `components/libssh` (LGPL-2.1; compliant because we publish the full
    source), `term/`, `servers/`, `net/` + `components/microlink` + `components/wireguard_lwip`,
    `input/`, `sys/`, `media/`, `crypto/`, `settings/`, `storage/`, `ota/`.
  - **Remove:** `ime/` and the SKK dictionary (GPL-2.0; Japanese input not needed), `web/` (file
    manager: not needed, and upstream calls it unstable over the C6 link), and the Japanese and
    Chinese UI languages.
  - **Leave the terminal font alone.** It carries the box-drawing and symbol glyphs that Claude
    Code's interface uses.
- **D7 — Auth.** The Tab5 generates its own SSH key; Clientre5 supports key auth and pins host
  keys in NVS. The companion link pins the first key it sees without asking (it has no screen to
  ask on), and refuses a changed key; the dashboard then says so. The SSH terminal asks before it
  pins. Both pin the address they connect to: the server entry's host, or the tailnet IP after a
  MagicDNS lookup, so they share one pin for the Mac and the terminal asks only if it reaches the Mac
  by another address (a MagicDNS lookup that failed on one side). A changed key is cleared from
  Settings → Companion → **Forget Mac host key** (or the console's `companion hostkey forget`), after
  a confirm. It erases both pins and reconnects the link, which pins the key it meets and shows its
  fingerprint in a toast (`New Mac key pinned: SHA256:…`) to compare with
  `ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub` on the Mac. The terminal then finds that pin and
  does not ask again (P76). The device key lives in NVS and never leaves it; the public line is shown with the
  `sshkey` console command. The Mac's `~/.ssh/authorized_keys` entry is limited to the tailnet:
  `from="100.64.0.0/10" ssh-ed25519 AAAA… claude-companion`. The Tailscale auth key is entered once
  on the device.
- **D8 — Debuggability.** A USB serial console with `screenshot`: the P4 hardware encoder turns an
  LVGL snapshot into a JPEG, which is sent as a framed base64 block and saved by
  `firmware/tools/screenshot.py`. No SD card is needed (none is inserted). The fork's host UI
  preview (`firmware/tools/ui_preview`) renders screens on the Mac.
- **D9 — Visual identity:** dark graphite with an amber accent, from the mockup
  (https://claude.ai/artifact/PBbMTBiaQXBn8i6mFojDcd) and `web/index.html`:

  | Token | Hex | Meaning |
  |---|---|---|
  | bg | `#0e1116` | screen background |
  | panel | `#161b22` | bars, raised areas |
  | panel-2 | `#1b212b` | cards |
  | line | `#2a323d` | hairlines |
  | fg | `#e9e7e1` | primary text |
  | muted | `#8a94a2` | secondary text |
  | amber | `#f4b545` | **needs you**; nothing else uses amber |
  | green | `#54d08a` | working / OK |
  | red | `#ef6f6f` | errors, unreachable |

- **D10 — Name.** Working name "Claude Companion" (repo `claude-companion`). No Anthropic logos or
  brand assets. The user picks the final name before any public release.

## Hardware (verified on this unit, 2026-10-22)

| Part | Detail |
|---|---|
| SoC | ESP32-P4 rev v1.3, 400 MHz, MAC `02:00:00:00:00:01` |
| Flash / PSRAM | 16 MB / 32 MB |
| Radio | ESP32-C6 over SDIO (esp-hosted): Wi-Fi + BLE |
| Display / touch | 1280×720 IPS, GT911 |
| Keyboard | A164 on I²C `0x6D` (SDA G0, SCL G1, INT G50); reports fw `0x01` |
| Other | SC2356 camera, dual mics, IMU, INA226 power monitor, RX8130 RTC |
| USB serial | `/dev/cu.usbmodem211201` (check with `ls /dev/cu.usbmodem*`) |

Known issues:
- Wi-Fi failed at boot because esp_hosted 3.0.10's Kconfig board choice defaults to the P4
  Function-EV board and overrides the Tab5 SDIO pins. Fixed by `CONFIG_ESP32P4_TAB5_C6_BOARD=y` plus
  pinning `espressif/esp_hosted` to `==3.0.10`. The C6 reporting firmware `0.0.0` ("major version
  mismatch") is harmless: scan and connect work, so don't update the C6.
- **Never open a login shell on the Mac.** The user's `.zshrc` autostarts tmux for interactive
  logins, so a login shell would type into their live panes. The companion paths always use a pty
  exec channel with an explicit `claude attach <id>`; a non-interactive zsh doesn't read `.zshrc`.
  The fork's More → SSH Terminal refuses the companion server ("Open sessions from the
  dashboard"): the configured entry, and any other entry with the same host (also after a MagicDNS
  lookup). It can't spot the Mac when no companion server is set, or under a host name it can't
  resolve to the same address.
- Board revision unknown. Upstream tested only board v3; it boots fine on this unit.
- No microSD card inserted.
- `tailscale_prepare_peer` returns at once when `ml_wg_mgr_peer_is_up` reports the peer up (Task 7:
  it used to move the DERP region on every open, about 22 s). "Up" means a WireGuard keypair is
  still valid, which can be up to about 3 minutes stale. The take-over path is fine, because the
  live check has just used the tunnel. The generic SSH terminal (More) can now skip the handshake
  kick on a stale tunnel, and its connect then fails after the 40 s libssh timeout instead of
  being repaired. The companion link reconnects through the same `tailscale_prepare_peer`, so after
  the Mac sleeps and wakes within the keypair lifetime its reconnect may also wait on a stale
  tunnel: retries on the 10 s libssh timeout and the backoff until WireGuard re-handshakes or the
  keypair expires. Not yet measured; Plan 2 (fast reconnect on wake) checks it.
- The poll buffer is 256 KB (about 970 sessions at ~270 B each with `--all`). Past that the poll
  fails, the dashboard says the list is too large and isn't updating, and the dot turns red.

## Environment

- Mac: Claude Code 2.1.309 at `~/.local/bin/claude`; login shell zsh; Tailscale installed and
  running (find the IP with `tailscale ip -4`). **Remote Login is ON** (own user only). The Mac's tailnet name is
  `my-host` (100.64.0.18); the Tab5 is `tab5` (100.64.0.20).
- ESP-IDF **v5.5.6** at `~/esp/esp-idf` (branch `release/v5.5`). Activate with `. ~/esp/esp-idf/export.sh`.
- esptool 5.6.0 (global). cmake, ninja and dfu-util from Homebrew.
- The Tab5 currently runs the Clientre5 v1.0 release image (verified by digest).
- Factory flash backup: `firmware/backups/tab5-factory-020000000001.bin` (16 MB, sha256
  `0000000000000000000000000000000000000000000000000000000000000000`, git-ignored). Restore with
  `esptool --port PORT write-flash 0x0 firmware/backups/tab5-factory-020000000001.bin`.

## Roadmap

| Plan | Delivers |
|---|---|
| **1 Session control** (`docs/superpowers/plans/2026-10-22-session-control.md`) | baseline build, debug console, a hands-on check of the couch path with the user, trimmed and rebranded fork, session dashboard with quick answers and take-over. Done when, from the couch, the Tab5 shows your Mac sessions, the amber ones need you, and you can answer or open them. |
| **2 Power & vitals** (`docs/superpowers/plans/2026-10-23-power-vitals.md`) | backlight dim → light sleep; wake on keyboard, touch or lift; fast reconnect on wake; full vitals bar |
| **3 Local link** (`docs/superpowers/plans/2026-10-23-local-link.md`) | the Host link and every check-in run over the home LAN (the Host's `.local` name, with a fallback address); Tailscale becomes a sticky on/off switch, off by default, that never starts MicroLink while off; fast check-in; SSH closed before every link-down |
| **4 UI overhaul** (`docs/superpowers/plans/2026-10-24-ui-overhaul.md`) | folds in the old "Home screen" and "Design pass" items: Music, Camera, USB drive, Files and the launcher removed; six screens (Sessions home, Take-over, Terminal, SSH session, Host form, Settings) on one token set and shared components; one tr() string table that says "Host". The typeface question (IBM Plex or the current pixel font) stays open for the user |
| **5 Voice input** (`docs/superpowers/plans/2026-10-24-voice-input.md`, done) | dictate answers and session input with the mic instead of typing. Push-to-talk on the keyboard; the Tab5 records and sends the audio over the existing SSH link to a local transcriber on the Host (whisper.cpp, one `brew install`, chosen by the user 2026-10-23), and the text goes into the answer field. Claude Code's own `/voice` needs a mic on the machine running it and doesn't work over SSH, and the Claude API takes no audio input. The user offered an API key with monthly credit if a later need for one comes up. |
| 6 Camera (last) | take a photo, copy it to the Host over SFTP, and ask the open session about it (lowest priority) |
| 7 Release | final name, README, GitHub publish (visibility approved by the user), OTA manifest |

Write each later plan when the previous one ships, against this spec.

## Things only the user can do (asked together in Plan 1, Task 5)

1. Turn on **Remote Login**: System Settings → General → Sharing → Remote Login, limited to their
   own user.
2. Create a **Tailscale auth key** at `login.tailscale.com/admin/settings/keys` (reusable +
   pre-authorized is easiest) and enter it on the Tab5.
3. Join **Wi-Fi** on the Tab5.
4. Approve adding the Tab5's public key to `~/.ssh/authorized_keys` with the tailnet-only
   restriction from D7.
