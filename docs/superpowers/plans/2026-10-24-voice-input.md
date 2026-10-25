# Voice Input Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Dictate answers on the Sessions screen. Ctrl+Space (or the mic button) starts a recording from the Tab5 mic and a second press stops it. The audio goes over the existing SSH link to `whisper-cli` on the Host, and the transcript lands in that session's answer field for review. Enter sends it as today; nothing is ever sent on its own.

**Architecture:**
- Pure parts first, host-tested: the two Host command builders (`cc_cmd_transcribe`, `cc_cmd_voice_check`) and `voice/voice_rules.c` (WAV header, levels, silence gate, key toggle guard, transcript cleanup, draft merge, exit-code mapping).
- One new link request: an exec channel with a stdin payload (`cc_link_transcribe`), no pty, link-busy while it runs. The audio is the channel's stdin; it is never on disk on either side.
- `voice/mic.c` owns the ES7210 capture with its own RX-only I2S channel per recording, so nothing audio-related runs between recordings.
- `voice/voice.c` owns one recording at a time: a PSRAM buffer, a voice task, a parallel setup check, the upload and one result slot that Sessions polls.
- Sessions enables the reserved mic slot, adds the voice strip (dot, timer, meter, hint) and routes keys by voice state. Settings › Host gets the two paths and a Check row.

**Tech Stack:** ESP-IDF v5.5.6, `esp_codec_dev` 1.5 (from the Tab5 BSP 1.3.2: ES7210), LVGL 9.4, libssh 0.11, Unity host tests (cmake/ctest), the host UI preview, Python unittest. On the Host: Homebrew `whisper.cpp` 1.9.5 (`whisper-cli`), model `ggml-base.en.bin`.

