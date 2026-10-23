# Plan 2 (power & vitals): rulings

Decisions made while executing Plan 2, each with what it costs if wrong. Overrule any of them in review.

## Rulings

- Ruling P1: the BSP reports `Discovered board version 2 (LCD ST7123, Touch ST7123, FW 3)` on this Tab5 — Tasks 5 and 6 depend on it — costs nothing.
- Ruling P2: `power_log_enable(true)` starts a new run and zeroes the in-RAM summaries (uptime restarts at 0 each boot, so mixing runs would break the energy maths); the NVS `sum` blob from the previous run is loaded at boot only so `power log status` can still show it after a brownout — costs a lost summary if someone toggles the log off and on within one boot.
- Ruling P3: the bench reads its step summaries from the log's per-tag sums (tag 0x10+n) instead of keeping its own; it turns the log on for the run and restores the previous setting — one sampling path, one set of numbers — costs a coupling between bench and log if wrong.
- Ruling P4: `power_bench_tag()` is added to the bench interface so the idle task tags samples with the running step; Task 5 will replace the `0` default with the power state — costs one extra function.
- Ruling P5: `power` prints `state=- idle=- holds=-` (all three) until Task 5, following the brief's text rather than its example's `holds=none` — costs a one-word change.
- Ruling P6: `power_report.py --bench` estimates battery life with an equal-weight mix of the steps that ran ok; a real mix needs the Task 10 usage profile — costs a rough number labelled "(estimate)".
- Ruling P7: the INA226 power register has a 25 mW LSB, so `mw` reads 0 at idle USB currents; the CSV keeps what the chip reports and does not recompute V x I — costs coarse low-power numbers until a later task computes mW from mV x mA.
