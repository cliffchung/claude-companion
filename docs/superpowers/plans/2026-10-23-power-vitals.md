# Power & Vitals Implementation Plan (Plan 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Tab5 dims and turns its screen off on its own, wakes instantly on a key, touch, lift or a session that needs you (without the waking input acting), keeps the Mac link live through a long poll, shows battery, charging, Wi-Fi, Tailscale, link and the right local time, and lights the keyboard amber while a session needs you. After an hour without input it saves battery first (Deep idle, then Hibernate), and its shortcuts work with two thumbs. All of it is measured on battery in one visit with the user.

**Architecture:** A new `sys/power.c` owns one state machine (Active → Dim → Off → Radio off → Deep idle → Hibernate) and the keyboard light. Hibernate itself (power-off or ESP deep sleep, its wake sources, the rails it cuts and the cold-boot path) lives in `sys/hibernate.c`, built in the Task 6 spike. `sys/imu.c` drives the BMI270's any-motion and no-motion interrupts through raw feature-register writes (Task 6 probes where its INT pins go; Task 7 uses them for lift wake, stillness, anticipatory wake and the IMU wake from Hibernate with a false-wake guard). Its decisions are pure functions in `sys/power_rules.c`, and the battery and clock maths are pure functions in `sys/vitals.c`, all host-tested. `cc_link` replaces the fixed poll with one long-lived pty exec channel. That channel runs a fixed `/bin/sh` loop built by `cc_cmd_watch`, and a pure marker parser (`companion/cc_watch.c`) reads its output. `wifi_mgr` gains a reconnect task and a parked state for the Radio-off tier. The light-sleep spike, Radio off and the light's Off cap are built behind settings that default off. One battery session with the user sets them from INA226 numbers logged on the device.

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
- New pure modules (`sys/power_rules.c`, `sys/vitals.c`, `sys/plog.c`, `sys/lift_detect.c`, `companion/cc_watch.c`, `ui/hotkeys.c`) include no ESP-IDF headers and also build in `firmware/test/host`.
- **This Tab5 is board v2: LCD ST7123, touch ST7123, no GT911** (Ruling P1). Controller ruling PT1: v2 has no touch INT wake, so touch wake in Off and deeper comes from polling the touch controller at about 10 Hz from the power task while LVGL is paused. The GT911 INT path is v1-only code.
- Match Clientre5's C style: 4-space indent, function brace on its own line, `static const char *TAG`, `s_` statics, `/* */` comments.
- Every gated feature (light sleep, Radio off, the light's Off cap, the Hibernate check-in) is built behind its setting with the default off before Task 10. Nothing waits on the measurement. The exception is the spec's Priority: Deep idle (60 min) and Hibernate (2 h) are on by default, and Hibernate's default stays on only if Task 6 finds a wake path that needs nothing more than the side button (Amendment A4).
- Record each judgment call as a ruling in `docs/superpowers/notes/2026-10-23-plan2-rulings.md`, in Plan 1's format (`Ruling Pn: … — why — cost if wrong`).
- **Never hibernate the device without a timer backstop while the user isn't there.** Nothing on the Mac can wake a sleeping P4, and a device lying on the desk gives the IMU no motion. Every agent-run hibernate arms a timer wake (`power hib test …`, `power hib test-wake`, or the persistent `power hib backstop` from Task 7 when a test spans several cold boots).

## Review Focus

1. **A held key or key repeat while the device wakes** → nothing reaches the dashboard; the first key after the device is awake works normally. Covered by Task 5 `test_swallow_held_key_repeats` and the Step 6 `key` check.
2. **The Mac sleeps with the watch loop running** (the loop is suspended, then resumes with a big `$SECONDS` jump and no keepalives) → the loop exits on resume and leaves nothing behind. Covered by Task 4 `test_suspended_loop_exits_on_resume` (SIGSTOP/SIGCONT) and the Task 10 `ps` check after a real Mac sleep.
3. **No battery fitted, or the INA226 not found** → the bar shows no percentage (never a red 0%), and Radio off, Deep idle and Hibernate never engage, because "on battery" is false; an unknown percentage never counts as ≤ 10%. With the INA226 present and no pack, the bus voltage is below 5.5 V and counts as no battery. Covered by Task 2 `test_battery_unknown_not_low` and `test_no_pack_is_unknown`, and Task 5 `test_radio_off_needs_battery` and `test_long_idle_needs_battery`.
4. **Radio off and Deep idle with check-ins up to 30 min apart** → the amber keyboard light is not dropped between check-ins by the 5-minute staleness rule; the age limit follows the period actually scheduled (`checkin_min`, or Deep idle's 15 and 30 min). Covered by Task 3 `test_light_age_limit_follows_checkin` and Task 8 `test_deep_idle_backoff`.
5. **A wake from Hibernate (a cold boot) shows cards as live** before the first fresh result → the dashboard shows `Waking: reconnecting to Mac` and no cards until `cc_poll_generation()` first moves. Covered by Task 5 `test_cards_view_after_hibernate` and the Task 6 Step 9 screenshot taken straight after a wake boot. (The amber-like saved colour that used to be item 5 stays covered by Task 3 `test_amber_like_colours`.)
6. **A false IMU wake in a bag or a car drains the battery** (each wake from Hibernate is a cold boot plus a Tailscale connect, about 2–3 W for 1–2 min, and a bag can keep the device moving for hours) → no key or touch within 20 s powers it down again; a wake confirmed only by motion goes back to Hibernate when it reaches Off without a key or touch; 3 unconfirmed IMU wakes within an hour turn IMU wake off until a side-button or key wake, and that count survives the cold boots (NVS). Covered by Task 7 `test_imu_cap`, `test_imu_confirm` and the Task 7 Step 7 data-ready wake loop on USB, and measured by the Task 10 Step 6b bag walk.

---

### Task 1: Power instrumentation, reset reasons and boot rails

**Files:**
- Create: `firmware/main/sys/plog.c`, `firmware/main/sys/plog.h` (pure ring and summaries)
- Create: `firmware/main/sys/power_log.c`, `firmware/main/sys/power_log.h` (sampling, NVS summaries, reset reasons)
- Create: `firmware/main/sys/power_bench.c`, `firmware/main/sys/power_bench.h`
- Create: `firmware/main/sys/power_console.c`, `firmware/main/sys/power_console.h` (the `power` command; `void power_console_init(void);`)
- Create: `firmware/tools/power_report.py`, `firmware/tools/test_power_report.py`
- Create: `firmware/test/host/test_plog.c`
- Modify: `firmware/test/host/CMakeLists.txt` (add a `pure` library with `../../main/sys/plog.c` and `firmware/main` in its include dirs, so `companion/cc_sessions.h` resolves; later tasks add their pure files to it; add `test_plog`)
- Modify: `firmware/main/sys/sysinfo.c`, `sysinfo.h` (expose the raw charge pin; `sysinfo_poll` reads it through that function)
- Modify: `firmware/main/app_main.c` (reset reason at boot, staggered rails, `power_console_init()` beside `cc_console_init()`, the idle task samples the log)
- Modify: `firmware/sdkconfig.defaults` (brownout)

**Interfaces:**
- Produces, in `plog.h`:

```c
typedef struct { uint32_t t_s; uint16_t mv; int16_t ma; uint16_t mw; uint8_t tag; uint8_t flags; } plog_sample_t;
/* tag: power state 0..3 (Task 5), 0x10+n for bench step n, 0x80|state for a Radio-off check-in (0x83); flags bit0 = charge pin low, bit1 = pin read OK */
typedef struct { plog_sample_t *buf; uint32_t cap, head, count; } plog_ring_t;
void plog_init(plog_ring_t *r, plog_sample_t *buf, uint32_t cap);
void plog_push(plog_ring_t *r, const plog_sample_t *s);           /* overwrites the oldest when full */
bool plog_get(const plog_ring_t *r, uint32_t i, plog_sample_t *out); /* i = 0 is the oldest */
typedef struct { uint32_t n; int64_t sum_mw, sum_ma; uint16_t min_mw, max_mw; uint32_t first_t, last_t; } plog_sum_t;
void plog_sum_add(plog_sum_t *s, const plog_sample_t *x);
uint32_t plog_sum_avg_mw(const plog_sum_t *s);                    /* 0 when empty */
uint32_t plog_sum_mwh_x1000(const plog_sum_t *s);                 /* avg mW x seconds covered / 3.6; seconds covered = last_t - first_t + 1 */
```

- Produces, in `power_log.h`: `void power_log_boot(void);` (logs `reset reason: <name>` and adds 1 to that reason's counter in NVS ns `power`, key `rst`); `void power_log_sample(uint8_t tag);` (called each second by the idle task while logging is on); `bool power_log_on(void); void power_log_enable(bool on);` (65 536-sample PSRAM ring, about 18 h; per-tag `plog_sum_t` saved to NVS key `sum` every 60 s while on, with a table that holds every tag in use: 0..3, 0x10+n and 0x80|state); `void power_log_dump(void);` (prints `@@PLOG <n>`, then one CSV line `t_s,mv,ma,mw,tag,flags` per sample, then `@@END`); `void power_log_clear(void);`; `void power_log_resets(void);`.
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
  - `test_sum_avg_and_energy`: three samples of 1000, 2000 and 3000 mW at t = 10, 11, 12 → avg 2000, min 1000, max 3000; `plog_sum_mwh_x1000` = 2000 × 3 s / 3.6 = 1 667 (±1).
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
  - `sysinfo_charge_pin`, which `sysinfo_poll` also calls instead of reading the expander pin inline.
  - In `app_main`, call `power_log_boot()` right after `settings_init()`, and `power_console_init()` beside `cc_console_init()`.
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
    uint16_t pw_deep_idle_min;   /* spec deep_idle_min: 60; 0 = never */
    uint8_t  pw_hibernate_h;     /* spec hibernate_h: 2; 0 = never (the battery <= 10% trigger stays) */
    uint8_t  pw_hibernate_checkin_h; /* spec hibernate_checkin_h: 0 = off; gated (Task 8, Task 10) */
```
  - `utc_offset_min` becomes reserved.
  - When migrating from version 17 or older, `tz_rule` takes the Pacific default, whatever the old offset was.
  - `settings_init` resets an invalid `tz_rule` (`!vitals_tz_valid`) to the default.
  - It also clamps `pw_dim_s` to 10–600 and `pw_off_s` to `pw_dim_s + 10`–3600.
  - It clamps `pw_deep_idle_min` to 0 or 15–720, `pw_hibernate_h` to 0 or 1–48, and `pw_hibernate_checkin_h` to 0 or 1–24 (a value in the gap goes to the nearest end of the range).
  - Still a single bump to `CFG_VERSION 18` for every field above.
- Produces, in `vitals.h`:

```c
float vitals_ocv(float bus_v, float current_a, float r_ohm);   /* bus_v + current_a * r_ohm (+ = discharging) */
int   vitals_percent_from_ocv(float pack_v);                   /* 2S curve below, clamped 0..100 */
typedef struct { float ema_v; bool init; } vitals_batt_t;
int   vitals_batt_update(vitals_batt_t *b, float bus_v, float current_a, float dt_s);
/* bus_v < 5.5 V (no 2S pack sits that low) → -1 without touching the EMA; otherwise load-compensate with VITALS_R_INT_OHM (0.15, an estimate), EMA with tau 30 s
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
  - `test_no_pack_is_unknown` (Review Focus 3): `vitals_batt_update` with 0.0 V and with 5.0 V → -1.
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
Expected: the header shows the battery glyph and %, the Wi-Fi bars, the Tailscale mark, a green link dot and the right clock. A settings blob saved by Plan 1 loaded without losing the Mac or the Claude path (`companion status` shows `connected <host>`). The long-idle fields have no reader yet; Task 5 Step 5 checks their defaults through `power settings`.

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
- Modify: `firmware/main/input/keyboard.c`, `keyboard.h` (the LED source getter; hot-plug; record the last values `keyboard_set_led` wrote)
- Modify: `firmware/main/ui/ui_settings.c` (drop the gold and orange presets; route LED changes through the owner; the "Keyboard light shows needs-you" switch)
- Modify: `firmware/main/settings/settings.c` (reset an amber-like saved colour)
- Modify: `firmware/main/sys/power_bench.c` (steps `off-light-amber`, 90 s, and `off-light-off`, 90 s; their `enter` returns false until Task 5 provides Off)
- Modify: `firmware/main/companion/cc_link.c`, `cc_link.h` (`cc_poll_last_ok_ms`), `firmware/main/sys/power_console.c` (`power light`), `firmware/main/app_main.c` (`power_init`)
- Modify: `firmware/tools/ui_preview/preview.c`, `firmware/tools/ui_preview/CMakeLists.txt` (stubs for the new calls)
- Modify: `firmware/test/host/CMakeLists.txt` (add `power_rules.c` to the `pure` library, link it to `companion`)

**Interfaces:**
- Produces, in `power_rules.h`:

```c
typedef struct { uint8_t mode, bright, r, g, b; } pr_led_t;   /* mode 0 = bind, 1 = user colour */
bool pr_rgb_amber_like(uint32_t rgb);   /* HSV hue 25..55 deg, saturation >= 0.5, value >= 0.5 */
bool pr_any_needs_you(const cc_session_t *s, int n);   /* background && live && CC_NEEDS_YOU */
pr_led_t pr_kbd_light(const pr_led_t *user, bool setting_on, bool needs_you,
                      int64_t result_age_ms, int64_t max_age_ms, bool screen_off, int off_cap_pct,
                      bool deep_idle);
/* needs_you && setting_on && result_age_ms < max_age_ms → {1, b, 0xf4, 0xb5, 0x45} with
   b = max(user->bright, 30), then min(b, max(off_cap_pct, 30)) when screen_off, then min(b, 30)
   when deep_idle; else, in deep_idle, PR_LED_OFF; else *user */
#define PR_LED_OFF ((pr_led_t){1, 0, 0, 0, 0})   /* mode 1, brightness 0, black */
int64_t pr_light_max_age_ms(bool radio_off, int checkin_min);
/* 300 000, or max(300 000, (checkin_min + 1) * 60 000) in Radio off */
```
- Produces, in `power.h`:
  - `void power_init(void);` starts the `power` task (stack 4096, priority 3).
  - `void power_kbd_light_refresh(void);` re-evaluates the light now. Settings calls it after a change.
  - `void power_kbd_light_get(pr_led_t *out);` gives the current colour, for hot-plug.
  - The owner subscribes with `cc_poll_subscribe`. Its callback stores `pr_any_needs_you`, the first needs-you session's name and the result time under the module's lock, then notifies the power task; the task calls `keyboard_set_led`.
- Produces, in `keyboard.h`: `void keyboard_set_led_source(void (*get)(uint8_t *mode, uint8_t *bright, uint8_t *r, uint8_t *g, uint8_t *b));`. `kb_probe` asks the source for the colour instead of reading `g_settings`, so a keyboard plugged in later picks up the current state. `keyboard.c` also records the last mode, brightness and colour it wrote with `keyboard_set_led`: `void keyboard_get_last_led(uint8_t *mode, uint8_t *bright, uint8_t *r, uint8_t *g, uint8_t *b);`.
- Consumes: `cc_poll_subscribe` (Plan 1). It adds `int64_t cc_poll_last_ok_ms(void);` to `cc_link.h`: the `esp_timer` ms of the last successful result, or 0. Task 4 keeps it current for watch blocks too.
- Console:
  - `power light` prints the owner's choice as `light: <mode> <bright> <r> <g> <b> (<reason>)`, where the reason is `needs you: <name>`, `nobody needs you`, `setting off` or `result too old`, followed by `kbd: <mode> <bright> <r> <g> <b>`, the values `keyboard_set_led` last wrote.
  - Debug subcommands: `power light test bind` writes mode 0 at brightness 60; `power light test user` restores the owner's choice; `power light test reprobe` marks the keyboard absent so the task re-probes it as on a hot-plug.

- [ ] **Step 1: Write the failing host tests** in `test_power_rules.c`
  - `test_amber_like_colours` (Review Focus 5): true for 0xF4B545, 0xFFD700 and 0xFF8000; false for 0x0080FF, 0xFFFFFF, 0xFF4040, 0x40FF40, 0xFF40FF, 0x00FFFF and 0x604818 (dark).
  - `test_light_needs_you_amber`: user {0, 20, 0, 128, 255}, setting on, needs_you, age 1 s → {1, 30, f4, b5, 45}. With user brightness 0 → brightness 30. With user brightness 80 → 80.
  - `test_light_user_when_clear`: needs_you false → exactly the user's struct; setting off → user's.
  - `test_light_stale_result_reverts`: age 300 001 ms, max 300 000 → user's.
  - `test_light_off_cap`: user brightness 80, screen_off, cap 40 → 40; cap 10 → 30.
  - `test_light_deep_idle`: deep_idle, needs_you, user brightness 80, cap 100 → {1, 30, f4, b5, 45}; user brightness 0 → brightness 30; needs_you false → `PR_LED_OFF` (not the user's colour); a stale result (age ≥ max) → `PR_LED_OFF`; setting off → `PR_LED_OFF`.
  - `test_light_age_limit_follows_checkin` (Review Focus 4): `pr_light_max_age_ms(false, 5)` = 300 000; `(true, 5)` = 360 000; `(true, 30)` = 1 860 000.
  - `test_any_needs_you`: a blocked interactive session doesn't count; a blocked stopped (`live` false) one doesn't; a blocked live background one does.

- [ ] **Step 2: Run them to make sure they fail**

Run the host test command.
Expected: FAIL

- [ ] **Step 3: Implement `power_rules.c`** and run the host tests until they pass

- [ ] **Step 4: Implement the owner, the keyboard source, the settings changes and the migration**
  - The presets become `{ 0x0080FF, 0xFFFFFF, 0xFF4040, 0x40FF40, 0xFF40FF, 0x00FFFF }`.
  - `settings_init` replaces an amber-like saved `kbd_led_rgb` with 0x0080FF.
  - In Task 3, `screen_off` and `deep_idle` are always false.
  - Add the preview stubs for the new external calls, then build the preview (`firmware/tools/ui_preview/README.md`).
  - `app_main` calls `power_init()` after `cc_link_start()`.
  - Build and flash.

- [ ] **Step 5: Probe what mode 0 ("bind") shows**

Run: `python3 $T5 "power light test bind"`, then `python3 $T5 "power light test user"`.
Expected: both print `ok`. Nobody can see the keyboard yet, so record "mode 0: not observed yet" in the rulings file; Task 10 Step 4 has the user look at it.

- [ ] **Step 6: Check needs-you on hardware**

On the Mac, run `~/.local/bin/claude --bg --name companion-test-light "Ask me what my favorite color is, then wait for my answer."`. Wait until `python3 $T5 "sessions"` lists it as `NEEDS_YOU`.

Run: `python3 $T5 "power light"`. This prints the owner's current `mode bright r g b` and why.
Expected: `light: 1 30 f4 b5 45 (needs you: companion-test-light)` (brightness 30, or the user's if higher).

Run: `python3 $T5 "reply <its id> green"`, wait for the next poll, then run `python3 $T5 "power light"`.
Expected: the user's setting, reason `nobody needs you`.

Hot-plug: start a fresh `companion-test-light2` (same prompt), wait for it to block, then run `python3 $T5 "power light test bind"`, `python3 $T5 "power light test reprobe"` and `python3 $T5 "power light"`.
Expected: after the re-probe, the `kbd:` values are amber (`1 30 f4 b5 45`) again, not the user's colour and not the bind probe's.

Stop the test session.

- [ ] **Step 7: Check the colour rules**

Open Settings → the keyboard light colour (navigate with `key`) and take a screenshot.
Expected: no gold or orange preset.

- [ ] **Step 8: Commit**

```bash
git add firmware/main firmware/test/host firmware/tools/ui_preview docs/superpowers/notes/2026-10-23-plan2-rulings.md
git commit -m "feat(power): keyboard light owner shows needs-you in amber; drop amber-like presets"
```

### Task 4: Long poll

**Files:**
- Create: `firmware/main/companion/cc_watch.c`, `firmware/main/companion/cc_watch.h` (pure parser)
- Create: `firmware/test/host/test_cc_watch.c`, `firmware/test/host/cc_cmd_print.c` (prints `cc_cmd_watch` output for the Python test)
- Create: `firmware/tools/test_watch_loop.py`
- Modify: `docs/superpowers/specs/2026-10-22-claude-tab5-design.md` (the "Nothing extra runs on the Mac" line)
- Modify: `firmware/main/companion/cc_cmd.c`, `cc_cmd.h` (`cc_cmd_watch`), `firmware/test/host/test_cc_cmd.c`
- Modify: `firmware/main/companion/cc_link.c`, `cc_link.h` (watch channel, liveness, fallback, new getters)
- Modify: `firmware/main/companion/cc_console.c` (`companion watch …`)
- Modify: `firmware/main/ui/ui_dashboard.c` (take-over sets rate 15 instead of 0)
- Modify: `firmware/tools/ui_preview/preview.c`, `firmware/tools/ui_preview/CMakeLists.txt` (stubs for the new calls)
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
    char *body; size_t cap, len;   /* the watch's own PSRAM buffer of JSON_CAP; NUL-terminated at BLOCK */
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
  - `void cc_poll_set_floor(int seconds);` makes the effective interval `floor > 0 ? floor : rate`. Task 5 sets 15 in Off and 0 otherwise.
  - `uint32_t cc_poll_generation(void);` counts successful results, from blocks or polls.
  - `int64_t cc_poll_last_ok_ms(void);` (from Task 3) also covers blocks.
  - `bool cc_link_busy(void);` is true while a paste or live check is in flight, as a power hold.
  - `void cc_link_watch_enable(bool on);` and `const char *cc_watch_status_text(char *buf, size_t cap);` write `watch: on I=3s, last block 4s ago, heartbeat 12s ago`, `watch: starting`, `watch: fallback (<why>), polling every 3s` or `watch: off`.
- Link-task behaviour, which replaces the timed `refresh()` while connected:
  0. The watch owns a separate PSRAM body buffer of `JSON_CAP` for `cc_watch_t`, so the `refresh()` calls of `do_paste`, `cc_link_check_live` and the fallback never overwrite a half-read block. Parsing into `s_parse` on the link task stays as it is.
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
     - three channel ends the Tab5 didn't ask for happen within 5 min (EOF, `BYE eof`, `BYE quiet` or a read error). `BYE life` and the Tab5's own closes (an interval change, Off and wake, `companion watch off`, `cc_link_park`, the host-key forget) don't count.
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
  - `test_max_lifetime`: L 5 with keepalives → `@@CC-BYE life` within L + I + 1 = 7 s.
  - `test_hangup_kills_children`: slow fake, close the pty master after 1.5 s → the shell and the fake `claude` are both gone within 2 s.
  - `test_suspended_loop_exits_on_resume` (Review Focus 2): T 3, keepalives every second; then SIGSTOP the process group for 6 s while writing nothing (a sleeping Mac gets no keepalives), then SIGCONT → exits with `quiet` within I + 1 s of resuming.
  - `test_tilde_path`: claude path `~/bin/claude`, with `HOME` set to a temp dir holding `bin/claude` → a BLOCK arrives.

- [ ] **Step 3: Run them to make sure they fail**

Run the host test command, then the Python tests.
Expected: the C tests FAIL (`cc_cmd_watch` and `cc_watch_*` undefined), and the loop tests report skipped (`cc_cmd_print` missing).

- [ ] **Step 4: Implement `cc_cmd_watch`, `cc_watch.c` and `cc_cmd_print.c`**, then run both suites until they pass. The loop tests must run and pass, not skip.

- [ ] **Step 5: Implement the link side, the console and the take-over rate**, then build and flash

The console gets `companion watch on|off|status`, plus `companion watch test-fallback`: a debug subcommand that sets the next start's banner timeout to 1 ms. Add the preview stubs for the new calls and build the preview (`firmware/tools/ui_preview/README.md`).

- [ ] **Step 6: Check the watch on hardware**

On the Mac, start `companion-test-watch` with the Task 3 prompt.

Run: `python3 $T5 "companion watch status"`
Expected: `watch: on I=3s, …`

Run on the Mac (this is the "Mac `ps` count"; it counts only top-level loops, not the `$(…)` fork that shares the argv): `ps -axo pid,ppid,command | awk '/[@]@CC-WATCH/{p[$1]=$2} END{n=0;for(i in p)if(!(p[i] in p))n++;print n}'`
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

Amend the main spec's line "Nothing extra runs on the Mac: no daemon, no helper, …" to add: "the watch loop runs only inside the companion's exec channel and exits on its own (Plan 2)".

- [ ] **Step 8: Check the fallback and the take-over rate**

Run: `python3 $T5 "companion watch test-fallback" "companion watch off" "companion watch on"`. Wait 15 s, then run `companion watch status` and `sessions`.
Expected: `watch: fallback (no banner in 1 ms), polling every 3s`, and `sessions` still updates. Run `companion watch off` and `companion watch on` to restore.

Open `companion-test-watch` full screen from the dashboard (`key enter`).
Expected: `watch: on I=15s`. Press Ctrl+Q (`key ctrl q`).
Expected: `I=3s` again.

Stop `companion-test-watch`.

- [ ] **Step 9: Commit**

```bash
git add firmware/main/companion firmware/main/ui/ui_dashboard.c firmware/test/host firmware/tools/test_watch_loop.py firmware/tools/ui_preview docs/superpowers/specs/2026-10-22-claude-tab5-design.md docs/superpowers/notes/2026-10-23-plan2-rulings.md
git commit -m "feat(companion): long-poll watch channel with keepalive, liveness and fallback"
```

### Task 5: Power ladder

> **Amendment for Task 5 (2026-10-23, IMU-driven deep sleep; A14).** Append this to the extracted brief. It is already folded into the text below.
>
> - `pr_in_t` gains two fields, after `deep_idle_min, hibernate_h`:
>   `int64_t still_ms;` (how long the BMI270 has reported no motion; 0 = unknown or moving) and
>   `int64_t still_hibernate_ms;` (the Hibernate-when-still threshold; 0 = off).
> - `pr_next_state` gains a third Hibernate trigger inside the existing `on_battery && hibernate_ok && (…)` clause, beside the idle-hours and low-battery triggers:
>   `still_hibernate_ms > 0 && still_ms >= still_hibernate_ms && idle >= still_hibernate_ms` (idle = `now_ms - last_input_ms`, so there has been no input for as long as it has been still). It doesn't depend on `hibernate_h`. Held, USB and `idle < off_s` are still checked first, so it only acts from Off or deeper, and never without battery.
> - The power task passes `still_ms = 0` and `still_hibernate_ms = 0`, so the trigger never fires in Task 5. Task 7 supplies both values. Task 5 adds no setting, Settings row or console command for it.
> - One more host test in Step 1, `test_hibernate_when_still`. Long-idle defaults plus `still_hibernate_ms` = 30 min:
>   - state OFF, idle 30 min, still 30 min → HIBERNATE;
>   - idle 30 min, still 29 min → OFF;
>   - idle 29 min, still 45 min → OFF (an input 29 min ago);
>   - idle 30 min, still 0 (unknown) → OFF;
>   - `still_hibernate_ms` 0, idle 45 min, still 45 min → OFF;
>   - hibernate_h 0, idle 30 min, still 30 min → HIBERNATE;
>   - state DEEP_IDLE, idle 70 min, still 40 min → HIBERNATE;
>   - on_battery false with sleep_on_usb, idle 45 min, still 45 min → OFF;
>   - hibernate_ok false, idle 45 min, still 45 min → OFF;
>   - held, idle 45 min, still 45 min → ACTIVE.

**Files:**
- Create: `firmware/main/sys/display_ctl.c`, `firmware/main/sys/display_ctl.h`
- Modify: `firmware/main/sys/power_rules.c`, `power_rules.h`, `firmware/test/host/test_power_rules.c`
- Modify: `firmware/main/sys/power.c`, `power.h` (states including Deep idle and Hibernate, holds, wake, shield, needs-you wake, the v2 touch poll, the pre-Hibernate notice)
- Modify: `firmware/main/sys/power_log.c`, `firmware/main/sys/plog.h` (summary slots for states 0..5 and check-ins 0x80..0x85)
- Modify: `firmware/main/input/keyboard.c`, `keyboard.h` (quiet mode on GPIO50 INT; the swallow check in the drain; `keyboard_post` and `keyboard_usb_hid_report` wake)
- Modify: `firmware/main/sys/screenshot.c` (use `display_ctl_touch()` each time, no cached handle)
- Modify: `firmware/main/app_main.c` (touch INT after the display starts, v1 only; `idle_poll` period from power; remove the `ui_screensaver_arm()` declaration and call)
- Modify: `firmware/main/ui/ui_dashboard.c` (stale cards after a wake, `Waking: reconnecting to Mac`, the take-over hold), `firmware/main/ui/ui_terminal.c` (the SSH hold), `firmware/main/ui/ui_settings.c` (the Power section), `firmware/main/ui/ui_core.c` (`ui_sleep_notice`), `ui_home.c`, `ui_music.c`, `ui.h`
- Modify: `firmware/tools/ui_preview/preview.c`, `firmware/tools/ui_preview/CMakeLists.txt` (stubs for the new calls, a `sleep-notice` case)
- Delete: `firmware/main/ui/ui_screensaver.c`, its entry in `firmware/tools/ui_preview/CMakeLists.txt` and the preview's `standby` case. Settings and Home drop their screensaver rows and tile.
- Modify: `firmware/main/idf_component.yml` (`espressif/esp_lvgl_port: "==2.8.0~1"`; display_ctl depends on its private layout)
- Modify: `firmware/main/sys/power_bench.c` (steps `dim` 90 s, `off-touch-poll` 120 s, `off-touch-awake` 120 s and `off-touch-asleep` 120 s; Task 3's light steps now enter Off; a finished bench calls `power_go(PS_ACTIVE)` before its toast)
- Modify: `firmware/main/sys/power_console.c`

**Interfaces:**
- Produces, in `power_rules.h`:

```c
typedef enum { PS_ACTIVE, PS_DIM, PS_OFF, PS_RADIO_OFF, PS_DEEP_IDLE, PS_HIBERNATE } power_state_t;
/* also the power-log tags 0..5; check-ins log 0x80|state (0x83 Radio off, 0x84 Deep idle, 0x85 Hibernate) */
typedef struct {
    power_state_t state;
    int64_t now_ms, last_input_ms, off_since_ms;   /* monotonic (esp_timer), never wall clock */
    int dim_s, off_s, radio_off_min;
    int deep_idle_min, hibernate_h;                /* 0 = never */
    int64_t still_ms;                              /* BMI270 no-motion duration; 0 = unknown or moving (Task 7 supplies it) */
    int64_t still_hibernate_ms;                    /* Hibernate-when-still threshold; 0 = off (Task 7: pw_still_hib_min) */
    int battery_pct;                               /* -1 = unknown */
    bool held, on_battery, sleep_on_usb, radio_off_enabled;
    bool charging, hibernate_ok;                   /* hibernate_ok = hibernate_supported() (false until Task 6) */
} pr_in_t;
#define PR_LOW_BATTERY_PCT 10
power_state_t pr_next_state(const pr_in_t *in);
/* In order:
   held → ACTIVE; on USB with !sleep_on_usb → ACTIVE; idle < dim_s → ACTIVE; idle < off_s → DIM.
   From here idle >= off_s:
   on_battery && hibernate_ok && ((hibernate_h > 0 && idle >= hibernate_h h) ||
       (still_hibernate_ms > 0 && still_ms >= still_hibernate_ms && idle >= still_hibernate_ms) ||
       (battery_pct >= 0 && battery_pct <= PR_LOW_BATTERY_PCT && !charging)) → HIBERNATE;
   on_battery && deep_idle_min > 0 && idle >= deep_idle_min min → DEEP_IDLE;
   RADIO_OFF when already OFF/RADIO_OFF, on_battery, radio_off_enabled and now - off_since >= radio_off_min min;
   else OFF. Without on_battery, RADIO_OFF and DEEP_IDLE fall back to OFF and HIBERNATE is never chosen.
   HIBERNATE is terminal: the caller never feeds it back in (the device powers down). */
typedef enum { PR_CARDS_LIVE, PR_CARDS_STALE, PR_CARDS_WAKING } pr_cards_t;
pr_cards_t pr_cards_view(bool hibernate_boot, uint32_t gen_now, uint32_t gen_at_wake, bool needs_you_wake);
/* hibernate_boot && gen_now == 0 → WAKING (no cards, "Waking: reconnecting to Mac");
   gen_now > gen_at_wake → LIVE; needs_you_wake → LIVE (its block is fresh); else STALE */
#define PR_SWALLOW_MS 600       /* longer than the A164's 500 ms auto-repeat delay (keyboard.c:365) */
bool pr_swallow(power_state_t state_at_input, int64_t now_ms, int64_t woke_at_ms, int64_t last_swallowed_ms);
/* true when state_at_input != ACTIVE, or now - woke_at < PR_SWALLOW_MS, or now - last_swallowed < PR_SWALLOW_MS */
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
  - `int display_ctl_board_version(void);` repeats the BSP's own probe once and caches it (the BSP's `bsp_get_board_version` is static): ST7123 touch answers with FW 3 → 2, FW 1 → 3; GT911 at its backup address → 1; else 0. This Tab5 returns 2.
  - `void display_ctl_touch_int_init(void (*isr)(void *));`. **v1 only** (GT911): it reconfigures GPIO23 as an input with pull-up, as generative-tab5 `prov_device.c:313` does, and adds a NEGEDGE ISR. On other versions the ST7123 driver owns GPIO23, so it adds none and logs `touch wake: polling on board vN` (PT1).
  - `esp_err_t display_ctl_touch_poll(bool *pressed);` (v2 and v3): `esp_lcd_touch_read_data(display_ctl_touch())`, then `esp_lcd_touch_get_data(..., &cnt, 1)`; `*pressed = cnt > 0`. It is called only while LVGL is paused, so it never races the LVGL read callback.
  - `screenshot.c` calls `display_ctl_touch()` each time it needs the handle, and caches nothing.
- Produces, in `power.h`:
  - `power_state_t power_state(void);` and `uint32_t power_wake_seq(void);` (bumped on every wake).
  - `typedef enum { PW_KEY, PW_TOUCH, PW_LIFT, PW_NEEDS_YOU, PW_CONSOLE } power_wake_t;` and `power_wake_t power_wake_reason(void);`.
  - `bool power_take_key(void);` is called by the keyboard drain on the LVGL task. It returns false when the key must be dropped, using `pr_swallow`. A drop in Dim wakes.
  - `void power_wake(power_wake_t why);` and `void power_wake_from_isr(power_wake_t why);`. Any of these wakes resets idle.
  - `typedef enum { POWER_HOLD_TAKEOVER, POWER_HOLD_SSH } power_hold_t;` and `void power_hold(power_hold_t h, bool on);`. `POWER_HOLD_SSH` counts only while `g_settings.ss_disable_in_ssh`. `cc_link_busy()` is polled as the third hold.
  - `int power_poll_period_ms(void);` is 1000, or 10 000 in Off, Radio off and Deep idle while logging is off.
  - `void power_go(power_state_t s);` is used by the console and the bench.
  - The keyboard light calls `pr_kbd_light` with `screen_off = (state >= PS_OFF) && on_battery` and `deep_idle = state >= PS_DEEP_IDLE`.
  - The power task fills `pr_in_t` from `sysinfo_power()` (`battery_pct` = `percent`, `charging`, `on_battery = src == PWR_BATTERY`), the settings and `hibernate_ok = false` (Task 6 replaces it with `hibernate_supported()`). It passes `still_ms = 0` and `still_hibernate_ms = 0`, so the stillness trigger never fires yet; Task 7 supplies both (the IMU's no-motion time and `pw_still_hib_min`). Task 5 adds no setting, row or console command for it.
  - **Touch wake on v2** (PT1): while the state is Off or deeper and `display_ctl_board_version() != 1`, the power task calls `display_ctl_touch_poll` every 100 ms and calls `power_wake(PW_TOUCH)` on the first `pressed`. A read error is counted, logged once, and never wakes. On v1 the GPIO23 ISR does the same.
  - **Deep idle** in this task is Off plus the Deep-idle light rule. Task 6 adds the display teardown and low CPU clock, and Task 8 the check-in backoff.
  - **Hibernate** in this task: entering it (from the rule, or `power go hibernate`) shows `ui_sleep_notice(true)` for 3 s, then calls `hibernate_now()`. Until Task 6 that is a stub that logs `hibernate: not built` and returns, and the state goes back to Off. Input during the 3 s cancels it: the notice goes away and the device wakes to Active as for any input. A hold that appears during the 3 s cancels it too.
- Produces, in `ui.h`: `void ui_sleep_notice(bool show);`. It draws a full-screen object on `lv_layer_top()`, bg `#0e1116`, with the centred fg text `Sleeping to save battery. Press the side button to wake.`. Before showing it, power resumes LVGL and lights the panel at `pr_dim_brightness(g_settings.brightness)`.
- Produces, in `power_log.c` and `plog.h`: the summary table holds tags 0..5, 0x10..0x1F and 0x80..0x85 (`SLOT_COUNT` 28). The NVS `sum` blob changes size, and the existing exact-size check drops an old one. The `plog.h` tag comment lists 0..5 and 0x80|state (0x83..0x85).
- Dashboard: it reads `pr_cards_view(hibernate_boot_kind() == POWER_BOOT_HIB_WAKE, cc_poll_generation(), gen_at_wake, reason == PW_NEEDS_YOU)`. Until Task 6, `hibernate_boot_kind()` is a stub that returns `POWER_BOOT_NORMAL`. WAKING draws no cards and the muted summary `Waking: reconnecting to Mac`; STALE is the `LV_OPA_50` + `Updating…` view below; LIVE is normal.
- Produces, in `keyboard.h`: `void keyboard_set_quiet(bool quiet);`. The drain timer calls `power_take_key()` before `keyboard_dispatch` and drops the event when it returns false. After queuing, `keyboard_post` (the `key` console command) and `keyboard_usb_hid_report` call `power_wake(PW_KEY)` when `power_state()` is Off or Radio off, because the stopped LVGL timer no longer drains them; the drain then swallows the event as usual.
- Off entry, under `bsp_display_lock`:
  1. `display_ctl_panel_on(false)`;
  2. `bsp_display_brightness_set(0)`;
  3. `lvgl_port_stop()`;
  4. `keyboard_set_quiet(true)`: the task waits on a GPIO50 NEGEDGE semaphore with a 1 s safety poll, clears `REG_INT_STA` after reading, and calls `power_wake(PW_KEY)` when it reads an event;
  5. `cc_poll_set_floor(15)`;
  6. arm the needs-you set with `pr_needs_you_ids` of the current snapshot;
  7. on v2, start the 100 ms touch poll (after step 3, so LVGL is already paused).
  - Touch stays awake: never `bsp_display_enter_sleep` or `bsp_touch_enter_sleep`, except in the bench step `off-touch-asleep`. That step calls `esp_lcd_touch_enter_sleep(display_ctl_touch())`, then `exit_sleep` afterwards, and logs `touch after exit_sleep: ok|dead` from an I²C read of the controller (`esp_lcd_touch_read_data` == ESP_OK).
  - Bench steps: `off-touch-poll` is Off as it ships on v2 (the poll running). `off-touch-awake` is Off with the poll paused, so the difference is the poll's cost. `off-touch-asleep` is as above. On v1, `off-touch-poll` returns false from `enter` (skipped: not built).
- Wake, in this order (the state stays non-Active until the last step, so the drain keeps swallowing):
  1. set `woke_at`, so `pr_swallow` drops the waking input;
  2. stop the touch poll (v2), so it never overlaps LVGL's own touch read;
  3. `lvgl_port_resume()`;
  4. panel on;
  5. `bsp_display_brightness_set(g_settings.brightness)` (or `pr_dim_brightness` for a needs-you wake);
  6. `lv_display_trigger_activity(NULL)`;
  7. `power_wake_seq++`;
  8. `cc_poll_set_floor(0)`;
  9. `keyboard_set_quiet(false)`.
  - Wake from Deep idle is the same, plus the light going back to `pr_kbd_light` with `deep_idle` false.
  - A needs-you wake sets `last_input_ms = now - dim_s * 1000`, so the ladder lands in Dim. If the screen isn't the dashboard, it calls `ui_nav_home()`. It fires only when `g_settings.pw_wake_needs_you` is set and `pr_needs_you_new(armed, now)` is true, and it then adds the new ids to `armed`.
- Wake shield: entering any non-Active state creates a full-screen transparent clickable object on `lv_layer_top()` if none exists. A press on it wakes the device. It is deleted on the release that follows the wake, or `PR_SWALLOW_MS` after the wake when no press is down. A touch on a dimmed screen therefore never reaches a card.
- Stale cards: when `power_wake_seq()` changes, the dashboard records `gen_at_wake = cc_poll_generation()`. While `pr_cards_view` says STALE it draws the card list at `LV_OPA_50` and shows the muted summary `Updating…`, until `cc_poll_generation()` moves past `gen_at_wake`.
- Holds: `open_now` sets `power_hold(POWER_HOLD_TAKEOVER, true)` and `session_closed_cb` clears it. `ui_terminal.c` sets `POWER_HOLD_SSH` wherever it sets `g_ssh_session_active`.
- Settings → Power, a new category, has these rows:
  - Dim after (s), 10–600;
  - Screen off after (s), from dim + 10 to 3600;
  - Sleep on USB power;
  - Wake when a session needs you;
  - Keyboard light shows needs-you (moved here from Task 3);
  - Stay awake in SSH Terminal (`ss_disable_in_ssh`);
  - Time zone: a menu of `vitals_tz_presets`, with the current rule shown;
  - Deep idle after (min): Off, 15–720;
  - Hibernate after (h): Off, 1–48. Until Task 6 builds Hibernate, the row's value reads `(not available yet)`.
  - Tasks 6, 7 and 8 add their rows here.
- Console:
  - `power go <active|dim|off|deep-idle|hibernate>` (`hibernate` prints `hibernate: not built` until Task 6);
  - `power sim touch` sends a synthetic press and release at the centre of the selected card through a virtual LVGL pointer, so the shield has to eat it;
  - `power set <dim|off|sleep-usb|needs-wake|deep-idle|hibernate> <n>` applies the same clamps as Settings (dim 10–600, off from dim + 10 to 3600, deep-idle 0 or 15–720 min, hibernate 0 or 1–48 h);
  - `power set defaults` restores every Plan 2 power setting to its Task 2 default;
  - `power settings` prints every Plan 2 power setting as `name=value` on one line;
  - `power sim battery <pct|off>` overrides `battery_pct` and `on_battery` in `pr_in_t` (RAM only; `off` ends the override) so the long-idle rules can be checked on USB;
  - `power sim idle <seconds>` moves `last_input_ms` that far into the past (and `off_since_ms` with it when already Off), so long idle times can be checked without waiting;
  - `power touch` prints the touch poll's state: `touch: poll on|off, reads=<n> errors=<n> last=<pressed|released|error>`;
  - `power` now prints `state`, `idle` and `holds`;
  - `power wakes` prints the last 8 wakes as `<seconds ago> <reason> swallowed=<n>`.

- [ ] **Step 1: Write the failing host tests** (added to `test_power_rules.c`)
  - `test_ladder_thresholds`: dim 30, off 120 → idle 29 s ACTIVE, 30 s DIM, 120 s OFF.
  - `test_held_stays_active`: idle 500 s, held → ACTIVE. Also from DIM.
  - `test_usb_without_sleep`: on_battery false, sleep_on_usb false, idle 500 s → ACTIVE. With sleep_on_usb true → OFF.
  - `test_radio_off_needs_battery` (Review Focus 3): state OFF, off_since 21 min ago, radio_off_enabled, on_battery false → OFF; on_battery true → RADIO_OFF; radio_off_enabled false → OFF; and RADIO_OFF with on_battery false → OFF.
  - `test_swallow_rules`: DIM → true; OFF → true; ACTIVE 100 ms after a wake → true; ACTIVE 601 ms after a wake with no swallow since → false.
  - `test_swallow_held_key_repeats` (Review Focus 1): a wake at t = 0, then keys every 60 ms from t = 500 to 1500 (auto-repeat), each swallowed one feeding `last_swallowed` → all swallowed; then a key at t = 2200 → not swallowed.
  - `test_needs_you_new`: armed {a}, now {a} → false; now {a, b} → true; armed {a, b}, now {b} → false; armed {}, now {} → false.
  - `test_dim_brightness`: 80 → 16; 10 → 5; 100 → 20.
  - Long idle. Unless a case says otherwise, these use dim 30, off 120, radio_off_min 20, radio off disabled, deep_idle_min 60, hibernate_h 2, on_battery, not charging, battery 50, hibernate_ok and nothing held.
  - `test_deep_idle_after_minutes`: state OFF, idle 59 min → OFF; idle 60 min → DEEP_IDLE; deep_idle_min 0 and idle 90 min → OFF; with radio off enabled and off_since 40 min ago, idle 60 min → DEEP_IDLE (Deep idle wins over Radio off).
  - `test_hibernate_after_hours`: idle 2 h → HIBERNATE; idle 119 min → DEEP_IDLE; hibernate_ok false and idle 5 h → DEEP_IDLE; hibernate_h 0 and idle 5 h → DEEP_IDLE.
  - `test_hibernate_low_battery`: idle 200 s with battery 10 → HIBERNATE; battery 11 → OFF; battery 10 while charging → OFF; battery 10 with idle 60 s → DIM (the low-battery trigger acts only from Off or deeper); battery 10 with hibernate_h 0 → HIBERNATE (that trigger doesn't depend on hibernate_h); battery 10 with hibernate_ok false → OFF.
  - `test_long_idle_needs_battery` (Review Focus 3): on_battery false, sleep_on_usb true, battery -1, idle 10 h → OFF (no DEEP_IDLE and no HIBERNATE); the same with battery 5 → OFF; state DEEP_IDLE with on_battery false → OFF; on_battery true with battery -1 and idle 200 s → OFF (an unknown % never counts as low).
  - `test_long_idle_held`: held, idle 10 h, battery 5 → ACTIVE; held from DEEP_IDLE → ACTIVE.
  - `test_hibernate_when_still` (Amendment A14). Long-idle defaults plus `still_hibernate_ms` = 30 min: state OFF, idle 30 min, still 30 min → HIBERNATE; idle 30 min, still 29 min → OFF; idle 29 min, still 45 min → OFF (an input 29 min ago); idle 30 min, still 0 (unknown) → OFF; `still_hibernate_ms` 0, idle 45 min, still 45 min → OFF; hibernate_h 0, idle 30 min, still 30 min → HIBERNATE (independent of hibernate_h); state DEEP_IDLE, idle 70 min, still 40 min → HIBERNATE; on_battery false with sleep_on_usb, idle 45 min, still 45 min → OFF; hibernate_ok false, idle 45 min, still 45 min → OFF; held, idle 45 min, still 45 min → ACTIVE.
  - `test_cards_view_after_hibernate` (Review Focus 5): (true, 0, 0, false) → WAKING; (true, 1, 0, false) → LIVE; (false, 5, 5, false) → STALE; (false, 6, 5, false) → LIVE; (false, 5, 5, true) → LIVE; (false, 0, 0, false) → STALE.

- [ ] **Step 2: Run them to make sure they fail**

Run the host test command.
Expected: FAIL

- [ ] **Step 3: Implement the rules** and run the host tests until they pass

- [ ] **Step 4: Implement `display_ctl`, the power states (Deep idle and Hibernate included), the v2 touch poll, the quiet keyboard, the shield, stale cards, the sleep notice, holds and the Settings → Power section, and remove the screensaver**

Keep the `ss_*` fields in `app_settings_t`, marked reserved, so old blobs load. `ui_keyboard_route` and the gesture callback lose their screensaver branches, and `app_main.c` loses its `ui_screensaver_arm()` declaration and call. Add the `hibernate_now()` and `hibernate_boot_kind()` stubs in `power.c` (Task 6 moves them to `sys/hibernate.c`). Build, flash, and also build the preview (no `standby` case). Render `./preview sleep-notice`, convert it with `sips` and look at it: the text is fg on bg, centred, with no amber.

- [ ] **Step 5: Check the ladder with short timings**

Run: `python3 $T5 "power settings"`.
Expected: the Task 2 defaults, including `deep-idle=60 hibernate=2 hib-checkin=0`.

Run: `python3 $T5 "power set dim 10" "power set off 20"`. Then run `python3 $T5 "power"` at 5 s, 15 s and 25 s.
Expected: `state=active`, then `state=dim`, then `state=off`. At `state=off`, `screenshot` still works (it is an LVGL snapshot), and `power touch` shows `poll on` with `reads` climbing by about 10 a second and `errors=0` (the ST7123 still answers with the panel off).

Run: `python3 $T5 "power sim battery 50" "power sim idle 3600" "power" "power light"`.
Expected: `state=deep-idle`, and `light: 1 0 0 0 0 (nobody needs you)`.

Run: `python3 $T5 "power sim battery 8" "power"`.
Expected: still `state=deep-idle`: `hibernate_ok` is false until Task 6, so the low-battery trigger can't fire yet.

Run: `python3 $T5 "power sim battery off" "power go hibernate"` and take a screenshot within the 3 s.
Expected: the dark screen with the fg text `Sleeping to save battery. Press the side button to wake.`; after 3 s the log says `hibernate: not built` and `power` shows `state=off`.

Run: `python3 $T5 "power go hibernate"` and, within 3 s, `python3 $T5 "key text z"`.
Expected: the notice goes, `state=active`, `power wakes` shows `key swallowed=1`, and no `z` was typed anywhere.

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
Expected: `holds=takeover` and `state=active`. Then run `python3 $T5 "power sim battery 8" "power sim idle 7200" "power"`.
Expected: still `holds=takeover` and `state=active`: a hold blocks Deep idle and Hibernate too. Run `power sim battery off`, then Ctrl+Q.

- [ ] **Step 8: Check touch wake through the shield**

Run: `python3 $T5 "power go dim" "power sim touch" "power wakes"`.
Expected: `touch swallowed=1`, and no card action: the selected card didn't open and nothing was sent. The real-finger check is Task 10 Step 4.

- [ ] **Step 9: Restore and commit**

Run `python3 $T5 "power sim battery off" "power set defaults"` and stop both test sessions.

```bash
git add -A firmware docs/superpowers/notes/2026-10-23-plan2-rulings.md
git commit -m "feat(power): Active/Dim/Off ladder with Deep idle and Hibernate states, swallowed wake input, v2 touch poll, needs-you wake and stale cards; remove the screensaver"
```

### Task 6: Light-sleep and hibernate spike (light sleep expected to fail)

Two parts. **Part A** is the display-teardown and light-sleep spike, as before; if its teardown checks pass, Deep idle also tears the display down and lowers the CPU clock. **Part B** finds out how this board can hibernate and wake, and builds `sys/hibernate.c` on what works. It starts (Steps 4a–4c, ahead of everything else in Part B) by finding where the BMI270's INT pins go, whether the board has a real power-off, and which rails the IO expanders can cut, because Hibernate's wake path and its floor depend on all three. Each part records its outcome in the notes, pass or fail.

**Files:**
- Modify: `firmware/main/sys/display_ctl.c`, `display_ctl.h` (teardown and re-create)
- Create: `firmware/sdkconfig.spike` (`CONFIG_PM_ENABLE=y`, `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`, `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y`, `CONFIG_CC_LIGHT_SLEEP_SPIKE=y`)
- Create: `firmware/main/Kconfig.projbuild` (`CONFIG_CC_LIGHT_SLEEP_SPIKE`, default n; `CONFIG_CC_DEEP_IDLE_TEARDOWN`, default n, depends on `PM_ENABLE`)
- Modify: `firmware/.gitignore` (add `build-spike/`)
- Modify: `firmware/sdkconfig.defaults` (only if Step 3 keeps the Deep-idle teardown: `CONFIG_PM_ENABLE=y`, `CONFIG_CC_DEEP_IDLE_TEARDOWN=y`; tickless idle stays spike-only)
- Modify: `firmware/main/ui/ui_settings.c` (the "Light sleep (experimental)" row; the Hibernate row now shows its value)
- Create: `firmware/main/sys/hibernate.c`, `firmware/main/sys/hibernate.h` (replaces Task 5's stubs in `power.c`; the rail table and the cuts)
- Create: `firmware/main/sys/imu.c`, `firmware/main/sys/imu.h` (raw BMI270 register access, the INT probe, any-motion and data-ready on a pin; Task 7 extends it)
- Modify: `firmware/main/idf_component.yml` (`espressif/bmi270: "==1.1.0"`, today only a BSP dependency; this rewrites `firmware/dependencies.lock`)
- Modify: `firmware/main/sys/power.c` (`hibernate_ok = hibernate_supported()`; Deep idle teardown and CPU clock), `power_bench.c`, `power_bench.h` (steps `light-sleep`, `floor` and `hibernate`; `power_bench_boot()`), `power_console.c`
- Modify: `firmware/main/sys/sysinfo.c`, `sysinfo.h` (INA226 slow averaging, power-down, and a raw read for the early boot)
- Modify: `firmware/main/app_main.c` (`hibernate_boot_early()` right after `bsp_i2c_init()`, before the first `bsp_feature_enable`; boot-to-live timing)
- Modify: `firmware/main/settings/settings.c` (only if Step 12 turns the Hibernate default off)
- Modify: `firmware/tools/ui_preview/preview.c`, `firmware/tools/ui_preview/CMakeLists.txt` (stubs for the new calls)
- Create: `docs/superpowers/notes/2026-10-23-light-sleep-spike.md` (both parts)

**Interfaces:**
- Part A, unchanged from before: `esp_err_t display_ctl_teardown(void);` and `esp_err_t display_ctl_recreate(void);`, under `#if CONFIG_CC_LIGHT_SLEEP_SPIKE || CONFIG_CC_DEEP_IDLE_TEARDOWN`.
  - Teardown, under the LVGL lock: `lvgl_port_remove_touch`, `lvgl_port_remove_disp`, `bsp_display_delete()` (panel, IO, DSI bus, PHY LDO, backlight PWM) and `esp_lcd_touch_del`.
  - Re-create: `bsp_display_new_with_handles` with the board's lane rate, then `lvgl_port_add_disp_dsi` with the app's Plan 1 config, then `bsp_touch_new` + `lvgl_port_add_touch`, then `ui_nav_refresh()`.
  - The lane rate is the BSP default on this board (v2). Only board version 3 (ST7121) needs 965 Mbps (`BSP_LCD_ST712X_MIPI_DSI_LANE_BITRATE_MBPS`).
  - The ST7123 is one chip for display and touch. So teardown also checks whether touch still answers an I²C read after the panel is deleted, and keeps the touch handle (no `esp_lcd_touch_del`) when it does, so the v2 poll keeps working. If it doesn't answer, Deep idle has no touch wake (keys and lift still wake). Record which.
  - With `pw_light_sleep` set, on battery only, Off calls teardown and configures `esp_pm_configure({.max_freq_mhz = 360, .min_freq_mhz = 40, .light_sleep_enable = true})` plus `gpio_wakeup_enable(GPIO50, GPIO_INTR_LOW_LEVEL)`, `gpio_wakeup_enable(GPIO23, GPIO_INTR_LOW_LEVEL)` (v1 only) and `esp_sleep_enable_gpio_wakeup()`. Before every sleep it clears the A164's `REG_INT_STA`. Wake re-creates the display, then calls `display_ctl_touch_int_init` again on v1 (`bsp_touch_new` sets GPIO23 back to output-low there).
  - **Deep idle teardown** (`CONFIG_CC_DEEP_IDLE_TEARDOWN`, kept only if Step 3 passes the teardown checks): entering Deep idle calls `display_ctl_teardown()` and releases power's own `ESP_PM_CPU_FREQ_MAX` lock (`s_pm_max`, held in every other state) after `esp_pm_configure({.max_freq_mhz = 360, .min_freq_mhz = 40, .light_sleep_enable = false})`, so the CPU drops to 40 MHz once nothing else holds a lock. Waking takes the lock first, then calls `display_ctl_recreate()`, then runs the usual wake steps. Wake-to-picture must stay under 500 ms.
- `screenshot.c` already asks `display_ctl_touch()` each time, so it never holds a dangling handle. In Deep idle with teardown it reports `screenshot: display torn down`.
- Part B, in `hibernate.h`:

```c
typedef enum { HIB_SRC_TIMER = 1, HIB_SRC_BUTTON = 2, HIB_SRC_RESET = 4, HIB_SRC_RTC = 8, HIB_SRC_IMU = 16 } hib_src_t;
typedef enum { HIB_MODE_NONE, HIB_MODE_POWEROFF, HIB_MODE_DEEPSLEEP } hib_mode_t;
typedef enum { POWER_BOOT_NORMAL, POWER_BOOT_HIB_WAKE, POWER_BOOT_HIB_CHECKIN, POWER_BOOT_HIB_PROBE } power_boot_t;
bool hibernate_supported(void);         /* mode != NONE and a wake source other than the timer works */
hib_mode_t hibernate_mode(void);
uint32_t hibernate_wake_sources(void);  /* hib_src_t bits that Step 9 proved; a compile-time table from the notes */
void hibernate_now(uint32_t wake_after_s, power_boot_t next);
/* 0 = no timed wake. Saves the power-log sums, writes NVS ns "power" key "hib"
   {mode, next, epoch, wake_after_s}, sets the light by the Deep-idle rule, cc_link_park(true),
   wifi off, then arms the wake sources (the IMU only for `power hib test imu*` until Task 7),
   suspends the BMI270 when it isn't a wake source (imu_suspend), cuts every rail in
   hibernate_rails() marked cut (LCD_EN, TOUCH_EN, speaker, camera, USB_EN, then WIFI_EN, so the
   C6 is fully off, plus the keyboard's rail if Step 4c finds a switch), puts the INA226 into
   power-down (sysinfo_ina226_power_down) unless a bench probe is running, then a full
   power-off or esp_deep_sleep_start(). Never returns. The cold boot re-enables the rails
   through the usual staggered bsp_feature_enable calls. */
power_boot_t hibernate_boot_kind(void); /* from "hib" + esp_sleep_get_wakeup_cause() (or the RX8130 AF flag after power-off); cleared after it is read */
uint32_t hibernate_wake_src(void);      /* the hib_src_t bit that woke this boot (ext1 on the IMU's LP GPIO → HIB_SRC_IMU), 0 on a normal boot */
typedef struct { const char *name; uint8_t addr; uint8_t pin; bool cut; } hib_rail_t;
const hib_rail_t *hibernate_rails(int *n);
/* compile-time table from Step 4c. Known from the BSP: 0x43 P1 speaker, P4 LCD_EN, P5 TOUCH_EN,
   P6 camera; 0x44 P0 WIFI_EN, P3 USB_EN; 0x44 P4 is the charge-status input and is never driven.
   Step 4c adds the rest (the keyboard's power among them) and marks each one cut or kept. */
void hibernate_boot_early(void);        /* runs before the rails come up; calls nvs_flash_init() itself (idempotent).
                                           POWER_BOOT_HIB_PROBE: read the INA226 through its own device on
                                           bsp_i2c_get_handle(), store the sample in the bench row, sleep again or finish */
uint32_t hibernate_boot_to_live_ms(void); /* boot to the first rc-0 result, kept in "hib" for the last boot */
```

- In `sysinfo.h`: `void sysinfo_ina226_slow(bool slow);` (slow: config `0x4E3F` = 1024 averages, bus 140 µs, shunt 8.244 ms, continuous; one result every ≈ 8.6 s; otherwise the Plan 1 config `0x4527`). `sysinfo_init` writes the Plan 1 config, so a probe boot that goes on to a normal boot is back to fast sampling. `void sysinfo_ina226_power_down(void);` writes config `0x4520` (mode 000, power-down; the cold boot's `sysinfo_init` writes `0x4527` again).
- Produces, in `imu.h` (Part B, Steps 4a and 7; Task 7 extends it). The espressif/bmi270 1.1.0 driver has no interrupt API, so everything past `bmi270_create` is raw register access through the public `bmi270_handle_t.i2c_handle`:

```c
esp_err_t imu_init(void);     /* bmi270_create at BMI270_I2C_ADDRESS_L (0x68) on bsp_i2c_get_handle() (uploads the feature blob); idempotent, called on first use */
esp_err_t imu_reg_read(uint8_t reg, uint8_t *buf, size_t n);
esp_err_t imu_reg_write(uint8_t reg, const uint8_t *buf, size_t n);   /* waits 450 us after a write while PWR_CONF.adv_power_save is set */
esp_err_t imu_feat_write(uint8_t page, uint8_t off, const uint8_t *buf, size_t n);   /* FEAT_PAGE 0x2F, then 0x30 + off; reads back and compares */
typedef enum { IMU_INT1 = 1, IMU_INT2 = 2 } imu_pin_t;
esp_err_t imu_pin_config(imu_pin_t p, bool output, bool active_high, bool open_drain, bool latched);
/* INT1_IO_CTRL 0x53 / INT2_IO_CTRL 0x54: bit1 lvl, bit2 od, bit3 output_en; INT_LATCH 0x55 bit0 */
esp_err_t imu_any_motion_arm(imu_pin_t p, uint16_t thr, uint16_t dur);
/* accel on (PWR_CTRL 0x7D bit2), ACC_CONF 0x40 = 0x17 (50 Hz, avg2, power-optimised), then the any-motion
   feature: page 1, offset 0x0C, word0 = dur (13 bits, 20 ms units) | x,y,z select (bits 13-15),
   word1 = thr (11 bits, 1 g / 2048 per LSB) | enable (bit 15); mapped by INTx_MAP_FEAT (0x56 / 0x57) bit6.
   Offsets and bits are Bosch BMI270_SensorAPI bmi270.h facts; imu_feat_write's read-back and the
   Step 4a self-test check them on this blob. */
esp_err_t imu_drdy_map(imu_pin_t p, bool on);   /* INT_MAP_DATA 0x58: drdy on INT1 bit2, INT2 bit6 */
esp_err_t imu_int_status(uint8_t *st0);         /* INT_STATUS_0 0x1C (any-motion bit6, no-motion bit5); clears a latched INT */
esp_err_t imu_suspend(void);                    /* features off, PWR_CTRL 0, adv_power_save on: Hibernate without IMU wake, and `floor` */
typedef struct { int gpio; bool lp; int exp_addr, exp_pin, exp_int_gpio; } imu_route_t;   /* -1 where none */
imu_route_t imu_route(imu_pin_t p);             /* compile-time table from Step 4a */
#define IMU_ANYMOT_THR    0x0AA   /* 83 mg, Bosch's default; start value, Task 10 tunes */
#define IMU_ANYMOT_HIB_DUR 50     /* 50 x 20 ms = 1 s of motion, so a bump doesn't wake Hibernate; start value */
```

- Bench step `floor` (probe-type like `hibernate`, inserted just before it): deep sleep with every rail in `hibernate_rails()` cut (the ones marked kept stay on), the BMI270 suspended, the keyboard light off and only the timer armed. It is the board floor the spec asks for (charger, regulators, IO expanders, INA226, RTC). It reuses the probe mechanism below, with its own row; after its last cycle the probe boot goes straight into the `hibernate` step's first cycle without bringing the rails up. The INA226 stays on in both probe steps (it is the meter), so both rows include its own draw (about 330 µA from its supply, datasheet typical), which a shipped Hibernate doesn't pay; the notes say so.
- Bench step `hibernate`. It is always the **last** step in `k_steps`, and later tasks insert their steps before `floor`. It measures Hibernate as shipped: rails cut as in `floor`, plus the IMU armed with any-motion on its LP GPIO when Step 4a found one (otherwise suspended, and the row equals `floor` plus nothing). On battery its `enter`:
  1. sets `sysinfo_ina226_slow(true)`;
  2. writes the probe record (`cycles_left = 3`, its row index);
  3. calls `hibernate_now(60, POWER_BOOT_HIB_PROBE)` (fast mode: 10 s; `wake_after_s` uses the timer, which deep sleep always has).
  - On each probe boot, `hibernate_boot_early()` reads the INA226's bus and current registers before any rail is enabled. The registers still hold the last 8.6 s average, taken while asleep. It adds that as one sample to the row's sum and logs `hib probe: <mv> mV <ma> mA at boot+<ms> ms`. While cycles remain it sleeps again at once. After the last one it marks the row `ok` (or `usb` when charging), boots on normally, and `power_bench_boot()` shows `Bench done. Plug USB back in.` once the link is live, with the boot-to-live time in the row's `seconds` column.
  - The row's `mwh_x1000` is not meaningful for this step (three 8.6 s windows), and `power_report.py` prints `-` for it.
  - If the device powers off instead of deep-sleeping, the INA226 may lose power too. Then the step reports `no samples`, and the notes record power-off draw as not measurable on the device.
- Settings → Power: the Hibernate row shows its value once `hibernate_supported()`.
- Console:
  - `power spike recreate <n>` runs n teardown/re-create cycles and prints the internal and PSRAM free heap after each one and the slowest wake-to-picture time (the first flush after re-create).
  - `power hib test <timer|rtc|imu|off> [s]`: one hibernate with only that wake source armed (default 30 s for timer and rtc). It prints `hib test: armed <src>, sleeping` and then sleeps.
  - `power hib test-wake <s>` arms a one-shot timer wake for the next hibernate of any kind (RAM only), so a rule-driven hibernate can be tested without a press.
  - `power boot` prints `boot: reason=<r> kind=<normal|hib-wake|hib-checkin|hib-probe> cause=<wakeup cause> to_live=<ms>`.
  - `power hib` prints `hibernate: mode=<off|deepsleep|none> sources=<list> supported=yes|no`.
  - `power hib test imu-drdy`: one hibernate with ext1 armed on the IMU's LP GPIO and data-ready mapped to the INT pin (latched), plus the 30 s timer backstop. Data-ready fires at 50 Hz, so it proves the electrical wake path from the INT pin without any motion.
  - `power imu probe` runs the Step 4a probe and prints its result lines; `power imu selftest` runs the Step 4a self-test attempt; `power imu` prints `imu: chip=0x24 int1=<route> int2=<route> st0=0x<hex>`.
  - `power rails` prints both expanders' pins as `0x43 P4 LCD_EN out 1 cut` (address, pin, name from the table or `?`, direction from register 0x03, level from 0x05 or input 0x0F, cut or kept). `power rail <name> on|off` switches one named rail while awake, only for `speaker`, `camera`, `usb` and a keyboard rail if Step 4c names one; LCD, touch and Wi-Fi are cut only on the hibernate path.

**Part A: display teardown and light sleep**

- [ ] **Step 1: Build the spike** with `idf.py -B build-spike -D SDKCONFIG=build-spike/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.spike" build`, then flash it with `idf.py -B build-spike -p /dev/cu.usbmodem211201 flash`

Expected: `Project build complete`. If it doesn't build (for example `PM_ENABLE` with SMP), that is the result: go to Step 4.

- [ ] **Step 2: Run the USB checks** (light sleep itself can't happen on USB; the console would die)

Run: `python3 $T5 "power spike recreate 10"`.
Expected for a pass:
- 10 cycles with no crash;
- internal free heap within 4 KB of the first cycle and PSRAM within 64 KB;
- the slowest wake-to-picture time under 500 ms;
- a screenshot afterwards shows the dashboard;
- the log line `touch after teardown: ok|dead` (v2: one chip for display and touch).

Then leave it 10 minutes and run `companion status` and `companion watch status`.
Expected: connected, and the watch on.

Also run `power resets`.
Expected: no new panic or brownout.

- [ ] **Step 3: Decide, in two halves**
  - **Teardown** (the re-create, heap and 500 ms checks, plus the 10-minute link check): if they pass, keep `display_ctl_teardown/recreate` in the normal build for Deep idle. Add `CONFIG_PM_ENABLE=y` and `CONFIG_CC_DEEP_IDLE_TEARDOWN=y` to `sdkconfig.defaults`, delete `firmware/sdkconfig`, build the normal build, flash it, and check:
    - `power go deep-idle`, then `power`, shows `cpu=40MHz` (`esp_clk_cpu_freq()`);
    - `key esc` brings the dashboard back with a wake-to-picture under 500 ms (`power wakes` prints it);
    - `power go deep-idle` and `power touch` show whether the poll still reads.
  - **Light sleep** (needs teardown plus the spike build): if every Step 2 check passed, keep that code behind `CONFIG_CC_LIGHT_SLEEP_SPIKE`, with `pw_light_sleep` default off, a Settings → Power row "Light sleep (experimental)" shown only in that build, and the bench step `light-sleep` (120 s, only when the spike is compiled in and `pw_light_sleep` is set). Task 10 runs it on battery.
  - Whatever failed goes to Step 4.

- [ ] **Step 4: Drop what failed**

If teardown failed, revert the teardown code, delete `sdkconfig.spike` and `Kconfig.projbuild`, and leave `pw_light_sleep` reserved. Deep idle then keeps the display (panel off) and the full CPU clock. Drop DFS too: the DPI panel's `ESP_PM_CPU_FREQ_MAX` lock pins the CPU while the panel exists. If only light sleep failed, delete `sdkconfig.spike` and the `CONFIG_CC_LIGHT_SLEEP_SPIKE` code but keep `CONFIG_CC_DEEP_IDLE_TEARDOWN`. Reflash the normal build.

**Part B: hibernate** (the normal build from here on)

Steps 4a–4c run first, ahead of everything else in Part B. They are lettered so that the Step numbers cited elsewhere (Step 9 in Review Focus 5, Steps 5, 7 and 12) stay right.

- [ ] **Step 4a: Find where the BMI270's INT1 and INT2 go, and try to trigger any-motion without moving the device**

On paper first: the Tab5 schematic (M5Stack's Tab5 docs page) for the BMI270's INT1/INT2 nets. The BSP header names no IMU INT pin. The candidates are a P4 GPIO, an LP GPIO (0–15, the only ones that can wake deep sleep through ext1), or an input of one of the two PI4IOE5V6408 expanders (0x43, 0x44), whose own INT line may in turn reach a P4 GPIO.

Write `imu.c` with the Interfaces above (only the Part B parts) and `power imu probe`, add `espressif/bmi270: "==1.1.0"` to `idf_component.yml`, build and flash. The probe, for INT1 and then INT2:
1. takes the candidate GPIOs: every P4 GPIO 0–54 that `gpio_dump_io_configuration` shows unclaimed (no peripheral signal, output off), never 24 and 25 (USB-Serial-JTAG). It sets input enable on them with no pull, and drives nothing;
2. reads them, and both expanders' input (0x0F) and interrupt-status (0x13) registers, with every expander input pin unmasked in its interrupt-mask register (0x11) for the run and the old mask restored after;
3. enables the pin's output (push-pull, nothing mapped, so it sits at its inactive level) and flips its level bit 3 times (active-high ↔ active-low), reading everything after each flip;
4. maps data-ready to the pin for 1 s and counts edges on the candidate GPIOs (a temporary any-edge ISR) and sets of the expanders' interrupt-status bits;
5. turns the pin's output off again and unmaps data-ready.

Run: `python3 $T5 "power imu probe"`.
Expected: for each pin, one line `imu int1: gpio<N> lp=<yes|no>`, or `imu int1: expander 0x4<x> pin <p> (expander int -> gpio<M> lp=<yes|no> | expander int -> none)`, or `imu int1: not found`. A real route follows all 3 flips and shows about 50 edges in the data-ready second (or the expander's status bit). A line that follows only some flips is noise: run the probe twice more and record all three outputs. Fill `imu_route()` from the result.

Then try to fire any-motion itself without a move. Run: `python3 $T5 "power imu selftest"`. It arms any-motion (Interfaces, threshold `IMU_ANYMOT_THR`, duration 5) on the routed pin, runs the BMI270 accelerometer self-test (±16 g, 1600 Hz, performance mode, ACC_SELF_TEST 0x6D positive then negative; the deflection is several g), reads INT_STATUS_0 and the routed pin, then soft-resets and runs `imu_init()` again (the self-test leaves the sensor in a test configuration).
Expected: `any-motion during self-test: yes|no`. The BMI270 has no documented INT test mode, so if it prints `no`, any-motion's trigger by real motion needs a physical move: record "any-motion trigger: Task 10 Step 4". Data-ready (step 4 above) has already proved the pin's routing either way.

Also record which feature blob the driver uploads: compare the length and first 64 bytes of `bmi270_config_file` (`managed_components/espressif__bmi270/include_priv/bmi270_priv.h`) with Bosch BMI270_SensorAPI's `bmi270.c` (the standard blob, with any-motion and no-motion) and `bmi270_maximum_fifo.c` (no features), and whether `imu_feat_write` reads page 1 offset 0x0C back as written. If the blob has no motion features, record it: Task 7 then uploads Bosch's standard blob itself (Task 7 Step 4 has the licence check).

- [ ] **Step 4b: Find whether the board has a real power-off with a wake source**

From the schematic and M5Stack's Tab5 factory firmware (look for a power-off call, a power-latch or "power off pulse" pin on an expander, or a PMIC register), record:
- whether a line powers the whole board off, and which (expander and pin, or register);
- what can switch it back on: the side button, the RX8130 `IRQ`, the IMU INT (into the power circuit), USB power, or only a press.

Don't drive an unknown expander pin to find out. The electrical check is Step 7's `power hib test off`, only once this step has named the line. Power-off beats deep sleep only if something other than USB can switch it back on.

- [ ] **Step 4c: Map the rails the expanders can cut**

From the schematic and `power rails`, record each expander output and what it powers: `LCD_EN`, `TOUCH_EN`, `USB_EN`, `WIFI_EN`, the speaker, the camera, and anything that powers the keyboard (the A164 MCU and its light). Also record any pin that must stay as it is (the charger, the charge-status input, anything feeding the P4, the I²C pull-ups or the expanders themselves). Fill `hibernate_rails()` and mark each one cut or kept. Hibernate cuts every rail it can.

Run: `python3 $T5 "power rails"`.
Expected: every known rail listed with its name, and the unknown pins as `?`.

If a keyboard rail is named, run `python3 $T5 "power rail kbd off" "power light"`, then `python3 $T5 "power rail kbd on"` and wait 3 s, then `python3 $T5 "power light test reprobe" "power light"`.
Expected: the keyboard drops out while off and re-probes with the owner's colour after. Record it. Without a keyboard rail, the keyboard keeps its power in Hibernate and the notes give its share of the floor as "not separable".

Record, too, whether `TOUCH_EN` cuts only the touch side of the one-chip ST7123 or the panel as well (from the schematic; Task 7's touch-off in Deep idle depends on it).

- [ ] **Step 5: Find the other wake sources on paper**

Read the Tab5 schematic and the BSP pin map (`managed_components/espressif__m5stack_tab5/include/bsp/m5stack_tab5.h`). Record in the notes, for each of these:
- the side button: whether it drives the power latch or EN, or reaches a GPIO;
- the reset button;
- the RX8130 `IRQ` output: whether it reaches the power circuit, an LP GPIO (0–15) or nothing;
- the BMI270 INT1/INT2: from Step 4a;
- the power-off line: from Step 4b.

Note too whether the keyboard connector's rail stays up in deep sleep (Step 4c), because a Hibernate check-in's amber light depends on it (Task 8).

- [ ] **Step 6: Build `hibernate.c` and the console**, add the preview stubs, then build and flash

Expected: `Project build complete`, and `power hib` prints a mode and the sources still to be proved.

- [ ] **Step 7: Try each wake path on USB**

Before each one, run `python3 $T5 "power hib test <src> …"`. Then wait, reconnect the console, and run `python3 $T5 "power boot"`:
- `power hib test timer 30` → back within 30–40 s, `kind=hib-wake cause=timer`.
- `power hib test rtc 60` (only if Step 5 found the RTC IRQ routed) → back within 60–70 s.
- `power hib test imu-drdy` (only if Step 4a found the INT on an LP GPIO, directly or through an expander's INT) → back within a few seconds with `kind=hib-wake cause=ext1` and `power boot` naming the IMU as the source. This proves the electrical wake path with ext1 on that GPIO, which Hibernate then uses for the IMU (`hibernate_wake_sources()` gets `HIB_SRC_IMU`).
- `power hib test imu` (the same GPIO, any-motion armed with `IMU_ANYMOT_THR` and `IMU_ANYMOT_HIB_DUR`) → with the device still on the desk it should stay asleep until the timer backstop: `cause=timer`. That shows a still device doesn't wake itself. The pick-up that fires it needs the user, so record "any-motion wake: Task 10 Step 6b" (unless the Step 4a self-test fired any-motion, which still doesn't prove a real pick-up).
- If the INT isn't on an LP GPIO, Hibernate has no IMU wake: record it. The IMU's role shrinks to the lift wake while the P4 is awake and the stillness signal (Task 7).
- `power hib test off` (only if Step 5 found a power-off line) → record whether USB power keeps it on.
- The side button and the reset button need a real press: the user does them in Task 10. The USB-Serial-JTAG peripheral is off in deep sleep and the Tab5 has no USB-UART bridge, so nothing on the Mac can reset a sleeping P4. Every test here therefore arms the timer as a backstop.

Record each result. Fill `hibernate_wake_sources()` and `hibernate_mode()` from them: power-off when it exists and something can wake it, else deep sleep.

- [ ] **Step 8: Check that state survives a hibernate**

Before: run `python3 $T5 "sshkey" "companion status"`, and note the Tailscale IP from the boot log's MicroLink lines.

Run: `python3 $T5 "power hib test timer 20"`, and after it comes back, run the same commands again.
Expected:
- the same `@@PUBKEY` line;
- the same Tailscale IP;
- `companion status` shows `connected <host>` with no `KH_UNKNOWN` line in the log (the host-key pins survived);
- `power settings` unchanged and `power resets` shows one more `deepsleep`;
- every rail came back after the cuts: Wi-Fi joined again (the C6 cold-booted from `WIFI_EN`), `power touch` reads without errors, the keyboard answers `power light`, and `power rails` shows the same levels as before;
- the INA226 reads again (`power` shows volts), so the power-down was undone by `sysinfo_init`.

- [ ] **Step 9: Check the cold boot to a live dashboard**

Run `power hib test timer 10` three times. Each time, take a screenshot as soon as the console is back, then run `power boot`.
Expected:
- the first screenshot shows `Waking: reconnecting to Mac` and no cards (Review Focus 5);
- `to_live` is recorded for each run (the spec says 50–130 s today);
- record all three.

- [ ] **Step 10: Check the bench's hibernate step on USB**

Run: `python3 $T5 "power bench start fast"`. When the console comes back after the probe cycles, run `python3 firmware/tools/power_report.py --port /dev/cu.usbmodem211201 --bench`.
Expected: a `floor` row and then a `hibernate` row, each with 3 samples, status `usb` (charging on USB), and `-` for mWh; the `hibernate` row has the boot-to-live time in `seconds`. The console drops for both, six probe cycles in all. Every row above them is unchanged from Task 5.

- [ ] **Step 11: Check the long-idle rule now that Hibernate exists**

Run: `python3 $T5 "power hib test-wake 30" "power sim battery 8" "power set off 20"`. Wait 25 s.
Expected: the notice for 3 s, then hibernate (the console drops), then back within about 30 s with `power boot` showing `kind=hib-wake`. The `sim battery` override is RAM only, so it is gone after the boot. Run `power set defaults`.

- [ ] **Step 12: Decide the Hibernate default and write the notes**
  - `hibernate_h` stays at 2 only if a wake path that needs nothing more than the side button exists on paper (Step 5) and its electrical path checked out in Step 7. Task 10 confirms it with a real press. Otherwise set the default to 0 in `settings.c` and run `power set hibernate 0` (the device already has CFG 18, so the code default alone wouldn't reach it). The battery ≤ 10% trigger stays on whenever `hibernate_supported()`.
  - Record which path that is: power-off with the side button, deep sleep with the button on an LP GPIO, or "reset only".
  - Write `docs/superpowers/notes/2026-10-23-light-sleep-spike.md`, covering:
    - Part A: what was tried, each check's result with numbers, the teardown and light-sleep decisions and why, and touch after teardown. Also record that `lvgl_port_remove_disp` deletes every screen of the display (the nav stack, the layer-top shield and toasts), and that `ui_nav_refresh()` rebuilds only the top screen. If light sleep failed, say that Off ships as panel off + quiet tasks + modem power save + the long poll + the Radio-off tier.
    - Part B: each answer from Steps 4a–4c as its own line (INT1 route, INT2 route, LP or not, expander INT routing, the self-test result, the feature blob; power-off yes/no with its line and wake sources; the rail map with each rail cut or kept, the keyboard rail, and whether `TOUCH_EN` takes the panel too); each wake source (on paper, then tried, `imu-drdy` included); the mode chosen; the survival checks with the rails cut; the three boot-to-live times; the probe's USB plumbing result for `floor` and `hibernate`; whether the keyboard rail stays up in deep sleep; and the Hibernate default with the path it relies on.

- [ ] **Step 13: Commit**

```bash
git add -A firmware docs/superpowers/notes/2026-10-23-light-sleep-spike.md docs/superpowers/notes/2026-10-23-plan2-rulings.md
git commit -m "spike(power): display teardown <kept for Deep idle | dropped>, light sleep <kept behind a setting | dropped>, hibernate via <power-off | deep sleep>, see notes"
```

### Task 7: Lift wake, stillness and the IMU wake from Hibernate (interrupt-first)

The BMI270 does the watching. Its any-motion and no-motion features run on the sensor with the accelerometer in low-power mode, and the P4 hears about them on the INT pin that Task 6 Step 4a found. Polling is only the fallback when the INT isn't wired. On top of that, this task supplies `still_ms` to the ladder (Task 5's stillness trigger), starts the link early on a pick-up, and guards the IMU wake from Hibernate against false wakes.

**Files:**
- Create: `firmware/main/sys/lift_detect.c`, `firmware/main/sys/lift_detect.h` (pure)
- Create: `firmware/main/sys/lift.c`, `firmware/main/sys/lift.h` (the `lift` task: INT-driven, polling fallback)
- Create: `firmware/test/host/test_lift_detect.c`
- Modify: `firmware/main/sys/imu.c`, `imu.h` (no-motion, low-power mode, the INT ISR; Task 6 created it)
- Modify: `firmware/main/sys/power_rules.c`, `power_rules.h`, `firmware/test/host/test_power_rules.c` (stillness accounting, the false-wake cap, the confirm window)
- Modify: `firmware/main/settings/settings.h`, `settings.c` (`pw_still_hib_min`, `pw_imu_wake`; `CFG_VERSION 19`)
- Modify: `firmware/main/sys/power.c`, `power.h` (`still_ms` into `pr_in_t`, `power_anticipate`, the IMU-wake boot, the Deep-idle touch-off behind a flag), `firmware/main/sys/hibernate.c`, `hibernate.h` (IMU wake armed by the setting and the cap; the persistent backstop)
- Modify: `firmware/main/app_main.c` (`lift_init()` after `power_init()`)
- Modify: `firmware/main/ui/ui_settings.c` (Wake on lift, Wake on pick-up from Hibernate, Hibernate when still for), `firmware/main/sys/power_console.c`, `firmware/tools/ui_preview/preview.c`, `firmware/tools/ui_preview/CMakeLists.txt` (stubs for the new calls), `firmware/test/host/CMakeLists.txt` (add `lift_detect.c`, `test_lift_detect`)
- Create (only if Step 4 finds the driver's blob has no motion features): `firmware/main/sys/bmi270_cfg_bosch.h` (Bosch's standard blob, BSD-3-Clause header kept)

**Interfaces:**
- Produces, in `lift_detect.h` (unchanged from before; it now confirms a lift after an any-motion interrupt, and is the whole detector in the polling fallback):

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

- Produces, in `power_rules.h` (pure, host-tested):

```c
typedef struct { bool known, still; int64_t since_ms; } pr_still_t;
void pr_still_init(pr_still_t *s);   /* unknown */
void pr_still_event(pr_still_t *s, int64_t now_ms, bool imu_ok, bool any_motion, bool no_motion, int64_t no_motion_dur_ms);
/* !imu_ok → unknown. any_motion → known, moving (it wins when both are set).
   no_motion while not already still → still, since = now - no_motion_dur_ms (the sensor had
   already seen that long without motion). Otherwise unchanged. */
int64_t pr_still_ms(const pr_still_t *s, int64_t now_ms);   /* known && still ? now - since : 0 */

#define PR_IMU_CONFIRM_MS 20000       /* the confirm window after an IMU wake from Hibernate */
#define PR_IMU_MOTION_CONFIRM_S 5     /* 1 s buckets with motion that count as "continued motion"; start value */
#define PR_IMU_CAP 3
#define PR_IMU_CAP_WINDOW_S 3600
typedef struct { uint32_t t_s[PR_IMU_CAP]; uint8_t n; uint8_t disabled; } pr_imu_cap_t;   /* NVS ns "power" key "imucap" */
void pr_imu_cap_unconfirmed(pr_imu_cap_t *c, uint32_t now_s);
/* drops entries with now_s > 0 && t > 0 && now_s - t >= PR_IMU_CAP_WINDOW_S (an entry or a now of 0,
   a clock not yet valid, never ages out: the safe side), appends now_s (the oldest falls off when
   full); PR_IMU_CAP entries left → disabled = 1 */
void pr_imu_cap_rearm(pr_imu_cap_t *c);   /* a side-button wake from Hibernate or a key wake: n = 0, disabled = 0 */
bool pr_imu_wake_armed(const pr_imu_cap_t *c, bool setting_on);   /* setting_on && !disabled */
typedef enum { PR_CONF_WAIT, PR_CONF_INPUT, PR_CONF_MOTION, PR_CONF_FAIL } pr_confirm_t;
pr_confirm_t pr_imu_confirm(int64_t since_wake_ms, bool key_or_touch, int motion_s);
/* key_or_touch → INPUT; else motion_s >= PR_IMU_MOTION_CONFIRM_S → MOTION;
   else since_wake_ms >= PR_IMU_CONFIRM_MS → FAIL; else WAIT */
```

- Produces, in `imu.h` (added to Task 6's):

```c
esp_err_t imu_no_motion_arm(imu_pin_t p, uint16_t thr, uint16_t dur);
/* page 2, offset 0x00, the same word layout as any-motion; mapped by INTx_MAP_FEAT bit5 */
esp_err_t imu_low_power(bool on);   /* PWR_CONF 0x7C adv_power_save; ACC_CONF 0x17 (50 Hz, avg2, power-optimised); gyro off */
void imu_set_isr(void (*isr)(void *));
/* on imu_route()'s GPIO (any edge to active), or on the expander INT GPIO, where the task then
   reads the expander's interrupt status (0x13) to see that it was the IMU's pin; none when unrouted */
#define IMU_ANYMOT_AWAKE_DUR 5   /* 100 ms: while the P4 is awake, any-motion only starts a lift check */
#define IMU_NOMOT_THR 0x090      /* 70 mg */
#define IMU_NOMOT_DUR 250        /* 5 s; also the no_motion_dur_ms credited by pr_still_event */
```

  Only one any-motion config is live at a time: the awake one (`IMU_ANYMOT_AWAKE_DUR`, not latched) while the P4 runs, the Hibernate one (`IMU_ANYMOT_HIB_DUR`, latched) written by `hibernate_now`. These are start values; Task 10 tunes them and the notes say which values were chosen.

- Produces, in `lift.h`:
  - `void lift_init(void);` calls `imu_init()`, `imu_low_power(true)`, arms any-motion (awake) and no-motion on the routed pin, installs the ISR, and starts the `lift` task (stack 3072, priority 4).
  - `int64_t lift_still_ms(void);` is `pr_still_ms` under the module's lock.
  - `const char *lift_path(void);` gives `int(gpio<N>)`, `int(expander 0x4<x>)` or `poll`.
  - The task waits on its notification (the ISR's, or a 500 ms timeout in the polling fallback, where it reads INT_STATUS_0 instead of a pin). On each event it reads INT_STATUS_0 (which also clears it) and:
    1. feeds `pr_still_event` with the any-motion and no-motion bits (`imu_ok` false after 3 failed reads in a row);
    2. on any-motion while the state is Off or deeper, calls `power_anticipate()`;
    3. on any-motion while the state is Dim or deeper and `pw_wake_lift` is set, samples the accelerometer at 50 ms with `lift_feed` for up to 2 s, and calls `power_wake(PW_LIFT)` on WAKE. The baseline isn't reset for this: while no motion is reported the task feeds one sample every 10 s, so the burst compares against the resting pose.
  - It also reads INT_STATUS_0 once every 10 s when nothing has come, so a missed edge can't freeze the stillness.
  - The IMU runs in every state, so stillness is known in all of them. There is no `lift_arm` and no continuous polling while the INT is wired.
- Produces, in `power.h`:
  - `void power_anticipate(void);`. In Off or Deep idle it sets `cc_poll_set_floor(0)` so the next watch block comes within I = 3 s, and logs `anticipate`. If no wake follows within 30 s, it sets the floor back to 15. Task 8 extends it for a parked radio. It never lights the screen.
  - The power task fills `pr_in_t.still_ms = lift_still_ms()` and `still_hibernate_ms = pw_still_hib_min * 60000`.
- The IMU wake from Hibernate:
  - `hibernate_now` arms the IMU (any-motion latched, `IMU_ANYMOT_THR`, `IMU_ANYMOT_HIB_DUR`, ext1 on the LP GPIO) only when `hibernate_wake_sources() & HIB_SRC_IMU` and `pr_imu_wake_armed(&cap, pw_imu_wake)`. Otherwise it calls `imu_suspend()`.
  - If the mode is power-off and the boot can't tell an IMU wake from a button press, Hibernate uses deep sleep whenever it arms the IMU, so the guard always knows the source (my call, A17; record the draw difference).
  - On a boot where `hibernate_wake_src() == HIB_SRC_IMU`, the device shows the dim `Waking: reconnecting to Mac` view (brightness `pr_dim_brightness`, WAKING cards) and starts the radio at once, as every boot does. The power task runs the confirm window: it reads INT_STATUS_0 once a second and counts `motion_s`, the 1 s buckets with the any-motion bit set from 2 s after boot, and asks `pr_imu_confirm` every second.
    - INPUT (a key or a touch): a normal wake. A key also calls `pr_imu_cap_rearm`.
    - MOTION: a provisional wake. If the ladder reaches Off before any key or touch, it is counted with `pr_imu_cap_unconfirmed`, saved, and the device calls `hibernate_now(0, POWER_BOOT_HIB_WAKE)` at once, with no notice, instead of walking the ladder (my call, A16).
    - FAIL: `pr_imu_cap_unconfirmed`, save `imucap` in NVS, then `hibernate_now(0, POWER_BOOT_HIB_WAKE)` at once, with no notice.
  - A wake from Hibernate whose cause is neither the IMU nor the timer (the side button, reset or power-on), and any `PW_KEY` wake, call `pr_imu_cap_rearm` and save.
  - `now_s` is `time(NULL)` when the clock is valid (year ≥ 2026: deep sleep keeps the RTC-backed system time, and after power-off `sysinfo_time_from_rtc` runs first), else 0.
- Touch off in Deep idle (built now, switched on only after the lift wake is proven on the device):
  - `#define PW_DEEP_IDLE_TOUCH_OFF 0` in `power.c`. Task 10 sets it to 1 once a real pick-up has woken the device from Deep idle. `power set deep-touch-off 0|1` overrides it in RAM for testing.
  - When on, Deep idle entry stops the 10 Hz touch poll and cuts `TOUCH_EN` (`bsp_feature_enable(BSP_FEATURE_TOUCH, false)`). Keys and lift still wake it. Waking turns `TOUCH_EN` on, waits 50 ms, and re-creates the touch handle (`esp_lcd_touch_del`, `bsp_touch_new`, `lvgl_port_add_touch`), unless the Task 6 teardown re-creates it anyway.
  - If Task 6 Step 4c found that `TOUCH_EN` takes the ST7123 panel down too, this needs the Deep-idle teardown. Without the teardown it only stops the poll and leaves `TOUCH_EN` on. Record which.
  - Off keeps touch, because it's cheap relative to the P4 and a tap is the natural gesture in the first minutes.
- Produces, appended to `app_settings_t` (after `pw_hibernate_checkin_h`):

```c
    uint16_t pw_still_hib_min;   /* spec still_hibernate_min: 30; 0 = off; clamped to 0 or 10-240 */
    uint8_t  pw_imu_wake;        /* 1: wake from Hibernate on a pick-up (with the false-wake cap) */
```
  `CFG_VERSION` becomes 19. When migrating from 18 or older, `settings_init` sets both to their defaults (`if (old_version <= 18)`), as `plan2_defaults` does for v17: the old blob's tail padding may have landed on them. It clamps `pw_still_hib_min` with `vitals_clamp_or_zero(…, 10, 240)`.
- Settings → Power:
  - Wake on lift (`pw_wake_lift`);
  - Wake on pick-up from Hibernate (`pw_imu_wake`), shown only when `hibernate_wake_sources() & HIB_SRC_IMU`, with the muted note `Turns itself off after 3 false wakes in an hour` and, while the cap holds, `Off after false wakes. Press a key to turn it back on.`;
  - Hibernate when still for (min): Off, 10–240, shown when `hibernate_supported()`.
- Persistent backstop for multi-boot tests, in `hibernate.c`: `power hib backstop <s> <n>` arms an extra timer wake after s seconds on each of the next n hibernates of any kind. The counter lives in `RTC_NOINIT_ATTR` memory, so it survives deep sleep (not power-off: the tests below need deep sleep). `power hib test imu-drdy [n]` now takes a count: the next n hibernates map data-ready instead of any-motion, so each one wakes at once as an IMU wake (debug, RTC memory too).
- Console:
  - `power imu` prints `imu: path=<int(gpio N)|int(expander 0x4x)|poll> lowpower=<yes|no> acc_conf=0x<hex> anymot=<n> nomot=<n> still=<s|unknown> anticipate=<n> reads=<n> cap=<n>/3 <armed|disabled> last-wake=<none|imu input|imu motion|imu fail>` (`reads` counts the I²C reads of the sensor since boot);
  - `power imu cap [clear]` prints the cap's entries with their ages, or rearms it;
  - `power sim still <s>` sets the stillness to s seconds (RAM only), `power sim anymotion` feeds one any-motion event through the task, `power sim lift` calls `power_wake(PW_LIFT)`;
  - `power set still-hib <min>`, `power set imu-wake 0|1`, `power set deep-touch-off 0|1` (RAM only); `power settings` and `power set defaults` include `still-hib` and `imu-wake`;
  - `power lift` prints the last sample, the baseline and the path.

- [ ] **Step 1: Write the failing host tests** in `test_lift_detect.c` and `test_power_rules.c`
  - `test_still_noise_never_wakes`: 1000 samples of (0, 0, 1) ± 0.03 g pseudo-noise → never WAKE.
  - `test_tilt_held_wakes`: baseline (0, 0, 1), then (0.3, 0, 0.95) five times → FAST, FAST, FAST, FAST, WAKE.
  - `test_single_spike_no_wake`: one sample at (0.5, 0, 1), then back to rest → FAST, then SLOW.
  - `test_slow_drift_follows`: a 0.001 g/sample drift over 500 samples → no WAKE.
  - `test_still_accounting`: after init, `pr_still_ms` = 0 at any time; no_motion at t = 100 000 with dur 5 000 → 5 000 at t = 100 000 and 1 805 000 at t = 1 900 000; a repeated no_motion at t = 1 950 000 leaves it at 1 855 000; any_motion at t = 2 000 000 → 0; no_motion at 2 100 000 → 5 000 there; an event with `imu_ok` false → 0 (unknown) until the next no_motion; any_motion and no_motion in one event → 0.
  - `test_imu_cap`: unconfirmed at 1 000, 2 000 and 3 000 → disabled, `pr_imu_wake_armed(c, true)` false; rearm → armed and n 0. Unconfirmed at 1 000 and 2 000, then at 4 600 → the 1 000 entry aged out (4 600 − 1 000 = 3 600), n 2, not disabled. Setting off → not armed with n 0. A clock of 0: unconfirmed at 0, 0, then 5 000 → disabled (the 0 entries never age out).
  - `test_imu_confirm`: (5 000, true, 0) → INPUT; (25 000, true, 0) → INPUT; (5 000, false, 5) → MOTION; (5 000, false, 4) → WAIT; (19 999, false, 0) → WAIT; (20 000, false, 4) → FAIL.

- [ ] **Step 2: Run them to make sure they fail**

Run the host test command.
Expected: FAIL

- [ ] **Step 3: Implement `lift_detect.c` and the `power_rules.c` additions** and run the tests until they pass

- [ ] **Step 4: Check the feature blob and the licences**
  - Take Task 6 Step 4a's finding on which blob the driver uploads. If it is Bosch's standard blob, use it as it is: it comes inside the espressif/bmi270 component (Apache-2.0), and Bosch publishes the blob under BSD-3-Clause.
  - If it has no any-motion and no-motion, `imu_init` uploads Bosch's standard `bmi270_config_file` from BMI270_SensorAPI itself, after `bmi270_create`. Keep it in `firmware/main/sys/bmi270_cfg_bosch.h` with Bosch's BSD-3-Clause header kept whole, and add a line for it beside the existing licence notices.
  - Check whether the blob has Bosch's wrist-wake gesture (it is in the wearable `bmi270_wh` config, not the standard one, so expect no). Use any-motion unless it's there.
  - The register offsets and bit positions written in `imu.c` are datasheet facts, not copied code. If any function is copied from the SensorAPI, it keeps Bosch's header.
  - Record the result as a ruling.

- [ ] **Step 5: Implement `imu.c` additions, `lift.c`, the power and hibernate hooks, the settings and the rows**, add the preview stubs, build the preview, then build and flash

Expected: the boot log shows `imu: bmi270 0x24 at 0x68, blob <standard|bosch upload>, path <int(gpio N)|int(expander 0x4x)|poll>`, and `power settings` shows `still-hib=30 imu-wake=1` (the CFG 19 migration ran).

- [ ] **Step 6: Check on hardware without moving the device**

Run: `python3 $T5 "power imu"`.
Expected: the path Task 6 found, `lowpower=yes`, `acc_conf=0x17`.

Leave the device on the desk 60 s, then run `python3 $T5 "power imu"`.
Expected: `nomot` at least 1 and `still` at least 50 s, with no I²C reads in between except the 10 s status read (`power imu` counts them as `reads=`). On the polling fallback, `reads` climbs at 2 a second instead: record it.

Run: `python3 $T5 "power set dim 10" "power set off 20"`. Wait for `state=off`, then run `python3 $T5 "power sim anymotion" "power imu" "companion watch status"`.
Expected: `anticipate=1`, the watch at `I=3s` within a few seconds, and `state=off` still (the screen stays dark). After 30 s, `I=15s` again.

Run: `python3 $T5 "power sim lift" "power wakes"`.
Expected: `lift swallowed=0`, `state=active`.

Leave it in Off for 3 minutes on the desk.
Expected: no `lift` wake in `power wakes`, and `still` keeps growing. Lifting it for real is Task 10 Step 4.

- [ ] **Step 7: Check the stillness trigger and the false-wake guard on USB**

Stillness: run `python3 $T5 "power hib test-wake 40" "power sim battery 50" "power sim still 1800" "power sim idle 1800" "power"`.
Expected: the notice for 3 s, then hibernate, then back within about 40 s with `power boot` showing `kind=hib-wake cause=timer`. Run `power set defaults`.

The guard (only if Task 6 proved `imu-drdy` from an LP GPIO; otherwise record "IMU wake from Hibernate: not wired, guard not exercised"). Run: `python3 $T5 "power imu cap clear" "power hib backstop 60 6" "power hib test imu-drdy 4"`. Don't send any key: the console reconnecting after each boot is not input.
Expected, over about 3 minutes:
- three IMU wakes in a row, each powering down again about 20 s after boot (`imu fail` in the log, which `power log dump` or the next boot's `power imu` shows);
- after the third, `cap=3/3 disabled`, and the next hibernate arms no IMU source, so the 60 s backstop wakes it (`cause=timer`, which neither counts nor rearms);
- `power imu` then shows `cap=3/3 disabled` and Settings → Power shows the "Off after false wakes" note.

Run: `python3 $T5 "power go off"`, then `python3 $T5 "key esc" "power imu"`.
Expected: `cap=0/3 armed` (a key wake rearms it).

Confirm by input: run `python3 $T5 "power hib backstop 60 2" "power hib test imu-drdy 1"`, and as soon as the console is back (within the 20 s), run `python3 $T5 "key esc" "power imu" "power"`.
Expected: `last-wake=imu input`, `cap=0/3 armed`, and the device stays up and follows the normal ladder.

Run `python3 $T5 "power hib backstop 0 0" "power set defaults"`.

- [ ] **Step 8: Check the Deep-idle touch-off on USB** (the code only; it stays off until Task 10)

Run: `python3 $T5 "power set deep-touch-off 1" "power go deep-idle" "power touch" "power rails"`.
Expected: `touch: poll off`, and `TOUCH_EN` at 0 (or still 1 with `touch-off: poll only` in the log when Task 6 found that `TOUCH_EN` also takes the panel and the teardown wasn't kept).

Run: `python3 $T5 "key esc"`, take a screenshot, then `python3 $T5 "power go off"`, wait 5 s, and run `python3 $T5 "power touch"`.
Expected: the dashboard is back, and in Off the touch poll reads again with `errors=0` (the re-created handle works). Run `power set deep-touch-off 0`.

- [ ] **Step 9: Commit**

```bash
git add firmware/main firmware/test/host firmware/tools/ui_preview docs/superpowers/notes/2026-10-23-plan2-rulings.md
git commit -m "feat(power): BMI270 any/no-motion interrupts for lift wake, stillness, anticipatory link and a guarded IMU wake from Hibernate"
```

### Task 8: wifi_mgr reconnect, parked state and the Radio-off tier

**Files:**
- Modify: `firmware/main/net/wifi_mgr.c`, `wifi_mgr.h`
- Modify: `firmware/main/net/tailscale.c`, `tailscale.h`
- Modify: `firmware/main/companion/cc_link.c`, `cc_link.h`
- Modify: `firmware/main/sys/power.c`, `power_rules.c`, `power_rules.h`, `firmware/test/host/test_power_rules.c`
- Modify: `firmware/tools/ui_preview/preview.c`, `firmware/tools/ui_preview/CMakeLists.txt` (stubs for the new calls)
- Modify: `firmware/main/app_main.c` (`wifi_global_cb`; the Hibernate check-in boot), `firmware/main/ui/ui_settings.c` (Radio-off rows, Hibernate check-in row), `firmware/main/ui/ui_dashboard.c` (`Reconnecting…`), `firmware/main/sys/power_bench.c` (steps `radio-off` 180 s, `checkin-handshake` and `checkin-rebind`, each up to 120 s, and `deep-idle` 180 s, all inserted before Task 6's `floor` and `hibernate`, which stay the last two; that makes 15 steps, within `BENCH_MAX` 16), `firmware/main/sys/power_console.c`
- Modify: `firmware/main/sys/hibernate.c`, `hibernate.h` (the check-in wake and boot)

**Interfaces:**
- Produces, in `power_rules.h`:
  - `int pr_backoff_s(int attempt);` gives 1, 2, 4, 8, 16, then 30 for every later attempt, with no cap on attempts.
  - `bool pr_radio_off_worth_it(float a_mw, float b_mw, float c_mwh, int checkin_min, int checkin_s);`. It is the spec's rule: `(a − b) × period > c`, the average draw drops at least 20%, and `checkin_s < 30`. The average with toggling is `b + c / period`.
  - `int pr_checkin_period_min(power_state_t st, int checkin_min, int deep_checkins_done);`. Radio off: `checkin_min`. Deep idle backs off 5 → 15 → 30 min: `max(checkin_min, 15)` for the first check-in after entering Deep idle (`deep_checkins_done == 0`), then `max(checkin_min, 30)` for every later one. Any other state: 0 (no check-ins).
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
  - `void cc_link_park(bool parked);`. Parked closes the session and doesn't reconnect. This close is the Tab5's own, so it doesn't count toward the watch fallback.
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
    6. a new needs-you does a needs-you wake (Dim) and stays up; otherwise run `cc_link_park(true)` + `wifi_mgr_park()` again, then `tailscale_set_fast_resume(false)`.
  - Each check-in's path, total ms and mWh go into the power log and `power checkins`. The path `handshake` is recorded only after step 4's SSH connect succeeds, because a stale keypair can report the peer as up.
  - A key, touch or lift during Radio off wakes to Active: unpark at once, then reconnect, while the dashboard shows the muted `Reconnecting to <host>…` with stale cards until the first fresh result.
  - Input wake from Radio off uses the same steps 1–5 (handshake first, so the target is about 10 s), then stays up. When that reconnect finishes it calls `tailscale_set_fast_resume(false)`, so a later real Wi-Fi drop still rebinds.
  - The keyboard light uses `pr_light_max_age_ms(true, period)` in Radio off and Deep idle, where `period` is the one `pr_checkin_period_min` scheduled for the next check-in (Review Focus 4).
  - Off entry also calls `wifi_mgr_set_ps_max(true)`, and every wake calls `wifi_mgr_set_ps_max(false)`.
  - `power_anticipate()` (Task 7) with the radio parked, in Radio off or Deep idle, runs check-in steps 1–5 without lighting the screen and then stays unparked for 60 s instead of step 6, so a lift or key that follows finds the link up. If nothing wakes the device in those 60 s, it parks again as after a check-in. It logs as a check-in with path `anticipate`.
- Power, Deep idle:
  - With `pw_radio_off` on, Deep idle keeps the radio parked (it parks it first if Radio off hadn't yet) and runs the same check-in steps 1–6, at `pr_checkin_period_min(PS_DEEP_IDLE, pw_checkin_min, k)`, logging tag `0x84`. Leaving Deep idle resets k.
  - With `pw_radio_off` off (the default until Task 10), Deep idle leaves the link on the long poll at I = 15 s, as in Off. There are no check-ins, so nothing backs off.
  - A check-in that finds a new needs-you does a needs-you wake, as in Radio off. The light rule is the Deep-idle one (amber at 30%) only until that wake lands in Dim.
  - Bench step `deep-idle` (180 s): `power_go(PS_DEEP_IDLE)` with the radio parked and no check-in due inside the step, so it measures the floor (with the Task 6 teardown and 40 MHz clock if they were kept).
- Hibernate check-in (`pw_hibernate_checkin_h`, default 0 = off):
  - When it is set, `hibernate_now` arms a wake after `pw_hibernate_checkin_h` hours with `next = POWER_BOOT_HIB_CHECKIN`. The wake comes from the timer, or the RX8130 alarm when Task 6 chose power-off with the RTC as a source.
  - On a check-in boot, `app_main` keeps the screen dark (brightness 0, panel off, power state Off from the start) and runs:
    1. Wi-Fi up;
    2. `tailscale_checkin(mac_ip, false, &ms)`;
    3. `cc_link_park(false)` and wait for connected;
    4. `cc_link_poll_now(15000)`;
    5. any background needs-you → keyboard light amber at 30% (the Deep-idle rule), else `PR_LED_OFF`;
    6. `hibernate_now(pw_hibernate_checkin_h * 3600, POWER_BOOT_HIB_CHECKIN)` again.
  - The whole check-in has 180 s. When that runs out it hibernates anyway and records `failed`.
  - A key, touch or lift during a check-in boot is an ordinary wake to Active, and the check-in is abandoned.
  - If Task 6 found that the keyboard rail drops in deep sleep or power-off, or Task 6 Step 4c put a keyboard rail among the ones Hibernate cuts, the amber light can't outlast the check-in. Then a needs-you check-in stays up in Deep idle with the light amber, instead of hibernating, until input or the next `pw_hibernate_h` (my call, Amendment A9).
  - Each check-in goes into `power checkins` with path `hib-handshake`, `hib-rebind` or `hib-failed`, its ms and its mWh (summed from 1 Hz samples during the boot, tag `0x85`). `power checkins` keeps its last 8 entries in NVS (ns `power`, key `chk`) so they survive the hibernate.
- Settings → Power: Radio off (switch, default off), Radio off after (min) 5–240, Check in every (min) 1–60, Hibernate check-in every (h): Off, 1–24.
- Console:
  - `power go radio-off`, `power checkin [rebind]` (one check-in now), `power checkins` (last 8: path, ms, mWh);
  - `power set radio-off 0|1`, `power set radio-after <min>`, `power set checkin <min>`, `power set hib-checkin <h>`;
  - `power checkin hibernate` runs one Hibernate check-in cycle with a 60 s wake. After that one check-in it stays awake instead of hibernating again (debug), so the console comes back;
  - `power` prints `next-checkin=<s>` and `period=<min>` in Radio off and Deep idle;
  - `companion reconnect-log` prints `cc_link_reconnect_log`;
  - the debug-only `wifi test drop` calls `esp_wifi_disconnect()` without setting the user flag, like a real drop.

- [ ] **Step 1: Write the failing host tests** (in `test_power_rules.c`)
  - `test_backoff`: attempts 0..7 → 1, 2, 4, 8, 16, 30, 30, 30; attempt 1000 → 30.
  - `test_radio_off_rule`: a = 300, b = 60, c = 2.0 mWh, 5 min, 12 s → true (the saving is 20 mWh per period; the average falls from 300 to 84 mW); c = 25 mWh → false; check-in 31 s → false; a = 100, b = 90, c = 0.1 mWh → false (the saving, 0.83 mWh, passes the first condition; the average falls only 8.8%, under 20%).
  - `test_deep_idle_backoff` (Review Focus 4): `pr_checkin_period_min(PS_RADIO_OFF, 5, 0)` = 5; `(PS_DEEP_IDLE, 5, 0)` = 15; `(PS_DEEP_IDLE, 5, 1)` = 30; `(PS_DEEP_IDLE, 5, 7)` = 30; `(PS_DEEP_IDLE, 20, 0)` = 20; `(PS_DEEP_IDLE, 20, 1)` = 30; `(PS_DEEP_IDLE, 45, 3)` = 45 (never shorter than the user's); `(PS_OFF, 5, 0)` = 0. And `pr_light_max_age_ms(true, 30)` = 1 860 000, so a 30-minute Deep-idle period keeps the amber.

- [ ] **Step 2: Run them to make sure they fail**

Run the host test command.
Expected: FAIL

- [ ] **Step 3: Implement the rules** and run the tests until they pass

- [ ] **Step 4: Implement wifi_mgr, Tailscale, cc_link and power (Radio off, the Deep-idle check-ins and the Hibernate check-in)**, add the preview stubs, build the preview, then build and flash

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

- [ ] **Step 8: Check the Deep-idle backoff and the Hibernate check-in**

Run: `python3 $T5 "power set radio-off 1" "power go deep-idle" "power"`.
Expected: `state=deep-idle`, the radio parked, `period=15`.

Run: `python3 $T5 "power checkin"`, wait 60 s, then `python3 $T5 "power"` (a forced check-in counts as the first one).
Expected: `period=30`, and `power light` shows `light: 1 0 0 0 0 (nobody needs you)`.

Start `companion-test-hib` on the Mac (Task 3 prompt) and wait for it to block. Run: `python3 $T5 "power set hib-checkin 1" "power checkin hibernate"`. The console drops for about 60 s plus the cold connect. When it is back, run `python3 $T5 "power checkins" "power light" "power boot" "power"`.
Expected:
- a `hib-handshake` or `hib-rebind` entry with its ms and mWh;
- `light: 1 30 f4 b5 45 (needs you: companion-test-hib)`;
- `kind=hib-checkin`;
- `state=off` with brightness 0: the screen stayed dark (an LVGL screenshot still renders, so it can't show this).

Record the check-in's total time. Stop `companion-test-hib`, then run `power set defaults`.

- [ ] **Step 9: Commit**

```bash
git add firmware/main firmware/test/host firmware/tools/ui_preview docs/superpowers/notes/2026-10-23-plan2-rulings.md
git commit -m "feat(net): Wi-Fi reconnect task with backoff, parked state, Radio-off tier with handshake-first check-ins, Deep-idle backoff and the Hibernate check-in"
```

### Task 9: Plan 1 follow-ups

**Files:**
- Modify: `firmware/main/ssh/ssh_client.c`, `ssh_client.h` (forget one pin)
- Modify: `firmware/main/companion/cc_link.c`, `cc_link.h`, `cc_console.c`
- Modify: `firmware/main/ui/ui_settings.c` (Companion → Forget Mac host key)
- Modify: `firmware/tools/ui_preview/preview.c`, `firmware/tools/ui_preview/CMakeLists.txt` (stubs for the new calls)
- Create: `firmware/tools/capture_attach_screen.py`
- Create: `firmware/test/host/fixtures/screen_permission_dialog.bin`
- Modify: `firmware/test/host/test_cc_cmd.c`
- Modify: `docs/superpowers/specs/2026-10-22-claude-tab5-design.md` (D5 and D7 wording), `docs/superpowers/notes/2026-10-22-plan1-rulings.md` (mark the follow-ups done)

**Interfaces:**
- Produces, in `ssh_client.h`: `bool ssh_known_host_forget(const char *host, int port);` erases the NVS `kh` entry for host:port and returns true if there was one.
- Produces, in `cc_link.h`: `esp_err_t cc_link_forget_host_key(char *fp_out, size_t fp_cap, int timeout_ms);`.
  - It is a queued request. It forgets the link's pin (the resolved address it connects to) and the terminal's pin (the server entry's host string), tears the session down and reconnects, so the new key is pinned as `KH_UNKNOWN`. The teardown is the Tab5's own close, so it doesn't count toward the watch fallback.
  - It returns `ESP_OK` with the new fingerprint, or `ESP_ERR_TIMEOUT` if no connect happened within `timeout_ms` (the pins are cleared either way).
- Settings → Companion → "Forget Mac host key" asks with `ui_confirm("Forget Mac host key?", "Only do this if your Mac's SSH host key changed. The Tab5 will trust the next key it sees.")`. After Yes, it calls the request on the dashboard worker and shows the toast `New Mac key pinned: SHA256:…` or `Old key forgotten; reconnecting…`.
- cc_link minors:
  - `cc_link_failure()` reads `s_state` and `s_fail` under `lock()`;
  - when `ssh_new()` fails, `connect_once` calls `set_fail(CC_LINK_FAIL_NONE, NULL)` before returning, so an old reason doesn't stick.
- Console `companion hostkey forget` runs the same request. The debug-only `companion hostkey corrupt` overwrites the link's own pin with a wrong value (through a debug helper such as `ssh_known_host_set_debug` in `ssh_client.c`), so the changed-key path can be shown without touching the Mac.
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

- [ ] **Step 4: Implement the forget action, the console and the minors**, add the preview stubs, build the preview, then build and flash

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

### Task 9b: Hotkeys for two thumbs

It is numbered 9b, not renumbered, so that the many "Task 10" and "Task 10 Step N" references in this plan, and the visit's name in the notes, stay right. It implements the spec's table under "Hotkeys for two thumbs" exactly. The real-press checks are in Task 10.

**Files:**
- Create: `firmware/main/ui/hotkeys.c`, `firmware/main/ui/hotkeys.h` (pure: it includes only `input/keyboard.h`, which has no ESP-IDF or LVGL headers)
- Create: `firmware/test/host/test_hotkeys.c`
- Modify: `firmware/test/host/CMakeLists.txt` (add `../../main/ui/hotkeys.c` to the `pure` library; add `test_hotkeys`)
- Modify: `firmware/main/ui/ui_terminal.c` (`term_key` asks `hk_takeover`; the 3 s open toast)
- Modify: `firmware/main/ui/ui_dashboard.c` (`dash_key` asks `hk_dashboard`; the footer hint; the Ctrl+K overlay)
- Modify: `firmware/main/ui/ui_core.c`, `firmware/main/ui/ui.h` (`ui_toast_ms`; `ui_dashboard_keys_overlay` for the preview)
- Modify: `firmware/main/input/key_console.c` (`key ctrl <char>` for any printable ASCII character, `key shift <key>`, `key log on|off`)
- Modify: `firmware/main/input/keyboard.c`, `keyboard.h` (only the `key log` hook: the drain prints each decoded event while it is on). The decode itself needs no change, as found from reading it:
  - In HID mode (stock layout), `hid_to_event` maps HID 0x30 to `cp=']'` and 0x10 to `cp='m'`, with `MOD_CTRL` from the modifier byte.
  - In matrix mode (JIS), `]` is row 2, column 12, and `m` is row 3. `matrix_event` reports both as `KEY_EV_CHAR` with the held `MOD_CTRL`.
  - USB keyboards go through the same `hid_to_event`.
  - So **Ctrl+M arrives as `cp='m'` with `MOD_CTRL`, never as `SK_ENTER`**, in every path. Only the terminal's `term_keys_encode` turns it into 0x0d, and only inside a terminal. Task 10 confirms with real presses what the A164's own firmware sends in HID mode.
- Modify: `firmware/tools/ui_preview/preview.c`, `firmware/tools/ui_preview/CMakeLists.txt` (add `hotkeys.c`; stubs for the new calls; a `dashboard-keys` case)

**Interfaces:**
- Produces, in `hotkeys.h`:

```c
typedef enum { HK_T_NONE, HK_T_LEAVE, HK_T_SCROLL_UP, HK_T_SCROLL_DOWN } hk_term_t;
hk_term_t hk_takeover(const key_event_t *ev, bool takeover);
/* takeover only: Ctrl+] (KEY_EV_CHAR ']' with MOD_CTRL) and Ctrl+Q / Ctrl+q (the alias) → LEAVE;
   Shift+Up → SCROLL_UP and Shift+Down → SCROLL_DOWN (takeover only: a plain SSH terminal keeps
   Shift+arrows for the remote side); Shift+PgUp / Shift+PgDn → SCROLL_UP / SCROLL_DOWN in any
   terminal, as today; everything else → NONE (sent to the Mac) */
typedef enum { HK_D_NONE, HK_D_OPEN, HK_D_SETTINGS, HK_D_MORE, HK_D_KEYS, HK_D_LOCAL } hk_dash_t;
hk_dash_t hk_dashboard(const key_event_t *ev);
/* KEY_EV_CHAR with MOD_CTRL or MOD_META: o/O → OPEN, p/P → SETTINGS, m/M → MORE, k/K → KEYS,
   any other character → LOCAL (eaten: never typed into an answer field, never sent);
   anything else → NONE (dash_key's existing handling) */
#define HK_DASH_HINT "Ctrl+O open \xc2\xb7 Ctrl+P settings \xc2\xb7 Ctrl+K keys"
#define HK_TAKEOVER_TOAST "Ctrl+] to leave"
#define HK_TAKEOVER_TOAST_MS 3000
extern const char *const hk_overlay_lines[];   /* NULL-terminated; the Ctrl+K overlay, one shortcut per line */
```

  The overlay lines, in this order: `Up / Down  move between cards`, `Enter  send the answer`, `Ctrl+O  open the session`, `Ctrl+P  settings`, `Ctrl+M  more`, `Ctrl+K  this list`, `Esc  back (Settings, More)`, `In a session: Ctrl+]  leave (it keeps running)`, `In a session: Shift+Up / Down  scroll back`, `Ctrl+Tab  buttons mode`.
- Produces, in `ui.h`:
  - `void ui_toast_ms(const char *text, uint32_t ms);`. `ui_toast` becomes `ui_toast_ms(text, 2200)`.
  - `void ui_dashboard_keys_overlay(bool show);`.
- Terminal: `term_key` calls `hk_takeover(&e, c->cmd_mode)` first, on the event with the on-screen CTRL latch applied, as the Ctrl+Q check does today:
  - LEAVE runs today's Ctrl+Q branch: `cmd_close(c, "Session still running on your Mac")`, never sent.
  - SCROLL_UP and SCROLL_DOWN run today's Shift+PgUp/PgDn branch (half a screen).
  - When a take-over opens (`cmd_mode` set and the channel up), it shows `ui_toast_ms(HK_TAKEOVER_TOAST, HK_TAKEOVER_TOAST_MS)`.
- Dashboard:
  - `dash_key` first checks the overlay: while it is shown, **any** key, special or not, closes it and is eaten. A touch on it closes it too.
  - Then `hk_dashboard`: OPEN → `open_selected()` (unchanged); SETTINGS → `ui_nav_push(ui_settings_create, NULL)`; MORE → `ui_nav_push(ui_home_create, NULL)` (the same calls as `settings_cb` and `more_cb`); KEYS → show the overlay; LOCAL → eaten.
  - These checks come before today's `if (!s_shown_live) return true;`, so Settings, More and the overlay work with no cards on screen; OPEN still needs a selected card.
  - All of them return before the `field_visible` branch, so none of them can reach `s_draft`.
  - The draft is left as it was, so a half-typed answer is still there after Esc comes back from Settings or More.
  - The overlay is a full-screen panel (`#161b22` on a `LV_OPA_80` `#0e1116` backdrop) on the dashboard screen, with the lines in fg and the keys in muted. No amber.
  - The footer text becomes `Up/Down select · Enter send / open · ` + `HK_DASH_HINT`. The Settings and More buttons and Ctrl+Tab stay.
  - The card hint `Enter sends · Ctrl+O opens the session` stays.
- Console (`key_console.c`):
  - `key ctrl <c>` accepts any single printable ASCII character (`key ctrl ]`, `key ctrl p`); letters stay lower case, as today.
  - `key shift <name>` posts a named special key with `MOD_SHIFT` (`key shift up`).
  - `key log on|off` prints `key: type=<char|special> cp=<'c' or hex> key=<name> mods=<ctrl,shift,…>` for every event the drain decodes, real presses included. It is for Task 10.

- [ ] **Step 1: Write the failing host tests** in `test_hotkeys.c`
  - `test_takeover_leave`: Ctrl+`]` in a take-over → LEAVE; Ctrl+`q` → LEAVE; Ctrl+`Q` → LEAVE; `]` without mods → NONE; Ctrl+`]` with takeover false → NONE; Ctrl+`c` → NONE (Claude Code gets it).
  - `test_takeover_scroll`: Shift+Up → SCROLL_UP; Shift+Down → SCROLL_DOWN; plain Up → NONE; Shift+Up with takeover false → NONE; Shift+PgUp with takeover false → SCROLL_UP; Shift+PgDn in a take-over → SCROLL_DOWN.
  - `test_dashboard_keys`: Ctrl+`o` → OPEN; Ctrl+`P` → SETTINGS; Ctrl+`m` → MORE; Ctrl+`k` → KEYS; Meta+`p` → SETTINGS; Ctrl+`x` → LOCAL; plain `p` → NONE; plain Enter → NONE.
  - `test_dashboard_ctrl_never_typed`: for every cp from 0x20 to 0x7e with `MOD_CTRL`, and again with `MOD_CTRL | MOD_SHIFT`, `hk_dashboard` is not NONE.
  - `test_ctrl_m_is_not_enter`: `{KEY_EV_CHAR, 'm', MOD_CTRL}` → MORE; `{KEY_EV_SPECIAL, SK_ENTER, MOD_CTRL}` → NONE (Ctrl+Enter keeps today's meaning).
  - `test_overlay_lists_every_shortcut`: `hk_overlay_lines` has 10 entries, and they contain `Ctrl+]`, `Ctrl+P`, `Ctrl+M`, `Ctrl+K`, `Ctrl+O` and `Shift+Up`.

- [ ] **Step 2: Run them to make sure they fail**

Run the host test command.
Expected: FAIL (undefined `hk_*`)

- [ ] **Step 3: Implement `hotkeys.c`** and run the host tests until they pass

- [ ] **Step 4: Wire the terminal, the dashboard, `ui_toast_ms`, the overlay, the console and the key log**, add the preview stubs, then build the preview and render `./preview dashboard` and `./preview dashboard-keys`

Convert both with `sips` and look at them.
Expected: the footer reads `… Ctrl+O open · Ctrl+P settings · Ctrl+K keys`, and the overlay lists all ten lines, readable, in D9 colours with no amber.

Build and flash.

- [ ] **Step 5: Check on hardware with the console**

On the Mac, start `companion-test-keys` (Task 3 prompt) and wait for it to block. On the dashboard, select its card (the answer field is visible).

Run: `python3 $T5 "key text ab" "key ctrl p"`, then take a screenshot.
Expected: Settings is open. Run `key esc`. Expected: the dashboard, with `ab` still in the field and nothing else in it.

Run: `python3 $T5 "key ctrl m"`, then take a screenshot.
Expected: More (the fork's home grid). Run `key esc`.

Run: `python3 $T5 "key ctrl k"`, then take a screenshot; then `python3 $T5 "key text z"` and take another.
Expected: the first shows the overlay; the second shows it closed, with the field still `ab` (no `z`). Run `key esc` to clear the field.

Run: `python3 $T5 "key ctrl x" "key ctrl ]"`.
Expected: nothing typed, nothing opened.

Run: `python3 $T5 "key ctrl o"` and take a screenshot within 3 s.
Expected: the take-over is open, with the toast `Ctrl+] to leave`. A second screenshot 4 s later shows no toast.

Run: `python3 $T5 "key shift up"` and take a screenshot.
Expected: the view scrolled back half a screen. The session on the Mac got no keystroke: its state and screen are unchanged, which `sessions` and the next attach show. Run `key shift down`.

Run: `python3 $T5 "key ctrl ]"`.
Expected: back on the dashboard, `sessions` still lists `companion-test-keys` as running.

Open it again with `key ctrl o`, then run `python3 $T5 "key ctrl q"`.
Expected: back on the dashboard (the alias works).

Stop `companion-test-keys`.

- [ ] **Step 6: Commit**

```bash
git add firmware/main/ui firmware/main/input firmware/test/host firmware/tools/ui_preview docs/superpowers/notes/2026-10-23-plan2-rulings.md
git commit -m "feat(ui): two-thumb hotkeys: Ctrl+] leaves a take-over, Shift+arrows scroll, Ctrl+P/M/K on the dashboard"
```

### Task 10: Measurement visit with the user

This is the only task that needs the user. Tasks 1–9b are done and flashed, and every gated setting is still at its default.

**Files:**
- Create: `docs/superpowers/notes/2026-10-23-power-measurements.md`
- Modify: `firmware/tools/power_report.py`, `firmware/tools/test_power_report.py` (`ladder_mwh` and `--ladder`)
- Modify: `firmware/main/sys/sysinfo.c` (only if Step 4 shows the charge pin doesn't track)
- Modify: `firmware/main/settings/settings.c`, `settings.h` (gated and long-idle defaults from the data, `pw_still_hib_min` among them; `CFG_VERSION 20`, since Task 7 took 19, with a migration that sets each changed default once)
- Modify: `firmware/main/sys/imu.h` (the any-motion and no-motion values chosen from Step 6b), `firmware/main/sys/power.c` (`PW_DEEP_IDLE_TOUCH_OFF` 1 if Step 4 proves the lift wake from Deep idle)
- Modify: `firmware/main/ui/hotkeys.c`, `hotkeys.h`, `firmware/test/host/test_hotkeys.c` (only if Step 3 shows Ctrl+] isn't reported)
- Modify: `docs/superpowers/specs/2026-10-22-claude-tab5-design.md` (Known issues: Mac sleep/wake reconnect, brownout counts; Roadmap row 2 done)

**Interfaces:**
- Consumes: `power bench`, `power_report.py`, `power wakes`, `power checkins`, `power boot`, `power light test bind`, `key log`, `cc_link_reconnect_log` and `companion kick` from the earlier tasks, plus `pr_radio_off_worth_it` (Task 8), and `power imu`, `power imu cap`, `power rails` and `power hib backstop` (Tasks 6, 7).
- Produces, in `power_report.py`: `ladder_mwh(mw: dict[str, float], hours: float, *, dim_s=30, off_s=120, radio_off_min: int | None = 20, checkin_min=5, deep_idle_min: int | None = 60, hibernate_h: int | None = 2, checkin_mwh=0.0, hib_checkin_h=0, hib_checkin_mwh=0.0) -> dict`. It returns `{"mwh", "by_state", "checkins"}` for `hours` of idle after the last input.
  - `mw` has the keys `active`, `dim`, `off`, `radio_off`, `deep_idle` and `hibernate`. `None` turns a tier off.
  - The timeline follows `pr_next_state`:
    - Active until `dim_s`;
    - Dim until `off_s`;
    - Off until `off_s + radio_off_min` min;
    - Radio off until `deep_idle_min` (Deep idle wins if it comes first);
    - Deep idle until `hibernate_h`;
    - Hibernate to the end.
  - Check-ins:
    - one every `checkin_min` in Radio off;
    - in Deep idle, only when Radio off is on, at `max(checkin_min, 15)` and then every `max(checkin_min, 30)` min (`pr_checkin_period_min`);
    - every `hib_checkin_h` in Hibernate.
  - The CLI's `--ladder 8,10` prints, for each number of hours, the mWh and the share of `--capacity-mwh` it uses, each with `(estimate)`.

- [ ] **Step 1: Write the failing `ladder_mwh` tests** in `test_power_report.py`, then implement it until they pass
  - `test_ladder_default_8h`: mw {active 3000, dim 2000, off 1000, radio_off 300, deep_idle 200, hibernate 5}, 8 h, defaults → 828.33 mWh (±0.1): 25 + 50 + 333.33 + 190 + 200 + 30.
  - `test_ladder_checkins`: the same with `checkin_mwh=2` → 9 check-ins (7 in Radio off, 2 in Deep idle at +15 and +45 min), 846.33 mWh.
  - `test_ladder_no_radio_off`: `radio_off_min=None`, `checkin_mwh=2` → 0 check-ins, 1 271.67 mWh (Off runs to 60 min).
  - `test_ladder_no_hibernate_10h`: `hibernate_h=None`, 10 h → 2 398.33 mWh (Deep idle to the end).

Run the Python tests.
Expected: they FAIL first (no `ladder_mwh`), then pass.

- [ ] **Step 2: Prepare without the user**

Flash the final build, then run `python3 $T5 "power set defaults" "power log clear" "power log on" "power resets" "power hib"` and record the reset counts and the hibernate mode. On the Mac, start `companion-test-bench` (Task 3 prompt) so needs-you is live for the light steps.

- [ ] **Step 3: Ask the user once, in one message, for everything**
  1. Confirm a battery is fitted, and read its capacity from the label if you can.
  2. While it's still plugged in, before the bench:
     - tap the dark screen once, press one key, and pick the Tab5 up off the table; I'll check that each one only wakes it;
     - pick it up twice more when I ask: once while it's in its deepest idle with the screen dark, and once more just to check the motion sensor's wiring (only if I couldn't prove it without you);
     - look at the keyboard light for 5 s while I switch it to the keyboard's own mode, and tell me what it shows;
     - press Ctrl+], Ctrl+P, Ctrl+M and Ctrl+K when I ask, in both keyboard layouts (I switch the layout between rounds).
  3. When I say so, unplug USB for about 30 minutes while `power bench` runs. Near the end the Tab5 goes dark and restarts by itself three times; that is the bench measuring Hibernate. It shows `Bench done. Plug USB back in.` when it's finished; then plug it back in.
  4. After the bench: the Tab5 will go to sleep with the same message. Press the side button once to wake it, and tell me when you pressed. If nothing happens within 10 s, press the reset button and tell me that instead.
  5. Then it goes to sleep again. Leave it on the table for a minute, then pick it up as you normally would, and tell me when. Then I'll put it to sleep a last time: put it in a bag, unplugged, and carry it on a short walk of about 10 minutes (stairs or a car are fine), then take it out, plug it back in and tell me. I'm counting how often the motion wakes it by mistake.
  6. Then put the Mac to sleep (Apple menu → Sleep) for about 3 minutes, and wake it.

- [ ] **Step 4: The wake and hotkey checks with the user** (on USB, before the bench)
  - Run `python3 $T5 "power go off"`, then ask for the tap. Then `power wakes`.
    Expected: `touch swallowed≥1`, no card action, and the screen lit. This is the first real-finger proof of the v2 touch poll (PT1).
  - The same for a key (`key swallowed=1`) and for a lift (`lift`). Run `power imu` before and after the lift.
    Expected: `anymot` went up on the path Task 6 found (`int(gpio N)` or `int(expander …)`). If Task 6 couldn't fire any-motion without a move, this pick-up is the proof of the INT routing: record it. If `anymot` didn't move on an `int` path, the routing is wrong: record it and set the path to `poll` for the rest of the visit.
  - Run `python3 $T5 "power go deep-idle"`, then ask for the pick-up in Deep idle. Then `power wakes` and `power imu`.
    Expected: a `lift` wake, and `anticipate` went up before it. If it works, set `PW_DEEP_IDLE_TOUCH_OFF` to 1 in `power.c` (the spec's touch-off, now that the lift wake is proven on the device), rebuild, flash, and run `power go deep-idle`, `power touch` (expected `poll off`) and `key esc`. Otherwise leave it at 0 and record why.
  - Run `power light test bind`, then `power light test user`, and record the user's description of mode 0.
  - Run `python3 $T5 "key log on"`, then, for the stock layout and then JIS (Settings → keyboard layout), ask for Ctrl+], Ctrl+P, Ctrl+M and Ctrl+K on the dashboard, and Ctrl+] again inside a take-over of `companion-test-bench`.
    Expected, in both layouts:
    - the log shows `cp=']' mods=ctrl`, `cp='p' mods=ctrl`, `cp='m' mods=ctrl` (not `key=enter`) and `cp='k' mods=ctrl`;
    - Settings, More and the overlay open;
    - Ctrl+] leaves the take-over.
  - If Ctrl+] isn't reported as `cp=']'` with ctrl in either layout, follow the spec's fallback:
    - `hk_takeover` maps Ctrl+P to LEAVE as well;
    - the toast and the overlay say `Ctrl+P to leave`;
    - update `test_takeover_leave` and run the host tests;
    - record it.
  - Run `key log off`.

- [ ] **Step 5: The bench on battery**

Run: `python3 $T5 "power bench start"`, then ask the user to unplug. After the user plugs back in, run `python3 firmware/tools/power_report.py --port /dev/cu.usbmodem211201 --bench --capacity-mwh <N>` and `--log`.
Expected: every row `ok` (or `skipped: not built` for what Task 6 dropped), including `deep-idle`, `floor` (3 samples: the board floor with every rail cut and the IMU suspended) and `hibernate` (3 samples, boot-to-live in `seconds`: Hibernate as it ships, with the C6 cut through `WIFI_EN`, and the IMU armed when it is a wake source). `hibernate` minus `floor` is the IMU's armed cost (plus the keyboard's, when no rail cuts it).

Check the charge pin: in the log, `flags` bit 0 should change at the unplug and the plug-in. If it doesn't track, set charging detection to the current direction only and record why.

- [ ] **Step 6: Wake from Hibernate with the side button**

Run: `python3 $T5 "power go hibernate"`. Ask the user to press the side button and to say when. When the console is back, run `python3 $T5 "power boot"` and take a screenshot.
Expected:
- `kind=hib-wake`;
- the screenshot shows `Waking: reconnecting to Mac` with no cards, or live cards once the poll landed (never stale ones drawn as live);
- `to_live` is recorded.

The wake time is from the user's press to `to_live`, and the notes give both parts. If only the reset button worked, record "reset only". Hibernate's default then becomes 0 in Step 8.

- [ ] **Step 6b: Wake from Hibernate with a pick-up, and the bag walk** (only if `power hib` lists the IMU as a source; otherwise record "IMU wake: not wired" and skip)

Run: `python3 $T5 "power imu cap clear" "power go hibernate"`. Ask the user to leave it a minute, then pick it up and say when. When the console is back, run `python3 $T5 "power boot" "power imu"` and take a screenshot.
Expected: `kind=hib-wake` with the IMU as the source, `last-wake=imu input` or `imu motion` (the user's touch or a key confirms it), the dim `Waking: reconnecting to Mac` view, and `to_live` recorded. The notes give pick-up to boot and boot to live.

If the pick-up didn't wake it within 5 s, the any-motion values are too strict: lower `IMU_ANYMOT_HIB_DUR` to 25 (0.5 s), rebuild, flash, and try once more. If a still device woke on its own during the minute on the table, they are too loose: raise `IMU_ANYMOT_THR` to 0x100 (125 mg) and try once more. Record the values that worked.

The bag walk, a short labelled test: run `python3 $T5 "power imu cap clear" "power log on" "power go hibernate"`, then ask the user to carry it in a bag, unplugged, for about 10 minutes. When it's back on USB, run `python3 $T5 "power imu" "power imu cap" "power resets"` and `python3 firmware/tools/power_report.py --port /dev/cu.usbmodem211201 --log`.
Expected, and record each:
- the number of IMU wakes during the walk (the `deepsleep` reset count went up by that number, and `power imu cap` lists the unconfirmed ones with their ages);
- whether the cap fired (`disabled`);
- the energy those wakes cost (tag 0 samples from the wake boots, from the log);
- the walk's length and kind (bag, stairs, car).
It is about 10 minutes of one walk, not a day's carry: label it so in the notes. If the walk gave more than 1 false wake per 10 minutes, raise `IMU_ANYMOT_HIB_DUR` to 100 (2 s) for the Step 8 defaults and record it.

Run `python3 $T5 "power imu cap clear"`.

- [ ] **Step 7: The Mac sleep/wake reconnect**

When the agent resumes after the Mac wakes, run:
- `pmset -g log | grep -E "Wake from|Entering Sleep" | tail -4` for the wall-clock sleep and wake times;
- `python3 $T5 "companion reconnect-log"` for the Tab5's link down/up times;
- the Mac `ps` count from Task 4 Step 6 (expected `0` or `1`, never more).

The reconnect time is link-up minus the Mac wake. If it is over 30 s, run the sleep once more with `companion kick off` and compare. Keep whichever is faster as the default, and record both.

- [ ] **Step 8: Set the defaults from the numbers**

"Off" below is `off-touch-poll`, the Off that ships on v2.
  - **Radio off:** on by default only if `pr_radio_off_worth_it(a, b, c, 5, checkin_s)` is true for the measured handshake path, where a is Off, b is `radio-off`, and c and the check-in time come from `checkin-handshake`.
  - **Light sleep:** on by default only if Task 6 kept it and its bench step shows a lower draw than Off, with the link still up afterwards.
  - **`kbd_off_cap`:** if `off-light-amber` minus `off-light-off` is over 5% of Off, cap it at the lowest brightness the user still saw clearly (default 30).
  - **Hibernate after (`pw_hibernate_h`):**
    - 0 if Step 6 needed the reset button;
    - otherwise 1 h if `(deep-idle − hibernate) mW × 1 h > 3 × E_boot`, where E_boot is the `hibernate` row's boot-to-live seconds × the `active` step's mW (an estimate), because the spec's Priority leans to battery after an hour;
    - otherwise keep 2 h.
  - **Deep idle after (`pw_deep_idle_min`):** stays 60. The Priority keeps the first hour responsive. Record the measured saving against the step it replaces (`radio-off` if Radio off ships on, else Off).
  - **Hibernate check-in:** stays 0. Record the Task 8 check-in's energy and time for the user to decide.
  - **Hibernate when still (`pw_still_hib_min`):** t_be = E_boot / (deep-idle − hibernate) mW is how long Hibernate must last to pay back one wake.
    - 0 (off) if stillness never read as still during the visit (`power imu` showed `still=unknown`), or Step 6 needed the reset button;
    - otherwise 30 if 3 × t_be ≤ 30 min;
    - otherwise 3 × t_be rounded up to 10 min, at most 120.
  - **IMU wake from Hibernate (`pw_imu_wake`):** stays 1 if Step 6b woke on the pick-up and the bag walk gave at most 1 false wake per 10 minutes; otherwise 0, and record why. The any-motion values in `imu.h` are the ones Step 6b settled on.
  - Bump `CFG_VERSION` to 20 (Task 7 took 19) so the device picks up each changed default once.
  - Build, flash and spot-check with `power settings`.

- [ ] **Step 9: Write the notes**

Write `docs/superpowers/notes/2026-10-23-power-measurements.md`:
- the per-step table (avg/min/max mW and mA, mWh), with the Deep idle and Hibernate draws called out (and power-off marked "not measurable on the device" if the INA226 lost power);
- the estimated battery life for a day of 1 h Active + 23 h Off, and the same with Radio off, labelled **estimate** with the capacity used;
- `power_report.py --ladder 8,10 --capacity-mwh <N>` with the defaults Step 8 chose: an **overnight** case (8 h idle) and a **day-out** case (10 h idle), each as mWh, the share of the battery, and the charge left, all labelled **estimate**; plus the same two with Hibernate off, for comparison;
- the touch costs: `off-touch-poll` vs `off-touch-awake` (the 10 Hz poll) vs `off-touch-asleep` (and whether touch came back);
- the keyboard light's cost;
- both check-in paths, the Deep-idle backoff and the Hibernate check-in;
- the Hibernate wake: the source that worked, press to boot, boot to live;
- the board floor (`floor`) and Hibernate as shipped (`hibernate`, C6 cut), and the difference; the INA226's own draw is in both rows and not in a shipped Hibernate;
- the IMU: the INT routing (proved by Task 6 or by the Step 4 pick-up), the pick-up wake from Hibernate (pick-up to boot, boot to live), the any-motion threshold and duration chosen, the bag walk's false wakes (labelled as one short test), whether the cap fired, and the touch-off in Deep idle (on or off, and why);
- the hotkey results in both layouts;
- the charge-pin result and what mode 0 shows;
- the `power` reading with no pack fitted (bus voltage, percent -1), if the user can briefly disconnect it, else "not tested";
- the Mac sleep/wake reconnect time, with and without the kick;
- the reset counts before and after;
- every gated and long-idle decision with the formula's numbers.

Keep simulated or forced-state (USB) results apart from battery results. Update the main spec's Known issues and the Roadmap row.

- [ ] **Step 10: Clean up and commit**

Stop `companion-test-bench`, then run `power log off`.

```bash
git add firmware/main/settings firmware/main/sys/sysinfo.c firmware/main/sys/imu.h firmware/main/sys/power.c firmware/main/ui/hotkeys.c firmware/main/ui/hotkeys.h firmware/test/host/test_hotkeys.c firmware/tools/power_report.py firmware/tools/test_power_report.py docs/superpowers
git commit -m "docs(power): battery measurements, gated and long-idle defaults from the data"
```

## Pre-flight rulings applied

- F1: `PR_SWALLOW_MS` 600, 601 ms test case, shield timing (Task 5).
- F2: `keyboard_post` and `keyboard_usb_hid_report` call `power_wake(PW_KEY)` in Off and Radio off (Task 5; Tasks 8 and 10 checks now work).
- F3: wake shield made on entry to any non-Active state (Task 5).
- F4: `woke_at` set first in the wake order, state non-Active until the end (Task 5).
- F5: the watch has its own `JSON_CAP` PSRAM buffer (Task 4).
- F6: effective interval is `floor > 0 ? floor : rate` (Task 4).
- F7: the fallback counts only closes the Tab5 didn't ask for (Tasks 4, 8, 9).
- F8: `test_max_lifetime` uses L = 5 (Task 4).
- F9: Step 3 expects skipped loop tests, Step 4 requires them to run (Task 4).
- F10: the Mac `ps` count is the awk top-level count (Tasks 4 and 10).
- F11: plog energy test expects 1 667, seconds covered = last_t - first_t + 1 (Task 1).
- F12: dark test colour is 0x604818 (Task 3).
- F13: the 20% case uses c = 0.1 mWh (Task 8).
- F14: `bus_v < 5.5` gives -1, new `test_no_pack_is_unknown`, no-pack reading recorded (Tasks 2, 10, Review Focus 3).
- F15: `power set` applies the clamps; Task 5 uses dim 10 / off 20 at 5, 15, 25 s; Task 7 uses the same (Tasks 5, 7).
- F16: commit lists and Files fixed: rulings file (Tasks 3, 4, 8), `dependencies.lock` (Task 7), `sysinfo.c` (Task 10), `build-spike/` in `.gitignore` (Task 6), preview stubs and `ui_preview` (Tasks 3, 4, 7, 8, 9).
- F17: `screenshot.c` calls `display_ctl_touch()` each time (Tasks 5, 6).
- F18: no touch ISR on boards other than v1, re-init after re-create (Tasks 5, 6).
- F19: `power sim touch` uses a virtual LVGL pointer; the bench checks the controller by I2C read (Task 5).
- F20: `keyboard.c` records the last LED values, `power light` prints `kbd:`, the hot-plug check compares them (Task 3).
- F21: fast resume switched off after each check-in and the input-wake reconnect (Task 8).
- F22: main spec's "Nothing extra runs on the Mac" line amended (Task 4).
- F23: `ui_screensaver_arm()` call removed and `power_go` moved to `power.h` (Task 5).
- F24: `power_console.h` with `power_console_init()`, tag scheme and NVS `sum` table (Task 1).
- F25: the light callback also copies the first needs-you session's name (Task 3).
- F26: "handshake" recorded only after the SSH connect succeeds (Task 8).
- F27: `sysinfo_poll` calls `sysinfo_charge_pin()` (Task 1).
- F28: `bmi270_set_acce_filter_perf(..., BMI270_POWER_OPTIMIZED)` after start (Task 7).
- F29: `ui_settings.c` added to Task 6 Files, spike notes record the `lvgl_port_remove_disp` behaviour (Task 6).
- F30: `screen_off = (state >= PS_OFF) && on_battery` passed to `pr_kbd_light` (Task 5).
- F31: hex-entry part dropped from Review Focus 5 and Task 3 (Steps 4, 7).
- F32: `pure` library includes `firmware/main`, Task 3 links it to `companion` (Tasks 1, 3).

## Amendments (2026-10-23)

These came from the spec's new Priority, "Long idle: Deep idle and Hibernate" and "Hotkeys for two thumbs" sections, and from controller ruling PT1. Task 1 was already done; nothing here changes it.

- A1: This Tab5 is board v2 (ST7123 display and touch, no GT911). Per PT1 there is no touch INT wake, so the power task polls the touch controller at 10 Hz in Off and deeper while LVGL is paused. The GT911 INT path is v1-only. This adds `display_ctl_board_version`, `display_ctl_touch_poll`, `power touch` and the bench step `off-touch-poll`. Task 10's "Off" baseline is now `off-touch-poll` (Global Constraints, Tasks 5, 6, 10).
- A2: Settings gain `pw_deep_idle_min` (60), `pw_hibernate_h` (2) and `pw_hibernate_checkin_h` (0 = off). These are the spec's `deep_idle_min`, `hibernate_h` and `hibernate_checkin_h`, with the `pw_` prefix the other power fields use. They get clamps, and there is still a single bump to `CFG_VERSION 18` (Task 2).
- A3: `pr_next_state` knows `PS_DEEP_IDLE` and `PS_HIBERNATE`. They are entered on battery only, from the idle times, plus Hibernate at battery ≤ 10% when not charging. Holds block both. Host tests cover every transition and the no-battery case. The log tags are 4 and 5, with check-ins at 0x84 and 0x85; `power_log.c` grows from 24 to 28 summary slots, because its table stopped at 0x83 (Task 5).
- A4: Deep idle (60 min) and Hibernate (2 h) are on by default, per the spec's Priority. This is an exception to "gated defaults off". Hibernate keeps its default only if a wake path that needs nothing more than the side button exists. Task 6 checks it on paper and electrically, and Task 10 with a real press. Otherwise the default is 0 (Global Constraints, Tasks 6, 10).
- A5 (my call): the battery ≤ 10% trigger acts only from Off or deeper, so it never powers down a device in use. It doesn't depend on `pw_hibernate_h`, it needs a known percentage and no charging, and it needs `hibernate_supported()` (Task 5).
- A6: Before Hibernate, the screen shows "Sleeping to save battery. Press the side button to wake." for 3 s (`ui_sleep_notice`). Input or a new hold during the 3 s cancels it (Task 5).
- A7: Task 6 gains Part B, hibernate:
  - it finds the wake sources (side button or reset, the RX8130 alarm IRQ, the IMU INT on an LP GPIO);
  - it uses a full power-off if the board has one, else `esp_deep_sleep_start`;
  - it checks survival (NVS, the Tailscale identity, the host-key pins);
  - it times a cold boot to a live dashboard.
  The power log can't sample while the CPU is off. So the bench step `hibernate` (always the last step) sets the INA226 to 1024-sample averaging and reads its last 8.6 s window at each wake, before the rails come up, and keeps it in NVS. Power-off may also unpower the INA226; then it is recorded as not measurable. The display teardown is now judged apart from light sleep: if teardown passes, Deep idle tears the display down and runs the CPU at 40 MHz (`CONFIG_CC_DEEP_IDLE_TEARDOWN`), and the spike checks whether the one-chip ST7123 touch still answers after teardown. Each part records its outcome either way (Tasks 6, 8).
- A8: Deep idle backs check-ins off from 5 to 15 to 30 min, with the pure, host-tested `pr_checkin_period_min`. The light's age limit follows the scheduled period. With Radio off disabled (its default until Task 10), Deep idle keeps the long poll and has no check-ins (Task 8).
- A9 (my call): the optional Hibernate check-in sits behind `pw_hibernate_checkin_h`, default 0. It is a timer or RTC boot, then connect, one poll, amber light on a needs-you, and back to Hibernate. `power checkins` moves to NVS so its entries survive. If Task 6 finds that the keyboard rail drops while hibernating, a needs-you check-in stays up in Deep idle instead, so the amber can be seen (Task 8).
- A10: The keyboard light in Deep idle is off (`PR_LED_OFF`) unless something needs you, and then exactly 30%. `pr_kbd_light` gains `deep_idle`, with `test_light_deep_idle` (Task 3).
- A11: New Task 9b, "Hotkeys for two thumbs". It is numbered 9b so every "Task 10" reference stays right.
  - The pure module is `ui/hotkeys.c`.
  - Ctrl+] leaves a take-over (Ctrl+Q stays as an alias) and Shift+▲/▼ scrolls.
  - On the dashboard, Ctrl+P opens Settings, Ctrl+M opens More, and Ctrl+K shows an overlay that any key closes.
  - It adds the new footer hint and the 3 s take-over toast (`ui_toast_ms`).
  - None of these keys can reach a card's answer field. They are checked before the dashboard's no-cards early return.
  - From reading `keyboard.c`: Ctrl+M arrives as `cp='m'` with `MOD_CTRL`, never as `SK_ENTER`, in the HID, matrix and USB paths.
  - The console gains `key ctrl <char>` (it took only a–z, so `key ctrl ]` failed), `key shift <key>` and `key log` (Task 9b).
- A12: Task 10 additions:
  - it measures the Deep idle and Hibernate currents;
  - it times a side-button wake from Hibernate;
  - it tries the real Ctrl+], Ctrl+P, Ctrl+M and Ctrl+K presses in both layouts, with the spec's Ctrl+P fallback;
  - it sets the long-idle defaults by stated rules;
  - it estimates an overnight case (8 h idle) and a day-out case (10 h idle) with `power_report.py`'s host-tested `ladder_mwh`, labelled estimates.
  The bench grows to about 30 minutes, more than the spec's "about 20", because it now covers Deep idle and Hibernate. The user checks moved from Step 3 to Step 4 (Tasks 3, 5, 7 and 10).
- A13: Review Focus item 5 is now "a wake from Hibernate shows stale cards as live", covered by `pr_cards_view` and the Task 6 Step 9 screenshot. The old item 5 stays covered by `test_amber_like_colours`. Items 3 and 4 widen to the long-idle states and the backed-off check-ins.

## Amendments (2026-10-23, IMU-driven deep sleep)

These come from the spec's new section "IMU-driven deep sleep (the user, 2026-10-23): where the power actually goes" and its updated Settings list. Tasks 1–4 were already done, and Task 5's brief had already been extracted, so Task 5 only gains an input hook; the IMU-dependent work sits in Tasks 6 and 7.

- A14: Task 5 gets a minimal hook. `pr_in_t` gains `still_ms` (the IMU's no-motion time, 0 when unknown) and `still_hibernate_ms` (0 = off), and `pr_next_state` enters Hibernate when on battery, with `hibernate_ok`, `still_ms ≥ still_hibernate_ms` and no input for that long, beside the idle-hours and low-battery triggers. Task 5 passes 0 for both, so it never fires until Task 7. One host test, `test_hibernate_when_still`. The "Amendment for Task 5" block at the top of Task 5 is the text to append to the extracted brief (Task 5).
- A15: Task 6 Part B starts with three new steps, ahead of everything else, lettered 4a–4c so existing Step references stay right (Task 6).
  - 4a finds where the BMI270's INT1/INT2 go (a P4 GPIO, an LP GPIO 0–15, or an expander input whose INT reaches the P4). It drives the pin with raw registers and watches every unclaimed GPIO and both expanders' input and interrupt-status registers: a polarity flip with nothing mapped, then data-ready at 50 Hz, so the routing is proved without motion. It then tries the BMI270 accelerometer self-test to fire any-motion. The BMI270 has no documented INT test mode, so if that fails, the any-motion trigger needs a physical move and goes to Task 10. It also records which feature blob the driver uploads.
  - 4b finds whether the board has a real power-off and what can switch it back on, without driving unknown pins.
  - 4c maps which rails the expanders can cut (LCD_EN, TOUCH_EN, USB_EN, WIFI_EN, speaker, camera, the keyboard's power) and which pins must stay as they are. It also records whether `TOUCH_EN` takes the ST7123 panel too.
  - Hibernate now cuts every rail it can, `WIFI_EN` included (the C6 fully off; the cold boot re-initialises SDIO anyway), suspends the BMI270 unless it is a wake source, and puts the INA226 into power-down outside the bench.
  - A new probe-type bench step `floor`, just before `hibernate`, measures the board floor with everything cut. `hibernate` measures Hibernate as shipped.
  - If the INT reaches an LP GPIO, Hibernate uses ext1 on it, proved on USB by `power hib test imu-drdy`. Step 8's survival check also covers the rails coming back.
  - New module `sys/imu.c` (raw register access, through the public `bmi270_handle_t.i2c_handle`). The `espressif/bmi270 ==1.1.0` pin moves from Task 7 to Task 6, its first user.
- A16 (my call): the spec's confirm window counts "continued motion" as confirmation, but a bag on a walk is continued motion too. So a wake confirmed only by motion is provisional: if the ladder reaches Off with no key or touch, it counts as unconfirmed toward the cap and goes straight back to Hibernate, instead of walking the 60 min ladder awake. This bounds a bag's cost at about 3 wakes of at most `pw_off_s` + boot each per hour before the cap turns IMU wake off (Task 7, Review Focus 6). — why: the user named the bag drain as the risk to guard — cost if wrong: a user who picks the device up and only looks at it, never touching it, sees it go back to sleep after the screen-off time.
- A17 (my call): if Hibernate is a power-off and the boot can't tell an IMU wake from a button press, Hibernate uses deep sleep whenever it arms the IMU, so the guard always knows the source. Task 10 records the draw difference (Task 7).
- A18: Task 7 becomes interrupt-first.
  - Any-motion and no-motion are set up through raw BMI270 feature-register writes, since the 1.1.0 driver has no interrupt API, with the accelerometer in low-power mode (adv_power_save, `ACC_CONF 0x17`). F28's `bmi270_set_acce_filter_perf` call is replaced by that raw `ACC_CONF` write.
  - A lift wakes from the INT while the P4 is awake: an any-motion interrupt starts a short `lift_detect` burst, and its WAKE wakes the screen. Polling INT_STATUS_0 at 2 Hz is only the fallback when the INT isn't wired. There is no `lift_arm` any more: the IMU runs in every state.
  - It supplies `still_ms` to the ladder through the pure `pr_still_*` accounting.
  - The pure `pr_imu_cap_*` and `pr_imu_confirm` give the false-wake guard: a 20 s confirm window (key, touch or continued motion), else power down again, and 3 unconfirmed IMU wakes per hour turn IMU wake off until a side-button or key wake. The cap is kept in NVS (it must survive power-off as well as deep sleep), and an entry made before the clock is valid never ages out.
  - The step that adopts Bosch's feature config checks its licence: the blob ships in the Apache-2.0 component, Bosch publishes it as BSD-3-Clause, and Bosch's header is kept if the blob or any code is copied.
  - Touch off in Deep idle (cut `TOUCH_EN`, stop the 10 Hz poll) is built behind `PW_DEEP_IDLE_TOUCH_OFF`, default 0. Task 10 turns it on only after a real pick-up wakes the device from Deep idle.
  - New settings `pw_still_hib_min` (30, 0 = off, 10–240) and `pw_imu_wake` (1), with `CFG_VERSION 19` and a v18 migration. The IMU wake defaults on, per the spec's Settings list, which is an exception to "gated defaults off", like A4's.
  - New Settings rows: Wake on pick-up from Hibernate, Hibernate when still for (Task 7).
- A19: Anticipatory wake. An any-motion event in Off or Deep idle calls `power_anticipate()`, which drops the poll floor to 0 for 30 s without lighting the screen. Task 8 extends it so a parked radio unparks and reconnects at once (check-in steps 1–5, then up for 60 s). The bench grows to 15 steps with `floor`, within `BENCH_MAX` 16 (Tasks 7, 8).
- A20: A persistent timer backstop (`power hib backstop <s> <n>`, RTC memory) and `power hib test imu-drdy [n]` let Task 7 run the false-wake guard end to end on USB, three data-ready IMU wakes in a row, without motion and without the user. The Global Constraints gain "never hibernate without a timer backstop while the user isn't there" (Global Constraints, Task 7).
- A21: Task 10 additions:
  - it measures `floor`, Hibernate with the C6 cut, and the pick-up wake from Hibernate;
  - it does the physical pick-up checks for the INT routing if Task 6 couldn't trigger any-motion, and the Deep-idle pick-up that gates the touch-off;
  - it counts false wakes during a short bag walk (about 10 min, labelled as one short test, not a day's carry) and tunes the any-motion values from it;
  - it sets `pw_still_hib_min` by a stated payback rule (3 × the time Hibernate needs to repay one wake, at least 30 min, at most 120) and keeps `pw_imu_wake` only if the pick-up worked and the walk gave at most 1 false wake per 10 min;
  - its `CFG_VERSION` bump becomes 20, since Task 7 took 19. The user's one message gains the pick-ups and the bag walk (Task 10).
- A22: Review Focus gains item 6, a false IMU wake in a bag draining the battery, covered by `test_imu_cap`, `test_imu_confirm`, the Task 7 Step 7 loop and the Task 10 Step 6b walk.
