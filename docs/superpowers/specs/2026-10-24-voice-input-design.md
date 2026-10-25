# Plan 5: Voice input: Design

**Date:** 2026-10-24
**Status:** direction set by the user (whisper.cpp on the Host, chosen 2026-10-23; the transcriber,
model and flow below, 2026-10-24). Calls this spec makes that the inputs don't settle are marked
**Decision** with a reason and the cost if wrong, and are indexed in §13.
**Builds on:** Plan 4 (UI overhaul, on main at 9cb7849). The whole-project spec is
`docs/superpowers/specs/2026-10-22-claude-tab5-design.md` (R7, the roadmap row "5 Voice input").

---

## 1. Intent and why

The Tab5's keyboard is small. Typing an answer to a Claude Code session with two thumbs is the
slowest part of the couch loop, and the user named voice as the next priority (R7, 2026-10-23).
Claude Code's own `/voice` needs a microphone on the machine running Claude Code, so it does not
work over SSH, and the Claude API takes no audio. So the Tab5 records, and the **Host** transcribes
locally with whisper.cpp, which the user installed with Homebrew. No cloud service, no API key, and
no audio ever leaves the home network or the Host's RAM.

The loop this plan adds, on the Sessions screen:

1. Select a session with an answer field. Press **Ctrl+Space** (or tap the mic button).
2. Speak. A red dot, a timer and a level meter show that it is listening.
3. Press **Ctrl+Space** again. The Tab5 sends the recording over its existing SSH link, the Host
   runs `whisper-cli` on it, and the text appears in the answer field.
4. Read it, fix it if needed, and press **Enter** to send, exactly as if you had typed it.

It never sends on its own.

## 2. Goals, non-goals, anti-goals

### Goals
1. Dictate into the Sessions answer field with one key to start and the same key to stop.
2. Transcription on the Host only, with `whisper-cli` and the `base.en` model, through one fixed
   `cc_cmd_*` command over the existing link. Nothing is installed on the Host by the Tab5, and
   nothing is left running or stored there.
3. The text lands in the draft of the session you started recording for, for review. Enter sends.
4. Clear, specific errors: a missing `whisper-cli` or model names the missing piece and the README
   step, before you have spoken if possible.
5. Two new Settings › Host rows (the whisper path and the model path) and a **Check** action.
6. The mic draws power only while recording. Idle power and the check-in path are unchanged.

### Non-goals
- **Voice in a take-over (full-screen session).** *Reversed by user request on 2026-10-24: see §14.*
  The original decision, kept for the record: out of scope for Plan 5. Reasons:
  in a take-over every key belongs to Claude Code (Ctrl+Space is a real terminal key, NUL), so the
  hotkey would have to be stolen from the session; the text would have to be typed into a live pty,
  which needs its own fail-closed gate like the answer field's (D5); and a take-over is the
  tightest internal-RAM case (≥ 20 KB with Tailscale on, Plan 4 B2), where recording's DMA buffers
  and task stack would cut into the margin. The answer field covers the common case: answer, then
  Open if you need the full screen. Cost if wrong: a follow-up plan with its own hotkey and gate.
- **Voice in Settings prompts.** (Voice in a Terminal SSH session was a non-goal too; §14 adds it
  with the take-over.)
- **Streaming or live (partial) transcription.** One recording, one transcription.
- **Sound feedback** (beeps). The speaker stays off; the `volume` field stays unused.
- **Wake word, voice activity auto-stop, voice commands** ("send", "open"). Voice only produces text.
- **Languages other than English.** `-l en` with `base.en`. A multilingual model is a settings
  change later (§12).
- **Hold-to-talk.** See §5.1.

### Anti-goals (a failure even if it "works")
- Text sent to a session without the user pressing Enter.
- Any part of the user's speech, or the transcript, in a log, an NVS key, a file on the Host, a
  crash dump or a screenshot in git.
- Any command line not built by a `cc_cmd_*` function; any login shell; any process left running
  on the Host after the channel ends.
- Amber used for recording.
- Idle power higher than Plan 4's, or the mic, I2S clock or a voice task alive while not recording.
- A transcript landing in a different session's draft than the one recording was started for.

## 3. Constraints

### Hard project rules (unchanged; the ones this plan touches)
1. **Never open a login shell on the Host.** Voice uses one non-pty exec channel per command.
2. **Every command line sent to the Host comes from a `cc_cmd_*` function**, from validated parts
   only. The whisper path and model path pass `cc_path_valid` (`[A-Za-z0-9_./~-]`, 1–63 chars),
   like the Claude path. The audio is never part of a command line: it is the channel's stdin.
3. **No secrets in the repo or in logs.** This plan adds two: **the recorded audio** and **the
   transcript**, which is answer text. Logs and console output carry only lengths, levels in dBFS,
   durations and exit codes.
4. **No GPL code** in the firmware. whisper.cpp (MIT) runs on the Host only and is not vendored.
   The mic path uses the BSP's `esp_codec_dev` (Apache-2.0), already linked.
7. **A wake input never acts.** A Ctrl+Space that wakes the screen is swallowed like every waking
   key, so it never starts a recording.
9. **Amber `#f4b545` means NEEDS YOU only.** Recording uses red (`UI_TK_RED`), §6.

### Technical constraints
- **Keyboard:** the A164 reports key presses only, no key-up, and the firmware auto-repeats a held
  key after 500 ms, every 60 ms (`keyboard.c`). A USB keyboard doesn't repeat.
