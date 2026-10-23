# Session Control Implementation Plan (Plan 1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** From the couch, the Tab5 shows the Claude Code sessions running on your Mac, highlights the ones that need you in amber, and lets you answer them or take one over completely.

**Architecture:** The base is a trimmed and rebranded Clientre5 fork, keeping its SSH, terminal and Tailscale. A pure `companion/` layer parses `claude agents --json` and builds safe remote command lines; it is unit-tested on the Mac. A link task keeps one SSH session to the Mac over Tailscale and polls it. The new home screen is an LVGL dashboard, and opening a session runs `claude attach <id>` in the fork's terminal.

**Tech Stack:** C, ESP-IDF v5.5.6 (target `esp32p4`), LVGL 9.4 + esp_lvgl_port 2.8, libssh 0.11 (`components/libssh`), MicroLink Tailscale client, cJSON (IDF `json`), `esp_console`, `esp_driver_jpeg`, Unity for host tests, Python 3 + pyserial for host tools.

**Spec:** `docs/superpowers/specs/2026-10-22-claude-tab5-design.md`

## Global Constraints

- Build: `. ~/esp/esp-idf/export.sh && cd firmware && idf.py set-target esp32p4 && idf.py build` (ESP-IDF v5.5.6).
- Dev flashing: `idf.py -p /dev/cu.usbmodem211201 flash`, logs with `idf.py -p /dev/cu.usbmodem211201 monitor` (exit with `Ctrl+]`). `idf.py flash` keeps NVS, so Wi-Fi, Tailscale and server settings survive reflashing.
- Host tests: `cmake -S firmware/test/host -B firmware/test/host/build && cmake --build firmware/test/host/build && ctest --test-dir firmware/test/host/build --output-on-failure`.
- Every task runs in its own git worktree off `main` and merges back when its checks pass (user rule). The top-level checkout stays on `main`.
- Match Clientre5's C style: 4-space indent, function opening brace on its own line, `static const char *TAG = "...";`, `s_` prefix for file statics, `/* */` comments.
- `firmware/main/companion/cc_sessions.c` and `cc_cmd.c` include no ESP-IDF headers and also build in `firmware/test/host`.
- Every command sent to the Mac is built by a `cc_cmd_*` function. Never build them with ad-hoc `snprintf`.
- The Claude path setting defaults to `~/.local/bin/claude` (spec D3).
- **Test only against sessions you create**, named `companion-test-*`. Never reply to, type into or stop any other session on the user's Mac. Stop the test sessions when a task is done.
- No GPL code after Task 3. libssh (LGPL-2.1) stays. Keep `firmware/LICENSE.clientre5` and the attribution line.
- Colors come only from the spec's D9 table. Amber means "needs you" and nothing else.
- No secrets in the repo or the logs: SSH private keys, Tailscale auth keys and Wi-Fi passwords stay on the device.
- Check every visible change with `python3 firmware/tools/screenshot.py` and look at the image.
- This is a session companion. It is not a security tool.

## Review Focus

1. Reply text containing quotes, `$( )`, backticks or newlines → reaches Claude exactly and never runs as a shell command. Covered by Task 4 `test_shell_quote_roundtrip`.
2. Mac asleep or unreachable, or Tailscale reconnecting → the status says so, the link retries with backoff, and the UI and console stay responsive. Covered by Task 6's bad-host check and Task 7's offline screenshots.
3. A session stops between a refresh and your action → a readable error, and the list refreshes. Covered by Task 6's reply-to-stopped check and Task 7's open-stopped check.
4. Interactive (not backgrounded) sessions → never offered reply or open; shown with the `/bg` hint. Covered by Task 4 `test_parse_flags` and Task 7's render check.
5. Claude Code changes its JSON (new fields, new states, missing `waitingFor`) → still parses, and an unknown state shows as idle. Covered by Task 4 `test_parse_unknown_state_is_idle`.

---

### Task 1: Baseline build of the vendored fork

**Files:**
- Modify: `.gitignore` (stop ignoring `firmware/dependencies.lock`; add `firmware/test/host/build/`)
- Create: `firmware/dependencies.lock` (generated)

