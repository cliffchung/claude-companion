# Claude Companion on the M5Stack Tab5 — Design

Status: direction agreed with the user on 2026-10-22. This supersedes the "SSH-direct / bridge"
direction described in the top-level README, which is kept only as an optional module.

## Goal

Turn an M5Stack Tab5 with its A164 keyboard into a beautiful, keyboard-first Claude device that
talks to the Claude API directly. Open source and shareable.

## Requirements (from the user)

- **R1** Keyboard-first: the A164 is the primary input; touch is secondary.
- **R2** A beautiful, functional UI that we fully control. This is why the device calls the API
  directly instead of rendering a terminal.
- **R3** Respond to Claude quickly; see agent sessions at a glance and which ones need attention.
- **R4** Use the sensors: camera → ask Claude about what it sees; mic → talk to Claude.
- **R5** Power-efficient: dims and sleeps on its own, wakes on key, touch or lift.
- **R6** Shows device stats: battery %, charging, Wi-Fi, clock.
- **R7** Shareable open source (MIT).
- **R8** Not a security or hacking tool. (The starting point was Evil-M5Project; it targets
  ESP32/ESP32-S3 boards and is not wanted. Do not propose it or anything like it.)
- Dropped: Tailscale/SSH access to servers. It was a "bonus" and the API-direct design doesn't
  need it. It can return later as a separate module.

## Decisions

- **D1 — Claude API direct over HTTPS from the device.** No Mac, no SSH, no terminal. Two API
  surfaces:
  - **Messages API** for chat and vision (Plans 1–2).
  - **Managed Agents API** (beta) for agent sessions: list, watch status, approve tool calls,
    reply (Plan 3). This is how R3 is met.

  Consequence, stated once: the device cannot see Claude Code CLI sessions running on the Mac,
  because the public API has no access to them. If that ever becomes a must-have, the repo's
  `bridge/` (Python, polls `claude agents --json`) is the path. It is kept as optional and is not
  on this roadmap.
- **D2 — Firmware is a fork of Clientre5** (MIT, © 2026 Mozgi512), vendored into `firmware/` at
  upstream commit `252e07a` (2026-10-05, https://github.com/Mozgi512/Clientre5). It supplies the
  Tab5 platform: BSP, LVGL 9.4, the A164 keyboard driver, Wi-Fi through the C6 (esp-hosted), camera
  + hardware JPEG, audio codec, INA226 power monitor, RX8130 RTC/NTP, an AES-256-GCM secure store,
  settings, SD storage, OTA and a screensaver. Syncing with upstream is manual: diff against `252e07a`.
- **D3 — Remove from the fork:** `ime/` and the SKK dictionary (GPL-2.0; Japanese input not needed),
  `ssh/` and `components/libssh` (LGPL), `term/`, `servers/`, `components/microlink` and
  `components/wireguard_lwip` (Tailscale), `web/` (file manager), and the zh/ja UI strings and CJK
  fonts. Afterwards the firmware contains no GPL or LGPL code.
- **D4 — Raw HTTP.** There is no official C SDK, so the client uses `esp_http_client` with the ESP
  certificate bundle, and IDF's `json` component (cJSON). Pure logic (SSE parsing, request building,
  chat history, error text) lives in files with no ESP-IDF includes, so it is unit-tested on the Mac.
- **D5 — API key.** The user's Anthropic API key (billed on the API Console account, separately from
  a Claude Pro/Max subscription). Provisioned over USB serial by `firmware/tools/provision_key.py`,
  which reads `CLAUDE_COMPANION_API_KEY` or prompts with no echo. The user runs it themselves, so the
  key never passes through an agent transcript. Stored with the fork's secure store. Never logged
  beyond the last 4 characters, never committed. Do **not** use `ANTHROPIC_API_KEY` for this: setting
  it in the user's shell would switch their Claude Code login to API-key billing.
- **D6 — Debuggability.** A USB serial console provides `screenshot`: an LVGL snapshot is encoded by
  the P4 hardware JPEG encoder and sent as a framed base64 block, and `firmware/tools/screenshot.py`
  saves it as a .jpg. An agent can see the screen without an SD card (none is inserted). The fork's
  host UI preview (`firmware/tools/ui_preview`) renders screens on the Mac.
