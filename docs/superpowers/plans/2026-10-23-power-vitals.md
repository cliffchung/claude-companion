# Power & Vitals Implementation Plan (Plan 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Tab5 dims and turns its screen off on its own, wakes instantly on a key, touch, lift or a session that needs you (without the waking input acting), keeps the Mac link live through a long poll, shows battery, charging, Wi-Fi, Tailscale, link and the right local time, and lights the keyboard amber while a session needs you. All of it is measured on battery in one visit with the user.

**Architecture:** A new `sys/power.c` owns one state machine (Active → Dim → Off → Radio off) and the keyboard light. Its decisions are pure functions in `sys/power_rules.c`, and the battery and clock maths are pure functions in `sys/vitals.c`, all host-tested. `cc_link` replaces the fixed poll with one long-lived pty exec channel. That channel runs a fixed `/bin/sh` loop built by `cc_cmd_watch`, and a pure marker parser (`companion/cc_watch.c`) reads its output. `wifi_mgr` gains a reconnect task and a parked state for the Radio-off tier. The light-sleep spike, Radio off and the light's Off cap are built behind settings that default off. One battery session with the user sets them from INA226 numbers logged on the device.

**Tech Stack:** C, ESP-IDF v5.5.6 (`esp32p4`), LVGL 9.4 + esp_lvgl_port 2.8.0~1, BSP m5stack_tab5 1.3.2, esp_hosted 3.0.10 / esp_wifi_remote, libssh 0.11, MicroLink, `espressif/bmi270` 1.1.0 (Apache-2.0), cJSON, `esp_console`, Unity host tests, Python 3 (`pty`, `unittest`, pyserial).