**Spec:** docs/superpowers/specs/2026-10-24-voice-input-design.md. Its §13 index lists the decisions; §8 is the Host feasibility record the command is built on.

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
- **Rule 3 grows two secrets: the recorded audio and the transcript.** No sample, buffer dump or transcript text goes to `ESP_LOG*`, `printf`, the console, NVS, a file or a screenshot in git. Logs and console verbs print lengths, durations, dBFS levels and exit codes only. The PSRAM buffer is zeroed and freed on every path.
- **The Host commands are exactly spec §7.1 and §7.2.** They run on a non-pty exec channel. No temp file, fifo, `mktemp` or background process on the Host.
- **Nothing is installed on the Host by the build or the tests.** whisper.cpp and the model are already there. Device tests may run `say` and `whisper-cli` **locally** on the Host in a scratch directory; nothing is run on the Host over SSH by hand.
- **Never auto-send.** No code path calls `cc_link_paste` with transcript text; only Enter (today's path) sends.
- Test only against Host sessions you create, named `companion-test-*`. Start one with `~/.local/bin/claude --bg --name companion-test-<x> "Ask me what my favorite color is, then wait for my answer."` and stop it when the task is done (`~/.local/bin/claude stop <id>`).
- Leave the Tailscale switch as you found it (off by default). A task that runs `ts on` for a gate runs `ts off` before it ends.
- Never change sshd config or `~/.ssh/authorized_keys`.

**Project facts:**
- **Base:** main at 9cb7849 (Plan 4 merged, plus "Ctrl+← leaves a take-over"). Work in the worktree `/Users/you/workspace/claude-companion-plan5` on branch `plan5-voice`; the top-level checkout stays on main.
- **Settings blob:** `app_settings_t` is one NVS blob. New fields go at the end. **This plan takes CFG 24**; CFG 22 stays reserved for Plan 2's Task 10 (which would take 25 if it lands later). Steps are keyed `old_version < N`.
- **Internal RAM:** LVGL allocations of 512 B or less land in internal RAM; use the shared static styles from `ui_comp`. The voice strip is built once with the pane, hidden. I2S DMA buffers and task stacks are internal: they exist only while recording.
- **Timers (Plan 4 B1):** no presentation timer under 300 ms, except the key drain, the pty pump and (ruling P5-4) the voice meter's 100 ms timer, which exists only while recording.
- **Keyboard:** the A164 has no key-up; a held key repeats after 500 ms, then every 60 ms (`keyboard.c`).
- **Strings:** every user-visible string is a `tr(STR_*)` entry in `firmware/main/i18n/i18n.h`, copied exactly from spec §6.4. Say "Host", never "Mac".
- **Defaults:** `CC_WHISPER_DEFAULT "/opt/homebrew/bin/whisper-cli"`, `CC_WHISPER_MODEL_DEFAULT "~/.local/share/whisper/ggml-base.en.bin"`, in `cc_cmd.h` only.

**Commands** (`PV` and `SHOTS` are directories in your scratchpad):
- **Firmware:** `. ~/esp/esp-idf/export.sh && cd firmware && idf.py build`. After editing `sdkconfig.defaults`, delete `firmware/sdkconfig` before building. Size: `idf.py size > $SHOTS/size-<task>.txt`.
- **Flash (P4 only):** `idf.py -p /dev/cu.usbmodem211201 flash`; if that port is gone, use `ls /dev/cu.usbmodem*`. It keeps NVS.
- **Host tests (HOST):** `export IDF_PATH=~/esp/esp-idf && cmake -S firmware/test/host -B firmware/test/host/build && cmake --build firmware/test/host/build && ctest --test-dir firmware/test/host/build --output-on-failure`.
- **Python tests (PY):** `python3 -m unittest discover -s firmware/tools -p 'test_*.py' -v` (build HOST first: `test_transcribe_cmd.py` uses `cc_cmd_print`).
- **Lint:** `python3 firmware/tools/ui_lint.py firmware/main` prints `[]`.
- **Preview (PV):** `cmake -S firmware/tools/ui_preview -B $PV -G Ninja && cmake --build $PV -j 8`, then `cd $PV && ./preview <case> [variant]`; `firmware/tools/ui_preview/run_all.sh $PV` runs every case.
- **Console:** the serial helper `T5` from Plan 4 (`python3 $T5 "cmd" …`, a copy in your scratchpad). Screenshots: `python3 firmware/tools/screenshot.py --out $SHOTS/<name>.jpg`. Look at every screenshot you take.
- **RAM gate:** `ui heapmin start`, the action, `ui heapmin stop` (`heapmin: internal min <N> B, largest <L> B, now <F> B`).
- **Power:** `power log on`, the window, then `python3 firmware/tools/power_report.py --port /dev/cu.usbmodem211201 --log --include-charging` (as in Plan 4 Task 1).
- **Notes:** each task appends its measurements and rulings to `docs/superpowers/notes/2026-10-24-voice-input-notes.md` (Task 1 creates it). Use placeholders (`my-host.local`), never the real host name, addresses or session names.

## Review Focus

1. **The transcript lands in the wrong place.** The selection moves, a poll re-sorts the list, the session ends, or Sessions is left and rebuilt while recording or transcribing. Expected: the text goes only into the draft of the session key captured at start, or is dropped with "That session is gone…"; it is never sent. Tests: Task 2 `test_merge_*`; Task 7 preview `sessions voice-done` (resort between start and result) and `voice-gone`; Task 7 device step 6.
2. **Held, repeated or waking Ctrl+Space.** Holding the keys 3 s, double taps, a Ctrl+Space that wakes a dim screen. Expected: one start per hold, a press after release stops, a waking key starts nothing. Tests: Task 2 `test_toggle_*`; Task 7 device step 4.
3. **Audio or transcript leaks.** A log line with text, a buffer not zeroed on cancel, error, timeout, park or leaving Sessions; a file or process left on the Host. Expected: none. Tests: Task 1 `test_no_files_left` and `test_no_process_left`; Task 6 code review checklist (every exit of `voice_task` passes `wipe_buffer()`); Task 9 privacy gate.
4. **Command safety.** A whisper or model path with a space, `'`, `;`, `$`, a backtick or a newline; stdin on a pty; a pre-check failing while the Tab5 is still uploading. Expected: refused by `cc_path_valid`; a plain exec channel; exit 120/121 still reaches the Tab5. Tests: Task 1 `test_cmd_transcribe_rejects`, `test_precheck_drains_stdin`.
5. **Nothing left running on the Tab5.** After stop, cancel, an error, leaving Sessions or a link park mid-upload: the mic closed, the I2S channel deleted, both voice tasks gone, the 100 ms timer deleted, the power hold off, PSRAM back. Tests: Task 6 `voice status` after each path; Task 9 timers, heap and power gates.
6. **Link interplay.** A transcription while the watch runs, behind a queued paste, or when Radio off / a check-in parks the link mid-upload. Expected: it queues, it gives up on a park with "Lost the connection to the Host", and the watch resumes. Tests: Task 3 device step 4; Task 9 step 5.

## Plan rulings (gaps the spec leaves open)

- **P5-1, one buffer.** The WAV is built in one PSRAM buffer: a 44-byte header slot, then the samples. The header is written at stop, when the length is known, and the whole buffer is the stdin payload.
- **P5-2, the stdin exec.** `cc_link_transcribe` is the only public way to run an exec with stdin. It builds its command itself from the settings (`cc_cmd_transcribe`), so no caller can pass a command line. Internally it is a `REQ_EXEC` with a payload (the existing `payload`/`payload_len` fields), never a pty.
- **P5-3, one result slot.** `voice.c` keeps one result with a generation counter. Sessions takes it on its 300 ms tick; a result not taken within 60 s is wiped.
- **P5-4, the meter timer.** A 100 ms LVGL timer exists only while recording; it is exempt from B1 like the pty pump, and Task 9 checks it is gone after every stop.
- **P5-5, Settings › Check.** It posts a job to the Sessions worker (as Forget host key does) through `ui_sessions_voice_check()`, which toasts the result with `ui_async`.
- **P5-6, the check runs in parallel.** A 3 KB helper task queues `cc_link_voice_check` at start; the voice task reads its exit code after each 100 ms block. A check still pending at stop is ignored (the transcribe command has the same pre-checks).
- **P5-7, defaults live in `cc_cmd.h`.** The lint rule `whisper-outside-cc_cmd` flags `whisper-cli` or `-otxt` in any string literal under `firmware/main` except `companion/cc_cmd.c`, `cc_cmd.h` and `i18n/i18n.h` (the placeholder).

---

### Task 1: The Host commands and the Host-side harness

**Files:**
- Modify: `firmware/main/companion/cc_cmd.h`, `cc_cmd.c`: the two builders, the exit codes, the defaults.
- Modify: `firmware/test/host/test_cc_cmd.c`: the new tests.
- Modify: `firmware/test/host/cc_cmd_print.c`: subcommands `transcribe <whisper> <model>` and `voicecheck <whisper> <model>`; the existing 5-argument watch form stays as it is.
- Create: `firmware/tools/test_transcribe_cmd.py`.
- Create: `docs/superpowers/notes/2026-10-24-voice-input-notes.md` (header plus "Task 1").

**Interfaces:**
- Produces (in `cc_cmd.h`):
```c
#define CC_WHISPER_DEFAULT        "/opt/homebrew/bin/whisper-cli"
#define CC_WHISPER_MODEL_DEFAULT  "~/.local/share/whisper/ggml-base.en.bin"
#define CC_VOICE_RC_NO_WHISPER 120   /* the whisper path isn't an executable file */
#define CC_VOICE_RC_NO_MODEL   121   /* the model path isn't a readable file */
/* spec §7.1: stdin is a 16 kHz mono 16-bit WAV, stdout the transcript. false for an invalid path or a small cap (out cleared). */
bool cc_cmd_transcribe(const char *whisper, const char *model, char *out, size_t cap);
/* spec §7.2: exit 0, 120 or 121; no output. */
bool cc_cmd_voice_check(const char *whisper, const char *model, char *out, size_t cap);
```
- The exact strings (C literals in `cc_cmd.c`, one `snprintf` each, `%s` for the two paths):
  - `exec /bin/sh -c 'W=%s; M=%s; [ -x "$W" ] || { cat >/dev/null; exit 120; }; [ -r "$M" ] || { cat >/dev/null; exit 121; }; exec "$W" -m "$M" -l en -nt -np -otxt -of - -f -'`
  - `exec /bin/sh -c 'W=%s; M=%s; [ -x "$W" ] || exit 120; [ -r "$M" ] || exit 121; exit 0'`

- [ ] **Step 1: Write the failing C tests** in `test_cc_cmd.c`:
  - `test_cmd_transcribe_exact`: with the two defaults, the output equals the first string above with the defaults substituted, byte for byte.
  - `test_cmd_voice_check_exact`: the same for the second string.
  - `test_cmd_transcribe_rejects`: false, and `out[0] == 0`, for a whisper or model path of `NULL`, `""`, 64 characters, and each of `"/a b"`, `"/a'b"`, `"/a;b"`, `"/a$b"`, `` "/a`b" ``, `"/a\nb"`, `"/a\"b"`, `"/a|b"`, `"/a&b"`; and for `cap` one byte short of the exact length.
  - `test_cmd_voice_check_rejects`: the same cases.
  - `test_cmd_transcribe_no_quote_inside`: the output has exactly two `'` characters (the outer pair), whatever valid paths are given.
- [ ] **Step 2: Run HOST and expect it to fail** (the builders don't exist).
- [ ] **Step 3: Implement the builders and `cc_cmd_print` subcommands.** Run HOST: pass.
- [ ] **Step 4: Write `test_transcribe_cmd.py`.** It gets the command from `cc_cmd_print transcribe|voicecheck <w> <m>` and runs it as `subprocess.run(['/bin/zsh', '-c', cmd], input=..., env={'HOME': tmp_home, 'PATH': '/usr/bin:/bin'}, timeout=30)`. The fake `whisper-cli` is a `/bin/sh` script in a temp dir (`tempfile.mkdtemp(prefix='ccv-', dir='/tmp')`, real path, ≤ 63 characters) that writes its argv to `args.txt` beside itself and its stdin byte count to `n.txt`, then prints `" Hello there.\n"`. Tests:
  - `test_fake_args_exact`: `args.txt` is exactly `-m <model> -l en -nt -np -otxt -of - -f -`, one per line.
  - `test_stdin_passes_through`: 100,044 bytes in, `n.txt` says 100044; stdout is `" Hello there.\n"`; exit 0.
  - `test_tilde_model_path`: the model given as `~/m.bin` with `m.bin` created under the temp `HOME` is found (exit 0), proving the `~` expands.
  - `test_missing_whisper_120` / `test_missing_model_121`: exit 120 / 121 with 960,044 bytes on stdin and no `BrokenPipeError` (`test_precheck_drains_stdin`: the write of all bytes completes).
  - `test_voicecheck_codes`: 0, 120, 121 for the three cases, no stdout.
  - `test_no_files_left`: after each run, the set of files under the temp `HOME` and the temp dir (excluding `args.txt` and `n.txt`) is unchanged.
  - `test_no_process_left`: `pgrep -f <tempdir>` is empty after each run.
  - `test_real_whisper` (skipped unless `CC_WHISPER_DEFAULT` is executable and the expanded model path is readable): `say -o <tmp>/t.wav --data-format=LEI16@16000 "Please run the tests and then commit the change."`, then the real command with the real `HOME`; stdout lower-cased contains `run the tests` and `commit`; exit 0; the scratch dir holds only `t.wav` afterwards; the test deletes `t.wav`.
- [ ] **Step 5: Run PY.** Expected: all pass (the real test runs on this Host). Record the real test's wall time in the notes.
- [ ] **Step 6: Commit.** Message: `feat(voice): cc_cmd_transcribe and cc_cmd_voice_check with the Host-side harness (Plan 5 Task 1)`.

### Task 2: `voice_rules`, the pure voice logic

**Files:**
- Create: `firmware/main/voice/voice_rules.h`, `voice_rules.c` (no ESP-IDF or LVGL headers; `i18n/i18n.h` only).
- Create: `firmware/test/host/test_voice_rules.c`.
- Modify: `firmware/test/host/CMakeLists.txt`: add `voice_rules.c` to `pure` and the test executable.

**Interfaces:**
- Produces:
```c
#define VR_RATE          16000
#define VR_MAX_S         30
#define VR_MIN_MS        500
#define VR_BLOCK_MS      100
#define VR_WAV_HDR       44
#define VR_PCM_MAX       (VR_RATE * 2 * VR_MAX_S)          /* 960,000 B */
#define VR_REPEAT_GAP_MS 150
#define VR_TOGGLE_GUARD_MS 700
#define VR_SILENCE_DBFS  (-50)    /* Task 5 may retune it (O2) */
#define VR_METER_FLOOR_DBFS (-60)
#define VR_KEY_MAX       128      /* == SM_KEY_MAX */
#define VR_TEXT_MAX      480      /* == SM_ANSWER_MAX */
/* 44-byte PCM WAV header; 0 for rate 0, channels 0 or bits not 16. */
size_t vr_wav_header(uint8_t out[VR_WAV_HDR], uint32_t data_bytes, uint32_t rate, uint16_t channels, uint16_t bits);
/* Interleaved -> mono. pick < 0: the average of all channels (int32 sum, saturating); else that channel. Returns frames. */
size_t vr_downmix(const int16_t *in, size_t frames, int channels, int pick, int16_t *out);
int vr_rms_dbfs(const int16_t *pcm, size_t n);   /* rounded; -96 for n == 0 or all zero */
int vr_meter(int dbfs);                          /* VR_METER_FLOOR_DBFS..0 -> 0..100, clamped */
bool vr_heard(int loudest_block_dbfs);           /* loudest >= VR_SILENCE_DBFS */
typedef enum { VR_KEY_START, VR_KEY_STOP, VR_KEY_IGNORE } vr_key_t;
typedef struct { int64_t last_ev_ms, last_toggle_ms; bool seen; } vr_keys_t;   /* zero-init */
vr_key_t vr_on_voice_key(vr_keys_t *k, bool recording, int64_t now_ms);
size_t vr_clean_transcript(char *s);             /* in place, spec §9.3; returns the new length */
typedef enum { VR_MERGE_OK, VR_MERGE_CUT, VR_MERGE_EMPTY } vr_merge_t;
/* out = draft + (space) + text, at most max_bytes bytes plus NUL, cut at the last space (or UTF-8 boundary) that fits. */
vr_merge_t vr_merge_draft(const char *draft, const char *text, char *out, size_t max_bytes);
typedef enum { VR_OUT_TEXT, VR_OUT_STARTED, VR_OUT_BUSY, VR_OUT_NO_FIELD, VR_OUT_NO_LINK, VR_OUT_NO_MEMORY,
               VR_OUT_MIC_FAILED, VR_OUT_TOO_SHORT, VR_OUT_NO_SPEECH, VR_OUT_NO_WHISPER, VR_OUT_NO_MODEL,
               VR_OUT_BAD_MODEL, VR_OUT_CANT_RUN, VR_OUT_AUDIO_BAD, VR_OUT_FAILED, VR_OUT_TIMEOUT,
               VR_OUT_LINK_LOST, VR_OUT_CANCELLED } vr_out_t;
vr_out_t vr_host_result(int exit_status);        /* spec §7.1 table; 0 -> VR_OUT_TEXT */
str_id_t vr_out_text(vr_out_t o);                /* the STR_ id; STR_COUNT for TEXT, STARTED, CANCELLED */
```
- `vr_out_text` maps: BUSY `VOICE_BUSY`, NO_FIELD `VOICE_NEEDS_FIELD`, NO_LINK `SES_NOT_CONNECTED` (exists), NO_MEMORY `VOICE_NO_MEMORY`, MIC_FAILED `VOICE_MIC_FAILED`, TOO_SHORT `VOICE_TOO_SHORT`, NO_SPEECH `VOICE_NO_SPEECH`, NO_WHISPER `VOICE_NO_WHISPER`, NO_MODEL `VOICE_NO_MODEL`, BAD_MODEL `VOICE_BAD_MODEL`, CANT_RUN `VOICE_CANT_RUN`, AUDIO_BAD `VOICE_AUDIO_BAD`, FAILED `VOICE_FAILED`, TIMEOUT `VOICE_TIMEOUT`, LINK_LOST `VOICE_LINK_LOST`. Those `STR_VOICE_*` entries (copy from spec §6.4) are added to `i18n.h` in this task. The UI-only ones (`VOICE_RECORDING`, `VOICE_HINT`, `VOICE_TRANSCRIBING`, `VOICE_REVIEW`, `VOICE_CUT`, `VOICE_AT_LIMIT`, `VOICE_SESSION_GONE`) come in Task 7 with their first use, so the lint's `unused-str` stays clean.

- [ ] **Step 1: Write the failing tests** in `test_voice_rules.c`:
  - `test_wav_header_exact`: `vr_wav_header(h, 32000, 16000, 1, 16)` returns 44 and `h` equals `"RIFF" 24 7D 00 00 "WAVE" "fmt " 10 00 00 00 01 00 01 00 80 3E 00 00 00 7D 00 00 02 00 10 00 "data" 00 7D 00 00`.
  - `test_wav_header_rejects`: rate 0, channels 0 and bits 8 return 0.
  - `test_downmix_average_pick_saturate`: `{100,300,-100,-300}` stereo → average `{200,-200}`, pick 0 `{100,-100}`, pick 1 `{300,-300}`; `{32767,32767}` → `32767`; `{-32768,-32768}` → `-32768`.
  - `test_rms_dbfs`: zeros → -96; a ±32767 square → 0; a ±3277 square → -20; n == 0 → -96.
  - `test_meter_mapping`: -60 → 0, -30 → 50, 0 → 100, -90 → 0, 5 → 100.
  - `test_heard_threshold`: -50 true, -51 false.
  - `test_toggle_press_then_tap_stops`: START at 0; IGNORE at 400; STOP at 800.
  - `test_toggle_held_key_starts_once`: START at 0, then a repeat chain at 500, 560, 620 … 3000 all IGNORE; a press at 3400 → STOP.
  - `test_toggle_hold_after_stop_does_not_restart`: STOP at 3400, then repeats at 3900, 3960 … 5000 all IGNORE; a press at 5500 → START.
  - `test_clean_leading_space`: `" Please run the tests.\n"` → `"Please run the tests."`.
  - `test_clean_tags`: `"[BLANK_AUDIO]\n"`, `"(blender whirring)\n"`, `" [MUSIC] "`, `"*coughs*"`, `" .\n"` → `""`; `"Hello [MUSIC] world."` → `"Hello world."`.
  - `test_clean_multiline_and_controls`: `"First.\nSecond.\n"` → `"First. Second."`; `"a\x1b\x7f\x01b\tc"` → `"ab c"`.
  - `test_clean_keeps_utf8_and_unclosed`: `"caf\xc3\xa9 [unclosed"` is unchanged except trimming.
  - `test_merge_spacing`: `("", "Hi")` → `"Hi"`; `("abc", "Def")` → `"abc Def"`; `("abc ", "Def")` → `"abc Def"`; `("abc", "")` → EMPTY with out `"abc"`.
  - `test_merge_cut_at_word`: a 470-byte draft plus `"one two three"` with max 480 → CUT, out is the draft plus `" one two"` (478 bytes): `" one two three"` would be 484.
  - `test_merge_cut_utf8_boundary`: a text of one 30-byte word of 3-byte characters that doesn't fit is cut on a character boundary (no partial sequence).
  - `test_host_result_map`: 0 TEXT; 120 NO_WHISPER; 121 NO_MODEL; 3 BAD_MODEL; 11 AUDIO_BAD; 126 and 127 CANT_RUN; 1, 2 and -1 FAILED.
  - `test_out_text_ids`: every `vr_out_t` except TEXT, STARTED and CANCELLED maps to a non-empty `tr()` string, and none of those strings contains "Mac".
- [ ] **Step 2: Run HOST and expect it to fail.**
- [ ] **Step 3: Implement `voice_rules.c` and the strings.** Run HOST: pass. Run the lint: `[]`.
- [ ] **Step 4: Commit.** Message: `feat(voice): voice_rules: WAV header, levels, toggle guard, transcript cleanup, draft merge (Plan 5 Task 2)`.

### Task 3: Settings fields, CFG 24 and the link's stdin exec

**Files:**
- Modify: `firmware/main/settings/settings.h`: append `char whisper_path[64];` and `char whisper_model[64];` (comment "v24, Plan 5: must pass cc_path_valid").
- Modify: `firmware/main/settings/settings_migrate.h`, `.c`: `CFG_VERSION 24`; the pure step `old_version < 24` sets both defaults; `settings_reset_scope` leaves both alone (comment it).
- Modify: `firmware/main/settings/settings.c`: `settings_defaults` sets both; `settings_init` NUL-terminates both and falls back to the default when `!cc_path_valid`.
- Modify: `firmware/main/companion/cc_link.h`, `cc_link.c`: the getters, setters and the two requests below; the payload write in `do_exec`.
- Modify: `firmware/main/companion/cc_console.c`: `companion voice-check` (prints `voice-check: rc <n>` or the esp_err name); `companion voice-path <path>` and `companion voice-model <path>` (call the setters; print `ok` or the path copy); `companion voice-upload-test <kb>` (Step 4).
- Modify: `firmware/tools/ui_lint.py`, `test_ui_lint.py`: the rule `whisper-outside-cc_cmd` (P5-7).
- Test: `firmware/test/host/test_settings_migrate.c`, `firmware/tools/test_ui_lint.py`.

**Interfaces:**
- Produces (in `cc_link.h`):
```c
const char *cc_settings_whisper_path(char *out, size_t cap);    /* default CC_WHISPER_DEFAULT */
const char *cc_settings_whisper_model(char *out, size_t cap);   /* default CC_WHISPER_MODEL_DEFAULT */
bool cc_link_set_whisper_path(const char *path);                /* false unless cc_path_valid; saves */
bool cc_link_set_whisper_model(const char *path);
/* cc_cmd_voice_check on an exec channel (10 s). ESP_OK with *exit_status 0/120/121/…; ESP_ERR_INVALID_STATE when not
   connected or parked; ESP_ERR_TIMEOUT. Blocks: never from the LVGL task. Counts as link-busy. */
esp_err_t cc_link_voice_check(int *exit_status);
/* cc_cmd_transcribe on a non-pty exec channel: writes wav (len bytes) to stdin in 16 KB chunks, draining stdout and
   stderr between chunks, sends EOF, then reads stdout into out (NUL-terminated) and the exit status. *upload_ms
   (may be NULL) is the time from the exec request to the EOF. ESP_OK; ESP_ERR_INVALID_STATE (not connected, or a
   park mid-request); ESP_FAIL (a channel or write error: the session is dropped); ESP_ERR_TIMEOUT (timeout_ms
   from the request's start; the session is dropped, as do_exec does); ESP_ERR_INVALID_SIZE (stdout didn't fit).
   Counts as link-busy (a power hold) while it runs. Logs only lengths, durations and the exit status. */
esp_err_t cc_link_transcribe(const void *wav, size_t len, char *out, size_t cap, int *exit_status,
                             int *upload_ms, int timeout_ms);
#define CC_TRANSCRIBE_TIMEOUT_MS 60000
```
- `do_exec` gains the payload write between `ssh_channel_request_exec` and the read loop when `r->payload_len > 0`. It checks the deadline and `s_parked` between chunks. A short `ssh_channel_write` after the remote has exited is not an error if the exit status then arrives (the pre-checks drain stdin, so it shouldn't happen).

- [ ] **Step 1: Write the failing tests.**
  - `test_migrate_v23_to_v24_voice_paths`: a v23 blob (its tail padding filled with 0xAA over the new fields' offsets) loads with both defaults and returns 23.
  - `test_v24_blob_keeps_voice_paths`: a v24 blob with `whisper_path` `/usr/local/bin/whisper-cli` keeps it.
  - `test_reset_scope_keeps_voice_paths`: `settings_reset_scope` leaves edited paths alone.
  - `test_newer_blob_takes_newer_path` is updated: a v25 blob copies what fits and returns 25.
  - Lint: `test_whisper_literal_flagged` (`"/x/whisper-cli"` in `ui/ui_sessions.c` is flagged; the same in `companion/cc_cmd.h` and `i18n/i18n.h` is not) and `test_otxt_literal_flagged`.
  - Run HOST and PY; expect failures.
- [ ] **Step 2: Implement** the settings, the migration, the lint rule and the link code. Run HOST, PY, the lint (`[]`) and the FW build.
- [ ] **Step 3: Device step: the check.** Flash. The first boot logs `settings v23 -> v24`; the second logs no migration. `companion voice-check` prints `rc 0`. `companion voice-model ~/nope.bin`, then `companion voice-check` prints `rc 121`; `companion voice-path /nope` gives `rc 120`; `companion voice-path "/a b"` prints the path copy and changes nothing. Restore both defaults.
- [ ] **Step 4: Device step: a stdin upload.** `companion voice-upload-test <kb>` builds a WAV of `<kb>` KB of silence in PSRAM, calls `cc_link_transcribe`, zeroes and frees it, and prints `upload <ms> ms, total <ms> ms, rc <n>, <len> chars` (never the text). Run it for 32, 320 and 937 KB (30 s) with the watch on. Expected: rc 0, an empty or one-word result, and `companion status` still connected with the watch running after each. Record the upload times on the LAN in the notes (O3).
  - Queued behind a paste: send `reply <test-id> hello` and the 937 KB upload on two console lines back to back; both complete.
  - A park mid-upload: start the 937 KB upload and, within 0.5 s, `power go radio-off` (which parks the link): the upload returns `ESP_ERR_INVALID_STATE` within 1 s. `power go active`; the link reconnects and the watch resumes (`companion status`).
  - Locally on the Host: `pgrep -fl whisper-cli` finds nothing.
- [ ] **Step 5: Commit.** Message: `feat(voice): whisper settings (CFG 24), cc_link_voice_check and cc_link_transcribe over stdin (Plan 5 Task 3)`.

### Task 4: Settings › Host: Whisper path, model and Check

**Files:**
- Modify: `firmware/main/ui/ui_settings.c`: three rows after Claude path (spec §6.5), their prompts, and `voice_check_cb`.
- Modify: `firmware/main/ui/ui_sessions.c`, `ui_sessions.h`: `JOB_VOICE_CHECK` and `bool ui_sessions_voice_check(void);` (false when a job is already pending).
- Modify: `firmware/main/i18n/i18n.h`: `SET_WHISPER_PATH`, `SET_WHISPER_PATH_PH`, `SET_WHISPER_MODEL`, `SET_WHISPER_MODEL_PH`, `SET_VOICE`, `SET_VOICE_CHECK`, `SET_VOICE_READY`, and the changed `SET_HOST_NOTE` (spec §6.4).
- Modify: `firmware/tools/ui_preview/preview.c`: stubs for the new `cc_settings_*`, setters and `cc_link_voice_check`; the `settings 0` case asserts the three rows.

**Interfaces:**
- Consumes: Task 3's getters, setters and `cc_link_voice_check`; Task 2's `vr_host_result` and `vr_out_text`.
- The job: `cc_link_voice_check(&rc)` → `ESP_ERR_INVALID_STATE` toasts `SES_NOT_CONNECTED`, `ESP_ERR_TIMEOUT` toasts `VOICE_TIMEOUT`, rc 0 toasts `SET_VOICE_READY`, else `tr(vr_out_text(vr_host_result(rc)))`, all through `ui_async`.

- [ ] **Step 1: Preview first.** Extend `settings 0` to assert the rows "Whisper path on Host", "Whisper model on Host" and "Voice input" exist in that order after "Claude path on Host", the values are the defaults, and the fit check passes. Build PV and expect the assertion to fail.
- [ ] **Step 2: Implement.** PV `settings 0` passes; `run_all.sh` passes; the lint gives `[]`.
- [ ] **Step 3: Device step.** Settings › Host shows the three rows (screenshot to `$SHOTS/t4-host.jpg`, read it). Check toasts "Voice input is ready on the Host". Set the model to `~/nope.bin` in the prompt: Check toasts the `VOICE_NO_MODEL` copy naming README › Set up voice input; restore. A path with a space is refused with the path copy and not saved.
- [ ] **Step 4: Commit.** Message: `feat(settings): Whisper path, model and Check rows in Settings › Host (Plan 5 Task 4)`.

### Task 5: The mic driver and the capture spike

**Files:**
- Create: `firmware/main/voice/mic.h`, `mic.c`.
- Modify: `firmware/main/CMakeLists.txt`: add `voice/mic.c` (and the `voice` directory); `esp_driver_i2s` in PRIV_REQUIRES if the build needs it (the BSP's `esp_codec_dev` is already public).
- Create: `firmware/main/voice/voice_console.c`: the `voice` console command (Task 6 adds verbs).
- Modify: `firmware/main/app_main.c`: register the console command only (no audio init at boot).

**Interfaces:**
- Produces:
```c
#define MIC_GAIN_DB 30   /* Step 2 sets the measured value */
#define MIC_PICK    (-1) /* -1: average both slots; 0 or 1: that slot only (Step 2) */
typedef struct { int rate; int gain_db; } mic_cfg_t;           /* { VR_RATE, MIC_GAIN_DB } */
/* Creates an RX-only I2S channel on CONFIG_BSP_I2S_NUM (std Philips, 16-bit, stereo slots, BSP pins MCLK/BCLK/WS/DIN,
   dma_desc_num 4, dma_frame_num 400), an esp_codec_dev input device on it (audio_codec_new_i2s_data,
   audio_codec_new_i2c_ctrl at ES7210_CODEC_DEFAULT_ADDR on bsp_i2c_get_handle(), es7210_codec_new), opens it at
   cfg->rate with 2 channels and sets the gain. ESP_OK or the failing step's error, with everything undone. */
esp_err_t mic_open(const mic_cfg_t *cfg);
/* Blocking read of frames stereo frames (2 * frames int16) within timeout_ms; returns frames read, or -1. */
int mic_read(int16_t *buf, size_t frames, int timeout_ms);
/* esp_codec_dev_close + delete, then i2s_channel_disable + i2s_del_channel: MCLK stops. Safe when not open. */
void mic_close(void);
bool mic_is_open(void);
```
- Console: `voice rec <1-10 s> [gain_db]`: opens the mic, reads 100 ms blocks into one reused PSRAM block buffer, prints `rec t=<s> L=<dBFS> R=<dBFS> avg=<dBFS>` every 500 ms (each the loudest block's RMS in that half second), closes the mic, zeroes the block buffer and prints `mic: closed`. Nothing is kept.

- [ ] **Step 1: Implement `mic.c` and `voice rec`.** FW build.
- [ ] **Step 2: Device spike, O1 and O2.** Flash. In a quiet room:
  - `voice rec 5` silent: record L/R/avg (the noise floor).
  - `voice rec 5` while running `say "The quick brown fox jumps over the lazy dog. Please run the tests and then commit the change."` locally on the Host with the Tab5 50 cm from its speaker: record the levels.
  - Repeat at gain 20, 30 and 40 dB. Choose the gain that puts speech peaks near −12 dBFS without clipping (no block at 0 dBFS), and whether to average or pick one channel (pick the one with the better speech-to-floor gap if they differ by more than 6 dB). Set `MIC_GAIN_DB` and `MIC_PICK` in `mic.h` and `VR_SILENCE_DBFS` in `voice_rules.h` to midway between the floor and speech, rounded to 5 dB (keep −50 if it already sits there). If `VR_SILENCE_DBFS` changes, update `test_heard_threshold` to the new value.
  - If `mic_open` can't configure the ES7210 on its own channel, fall back to `bsp_audio_codec_microphone_init()` and record the idle-power cost of the always-on channels in the notes; that is a ruling for the controller, not a silent switch.
- [ ] **Step 3: Device spike, O4 and RAM.**
  - `ui heapmin start`, `voice rec 10`, `ui heapmin stop`: record the internal minimum and the delta against the idle `ts` value (expected: about the DMA buffers, ≤ 8 KB).
  - Power: 2 minutes idle on Sessions with the screen on (`power log on` and the report), then `voice rec 10` three times, then 2 more minutes idle. The second idle window's mean is within 0.02 W of the first; record the recording's own draw.
- [ ] **Step 4: Commit.** Message: `feat(voice): ES7210 mic driver with a per-recording I2S channel; voice rec spike (Plan 5 Task 5)`. The notes record the gain, the channel choice, the threshold, the floor, the RAM and the power figures.

### Task 6: The voice service

**Files:**
- Create: `firmware/main/voice/voice.h`, `voice.c`.
- Modify: `firmware/main/voice/voice_console.c`: `voice status`, `voice test <1-30 s>`.
- Modify: `firmware/main/sys/power.h`, `power.c`: `POWER_HOLD_VOICE` (counted like `POWER_HOLD_TAKEOVER`; `power_holds_text` prints `voice`).
- Modify: `firmware/main/CMakeLists.txt`: `voice/voice.c`, `voice/voice_rules.c`.

**Interfaces:**
- Produces:
```c
typedef enum { VOICE_IDLE, VOICE_STARTING, VOICE_RECORDING, VOICE_TRANSCRIBING } voice_phase_t;
typedef struct {
    voice_phase_t phase;
    int elapsed_ms;              /* recorded so far */
    int meter;                   /* 0..100, the last block */
    bool at_limit;               /* stopped by the 30 s limit */
    int transcribing_ms;         /* time in TRANSCRIBING */
    char key[VR_KEY_MAX];        /* the session key it was started for */
} voice_status_t;
typedef struct { uint32_t gen; char key[VR_KEY_MAX]; vr_out_t out; bool at_limit; char text[VR_TEXT_MAX + 1]; } voice_result_t;
/* Any task. VR_OUT_STARTED, or VR_OUT_BUSY (not idle), VR_OUT_NO_LINK (cc_link_connected() false),
   VR_OUT_NO_MEMORY (the 960,044 B PSRAM buffer or a task didn't allocate). On STARTED: POWER_HOLD_VOICE on, the
   buffer allocated, the voice task created (4 KB), the check helper created (3 KB). */
vr_out_t voice_start(const char *key);
void voice_stop(void);       /* RECORDING/STARTING: stop and transcribe; otherwise nothing */
void voice_cancel(void);     /* RECORDING/STARTING: stop and wipe, no result; TRANSCRIBING: its result is dropped */
void voice_status(voice_status_t *out);
/* True once per result (cleared and zeroed after the copy); a result not taken within 60 s is zeroed. */
bool voice_take_result(voice_result_t *out);
```
- The voice task, in order: `mic_open` (fail → MIC_FAILED) → phase RECORDING → 100 ms blocks: `mic_read`, `vr_downmix(..., 2, MIC_PICK, ...)`, append, `vr_rms_dbfs` → meter and loudest; after each block read the check's code (non-zero → stop with `vr_host_result(code)`); stop on `voice_stop`, `voice_cancel` or `VR_MAX_S` → `mic_close()` and `POWER_HOLD_VOICE` off → cancel: wipe, no result; `< VR_MIN_MS`: wipe, TOO_SHORT; `!vr_heard(loudest)`: wipe, NO_SPEECH → else phase TRANSCRIBING, `vr_wav_header` into the slot, `cc_link_transcribe(..., CC_TRANSCRIBE_TIMEOUT_MS)`, **wipe** (memset 0, free) → map: `INVALID_STATE`/`FAIL` → LINK_LOST, `TIMEOUT` → TIMEOUT, rc ≠ 0 → `vr_host_result(rc)`, rc 0 → `vr_clean_transcript`, empty → NO_SPEECH, else TEXT → post the result unless cancelled → phase IDLE → the task deletes itself. One `wipe_buffer()` helper is the only free of the buffer, and every exit passes it.
- Log line, once per recording: `voice: rec <s> s, loudest <dBFS> dBFS, upload <s> s, host <s> s, rc <n>, <len> chars, <outcome>`.
- Console: `voice status` prints `voice: <phase>, mic <open|closed>, tasks <n>, hold <on|off>, result <none|pending>`. `voice test <1-30>` runs `voice_start("console")`, waits that many seconds (or for the 30 s limit), `voice_stop()`, waits for the result and prints `voice test: <outcome>, <len> chars` (never the text), then zeroes it. `voice test-short` stops after 300 ms; `voice test-cancel <s>` calls `voice_cancel()` after s seconds and prints `voice test: cancelled` once the task is gone.

- [ ] **Step 1: Implement.** FW build; HOST still passes (no pure change).
- [ ] **Step 2: Device step: the happy path.** `voice test 5` while `say "Please run the tests and then commit the change."` plays locally on the Host: `voice test: TEXT, <n> chars` with n between 40 and 60; the log line shows the timings. `voice status` afterwards: `idle, mic closed, tasks 0, hold off, result none`. Record the timings (rec, upload, host) in the notes.
- [ ] **Step 3: Device step: every other exit.** For each, `voice status` must end idle/closed/0/off:
  - `voice test 5` in silence → NO_SPEECH (nothing uploaded: the log shows no upload);
  - `voice test-short` (start, then stop after 300 ms) → TOO_SHORT;
  - `companion voice-model ~/nope.bin`, then `voice test 10` → the recording ends within about 1 s with NO_MODEL; restore;
  - `companion voice-path /nope` → NO_WHISPER; restore;
  - `voice test 30` with speech: stops at 30 s, `at_limit`, TEXT; record the upload and host times for 30 s (O3);
  - the link parked mid-upload (`power go radio-off` right after a `voice test 30` reaches 30 s) → LINK_LOST; `power go active`;
  - a cancel: a debug verb `voice test-cancel <s>` (start, wait, `voice_cancel()`) → no result, buffer freed (`voice status`, and PSRAM free from `ts` back within 4 KB of before).
- [ ] **Step 4: Device step: power hold.** During a `voice test 20`, run `power sim idle 300` (past the dim and off timeouts): `power` still shows `active` with holds `voice`, and during the upload `busy`. After the result, `power sim idle 300` again dims the screen as usual.
- [ ] **Step 5: Commit.** Message: `feat(voice): voice service: PSRAM recording, parallel check, transcription, one result slot, power hold (Plan 5 Task 6)`.

### Task 7: Sessions: the mic button, the voice strip and the keys

**Files:**
- Modify: `firmware/main/ui/ui_sessions.c`:
  - the mic slot becomes enabled (solid `line` border, `fg` glyph; red 2 px border and a red dot while recording; muted while transcribing) with `mic_cb` → `voice_toggle()`;
  - the voice strip in the action row (built once, hidden): dot, "Recording", the timer label, the `lv_bar` meter, the hint;
  - the 100 ms meter timer (P5-4), created at a successful start and deleted by itself when `voice_status` is no longer STARTING/RECORDING;
  - key routing by voice state (spec §5.2) at the top of `sessions_key`, before `hk_dashboard`;
  - the 300 ms tick: the transcribing note with seconds, `voice_take_result` → `apply_voice_result()`;
  - the screen delete path: `voice_cancel()` when STARTING/RECORDING;
  - `_Static_assert(VR_KEY_MAX == SM_KEY_MAX && VR_TEXT_MAX == SM_ANSWER_MAX, ...)`.
- Modify: `firmware/main/ui/ui_comp.c`, `.h` only if a shared static style is missing for the red ring or the meter (prefer adding the styles to `ui_sessions.c`'s existing static style set).
- Modify: `firmware/main/ui/hotkeys.h` comment (VOICE now acts); `firmware/main/i18n/i18n.h`: the remaining `VOICE_*` UI strings, `HK_W_CTRL_SPACE` changed, `HK_VOICE` deleted.
- Modify: `firmware/tools/ui_preview/preview.c`: a scriptable `voice_*` stub (phase, elapsed, meter, a queued result) and the variants below.

**Interfaces:**
- Consumes: Task 6 `voice_*`, Task 2 `vr_on_voice_key`, `vr_merge_draft`, `vr_out_text`.
- Internal: `static void voice_toggle(void)` (the one path for Ctrl+Space and the mic tap: `vr_on_voice_key` on a static `vr_keys_t`; START needs `field_visible(selected())`, else toast `VOICE_NEEDS_FIELD`; START calls `voice_start(s_sel_key)` and maps a non-STARTED outcome to a note; STOP calls `voice_stop()`); `static void apply_voice_result(const voice_result_t *r)` (finds the item by `r->key`; none → toast `VOICE_SESSION_GONE`; TEXT → `vr_merge_draft(sm_draft_get(s_drafts, key), r->text, buf, SM_ANSWER_MAX)` then `sm_draft_set`, note `VOICE_CUT` or `VOICE_REVIEW`, plus `VOICE_AT_LIMIT` first when `at_limit`; errors → a NOTE_ERROR on that session with `tr(vr_out_text(r->out))`; the result buffer is zeroed after).

- [ ] **Step 1: Preview cases first** (they fail until Step 2):
  - `sessions voice-rec`: the stub reports RECORDING at 7 s, meter 60. Asserts: the strip is visible with `0:07 / 0:30`, the mic button has a red border, the amber scan finds no amber, the fit check passes, `ui timers` in the preview lists one 100 ms timer. Keys: `key text x`, Enter and Down change nothing (draft unchanged, no paste call, selection unchanged); Esc calls `voice_cancel`; Ctrl+Space calls `voice_stop` (the stub's clock is past the 700 ms guard).
  - `sessions voice-rec-limit`: RECORDING at 27 s: the timer is red.
  - `sessions voice-busy`: TRANSCRIBING for 5 s: the note reads `Transcribing on the Host… 5 s`; no 100 ms timer remains.
  - `sessions voice-done`: a TEXT result for the first session's key while the second is selected and the list has been re-sorted: the first session's draft is `"<old> <text>"`, the second's is untouched, the note on the first reads the review copy; `cc_link_paste` was never called.
  - `sessions voice-gone`: a TEXT result for a key not in the list: the toast `VOICE_SESSION_GONE`, no draft changed.
  - `sessions voice-error`: a NO_MODEL result: the red note with the README copy, fit-checked (it wraps or dots inside the note slot, never overflows).
  - `sessions voice-nofield`: a stopped session selected, Ctrl+Space: the toast `VOICE_NEEDS_FIELD`, `voice_start` not called.
  - `ctrlk`: the overlay line reads "voice input: press to start, again to stop".
- [ ] **Step 2: Implement.** PV `run_all.sh` passes; HOST, PY and the lint (`[]`) pass; FW build.
- [ ] **Step 3: Device step: the core loop.** Start `companion-test-voice` (the favourite-colour prompt). Select it. `key ctrl space`, then run locally on the Host `say "My favorite color is blue."`, then `key ctrl space`. Within 3 s the field reads "My favorite color is blue." (screenshot `$SHOTS/t7-done.jpg`, read it). Nothing was sent (`claude logs <id>` locally shows no answer). `key enter` sends it; the session answers.
- [ ] **Step 4: Device step: keys.** Hold-to-start: `key ctrl space` posted 30 times 60 ms apart (a held key) starts once and doesn't stop (`voice status` stays recording); a single `key ctrl space` 1 s later stops it. `power go dim`, then `key ctrl space`: it only wakes (`voice status` idle, `power wakes` shows swallowed). Esc while recording: idle, no result, the draft unchanged.
- [ ] **Step 5: Device step: touch.** `power sim touch <x> <y>` on the mic button (its centre from a screenshot) starts; a second touch stops.
- [ ] **Step 6: Device step: the wrong-session guard.** With two `companion-test-*` sessions, start recording on the first, select the second by touch, stop: the text lands in the first session's draft (select it to see), the second's draft is empty.
- [ ] **Step 7: Device step: leaving.** Start recording, press the footer's Settings chip: `voice status` idle, mic closed, no result. Start, stop, and press Settings during TRANSCRIBING, then come back within 60 s: the text is in the draft.
- [ ] **Step 8: Commit.** Message: `feat(ui): voice input on Sessions: mic button, voice strip, Ctrl+Space toggle, review before send (Plan 5 Task 7)`.

### Task 8: README, firmware README and the main spec

**Files:**
- Modify: `README.md`:
  - §2 Set up the Host: a new `### Set up voice input (optional)` with: `brew install whisper.cpp`; `mkdir -p ~/.local/share/whisper` and `curl -L -o ~/.local/share/whisper/ggml-base.en.bin https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin`; `shasum ~/.local/share/whisper/ggml-base.en.bin` must print `137c40403d78fd54d454da0f9bd998f78703390c`; a one-line test (`say -o /tmp/t.wav --data-format=LEI16@16000 "hello from the Host" && /opt/homebrew/bin/whisper-cli -m ~/.local/share/whisper/ggml-base.en.bin -l en -nt -np -otxt -of - -f - < /tmp/t.wav; rm /tmp/t.wav`); Intel Macs use `/usr/local/bin/whisper-cli` and set the Whisper path; the first transcription after a Host restart takes several seconds (Metal warm-up).
  - §4 Daily use: a `### Voice input` subsection (the loop from spec §1, the 30 s limit, Esc cancels, never auto-sends, take-over not supported); the shortcuts table row for Ctrl+Space.
  - §5 Settings: the Host row lists the Whisper path, model and Check.
  - §10 Troubleshooting: "whisper-cli isn't on the Host…", "The whisper model isn't on the Host…", "No speech heard", "The Host took too long to transcribe".
  - §12 Security model: a bullet: the audio goes to `whisper-cli` on the channel's stdin and is never written to disk on either side; the command is fixed (`cc_cmd_transcribe`) with validated paths; no audio or transcript is logged; the mic is on only while recording.
  - Status and Roadmap: Plan 5 done; Plan 6 next.
- Modify: `firmware/README.md`: the layout (`voice/`), the Host link commands (the two new ones), and the mic notes (own I2S channel, per recording).
- Modify: `docs/superpowers/specs/2026-10-22-claude-tab5-design.md`: the roadmap row for Plan 5 links this plan and is marked done; the "How it works" command table gains the transcribe and check rows.

- [ ] **Step 1: Write the docs.** Every command in them is copied from the code or the spec, not retyped from memory. Run the README's one-line test locally on the Host (allowed: local, scratch file removed).
- [ ] **Step 2: Check** `grep -n "Mac\b" README.md` adds no new user-facing "Mac" for the Host (the Intel-Mac Homebrew note names the hardware, which is fine); no real host names, addresses or session names.
- [ ] **Step 3: Commit.** Message: `docs: voice input setup, use, troubleshooting and security model (Plan 5 Task 8)`.

### Task 9: Final device gates

**Files:**
- Modify: `docs/superpowers/notes/2026-10-24-voice-input-notes.md`: the final table.
- No product code changes. A failed gate gets a fix commit in the task that owns the code, and this task's step is re-run.

- [ ] **Step 1: Sizes.** `idf.py size`: record flash and static internal RAM against the Plan 4 final figures. Expected: flash up by tens of KB (the ES7210 driver and the voice code), static internal RAM up by under 1 KB.
- [ ] **Step 2: Heap gates.** Tailscale off, then on (`ts on`, then `ts off` at the end):
  - `ui heapmin` over a 30 s recording plus its transcription on Sessions: internal minimum ≥ 40,960 B with Tailscale off and ≥ 20,480 B with it on; no `ERR_BUF` in the console log; `companion status` connected afterwards.
  - PSRAM: `ts` before and 5 s after the result: free PSRAM within 4 KB.
- [ ] **Step 3: Transcription.** Three phrases played with `say` locally on the Host, the Tab5 50 cm from its speaker, through the real Sessions loop (`key ctrl space`, `say`, `key ctrl space`), each screenshotted to `$SHOTS/t9-phrase-<n>.jpg` and read:
  1. "Please run the tests and then commit the change." (must contain "run the tests" and "commit");
  2. "Yes, go ahead with option two." (must contain "option" and "two" or "2");
  3. "Rename the function to parse config and add a unit test for the empty file case." (must contain "rename" and "test").
  Record each end-to-end time (second Ctrl+Space to text shown) on the LAN, and phrase 1 again with Tailscale on. Expected on the LAN: ≤ 3 s for each warm.
- [ ] **Step 4: Errors.** With `companion voice-path /nope`: Ctrl+Space shows the `VOICE_NO_WHISPER` note within about 1 s of starting, screenshot. Then the model, the same. Restore both. Link down (`wifi leave`) before Ctrl+Space: "Not connected to the Host" and no recording; `wifi join`.
- [ ] **Step 5: Link interplay.** A recording stopped while a `reply` to another test session is in flight: both complete. A recording stopped and `power go radio-off` during its upload: LINK_LOST, then `power go active` and the watch resumes (`companion status`).
- [ ] **Step 6: Timers.** `ui timers` during a recording: one 100 ms timer more than idle Sessions; 1 s after stop, cancel and an error: back to the idle count.
- [ ] **Step 7: Power.** On USB: 5 minutes idle on Sessions before any recording, then five recordings, then 5 minutes idle: the second idle mean is ≤ the first + 0.02 W, and ≤ the Plan 4 final idle figure + 0.02 W. Record the recording's draw. `voice status` reads closed throughout the idle windows. A check-in boot (if Hibernate check-ins are on) logs no voice or mic line.
- [ ] **Step 8: Privacy.**
  - Capture the serial log over all of Steps 3–5 to `$SHOTS/t9-console.txt`; `grep -i -E "run the tests|option|rename|favorite|blue"` finds nothing.
  - On the Host, locally: `touch $SCRATCH/marker` before Step 3, and after it `find ~ /private/tmp /private/var/folders -newer $SCRATCH/marker -type f \( -name '*.wav' -o -name '*.txt' -o -name '*.raw' \) 2>/dev/null` finds nothing voice-related; `pgrep -fl whisper-cli` finds nothing.
  - The NVS settings blob holds no transcript (the settings fields are the only change: `companion settings dump` if it exists, else skip and note it).
- [ ] **Step 9: The user's visit.** Write the batched list in the notes: speech from the couch at a normal distance and voice (the real accuracy check, beyond `say`), the mic button and the strip at arm's length, whether the meter and the red dot read clearly, whether 30 s is long enough, and every PENDING item above.
- [ ] **Step 10: Stop all `companion-test-*` sessions, check `ts` is off, and commit.** Message: `docs(plan5): final measurements, gates and the visit list (Plan 5 Task 9)`.

---

Execution: subagent-driven development (superpowers:subagent-driven-development), one fresh implementer and reviewer per task.