- **D7 — Visual identity:** dark graphite with an amber accent, taken from the published mockup
  (https://claude.ai/artifact/PBbMTBiaQXBn8i6mFojDcd) and `web/index.html`:

  | Token | Hex | Meaning |
  |---|---|---|
  | bg | `#0e1116` | screen background |
  | panel | `#161b22` | bars, raised areas |
  | panel-2 | `#1b212b` | cards, user bubbles |
  | line | `#2a323d` | hairlines |
  | fg | `#e9e7e1` | primary text |
  | muted | `#8a94a2` | secondary text |
  | amber | `#f4b545` | **needs you** — nothing else uses amber |
  | green | `#54d08a` | working / OK |
  | red | `#ef6f6f` | errors |

- **D8 — Name.** Working name "Claude Companion" (repo `claude-companion`). No Anthropic logos or
  brand assets on the device. The final name is the user's call before public release.

## Hardware (verified on this unit, 2026-10-22)

| Part | Detail |
|---|---|
| SoC | ESP32-P4 rev v1.3, dual core + LP core, 400 MHz, MAC `02:00:00:00:00:01` |
| Flash / PSRAM | 16 MB (mfr `0x46`, device `0x4018`) / 32 MB |
| Radio | ESP32-C6 over SDIO (esp-hosted): Wi-Fi + BLE |
| Display / touch | 1280×720 IPS, GT911 |
| Keyboard | A164, I²C `0x6D` (SDA G0, SCL G1, INT G50), reports fw `0x01` |
| Sensors | SC2356 2 MP camera, dual mics, IMU |
| Power / time | INA226, RX8130 RTC |
| USB serial | `/dev/cu.usbmodem211201` (check with `ls /dev/cu.usbmodem*`) |

Known issues:
- The C6's esp-hosted firmware reports `0.0.0` against host `3.0.7` ("major version mismatch — OTA
  coprocessor from host"). Wi-Fi events arrive; association has not been tested. If Wi-Fi will not
  connect, update the C6 slave firmware first.
- Board revision unknown. Clientre5 is only tested on board v3; it boots fine on this unit.
- No microSD card inserted.

## Environment (installed)

- ESP-IDF **v5.5.6** at `~/esp/esp-idf` (branch `release/v5.5`). Activate with `. ~/esp/esp-idf/export.sh`.
- esptool 5.6.0 (global, pyenv 3.11). cmake, ninja and dfu-util from Homebrew.
- Currently flashed: the Clientre5 v1.0 release image (verified by digest).
- Factory flash backup: `firmware/backups/tab5-factory-020000000001.bin` (16 MB, sha256
  `0000000000000000000000000000000000000000000000000000000000000000`, git-ignored). Restore with
  `esptool --port PORT write-flash 0x0 firmware/backups/tab5-factory-020000000001.bin`.

## Claude API rules for this device

Checked against the `claude-api` skill on 2026-10-22. Load that skill before writing API code, and
read its `curl/` docs and `shared/api-version-migration.md` (raw-HTTP section).

- `POST https://api.anthropic.com/v1/messages` with headers `x-api-key`,
  `anthropic-version: 2026-10-21`, `content-type: application/json` and
  `anthropic-beta: server-side-fallback-2026-07-01`.
- Model `claude-opus-5-5` (the skill's default; don't downgrade for cost unless the user asks).
  Confirm it with `GET /v1/models` when building.
- Body: `"stream": true`, `"fallbacks": "default"`, `"output_config": {"effort": "low"}` for chat,
  `"max_tokens": 8192`, and a short system prompt asking for concise answers that fit a 5-inch
  screen.
- Never send `temperature`, `top_p` or `top_k` (400 under 2026-10-21), never prefill an assistant
  turn, and never send a `thinking` parameter (Opus 5.5 runs adaptive thinking and rejects disabling
  it; thinking blocks stream with empty text by default).
- SSE events: `message_start`, `content_block_start`, `content_block_delta` (`text_delta`; thinking
  deltas ignored), `content_block_stop`, `message_delta` (carries `stop_reason`), `message_stop`,
  `ping`, `error`, and `fallback` when a server-side fallback switches models mid-reply. Text before
  a fallback must be discarded. Get the exact `fallback` wire shape from the skill's
  `shared/model-migration.md` (refusal and fallback sections). Ignore unknown events.
- Always check `stop_reason`: `end_turn` is normal; `max_tokens` → "Reply cut off at the length
  limit."; `refusal` → "Claude declined this request."
- Audio input is not documented anywhere in the API, so voice needs speech-to-text before the API.
  Plan 5 decides how.
- Managed Agents: beta header `managed-agents-2026-04-01`. Agents are created once (store the ID),
  sessions are created per task, events arrive over SSE, and tool confirmations carry approvals.
  Read the skill's `shared/managed-agents-*.md` before Plan 3.
- Cost: Opus 5.5 is $4 / $20 per million input/output tokens; a short chat exchange at low effort
  costs about a cent.

## Roadmap

| Plan | Delivers | Status |
|---|---|---|
| 1 Foundation + chat | debug console and screenshots, stripped and rebranded fork, Claude client, chat screen. Done when you type on the A164 and Claude's answer streams onto the screen. | `docs/superpowers/plans/2026-10-22-foundation-chat.md` |
| 2 Camera | photograph something, ask Claude about it | not written |
| 3 Agents dashboard | Managed Agents sessions as cards; amber = needs you; approve / deny / reply. Includes the typography pass (IBM Plex via the fork's font tools). | not written |
| 4 Power & vitals | backlight dim → light sleep; wake on keyboard INT, touch or IMU; full vitals bar | not written |
| 5 Voice | mic capture, speech-to-text choice, optional spoken replies | not written |
| 6 Release | final name, README, GitHub publish (visibility approved by the user), OTA manifest | not written |

Write each later plan when the previous one ships, against this spec.

## Open questions for the user

Ask each one when its plan starts, batched with any others due at that point.

- **Q1 (Plan 1, Task 6):** Wi-Fi network to join, and running `provision_key.py` with their
  Anthropic API key.
- **Q2 (Plan 3):** What should Managed Agents work on (repos, tools)? They run in Anthropic's
  sandbox, not on the Mac.
- **Q3 (Plan 5):** Speech-to-text: on-device commands only (ESP-SR), or a cloud STT service?
- **Q4 (Plan 6):** Final product name and when to publish.

## What else is in the repo

- `bridge/`, `web/`, `docs/PROTOCOL.md`: the earlier "local Claude Code sessions via the Mac"
  direction. It works and is kept as optional, but it is not part of the device roadmap.
- `firmware/`: vendored Clientre5, unmodified at the commit that adds this spec, plus
  `firmware/LICENSE.clientre5`.
