# Plan 3 (local link): cutover and measurements

**Date:** 2026-10-23. **Device:** M5Stack Tab5 v2 on USB (`/dev/cu.usbmodem1101`), build 0ce04d8 + Task 2's commit. **Mac:** `my-host.local`, 192.168.1.50.

## Cutover

1. **The key line (Step 1).** The user widened the Tab5 key's line in `~/.ssh/authorized_keys` to `from="100.64.0.0/10,192.168.1.0/24"` themselves. The coordinator verified it; the backup is `~/.ssh/authorized_keys.bak-2026-10-23`. The agent didn't touch the file.
2. **The Mac entry (Step 2).** With the link connected over the tailnet (`path: tailnet 100.64.0.18`, carry source 100.64.0.18), I ran `companion server-host my-host.local 192.168.1.50` and then `ts off`.
3. **The link and the pin carry-over (Step 3).** The log, 1.2 s after the command:
   ```
   cc_link: Host entry host -> my-host.local, fallback 192.168.1.50, carry source 100.64.0.18
   cc_link: mac: my-host.local -> 192.168.1.50 (mdns)
   cc_link: kh: carried tailnet pin to 192.168.1.50 (from 100.64.0.18) SHA256:EXAMPLEHostKeyFingerprint000000000000000000
   cc_link: connected to Mac (192.168.1.50:22)
   ```
   - There was no prompt, and the fingerprint is the one pinned for the tailnet address.
   - `companion status` read `connected Mac`, `path: lan 192.168.1.50 (fallback 192.168.1.50; tailnet pin source 100.64.0.18; Tailscale off)`, `watch: on`.
   - The cards went live. The screenshot isn't kept because it shows the user's sessions.
4. **The take-over (found during Step 4, fixed: L25).** The first LAN take-over asked "New host key. Trust and continue?". The terminal connected to the entry's name `my-host.local`, and pins are per address (P76), so the key pinned for 192.168.1.50 didn't apply. I cancelled the prompt (rejected, nothing pinned). A take-over now connects to the address the link resolved and pinned, and it opens without asking.

## Measurements (Step 4, on USB)

"Before" is this build with the Tailscale switch on and the tailnet entry (100.64.0.18), which is Plan 2's path. "After" is the LAN entry with the switch off. Each figure comes from the device log.

| | Before (tailnet) | After (LAN, Tailscale off) |
|---|---|---|
| Cold boot to live (`hib: boot to live`) | 46.4 s (Plan 2 notes: 45.6–67.8 s) | 9.0, 9.0, 10.6, 9.0, 9.0, 8.8, 8.7, 8.0 s |
| Check-in boot, awake (`power checkins`) | `hib-handshake` 45.0 s (Plan 2 on battery: 44.6–46.5 s; rebinds 94.5–95.0 s) | `hib-lan` 8.0 s and 8.4 s |
| Check-in boot, energy | 7.6 mWh on USB standby (Plan 2 on battery: 43–46 mWh; rebind 95 mWh) | 2.7 and 2.0 mWh on USB standby |
| Check-in boot, boot to live | 44.5 s | 7.8 s and 8.2 s |
| Reconnect after `wifi leave` / `wifi join` (join to `connected`) | 5.0 s (`watch: on` 6.0 s) | 5.3 s and 5.6 s (`watch: on` 6.9 s and 5.9 s) |
| Time to open a take-over (`command session: connected … after open`) | 15.2 s (first output 15.6 s) | 0.68, 0.62, 0.62 s (first output 0.88–1.08 s) |
| Internal free heap, link and watch up | 64.7–84.5 KB, min 8.1–35 KB, largest 31744 B | 128.2–129.8 KB, min 35.9 KB, largest 63488 B |

Notes on reading the table:

