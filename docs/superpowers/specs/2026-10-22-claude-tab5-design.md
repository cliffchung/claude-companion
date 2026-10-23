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
- **R7** Later: send a camera photo into a session; voice after that.
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
  | List sessions and their state | `claude agents --json` (`state: "blocked"` = needs you) |
  | Preview a session | `claude logs <id>` |
  | Quick answer | `claude -p --resume <sessionId> -- '<text>'` (queues into the running background session) |
  | Take over | `claude attach <id>` in a terminal (pty) channel. ← returns to agent view and Ctrl+Z to the shell; the session keeps running. |
  | Stop | `claude stop <id>` |

- **Nothing extra runs on the Mac:** no daemon, no helper, no port forwarding. Both machines
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
- **D4 — Safe command lines.** The device builds remote commands from what the user types. Reply
  text is always passed as one single-quoted argument with `'` escaped as `'\''`, after a `--`
  end-of-options marker so text starting with `-` is a message, not a flag (verified on Claude Code
  2.1.309: `-p --resume <sid> -- '--help me'` delivered the literal text). Before use,
  session ids are checked: short ids against `^[0-9a-f]{8}$`, session UUIDs against the standard
  UUID form.
- **D5 — Input versus permission prompts.** Per the docs, a blocked session reports what it's
  waiting for (`waitingFor`: "permission prompt" or "input needed"). The Tab5 offers a quick answer
  for "input needed". For a permission prompt it offers **Open**, so you approve inside the session,
  unless Plan 1 Task 5 shows that `-p --resume` can answer permission prompts.
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
  keys in NVS. The Mac's `~/.ssh/authorized_keys` entry is limited to the tailnet:
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
- The C6's esp-hosted firmware reports `0.0.0` against host `3.0.7` ("major version mismatch").
  If Wi-Fi won't connect, update the C6 firmware first.
- Board revision unknown. Upstream tested only board v3; it boots fine on this unit.
- No microSD card inserted.

## Environment

- Mac: Claude Code 2.1.309 at `~/.local/bin/claude`; login shell zsh; Tailscale installed and
  running (find the IP with `tailscale ip -4`). **Remote Login is currently OFF.**
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
| 2 Power & vitals | backlight dim → light sleep; wake on keyboard, touch or lift; fast reconnect on wake; full vitals bar |
| 3 Camera | take a photo, copy it to the Mac over SFTP, and ask the open session about it |
| 4 Design pass | typography (IBM Plex via the fork's font tools), motion, plus a look at voice options |
| 5 Release | final name, README, GitHub publish (visibility approved by the user), OTA manifest |

Write each later plan when the previous one ships, against this spec.

## Things only the user can do (asked together in Plan 1, Task 5)

1. Turn on **Remote Login**: System Settings → General → Sharing → Remote Login, limited to their
   own user.
2. Create a **Tailscale auth key** at `login.tailscale.com/admin/settings/keys` (reusable +
   pre-authorized is easiest) and enter it on the Tab5.
3. Join **Wi-Fi** on the Tab5.
4. Approve adding the Tab5's public key to `~/.ssh/authorized_keys` with the tailnet-only
   restriction from D7.