**Interfaces:**
- Produces: a known-good build and its app size, recorded in the commit message for later comparison.

- [ ] **Step 1: Build**

Run: `. ~/esp/esp-idf/export.sh && cd firmware && idf.py set-target esp32p4 && idf.py build`
Expected: `Project build complete`, and `firmware/build/clientre5.bin` exists.

- [ ] **Step 2: Flash and check the boot log**

Run: `idf.py -p /dev/cu.usbmodem211201 flash`, then `idf.py -p /dev/cu.usbmodem211201 monitor`
Expected: the log contains `app: boot complete in`.

- [ ] **Step 3: Commit**

```bash
git add .gitignore firmware/dependencies.lock
git commit -m "build: pin component versions for the Clientre5 baseline (app NNNN KB)"
```

### Task 2: USB debug console with screenshots

**Files:**
- Create: `firmware/main/sys/debug_console.c`, `firmware/main/sys/debug_console.h`
- Modify: `firmware/main/sys/screenshot.c`, `firmware/main/sys/screenshot.h`
- Modify: `firmware/main/app_main.c` (call `debug_console_start()` after the UI is up)
- Modify: `firmware/main/CMakeLists.txt` (add `console` and `esp_driver_jpeg` to `REQUIRES`)
- Create: `firmware/tools/screenshot.py`, `firmware/tools/test_screenshot.py`

**Interfaces:**
- Produces: `void debug_console_start(void);` starts an `esp_console` REPL on USB-Serial-JTAG with prompt `tab5> `.
- Produces: `esp_err_t debug_console_register(const esp_console_cmd_t *cmd);`
- Produces: `bool screenshot_dump_usb(void);` It takes an RGB888 LVGL snapshot under `lvgl_port_lock(0)`, copies it into a buffer from `jpeg_alloc_encoder_mem`, encodes it with the hardware encoder at quality 80, and prints the frame described below. Logging is set to `ESP_LOG_NONE` while the frame prints, then restored.
- Frame format: the line `@@SHOT jpeg <byte_len> <width> <height>`, then base64 lines of 76 characters, then the line `@@END`.
- Produces: `decode_frame(lines: Iterable[str]) -> tuple[bytes, int, int]` in `screenshot.py`. It ignores every line outside the markers.
- CLI: `python3 firmware/tools/screenshot.py [--port PORT] [--out PATH]` sends `screenshot`, waits up to 20 s for `@@END`, writes the JPEG (default `firmware/build/screenshots/shot-YYYYmmdd-HHMMSS.jpg`) and prints `saved <path> <w>x<h>`.

- [ ] **Step 1: Write the failing tests** in `firmware/tools/test_screenshot.py`:
  - `test_decode_frame_ignores_noise`: log lines around a frame built from `b"\xff\xd8hello\xff\xd9"` → returns those exact bytes and the header's width and height.
  - `test_missing_end_raises`: no `@@END` → `ValueError`.
  - `test_length_mismatch_raises`: the header `byte_len` differs from the decoded length → `ValueError`.

- [ ] **Step 2: Run them to make sure they fail**

Run: `python3 -m unittest firmware/tools/test_screenshot.py -v`
Expected: FAIL (`decode_frame` not defined)

- [ ] **Step 3: Implement `decode_frame` and the CLI**, then run the tests until all 3 pass

- [ ] **Step 4: Implement the console, the `screenshot` command and `screenshot_dump_usb`**, then build and flash

- [ ] **Step 5: Take a screenshot from the device**

Run: `python3 firmware/tools/screenshot.py`
Expected: `saved firmware/build/screenshots/shot-….jpg 1280x720`. The image shows the Clientre5 home screen.

- [ ] **Step 6: Commit**

```bash
git add firmware/main/sys firmware/main/app_main.c firmware/main/CMakeLists.txt firmware/tools/screenshot.py firmware/tools/test_screenshot.py
git commit -m "feat(debug): USB serial console with JPEG screenshots"
```

### Task 3: Trim and rebrand the fork