- **USB energy.** The check-in mWh on USB is the USB-standby draw, a full pack with almost no charge current (P118). Indicative only; the battery projection uses awake time. On battery, Plan 2 measured about 1 mWh per awake second, so a LAN check-in should come to about 8–9 mWh instead of about 45.
- **Projected daily cost.** At the default 15-minute Hibernate check-in, that is about 96 × 8.5 ≈ 0.8 Wh a day instead of about 4.3 Wh (simulated from the awake time; not measured on battery).
- **Reconnect is the same both ways.** A short drop is bound by the Wi-Fi join, and the tunnel's peer stayed up, so Tailscale didn't add its 95 s rebind (P117 and Plan 2's rebind). What the LAN removes is that rebind after long drops and parks. A long drop wasn't re-measured.
- **The before take-over failed once.** A second take-over over the tailnet, with the link's own SSH session also up, drove the internal minimum to 8116 B. The link dropped, and WireGuard sends failed with `err=-2` (lwIP `ERR_BUF`). See L28.
- **`power boot` `to_live` holds only hibernate wakes.** It showed 44523 or 8178 after normal boots. Normal-boot figures come from the `hib: boot to live:` log line (L27).

## Safety (Step 5)

- **Amber.** `companion-test-local` was blocked (NEEDS YOU) during both LAN check-in boots. Each check-in recorded `needs you`, and `power light` read `1 5 f4 b5 45 (needs you: companion-test-local)`: amber at the 5% dark level. After `claude stop d0000003` the light went back to `0 0 00 80 ff (nobody needs you)`.
- **Five reboots.** All gave `reason=3` (software). `power resets` software went from 62 to 67, and the panic count stayed at 1. The logs have no brownout or panic line, and every boot was live in 8.0–9.0 s.
- **Test sessions.** Only `companion-test-local` (d0000003) was created and used, and it has been stopped. No other session was opened, typed into or stopped. The take-over debug hook refuses any card not named `companion-test-*`.

## Final state

- Mac entry `my-host.local`, fallback 192.168.1.50, Tailscale off.
- Link connected over the LAN, watch on.
- `power set defaults` run.
- Carry source kept (100.64.0.18), in case the entry ever goes back to the tailnet.

## Task 3: fast check-in (2026-10-23/24)

**Method.** Each column is the median over 5 Hibernate check-in boots on USB, run with `power checkin hibernate keep usb chain 4 [exp <mask>]` (L30). Times are ms from the timer wake (RTC-derived), and every boot was timed.
- **Before:** this build's instrumentation on Task 2's code (base).
- **After:** everything kept (F1, the committed build).
- **Raw rows:** in the scratchpad `t3/*-phases.txt`.

| Phase (ms from the wake) | Before | After | Segment before → after |
|---|---|---|---|
| app_main (ROM + WDT reset + bootloader + XIP copy) | 3397 | 3397 | 3397 → 3397 |
| esp_hosted/C6 ready | 5362 | 5362 | 1965 → 1965 |
| Wi-Fi start | 5477 | 5477 | 115 → 115 |
| join issued | 5509 | 5512 | 30 → 30 |
| associated (scan + auth; scan not separable) | 8699 | 8674 | 3185 → 3162 |
| got IP | 9795 | 8724 | 1096 → 33 |
| Host resolved | 10045 | 8725 | 225 → 0 |
| TCP connected | 10334 | 8809 | 192 → 98 |
| SSH kex done | 11012 | 9140 | 684 → 335 |
| auth done | 11540 | 9257 | 438 → 121 |
| poll done | 11936 | 9519 | 463 → 270 |
| judged | 12136 | 9538 | 200 → 20 |
| sleep | **12474** | **9630** | 537 → 98 |

**Awake per check-in: 12.5 s before, 9.6 s after (−2.84 s, −23%).**
- **What Task 2 reported:** `power checkins` showed 8.0–8.4 s for these check-ins. It counts from the app start, so it misses the 3.4 s before it (L30).
- **Energy, estimated** at the brief's 0.7 mWh per awake second:
  - before: 8.7 mWh a check-in, × 96 = **0.84 Wh a day**;
  - after: 6.7 mWh a check-in, × 96 = **0.65 Wh a day**.
- **On battery, estimated (not measured):** Plan 2's 10–12 s source settle used to decide the awake time, so a check-in was about 15.4 s (about 1.03 Wh a day). With L42 the settle lands at about 8 s from the wake, before the poll. A battery check-in should then match USB at about 9.8 s, about 0.66 Wh a day.

| Change | Kept? | Measured effect (sleep median) |
|---|---|---|
| Wi-Fi join cache (BSSID/channel/fast scan) | dropped (L31) | the C6 associates in about 3.1 s either way; the 3 s fallback cost +1.4 s |
| DHCP restore last IP | kept (L32) | about 40 ms alone |
| No lwIP DHCP ARP probe | kept (L32) | about 1.0 s (10639 → 9800) |
| Static address | setting, off (L33) | 58 ms over the restored lease (9742 against 9800) |
| Host address cache | kept (L34) | resolve 142 → 0 ms; the median moved only 24 ms |
| WPA2/WPA3 | nothing to do (L35) | the router is WPA2-only |
| No modem sleep during the check-in | kept (L36) | 0.70 s (10505 → 9800) |
| CPU 360 MHz | already (L37) | — |
| Wi-Fi before the display | dropped (L38) | none (9815 against 9800) |
| Skip validation after deep sleep | dropped (L39) | none: the wake is a WDT reset, and validation still costs 1.43 s |
| 20 ms waits, fast way down | kept (L40) | 0.74 s (10539 → 9800) |
| Close SSH before the sleep | kept (L41) | about 0; stops orphaned Host sessions |
| Battery settle from 3 samples | kept (L42) | unmeasurable on USB; estimated about 5.6 s on battery |

**What is left.**
- About 3.4 s before `app_main`: 0.67 s pre-reset, 1.43 s validation, 1.0 s XIP copy and 0.17 s PSRAM tuning.
- About 2 s of C6 boot.
- About 3.1 s of C6 association.
- All three are outside what this task may change: the C6 can't be flashed, and skipping validation outright is ruled out by P125. Keeping the C6 powered and associated is Task 4's spike. Quad-I/O flash would roughly halve the validation and XIP copy, but it needs the flash part confirmed first (`spi_flash: detected chip: generic`).

**Safety.**
- No NVS erase and no C6 flash.
- No secret was logged or stored: the join cache held no PSK, and the code is gone anyway.
- Every deep sleep had a timer (the 60 s test timer).
- Only the Tab5's own orphaned sshd sessions were ended (L41).
- No `companion-test-*` session was needed.
- Final state:
  - Host entry `my-host.local`, fallback 192.168.1.50, `ts off`;
  - `power set defaults`;
  - static address off;
  - experiment mask 0;
  - link connected over the LAN, watch on.
