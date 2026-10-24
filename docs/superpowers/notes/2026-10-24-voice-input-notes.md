# Plan 5 (voice input): measurements and rulings

Each task appends its section. Host names, addresses and session names are placeholders.

## Task 1: the Host commands and the Host-side harness

- `cc_cmd_transcribe` and `cc_cmd_voice_check` in `cc_cmd.c` are the exact spec §7.1 / §7.2 strings, one `snprintf` each. Both paths pass `cc_path_valid`; `out` is cleared on every failure.
- `cc_cmd_print` gained `transcribe|voicecheck <whisper> <model>`; the 5-argument watch form is unchanged.
- `firmware/tools/test_transcribe_cmd.py` runs the real command strings under `/bin/zsh -c` on plain pipes (no pty) against a fake `whisper-cli`. It finds `cc_cmd_print` through `CC_CMD_PRINT`, default `firmware/test/host/build/cc_cmd_print`.
- Checked: exact argv, 100,044 bytes through stdin, `~` model path, exit 120 / 121 with 960,044 bytes and with 4 MB on stdin (stdin is drained, no BrokenPipeError), `voicecheck` codes 0/120/121 with no output and no whisper run, no file and no process left behind.
- `test_real_whisper` ran (not skipped) with the real whisper-cli and `ggml-base.en.bin` on `say` speech: wall time 0.33 to 0.50 s for a 3 s clip, exit 0, transcript contained "run the tests" and "commit", scratch dir empty after `t.wav` was deleted.
- `ui_lint.py` prints nothing when clean (it prints only violations), not `[]`.
