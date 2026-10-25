# Output pane: notes (2026-10-24)

The Sessions pane renders `claude logs <id>` on a virtual screen (`cc_logview`) instead of stripping escapes
(`cc_log_tail`), so it shows the conversation, not Claude Code's footer, status line and spinner.

## Measured (sizes and counts only)

- Claude Code 2.1.310 draws with a cell-diff renderer at absolute rows below `ESC[H` on the main screen: no
  scrolling, no alternate screen. A virtual screen at least as large as the session's renders the same.
- Session sizes seen: 200x50, 163x76, 114x24. Live `claude logs` outputs: 6-163 KB, 1-14 full clears each.
- Replay cost: 2-5 `vt_work` units a byte on 21 live streams.
- `claude logs` exits 1 for a stopped session ("job not found"); `| tail -c 524288` exits 0, so the pane says
  "No output yet" as before the branch.

## Ruling: the 512 KB limit (fix round 2)

Accepted with the fallback. The Tab5 keeps the bytes since the stream's latest clear in 512 KB of PSRAM
(`cc_tailbuf`, `cc_link_exec_screen`); when they don't fit, the pane falls back to `cc_log_tail`. A session that
runs long without an attach (Claude Code clears on start, attach and resize) can pass it.

**What would lift it:** a stateful streaming replay. Keep a vt per watched session and feed it only the new bytes
(`claude logs <id> | tail -c +<offset>`), so every byte is replayed once and nothing since the clear is ever lost.
That also stops the whole tail crossing the link every job. Not built.
