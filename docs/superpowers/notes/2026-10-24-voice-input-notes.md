# Plan 5 (voice input): measurements and rulings

Each task appends its section. Host names, addresses and session names are placeholders.

## Task 1: the Host commands and the Host-side harness

- `cc_cmd_transcribe` and `cc_cmd_voice_check` in `cc_cmd.c` are the exact spec §7.1 / §7.2 strings, one `snprintf` each. Both paths pass `cc_path_valid`; `out` is cleared on every failure.
- `cc_cmd_print` gained `transcribe|voicecheck <whisper> <model>`; the 5-argument watch form is unchanged.
- `firmware/tools/test_transcribe_cmd.py` runs the real command strings under `/bin/zsh -c` on plain pipes (no pty) against a fake `whisper-cli`. It finds `cc_cmd_print` through `CC_CMD_PRINT`, default `firmware/test/host/build/cc_cmd_print`.
- Checked: exact argv, 100,044 bytes through stdin, `~` model path, exit 120 / 121 with 960,044 bytes and with 4 MB on stdin (stdin is drained, no BrokenPipeError), `voicecheck` codes 0/120/121 with no output and no whisper run, no file and no process left behind.
- `test_real_whisper` ran (not skipped) with the real whisper-cli and `ggml-base.en.bin` on `say` speech: wall time 0.33 to 0.50 s for a 3 s clip, exit 0, transcript contained "run the tests" and "commit", scratch dir empty after `t.wav` was deleted.
- `ui_lint.py` prints nothing when clean (it prints only violations), not `[]`.

## Task 3: whisper settings (CFG 24), cc_link_voice_check and cc_link_transcribe

- `app_settings_t` gains `whisper_path[64]` and `whisper_model[64]` at the end; `CFG_VERSION` 24. The step `old_version < 24` sets both defaults (the v23 tail padding may sit on them). `settings_reset_scope` keeps both. `settings_voice_paths_check` (in `settings_migrate.c`, so the host tests cover it) NUL-terminates both and falls back to the default unless `cc_path_valid`; `settings_init` calls it.
- `voice/voice_rules.c` was already in the firmware build: `main/CMakeLists.txt` globs `*.c` recursively. No CMake change needed; `companion voice-upload-test` uses `vr_wav_header`.
- **The libssh buffer cap (ruling for later tasks).** The port's libssh caps a buffer at `BUFFER_SIZE_MAX` 16384 (`components/libssh/src/buffer.c`), rounded up to a power of two. One 16 KB `ssh_channel_write` fails at once with "channel_write_common: Out of memory", because the data packet plus its header and MAC doesn't fit, and the session drops. So `exec_write_stdin` keeps the brief's 16 KB chunk (it drains stdout/stderr and checks the deadline and park between chunks), but writes each chunk as `ssh_channel_write` calls of at most 8 KB (`SSH_WRITE_MAX`). libssh was not changed.
- While a stdin exec runs, the link task writes the watch's keepalive when one is due. Without that, a 60 s transcribe could outlast the watch loop's 60 s quiet limit (3 K) and end the watch without the Tab5 asking.
- Console: `companion voice-check`, `companion voice-path|voice-model <path>` (`ok`, or the `SET_CLAUDE_PATH_BAD` copy), `companion voice-paths` (shows both), and `companion voice-upload-test <kb> [delay-ms]`. The upload test runs on its own 4 KB task, so the console stays free for a `reply` or `power go radio-off` on the next line. It prints lengths, times and the exit status only. The WAV buffer is zeroed and freed.

### Device, LAN (Host `my-host.local`, Tailscale off, watch on)

