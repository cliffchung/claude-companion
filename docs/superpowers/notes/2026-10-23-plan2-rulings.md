# Plan 2 (power & vitals): rulings

Decisions made while executing Plan 2, each with what it costs if wrong. Overrule any of them in review.

## Rulings

- Ruling P1: the BSP reports `Discovered board version 2 (LCD ST7123, Touch ST7123, FW 3)` on this Tab5 — Tasks 5 and 6 depend on it — costs nothing.
- Ruling P2: `power_log_enable(true)` starts a new run and zeroes the in-RAM summaries (uptime restarts at 0 each boot, so mixing runs would break the energy maths); the NVS `sum` blob from the previous run is loaded at boot only so `power log status` can still show it after a brownout — costs a lost summary if someone toggles the log off and on within one boot.
- Ruling P3: the bench reads its step summaries from the log's per-tag sums (tag 0x10+n) instead of keeping its own; it turns the log on for the run and restores the previous setting — one sampling path, one set of numbers — costs a coupling between bench and log if wrong.
- Ruling P4: `power_bench_tag()` is added to the bench interface so the idle task tags samples with the running step; Task 5 will replace the `0` default with the power state — costs one extra function.
- Ruling P5: `power` prints `state=- idle=- holds=-` (all three) until Task 5, following the brief's text rather than its example's `holds=none` — costs a one-word change.
- Ruling P6: `power_report.py --bench` estimates battery life with an equal-weight mix of the steps that ran ok; a real mix needs the Task 10 usage profile — costs a rough number labelled "(estimate)".
- Ruling P7 (revised in fix round 1): `mw` is computed as mV x |mA| / 1000, rounded, not read from the INA226 power register (25 mW steps, unsigned) — low-power figures decide Plan 2's defaults — costs nothing; the raw register is no longer logged.
- Ruling P8: a sample is "charging" when ma < 0 or the charge pin is low (flag bit2). It stays in the ring but is kept out of the per-tag sums, bench steps and `power_report --log` (unless `--include-charging`); the fast bench runs on USB, so it lets them in — otherwise its scaffold check could never pass — costs a fast run that doesn't prove the exclusion.
- Ruling P9: if USB appears mid-step the bench keeps going and marks that step `usb`; later steps still run — the rest of the run stays useful and the bad step is labelled — costs a long run with several `usb` rows if the cable stays in. A step with n = 0 reports `no samples`; no INA226 refuses the start, and a log that can't enable stops the run with a reason shown by `power bench status`.
- Ruling P10: a step is stored as `running` in NVS when it starts and reports `interrupted` after a reset, with the partial data from the log's NVS `sum` blob if the log hasn't been re-enabled; the blob is only saved every 60 s, so short steps show no data — costs partial data up to 60 s stale.
- Ruling P11: during `power log dump`, samples are not pushed to the ring (sums keep updating) and `clear` is refused — the header count and row indices stay true without copying 768 KB — costs missing samples for the length of a dump.
- Ruling P12: energy uses the sample count as the seconds covered (1 Hz) instead of last_t - first_t + 1, which overstates interleaved tags; the brief's test still gives 1667 — costs nothing at 1 Hz.