- **RAM:** PSRAM is 32 MB; the recording buffer goes there. Internal RAM is the tight resource
  (Plan 4 B2): I2S DMA descriptors and buffers and the voice task's stack are internal.
- **Timers (Plan 4 B1):** no presentation timer faster than 300 ms, and only while Sessions is
  shown. This plan adds one exemption (§6.3, ruling P5-4).
- **Settings blob:** new fields go at the end of `app_settings_t`; this plan takes **CFG 24**
  (22 stays reserved for Plan 2's Task 10, which would then take 25).
- **One link task** owns libssh; every Host command is a queued request (`cc_link.c`). The
  transcription is one more request type and counts as link-busy (a power hold) while it runs.
- **Strings:** English only, every UI string in `i18n/i18n.h` through `tr()`, "Host" never "Mac",
  sentence case (Plan 4 §9). `ui_lint.py` enforces it.

## 4. Decisions given by the user and the controller

| # | Decision | Source |
|---|---|---|
| V1 | Transcription on the Host with whisper.cpp from Homebrew: `/opt/homebrew/bin/whisper-cli` (v1.9.5) | user |
| V2 | Model `~/.local/share/whisper/ggml-base.en.bin` (base.en, SHA1 `137c40403d78fd54d454da0f9bd998f78703390c`, verified on the Host) | user |
| V3 | Measured on the Host: ~0.3 s for one spoken sentence once warm; ~6.7 s on the first run after a Host restart (Metal warm-up) | user, confirmed in §8 |
| V4 | Audio goes over the existing SSH link; the command comes from a `cc_cmd_*` builder; never auto-send | user |
| V5 | The audio never persists on the Host: streamed over the channel's stdin (verified to work, §8) | user, §8 |
| V6 | Whisper path and model path are Settings › Host rows with those defaults, validated like the Claude path | user |
| V7 | Recording uses a colour other than amber; text says Host; English only; strings through `tr()` | user |
| V8 | No GPL code added to the firmware | user |
| V9 | Recording holds the screen on; the mic is powered only while recording | user |
| V10 | A missing `whisper-cli` or model gives a clear error naming the missing piece and the README step | user |

## 5. Interaction design

### 5.1 Toggle, not hold-to-talk
**Decision:** Ctrl+Space **toggles**: press to start, press again to stop. A tap on the mic button
does the same.

- **Why:**
  - the A164 has no key-up, so hold-to-talk can't be built on it without guessing a release from
    the end of the auto-repeat chain (500 ms before the first repeat, then 60 ms gaps), which would
    cut the first half second or stop late;
  - holding Ctrl and Space with two thumbs for 10–30 s is tiring and blocks the hands;
  - the Plan 4 spec already reserved Ctrl+Space "as a toggle" (§4 Hotkeys);
  - one state machine serves keyboard and touch.
- The Plan 4 audit sketched hold-to-talk for **touch**. **Decision:** touch toggles too, so the
  mic button behaves one way. A finger sliding off a held glass button would otherwise end the
  recording. Cost if wrong: press and release handlers on one button.
- **Auto-repeat guard** (pure, host-tested: `vr_on_voice_key`): a Ctrl+Space less than 150 ms after
  the previous one is a repeat and is ignored, and so is one less than 700 ms after the last
  start or stop. Holding the keys therefore starts once, and a press after letting go stops it.

### 5.2 What each key does in each state

| State | Ctrl+Space / mic tap | Esc | Enter | Other keys | Touch on a row |
|---|---|---|---|---|---|
| Idle (field shown) | start | clears the draft (today) | send (today) | type (today) | select (today) |
| Idle (no field: stopped, interactive, no rows) | toast "Voice input works in a session's answer field" | today | today | today | today |
| Recording | stop and transcribe | **cancel**: the audio is wiped, nothing is sent | eaten | eaten (key clicks would be recorded, and the draft must not change under the recording) | selects, but the recording stays tied to its session |
| Transcribing | note "Still transcribing…" | **cancel**: the result is dropped when it arrives | eaten | eaten | selects |
| Done (text in the field) | start a new recording (appends) | clears the draft (today) | send | type | select |

Leaving Sessions (Settings, Terminal, Open) while recording cancels the recording. Leaving while
transcribing does not: the result is held for 60 s and goes into its session's draft when Sessions
is next shown.

### 5.3 Where the text goes
- The recording is tied to the **session key** that was selected when it started (the same key
  drafts use, Plan 4 P4-8). The transcript is appended to **that** session's draft, whatever is
  selected when it arrives (Plan 4 Review Focus 1).
- Appending: one space between the existing draft and the new text, none when the draft is empty
  or already ends in a space.
- The answer field holds 480 bytes (`SM_ANSWER_MAX`). 30 s of speech is about 75 words, about 420
  characters, so it normally fits. If the draft plus the text doesn't fit, the text is cut at the
  last whole word that fits and the note says "The text was cut to fit the answer field".
- If that session has gone from the list, the text is dropped and a toast says "That session is
  gone, so the text was dropped". Nothing is kept.
- **Never auto-send.** The field gets focus and the note says "Check the text, then press Enter
  to send". Enter goes through today's paste path with its input-ready gate (D5).

### 5.4 Limits
- **Maximum recording 30 s. Decision.** Why:
  - whisper decodes audio in 30 s windows, so ≤ 30 s is one window: no seams between windows and
    the fastest decode;
  - 30 s of speech (~420 characters) fits the 480-byte answer field;
  - RAM: 16 kHz × 16 bit × mono = 32,000 B/s, so 30 s is 960,000 B plus a 44-byte header, under
    1 MB of the 32 MB PSRAM, allocated only while recording;
  - the upload is about 1 MB; on the home LAN that is about a second (measured in Task 5/9).
  - Cost if wrong: one constant (`VR_MAX_S`); longer would need chunking above 30 s.
- At 30 s the recording stops by itself and transcribes; the note first says "Stopped at the 30 s
  limit". The timer shows `0:25 / 0:30` and turns red for the last 5 s.
- **Minimum 0.5 s.** A shorter recording is discarded with "Too short. Press Ctrl+Space, speak,
  then press it again." (a double tap, not speech).
- **Silence gate.** If no 100 ms block of the recording reaches −50 dBFS (tuned on the device,
  Task 5), nothing is sent and the note says "No speech heard. Try again closer to the Tab5."
  Why: whisper turns digital silence into "you" and low noise into "(blender whirring)" (§8); not
  sending them saves a Host round trip and a wrong draft.

## 6. UI states (Sessions detail pane)

The answer row today is the field plus a 64 × 64 mic slot drawn disabled (dashed border, muted mic,
Plan 4 §5.1). Plan 5 enables it. Nothing is relaid out: the field keeps its width.

### 6.1 States

| State | Mic button | Action-row note (right of Send / Open) | Field |
|---|---|---|---|
| **Idle** | solid 1 px `line` border, `fg` mic glyph (enabled); hit area 80 px | today's notes | today |
| **Starting** (mic opening, ≤ 300 ms) | 2 px red border, red mic glyph | "Recording" in red, timer `0:00 / 0:30` | unchanged, not editable |
| **Recording** | 2 px red border; a filled red 12 px dot replaces the glyph | the **voice strip**: red dot · "Recording" · timer `0:07 / 0:30` · level meter · "Ctrl+Space stops · Esc cancels" (muted) | unchanged, not editable |
| **Transcribing** | 1 px `line` border, muted glyph | "Transcribing on the Host…" (muted); after 3 s it adds the seconds: "Transcribing on the Host… 5 s" | unchanged |
| **Done** | idle | "Check the text, then press Enter to send" (muted), 5 s, or the cut note | the text appended, cursor at the end, focused |
| **Error** | idle | the error in red (§7), until the next key or 90 s (today's note TTL) | unchanged |

### 6.2 The voice strip
Built once with the pane (hidden), shown only while recording, in the action row's note slot:
- a 12 px red dot (`UI_TK_RED`, radius full);
- "Recording", red, `FONT_SMALL`;
- the timer, `fg` (red in the last 5 s), `m:ss / 0:30`;
- the level meter: an `lv_bar` 160 × 8 px, indicator `fg`, track `line`, radius 4, value 0–100
  from the last 100 ms block's RMS (−60 dBFS → 0, 0 dBFS → 100);
- the hint, muted.

**Decision: red for recording.** The token set is fixed (Plan 4 §8.1) and has no other free accent:
green means working/OK, amber means needs-you, `fg` and `muted` are neutral. Red with a dot is the
common recording sign. It also means errors, but an error is text, never a dot plus a running timer,
so they don't read alike. Cost if wrong: one token. The meter is `fg`, not green, so it doesn't read
as "working".

### 6.3 Timer
**Decision (ruling P5-4):** a **100 ms** LVGL timer exists only while recording; it updates the
timer label and the meter, and is deleted at stop or cancel. It is exempt from the B1 300 ms ceiling
like the pty pump: a meter at 300 ms looks broken, and the meter is how you know the mic hears you.
The 300 ms Sessions tick polls the transcribing state and results. Cost if wrong: a choppier meter
at 300 ms.

### 6.4 Copy (exact; all in `i18n.h`)

| Id | English |
|---|---|
| `VOICE_RECORDING` | Recording |
| `VOICE_HINT` | Ctrl+Space stops · Esc cancels |
| `VOICE_TRANSCRIBING` | Transcribing on the Host… |
| `VOICE_REVIEW` | Check the text, then press Enter to send |
| `VOICE_CUT` | The text was cut to fit the answer field |
| `VOICE_AT_LIMIT` | Stopped at the 30 s limit |
| `VOICE_TOO_SHORT` | Too short. Press Ctrl+Space, speak, then press it again. |
| `VOICE_NO_SPEECH` | No speech heard. Try again closer to the Tab5. |
| `VOICE_BUSY` | Still transcribing… |
| `VOICE_NEEDS_FIELD` | Voice input works in a session's answer field |
| `VOICE_SESSION_GONE` | That session is gone, so the text was dropped |
| `VOICE_NO_WHISPER` | whisper-cli isn't on the Host at the Whisper path. See README › Set up voice input. |
| `VOICE_NO_MODEL` | The whisper model isn't on the Host at the model path. See README › Set up voice input. |
| `VOICE_BAD_MODEL` | The Host couldn't load the whisper model. Download it again (README › Set up voice input). |
| `VOICE_CANT_RUN` | whisper-cli didn't run on the Host. Check the Whisper path in Settings › Host. |
| `VOICE_AUDIO_BAD` | The Host couldn't read the recording. Try again. |
| `VOICE_FAILED` | Transcription failed on the Host. Try again. |
| `VOICE_TIMEOUT` | The Host took too long to transcribe. Try again. |
| `VOICE_LINK_LOST` | Lost the connection to the Host. Try again. |
| `VOICE_MIC_FAILED` | The microphone didn't start. Restart the Tab5 and try again. |
| `VOICE_NO_MEMORY` | Not enough memory to record. Restart the Tab5 and try again. |
| `SES_NOT_CONNECTED` (exists) | Not connected to the Host |
| `SET_WHISPER_PATH` | Whisper path on Host |
| `SET_WHISPER_PATH_PH` | /opt/homebrew/bin/whisper-cli |
| `SET_WHISPER_MODEL` | Whisper model on Host |
| `SET_WHISPER_MODEL_PH` | ~/.local/share/whisper/ggml-base.en.bin |
| `SET_VOICE` | Voice input |
| `SET_VOICE_CHECK` | Check |
| `SET_VOICE_READY` | Voice input is ready on the Host |
| `SET_CLAUDE_PATH_BAD` (exists, reused for both new paths) | Use only letters, digits and _ . / ~ - (1 to 63 characters) |
| `HK_W_CTRL_SPACE` (changed) | voice input: press to start, again to stop |
| `SET_HOST_NOTE` (changed) | The Host is one of your SSH hosts. The Tab5 lists its Claude Code sessions, runs claude from this path, and runs whisper-cli for voice input. |

`HK_VOICE` ("Voice input comes in a later update") is deleted. The timer is a format with no
letters (`"%d:%02d / %d:%02d"`), which `ui_lint` allows.

### 6.5 Settings › Host
After **Claude path on Host**, three rows:
- **Whisper path on Host** · `/opt/homebrew/bin/whisper-cli` (value row; prompt; refused with
  `SET_CLAUDE_PATH_BAD` unless `cc_path_valid`);
- **Whisper model on Host** · `~/.local/share/whisper/ggml-base.en.bin` (same);
- **Voice input** · Check (action row): runs the check command (§7.2) and toasts
  `SET_VOICE_READY` or the matching error (`VOICE_NO_WHISPER`, `VOICE_NO_MODEL`,
  `SES_NOT_CONNECTED`, …).

Reset settings (About) keeps both paths, like the Claude path (Plan 4 §5.12 scope).

## 7. The Host commands

### 7.1 Transcribe (`cc_cmd_transcribe`)
Exact text, with `W` and `M` the validated settings:

```sh
exec /bin/sh -c 'W=<whisper>; M=<model>; [ -x "$W" ] || { cat >/dev/null; exit 120; }; [ -r "$M" ] || { cat >/dev/null; exit 121; }; exec "$W" -m "$M" -l en -nt -np -otxt -of - -f -'
```

- It runs as a **non-pty exec channel** (a pty would echo and translate the binary stdin). The
  Tab5 writes a 44-byte PCM WAV header plus the samples to stdin, sends EOF, and reads stdout (the
  transcript), the first line of stderr and the exit status.
- `exec /bin/sh -c '…'` gives fixed POSIX semantics whatever the user's login shell is, the same
  pattern as the session watch (`cc_cmd_watch`). sshd runs it with the user's shell non-
  interactively, which doesn't read `.zshrc` (no tmux, rule 1).
- `W=~/…` in an assignment gets tilde expansion in `/bin/sh`; the paths are quoted when used. No
  `'` can appear inside (the paths can't hold one), so the single-quoted argument is safe.
- The pre-checks give two exit codes of our own, 120 and 121, because `whisper-cli` with a missing
  model exits 3 and prints its whole usage text. `cat >/dev/null` drains stdin first, so the
  Tab5's upload never hits a closed channel and the exit code always comes back.
- `-f -` reads the WAV from stdin. On its own that prints nothing (whisper-cli takes `-` as the
  output name too); `-otxt -of -` writes the plain transcript to stdout. `-nt` drops timestamps,
  `-np` drops progress prints, `-l en` fixes the language.
- **Nothing is written on the Host.** The audio only exists in sshd's and whisper-cli's memory. No
  temp file, no output file (verified: §8). No fifo or `mktemp` is needed, so there is no trap.
- **Nothing is left running.** whisper-cli exits after one file. If the Tab5 vanishes mid-upload,
  stdin ends early and whisper-cli exits 11 (truncated audio). If it vanishes during decoding,
  whisper-cli finishes (≤ ~1 s warm for 30 s of audio, ~7 s cold) and dies on its first write to the
  closed pipe. No pty means no SIGHUP is needed.

**Exit status → outcome** (pure, host-tested: `vr_host_result`):

| Exit | Meaning (verified §8) | Copy |
|---|---|---|
| 0, non-empty after cleanup | the transcript | — |
| 0, empty after cleanup | nothing recognised (also a near-empty upload) | `VOICE_NO_SPEECH` |
| 120 | `whisper-cli` missing or not executable at the path | `VOICE_NO_WHISPER` |
| 121 | the model file missing or unreadable | `VOICE_NO_MODEL` |
| 3 | the model didn't load ("failed to initialize whisper context") | `VOICE_BAD_MODEL` |
| 11 | the audio couldn't be read | `VOICE_AUDIO_BAD` |
| 126, 127 | the path isn't a runnable program | `VOICE_CANT_RUN` |
| anything else | — | `VOICE_FAILED` |

### 7.2 Check (`cc_cmd_voice_check`)

```sh
exec /bin/sh -c 'W=<whisper>; M=<model>; [ -x "$W" ] || exit 120; [ -r "$M" ] || exit 121; exit 0'
```

- Runs when a recording starts, **in parallel with the recording**. If it fails, the recording stops
  at once, the audio is wiped and the error shows, so you learn about a missing model before you
  have spoken 30 s, not after. The Settings **Check** row runs it too.
- It does not warm whisper up. **Decision:** the first transcription after a Host restart pays the
  ~7 s Metal warm-up; the transcribing note counts seconds, and the request timeout is 60 s. Why: a
  warm-up run would cost the Host CPU and battery on every recording for a once-per-boot delay. Cost
  if wrong: one more command that runs whisper on 1 s of silence at check time.

### 7.3 Timeouts
- The check: 10 s (the link's usual exec budget).
- The transcribe request: **60 s** from its start, covering the upload, a cold Metal start and the
  decode. Over Tailscale (DERP relay) the 1 MB upload may be much slower than on the LAN; Task 9
  measures it, and the builder raises the deadline if a 30 s clip over Tailscale misses it.

## 8. Host feasibility (run on the Host, 2026-10-24)

Run locally on the Host in a scratch directory (not over SSH; nothing installed). The test WAVs
were made with `say -o x.wav --data-format=LEI16@16000 "…"`.

1. **`whisper-cli --help` (v1.9.5)** lists `-f FNAME, --file FNAME  input audio file path` and
   "supported audio formats: flac, mp3, ogg, wav". It does not mention stdin.
2. **`-f -` reads stdin.** `whisper-cli -m <model> -nt -np -f - < x.wav` logs
   `read_audio_data: read 79824 bytes from stdin` to stderr and exits 0, but prints **no
   transcript**, with the warning `'--output-file -' used without any other '--output-*'`.
3. **`-otxt -of -` puts the transcript on stdout:** `…-otxt -of - -f - < x.wav` printed
   `Please run the tests and then commit the change.` plus a newline, exit 0, with nothing else on
   stdout. A piped stdin (`cat x.wav | …`), not just a redirected file, works the same.
4. **The exact §7.1 command**, run as `zsh -c "<command>" < clip.wav`, transcribed correctly with
   the `~/…` model path; exit 0; stderr's first line is the `read_audio_data` line.
5. **Header forms.** A minimal 44-byte PCM header built from scratch works. A header with
   `0xFFFFFFFF` RIFF and data sizes (unknown length) also works, so streaming while recording
   is possible later (§12). A 22.05 kHz WAV is resampled and works; the Tab5 sends 16 kHz anyway.
6. **Failure modes:** a missing whisper path gave 120; a missing model gave 121; a corrupt model
   (1000 random bytes) gave 3 with `error: failed to initialize whisper context`; an empty stdin, or
   the first 100 bytes of a `say` WAV (its header is 4 KB, so that cuts the header off), gave 11
   (`failed to read audio file '-'`); a complete 44-byte header plus 16 bytes of samples gave 0 with
   empty output. Without the pre-checks, a missing model gives 3 and the whole usage text.
7. **Non-speech output:** 2 s of digital silence gave `you`; 3 s of low Gaussian noise (σ = 30)
   gave `(blender whirring)`; speech followed by 3 s of that noise gave only the speech.
   `--suppress-nst` (`-sns`) turned the noise into `you`, which is harder to clean, so it is not
   used. Hence the device-side silence gate (§5.4) and the cleanup rules (§9.3).
8. **Multi-sentence output:** a 10.7 s clip of four sentences came back on one line with digits as
   numerals ("8081"); the cleanup still joins lines in case a longer clip splits into segments.
9. **Timing (warm):** 2.4 s clip 0.30–0.40 s wall; 10.7 s clip 0.37–0.38 s wall (three runs).
10. **Nothing left behind:** the scratch directory held only the input WAVs after every run, and
    `pgrep -fl whisper-cli` found nothing afterwards.
11. **Model:** `shasum` of the model file is `137c40403d78fd54d454da0f9bd998f78703390c`, matching V2.

**Conclusion: the stdin route works.** No temp file, fifo or trap is needed.

## 9. Firmware design

### 9.1 Modules

| Module | Kind | Job |
|---|---|---|
| `companion/cc_cmd.c` | pure, host-tested | `cc_cmd_transcribe`, `cc_cmd_voice_check` |
| `voice/voice_rules.c` | pure, host-tested | WAV header, downmix, RMS/dBFS, meter, silence gate, key toggle guard, transcript cleanup, draft merge, exit-code mapping, outcome → string id |
| `companion/cc_link.c` | firmware | `cc_link_exec_stdin` (a REQ_EXEC with a stdin payload, link-busy while it runs); the whisper/model settings getters and setters |
| `voice/mic.c` | firmware | the ES7210 capture: open, read, close (I2S RX channel created per recording and deleted after) |
| `voice/voice.c` | firmware | the voice task: the PSRAM buffer, recording, the parallel check, the upload and result; the `voice` console command |
| `ui/ui_sessions.c` | firmware (preview-tested) | the states, mic button, voice strip, keys |
| `ui/ui_settings.c` | firmware (preview-tested) | the three Host rows |
| `settings/*` | pure migration + firmware | `whisper_path`, `whisper_model`, CFG 24 |
| `sys/power.*` | firmware | `POWER_HOLD_VOICE` |

### 9.2 Capture
- **Hardware:** the ES7210 4-channel mic ADC on I²C (`ES7210_CODEC_DEFAULT_ADDR`), I2S on the BSP
  pins (`BSP_I2S_MCLK` G30, `SCLK` G27, `LCLK` G29, `DSIN` G28). The ES8388 and the speaker amp
  stay off.
- **Decision: `mic.c` builds its own RX-only I2S channel per recording** (std Philips, 16 kHz,
  16-bit, stereo slots) and an `esp_codec_dev` input device on it (`audio_codec_new_i2s_data`,
  `es7210_codec_new` on `bsp_i2c_get_handle()`), instead of `bsp_audio_init()`. Why: the BSP creates
  a TX and an RX channel, enables both, keeps their handles private and has no deinit, so MCLK would
  run forever after the first recording. With our own channel, `mic_close()` closes the codec (the
  ES7210 powers down its ADCs), disables and deletes the channel (MCLK stops), and frees the DMA
  buffers. Cost if wrong: if the codec won't configure this way, fall back to the BSP calls and
  measure the idle cost.
- **Format:** 16 kHz, 16-bit, read as 2 interleaved channels, downmixed to mono by averaging (or one
  channel picked: open item O1). Gain via `esp_codec_dev_set_in_gain`, starting at 30 dB, tuned so
  normal speech at arm's length peaks around −12 dBFS (Task 5).
- **Reads:** 100 ms blocks (1,600 frames), each written into the PSRAM buffer after the 44-byte
  header slot; each block's RMS feeds the meter and the silence gate.
- **The voice task** (stack 4 KB, internal) is created at start and deletes itself after the result.
  The parallel check (§7.2) runs in a short helper task (3 KB, internal) that queues the check on
  the link, stores its exit code and ends; the voice task reads that code after each block.
  Nothing voice-related exists at boot or on a check-in boot: the mic, the codec and the tasks are
  all created on the first Ctrl+Space.
- **Homebrew:** the formula is `whisper.cpp` (it was renamed from `whisper-cpp`; `brew list
  --versions whisper-cpp` on the Host prints that warning and `whisper.cpp 1.9.5`).
  `/opt/homebrew/bin/whisper-cli` is its symlink on Apple silicon; an Intel Host has
  `/usr/local/bin/whisper-cli` and sets the Whisper path.

### 9.3 Transcript cleanup (`vr_clean_transcript`, in place)
1. ESC, DEL and C0 control bytes other than `\n` and `\t` are dropped.
2. Every `[...]` and `(...)` segment and every `*...*` segment is removed (whisper's non-speech
   tags: `[BLANK_AUDIO]`, `[MUSIC]`, `(blender whirring)`, `*coughs*`). An unclosed bracket is kept
   as text.
3. Newlines and tabs become spaces; runs of spaces collapse to one; leading and trailing spaces go
   (whisper's leading space on console output is one of them).
4. If no letter or digit is left, the result is empty (→ `VOICE_NO_SPEECH`).
5. Valid UTF-8 passes through unchanged. The paste path's `cc_paste_build` sanitises again at send.

### 9.4 Privacy in RAM
- The buffer is `heap_caps_malloc(960,044, MALLOC_CAP_SPIRAM)` at start, `memset` to zero and freed
  as soon as the upload ends (success, error, cancel or timeout). It is never retried from RAM.
  **Decision:** discard on every outcome. Why: privacy and simplicity. Cost if wrong: re-speaking
  after a rare link drop.
- The transcript lives in the result slot (cleared after the UI takes it, or after 60 s) and then
  in the draft, like typed text.
- Logs print lengths, durations, dBFS and exit codes only: for example
  `voice: rec 3.2 s, peak -14 dBFS, upload 0.41 s, host 0.38 s, rc 0, 48 chars`.

### 9.5 Power
- `POWER_HOLD_VOICE` is on from start until the recording stops or is cancelled, so the screen
  stays Active (held → ACTIVE in `pr_next_state`). The transcription keeps the device Active through
  the existing link-busy hold (`cc_link_busy()`).
- The one exception stays as Plan 2 ruled (I1): on battery at ≤ 10 % with the off timeout passed,
  Hibernate wins over a hold. A recording can then be cut off; the audio dies with the RAM.
- The mic, I2S and the voice task exist only between start and the end of the upload (§9.2).

## 10. Failure modes

| What happens | What the user sees | Notes |
|---|---|---|
| Link down at Ctrl+Space | `SES_NOT_CONNECTED`; no recording starts | checked before the mic opens |
| `whisper-cli` missing | recording stops within about a second (the parallel check); `VOICE_NO_WHISPER` | also from Settings › Check |
| Model missing | same, `VOICE_NO_MODEL` | |
| Model corrupt | after the upload, `VOICE_BAD_MODEL` | exit 3 |
| Path is a directory or not executable | `VOICE_CANT_RUN` | exit 126 |
| PSRAM allocation fails | `VOICE_NO_MEMORY`; nothing records | |
| Mic or codec won't open | `VOICE_MIC_FAILED`; the channel is torn down | |
| Recording < 0.5 s | `VOICE_TOO_SHORT` | nothing sent |
| No speech (silence gate) | `VOICE_NO_SPEECH` | nothing sent |
| Whisper returns only non-speech tags | `VOICE_NO_SPEECH` | cleanup empties it |
| 30 s reached | auto-stop, `VOICE_AT_LIMIT` briefly, then transcribing | |
| Link drops during the upload or decode | `VOICE_LINK_LOST`; audio wiped | `cc_link_exec_stdin` → `ESP_FAIL` / `ESP_ERR_INVALID_STATE` |
| Host slow (cold start, Tailscale) | the note counts seconds; at 60 s `VOICE_TIMEOUT` | the link drops the session on a timeout, as `do_exec` does |
| A paste or log fetch already queued | the transcription waits its turn on the link queue | |
| Esc while recording | the recording is cancelled, audio wiped, no note | |
| Esc while transcribing | the result is dropped when it arrives | the Host command still finishes (bounded) |
| Leave Sessions while recording | cancelled | |
| Leave Sessions while transcribing | the result waits up to 60 s for Sessions | |
| The session ended meanwhile | `VOICE_SESSION_GONE` | text dropped |
| Text too long for the field | cut at a word, `VOICE_CUT` | |
| Screen would dim | it doesn't: the voice hold, then the link-busy hold | |
| Battery ≤ 10 % + off timeout | Hibernate may end the recording | Plan 2 I1, accepted |

## 11. Test strategy

### Host unit tests (`firmware/test/host`)
- `test_cc_cmd.c`: the exact transcribe and check strings; rejection of every bad path (space,
  `'`, `;`, `$`, backtick, newline, empty, 64 chars, NULL) and of a small `cap` (out cleared).
- `test_voice_rules.c`: the WAV header bytes; downmix (average, pick, saturation); RMS/dBFS and
  meter mapping; the silence gate; the key toggle guard (a held key starts once, a release-and-press
  stops, the hold after a stop doesn't restart); cleanup (leading space, `[BLANK_AUDIO]`,
  `(blender whirring)`, multi-line, control bytes, UTF-8, unclosed bracket); draft merge (spacing,
  cut at a word, UTF-8 boundary, empty text); exit-code mapping; outcome → `STR_*`.
- `test_settings_migrate.c`: v23 → v24 gets the default paths; a v24 blob keeps edited ones; an
  invalid stored path falls back to the default.

### Python tests (`firmware/tools`)
- `test_transcribe_cmd.py` runs the real §7.1 and §7.2 strings (from `cc_cmd_print`) under
  `/bin/zsh -c`, with a **fake** `whisper-cli` in a temp dir and `HOME` pointed at a temp dir: exit
  120/121 with stdin drained, the fake's args exactly `-m <model> -l en -nt -np -otxt -of - -f -`,
  stdin bytes passed through unchanged, no new file anywhere under the temp `HOME` or the temp dir,
  and no process left.
- The same file has one test that uses the **real** `whisper-cli` and model when both exist
  (skipped otherwise): a `say` WAV of "Please run the tests and then commit the change" comes back
  containing "run the tests" and "commit".
- `test_ui_lint.py`: the new lint rules (§12 of the plan).

### Preview (`firmware/tools/ui_preview`)
New `sessions` variants `voice-rec`, `voice-rec-limit`, `voice-busy`, `voice-done`, `voice-error`
and `voice-nofield`: the fit check, the amber scan (no amber pixels in any voice state), the timer
check (a 100 ms timer only in `voice-rec`), and the key routing (Enter and letters eaten while
recording; Esc cancels). `settings 0` (Host) shows the three rows.

### Device gates (Task 9)
Heap (internal minimum during a 30 s recording plus transcription, Tailscale off and on); PSRAM
returned; a test phrase played with `say` on the Host transcribed into the answer field; every error
path; timers; power (idle before and after, mic off); privacy (no transcript text in the serial log;
nothing new on the Host).

## 12. Open items for the builder
- **O1. ES7210 channel layout and gain on this board.** Which slots carry the two mics in std
  stereo mode, whether one mic alone is cleaner than the average, and the gain. Task 5 measures it
  with `voice rec` and records the choice in the notes.
- **O2. The silence threshold** (−50 dBFS default) and the meter floor (−60 dBFS), from a quiet
  room and from speech at arm's length.
- **O3. Upload throughput** on the LAN and over Tailscale (DERP), and whether 60 s is enough for a
  30 s clip over Tailscale. If not, raise the deadline or (later) stream while recording: the
  unknown-length header works (§8 item 5).
- **O4. Whether `esp_codec_dev_close` powers the ES7210 down**, measured as idle power before and
  after a recording.
- **O5. A one-word "you" or "Thank you."** from near-silence that passes the gate. Measure how often
  it happens; if it does, a later rule can drop a lone one. Not filtered now (it can be real speech).
- **Later, not in Plan 5:** a multilingual model (a settings change plus `-l auto`); streaming while
  recording. (Voice in a take-over and in an SSH session: done in §14.)

## 13. Decisions (index)
1. Toggle on Ctrl+Space, not hold-to-talk (§5.1).
2. Touch toggles too (§5.1).
3. Auto-repeat guard: 150 ms repeat gap, 700 ms after a toggle (§5.1).
4. Maximum 30 s; minimum 0.5 s; silence gate (§5.4).
5. Text appended to the starting session's draft; never sent (§5.3).
6. Keys other than Ctrl+Space and Esc are eaten while recording (§5.2).
7. Red for recording; meter in `fg` (§6.2).
8. A 100 ms timer only while recording (§6.3, P5-4).
9. Audio over stdin, no temp file (§7.1, §8).
10. Exit codes 120 and 121 from pre-checks (§7.1).
11. A parallel check at start; no warm-up run (§7.2).
12. 60 s transcription deadline (§7.3).
13. Our own RX-only I2S channel per recording (§9.2).
14. Audio discarded on every outcome (§9.4).
15. Voice in a take-over was out of scope (§2); reversed by user request, with SSH sessions too (§14).
16. CFG 24 (§3).

## 14. Voice in a session (follow-up, user request 2026-10-24)

The user asked for voice input inside the full-screen terminal screen too: the take-over of a Claude
Code session (`claude attach <id>` on a pty) and an SSH session to another host. This reverses the
first Non-goals bullet (§2). Everything in §5–§10 still holds unless this section says otherwise.

1. **The toggle.** Ctrl+Space in the terminal screen toggles a recording, in a take-over and in an
   SSH session alike. `hk_takeover` returns `HK_T_VOICE` for it, so it is no longer sent to the Host
   as NUL; **Ctrl+@ still sends NUL** (README). The same `vr_on_voice_key` guard as Sessions (150 ms
   repeat gap, 700 ms after a start or stop, `vr_on_voice_ended` after a stop without a key), with
   its own `vr_keys_t` per screen. Ctrl+Space while transcribing toasts "Still transcribing…"; before
   the session is connected it toasts "Connecting…" and starts nothing.
2. **The indicator.** No new widgets: the session bar's own dot and state label. Recording: a red dot
   and "Recording 0:07 / 0:30" in red, updated on the 16 ms pty pump only when the second changes
   (no new timer; the 100 ms meter timer is Sessions-only). Transcribing: a muted dot and
   "Transcribing on the Host…" (muted). The bar stays up while voice is active and hides 3 s after.
   **Red, never amber.** **Decision:** no hint and no meter in the bar. The state slot (about 420 px
   beside the name and five chips) cuts "Recording 0:07 / 0:30 · Esc cancels" short, and a hint
   toast at the start would be one more internal-RAM allocation while the DMA buffers are up (item
   5). The README says Ctrl+Space stops and Esc cancels. Cost if wrong: a wider bar layout or a
   second label.
3. **Keys while recording.** Every key but Ctrl+Space, Esc and the leave keys (Ctrl+←, Ctrl+Q,
   Ctrl+]) is eaten, and so are the bar's key chips (the Esc chip cancels), so key clicks stay off
   the Host and typing doesn't mix with the transcript. Esc cancels. A leave key (or "← Leave")
   cancels, then leaves (an SSH session still asks first). **While transcribing, keys pass normally,
   Esc included**: Esc belongs to Claude Code and the shell, so it doesn't cancel a transcription
   here (unlike Sessions); leaving does.
4. **Delivering the text.** The recording is keyed `term:<the screen's generation>`
   (`VR_TERM_KEY_PREFIX`); the screen takes only its own result (`voice_take_result_for`), and
   Sessions drops any `term:` result (`vr_key_is_term`). A TEXT result is typed into the pty as input
   with no Enter or newline, built by the pure, host-tested `cc_voice_type_build(text, bracketed,
   out, cap)` in `cc_cmd.c`:
   - every C0 control (`\n`, `\r`, `\t` become a space), DEL, ESC, C1 (U+0080–U+009F) and invalid
     UTF-8 removed, so the text can't execute anything or end the paste early; whitespace runs
     collapsed to one space and both ends trimmed;
   - wrapped in `ESC[200~ … ESC[201~` like `cc_paste_build`, but with no Ctrl+U clear and no Enter.
     **Decision:** wrapped only when the remote side turned bracketed paste on (`vt_bracketed_paste`,
     DECSET 2004; Claude Code does); otherwise the bare sanitized text, so a shell without it doesn't
     get `[200~` typed into its line. The text is already free of control bytes, so either form is
     safe. Cost if wrong: one argument.
   - empty (also after sanitizing) gives nothing; output that doesn't fit `cap` gives nothing.
   Errors toast with the §6.4 copy (6 s); "Stopped at the 30 s limit" toasts after the text. If the
   screen closed (or the session dropped) before the result arrived, the result is dropped: leaving
   calls `voice_cancel` for a run of this screen's (recording or transcribing) and takes and wipes
   a result it posted that nobody took. Never auto-send: no Enter, no `cc_link_paste`.
5. **RAM.** A take-over is the tightest internal-RAM case (≥ 20,480 B with Tailscale on, Plan 4 B2).
   This adds no internal-RAM allocation beyond what `voice_start` already takes (its two task
   stacks and the I2S DMA buffers): no LVGL objects (the bar's dot and label are reused), the result
   struct (about 620 B) and the typed bytes (≤ 493 B) are in PSRAM and zeroed after use. The bytes go
   through the SSH client's existing tx ring buffer, like typed keys. **Device gate:** the combined
   device pass measures `ui heapmin` across a take-over recording with Tailscale on; it must stay
   ≥ 20,480 B.

Tests: `test_cc_cmd` `test_voice_type_*` (no byte < 0x20 outside the markers, no 0x7f, no CR or LF,
exact wrapping, empty input, the cap), `test_hotkeys` `test_terminal_ctrl_space_is_voice`,
`test_voice_rules` `test_term_key`; preview `terminal voice-rec` (indicator, bar kept up, keys and
chips eaten, Esc, transcribing passes keys, leaving cancels, SSH too, no amber) and `terminal
voice-done` (the pty write equals the builder's output, bracketed and bare; no Enter or newline; no
paste; limit and error toasts; another key's result left alone).