| Check | Result |
|---|---|
| First boot after the flash | `settings v23 -> v24`. Captured on a second pass: the first flash's boot was missed, so a v23 build (2f85cf3) was flashed (it logged `settings v24 -> v23` and saved a v23 blob, which also tests the downgrade path), then this build again |
| Second boot (`reboot`) | no `settings v` line |
| `voice-check`, defaults | `rc 0` (about 50 to 80 ms) |
| `voice-model ~/nope.bin` | `rc 121` |
| `voice-path /nope` | `rc 120` |
| `voice-path "/a b"` | the path copy; `voice-paths` unchanged |
| 937 KB upload, `voice-path /nope` | rc 120 after the whole upload (1207 ms, then 1410 ms): the pre-check drains stdin, so the exit code comes back |
| 937 KB upload, `voice-model ~/nope.bin` | rc 121 after the whole upload (1225 ms) |

Upload times (the brief's O3): `upload` is the time from the exec request to the EOF, `total` the whole `cc_link_transcribe` call. All runs: rc 0, 4 chars back (not shown), and `companion status` connected with the watch on afterwards.

| Size | upload ms (3 runs) | total ms (3 runs) |
|---|---|---|
| 32 KB (1 s) | 60, 46, 51 | 362, 279, 445 |
| 320 KB (10 s) | 417, 420, 430 | 664, 640, 706 |
| 937 KB (30 s) | 1186, 1187, 1179 | 1316, 1514, 1339 |

LAN throughput is about 800 KB/s, so 30 s of audio uploads in about 1.2 s. A warm whisper takes about 0.1 to 0.3 s on silence.

- **Queued behind a paste:** `voice-upload-test 937 300`, then `reply <test-id> hello`, on back-to-back lines. The paste went first (`sent` at 3.35 s). The transcribe queued behind it and finished at 4.47 s (upload 1226 ms, rc 0).
- **A park mid-upload:** `voice-upload-test 937`, then `power go radio-off` 0.3 s later. The link parked at 0.44 s and the upload returned `ESP_ERR_INVALID_STATE` 20 ms after the park, with no EOF sent. After `power go active` the link reconnected in about 1 s and the watch was on again.
- On the Host: `pgrep -fl whisper-cli` found nothing after the park test or at the end.
- Afterwards: both paths were back at their defaults, `voice-check` gave rc 0, the link was connected with the watch on, and Tailscale was off. The test session was stopped and removed.

## Tasks 5 and 6: device pass A (mic tuning, voice service on the Tab5)

Tab5 on the desk near the Host (`my-host.local`, LAN, Tailscale off, watch on). Speech came from `say`, run locally on the Host through its built-in speakers at the user's volume (19 %, not changed). That is quieter than a voice at arm's length, so the speech levels below are a **lower bound**. The distance from the speaker was not measured.

### The driver on the board
- `mic_open` works as written: an RX-only I2S master on I2S 1 drives MCLK, the ES7210 runs as a slave, MIC1 and MIC2 fill the two slots, and no TX handle is needed. **No fallback to `bsp_audio_codec_microphone_init`.**
- **Fix: the power-up pop.** In every open and at every gain, block 1 (100 to 200 ms after the open) read about 20 dB above the room floor (−37 dBFS at 30 dB gain). That is above the −50 gate, so a silent recording would have been uploaded. `mic_read` now drops the first 250 ms after each open (`MIC_SETTLE_MS`), reading into the caller's buffer, so it costs no RAM. After the fix, blocks 0 to 11 read at the room floor.
- **Fix: one error line per close.** Teardown called `i2s_channel_disable` after `esp_codec_dev_close` had already disabled the channel, which logged `E i2s_common: the channel has not been enabled yet`. Teardown now skips that call once the device was opened. One harmless E line is left on each open; it comes from esp_codec_dev's own `set_fmt`, not from mic.c.
- **Channels (O1):** L (MIC1) and R (MIC2) stay within 1 to 2 dB of each other in every run, silent and speech. Their average reads up to 2 dB below either channel, so they are two separate mics. **`MIC_PICK` stays −1 (average).** `MIC_SELECT` is unchanged.

### Levels (loudest 100 ms block per half second, dBFS)
| Gain | Silent floor (quiet room) | `say` phrase | Notes |
|---|---|---|---|
| 20 dB | −62 to −70 | −52 to −56 | |
| 30 dB | −60 to −70 (median about −66); −46 to −55 at noisy moments | −43 to −48 (6 runs) | A loud room sound: blocks at −14 dBFS RMS, peak −4 dBFS, not clipped |
| 37.5 dB | −45 to −53 at a noisy moment | −39 to −44 | no clipping |

The floor rises with the gain one for one, so it is acoustic (the room and the Host's fans after a build), not ADC noise. More gain adds no SNR, only less headroom.
- **`MIC_GAIN_DB` stays 30.** The `say` phrase at about −47 dBFS transcribes exactly every time. 37.5 dB would push the loud room sound into clipping. A real voice at arm's length is louder than the speaker at 19 %, so peaks near −12 dBFS are likely at 30 dB. This was not measured with a person speaking.
- **`VR_SILENCE_DBFS` stays −50 (O2).** The brief's midway between the floor (−66) and the measured speech (−46) gives −55, so −55 was tried. In silence, a −55 dBFS block got through it: it was uploaded, and whisper returned 3 characters (a hallucination, O5). At −50 the same silence gives NO_SPEECH with no upload (loudest −64 and −55). Every `say` run passes, but with only a 2 to 3 dB margin, because the measured speech is a lower bound. `test_heard_threshold` is unchanged.

### RAM (O4 and the internal budget)
- `mic_open` takes **7,412 B internal** (the 6.4 KB DMA ring plus about 1 KB of driver structs), measured as the free-heap difference around the open. All of it comes back at close.
- `ui heapmin` with `voice rec 10`: internal min 127,727 B. An idle window with nothing running reads min 138,355 B, and free at idle is 143,123 B. Of the 10.6 KB dip below the idle min, 7.4 KB is the mic; the rest is ordinary churn.
- `ui heapmin` with `voice test 6` (the voice task 4 KB, the check helper 3 KB, the mic, the exec): min 125,039 B. Free afterwards was 143,399 B, so nothing leaked.
- The voice task's stack low-water mark is 792 to 992 B of 4,096 across every path. `VOICE_STACK` stays at 4 KB.
- PSRAM: a `voice test-cancel 3` leaves free PSRAM exactly as before (23,588,760 B both times).

### Power (O4)
- The brief's windows ran with `power_report.py --log --include-charging`: 2 min idle on Sessions, `voice rec 10` three times from Home, 2 min idle. Every tag read 8 to 9 mW (300 samples: 0x00 9 mW, 0x01 9 mW, 0x02 8 mW). That says nothing here: the Tab5 is on USB (`src=usb-standby`), and the INA226 reads about 1 mA, as Plan 4 found. **The recording's own draw and the before/after idle difference could not be measured on USB.** This needs a run on battery.
- Instead, the ES7210's registers were read back over I2C, with a temporary verb that is not committed. Before any recording since boot and after a recording, they are identical: `0x01`=0x7f (clocks off), `0x06`=0x07 (power down), `0x40`=0xc0 (analog off), `0x4b`/`0x4c`=0xff (MIC12/34 power off). After `mic_close` the codec is back in its boot power-down state. The I2S channel is deleted, so MCLK stops.

### Task 6: the voice service
| Path | Result | `voice status` after |
|---|---|---|
| `voice test 5`, the phrase | TEXT, 47 chars (4 runs); rec 4.8 to 5.8 s, upload 0.20 to 0.27 s, host 0.18 to 0.41 s; from stop to result about 0.5 to 0.65 s | idle, closed, 0, off, none |
| `voice test 5`, silence | NO_SPEECH, no upload (loudest −64, −55) | same |
| `voice test-short` | TOO_SHORT (rec 0.1 s) | same |
| `voice-model ~/nope.bin`, `voice test 10` | NO_MODEL, the recording ended 0.39 s after the start (the check came back at 80 ms) | same |
| `voice-path /nope`, `voice test 10` | NO_WHISPER, ended at 0.39 s | same |
| `voice test 30`, speech | TEXT, 464 chars, at limit; rec 30.0 s, 960,044 B up in 1.28 s, host 0.52 s; from stop to result 1.8 s (O3, LAN). An earlier run that stopped at 29.9 s: 456 chars, upload 1.19 s, host 0.59 s | same |
| `voice test-bg 30`, then `power go radio-off` 0.45 s into the upload | LINK_LOST (`ESP_ERR_INVALID_STATE`, 20 ms after the park). After `power go active` the link reconnected in about 3 s | same |
| `voice test-cancel 3` | cancelled, no result, PSRAM back exactly | same |
| Power hold: `voice test-bg 20`, `power sim idle 300` at 5 s | `state=active holds=voice` at 6 s and 12 s; `holds=busy` during the upload; after the result the device idled to off, and `power sim idle 300` left it off with `holds=none` | same |

- **Console changes (debug verbs only):**
  - `voice test <s>` now counts recorded time, not wall time. At 30 s it no longer calls `voice_stop` at all, so the limit really ends the recording and `at limit` is reported. Before, the 250 ms settle plus the open meant the console's stop at 30 s of wall time beat the limit at 29.9 s recorded.
  - New `voice test-bg <1-30 s>`: the same test on a short-lived 4 KB task, so the console stays free for `power go radio-off`, `power sim idle` and `power`. Steps 3 (park) and 4 (hold) need it, because `voice test` holds the console's only task.
  - `voice status` prints `psram free` and `internal free`.
- Log quirk, harmless: on a park, cc_link logs `up in -1 ms` (no EOF was sent), and the voice line reports the call's total (0.58 s) as `upload`.
- A 20 s run of the phrase said 5 times came back as 47 chars, about one sentence. The text can't be inspected (rule 3), so whether whisper merged the repeats is not known.
- Privacy: the captured serial logs hold no transcript text. Lengths and outcomes only.

## Tasks 4, 7 and 9: device pass B (the Sessions loop, Settings › Host, the final gates)

The Tab5 sat on the desk near the Host (`my-host.local`, LAN). HEAD be7ed4e was flashed with NVS kept. Speech came from `say`, run locally on the Host through its built-in speakers at the user's volume, which was not changed. As in pass A, the speech levels are a lower bound.

Every device test used two Host sessions, `companion-test-voice` and `companion-test-voice2`, started with the favourite-colour prompt. Both were stopped and removed at the end. Each Enter, Ctrl+Space and text key came only after a screenshot showed a test row selected. All screenshots stayed in scratch storage.

The whole serial output was captured through one broker, so the privacy grep covers everything. Rule 3 applies here: this note records only whether each required word was present, never what the transcript said.

A testing gotcha, by design and not a bug: the 600 ms wake-swallow window (`PR_SWALLOW_MS`) also follows a console `power go active`, and every swallowed key extends it. A burst of keys sent right after a console wake is dropped entirely. The tests therefore wait 1 s after a wake.

### Task 7: the Sessions loop
| Step | Result |
|---|---|
| 3, the core loop | `key ctrl space`, `say` the colour answer, `key ctrl space`. The exact sentence appeared in the field 0.52 s after the mic closed, with the review note. `claude logs` showed nothing sent. Enter sent it, and the session answered. **Pass** |
| 4, held key | 30 Ctrl+Space presses 60 ms apart, all 30 decoded: one start, no stop, still recording 1.2 s after the burst. A single press 1 s later stopped it. **Pass** |
| 4, wake | `power go dim`, then Ctrl+Space: the key log shows `(wake key, swallowed)`, `voice status` is idle, and `power wakes` shows `key swallowed=1`. **Pass** |
| 4, Esc | A typed draft, then Ctrl+Space, then Esc 2 s later: CANCELLED, no upload, no result, the draft unchanged, timers back at the idle count. **Pass** |
| 5, touch | `power sim touch 1224 528` (the mic button's centre, from a screenshot) started recording, and a second touch stopped it. The text merged after the existing draft with one space. **Pass** |
| 6, wrong-session guard | Started on `companion-test-voice`, tapped `companion-test-voice2`'s row while recording (the strip followed the selection), then stopped. The text landed only in `companion-test-voice`'s draft; `companion-test-voice2`'s draft stayed empty. **Pass** |
| 7, leaving while recording | Tapped the footer's Settings chip: CANCELLED, mic closed, no result, `voice status` idle. **Pass** |
| 7, leaving while transcribing | Tapped Settings 0.25 s after the stop, during the transcription. Coming back 25 s later, the text was in the draft. **Pass**. See the edge below. |
| Edge: leaving 50 ms after a stop | The stop is a request that the voice task acts on at its next 100 ms block, so the phase is still RECORDING. The screen delete calls `voice_cancel`, and the run ends CANCELLED. The window is under 100 ms, which a person can't realistically hit (Ctrl+Space, then a tap). Not fixed; recorded as a minor. |
| M7, a press just after the 30 s auto-stop | Ctrl+Space about 0.3 s after the auto-stop: no new recording; `voice status` read transcribing with `at_limit 1`. **Pass** |
| M2, no dim before the result | A 30 s recording with no key at all, against the default 30 s dim: the state stayed `active` (`holds=voice`, then `voice,busy`), and the screen dimmed 1 ms after the result posted. **Pass** |
| I1, Esc while transcribing | Esc 0.3 s after an auto-stop, then Ctrl+Space: the toast reads "Still transcribing…"; the late result ends CANCELLED with 0 chars; the draft unchanged; idle afterwards. **Pass** |

### Task 4 step 3: Settings › Host
- The three rows sit after "Claude path on Host", with the defaults. **Pass**.
- Check toasts "Voice input is ready on the Host". **Pass**.
- The model set to `~/nope.bin` in the prompt (saved with Enter): Check toasts the NO_MODEL copy naming README › Set up voice input. **Pass**. The default was then restored through the prompt, and `voice-check` gave rc 0.
- `~/my model.bin` in the prompt is refused with the path copy, and the saved value is unchanged. **Pass**.

### Today's earlier fixes, on the device
- **Enter presses OK in a Settings prompt.** Claude path: typed `/usr/local/bin/claude` and pressed Enter, and the row showed the new value. The original `~/.local/bin/claude` was restored the same way. The Whisper model prompt behaves the same. **Pass**.
- **First Down on an empty Terminal list.** The Terminal list is empty, and the first Down lands on "+ Add host". **Pass**.
- **Settings › Network keeps focus across a refresh.** With the focus on Advanced, `ts on` (the toggle and Status updated) and then `ts off` both kept the focus on Advanced. **Pass**.
- The host-key trust flow was not tested; it needs real pins and is on the visit list.

### Task 9: the final table
| Gate | Measured | Verdict |
|---|---|---|
| 1, sizes against main 3d99c07 (built the same way) | Image 7,257,451 → 7,342,673 B (**+85,222**). DIRAM 190,800 → 194,634 B (**+3,834**): `.text` (IRAM) +2,402, `.bss` +1,208, `.data` +224. Against the Plan 4 final (7,253,147 / 190,008 B): +89,526 / +4,626 B, which includes main's own growth since. | Flash: **pass**. Static internal RAM: **over the < 1 KB expectation**, see below |
| 2, heap, Tailscale off | `ui heapmin` over a 30 s recording that hits the limit, plus its transcription on Sessions: internal min **111,899 B** (gate 40,960); PSRAM 23,537,060 → 23,537,016 B, 5 s after the result (−44 B); no ERR_BUF; connected afterwards | **Pass** |
| 2, heap, Tailscale on (connected; the link stayed on the LAN, local-first) | Internal min **55,467 B** (gate 20,480); PSRAM 23,120,612 → 23,120,580 B (−32 B); no ERR_BUF; connected | **Pass** |
| 3, phrase 1, "Please run the tests and then commit the change." | Tailscale off, 3 runs; Tailscale on, 1 run. "commit" present 4/4; "tests" heard as the singular "test" 4/4. Pass A's 47-char results were most likely the same. | **Strict substring fails** ("run the test", not "run the tests"). A whisper base.en accuracy point with `say`, not a firmware issue. Real voice is PENDING |
| 3, phrase 2, "Yes, go ahead with option two." | 3 runs: "option" and "two" present in 2 of 3; one run heard both as near-homophones | **Pass 2/3** |
| 3, phrase 3, "Rename the function to parse config…" | 3 runs: "rename" and "test" present in every run where the start could be seen (2 seen, 1 hidden by a merged draft) | **Pass** |
| 3, end-to-end time on the LAN (6 runs at real lengths, 3.7 to 6 s of audio) | From the mic close to the posted result: 0.36 to 0.59 s (upload 0.15 to 0.27 s, Host 0.20 to 0.33 s). From the stop key to the result line, wall clock: 0.49 to 0.70 s. The field shows it on the next 300 ms tick, so the text is on screen within about 1.0 s. | **Pass** (≤ 3 s) |
| 3, phrase 1 with Tailscale on | From the mic close to the result: 0.40 s; wall: 0.48 s | **Pass** |
| 3, 30 s at the limit | 960,044 B up in 1.20 to 1.25 s, Host 0.6 to 0.7 s, 1.9 s from the mic close to the result | **Pass** |
| 4, errors | `voice-path /nope`: NO_WHISPER 0.33 s after the start (check exit 120), a red note cut to fit, and a toast with the README pointer. `~/nope.bin`: NO_MODEL at 0.33 s (exit 121), the same. Both restored. `wifi leave`, then Ctrl+Space: a "Not connected to the Host" note and toast, no recording, the selection kept. `wifi join` reconnected in about 4 s. | **Pass** |
| 5, a recording stopped while a `reply` is in flight | The console runs one command at a time, so only the 30 s auto-stop can land while a reply runs. The reply went to `companion-test-voice2` at 29.6 s, and the auto-stop came during it. The reply printed `sent`, and the session answered. The transcription queued behind the reply (5.3 s in all, of which the upload was 1.19 s) and returned TEXT. | **Pass** |
| 5, `power go radio-off` 0.35 s into a 20 s upload | `ESP_ERR_INVALID_STATE` 30 ms after the park, then LINK_LOST, then the "Lost the connection to the Host" toast at the wake. After `power go active` the link reconnected in 4.4 s and the watch was back on. `pgrep whisper-cli` on the Host found nothing. | **Pass** |
| 6, timers | Idle Sessions: 6 timers. After the first `power sim touch` it is 7, and stays 7: that verb creates an event-mode pointer indev with a paused 16 ms timer that never runs, and it is not voice. Recording: one 100 ms timer more. One second after a stop, an Esc cancel, an error (once the toast's 6 s timer is gone) and a leave: back to the idle count. | **Pass** |
| 7, power | On USB the INA226 reads 1 to 2 mA (`src=usb-standby`) whether idle or recording, as pass A found. The draw and the before/after idle comparison can't be measured. `voice status` read idle and closed in every idle check, and no voice task, codec or I2S is created at boot. | **PENDING** (battery) |
| 8, privacy | Over the whole 15,000-line capture (screenshot frames removed), no word of any phrase, the colour answers or the long text appears. Of the transcript, the logs carry only lengths, dBFS and outcomes. The only reply text is the console echoing my own `reply` command, which is outside voice. On the Host, the `find … -newer marker` for `*.wav`, `*.txt` and `*.raw` found nothing voice-related: only tmux-resurrect, other Claude projects' tool results and an Apple file. `pgrep -fl whisper-cli` found nothing. The NVS check was skipped: there is no `companion settings dump` verb. | **Pass** (NVS: skipped) |

**Static internal RAM.** Of the DIRAM growth:
- About 2.4 KB is ESP-IDF driver code that runs from IRAM, pulled in by the first GDMA/I2S user: `libesp_hw_support` +1,954 B, mostly `gdma.c` and `dw_gdma.c` under the default `CONFIG_GDMA_CTRL_FUNC_IN_IRAM=y` and `CONFIG_GDMA_ISR_HANDLER_IN_IRAM=y`, plus `i2s_common` +372 B.
- `libmain` is +1,189 B, of which `voice.c`'s `.bss` is 806 B. Most of that is the one result slot `s_result` (`VR_KEY_MAX` 128 + `VR_TEXT_MAX` 480 + fields) in internal `.bss`.
- FreeRTOS and esp_system add +224 B.

Not changed here. Two options for the controller:
- (a) Allocate the result slot in PSRAM at the first post. This saves about 0.6 KB of `.bss` and takes `.bss` + `.data` from +1.4 KB to about +0.8 KB.
- (b) Set `CONFIG_GDMA_CTRL_FUNC_IN_IRAM=n`. This saves about 2 KB of IRAM, but it touches the display's DMA paths and needs an sdkconfig regeneration.

The runtime gates pass by wide margins.

**Other observations:**
- A quick `ts on` then `ts off` (about 10 s apart, still connecting) logged `microlink: 1 task(s) still running 15000 ms after the stop request -- the instance will be leaked`, and internal free was about 2.5 KB lower afterwards (135.2 → 132.7 KB). This is Tailscale, not voice.
- The status-bar clock doesn't redraw while the screen is off, so a screenshot taken then shows a stale time. That is expected.

### Visit list (PENDING, for the user)
1. **Real voice at a normal distance.** Speak from the couch and at arm's length, in an ordinary voice. Use the three phrases, plus something of your own. This is the real accuracy check. With `say` at 19 % volume, base.en heard "test" for "tests" every time and confused "option two" once. If real speech misses words too, a larger model (`ggml-small.en`, set in Settings › Host › Whisper model) is the lever; time it on the Host.
2. **The −50 dBFS gate margin.** `say` speech peaks at −44 to −47 dBFS, only 3 to 6 dB above the gate. Check that a quiet voice across the desk isn't gated as "No speech heard", and that a noisy room's silence doesn't upload. Then decide whether to retune `VR_SILENCE_DBFS`.
3. **Battery power.** On battery, 5 min idle on Sessions, five recordings, then 5 min idle. The second idle mean should be ≤ the first + 0.02 W and ≤ the Plan 4 final idle figure + 0.02 W. Record the recording's draw. With Hibernate check-ins on, a check-in boot should log no voice or mic line.
4. **The mic button and the strip at arm's length.** Do the red ring, the red dot and the meter read clearly? The meter sat near empty between words with `say`. Is 30 s long enough?
5. **The host-key trust flow** (polish-1), which needs real pins: Trust a first-run Host key on screen.
6. **Static internal RAM:** choose between options (a) and (b) above, or accept the 1.4 KB as is.
7. **Optional:** whether the error toast covering Send and Open for 6 s is acceptable, and whether the screen dimming right at the result (after a 30 s recording with no keys) should get a short extra hold. M2's "no extra hold" decision stands until then.

At the end:
- `voice status`: idle, mic closed, tasks 0, hold off, result none.
- Whisper path, model and Claude path at their defaults (the user's values were the defaults).
- Tailscale off; `companion status` connected with the watch on.
- Power settings unchanged and the key log off.
- Both `companion-test-*` sessions stopped and removed.