**Spec:** `docs/superpowers/specs/2026-10-23-power-vitals-design.md` (this plan's spec), under `docs/superpowers/specs/2026-10-22-claude-tab5-design.md` (binding overall). Read both. Also read `docs/superpowers/notes/2026-10-22-plan1-rulings.md` and `docs/superpowers/notes/2026-10-22-couch-path-check.md`.

## Global Constraints

- **Worktree:** all work happens in `/Users/you/workspace/claude-companion-plan2` on branch `plan2-power-vitals`. Commit there, task by task (as Plan 1's R1). Never touch the top-level checkout, which stays on `main`.
- **Build:** `. ~/esp/esp-idf/export.sh && cd firmware && idf.py set-target esp32p4 && idf.py build` the first time (the worktree has no build yet), then `idf.py build`. `firmware/sdkconfig` is generated and git-ignored: after editing `sdkconfig.defaults`, delete `firmware/sdkconfig` before building.
- **Flash:** `idf.py -p /dev/cu.usbmodem211201 flash`. It keeps NVS.
- **Host tests:** `export IDF_PATH=~/esp/esp-idf && cmake -S firmware/test/host -B firmware/test/host/build && cmake --build firmware/test/host/build && ctest --test-dir firmware/test/host/build --output-on-failure`.
- **Python tests:** `python3 -m unittest discover -s firmware/tools -p 'test_*.py' -v`.
- **Console helper:** `T5=/tmp/tab5.py`, run as `python3 $T5 "cmd" …`. It reads for 0.6 s after each line. Its `--shot` writes into Plan 1's directory, so take screenshots with `python3 firmware/tools/screenshot.py --out $SHOTS/<name>.jpg`, where `SHOTS=/tmp/plan2-shots`. Screenshots stay out of git unless they show only `companion-test-*` sessions (Plan 1 R4/R25). Look at every screenshot you take.
- Existing console commands: companion, key (`key <name>`, `key text …`, `key ctrl <key>`), logs, reboot, reply, screenshot, sessions, sshkey, ui, wifi (`wifi leave` / `wifi join`).
- **Test only against sessions you create**, named `companion-test-*`. Never reply to, type into or stop any other session on the user's Mac. Stop your test sessions when the task is done (`~/.local/bin/claude stop <id>`).
- **No secrets in the repo or the logs:** SSH private keys, Tailscale auth keys and Wi-Fi passwords stay on the device.
- **No GPL code.** libssh (LGPL-2.1) stays. New third-party code is MIT/BSD/Apache only. Don't copy generative-tab5's unlicensed `accel_gyro_bmi270` wrapper.
- Keep `firmware/LICENSE.clientre5` and the attribution line `Based on Clientre 5 by Mozgi512 (MIT)`.
- **Never open a login shell on the Mac and never touch the user's tmux.** Companion channels are pty exec channels with an explicit command.
- **Every command line sent to the Mac comes from a `cc_cmd_*` function** and validated parts (the Claude path, session ids, fixed integers). Never use an ad-hoc `snprintf`.
- **Never flash or update the C6, never erase NVS, never run `sshkey regenerate`.**
- **Never change sshd config** (`ClientAliveInterval` and the rest stay as they are).
- **Amber `#f4b545` means NEEDS YOU only**, on the screen and on the keyboard light. Low battery is red, never amber.
- **D9 colours only:** bg `#0e1116`, panel `#161b22`, panel-2 `#1b212b`, line `#2a323d`, fg `#e9e7e1`, muted `#8a94a2`, amber `#f4b545`, green `#54d08a`, red `#ef6f6f`.
- **No cc_link calls from subscriber callbacks** (`cc_poll_subscribe`). A callback copies what it needs and notifies its own task.
- **A wake input never reaches the Mac or acts on a card.**
- **The long-poll loop leaves nothing running on the Mac.**
- New pure modules (`sys/power_rules.c`, `sys/vitals.c`, `sys/plog.c`, `sys/lift_detect.c`, `companion/cc_watch.c`) include no ESP-IDF headers and also build in `firmware/test/host`.
- Match Clientre5's C style: 4-space indent, function brace on its own line, `static const char *TAG`, `s_` statics, `/* */` comments.
- Every gated feature (light sleep, Radio off, the light's Off cap) is built behind its setting with the default off before Task 10. Nothing waits on the measurement.
- Record each judgment call as a ruling in `docs/superpowers/notes/2026-10-23-plan2-rulings.md`, in Plan 1's format (`Ruling Pn: … — why — cost if wrong`).

## Review Focus

1. **A held key or key repeat while the device wakes** → nothing reaches the dashboard; the first key after the device is awake works normally. Covered by Task 5 `test_swallow_held_key_repeats` and the Step 6 `key` check.
2. **The Mac sleeps with the watch loop running** (the loop is suspended, then resumes with a big `$SECONDS` jump and no keepalives) → the loop exits on resume and leaves nothing behind. Covered by Task 4 `test_suspended_loop_exits_on_resume` (SIGSTOP/SIGCONT) and the Task 10 `ps` check after a real Mac sleep.
3. **No battery fitted, or the INA226 not found** → the bar shows no percentage (never a red 0%), and Radio off never engages, because "on battery" is false. Covered by Task 2 `test_battery_unknown_not_low` and Task 5 `test_radio_off_needs_battery`.
4. **Radio off with check-ins up to `checkin_min` apart** → the amber keyboard light is not dropped between check-ins by the 5-minute staleness rule. Covered by Task 3 `test_light_age_limit_follows_checkin`.
5. **A keyboard colour saved before this plan as gold or orange, or typed as amber hex** → never shows on the keyboard; it is reset to the default blue on load and refused on entry. Covered by Task 3 `test_amber_like_colours` and the Step 7 hex-entry check.

---

### Task 1: Power instrumentation, reset reasons and boot rails

**Files:**
- Create: `firmware/main/sys/plog.c`, `firmware/main/sys/plog.h` (pure ring and summaries)
- Create: `firmware/main/sys/power_log.c`, `firmware/main/sys/power_log.h` (sampling, NVS summaries, reset reasons)
- Create: `firmware/main/sys/power_bench.c`, `firmware/main/sys/power_bench.h`
- Create: `firmware/main/sys/power_console.c` (the `power` command)
- Create: `firmware/tools/power_report.py`, `firmware/tools/test_power_report.py`
- Create: `firmware/test/host/test_plog.c`
- Modify: `firmware/test/host/CMakeLists.txt` (add a `pure` library with `../../main/sys/plog.c`, later tasks add their pure files to it; add `test_plog`)
- Modify: `firmware/main/sys/sysinfo.c`, `sysinfo.h` (expose the raw charge pin)
- Modify: `firmware/main/app_main.c` (reset reason at boot, staggered rails, the idle task samples the log)
- Modify: `firmware/sdkconfig.defaults` (brownout)

**Interfaces:**
- Produces, in `plog.h`:

```c
typedef struct { uint32_t t_s; uint16_t mv; int16_t ma; uint16_t mw; uint8_t tag; uint8_t flags; } plog_sample_t;
/* tag: power state 0..3 (Task 5), 0x10+n for bench step n; flags bit0 = charge pin low, bit1 = pin read OK */
typedef struct { plog_sample_t *buf; uint32_t cap, head, count; } plog_ring_t;
void plog_init(plog_ring_t *r, plog_sample_t *buf, uint32_t cap);
void plog_push(plog_ring_t *r, const plog_sample_t *s);           /* overwrites the oldest when full */
bool plog_get(const plog_ring_t *r, uint32_t i, plog_sample_t *out); /* i = 0 is the oldest */
typedef struct { uint32_t n; int64_t sum_mw, sum_ma; uint16_t min_mw, max_mw; uint32_t first_t, last_t; } plog_sum_t;
void plog_sum_add(plog_sum_t *s, const plog_sample_t *x);
uint32_t plog_sum_avg_mw(const plog_sum_t *s);                    /* 0 when empty */
uint32_t plog_sum_mwh_x1000(const plog_sum_t *s);                 /* avg mW x seconds covered / 3.6 */
```

- Produces, in `power_log.h`: `void power_log_boot(void);` (logs `reset reason: <name>` and adds 1 to that reason's counter in NVS ns `power`, key `rst`); `void power_log_sample(uint8_t tag);` (called each second by the idle task while logging is on); `bool power_log_on(void); void power_log_enable(bool on);` (65 536-sample PSRAM ring, about 18 h; per-tag `plog_sum_t` saved to NVS key `sum` every 60 s while on); `void power_log_dump(void);` (prints `@@PLOG <n>`, then one CSV line `t_s,mv,ma,mw,tag,flags` per sample, then `@@END`); `void power_log_clear(void);`; `void power_log_resets(void);`.
- Produces, in `power_bench.h`:

```c
typedef struct { const char *name; uint16_t seconds; bool (*enter)(void); void (*leave)(void); } bench_step_t;
/* enter() returns false when the feature isn't built yet: the step is reported "skipped: not built". */
void power_bench_start(bool fast);   /* fast: on USB, 3 s per step, no battery wait (scaffold check) */
void power_bench_abort(void);
void power_bench_report(void);       /* @@BENCH <n>, then "name,seconds,avg_mw,min_mw,max_mw,avg_ma,mwh_x1000,status", then @@END */
```
  The step table lives in `power_bench.c`. Task 1 adds `settle` (60 s) and `active` (120 s). Tasks 3, 5, 6 and 8 add their own steps (listed there). A real run waits until `sysinfo_power().src == PWR_BATTERY` for 5 s, then runs the steps in order. When it finishes it returns to Active and shows the toast `Bench done. Plug USB back in.` It keeps its summaries in NVS key `bench`, so they survive a brownout reset.
- Produces, in `sysinfo.h`: `int sysinfo_charge_pin(void);` (-1 unreadable, 0 low = charging per the fork's guess, 1 high).
- Console `power`:
  - `power` prints one line: `power: 7.62 V -412 mA 3.14 W 52% charging=yes src=usb-charging chg_pin=0 state=active idle=12s holds=none`. `state`, `idle` and `holds` print `-` until Task 5.
  - `power log on|off|status|dump|clear`, `power resets [clear]`, `power bench start [fast]|status|report|abort`.
- `power_report.py`:
  - `parse_dump(lines) -> list[dict]` and `parse_bench(lines) -> list[dict]`. Both ignore lines outside the markers and raise `ValueError` on a missing `@@END` or a count mismatch.
  - `summarize(samples) -> dict[tag, {"n", "avg_mw", "avg_ma", "mwh"}]`.
  - `estimate_hours(capacity_mwh, mix: dict[tag, fraction], summary) -> float`.
  - CLI: `power_report.py --port PORT (--log | --bench) [--capacity-mwh N]`. It sends the dump command, waits up to 60 s for `@@END`, and prints a table plus `estimated battery life: N h (estimate)`.

- [ ] **Step 1: Write the failing host tests** in `firmware/test/host/test_plog.c`
  - `test_ring_wraps_oldest_first`: cap 4, push t = 1..6 → count 4, `plog_get(0)` is t = 3 and `plog_get(3)` is t = 6, `plog_get(4)` returns false.
  - `test_sum_avg_and_energy`: three samples of 1000, 2000 and 3000 mW at t = 10, 11, 12 → avg 2000, min 1000, max 3000; `plog_sum_mwh_x1000` = 2000 × 3 s / 3.6 = 1 666 666 (±1).
  - `test_sum_empty`: avg 0, mWh 0.

- [ ] **Step 2: Run them to make sure they fail**

Run the host test command.
Expected: FAIL (undefined `plog_*`)

- [ ] **Step 3: Implement `plog.c`** and run the host tests until they pass

- [ ] **Step 4: Write the failing Python tests** in `firmware/tools/test_power_report.py`
  - `test_parse_dump_ignores_noise`: log lines around `@@PLOG 2` with two CSV rows → 2 dicts with integer fields.
  - `test_parse_dump_count_mismatch`: header 3, two rows → `ValueError`.
  - `test_summarize_per_tag`: samples tagged 0 and 2 → separate averages.
  - `test_estimate_hours`: capacity 14 800 mWh, mix {0: 0.1, 2: 0.9}, avg 3000 and 1000 mW → 14 800 / (300 + 900) = 12.33 h.

- [ ] **Step 5: Implement `power_report.py`** and run the Python tests until they pass

- [ ] **Step 6: Implement the device side**
  - `power_log.c`, `power_bench.c` and `power_console.c`.
  - `sysinfo_charge_pin`.
  - In `app_main`, call `power_log_boot()` right after `settings_init()`.
  - Stagger the rails: wait `vTaskDelay(pdMS_TO_TICKS(50))` between the four `bsp_feature_enable` calls (LCD, touch, Wi-Fi, USB), keeping their order.
  - Add `CONFIG_ESP_BROWNOUT_DET=y` and `CONFIG_ESP_BROWNOUT_DET_LVL_SEL_7=y` (2.6 V, the P4's highest threshold and its default, now explicit) to `sdkconfig.defaults` with a comment.
  - Delete `firmware/sdkconfig`, then build and flash.

Expected: `Project build complete`. The boot log has `reset reason: <name>`, and the BSP line `Discovered board version N (…)`. Record N in the rulings file: Tasks 5 and 6 depend on it.

- [ ] **Step 7: Check on hardware**

Run: `python3 $T5 "power" "power resets"`
Expected: volts between 6.0 and 8.5, `src=usb…`, `chg_pin=0` or `1`, and a resets table with at least one entry.

Run: `python3 $T5 "power log on"`. Wait 15 s, then run `python3 firmware/tools/power_report.py --port /dev/cu.usbmodem211201 --log`.
Expected: at least 14 samples, all with tag 0.

Run: `python3 $T5 "power bench start fast"`. Wait 10 s, then run `python3 firmware/tools/power_report.py --port /dev/cu.usbmodem211201 --bench`.
Expected: `settle` and `active` rows with status `ok`.

Run: `python3 $T5 "reboot"`. Wait 15 s, then run `python3 $T5 "power resets"`.
Expected: the `software` count went up by 1.

Run: `python3 $T5 "power log off" "power log clear"`.

- [ ] **Step 8: Commit**

```bash
git add firmware/main/sys firmware/main/app_main.c firmware/sdkconfig.defaults firmware/test/host firmware/tools/power_report.py firmware/tools/test_power_report.py docs/superpowers/notes/2026-10-23-plan2-rulings.md
git commit -m "feat(power): power console, on-device power log and bench, reset-reason counter, staggered rails"
```

### Task 2: Settings fields, local time and the vitals bar

**Files:**
- Create: `firmware/main/sys/vitals.c`, `firmware/main/sys/vitals.h` (pure)
- Create: `firmware/main/ui/ui_vitals.c`, `firmware/main/ui/ui_vitals.h` (header widgets)
- Create: `firmware/test/host/test_vitals.c`
- Modify: `firmware/main/settings/settings.h`, `settings.c` (every Plan 2 field, `CFG_VERSION 18`)
- Modify: `firmware/main/sys/sysinfo.c` (TZ rule; curve, EMA and load compensation)
- Modify: `firmware/main/sys/power_console.c` (`clock` command)
- Modify: `firmware/main/ui/ui_dashboard.c` (header uses `ui_vitals`), `firmware/main/ui/ui_settings.c` (drop the UTC offset row; the Time zone row arrives in Task 5)
- Modify: `firmware/tools/ui_preview/preview.c`, `firmware/tools/ui_preview/CMakeLists.txt` (stubs, `vitals.c`, `ui_vitals.c`, env-driven vitals)
- Modify: `firmware/test/host/CMakeLists.txt` (add `vitals.c` and `test_vitals`)

**Interfaces:**
- Produces, appended to `app_settings_t` (after `claude_path`; older blobs load with these defaults):

```c
    char     tz_rule[48];        /* POSIX TZ, default "PST8PDT,M3.2.0,M11.1.0" */
    uint16_t pw_dim_s;           /* 30 */
    uint16_t pw_off_s;           /* 120 */
    uint8_t  pw_sleep_on_usb;    /* 1: dim and screen off also on USB */
    uint8_t  pw_wake_lift;       /* 1 */
    uint8_t  pw_wake_needs_you;  /* 1 */
    uint8_t  kbd_needs_light;    /* 1: keyboard light shows needs-you */
    uint8_t  kbd_off_cap;        /* 100 = no cap; gated, set in Task 10 */
    uint8_t  pw_radio_off;       /* 0; gated, set in Task 10 */
    uint16_t pw_radio_off_min;   /* 20 */
    uint16_t pw_checkin_min;     /* 5 */
    uint8_t  pw_light_sleep;     /* 0; gated (Task 6, Task 10) */
```
  - `utc_offset_min` becomes reserved.
  - When migrating from version 17 or older, `tz_rule` takes the Pacific default, whatever the old offset was.
  - `settings_init` resets an invalid `tz_rule` (`!vitals_tz_valid`) to the default.
  - It also clamps `pw_dim_s` to 10–600 and `pw_off_s` to `pw_dim_s + 10`–3600.
- Produces, in `vitals.h`:

```c
float vitals_ocv(float bus_v, float current_a, float r_ohm);   /* bus_v + current_a * r_ohm (+ = discharging) */
int   vitals_percent_from_ocv(float pack_v);                   /* 2S curve below, clamped 0..100 */
typedef struct { float ema_v; bool init; } vitals_batt_t;
int   vitals_batt_update(vitals_batt_t *b, float bus_v, float current_a, float dt_s);
/* load-compensate with VITALS_R_INT_OHM (0.15, an estimate), EMA with tau 30 s
   (alpha = dt / (30 + dt)), first sample seeds the EMA; returns the percent */
#define VITALS_R_INT_OHM 0.15f
typedef enum { VB_NONE, VB_NORMAL, VB_LOW } vitals_bat_level_t;
vitals_bat_level_t vitals_bat_level(int percent, bool charging);  /* -1 → NONE; <= 15 and not charging → LOW */
int   vitals_wifi_bars(bool connected, int rssi);   /* 0 if not connected; >= -55: 4, >= -67: 3, >= -75: 2, else 1 */
bool  vitals_tz_valid(const char *rule);            /* 1..47 chars of [A-Za-z0-9,.:/+<>-], starts with a letter or '<' */
typedef struct { const char *label; const char *rule; } vitals_tz_t;
extern const vitals_tz_t vitals_tz_presets[];       /* NULL-terminated, US Pacific first */
```
  - Per-cell curve as (V, %): (4.20, 100), (4.10, 90), (4.00, 80), (3.92, 70), (3.85, 60), (3.79, 50), (3.74, 40), (3.70, 30), (3.66, 20), (3.60, 10), (3.50, 5), (3.30, 0). The pack value is 2× the cell value. Interpolate linearly between points.
  - Presets:
    - US Pacific `PST8PDT,M3.2.0,M11.1.0`
    - US Mountain `MST7MDT,M3.2.0,M11.1.0`
    - Arizona `MST7`
    - US Central `CST6CDT,M3.2.0,M11.1.0`
    - US Eastern `EST5EDT,M3.2.0,M11.1.0`
    - UTC `UTC0`
    - UK `GMT0BST,M3.5.0/1,M10.5.0`
    - Central Europe `CET-1CEST,M3.5.0,M10.5.0/3`
    - Japan `JST-9`
    - Australia East `AEST-10AEDT,M10.1.0,M4.1.0/3`
- Produces, in `sysinfo.h`:
  - `sysinfo_apply_timezone()` now sets `TZ` from `g_settings.tz_rule`.
  - `power_info_t.percent` comes from `vitals_batt_update`, with dt measured by `esp_timer`.
  - New: `int sysinfo_rssi_cached(void);`. It calls `wifi_mgr_rssi()` at most every 10 s, because that is an SDIO RPC.
- Produces, in `ui_vitals.h`: `lv_obj_t *ui_vitals_create(lv_obj_t *parent);` and `void ui_vitals_update(lv_obj_t *bar);`.
  - Left to right: battery glyph + `NN%`, a charging bolt, Wi-Fi bars, the Tailscale mark, the link dot + host, and the clock `HH:MM`.
  - The battery glyph is a 28×14 outline (muted, 2 px) with a 3×6 nub and a fill whose width is the percent. The fill and the % are fg; both are red at `VB_LOW`. At `VB_NONE` the glyph and % are hidden.
  - The bolt is an `lv_line` zigzag in fg, shown only while charging.
  - The Wi-Fi bars are 4 bars, 4 px wide and 5, 9, 13 and 17 px tall: fg when lit, line colour when not.
  - The Tailscale mark (`ui_tailscale_icon`) is green when connected, muted while connecting or registering, and red on error. It is hidden when Tailscale is disabled.
  - The dashboard's existing link dot and host logic moves in unchanged. The dashboard calls `ui_vitals_update` every 0.9 s, as `update_vitals` does today.
- Console `clock`:
  - `clock` prints `clock: 2026-10-23 14:05:09 PDT tz=PST8PDT,M3.2.0,M11.1.0`.
  - `clock tz <rule>` checks `vitals_tz_valid`, saves and applies the rule.
  - `clock at <epoch>` prints that instant in local time, in the same format.

- [ ] **Step 1: Write the failing host tests** in `test_vitals.c`
  - `test_curve_points`: 8.40 → 100; 7.58 → 50; 6.60 → 0; 7.50 → 42 (±1); 9.00 → 100; 5.00 → 0.
  - `test_curve_monotonic`: from 6.5 to 8.5 V in 0.01 V steps the percent never falls.
  - `test_load_compensation`: `vitals_ocv(7.40, 1.0, 0.15)` = 7.55; charging `vitals_ocv(8.10, -0.5, 0.15)` = 8.025.
  - `test_ema_tau`: seed at 7.40 V (0 A), then 30 samples at 8.00 V with dt 1 → `ema_v` within 7.40 + 0.6 × 0.63 ± 0.03.
  - `test_battery_unknown_not_low` (Review Focus 3): `vitals_bat_level(-1, false)` = `VB_NONE`; `(15, false)` = `VB_LOW`; `(15, true)` = `VB_NORMAL`; `(16, false)` = `VB_NORMAL`.
  - `test_wifi_bars`: (false, -40) → 0; (true, -55) → 4; (true, -56) → 3; (true, -67) → 3; (true, -75) → 2; (true, -90) → 1.
  - `test_tz_valid`: every preset is valid; `""`, `"PST8PDT;rm"`, `"x y"` and a 48-character string are not.
  - `test_tz_dst_end_2026`: with `setenv("TZ", "PST8PDT,M3.2.0,M11.1.0")` + `tzset`, `localtime_r(1793523599)` is 01:59:59 with `tm_isdst` 1, and `localtime_r(1793523600)` is 01:00:00 with `tm_isdst` 0 (2026-11-01 fall-back). This checks the rule string with the host libc; Step 6 checks newlib on the device.

- [ ] **Step 2: Run them to make sure they fail**

Run the host test command.
Expected: FAIL (undefined `vitals_*`)

- [ ] **Step 3: Implement `vitals.c`** and run the host tests until they pass

- [ ] **Step 4: Implement the settings fields, the `sysinfo` changes, `clock`, `ui_vitals` and the dashboard header, plus the preview stubs**

The preview reads `PREVIEW_BAT` (percent, `-1` for none), `PREVIEW_CHG` (0/1), `PREVIEW_RSSI` and `PREVIEW_TS` (0 off, 1 connecting, 2 connected) from the environment. The defaults are 52, 0, -60 and 2.

- [ ] **Step 5: Check the header in the preview**

Build the preview (`firmware/tools/ui_preview/README.md`) and render `./preview dashboard` four times:
1. the defaults;
2. `PREVIEW_BAT=12 PREVIEW_CHG=0`;
3. `PREVIEW_BAT=12 PREVIEW_CHG=1 PREVIEW_RSSI=-80`;
4. `PREVIEW_BAT=-1 PREVIEW_TS=1`.

Convert each with `sips` and look at it.
Expected:
- (1) shows a half-full glyph, 3 bars and a green Tailscale mark;
- (2) shows a red glyph and a red `12%`, with no amber anywhere;
- (3) shows a fg `12%`, the bolt and 1 bar;
- (4) shows no battery glyph and a muted mark.

- [ ] **Step 6: Build, flash and check on hardware**

Run: `python3 $T5 "clock" "clock at 1793523599" "clock at 1793523600" "clock at 1772964000"`
Expected:
- `clock` shows the Mac's local time (compare `date`) with `PDT`;
- the next two lines are `2026-11-01 01:59:59 PDT` and `2026-11-01 01:00:00 PST`;
- the last one is `2026-03-08 03:00:00 PDT` (spring-forward).

Run: `python3 $T5 "clock tz bad;rule" "clock"`
Expected: rejected; the rule is unchanged.

Run: `python3 $T5 "ui home"`, then take a screenshot.
Expected: the header shows the battery glyph and %, the Wi-Fi bars, the Tailscale mark, a green link dot and the right clock. A settings blob saved by Plan 1 loaded without losing the Mac or the Claude path (`companion status` shows `connected <host>`).

- [ ] **Step 7: Commit**

```bash
git add firmware/main firmware/test/host firmware/tools/ui_preview
git commit -m "feat(vitals): POSIX time zone, battery curve with EMA and load compensation, full vitals bar"
```

### Task 3: Keyboard light owner

**Files:**
- Create: `firmware/main/sys/power_rules.c`, `firmware/main/sys/power_rules.h` (pure; Task 5 extends it)
- Create: `firmware/main/sys/power.c`, `firmware/main/sys/power.h` (Task 3 creates the module with the light owner only; Task 5 adds the states)
- Create: `firmware/test/host/test_power_rules.c`
- Modify: `firmware/main/input/keyboard.c`, `keyboard.h` (the LED source getter; hot-plug)
- Modify: `firmware/main/ui/ui_settings.c` (drop the gold and orange presets; refuse amber hex; route LED changes through the owner; the "Keyboard light shows needs-you" switch)
- Modify: `firmware/main/settings/settings.c` (reset an amber-like saved colour)
- Modify: `firmware/main/sys/power_bench.c` (steps `off-light-amber`, 90 s, and `off-light-off`, 90 s; their `enter` returns false until Task 5 provides Off)
- Modify: `firmware/main/companion/cc_link.c`, `cc_link.h` (`cc_poll_last_ok_ms`), `firmware/main/sys/power_console.c` (`power light`), `firmware/main/app_main.c` (`power_init`)
- Modify: `firmware/test/host/CMakeLists.txt`

**Interfaces:**
- Produces, in `power_rules.h`:

```c
typedef struct { uint8_t mode, bright, r, g, b; } pr_led_t;   /* mode 0 = bind, 1 = user colour */
bool pr_rgb_amber_like(uint32_t rgb);   /* HSV hue 25..55 deg, saturation >= 0.5, value >= 0.5 */
bool pr_any_needs_you(const cc_session_t *s, int n);   /* background && live && CC_NEEDS_YOU */
pr_led_t pr_kbd_light(const pr_led_t *user, bool setting_on, bool needs_you,
                      int64_t result_age_ms, int64_t max_age_ms, bool screen_off, int off_cap_pct);
/* needs_you && setting_on && result_age_ms < max_age_ms → {1, b, 0xf4, 0xb5, 0x45} with
   b = max(user->bright, 30), then min(b, max(off_cap_pct, 30)) when screen_off; else *user */
int64_t pr_light_max_age_ms(bool radio_off, int checkin_min);
/* 300 000, or max(300 000, (checkin_min + 1) * 60 000) in Radio off */
```
- Produces, in `power.h`:
  - `void power_init(void);` starts the `power` task (stack 4096, priority 3).
  - `void power_kbd_light_refresh(void);` re-evaluates the light now. Settings calls it after a change.
  - `void power_kbd_light_get(pr_led_t *out);` gives the current colour, for hot-plug.
  - The owner subscribes with `cc_poll_subscribe`. Its callback stores `pr_any_needs_you` and the result time under the module's lock, then notifies the power task; the task calls `keyboard_set_led`.
- Produces, in `keyboard.h`: `void keyboard_set_led_source(void (*get)(uint8_t *mode, uint8_t *bright, uint8_t *r, uint8_t *g, uint8_t *b));`. `kb_probe` asks the source for the colour instead of reading `g_settings`, so a keyboard plugged in later picks up the current state.
- Consumes: `cc_poll_subscribe` (Plan 1). It adds `int64_t cc_poll_last_ok_ms(void);` to `cc_link.h`: the `esp_timer` ms of the last successful result, or 0. Task 4 keeps it current for watch blocks too.
- Console:
  - `power light` prints the owner's choice as `light: <mode> <bright> <r> <g> <b> (<reason>)`, where the reason is `needs you: <name>`, `nobody needs you`, `setting off` or `result too old`.
  - Debug subcommands: `power light test bind` writes mode 0 at brightness 60; `power light test user` restores the owner's choice; `power light test reprobe` marks the keyboard absent so the task re-probes it as on a hot-plug.

- [ ] **Step 1: Write the failing host tests** in `test_power_rules.c`
  - `test_amber_like_colours` (Review Focus 5): true for 0xF4B545, 0xFFD700 and 0xFF8000; false for 0x0080FF, 0xFFFFFF, 0xFF4040, 0x40FF40, 0xFF40FF, 0x00FFFF and 0x806020 (dark).
  - `test_light_needs_you_amber`: user {0, 20, 0, 128, 255}, setting on, needs_you, age 1 s → {1, 30, f4, b5, 45}. With user brightness 0 → brightness 30. With user brightness 80 → 80.
  - `test_light_user_when_clear`: needs_you false → exactly the user's struct; setting off → user's.
  - `test_light_stale_result_reverts`: age 300 001 ms, max 300 000 → user's.
  - `test_light_off_cap`: user brightness 80, screen_off, cap 40 → 40; cap 10 → 30.
  - `test_light_age_limit_follows_checkin` (Review Focus 4): `pr_light_max_age_ms(false, 5)` = 300 000; `(true, 5)` = 360 000; `(true, 30)` = 1 860 000.
  - `test_any_needs_you`: a blocked interactive session doesn't count; a blocked stopped (`live` false) one doesn't; a blocked live background one does.

- [ ] **Step 2: Run them to make sure they fail**

Run the host test command.
Expected: FAIL

- [ ] **Step 3: Implement `power_rules.c`** and run the host tests until they pass

- [ ] **Step 4: Implement the owner, the keyboard source, the settings changes and the migration**
  - The presets become `{ 0x0080FF, 0xFFFFFF, 0xFF4040, 0x40FF40, 0xFF40FF, 0x00FFFF }`.
  - The colour-entry path refuses an amber-like value with the toast `Amber is reserved for sessions that need you`.
  - `settings_init` replaces an amber-like saved `kbd_led_rgb` with 0x0080FF.
  - In Task 3, `screen_off` is always false.
  - `app_main` calls `power_init()` after `cc_link_start()`.
  - Build and flash.

- [ ] **Step 5: Probe what mode 0 ("bind") shows**

Run: `python3 $T5 "power light test bind"`, then `python3 $T5 "power light test user"`.
Expected: both print `ok`. Nobody can see the keyboard yet, so record "mode 0: not observed yet" in the rulings file; Task 10 Step 3 has the user look at it.

- [ ] **Step 6: Check needs-you on hardware**

On the Mac, run `~/.local/bin/claude --bg --name companion-test-light "Ask me what my favorite color is, then wait for my answer."`. Wait until `python3 $T5 "sessions"` lists it as `NEEDS_YOU`.

Run: `python3 $T5 "power light"`. This prints the owner's current `mode bright r g b` and why.
Expected: `light: 1 30 f4 b5 45 (needs you: companion-test-light)` (brightness 30, or the user's if higher).

Run: `python3 $T5 "reply <its id> green"`, wait for the next poll, then run `python3 $T5 "power light"`.
Expected: the user's setting, reason `nobody needs you`.

Hot-plug: start a fresh `companion-test-light2` (same prompt), wait for it to block, then run `python3 $T5 "power light test reprobe"`.
Expected: after the re-probe, `power light` still shows amber.

Stop the test session.

- [ ] **Step 7: Check the colour rules**

Open Settings → the keyboard light colour (navigate with `key`) and take a screenshot.
Expected: no gold or orange preset. If there is a hex-entry row, entering `F4B545` shows the refusal toast (take a screenshot).

- [ ] **Step 8: Commit**

```bash
git add firmware/main firmware/test/host
git commit -m "feat(power): keyboard light owner shows needs-you in amber; drop amber-like presets"
```

### Task 4: Long poll

**Files:**
- Create: `firmware/main/companion/cc_watch.c`, `firmware/main/companion/cc_watch.h` (pure parser)
- Create: `firmware/test/host/test_cc_watch.c`, `firmware/test/host/cc_cmd_print.c` (prints `cc_cmd_watch` output for the Python test)
- Create: `firmware/tools/test_watch_loop.py`
- Modify: `firmware/main/companion/cc_cmd.c`, `cc_cmd.h` (`cc_cmd_watch`), `firmware/test/host/test_cc_cmd.c`
- Modify: `firmware/main/companion/cc_link.c`, `cc_link.h` (watch channel, liveness, fallback, new getters)
- Modify: `firmware/main/companion/cc_console.c` (`companion watch …`)
- Modify: `firmware/main/ui/ui_dashboard.c` (take-over sets rate 15 instead of 0)
- Modify: `firmware/test/host/CMakeLists.txt` (add `cc_watch.c`, `test_cc_watch` and the `cc_cmd_print` executable)

**Interfaces:**
- Produces, in `cc_cmd.h`:

```c
typedef struct { int interval_s, heartbeat_s, quiet_s, life_s; } cc_watch_params_t;
/* ranges: interval 1..120, heartbeat 1..600, quiet 2..600, life 5..7200 */
bool cc_cmd_watch(const char *claude, const cc_watch_params_t *p, char *out, size_t cap);
#define CC_WATCH_KEEPALIVE_S 20     /* K: the Tab5 writes "\n" this often */
/* product values: interval 3 (Active/Dim) or 15 (Off), heartbeat 60, quiet 3K = 60, life 1800 */
```
  The exact output for `("~/.local/bin/claude", {3, 60, 60, 1800})` is the line below. It was verified against this Mac's `/bin/sh` (bash 3.2). Bash 3.2's `read -t` returns 1 on both a timeout and EOF, so EOF is detected as a read that returns before its deadline. `</dev/null` keeps `claude` from eating keepalives, and `stty -echo` stops the pty echoing them back.

```
exec /bin/sh -c 'stty -echo 2>/dev/null; C=~/.local/bin/claude; I=3; H=60; T=60; L=1800; echo "@@CC-WATCH 1"; p=; s=0; k=0; while [ "$SECONDS" -lt "$L" ]; do o=$("$C" agents --json --all </dev/null 2>&1); r=$?; if [ "$o" != "$p" ]; then echo "@@CC-BEGIN $r"; printf "%s\n" "$o"; echo "@@CC-END"; p=$o; s=$SECONDS; elif [ $((SECONDS-s)) -ge "$H" ]; then echo "@@CC-HB $SECONDS"; s=$SECONDS; fi; e=$((SECONDS+I)); while [ "$SECONDS" -lt "$e" ]; do if read -r -t $((e-SECONDS)) x; then k=$SECONDS; elif [ "$SECONDS" -lt "$e" ]; then echo "@@CC-BYE eof"; exit 0; fi; done; if [ $((SECONDS-k)) -ge "$T" ]; then echo "@@CC-BYE quiet"; exit 0; fi; done; echo "@@CC-BYE life"'
```

- Produces, in `cc_watch.h`:

```c
typedef enum { CC_WATCH_NONE, CC_WATCH_BANNER, CC_WATCH_BLOCK, CC_WATCH_HEARTBEAT,
               CC_WATCH_BYE, CC_WATCH_OVERFLOW } cc_watch_ev_t;
typedef struct {
    char *body; size_t cap, len;   /* caller's buffer (the 256 KB poll buffer); NUL-terminated at BLOCK */
    int rc;                        /* BLOCK: the loop's exit status for that run */
    char bye[16];                  /* BYE: "eof" | "quiet" | "life" */
    /* private line state follows */
} cc_watch_t;
void cc_watch_init(cc_watch_t *w, char *body, size_t cap);
cc_watch_ev_t cc_watch_feed(cc_watch_t *w, const char *data, size_t n, size_t *used);
/* consumes bytes up to and including the first complete event and returns it (*used = bytes
   consumed); NONE when data ran out first. \r is dropped. Marker lines must match exactly.
   Lines outside a block that aren't markers (pty echo, blanks) are ignored. A body over cap
   gives OVERFLOW at its END line instead of BLOCK. A body line of any length works. */
```
- Produces, in `cc_link.h`:
  - `void cc_poll_set_floor(int seconds);` makes the effective interval `max(rate, floor)`. Task 5 sets 15 in Off and 0 otherwise.
  - `uint32_t cc_poll_generation(void);` counts successful results, from blocks or polls.
  - `int64_t cc_poll_last_ok_ms(void);` (from Task 3) also covers blocks.
  - `bool cc_link_busy(void);` is true while a paste or live check is in flight, as a power hold.
  - `void cc_link_watch_enable(bool on);` and `const char *cc_watch_status_text(char *buf, size_t cap);` write `watch: on I=3s, last block 4s ago, heartbeat 12s ago`, `watch: starting`, `watch: fallback (<why>), polling every 3s` or `watch: off`.
- Link-task behaviour, which replaces the timed `refresh()` while connected:
  1. Open a pty channel (`ssh_channel_request_pty_size(ch, "xterm-256color", 200, 50)`), exec `cc_cmd_watch`, and write `"\n"` at once and then every `CC_WATCH_KEEPALIVE_S`.
  2. Between queued requests, read without blocking into `cc_watch_feed`.
  3. A BLOCK with rc 0 is parsed with `cc_sessions_parse` and published exactly like `refresh_once` does today. Factor out `publish(n)`.
  4. A BLOCK with rc ≠ 0 sets `s_poll_fails = CC_POLL_FAILS_SHOWN` until the next rc 0 block. The loop never repeats unchanged output, so the failure must show at once.
  5. OVERFLOW sets `s_poll_too_big`.
  6. No BLOCK or HEARTBEAT for `2H + I` seconds means the link is dead: set `s_drop`.
  7. A change in the effective interval closes the channel (EOF + close, so sshd's pty hangup kills the loop) and opens a new one.
  8. `BYE life` restarts at once and isn't a failure. `BYE quiet` logs a warning and restarts.
  9. **Fallback** to Plan 1's `refresh()` at the effective interval, logging `watch: falling back to polling: <why>`, when:
     - the pty or exec request fails;
     - no BANNER arrives within 10 s;
     - three channel ends without `BYE life` happen within 5 min.
     The watch is retried after 10 min or on the next connect.
  10. Quick answers and live checks keep their own exec channels on the same session, and `do_paste` keeps its own fresh `refresh()`.
- Take-over: `open_now` calls `cc_poll_set_rate(15)` instead of `0`, so blocks keep feeding the keyboard light. `session_closed_cb` sets 3 again. The dashboard is deleted while the terminal is open, so nothing redraws.

- [ ] **Step 1: Write the failing C tests**

`test_cc_cmd.c`:
  - `test_cmd_watch_exact`: the line above, byte for byte.
  - `test_cmd_watch_rejects`: path `my claude`, interval 0 and 121, quiet 1, life 4, and a cap one byte too small → false. The output contains no `'` other than the two that delimit the `sh -c` argument.

`test_cc_watch.c`:
  - `test_banner_block_heartbeat`: `"@@CC-WATCH 1\r\n@@CC-BEGIN 0\r\n[1]\r\n@@CC-END\r\n@@CC-HB 63\r\n"` → BANNER, then BLOCK with body `"[1]\n"` and rc 0, then HEARTBEAT.
  - `test_feed_bytewise`: the same input fed one byte at a time → the same events and body.
  - `test_ignores_echo_and_noise`: blank lines, `"\r\n"`, `"x@@CC-END"` and an END before any BEGIN → no events except the real ones.
  - `test_long_body_line`: a 200 000-byte single-line JSON body with cap 256 KB → BLOCK with len 200 001.
  - `test_overflow`: cap 64 with a 100-byte body → OVERFLOW; the next block parses normally.
  - `test_rc_and_bye`: `@@CC-BEGIN 127` → rc 127; `@@CC-BYE quiet` → BYE with `bye` = `"quiet"`.
  - `test_two_events_one_feed`: two blocks in one buffer → the first call returns BLOCK with `used` short of n, and the second call returns the second BLOCK.

- [ ] **Step 2: Write the failing loop tests** in `firmware/tools/test_watch_loop.py`
  - Setup:
    - It builds the command with `firmware/test/host/build/cc_cmd_print <claude> <I> <H> <T> <L>`, and skips with a message if that binary is missing.
    - It runs `/bin/zsh -c "<command>"` (as sshd would) under `pty.fork()`, so the pty is the controlling terminal as on the Mac.
    - It uses a fake `claude` in a temp dir that prints `state.json`. It can also sleep 30 s first: the "slow" variant.
    - Every test kills its process group in teardown and asserts that `pgrep -f <tempdir>` finds nothing.
  - `test_change_detection`: I 1, H 3, keepalive each second → one BLOCK; rewriting `state.json` → a second BLOCK within 2 s; then a HB within H + 1 s and no repeated BLOCK.
  - `test_quiet_exit`: T 3 with no keepalive → `@@CC-BYE quiet` and exit within T + I + 1 s.
  - `test_keepalive_keeps_running`: T 3 with a keepalive every second for 8 s → still running.
  - `test_max_lifetime`: L 4 with keepalives → `@@CC-BYE life` within L + I + 1 s.
  - `test_hangup_kills_children`: slow fake, close the pty master after 1.5 s → the shell and the fake `claude` are both gone within 2 s.
  - `test_suspended_loop_exits_on_resume` (Review Focus 2): T 3, keepalives every second; then SIGSTOP the process group for 6 s while writing nothing (a sleeping Mac gets no keepalives), then SIGCONT → exits with `quiet` within I + 1 s of resuming.
  - `test_tilde_path`: claude path `~/bin/claude`, with `HOME` set to a temp dir holding `bin/claude` → a BLOCK arrives.

- [ ] **Step 3: Run them to make sure they fail**

Run the host test command, then the Python tests.
Expected: FAIL (`cc_cmd_watch` and `cc_watch_*` undefined; `cc_cmd_print` missing)

- [ ] **Step 4: Implement `cc_cmd_watch`, `cc_watch.c` and `cc_cmd_print.c`**, then run both suites until they pass

- [ ] **Step 5: Implement the link side, the console and the take-over rate**, then build and flash

The console gets `companion watch on|off|status`, plus `companion watch test-fallback`: a debug subcommand that sets the next start's banner timeout to 1 ms.

- [ ] **Step 6: Check the watch on hardware**

On the Mac, start `companion-test-watch` with the Task 3 prompt.

Run: `python3 $T5 "companion watch status"`
Expected: `watch: on I=3s, …`

Run on the Mac: `ps -axo pid,etime,command | grep -c '[@]@CC-WATCH'`
Expected: `1`

Run: `python3 $T5 "reply <companion-test-watch id> blue"` and time how long until `sessions` no longer shows it as `NEEDS_YOU`.
Expected: under I + 2 s after `sent`.

Leave it idle for 70 s, then run `companion watch status`.
Expected: a heartbeat under 60 s ago, and no new block.

- [ ] **Step 7: Check that nothing is left on the Mac**

For each case, run the Mac `ps` count from Step 6 every 5 s and record how long until it reaches `0`:
1. Clean close: `python3 $T5 "companion watch off"`. Expected: 0 within 5 s.
2. Wi-Fi drop: run `companion watch on`, wait for `on`, then `wifi leave`. Expected: 0 within 3K + I = 63 s. Then run `wifi join` and check `watch: on` again within 60 s.
3. Tab5 reboot: `reboot`. Expected: the old loop is gone within 63 s. After the boot, exactly 1 is running again once the link is up.

Record the three times in the rulings file.

- [ ] **Step 8: Check the fallback and the take-over rate**

Run: `python3 $T5 "companion watch test-fallback" "companion watch off" "companion watch on"`. Wait 15 s, then run `companion watch status` and `sessions`.
Expected: `watch: fallback (no banner in 1 ms), polling every 3s`, and `sessions` still updates. Run `companion watch off` and `companion watch on` to restore.

Open `companion-test-watch` full screen from the dashboard (`key enter`).
Expected: `watch: on I=15s`. Press Ctrl+Q (`key ctrl q`).
Expected: `I=3s` again.

Stop `companion-test-watch`.

- [ ] **Step 9: Commit**

```bash
git add firmware/main/companion firmware/main/ui/ui_dashboard.c firmware/test/host firmware/tools/test_watch_loop.py
git commit -m "feat(companion): long-poll watch channel with keepalive, liveness and fallback"
```

### Task 5: Power ladder

**Files:**
- Create: `firmware/main/sys/display_ctl.c`, `firmware/main/sys/display_ctl.h`
- Modify: `firmware/main/sys/power_rules.c`, `power_rules.h`, `firmware/test/host/test_power_rules.c`
- Modify: `firmware/main/sys/power.c`, `power.h` (states, holds, wake, shield, needs-you wake)
- Modify: `firmware/main/input/keyboard.c`, `keyboard.h` (quiet mode on GPIO50 INT; the swallow check in the drain)
- Modify: `firmware/main/app_main.c` (touch INT after the display starts; `idle_poll` period from power)
- Modify: `firmware/main/ui/ui_dashboard.c` (stale cards after a wake, the take-over hold), `firmware/main/ui/ui_terminal.c` (the SSH hold), `firmware/main/ui/ui_settings.c` (the Power section), `firmware/main/ui/ui_core.c`, `ui_home.c`, `ui_music.c`, `ui.h`
- Delete: `firmware/main/ui/ui_screensaver.c`, its entry in `firmware/tools/ui_preview/CMakeLists.txt` and the preview's `standby` case. Settings and Home drop their screensaver rows and tile.
- Modify: `firmware/main/idf_component.yml` (`espressif/esp_lvgl_port: "==2.8.0~1"`; display_ctl depends on its private layout)
- Modify: `firmware/main/sys/power_bench.c` (steps `dim` 90 s, `off-touch-awake` 120 s and `off-touch-asleep` 120 s; Task 3's light steps now enter Off)
- Modify: `firmware/main/sys/power_console.c`

**Interfaces:**
- Produces, in `power_rules.h`:

```c
typedef enum { PS_ACTIVE, PS_DIM, PS_OFF, PS_RADIO_OFF } power_state_t;
typedef struct {
    power_state_t state;
    int64_t now_ms, last_input_ms, off_since_ms;   /* monotonic (esp_timer), never wall clock */
    int dim_s, off_s, radio_off_min;
    bool held, on_battery, sleep_on_usb, radio_off_enabled;
} pr_in_t;
power_state_t pr_next_state(const pr_in_t *in);
/* held → ACTIVE; on USB with !sleep_on_usb → ACTIVE; idle >= off_s → OFF, or RADIO_OFF when
   already OFF/RADIO_OFF, on_battery, radio_off_enabled and now - off_since >= radio_off_min min;
   idle >= dim_s → DIM; else ACTIVE. RADIO_OFF without on_battery → OFF. */
#define PR_SWALLOW_MS 400
bool pr_swallow(power_state_t state_at_input, int64_t now_ms, int64_t woke_at_ms, int64_t last_swallowed_ms);
/* true when state_at_input != ACTIVE, or now - woke_at < 400, or now - last_swallowed < 400 */
#define PR_IDS_MAX 32
typedef struct { char ids[PR_IDS_MAX][16]; int n; } pr_idset_t;
void pr_needs_you_ids(const cc_session_t *s, int n, pr_idset_t *out);   /* background, live, NEEDS_YOU */
bool pr_needs_you_new(const pr_idset_t *armed, const pr_idset_t *now);   /* an id in now not in armed */
int  pr_dim_brightness(int brightness);   /* max(5, brightness * 20 / 100) */
```
- Produces, in `display_ctl.h`. It reads the handles from esp_lvgl_port 2.8.0~1's driver data: a mirror of the first three fields of `lvgl_port_display_ctx_t` `{disp_type, io_handle, panel_handle}`, and the first field of `lvgl_port_touch_ctx_t` `{handle}`. The BSP keeps both static. The idf_component.yml pin keeps that layout fixed.
  - `esp_lcd_panel_handle_t display_ctl_panel(void);`
  - `esp_lcd_touch_handle_t display_ctl_touch(void);`
  - `esp_err_t display_ctl_panel_on(bool on);`, which is `esp_lcd_panel_disp_on_off`.
  - `void display_ctl_touch_int_init(void (*isr)(void *));`. On board version 1 (GT911, from Task 1), it reconfigures GPIO23 as an input with pull-up, as generative-tab5 `prov_device.c:313` does. On other versions it keeps the driver's pin setup. Either way it adds a NEGEDGE ISR.
- Produces, in `power.h`:
  - `power_state_t power_state(void);` and `uint32_t power_wake_seq(void);` (bumped on every wake).
  - `typedef enum { PW_KEY, PW_TOUCH, PW_LIFT, PW_NEEDS_YOU, PW_CONSOLE } power_wake_t;` and `power_wake_t power_wake_reason(void);`.
  - `bool power_take_key(void);` is called by the keyboard drain on the LVGL task. It returns false when the key must be dropped, using `pr_swallow`. A drop in Dim wakes.
  - `void power_wake(power_wake_t why);` and `void power_wake_from_isr(power_wake_t why);`. Any of these wakes resets idle.
  - `typedef enum { POWER_HOLD_TAKEOVER, POWER_HOLD_SSH } power_hold_t;` and `void power_hold(power_hold_t h, bool on);`. `POWER_HOLD_SSH` counts only while `g_settings.ss_disable_in_ssh`. `cc_link_busy()` is polled as the third hold.
  - `int power_poll_period_ms(void);` is 1000, or 10 000 in Off and Radio off while logging is off.
- Produces, in `keyboard.h`: `void keyboard_set_quiet(bool quiet);`. The drain timer calls `power_take_key()` before `keyboard_dispatch` and drops the event when it returns false.
  - `void power_go(power_state_t s);` is used by the console and the bench.
- Off entry, under `bsp_display_lock`:
  1. `display_ctl_panel_on(false)`;
  2. `bsp_display_brightness_set(0)`;
  3. `lvgl_port_stop()`;
  4. `keyboard_set_quiet(true)`: the task waits on a GPIO50 NEGEDGE semaphore with a 1 s safety poll, clears `REG_INT_STA` after reading, and calls `power_wake(PW_KEY)` when it reads an event;
  5. `cc_poll_set_floor(15)`;
  6. arm the needs-you set with `pr_needs_you_ids` of the current snapshot.
  - Touch stays awake: never `bsp_display_enter_sleep` or `bsp_touch_enter_sleep`, except in the bench step `off-touch-asleep`. That step calls `esp_lcd_touch_enter_sleep(display_ctl_touch())`, then `exit_sleep` afterwards, and logs `touch after exit_sleep: ok|dead` from the next INT within 30 s of a `power sim touch`.
- Wake, in this order:
  1. `lvgl_port_resume()`;
  2. panel on;
  3. `bsp_display_brightness_set(g_settings.brightness)` (or `pr_dim_brightness` for a needs-you wake);
  4. `lv_display_trigger_activity(NULL)`;
  5. set `woke_at`, so `pr_swallow` drops the waking input;
  6. `power_wake_seq++`;
  7. `cc_poll_set_floor(0)`;
  8. `keyboard_set_quiet(false)`.
  - A needs-you wake sets `last_input_ms = now - dim_s * 1000`, so the ladder lands in Dim. If the screen isn't the dashboard, it calls `ui_nav_home()`. It fires only when `g_settings.pw_wake_needs_you` is set and `pr_needs_you_new(armed, now)` is true, and it then adds the new ids to `armed`.
- Wake shield: entering Dim creates a full-screen transparent clickable object on `lv_layer_top()`. A press on it wakes the device. It is deleted on the release that follows the wake, or 400 ms after the wake when no press is down. A touch on a dimmed screen therefore never reaches a card.
- Stale cards: when `power_wake_seq()` changes and the reason isn't `PW_NEEDS_YOU`, the dashboard draws the card list at `LV_OPA_50` and shows the muted summary `Updating…` until `cc_poll_generation()` moves past its value at the wake.
- Holds: `open_now` sets `power_hold(POWER_HOLD_TAKEOVER, true)` and `session_closed_cb` clears it. `ui_terminal.c` sets `POWER_HOLD_SSH` wherever it sets `g_ssh_session_active`.
- Settings → Power, a new category, has these rows:
  - Dim after (s), 10–600;
  - Screen off after (s), from dim + 10 to 3600;
  - Sleep on USB power;
  - Wake when a session needs you;
  - Keyboard light shows needs-you (moved here from Task 3);
  - Stay awake in SSH Terminal (`ss_disable_in_ssh`);
  - Time zone: a menu of `vitals_tz_presets`, with the current rule shown.
  - Tasks 7 and 8 add their rows here.
- Console:
  - `power go <active|dim|off>`;
  - `power sim touch` (calls the touch wake path as the ISR would);
  - `power set <dim|off|sleep-usb|needs-wake> <n>`;
  - `power set defaults` restores every Plan 2 power setting to its Task 2 default;
  - `power` now prints `state`, `idle` and `holds`;
  - `power wakes` prints the last 8 wakes as `<seconds ago> <reason> swallowed=<n>`.

- [ ] **Step 1: Write the failing host tests** (added to `test_power_rules.c`)
  - `test_ladder_thresholds`: dim 30, off 120 → idle 29 s ACTIVE, 30 s DIM, 120 s OFF.
  - `test_held_stays_active`: idle 500 s, held → ACTIVE. Also from DIM.
  - `test_usb_without_sleep`: on_battery false, sleep_on_usb false, idle 500 s → ACTIVE. With sleep_on_usb true → OFF.
  - `test_radio_off_needs_battery` (Review Focus 3): state OFF, off_since 21 min ago, radio_off_enabled, on_battery false → OFF; on_battery true → RADIO_OFF; radio_off_enabled false → OFF; and RADIO_OFF with on_battery false → OFF.
  - `test_swallow_rules`: DIM → true; OFF → true; ACTIVE 100 ms after a wake → true; ACTIVE 401 ms after a wake with no swallow since → false.
  - `test_swallow_held_key_repeats` (Review Focus 1): a wake at t = 0, then keys every 60 ms from t = 500 to 1500 (auto-repeat), each swallowed one feeding `last_swallowed` → all swallowed; then a key at t = 2000 → not swallowed.
  - `test_needs_you_new`: armed {a}, now {a} → false; now {a, b} → true; armed {a, b}, now {b} → false; armed {}, now {} → false.
  - `test_dim_brightness`: 80 → 16; 10 → 5; 100 → 20.

- [ ] **Step 2: Run them to make sure they fail**

Run the host test command.
Expected: FAIL

- [ ] **Step 3: Implement the rules** and run the host tests until they pass

- [ ] **Step 4: Implement `display_ctl`, the power states, the quiet keyboard, the shield, stale cards, holds and the Settings → Power section, and remove the screensaver**

Keep the `ss_*` fields in `app_settings_t`, marked reserved, so old blobs load. `ui_keyboard_route` and the gesture callback lose their screensaver branches. Build, flash, and also build the preview (no `standby` case).

- [ ] **Step 5: Check the ladder with short timings**

Run: `python3 $T5 "power set dim 5" "power set off 15"`. Then run `python3 $T5 "power"` at 3 s, 8 s and 20 s.
Expected: `state=active`, then `state=dim`, then `state=off`. At `state=off`, `screenshot` still works (it is an LVGL snapshot).

- [ ] **Step 6: Check key wake and swallow**

With the dashboard showing a blocked `companion-test-ladder` card (Task 3 prompt) selected and the device in Off, run `python3 $T5 "key text z" "power wakes"`. Take a screenshot 1 s later.
Expected: `state=active`; the last wake is `key swallowed=1`; the answer field is empty; the cards show `Updating…` and are fresh again within 2 s of the wake (spec Goal 4: the link never dropped). Record the time.

Run: `python3 $T5 "key text z"`.
Expected: `z` appears in the field (now awake). Press `key esc` to clear it.

- [ ] **Step 7: Check needs-you wake and holds**

Let the device go Off, then on the Mac start `companion-test-ladder2` (Task 3 prompt). Wait for it to block.
Expected: within I = 15 s + 2 s, `power` shows `state=dim`, `power wakes` shows `needs-you`, the dashboard is on screen (it went home even if Settings was open when it slept, so try it that way), and the cards aren't marked stale.

Wait 20 s without input.
Expected: Off again.

Answer `companion-test-ladder2` with `python3 $T5 "reply <its id> green"`. `companion-test-ladder` was already NEEDS YOU when the screen went off. Let the device go Off again and wait 60 s.
Expected: `power wakes` shows no new `needs-you` wake from `companion-test-ladder`.

Open `companion-test-ladder` full screen and wait 30 s.
Expected: `holds=takeover` and `state=active`. Ctrl+Q.

- [ ] **Step 8: Check touch wake through the shield**

Run: `python3 $T5 "power go dim" "power sim touch" "power wakes"`.
Expected: `touch swallowed=1`; no card action. The real-finger check is Task 10 Step 3.

- [ ] **Step 9: Restore and commit**

Run `python3 $T5 "power set defaults"` and stop both test sessions.

```bash
git add -A firmware docs/superpowers/notes/2026-10-23-plan2-rulings.md
git commit -m "feat(power): Active/Dim/Off ladder with swallowed wake input, needs-you wake and stale cards; remove the screensaver"
```

### Task 6: Light-sleep spike (expected to fail)

**Files:**
- Modify: `firmware/main/sys/display_ctl.c`, `display_ctl.h` (teardown and re-create)
- Create: `firmware/sdkconfig.spike` (`CONFIG_PM_ENABLE=y`, `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`, `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y`, `CONFIG_CC_LIGHT_SLEEP_SPIKE=y`)
- Create: `firmware/main/Kconfig.projbuild` (`CONFIG_CC_LIGHT_SLEEP_SPIKE`, default n)
- Modify: `firmware/main/sys/power.c`, `power_bench.c` (step `light-sleep`, 120 s, only when the spike is compiled in and `pw_light_sleep` is set), `power_console.c`
- Create: `docs/superpowers/notes/2026-10-23-light-sleep-spike.md`

**Interfaces:**
- Produces, under `#if CONFIG_CC_LIGHT_SLEEP_SPIKE`: `esp_err_t display_ctl_teardown(void);` and `esp_err_t display_ctl_recreate(void);`.
  - Teardown, under the LVGL lock: `lvgl_port_remove_touch`, `lvgl_port_remove_disp`, `bsp_display_delete()` (panel, IO, DSI bus, PHY LDO, backlight PWM) and `esp_lcd_touch_del`.
  - Re-create: `bsp_display_new_with_handles` with the board's lane rate, then `lvgl_port_add_disp_dsi` with the app's Plan 1 config, then `bsp_touch_new` + `lvgl_port_add_touch`, then `ui_nav_refresh()`.
  - On board version 3 (ST7121, from Task 1) use 965 Mbps (`BSP_LCD_ST712X_MIPI_DSI_LANE_BITRATE_MBPS`); otherwise use the BSP default.
  - With `pw_light_sleep` set, on battery only, Off calls teardown and configures `esp_pm_configure({.max_freq_mhz = 360, .min_freq_mhz = 40, .light_sleep_enable = true})` plus `gpio_wakeup_enable(GPIO50, GPIO_INTR_LOW_LEVEL)`, `gpio_wakeup_enable(GPIO23, GPIO_INTR_LOW_LEVEL)` and `esp_sleep_enable_gpio_wakeup()`. Before every sleep it clears the A164's `REG_INT_STA`. Wake re-creates the display.
- Console: `power spike recreate <n>` runs n teardown/re-create cycles and prints the internal and PSRAM free heap after each one and the slowest wake-to-picture time (the first flush after re-create).

- [ ] **Step 1: Build the spike** with `idf.py -B build-spike -D SDKCONFIG=build-spike/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.spike" build`, then flash it with `idf.py -B build-spike -p /dev/cu.usbmodem211201 flash`

Expected: `Project build complete`. If it doesn't build (for example `PM_ENABLE` with SMP), that is the result: go to Step 4.

- [ ] **Step 2: Run the USB checks** (light sleep itself can't happen on USB; the console would die)

Run: `python3 $T5 "power spike recreate 10"`.
Expected for a pass:
- 10 cycles with no crash;
- internal free heap within 4 KB of the first cycle and PSRAM within 64 KB;
- the slowest wake-to-picture time under 500 ms;
- a screenshot afterwards shows the dashboard.

Then leave it 10 minutes and run `companion status` and `companion watch status`.
Expected: connected, and the watch on.

Also run `power resets`.
Expected: no new panic or brownout.

- [ ] **Step 3: Decide**

If every Step 2 check passed, keep the code behind `CONFIG_CC_LIGHT_SLEEP_SPIKE`, with `pw_light_sleep` default off, a Settings → Power row "Light sleep (experimental)" shown only in that build, and the bench step. Task 10 runs it on battery. Otherwise go to Step 4.

- [ ] **Step 4: If it failed, drop the code**

Revert the spike code and delete `sdkconfig.spike` and `Kconfig.projbuild`. Leave `pw_light_sleep` reserved. Drop DFS too: the DPI panel's `ESP_PM_CPU_FREQ_MAX` lock pins the CPU while the panel exists. Reflash the normal build.

- [ ] **Step 5: Write the notes**

Write `docs/superpowers/notes/2026-10-23-light-sleep-spike.md`: what was tried, each check's result with numbers, the decision, and why. In the failed case, say that Off ships as panel off + quiet tasks + modem power save + the long poll + the Radio-off tier.

- [ ] **Step 6: Commit**

```bash
git add -A firmware docs/superpowers/notes/2026-10-23-light-sleep-spike.md
git commit -m "spike(power): display teardown with light sleep: <kept behind a setting | dropped>, see notes"
```

### Task 7: Lift wake

**Files:**
- Create: `firmware/main/sys/lift_detect.c`, `firmware/main/sys/lift_detect.h` (pure)
- Create: `firmware/main/sys/lift.c`, `firmware/main/sys/lift.h` (BMI270 poller)
- Create: `firmware/test/host/test_lift_detect.c`
- Modify: `firmware/main/idf_component.yml` (`espressif/bmi270: "==1.1.0"`), `firmware/main/sys/power.c`, `firmware/main/ui/ui_settings.c` (Wake on lift), `firmware/main/sys/power_console.c`, `firmware/test/host/CMakeLists.txt`

**Interfaces:**
- Produces, in `lift_detect.h`:

```c
typedef enum { LIFT_SLOW, LIFT_FAST, LIFT_WAKE } lift_ev_t;   /* next sample in 500 ms, in 50 ms, or wake */
typedef struct { float bx, by, bz; bool init; int over; } lift_detect_t;
#define LIFT_THRESHOLD_G 0.15f   /* generative-tab5 prov_imu.c:53 */
#define LIFT_HOLD_SAMPLES 4      /* 4 x 50 ms ≈ 200 ms held */
void lift_reset(lift_detect_t *d);
lift_ev_t lift_feed(lift_detect_t *d, float x, float y, float z);
/* first sample seeds the baseline; |v - baseline| >= 0.15 g starts a candidate (FAST);
   LIFT_HOLD_SAMPLES more in a row over the threshold → WAKE; a sample under it ends the
   candidate (SLOW); while SLOW the baseline follows with alpha 0.1 */
```
- Produces, in `lift.h`: `void lift_init(void);` creates the BMI270 at `BMI270_I2C_ADDRESS_L` (0x68) on `bsp_i2c_get_handle()` and the `lift` task, which waits on a notification. `void lift_arm(bool on);` is called by power when entering Dim, Off or Radio off with `pw_wake_lift` set, and on wake. While armed, the task runs `bmi270_start` (accelerometer 25 Hz, ±2 g, power-optimized), reads with `bmi270_get_acce_data` at the interval `lift_feed` asks for, and calls `power_wake(PW_LIFT)` on WAKE. While disarmed it calls `bmi270_stop`.
- Console: `power sim lift` calls `power_wake(PW_LIFT)`; `power lift` prints the last sample, the baseline and armed yes/no.

- [ ] **Step 1: Write the failing host tests** in `test_lift_detect.c`
  - `test_still_noise_never_wakes`: 1000 samples of (0, 0, 1) ± 0.03 g pseudo-noise → never WAKE.
  - `test_tilt_held_wakes`: baseline (0, 0, 1), then (0.3, 0, 0.95) five times → FAST, FAST, FAST, FAST, WAKE.
  - `test_single_spike_no_wake`: one sample at (0.5, 0, 1), then back to rest → FAST, then SLOW.
  - `test_slow_drift_follows`: a 0.001 g/sample drift over 500 samples → no WAKE.

- [ ] **Step 2: Run them to make sure they fail**

Run the host test command.
Expected: FAIL

- [ ] **Step 3: Implement `lift_detect.c`** and run the tests until they pass

- [ ] **Step 4: Implement `lift.c`, the power hooks and the setting row**, then build and flash

Expected: the boot log shows the BMI270 found at 0x68 (chip id 0x24).

- [ ] **Step 5: Check on hardware without moving the device**

Run: `python3 $T5 "power set off 15"`. Wait for `state=off`, then run `python3 $T5 "power lift"`.
Expected: armed yes, with z near ±1 g.

Run: `python3 $T5 "power sim lift" "power wakes"`.
Expected: `lift swallowed=0`, `state=active`.

Leave it in Off for 3 minutes on the desk.
Expected: no `lift` wake in `power wakes`. Lifting it for real is Task 10 Step 3.

Run `power set defaults`.

- [ ] **Step 6: Commit**

```bash
git add firmware/main firmware/test/host
git commit -m "feat(power): wake on lift by polling the BMI270 in Dim and Off"
```

### Task 8: wifi_mgr reconnect, parked state and the Radio-off tier

**Files:**
- Modify: `firmware/main/net/wifi_mgr.c`, `wifi_mgr.h`
- Modify: `firmware/main/net/tailscale.c`, `tailscale.h`
- Modify: `firmware/main/companion/cc_link.c`, `cc_link.h`
- Modify: `firmware/main/sys/power.c`, `power_rules.c`, `power_rules.h`, `firmware/test/host/test_power_rules.c`
- Modify: `firmware/main/app_main.c` (`wifi_global_cb`), `firmware/main/ui/ui_settings.c` (Radio-off rows), `firmware/main/ui/ui_dashboard.c` (`Reconnecting…`), `firmware/main/sys/power_bench.c` (steps `radio-off` 180 s, `checkin-handshake` and `checkin-rebind`, each up to 120 s), `firmware/main/sys/power_console.c`

**Interfaces:**
- Produces, in `power_rules.h`:
  - `int pr_backoff_s(int attempt);` gives 1, 2, 4, 8, 16, then 30 for every later attempt, with no cap on attempts.
  - `bool pr_radio_off_worth_it(float a_mw, float b_mw, float c_mwh, int checkin_min, int checkin_s);`. It is the spec's rule: `(a − b) × period > c`, the average draw drops at least 20%, and `checkin_s < 30`. The average with toggling is `b + c / period`.
- Produces, in `wifi_mgr.h`:
  - Background reconnect moves to a `wifi_rc` task (stack 3072). The disconnect handler only notifies it, never `vTaskDelay`. The task calls `esp_wifi_connect()` after `pr_backoff_s(attempt)` seconds until it gets an IP, is parked, or the user disconnected. A background attempt that fails is retried, so it never stops for good.
  - `bool wifi_mgr_park(void);` sets parked and calls `esp_wifi_stop()`. If that fails, or if the next `esp_wifi_start()` fails, it falls back to `esp_wifi_disconnect()` + `esp_wifi_set_ps(WIFI_PS_MAX_MODEM)` and records `park mode: disconnect`.
  - `void wifi_mgr_unpark(void);` clears parked and calls `esp_wifi_start()` (or `esp_wifi_connect()` in disconnect mode).
  - `bool wifi_mgr_parked(void);` and `const char *wifi_mgr_park_mode(void);` (`"stop"` or `"disconnect"`).
  - While parked, `STA_DISCONNECTED` never reconnects and `STA_START` never auto-connects.
  - `void wifi_mgr_set_ps_max(bool max);` switches between `WIFI_PS_MAX_MODEM` (Off) and `WIFI_PS_MIN_MODEM` (otherwise).
- Produces, in `tailscale.h`:
  - `void tailscale_set_fast_resume(bool on);`. While it is on, `tailscale_on_wifi(true)` doesn't call `microlink_rebind`.
  - `typedef enum { TS_CHECKIN_HANDSHAKE, TS_CHECKIN_REBIND, TS_CHECKIN_FAILED } ts_checkin_t;` and `ts_checkin_t tailscale_checkin(const char *ip, bool force_rebind, uint32_t *ms_out);`. It kicks `ml_wg_mgr_trigger_handshake` + `ml_wg_mgr_send_cmm` and waits up to 8 s for `ml_wg_mgr_peer_is_up`. Only when that fails (or `force_rebind` is set) does it call `microlink_rebind`, wait up to 60 s for `ML_EVT_COORD_REGISTERED | ML_EVT_DERP_CONNECTED`, and handshake again.
  - `void tailscale_kick_peer(const char *ip);` kicks a handshake without waiting.
- Produces, in `cc_link.h`:
  - `void cc_link_park(bool parked);`. Parked closes the session and doesn't reconnect.
  - `esp_err_t cc_link_poll_now(int timeout_ms);` runs one plain `refresh()` now, as a queued request.
  - The stale-peer fix: after a failed `ssh_connect` to a tailnet IP, call `tailscale_kick_peer(ip)` before the next retry. Console `companion kick on|off` (RAM only, default on) lets Task 10 measure with and without it.
  - `const char *cc_link_reconnect_log(char *buf, size_t cap);` prints the last 8 link down/up events with wall-clock time, for the Mac sleep measurement.
- Power, Radio off:
  - Entry follows `pr_next_state`. It runs `cc_link_park(true)`, then `wifi_mgr_park()`, and records `park mode`.
  - Every `pw_checkin_min` it runs a check-in, logging tag `0x80 | PS_RADIO_OFF`:
    1. `tailscale_set_fast_resume(true)` + `wifi_mgr_unpark()`;
    2. wait up to 20 s for an IP;
    3. `tailscale_checkin(mac_ip, false, &ms)`;
    4. `cc_link_park(false)` and wait up to 15 s for connected;
    5. `cc_link_poll_now(15000)`;
    6. a new needs-you does a needs-you wake (Dim) and stays up; otherwise run `cc_link_park(true)` + `wifi_mgr_park()` again.
  - Each check-in's path, total ms and mWh go into the power log and `power checkins`.
  - A key, touch or lift during Radio off wakes to Active: unpark at once, then reconnect, while the dashboard shows the muted `Reconnecting to <host>…` with stale cards until the first fresh result.
  - Input wake from Radio off uses the same steps 1–5 (handshake first, so the target is about 10 s), then stays up.
  - The keyboard light uses `pr_light_max_age_ms(true, pw_checkin_min)` while in Radio off.
  - Off entry also calls `wifi_mgr_set_ps_max(true)`, and every wake calls `wifi_mgr_set_ps_max(false)`.
- Settings → Power: Radio off (switch, default off), Radio off after (min) 5–240, Check in every (min) 1–60.
- Console:
  - `power go radio-off`, `power checkin [rebind]` (one check-in now), `power checkins` (last 8: path, ms, mWh);
  - `power set radio-off 0|1`, `power set radio-after <min>`, `power set checkin <min>`;
  - `companion reconnect-log` prints `cc_link_reconnect_log`;
  - the debug-only `wifi test drop` calls `esp_wifi_disconnect()` without setting the user flag, like a real drop.

- [ ] **Step 1: Write the failing host tests** (in `test_power_rules.c`)
  - `test_backoff`: attempts 0..7 → 1, 2, 4, 8, 16, 30, 30, 30; attempt 1000 → 30.
  - `test_radio_off_rule`: a = 300, b = 60, c = 2.0 mWh, 5 min, 12 s → true (the saving is 20 mWh per period; the average falls from 300 to 84 mW); c = 25 mWh → false; check-in 31 s → false; a = 100, b = 90 → false (under 20%).

- [ ] **Step 2: Run them to make sure they fail**

Run the host test command.
Expected: FAIL

- [ ] **Step 3: Implement the rules** and run the tests until they pass

- [ ] **Step 4: Implement wifi_mgr, Tailscale, cc_link and power**, then build and flash

- [ ] **Step 5: Check background reconnect**

Run: `python3 $T5 "wifi test drop"`.
Expected: the log shows retries at 1, 2, 4 … s, and the device is back on Wi-Fi without a reboot. `companion status` shows connected within 60 s.

- [ ] **Step 6: Check parking and both check-in paths on USB**

The rule needs battery, so this uses the forced state. Run: `python3 $T5 "power go radio-off"`. Wait 10 s, then run `python3 $T5 "power"`.
Expected: `state=radio-off`, `wifi parked (stop)` or `(disconnect)`. Record which in the rulings file.

Run: `python3 $T5 "power checkin"`, wait 60 s, then `python3 $T5 "power checkins"`.
Expected: one entry with path `handshake` or `rebind` and its ms.

Run: `python3 $T5 "power checkin rebind"`, wait 120 s, then `power checkins`.
Expected: a `rebind` entry.

Over 6 minutes parked, watch the log for MicroLink escalating to a full re-registration. Record what it does: it decides whether check-ins stay cheap.

- [ ] **Step 7: Check a check-in that finds needs-you, and input during Radio off**

While parked, start `companion-test-radio` on the Mac (Task 3 prompt) and wait for it to block. Run `python3 $T5 "power checkin"`.
Expected: within the check-in time, `state=dim`, `power wakes` shows `needs-you`, and `power light` is amber.

Go back to Radio off, then run `python3 $T5 "key text z"` and take screenshots at once and after 15 s.
Expected: the first shows `Reconnecting to <host>…` with stale cards and no `z` anywhere; the second shows fresh cards. Record the time from the key to fresh cards against the 10 s target.

Stop the session, then run `power set defaults`.

- [ ] **Step 8: Commit**

```bash
git add firmware/main firmware/test/host
git commit -m "feat(net): Wi-Fi reconnect task with backoff, parked state, Radio-off tier with handshake-first check-ins"
```

### Task 9: Plan 1 follow-ups

**Files:**
- Modify: `firmware/main/ssh/ssh_client.c`, `ssh_client.h` (forget one pin)
- Modify: `firmware/main/companion/cc_link.c`, `cc_link.h`, `cc_console.c`
- Modify: `firmware/main/ui/ui_settings.c` (Companion → Forget Mac host key)
- Create: `firmware/tools/capture_attach_screen.py`
- Create: `firmware/test/host/fixtures/screen_permission_dialog.bin`
- Modify: `firmware/test/host/test_cc_cmd.c`
- Modify: `docs/superpowers/specs/2026-10-22-claude-tab5-design.md` (D5 and D7 wording), `docs/superpowers/notes/2026-10-22-plan1-rulings.md` (mark the follow-ups done)

**Interfaces:**
- Produces, in `ssh_client.h`: `bool ssh_known_host_forget(const char *host, int port);` erases the NVS `kh` entry for host:port and returns true if there was one.
- Produces, in `cc_link.h`: `esp_err_t cc_link_forget_host_key(char *fp_out, size_t fp_cap, int timeout_ms);`.
  - It is a queued request. It forgets the link's pin (the resolved address it connects to) and the terminal's pin (the server entry's host string), tears the session down and reconnects, so the new key is pinned as `KH_UNKNOWN`.
  - It returns `ESP_OK` with the new fingerprint, or `ESP_ERR_TIMEOUT` if no connect happened within `timeout_ms` (the pins are cleared either way).
- Settings → Companion → "Forget Mac host key" asks with `ui_confirm("Forget Mac host key?", "Only do this if your Mac's SSH host key changed. The Tab5 will trust the next key it sees.")`. After Yes, it calls the request on the dashboard worker and shows the toast `New Mac key pinned: SHA256:…` or `Old key forgotten; reconnecting…`.
- cc_link minors:
  - `cc_link_failure()` reads `s_state` and `s_fail` under `lock()`;
  - when `ssh_new()` fails, `connect_once` calls `set_fail(CC_LINK_FAIL_NONE, NULL)` before returning, so an old reason doesn't stick.
- Console `companion hostkey forget` runs the same request. The debug-only `companion hostkey corrupt` overwrites the link's own pin with a wrong value, so the changed-key path can be shown without touching the Mac.
- `capture_attach_screen.py <short-id> <out.bin>`, run on the Mac:
  1. spawns `~/.local/bin/claude attach <id>` under `pty.fork()` at 120×40 with `TERM=xterm-256color`;
  2. reads until quiet for 1.0 s (at most 5 s);
  3. sends only Ctrl+Z (never ← or a bare Esc);
  4. writes the raw bytes with `$HOME` replaced by `/Users/me`.

- [ ] **Step 1: Capture a real permission dialog (R19)**

On the Mac, start `~/.local/bin/claude --bg --permission-mode default --name companion-test-perm1 "Use the Bash tool to run exactly: date > /tmp/companion-test-perm.txt"`. Wait up to 2 min for `state` `blocked`.
- If it never blocks, try `companion-test-perm2` with `"Use WebFetch to fetch https://example.com and tell me its title."`.
- If neither blocks, record it in the rulings file, keep the synthetic case, and skip Steps 2–3.

Run `python3 firmware/tools/capture_attach_screen.py <id> firmware/test/host/fixtures/screen_permission_dialog.bin`. Check the file for anything personal beyond the test command. Then stop the session and delete `/tmp/companion-test-perm.txt` if it exists.

- [ ] **Step 2: Write the failing fixture test**

`test_screen_real_permission_dialog`: the fixture, through `cc_strip_ansi`, gives `cc_screen_input_ready` false and `cc_screen_is_permission_prompt` true.

Run the host test command.
Expected: the `input_ready` assertion passes (the gate is fail-closed already). The matcher assertion may fail on real bytes.

- [ ] **Step 3: Make it pass** and run the host tests

If the matcher assertion failed, fix `cc_screen_is_permission_prompt` against the real bytes without loosening `cc_screen_input_ready`. Every existing screen test must still pass.

- [ ] **Step 4: Implement the forget action, the console and the minors**, then build and flash

- [ ] **Step 5: Check on hardware**

Run on the Mac: `ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub` (the public file is world-readable; no sudo).

Run: `python3 $T5 "companion hostkey corrupt"`, wait 15 s, then `python3 $T5 "companion status"`.
Expected: `host key changed for <ip>; not connecting (retry in Ns)`, and a screenshot of the dashboard shows the red line.

Open Settings → Companion → Forget Mac host key and take a screenshot of the confirm. Confirm it (`key enter`) and take a screenshot of the toast.
Expected: the toast fingerprint equals the Mac's `ssh-keygen` fingerprint, and `companion status` shows `connected <host>`.

Start `companion-test-hostkey` on the Mac (Task 3 prompt) and open it full screen from the dashboard.
Expected: the terminal's pin was cleared too, so it asks once to trust the key (D7: the terminal asks before it pins), showing the same fingerprint. Accept it with `key enter`, check the session opens, press Ctrl+Q (`key ctrl q`), and stop the session.

- [ ] **Step 6: Commit**

```bash
git add firmware docs/superpowers
git commit -m "feat(companion): forget Mac host key; real permission-dialog fixture; cc_link failure-reason fixes"
```

### Task 10: Measurement visit with the user

This is the only task that needs the user. Tasks 1–9 are done and flashed, and every gated setting is still at its default.

**Files:**
- Create: `docs/superpowers/notes/2026-10-23-power-measurements.md`
- Modify: `firmware/main/settings/settings.c`, `settings.h` (gated defaults from the data; `CFG_VERSION 19` with a migration that sets each changed default once)
- Modify: `docs/superpowers/specs/2026-10-22-claude-tab5-design.md` (Known issues: Mac sleep/wake reconnect, brownout counts; Roadmap row 2 done)

**Interfaces:**
- Consumes: `power bench`, `power_report.py`, `power wakes`, `power checkins`, `power light test bind`, `cc_link_reconnect_log` and `companion kick` from the earlier tasks, plus `pr_radio_off_worth_it` (Task 8).

- [ ] **Step 1: Prepare without the user**

Flash the final build, then run `python3 $T5 "power set defaults" "power log clear" "power log on" "power resets"` and record the reset counts. On the Mac, start `companion-test-bench` (Task 3 prompt) so needs-you is live for the light steps.

- [ ] **Step 2: Ask the user once, in one message, for everything**
  1. Confirm a battery is fitted, and read its capacity from the label if you can.
  2. When I say so, unplug USB for about 20 minutes while `power bench` runs. The Tab5 shows `Bench done. Plug USB back in.` when it's finished; then plug it back in.
  3. While it's still plugged in, before the bench: tap the dark screen once, press one key, and pick the Tab5 up off the table. I'll check that each one only wakes it. Then look at the keyboard light for 5 s while I switch it to the keyboard's own mode, and tell me what it shows.
  4. After the bench: put the Mac to sleep (Apple menu → Sleep) for about 3 minutes, then wake it.

- [ ] **Step 3: The wake checks with the user** (on USB, before the bench)
  - Run `python3 $T5 "power go off"`, then ask for the tap. Then `power wakes`.
    Expected: `touch swallowed≥1`, no card action, and the screen lit.
  - The same for a key (`key swallowed=1`) and for a lift (`lift`).
  - Run `power light test bind`, then `power light test user`, and record the user's description of mode 0.

- [ ] **Step 4: The bench on battery**

Run: `python3 $T5 "power bench start"`, then ask the user to unplug. After the user plugs back in, run `python3 firmware/tools/power_report.py --port /dev/cu.usbmodem211201 --bench --capacity-mwh <N>` and `--log`.

Check the charge pin: in the log, `flags` bit 0 should change at the unplug and the plug-in. If it doesn't track, set charging detection to the current direction only and record why.

- [ ] **Step 5: The Mac sleep/wake reconnect**

When the agent resumes after the Mac wakes, run:
- `pmset -g log | grep -E "Wake from|Entering Sleep" | tail -4` for the wall-clock sleep and wake times;
- `python3 $T5 "companion reconnect-log"` for the Tab5's link down/up times;
- the Mac `ps` count from Task 4 (expected `0` or `1`, never more).

The reconnect time is link-up minus the Mac wake. If it is over 30 s, run the sleep once more with `companion kick off` and compare. Keep whichever is faster as the default, and record both.

- [ ] **Step 6: Set the gated defaults from the numbers**
  - **Radio off:** on by default only if `pr_radio_off_worth_it(a, b, c, 5, checkin_s)` is true for the measured handshake path, where a is `off-touch-awake`, b is `radio-off` and c and the check-in time come from `checkin-handshake`.
  - **Light sleep:** on by default only if Task 6 kept it and its bench step shows a lower draw than `off-touch-awake` with the link still up afterwards.
  - **`kbd_off_cap`:** if `off-light-amber` minus `off-light-off` is over 5% of `off-touch-awake`, cap it at the lowest brightness the user still saw clearly (default 30).
  - Bump `CFG_VERSION` to 19 so the device picks up each changed default once.
  - Build, flash and spot-check with `power`.

- [ ] **Step 7: Write the notes**

Write `docs/superpowers/notes/2026-10-23-power-measurements.md`:
- the per-step table (avg/min/max mW and mA, mWh);
- the estimated battery life for a day of 1 h Active + 23 h Off, and the same with Radio off, labelled **estimate** with the capacity used;
- the touch-awake cost (`off-touch-awake` vs `off-touch-asleep`, and whether touch came back);
- the keyboard light's cost;
- both check-in paths;
- the charge-pin result and what mode 0 shows;
- the Mac sleep/wake reconnect time, with and without the kick;
- the reset counts before and after;
- every gated decision with the formula's numbers.

Keep simulated or forced-state (USB) results apart from battery results. Update the main spec's Known issues and the Roadmap row.

- [ ] **Step 8: Clean up and commit**

Stop `companion-test-bench`, then run `power log off`.

```bash
git add firmware/main/settings docs/superpowers
git commit -m "docs(power): battery measurements, gated defaults from the data"
```