**Files:**
- Delete: `firmware/main/ime/` (including `dict/skk_m.bin`), `firmware/main/web/`, and the dictionary generator in `firmware/tools/`
- Modify: `firmware/main/CMakeLists.txt`, `firmware/main/app_main.c`, `firmware/main/ui/ui_core.c`, `ui_settings.c`, `ui_softkbd.c`, `ui_terminal.c`, `ui_screensaver.c`, `ui_devinfo.c`, `firmware/main/input/keyboard.c` (drop the `Ctrl+Space` input-source switch), `firmware/main/sys/sysinfo.c`, `firmware/main/sys/screenshot.c`, `firmware/main/i18n/` (English only; drop the language picker), `firmware/main/settings/settings.c`, `settings.h`
- Modify: `firmware/CMakeLists.txt` (`project(claude_companion)`), the boot splash text (`grep -rn "STARTING SYSTEM" firmware/main`), the USB descriptor strings, the default Tailscale node name (`claude-companion`), `firmware/main/ota/` (empty manifest URL; hide the OTA tile while it's empty), `firmware/tools/merge_flash.sh` (outputs named `claude_companion*`)
- Modify: `firmware/README.md` (drop the Japanese input and web file manager sections; third-party table becomes libssh LGPL-2.1, MicroLink MIT, wireguard-lwip BSD-3, LVGL MIT, ESP-IDF Apache-2.0, Clientre5 MIT)
- Create: `firmware/tools/check_trim.sh`

**Interfaces:**
- Produces: the build output `firmware/build/claude_companion.bin`.
- The settings storage format must stay compatible. Leave removed settings fields in place, marked reserved, so NVS data from older builds still loads.
- The terminal font is not touched.

- [ ] **Step 1: Write `firmware/tools/check_trim.sh`**

It exits 1 and prints the hits if any of these are true:
- `grep -rIl -E 'ime_|SKK|skk_|pinyin|romaji|webfm' firmware/main firmware/CMakeLists.txt` finds anything;
- `firmware/main/ime` or `firmware/main/web` still exists;
- `grep -rIn 'Clientre' firmware/main firmware/CMakeLists.txt firmware/tools/merge_flash.sh` finds any line other than the attribution line in `ui_devinfo.c`.

Otherwise it prints `trim: OK`.

- [ ] **Step 2: Run it to make sure it fails**

Run: `sh firmware/tools/check_trim.sh`
Expected: exit 1 with hits

- [ ] **Step 3: Remove the modules, fix the call sites and rename to "Claude Companion"**

Splash: `Claude Companion` / `STARTING`. Device info adds the line `Based on Clientre 5 by Mozgi512 (MIT)`. No Anthropic logos.

- [ ] **Step 4: Check and build**

Run: `sh firmware/tools/check_trim.sh && idf.py build`
Expected: `trim: OK`, `Project build complete`, `firmware/build/claude_companion.bin` exists, and the app is smaller than the Task 1 baseline. Record both sizes.

- [ ] **Step 5: Flash and screenshot**

Take screenshots of the home screen, Settings and Device info, navigating with the arrow keys and Enter.
Expected: `boot complete` in the log; no Web Files tile and no language picker; the attribution line appears in Device info; nothing crashes.

- [ ] **Step 6: Commit**

```bash
git add -A firmware
git commit -m "refactor(firmware): drop IME/SKK and web file manager, rebrand to Claude Companion"
```

### Task 4: Session model and safe command builders (pure)

**Files:**
- Create: `firmware/main/companion/cc_sessions.c`, `firmware/main/companion/cc_sessions.h`
- Create: `firmware/main/companion/cc_cmd.c`, `firmware/main/companion/cc_cmd.h`
- Create: `firmware/test/host/CMakeLists.txt`. It builds the pure sources plus `$ENV{IDF_PATH}/components/json/cJSON/cJSON.c` and Unity from `$ENV{IDF_PATH}/components/unity/unity/src/unity.c` with the host compiler, and registers each test with `add_test`.
- Create: `firmware/test/host/test_cc_sessions.c`, `firmware/test/host/test_cc_cmd.c`
- Create: `firmware/test/host/fixtures/agents_synthetic.json` (content below) and `firmware/test/host/fixtures/agents_captured.json` (captured in Step 1)
- Modify: `firmware/main/CMakeLists.txt` (add the `companion/` sources)

**Interfaces:**
- Produces, in `cc_sessions.h`:

```c
typedef enum { CC_NEEDS_YOU, CC_WORKING, CC_IDLE, CC_DONE, CC_ENDED } cc_state_t;

typedef struct {
    char id[16];            /* short id: attach / logs */
    char session_id[40];    /* UUID: -p --resume */
    char name[96];
    char repo[64];          /* basename of cwd, trailing slash ignored */
    cc_state_t state;
    bool background;        /* kind == "background": can be answered and opened */
    bool needs_permission;  /* waitingFor == "permission prompt" */
    int64_t started_ms;
} cc_session_t;

int cc_sessions_parse(const char *json, cc_session_t *out, int cap);
/* Returns the count (≤ cap), or -1 if the input isn't a JSON array.
   State comes from "state" if present, else "status", else idle:
     blocked / waiting → NEEDS_YOU;  working / busy → WORKING;  idle → IDLE;
     done → DONE;  failed / stopped → ENDED;  anything else → IDLE.
   Entries whose id or sessionId fails validation are skipped.
   Order: NEEDS_YOU, WORKING, IDLE, DONE, ENDED; newest startedAt first within each state. */
int cc_sessions_visible(cc_session_t *s, int n, int max_finished);
/* compacts in place: every active session plus the newest max_finished DONE/ENDED; keeps order */
size_t cc_summary(const cc_session_t *s, int n, char *out, size_t cap);
/* "2 need you · 1 working", "1 needs you", "3 working", or "All quiet" */
size_t cc_age(int64_t now_ms, int64_t then_ms, char *out, size_t cap);
/* under 60 s "45s", under 60 min "12m", under 24 h "3h", otherwise "2d" */
```

- Produces, in `cc_cmd.h`:

```c
bool cc_path_valid(const char *p);          /* only [A-Za-z0-9_./~-], 1–63 chars */
bool cc_id_valid(const char *id);           /* ^[0-9a-f]{8}$ */
bool cc_session_id_valid(const char *sid);  /* lowercase 8-4-4-4-12 hex UUID */
bool cc_shell_quote(const char *in, char *out, size_t cap);   /* '…' with ' written as '\'' */
bool cc_cmd_agents(const char *claude, char *out, size_t cap);
/* "<claude> agents --json --all" */
bool cc_cmd_logs(const char *claude, const char *id, char *out, size_t cap);
/* "<claude> logs <id> | tail -n 40" */
bool cc_cmd_reply(const char *claude, const char *sid, const char *text, char *out, size_t cap);
/* "<claude> -p --resume <sid> <quoted text>" */
bool cc_cmd_attach(const char *claude, const char *id, char *out, size_t cap);
/* "<claude> attach <id>" */
size_t cc_strip_ansi(char *s);   /* removes CSI and OSC escape sequences in place; returns the new length */
/* Every builder returns false for an invalid path, id or sid, empty text, or output that doesn't fit cap. */
```

- `agents_synthetic.json`:

```json
[
 {"id":"aaaaaaa1","cwd":"/Users/me/code/alpha","kind":"background","startedAt":1000,"sessionId":"11111111-1111-4111-8111-111111111111","name":"done one","state":"done"},
 {"id":"aaaaaaa2","cwd":"/Users/me/code/beta","kind":"background","startedAt":3000,"sessionId":"22222222-2222-4222-8222-222222222222","name":"needs input","state":"blocked","waitingFor":"input needed"},
 {"id":"aaaaaaa3","cwd":"/Users/me/code/gamma/","kind":"background","startedAt":2000,"sessionId":"33333333-3333-4333-8333-333333333333","name":"needs approval","state":"blocked","waitingFor":"permission prompt"},
 {"id":"aaaaaaa4","cwd":"/Users/me/code/delta","kind":"interactive","startedAt":4000,"sessionId":"44444444-4444-4444-8444-444444444444","name":"desk session","status":"busy"},
 {"id":"BAD!","cwd":"/x","kind":"background","startedAt":5000,"sessionId":"55555555-5555-4555-8555-555555555555","name":"bad id","state":"working"},
 {"id":"aaaaaaa6","cwd":"/Users/me/code/eps","kind":"background","startedAt":6000,"sessionId":"66666666-6666-4666-8666-666666666666","name":"future","state":"hibernating","extra":{"x":1}}
]
```

- [ ] **Step 1: Capture real output on the Mac**

Run: `~/.local/bin/claude --bg --name companion-test-input "Ask me what my favorite color is, then wait for my answer."` and `~/.local/bin/claude --bg --name companion-test-perm "Create the file /tmp/companion-test.txt containing hi."`

Once both show `"state": "blocked"`, save `~/.local/bin/claude agents --json --all` to `agents_captured.json`. Replace the home directory in every `cwd` with `/Users/me` before saving. Note the real `waitingFor` values; if they differ from the docs, update `cc_sessions_parse` and the spec's D5 to match what you saw.

- [ ] **Step 2: Write the failing tests**

`test_cc_sessions.c`:
  - `test_parse_synthetic_order`: count 5; ids in order `aaaaaaa2, aaaaaaa3, aaaaaaa4, aaaaaaa6, aaaaaaa1`.
  - `test_parse_flags`: only `aaaaaaa3` has `needs_permission`; `aaaaaaa4` has `background == false`; `aaaaaaa3`'s repo is `"gamma"`.
  - `test_parse_unknown_state_is_idle`: `aaaaaaa6` → `CC_IDLE`.
  - `test_parse_invalid_json`: `"not json"` → -1; `"{}"` → -1; `"[]"` → 0.
  - `test_parse_cap`: cap 2 → returns 2, with ids `aaaaaaa2, aaaaaaa3`.
  - `test_parse_real_capture`: `agents_captured.json` → ≥ 2 entries, and the `companion-test-*` sessions are `CC_NEEDS_YOU`.
  - `test_visible_limits_finished`: synthetic data plus three more done sessions, `max_finished` 2 → every active session plus the 2 newest done.
  - `test_summary_text`: synthetic → `"2 need you · 1 working"`; only one blocked → `"1 needs you"`; only done → `"All quiet"`.
  - `test_age_format`: 45 000 ms → `"45s"`; 720 000 → `"12m"`; 10 800 000 → `"3h"`; 172 800 000 → `"2d"`.

`test_cc_cmd.c`:
  - `test_shell_quote_roundtrip`: for each of `yes`, `it's fine`, `` $(touch /tmp/cc-pwned) `id` "q" ``, `line1\nline2` and `back\\slash`, run `/bin/sh -c "printf %s <quoted>"` through `popen` → the output equals the input exactly, and `/tmp/cc-pwned` does not exist afterwards.
  - `test_cmd_reply_exact`: `("~/.local/bin/claude", "22222222-2222-4222-8222-222222222222", "it's ok")` → `~/.local/bin/claude -p --resume 22222222-2222-4222-8222-222222222222 'it'\''s ok'`.
  - `test_cmd_exact`: `cc_cmd_agents` → `~/.local/bin/claude agents --json --all`; `cc_cmd_attach(…, "aaaaaaa2")` → `~/.local/bin/claude attach aaaaaaa2`; `cc_cmd_logs(…, "aaaaaaa2")` → `~/.local/bin/claude logs aaaaaaa2 | tail -n 40`.
  - `test_cmd_rejects`: an invalid sid, empty text, the path `~/bin/claude;rm`, the path `my claude`, the id `AAAAAAA2`, and a `cap` one byte too small → all return false.
  - `test_strip_ansi`: `"\x1b[1;32m✓ done\x1b[0m\x1b]0;title\x07"` → `"✓ done"`.

- [ ] **Step 3: Run them to make sure they fail**

Run the host test command.
Expected: FAIL (undefined symbols)

- [ ] **Step 4: Implement `cc_sessions.c` and `cc_cmd.c`**, then run the tests until they all pass

- [ ] **Step 5: Firmware build**

Run: `idf.py build`
Expected: `Project build complete`

- [ ] **Step 6: Clean up and commit**

Leave the two `companion-test-*` sessions running for Task 5.

```bash
git add firmware/main/companion firmware/main/CMakeLists.txt firmware/test/host
git commit -m "feat(companion): parse claude agents JSON and build safe remote commands"
```

### Task 5: Prove the couch path with the user

No firmware changes unless a check fails. This is the only task that needs the user.

**Files:**
- Create: `docs/superpowers/notes/2026-10-22-couch-path-check.md`
- Create: `firmware/tools/authorize_device_key.sh`

**Interfaces:**
- `authorize_device_key.sh "<public key line>"` appends `from="100.64.0.0/10" <key> claude-companion` to `~/.ssh/authorized_keys`. It first creates `~/.ssh` (mode 700) and the file (mode 600) if needed, and it refuses to add a key that is already present.
- Produces: pass/fail findings that Tasks 6–7 rely on: whether `-p --resume` answers permission prompts, what happens when you attach from two places at once, and the terminal's column count.

- [ ] **Step 1: Ask the user once for everything** (spec, "Things only the user can do"):
  - turn on Remote Login;
  - create a Tailscale auth key and enter it on the Tab5;
  - join Wi-Fi on the Tab5;
  - approve adding the Tab5's public key.

- [ ] **Step 2: Set up the connection**

The user does the Tab5 side. Take a screenshot at each screen and guide from what you see:
1. Join Wi-Fi.
2. Join the tailnet, and confirm the Mac appears in the peer list.
3. Add the Mac as a server.
4. Generate the device key and show its public key.

Then, with the user's approval, run `sh firmware/tools/authorize_device_key.sh "<key>"`, and connect using the key.
Expected: a shell prompt from the Mac in the Tab5 terminal. If Wi-Fi won't associate, check for the C6 version mismatch (spec, Known issues).

- [ ] **Step 3: Check Claude Code in the terminal**

In the Tab5 terminal, run `~/.local/bin/claude attach companion-test-input`, then go through these. Take a screenshot for each and record pass/fail:
  - colors render;
  - box drawing and the symbols `✻ ⏺ ⎿ ✓ ●` render;
  - the spinner animates;
  - typing `blue` and pressing Enter continues the session;
  - the arrow keys, Esc and Ctrl+C reach Claude;
  - ← returns to agent view, and Ctrl+Z returns to the shell;
  - `tput cols` reports ≥ 100.

- [ ] **Step 4: Check quick answers**

From the Tab5 shell, run `~/.local/bin/claude -p --resume <sessionId of companion-test-perm> 'yes'`. Record whether it approves the permission prompt; this decides spec D5.

Also time the first connection and note how typing latency feels.

- [ ] **Step 5: Check attaching from two places**

Attach `companion-test-input` on the Mac and on the Tab5 at the same time. Record what each side sees.

- [ ] **Step 6: Write the notes**

Write `docs/superpowers/notes/2026-10-22-couch-path-check.md` with each check, pass/fail and the screenshot paths. Update spec D5 with the Step 4 result. If a terminal check failed, add a task before Task 7 that fixes it in `firmware/main/term/`, with a test.

- [ ] **Step 7: Clean up and commit**

Run `~/.local/bin/claude stop companion-test-perm` and remove `/tmp/companion-test.txt`. Keep `companion-test-input` for Task 6.

```bash
git add docs/superpowers firmware/tools/authorize_device_key.sh
git commit -m "docs: couch path check on hardware"
```

### Task 6: Link to the Mac and session polling

**Files:**
- Create: `firmware/main/companion/cc_link.c`, `firmware/main/companion/cc_link.h`
- Create: `firmware/main/companion/cc_console.c`
- Modify: `firmware/main/settings/settings.c`, `settings.h` (add `companion_server` and `claude_path`)
- Modify: `firmware/main/app_main.c`, `firmware/main/CMakeLists.txt`

**Interfaces:**
- Consumes: `cc_sessions_parse`, `cc_cmd_*` (Task 4); `debug_console_register` (Task 2); the fork's `ssh/` helpers for loading the device key and pinning host keys.
- Produces, in `cc_link.h`. It opens its own libssh session, separate from the terminal's:

```c
esp_err_t cc_link_start(void);
/* connects to the companion server over the tailnet using the device key;
   reconnects with backoff 1, 2, 4, 8, 16, 30 s */
bool cc_link_connected(void);
const char *cc_link_status_text(void);
/* "connected <host>" | "connecting to <host> (retry in Ns)" | "no Mac set up" */
esp_err_t cc_link_exec(const char *cmd, char *out, size_t cap, int *exit_status,
                       char *err, size_t err_cap, int timeout_ms);
/* one exec channel on the shared session; out holds stdout and err the first line of stderr, both NUL-terminated.
   Returns ESP_ERR_INVALID_STATE when not connected, ESP_ERR_TIMEOUT on timeout. */
void cc_poll_set_rate(int seconds);
/* 3 while the dashboard is visible, 30 when dimmed, 0 = paused */
typedef void (*cc_sessions_cb_t)(const cc_session_t *s, int n, void *ctx);
void cc_poll_subscribe(cc_sessions_cb_t cb, void *ctx);
/* called on the poll task after each successful refresh */
const char *cc_settings_claude_path(void);
/* the default is "~/.local/bin/claude" */
int cc_settings_server(void);
/* index in the fork's server list, or -1 */
```

- Console commands:
  - `companion host <server name>`
  - `companion claude-path <path>` (rejected unless `cc_path_valid`)
  - `companion status` (prints `cc_link_status_text()`)
  - `sessions` prints one line per session: `<STATE> <id> <repo> <name>`
  - `reply <id> <text…>` looks up the `session_id` from the last poll. It prints `sent`, `no such session`, or `error: <stderr line>`.
  - `logs <id>` prints the output with ANSI stripped.

- [ ] **Step 1: Implement the settings fields, `cc_link`, the poller and the console commands**

Build and flash.
Expected: `Project build complete`

- [ ] **Step 2: Connect**

Run: `companion host <the Mac's server name>`, then `companion status`.
Expected: `connected <host>`

- [ ] **Step 3: Compare the session list with the Mac**

Run: `sessions`. On the Mac, run `~/.local/bin/claude agents --json --all`.
Expected: the same ids and matching states, with `companion-test-input` listed first as `NEEDS_YOU` if it's still blocked.

- [ ] **Step 4: Check a quick answer**

Run: `reply <companion-test-input id> green`. Then run `~/.local/bin/claude logs <id>` on the Mac.
Expected: `sent`, and the logs show the session received `green`.

- [ ] **Step 5: Check an unreachable host**

Run: `companion host nonexistent-host`, then `companion status` a few times.
Expected: `connecting to nonexistent-host (retry in Ns)` with growing waits. The console responds immediately. Set the host back afterwards.

- [ ] **Step 6: Check reconnecting**

Run: `wifi leave`, wait 10 s, then `wifi join …`.
Expected: `connected <host>` within 35 s, without rebooting.

- [ ] **Step 7: Check replying to a stopped session**

Run `~/.local/bin/claude stop companion-test-input` on the Mac, then `reply <its id> hi`.
Expected: `no such session` once the next poll drops it, or `error: <stderr line>` if you're faster than the poll. Never a hang.

- [ ] **Step 8: Commit**

```bash
git add firmware/main/companion firmware/main/settings firmware/main/app_main.c firmware/main/CMakeLists.txt
git commit -m "feat(companion): SSH link to the Mac with session polling and console commands"
```

### Task 7: Dashboard, quick answers and take-over

**Files:**
- Create: `firmware/main/ui/ui_dashboard.c`, `firmware/main/ui/ui_dashboard.h`
- Modify: `firmware/main/ssh/` and `firmware/main/ui/ui_terminal.c` (optional remote command: pty + exec instead of a login shell; `Ctrl+Q` handled on the device and never sent)
- Modify: `firmware/main/ui/ui_core.c` (the dashboard is the home screen; the old tiles move to a `More` row)
- Modify: `firmware/main/ui/ui_settings.c` (a "Companion" section with Mac and Claude path)
- Modify: `firmware/main/input/keyboard.c` (keys go to the dashboard while it's active)
- Modify: `firmware/tools/ui_preview/preview.c` (add a `dashboard` case)

**Interfaces:**
- Consumes: `cc_poll_subscribe`, `cc_poll_set_rate`, `cc_link_exec`, `cc_link_status_text` (Task 6); `cc_sessions_visible`, `cc_summary`, `cc_age`, `cc_cmd_reply`, `cc_cmd_logs`, `cc_cmd_attach`, `cc_strip_ansi` (Task 4); the fork's INA226 battery reading (in `firmware/main/sys/`).
- Produces: `void ui_terminal_open_command(int server_idx, const char *cmd, void (*on_close)(void *ctx), void *ctx);`
- Layout:
  - Top: a vitals bar with battery % (plus `charging` when charging), Wi-Fi, a link dot (green connected, muted connecting, red unreachable) and the clock (`HH:MM`).
  - Below it: a summary line from `cc_summary`.
  - Main area: a card list from `cc_sessions_visible(…, 4)`. Each card shows the name, repo, a state pill (`NEEDS YOU` amber, `WORKING` green, `IDLE` muted, `DONE` muted, `ENDED` red) and the age from `cc_age`.
- Selection: ↑/↓ moves it. The selected card expands to show the last 6 lines of `cc_cmd_logs`, ANSI stripped, refreshed every 5 s while selected.
- Actions on the selected card:
  - Background + needs input: an answer field `Type an answer…`. Enter sends it with `cc_cmd_reply`, and the card shows `Sent` in muted.
  - Background + permission prompt: `Open to approve`. Enter opens the session (unless Task 5 showed that quick answers approve).
  - Other background states: Enter opens the session; `R` focuses a message field.
  - Interactive: no actions. Show the muted hint `On your Mac — run /bg there to take it over here.`
- Opening a session: `ui_terminal_open_command(server, cc_cmd_attach(…))`. `Ctrl+Q` closes the channel and returns to the dashboard with the toast `Session still running on your Mac`. If the remote command exits, return to the dashboard; if it exits with an error, show its first stderr line.
- Status messages:
  - no Mac set up: `Set up your Mac in Settings → Companion.`
  - connecting: `Connecting to <host>…` (muted)
  - connecting for ≥ 30 s: `Can't reach <host>. Is your Mac awake?` (red)
  - no sessions: `No sessions on <host>. Start one on your Mac with claude --bg.`
- Polling rate: `cc_poll_set_rate(3)` while the dashboard is visible, `0` while a session is open.

- [ ] **Step 1: Add the `dashboard` preview case**

It uses `agents_synthetic.json` and writes `/tmp/claude-dashboard.ppm`. Run it as described in `firmware/tools/ui_preview/README.md` and look at it.
Expected: the layout and colors match the spec's D9 table and the mockup; the only amber pills are the two `NEEDS YOU` ones.

- [ ] **Step 2: Implement the dashboard, the terminal command mode and the settings section**

Build and flash.
Expected: `Project build complete`

- [ ] **Step 3: Set up a test session**

On the Mac: `~/.local/bin/claude --bg --name companion-test-input "Ask me what my favorite color is, then wait for my answer."`

- [ ] **Step 4: Check the live dashboard**

Take a screenshot.
Expected: `companion-test-input` is first, with an amber pill. The battery %, link dot and clock show.

- [ ] **Step 5: Check a quick answer**

Select the test card, type `purple` and press Enter.
Expected: `Sent`, and within one poll the card leaves `NEEDS YOU`. Take a screenshot.

- [ ] **Step 6: Check take-over**

Press Enter on the card.
Expected: the terminal shows the Claude Code session. Type a line, press `Ctrl+Q`, and you're back on the dashboard with the toast. Take screenshots at each stage.

- [ ] **Step 7: Check the edge cases**

- An interactive session shows the `/bg` hint and no actions.
- With the Mac host set to `nonexistent-host`, the red unreachable message appears after 30 s. Restore the host afterwards.
- Stop the test session on the Mac, then try opening its card before the next poll → you're back on the dashboard with the stderr line.

- [ ] **Step 8: Clean up and commit**

Run `~/.local/bin/claude stop companion-test-input`.

```bash
git add firmware/main/ui firmware/main/ssh firmware/main/input firmware/tools/ui_preview
git commit -m "feat(ui): session dashboard with quick answers and take-over"
```
