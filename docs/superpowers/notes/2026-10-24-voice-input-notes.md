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
