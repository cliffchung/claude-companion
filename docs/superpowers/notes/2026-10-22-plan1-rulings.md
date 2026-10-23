# Plan 1 (session control): rulings and follow-ups

Decisions made while executing `docs/superpowers/plans/2026-10-22-session-control.md`, each with what it costs if wrong. Overrule any of them in review.

## Rulings

- Ruling R1: one worktree/branch (plan1-session-control) for the whole plan instead of one per task, merged at the end — tasks are strictly sequential and share one Tab5, so per-task merges add nothing; keeps main untouched — costs only coarser merge granularity if wrong.
- Ruling R2: Task 6 implementer adds minimal `wifi leave` / `wifi join` console commands wrapping the fork's existing Wi-Fi connect/disconnect (join uses saved credentials) — Step 6 needs them and nothing provides them — costs a small extra surface if wrong.
- Ruling R3: run Tasks 1–4 without the user, then ask everything for Task 5 in one batch (user's CLAUDE.md: do independent work first, then batch questions) — costs idle time between Task 4 and the user's answer.
- Ruling R4: agents_captured.json keeps only the companion-test-* entries, with home dir replaced by /Users/me — avoids committing names/paths of the user's other sessions; real-shape coverage is preserved by those entries — costs fewer real examples if wrong.
- Ruling R5: Task 3 adds a debug console command `key <up|down|left|right|enter|esc|tab|text...>` that injects events into the fork's keyboard path, so agents can navigate the UI for screenshots without a human pressing keys — Task 3 Step 5 and all later screenshot checks need it and nothing provides it — costs a small debug-only surface if wrong.
- Ruling R6: Task 3 also deletes ui_webfm.c and ui_lang.c (the Web Files screen and language picker); UI and terminal fonts are left untouched (CJK glyph trimming isn't in the plan) — costs some flash size if wrong.
- Ruling R7: accept check_trim ime_ regex tightened to (^|[^[:alnum:]])ime_ — the brief's pattern matches time_t/uptime_s and could never pass — costs missing an ime_ preceded by alnum if wrong.
- Ruling R8: insert Task 4a "Wi-Fi bring-up fix" after Task 4 and before Task 5: CONFIG_ESP32P4_TAB5_C6_BOARD=y, drop dead RESET_ACTIVE_LOW lines, regenerate sdkconfig, pin esp_hosted explicitly; verify SDIO init + a Wi-Fi scan on device. No C6 firmware update. Fallback esp_hosted 2.12.x — Task 5 is impossible without Wi-Fi and nothing in the plan covers it — costs one extra task if wrong.
- Ruling R9: test_parse_real_capture asserts input=NEEDS_YOU and perm=DONE (what really happened) instead of both NEEDS_YOU — the plan's assumption didn't hold on this Mac — costs nothing real; the assertion matches observed data.
- Ruling R10: cc_cmd_reply emits `<claude> -p --resume <sid> -- '<text>'` if a throwaway companion-test-dash session proves claude accepts `--` as end-of-options; otherwise reject text starting with '-' — Review Focus 1 (text reaches Claude exactly) outranks the plan's exact string — costs a test string change if wrong.
- Ruling R11: quick answers use attach-paste over an SSH pty channel instead of `-p --resume` (which claude refuses while the session runs). Task 6 adds a pure `cc_paste_build(text,out,cap)` in cc_cmd.c (host-tested) that emits 4×0x15 + ESC[200~ + sanitized text + ESC[201~ and strips ESC and all C0 controls except \n from the text so it can't end the paste early; plus `cc_link_paste(id, text)` in cc_link. cc_cmd_reply stays for stopped sessions only — the spec's route doesn't work on 2.1.309 — costs a pty round-trip (~1.5–3 s) per answer.
- Ruling R12: no `waitingFor` field exists on 2.1.309, so permission prompts can't be told apart from input. Every NEEDS_YOU background card gets the answer field, with Ctrl+O to open the session; the logs preview shows what it's waiting for — costs possible misdirected text into a permission dialog if wrong (mitigated: no keystrokes outside a bracketed paste).
- Ruling R13: insert Task 4b "Device SSH key": generate an ed25519 key on first use, store it in NVS, add console `sshkey` (prints the public line only, never the private key), and make key auth use the device key when no key file exists — spec D7 says the Tab5 generates its own key; this is the smallest change that keeps secrets on the device and lets the Mac read the public key over USB — costs a small firmware task if wrong.
- Ruling R14: Task 5 Steps 3–5 (terminal rendering, keys, attach from two places, tput cols) move to right after Task 7 Step 2, run through ui_terminal_open_command (pty + exec `claude attach <id>`). An exec'd command runs a non-interactive zsh, which doesn't source .zshrc, so there's no tmux autostart, and that's the path the product actually uses. Step 4's -p --resume question is already answered (refused while running; see the R11 attach-paste route) — costs a later hardware check of rendering if wrong.
- Ruling R15: companion code never opens an interactive login shell on the Mac. Take-over and quick answers always use a pty exec channel with an explicit `claude attach <id>` command (non-interactive zsh → no .zshrc → no tmux), one session full-screen. Reviewers for Tasks 6–7 must check this — the user said so — costs nothing.
- Ruling R16: cc_link_paste checks the attached screen for permission-dialog markers before sending any byte; on a match it sends only Ctrl+Z and returns "needs approval — open the session" (pure matcher cc_screen_is_permission_prompt, host-tested). Sent to the Task 6 implementer mid-task. Docs wording fix (D5, notes open items, minors 1–5) is deferred to a docs fix after Task 6, because a docs fixer can't run concurrently in the same worktree — costs a stale D5 for an hour if wrong.
- Ruling R17: poll output buffer goes to PSRAM at 256 KB, with a distinct truncation error, and POLL_CAP is raised so no live session falls off — PSRAM is 32 MB, and dropping --all would lose the finished cards — costs 256 KB PSRAM.
- Ruling R18: interactive entries are kept with background=false and a display key (name/cwd), and are never actionable. The fixture gets one real interactive entry with every value anonymized (R4 privacy) — fixing it in Task 6 because Task 7 builds on the parser — costs a parser change now.
- Ruling R19: the guard becomes fail-closed: typing is allowed only when the bottom of the stripped screen is positively the empty `❯` input line; anything else → needs approval / open. The R16 matcher stays as a secondary signal — a wrong refusal costs one Open, a wrong allow can approve a tool — costs some false refusals if wrong.
- Ruling R20: typing into a WORKING session is allowed (the text queues as the next message without interrupting). That is the plan's `R` message field for other background states. Fail-closed refusals (a leftover draft, a queued message) show "Open the session" — costs an occasional extra Open if wrong.
- Ruling R21: accept last-output-line in the error toast instead of first stderr line — a pty merges stderr into stdout so "first stderr line" is not separable — cost if wrong: a less specific error message, one-line change.
- Ruling R22: defer finding 15 (split ui_dashboard.c) and finding 4 (stale-tunnel repair for the generic SSH terminal; documented as known issue) — not load-bearing for Plan 1 — cost if wrong: later refactor / one slow reconnect.
- Ruling R23: fix I1 by refusing an interactive shell to the companion server entry (match by host, not only index) with "Open sessions from the dashboard" — spec's R15 sentence is the authority — cost if wrong: user loses a generic shell to their Mac from the Tab5, easy to re-enable later behind a setting.
- Ruling R24: I1 is verified by code + host test only, never by opening the Mac entry on hardware — a broken guard would land in the user's tmux again — cost if wrong: a guard bug ships unseen; review covers it.
- Ruling R25: defer M4 (scrub SSID/IP screenshots) and plain-NVS device key to Plan 5 (before going public) — not secrets under the global constraint, repo is local-only — cost if wrong: must scrub history before publishing.

## Deferred minors (none load-bearing per the final review)

- Task 1: minor (deferred): build warns factory partition 0x80000 too small for app — pre-existing baseline condition, app boots from OTA slot
- Task 2: minor (deferred): screenshot.c:253 esp_log_level_set("*",prev) clears per-tag overrides
- Task 2: minor (deferred): screenshot.c:222 lv_draw_buf_destroy outside lvgl lock (safe with libc malloc)
- Task 2: minor (deferred): dump failures print no @@ERR line → screenshot.py waits full 20 s
- Task 2: minor (deferred): screenshot.py raw ImportError without pyserial; SerialException uncaught (run after export.sh)
- Task 2: minor (deferred): JPEG out buf w*h*3/4+4096 could overflow on pathological frames
- Task 3: minor (deferred): backup.c apply_json restores settings without the OTA-URL/old node-name sanitizing done in settings_init
- Task 3: minor (deferred): key_console `key text` partial post on full queue doesn't report count
- Task 3: minor (deferred): "Japan is UTC+9." leftover string (i18n.h:175)
- Task 3: minor (deferred): merge_flash.sh cp whitespace misaligned
- Task 3: minor (deferred): tools/ui_preview/preview.c and gen_fonts.sh still mention IME stubs/SKK path — Task 7 must make preview build
- Task 3: minor (deferred): existing devices keep old Tailscale node name in NVS
- Task 4: minor (deferred): __pycache__/*.pyc committed in firmware/tools — folded into Task 4a
- Task 4: minor (deferred): cc_cmd_reply doesn't clear out[0] on argument rejection
- Task 4: minor (deferred): zsh roundtrip silently skipped if /bin/zsh missing
- Task 4: minor (deferred): put() helper awkward len out-param; ESC ( B leaves stray B; unchecked setup return; unity_config.h no-op; brace style in ArrayForEach
- Task 4a: minor (deferred): esp_wifi_remote still "*" in idf_component.yml
- Task 4a: minor (deferred): Wi-Fi scan list empty until refresh pressed
- Task 5: minor (deferred): authorize_device_key.sh ecdsa-sha2-* glob accepts junk type (not exploitable; tighten to nistp256/384/521)
- Task 5: minor (deferred): blob check is char-class only, locale-dependent; HOME empty → /.ssh; test uses hard-coded scratch path and weak reject assertions
- Task 4b: minor (deferred): device_key_get explicit_bzero(buf,len) after INVALID_LENGTH could use an updated len > allocation (needs concurrent write)
- Task 6: minor (deferred): subscriber callback calling cc_link_paste re-enters refresh() and clobbers s_parse — document "exec only from callbacks"
- Task 6: minor (deferred): host-key pin keyed on resolved IP vs terminal's p.host; NVS read error treated as UNKNOWN → re-pin — the first half was a misreading: the terminal's `ssh_client` resolves MagicDNS before its check too, so both pin the same address (Plan 2 P76); the forget action clears both forms. The NVS read error half stays open.
- Task 6: minor (deferred): do_poll trivial wrapper; cc_console_init declared in cc_link.h; UTF-8 C1 controls pass cc_paste_build
- Task 6: minor (deferred): logs tail can start inside a CSI sequence → stray params survive strip (cosmetic)
- Task 6: minor (deferred, final-review triage): gate checks only the last `❯` line; a ❯-less dialog drawn after an empty input box, a selected ❯ followed by blanks, or a trailing bare ❯ would pass — harden by refusing when the tail after that line has options or an esc hint, or when ❯ ends the buffer
- Task 6: minor (deferred): every refusal says "needs approval"; use "session isn't at its input prompt" for non-dialog refusals
- Task 6: minor (deferred): no real permission-dialog capture exists; the positive case is synthetic — done in Plan 2 Task 9 (`screen_permission_dialog.bin`, `test_screen_real_permission_dialog`)
- Task 7: "No Wi-Fi" red line may flash at boot; the empty-command terminal is blank for one frame; the stale open-check drop path is verified by code review only.

## Follow-ups for Plan 2 and later

- On-device "forget the Mac host key" action (confirmed); nothing can clear a changed key today. — done in Plan 2 Task 9: Settings → Companion → Forget Mac host key, console `companion hostkey forget` (P76, P77).
- Capture a real permission dialog for the R19 gate tests (the positive sample is synthetic). — done in Plan 2 Task 9: a real 2.1.309 Bash dialog; the existing gate and matcher pass on it unchanged (P78).
- cc_link_failure() reads state without the lock; ssh_new() failure keeps an old failure reason. — done in Plan 2 Task 9: both read under the lock, and `connect_once` clears the reason when `ssh_new()` fails.
- MicroLink peer table is read lock-free across tasks; fine for a best-effort guard only.
- Check reconnect lag after Mac sleep/wake within the keypair lifetime (stale peer-up shortcut).
- Two brownout resets seen on USB power during Task 7.
- Split ui_dashboard.c (R22).
- Before the repo goes public (R25): scrub SSID/IP/username from notes/img screenshots and the spec Environment section; consider NVS encryption for the device key.
