# Couch path check on hardware (Plan 1, Task 5)

Date: 2026-10-22. Mac: Claude Code 2.1.309, tailnet name `my-host` (100.64.0.18). Tab5: tailnet name
`tab5` (100.64.0.20), Wi-Fi `home-wifi`.

Result in one line: the Tab5 reaches the Mac over Tailscale and logs in with its own key. Terminal
rendering, keys and attaching from two places are not checked yet (moved, see below).

No secrets are recorded here: not the Tailscale auth key, not the Wi-Fi password, not any private key.

## Connection setup

| # | Check | Result | Evidence |
|---|---|---|---|
| 1 | Wi-Fi: the Tab5 joins the home network | PASS (after a fix) | The first boot failed in `esp_wifi_init`: esp_hosted 3.0.10 defaulted to the P4 Function-EV board and clamped the Tab5 SDIO pins. Task 4a set `CONFIG_ESP32P4_TAB5_C6_BOARD=y` and pinned esp_hosted to `==3.0.10`; SDIO came up and a scan listed networks (the scan screenshot shows neighbours' network names, so it is not kept here). The Tab5 then joined `home-wifi` and got 192.168.1.60 (`img/couch-home-after-tailnet.jpg`, `img/couch-ssh-servers.jpg`). Diagnosis: `a local review file (not kept)`. |
| 2 | Tailnet join | PASS (after one reboot) | The first attempt sat on "Status: Connecting" with "No peers yet" and the old device name `clientre5`. After one reboot it registered on the first try as `tab5` (100.64.0.20). The Mac showed in the peer list. The auth key was typed by the user on the Tab5 and is not recorded. |
| 3 | Server added | PASS | Server "Mac": `you@100.64.0.18:22`, key auth with the device key (`img/couch-ssh-servers.jpg`). |
| 4 | Device key generated and authorized | PASS | Task 4b: the Tab5 generates an ed25519 key on first use, keeps it in NVS, and `sshkey` prints the public line only. Fingerprint `SHA256:EXAMPLEDeviceKeyFingerprint0000000000000000` (same after a reboot and after flashing the fix round). With the user's approval it was added to `~/.ssh/authorized_keys` as `from="100.64.0.0/10" ssh-ed25519 ... claude-companion` by `firmware/tools/authorize_device_key.sh`. |
| 5 | SSH key-auth connect | PASS | Connected from the Tab5 with the device key. Tailscale used a **direct WireGuard path over the LAN** (Mac at 192.168.1.50:41641), not a DERP relay. The screenshot of this screen shows the user's own tmux and Claude panes, so it is not kept; in words: the Tab5 terminal shows a live zsh/tmux session of the Mac, with the user's tmux panes and a Claude pane. |

## Findings

| # | Finding | Result | Evidence and decision |
|---|---|---|---|
| 6 | An interactive login lands in the user's tmux | FINDING | The user's `.zshrc` (oh-my-zsh tmux autostart) attaches any interactive login to their live tmux, so typing in the Tab5 terminal would type into their real panes and Claude sessions. We stopped sending keys and rebooted the Tab5; the user detached the stale tmux client. The user does not want tmux on the Tab5. **R15:** companion code never opens an interactive login shell on the Mac. It always uses a pty exec channel running an explicit `claude attach <id>`: a non-interactive zsh does not read `.zshrc`, so no tmux. |
| 7 | `claude -p --resume <id> -- '<text>'` on a running session | FAIL (refused) | Claude Code 2.1.309 refuses it while the session runs. It only works for a stopped session. **R11:** quick answers use attach-paste instead (next table). The test `-p --resume` against a permission prompt could not be run: `companion-test-perm` never blocked. |
| 8 | `waitingFor` in `claude agents --json` | FAIL (absent) | Real output has no `waitingFor` field at all, only `state` (blocked / working / done) and `status` (idle / busy). **R12:** a permission prompt cannot be told from an input request, so every NEEDS_YOU background card gets the answer field, with Ctrl+O to open the session. `claude logs` shows what it waits for. |

## Quick answer by attach-paste (R11)

Verified on the Mac with a pty (spike `a local review file (not kept)`). The
answer reached the session as exactly one user message with the exact text, in every test, and the session kept running.

Recipe, on a pty channel running `~/.local/bin/claude attach <id>` (120x40, `TERM=xterm-256color`):

1. Read and discard output until it has been quiet for 1.0 s (0.75 to 1.5 s for an idle or blocked session).
2. Send Ctrl+U four times (`\x15` x4), wait 0.2 s. This clears a leftover draft.
3. Send `ESC[200~` + text + `ESC[201~`, wait 0.15 s, then send `\r`.
4. Keep reading about 1 s, then close the channel (or send Ctrl+Z, which exits attach with code 0).

Rules:
- Never detach with the left arrow: it opens the agent view, where a stray byte can reply to or delete another session.
- Never send a bare Esc: it interrupts a working turn.
- Without the paste markers any CR in the text submits early. Inside a paste, CRLF becomes LF and a tab becomes 4 spaces.
- Sending before the UI has rendered loses the CR and leaves a draft that the next answer is appended to.
- On a working session the text is queued as the next user message, and the quiet-wait never ends, so cap it at 2 to 3 s.

Timing, spawn to exit: about 3.0 to 3.3 s with the defaults (quiet 1.0 s, post 1.5 s); about 1.3 s tight (quiet 0.5 s, post 0.3 s).
Task 6 builds this as `cc_paste_build` and `cc_link_paste`. `--bg --resume` is only right for a session with no live process.

## Moved checks (R14)

| # | Check | Result | Where it happens now |
|---|---|---|---|
| 9 | Step 3: colors, box drawing and `✻ ⏺ ⎿ ✓ ●`, spinner, typing `blue` + Enter, arrows, Esc, Ctrl+C, ← and Ctrl+Z, `tput cols` >= 100 | MOVED | Right after Task 7 Step 2, through `ui_terminal_open_command` (pty + exec `claude attach <id>`), the path the product uses. Not run here because the interactive shell is the tmux one (finding 6). |
| 10 | Step 5: attach `companion-test-input` from the Mac and the Tab5 at once | MOVED | Same place. The spike did not test two attached clients either. |

## Open items

- Typing latency and the first-connection time were not measured on the Tab5; they go with the moved checks.
- The device key comment says `claude-companion@tab5` because the Tailscale device-name setting was empty at key time.
