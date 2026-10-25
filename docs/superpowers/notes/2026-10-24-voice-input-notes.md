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
