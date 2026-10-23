# Claude Companion

A dedicated desk device for your Claude Code sessions. It sits next to your keyboard,
shows every session at a glance, glows **amber** the moment one is waiting on you, and lets
you answer — by keyboard, voice, or camera — without switching windows.

Built for the **M5Stack Tab5** (ESP32-P4 + ESP32-C6, 5" 1280×720 touch, physical keyboard),
but the bridge and web dashboard run anywhere, so you can use it from a browser today.

> **Direction (2026-10-22):** take your Mac's Claude Code sessions to the couch. The Tab5 connects
> to the Mac over SSH through Tailscale, shows your background sessions on a native dashboard, and
> lets you answer one or fully take it over with `claude attach`. It uses your existing Claude
> subscription, with no API key and no phone. The firmware is a fork of
> [Clientre5](https://github.com/Mozgi512/Clientre5) (MIT) in `firmware/`. Read the
> [design spec](docs/superpowers/specs/2026-10-22-claude-tab5-design.md) and
> [Plan 1](docs/superpowers/plans/2026-10-22-session-control.md). The bridge material below is
> optional.

---

## Do you even need the bridge? No.

The device just needs to reach *something that can run `claude`*. The simplest version is
**no extra host software at all** — the device SSHes into your machine (over Tailscale) and
runs the normal CLI:

```sh
ssh mac 'claude agents --json --all'                # roster ("blocked" = waiting on you)
ssh mac 'claude -p --resume <id> "yes, ship it"'    # reply into a background session
```

Your Mac already has what this needs: **Remote Login** (sshd) and **Claude Code**. SSH keys
are the auth, Tailscale is the network. Poll the first command every couple of seconds and
you have the whole dashboard with no daemon to install or keep running.

**So there are two modes — the firmware speaks both:**

| | **SSH-direct** (recommended start) | **Bridge** (optional) |
|---|---|---|
| Host software | none (sshd + claude) | one Python file per host |
| Updates | poll `claude agents --json` | instant (hooks) + poll |
| Clients | the device | device **and** any browser |
| Camera / mic | `scp` file + remote `claude` | upload endpoints + STT hook |
| Multi-host | device SSHes to each | one bridge fans several in |

Even sensors work SSH-direct: `scp` the photo/audio over, then `claude -p --resume <id>
"look at /tmp/shot.jpg"`. Start SSH-direct; add the bridge later only for push updates, a
browser view, tidy sensor handling, or many hosts behind one endpoint. Everything below
documents the (optional) bridge.

---

## How it works

Claude Code sessions live on your machine(s). A small **bridge** on each host reads their
state and relays it to the device over your private Tailscale network.

```
  ┌──────────────┐    agents --json / hooks     ┌─────────────┐   SSE / HTTP    ┌──────────────┐
  │ Claude Code  │ ───────────────────────────▶ │   bridge    │ ──────────────▶ │  Tab5 / web  │
  │  sessions    │ ◀─────────────────────────── │ (this repo) │ ◀────────────── │  dashboard   │
  └──────────────┘   claude -p --resume (reply)  └─────────────┘  reply / upload └──────────────┘
```

- **See everything** — `claude agents --json --all` gives the roster; hooks give instant
  state changes. Each session shows as *waiting / working / done / idle*.
- **Answer fast** — quick **Yes / No**, a text reply, or (device) your **voice**. Replies go
  back via `claude -p --resume <id>` into background sessions.
- **Show Claude things** — snap a photo with the **camera** and pipe it into a session.
- **Reach every host** — the device talks to a bridge on any machine on your **tailnet**.

### One constraint worth knowing

You can reply to **background** sessions (`claude --bg`) and Channel sessions. You cannot
inject into a plain interactive `claude` running in a terminal — Claude Code does not allow
it. Those still show up (read-only); start sessions you want to answer remotely with `--bg`.

---

## Quick start (no hardware needed)

```sh
# 1. run the bridge, bound to your Tailscale IP so devices can reach it
COMPANION_HOST=$(tailscale ip -4 | head -1) python3 bridge/companion-bridge.py

# 2. open the dashboard URL it prints (includes the token), e.g.
#    http://100.x.y.z:8787/?token=…

# 3. start a background session and watch it appear
claude --bg "tidy up the imports in this repo"
```

Config via env: `COMPANION_HOST` (default `127.0.0.1`), `COMPANION_PORT` (`8787`),
`COMPANION_POLL_SECONDS` (`2`), `COMPANION_TOKEN` (auto-generated in
`~/.config/claude-companion/token`), `CLAUDE_BIN` (`claude`).

### Instant updates (optional)

Polling every 2s works out of the box. For *instant* state changes, add the hooks: copy
`bridge/settings.snippet.json` into `~/.claude/settings.json` (replace `ABSOLUTE_PATH`).
They just `curl` the bridge on each event and never block Claude.

### Sensors (device)

- **Camera → Claude:** the 📷 button on a card uploads an image to `/api/uploads/image`,
  which attaches it to that session. Works from the web client with a file picker today.
- **Mic → Claude:** audio uploads to `/api/uploads/audio`; speech-to-text is the next
  integration point (whisper.cpp or an API) — see the `TODO(M3)` in the bridge.

---

## Security

The bridge is **token-gated** and meant to be bound to your **Tailscale interface**, not
`0.0.0.0`. Treat the token like a password; it lives in `~/.config/claude-companion/token`
(git-ignored). The `/reply` and upload endpoints run `claude` on your behalf, so only expose
the bridge to devices you trust.

---

## Roadmap

- [x] **M0** — bridge: session roster, live SSE dashboard, text + Yes/No reply, image upload
- [ ] **M1** — validate Tab5 hardware (flash an existing SSH terminal, confirm keyboard/battery)
- [ ] **M2** — native Tab5 firmware (ESP-IDF + LVGL): dashboard, keyboard reply, camera capture
- [ ] **M3** — voice (mic → STT → reply, reply → TTS), IMU lift-to-wake, light-sleep + battery UI
- [ ] **M4** — permission approve/deny via the `PermissionRequest` hook / Channels; multi-host

## Repo layout

```
bridge/    Python 3 (stdlib) host bridge + hook script + settings snippet
web/       live dashboard (served by the bridge; also the firmware's UI reference)
firmware/  M5Stack Tab5 ESP-IDF app (M2)
docs/      architecture & protocol
```

## License

MIT — see [LICENSE](LICENSE).
