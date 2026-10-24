# Plan 2 Task 6: light-sleep and hibernate spike

Board: M5Stack Tab5, board v2 (ST7123 display and touch, ruling P1), ESP32-P4 rev < 3, on USB throughout.
Sources: the Tab5 schematic (`Tab5_Schematics_PDF.pdf` from docs.m5stack.com/en/core/Tab5; pages 1, 2, 4, 5),
M5Stack's factory firmware (github.com/m5stack/M5Tab5-UserDemo at b4e356b, `platforms/tab5`), the BSP
(`espressif__m5stack_tab5`), and probes on this board. Rulings P57–P66 carry the decisions.

## Part A: display teardown and light sleep — both dropped

**Tried.** `display_ctl_teardown()` (on the LVGL task: `lvgl_port_remove_touch`, the DPI "done" callback
unregistered, `lvgl_port_remove_disp`, `bsp_display_delete()`, then a touch read to decide whether to keep the
touch handle) and `display_ctl_recreate()` (`bsp_display_new_with_handles` at the BSP lane rate,
`lvgl_port_add_disp_dsi` with the app's config, the kept touch handle re-added with `lvgl_port_add_touch`,
rotation, `ui_display_recreated()`, one synchronous `lv_refr_now`). Built as `build-spike` with
`CONFIG_PM_ENABLE`, tickless idle, `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION` and `CONFIG_CC_LIGHT_SLEEP_SPIKE`.
The code is kept as `2026-10-23-light-sleep-spike-partA.patch` beside this file; it is not in the tree.

| Check | Result |
|---|---|
| Step 1: spike build | `Project build complete` (PM + tickless with SMP builds) |
| 10 cycles, no crash | 10/10 |
| internal free within 4 KB | 64 003 B before, 63 611–64 043 B after each cycle, 63 979 B after the 10th: pass |
| PSRAM free within 64 KB | 22 300 760 B before, 22 295 792–22 300 748 B: pass |
| wake-to-picture < 500 ms | 680, 685, 680, 681, 681, 681, 688, 684, 680, 681 ms: **fail** (slowest 688 ms) |
| screenshot afterwards | the dashboard, correct |
| `touch after teardown` | `ok` every cycle: the ST7123's touch side answers with the panel deleted, so the v2 poll could keep its handle |
| link after 10 min | `connected Mac`, `watch: on I=15s` at 10 min 23 s |
| `power resets` | no new panic or brownout |

**Where the 680 ms goes.** About 440 ms from the start of the re-create to the touch re-add, nearly all in the ST7123
driver: `panel_st7123_reset` sends SWRESET and waits 120 ms, the 27-command init table waits 5 ms after each command,
then sleep-out waits 120 ms and display-on 50 ms (`esp_lcd_st7123.c`, `disp_init_data_1.h`). The remaining ~240 ms
is the LVGL display, the dashboard build and the first full refresh. None of it is skippable through the BSP API.

**Decisions.** Teardown failed its 500 ms check, so it is reverted (no `Kconfig.projbuild`, no `sdkconfig.spike`),
`pw_light_sleep` stays reserved, and Deep idle keeps the display (panel off) at the full CPU clock. DFS is dropped too:
the DPI panel holds `ESP_PM_CPU_FREQ_MAX` from creation to deletion (`esp_lcd_panel_dpi.c`), and the DSI bus holds
`ESP_PM_NO_LIGHT_SLEEP`. Light sleep needs the teardown, so it is dropped with it. Off ships as panel off + quiet
tasks + Wi-Fi modem power save + the long poll + the Radio-off tier.

**Learned, for anyone who retries.**
- `lvgl_port_remove_disp` deletes every screen of the display, its layers too: the nav stack's screen, the layer-top
  wake shield, toasts, the music bar and the sleep notice. Pointers to them must clear on `LV_EVENT_DELETE` (the toast
  and the music bar had no such handler). `ui_nav_refresh()` rebuilds only the top of the nav stack.
- Indevs left without a display are inert until `lv_indev_set_display`; the BSP's `bsp_display_get_input_dev()` keeps
  returning the deleted touch indev.
- With no display, the esp_lvgl_port task never calls `lv_timer_handler` and wakes every tick (`esp_lvgl_port.c`), so
  `lv_async_call` never runs and the CPU never goes quiet. A 1×1 placeholder display without a refresh timer fixes
  both.
- The teardown ran fine while LVGL was paused (P44): a period-0 async timer still fires, and
  `lvgl_port_task_wake(LVGL_PORT_EVENT_USER)` cuts the wait.

## Part B: hibernate

### Step 4a: the BMI270 (PT15)

- Chip ID: `0x24` at 0x68 (`power imu`).
- Nobody in the BSP or the fork starts it, yet the first read found `pwr_ctrl=0x0e pwr_conf=0x00`: accel, gyro and
  temperature on, left by an earlier firmware (the IMU keeps power across P4 resets). `imu_suspend()` (PWR_CTRL 0,
  PWR_CONF 0x03) now runs at every boot as well as on the way into Hibernate and `floor` (P58). After it:
  `imu: chip=0x24 suspended=yes (pwr_ctrl=0x00 pwr_conf=0x03)`.
- Schematic, for the record: BMI270 INT1 drives Q8 (2N7002, R110 10 k pull-down on its gate), whose drain is E_TRG,
  the power controller's trigger (below). INT2 is not connected. Neither reaches a P4 GPIO or an expander. So an
  INT1 configured active-high push-pull would switch the board on from power-off (that is the factory firmware's
  "shake to wake"); motion wake is out of scope (PT15), and the suspend keeps INT1 quiet.

### Step 4b: a real power-off exists

- **The line:** expander 0x44 P4, PWROFF_PULSE (schematic p1 U7, p5). Through D12 it turns on Q3A, which pulls
  nINT_STAT_TRIG, PA6 of U28, low. U28 is a PMS150G-U06, a custom-programmed MCU on the always-on VDD_STBY rail; its
  PA3 is MPWR_EN (the main power enable), PA4 SW_PWR (the side button S1), PA5 BOOT_GPIO35.
- **Factory firmware:** `bsp_generate_poweroff_signal()` pulses P4 high for 100 ms three times; `powerOff()` calls it.
  `sleepAndRtcWakeup()` sets an RX8130 alarm and powers off; `sleepAndShakeWakeup()` arms BMI270 any-motion on INT1
  and powers off.
- **What can switch it on (paper):** the side button (SW_PWR, PA4; M5Stack docs: "press once to turn on, double-press
  to shut down"); E_TRG, pulled by the RX8130's nIRQ (through Q11/Q12) and by the BMI270's INT1 (through Q8); and a
  USB plug-in edge (SYS_5VBUS through D14, C129/R81 and Q4 onto Q3A's gate).
- **Electrical check (Step 7):** see below. On USB the power-off took; the RX8130 didn't bring it back.

### Step 4c: the rail map

From the schematic (U6 = 0x43, U7 = 0x44) and `power rails` (`hi-z` = the expander's output disabled; the driver's
reset leaves every pin like that, with its pull-down on):

| Pin | Net (schematic) | BSP name | Hibernate | `power rails` on USB, awake |
|---|---|---|---|---|
| 0x43 P0 | RF_PTH_L_INT_H_EXT (antenna switch) | – | kept | hi-z 0 |
| 0x43 P1 | SPK_EN (NS4150B) | SPEAKER_EN | cut | hi-z 0 |
| 0x43 P2 | EXT5V_EN (M5-Bus and J9 5 V) | – | cut | hi-z 0 (off: the BSP never enables it) |
| 0x43 P3 | not connected | – | – | hi-z 0 |
| 0x43 P4 | LCD_RST | LCD_EN | cut (held in reset) | in 1 (released, open-drain) |
| 0x43 P5 | TP_RST | TOUCH_EN | cut (held in reset) | out 1 |
| 0x43 P6 | CAM_RST | CAMERA_EN | cut (held in reset) | hi-z 0 |
| 0x43 P7 | HP_DET (input) | – | never driven | hi-z 0 |
| 0x44 P0 | WLAN_PWR_EN (the C6's supply) | WIFI_EN | cut | out 1 |
| 0x44 P1, P2 | not connected | – | – | hi-z 0 |
| 0x44 P3 | USB5V_EN (USB-A host 5 V) | USB_EN | cut | out 1 |
| 0x44 P4 | PWROFF_PULSE | (the fork read it as the charge pin) | driven only to power off | hi-z 0 |
| 0x44 P5 | nCHG_QC_EN (IP2326) | – | never touched | hi-z 0 |
| 0x44 P6 | CHG_STAT (IP2326 LED pin, via R37) | – | never driven | hi-z 0 |
| 0x44 P7 | CHG_EN (IP2326 EN, via R59) | – | never touched | hi-z 0 |

- Both expanders' nINT pins are not connected.
- **LCD_EN, TOUCH_EN and CAMERA_EN are resets, not supplies.** Cutting them holds the parts in reset; the panel and
  touch supplies stay on. `TOUCH_EN` (TP_RST) resets the touch side of the one-chip ST7123 only by name; on a one-chip
  part a reset line may well take the panel too, and the schematic can't tell. Informational since PT15.
- **The charge pin was wrong.** The fork's `sysinfo_charge_pin()` read 0x44 P4, which is PWROFF_PULSE with the
  expander's pull-down: it reads 0 on any supply, and Plan 2's rules treat 0 as "USB charging", so the device could
  never have read "on battery". It now reads P6 (CHG_STAT). On USB P6 reads 0 too, so nothing changes today; its
  polarity on battery is for Task 10's unplug check (P59).
- Not ours to fix, but recorded: CHG_EN (P7) sits at the expander's pull-down, and the pack has read 50 % at
  7.58 V with +1–2 mA (out of the pack) for hours on USB. It may not be charging. The factory firmware drives P7 low
  too, so low may be its normal state; Task 10 should look.
- **The keyboard has no switchable rail.** It sits on J9 (pins: SYS_VIN, SYS_EXT5VO, SOC_3.3V, G0/G1 I²C, G49, G50
  INT). With EXT5V_EN driven low, `power light test reprobe` found it again (`kbd: Tab5 keyboard detected`), so it is
  on SOC_3.3V or SYS_VIN. Its share of the floor is "not separable". `power rail ext5v on|off` exists for the record.
- **The keyboard survives deep sleep with the light held (PT16):** after `power hib test light 60` (amber, 30 %,
  written just before the sleep), the next boot read `kbd: light at boot: mode=1 bright=30 rgb=f4b545` before writing
  anything: the A164 kept power and state through 60 s of deep sleep. `kbd rail survives deep sleep: yes`,
  `light holds: yes` (registers; the user looks in Task 10). A power-off drops SOC_3.3V, so it holds in deep sleep
  only. `hibernate_kbd_rail_survives()` is true and the `hibernate-light` bench step is built (P61). What holding the
  light costs is Task 10's `hibernate-light` row minus `floor`.

### Step 5: wake sources on paper

- **Side button (S1):** SW_PWR into the PMS150G's PA4. It drives the power controller, not EN and not a GPIO. When the
  board is on (deep sleep counts as on) a press goes to the PMS150G's firmware, whose behaviour isn't documented
  beyond "double-press to shut down". **PT26/PT27 (final review C1, on hardware 2026-10-23):** one press wakes deep
  sleep: `power hib test timer 900`, pressed at once, booted about 76 s in, as hw reason 7 (HP watchdog), the same
  reset as a timer wake. A press on an awake board also resets it.
- **Reset button:** none on the board. SOC_RST goes to the P4's reset circuit and the M5-Bus EN pin; the USB-UART
  bridge (CH9102F, U32) is marked NC. M5Stack's "Reset/Boot button" is S1 through the PMS150G (PA5 drives
  BOOT_GPIO35).
- **RX8130 IRQ:** nIRQ → Q11 → Q12 → E_TRG, the power controller's trigger. Not an LP GPIO.
- **Power-off line:** 0x44 P4 PWROFF_PULSE (Step 4b).
- Keyboard: survives deep sleep with the light held (Step 4c), not power-off.

### Steps 6–7: built, and each wake path on USB

`power hib` → `hibernate: mode=off sources=timer,button supported=yes` (the build now on disk; the device last ran
the build before P60, which printed `sources=timer supported=no`).

| Test | Result |
|---|---|
| `power hib test timer 30` | back after ~31 s; `boot: reason=7 kind=hib-wake cause=timer` |
| `power hib test timer 10` (Step 7 retry, Step 9 ×4) | back after ~10.5 s each time |
| `power hib test light 60` | back after ~63 s; the A164 held amber 30 % |
| `power hib test off 60` (power-off + RX8130 60 s timer) | **the board powered off with USB in** (`E BOD: Brownout detector was triggered`, USB gone); the RX8130 timer (`ext=0x12 ctrl0=0x10 count=60: armed`) **did not switch it back on** within 120 s; still off at the time of writing; it needs the side button |
| `power hib test rtc` | the same path as `off`: not repeated |
| side button | **wakes it from power-off** (user-reported, 2026-10-23): the user's press brought it back at ~15:41, 2 h 9 min after the 13:32 power-off; the RX8130 timer never did. The power-on arrives as an HP watchdog reset (reason 7), like a deep-sleep wake (P114) |

- **Deep sleep wakes on the timer through a watchdog reset.** Every deep-sleep wake arrives as `ESP_RST_WDT`
  (hw reason 7, HP system reset from the HP watchdog) with no wakeup cause, about 0.5 s after the timer. The cause is
  inferred from the RTC-kept clock (`hib_rules.c`, P62) and `power resets` counts it as `deepsleep`.
- **First power-off attempt aborted:** cutting WLAN_PWR_EN before the 600 ms of pulses made esp_hosted's next SDIO write
  fail (`Unrecoverable host sdio state`, `abort()` in `eh_host_port_restart_host`). The pulses now go first (P63).
- **Why the RTC didn't wake it** is open. Candidates: the PMS150G may ignore triggers for a while after a power-off,
  or treat a held-low E_TRG (the RX8130 holds nIRQ low until TF is cleared) differently from the factory's alarm;
  TSEL=010 (1 Hz) may be wrong for this part. The boot after the button press will log the RX8130 flags
  (`rtc flags 0x..`): TF (0x10) set means the timer fired and the PMS150G didn't act on it.
- **Wake cause (P62, amended by P68):** an RX8130 flag counts only when the time asleep matches the armed timer
  (after a power-off the time comes from the RX8130's own clock). A stale flag on a power-on is the button, and the
  boot log says the RTC fired earlier. A power-off that doesn't take deep-sleeps with its timer, or a 60 min
  backstop when it had none.
- **Superseded by PT26 (final review):** every product Hibernate is a deep sleep with a timer (the check-in, or a
  2 h power probe that reads the INA226 once and boots on USB). The button wakes deep sleep (PT27), and a power-off
  drops the expander and with it CHG_EN (PT22), so a powered-off Tab5 would not charge; deep sleep keeps P7 driven
  (seen: the probe wake reads -295..-464 mA into the pack before any init). The power-off stays for
  `power hib test off`. Task 10 measures the deep-sleep floor against the power-off floor.
- **Mode chosen (P60, superseded by PT26):** power-off for an untimed Hibernate (the ladder, battery ≤ 10 %), with the side button as its
  wake; a timed one (test-wake, bench probes, Task 8's check-in) deep-sleeps, since only the timer is proved and only
  deep sleep has it. Power-off beats deep sleep because something other than USB (the button) switches it on, and its
  floor is the PMS150G and RTC on VDD_STBY.

### Step 8: state survives a hibernate (rails cut)

After `power hib test timer 30` (Step 8 ran on that wake): the same `@@PUBKEY` (md5 of the line `00000000…` before and
after), the same Tailscale IP (100.64.0.20), `connected Mac` with no `KH_UNKNOWN` in the log, `power settings`
unchanged, `power rails` the same levels, the INA226 reading 7.58 V, Wi-Fi joined (the C6 cold-booted from
WLAN_PWR_EN), the keyboard answering `power light`, and `power touch` in Off: `poll on, reads=8 errors=0`.
`power resets` gained one `deepsleep` (after P62's change; the first two tests counted as `wdt`).

### Step 9: cold boot to a live dashboard

Each first screenshot showed `Waking: reconnecting to Mac` and no cards. `to_live` (boot to the first rc-0 result):

| Run | to_live |
|---|---|
| 1 | a brownout reset 6.5 s into the boot (Wi-Fi starting, screenshot running; on USB); its retry, a normal boot, was live at 45.1 s |
| 2 | 67.8 s |
| 3 | 45.9 s |
| 4 | 46.2 s |

A normal boot after a flash reaches live in 45.6–45.9 s. The spec's 50–130 s holds at the low end.

### Steps 10–11 (run after the press, on the branch HEAD 414703e plus the P114 fix)

- **Step 10, `power bench start fast`** (15 steps with Tasks 8 and 9). The rows: `hibernate-light`, `floor` and
  `hibernate`, 3 probe cycles each, all `usb`, with `-` for mWh. `hibernate` shows 69 s in `seconds`
  (boot-to-live 68.8 s). `hibernate-light` ended in a normal boot, and the bench went on at step 13 (`floor`) by
  itself; `floor` chained into `hibernate` from the early boot.
  - Each probe sample read 7580 mV, 0.5 mA (one shunt LSB: on USB the pack carries ~0), at boot+20 ms.
  - The final boot browned out once (on USB) and went live on the retry.
- **Step 11** (`power hib test-wake 30`, `power sim battery 8`, `power set off 20`): Active → Dim → Off → the
  notice → Hibernate. It deep-slept on the test-wake timer, came back after 33 s with `boot: kind=hib-wake
  cause=timer`, and the sim battery was gone. A second run logged `hib: battery low: hibernating with no
  check-in` (Task 8's ladder) and `down from task hib, stack free 4176 B`.
- **Stack high-water marks:**

  | Task | Stack | Free at the lowest |
  |---|---|---|
  | `hib` (the ladder's hibernate) | 6144 B | 4176 B |
  | `bench` | 6144 B | 3820–3916 B |
  | `bench_boot` | 4096 B | 2212 B |
  | main, probe boots | 16 KB | 14 744 B |
  | Task 8's check-in job | 6144 B | 3780 B |

- **The keyboard light on HEAD:** after `power hib test light 30`, the next boot again read
  `mode=1 bright=30 rgb=f4b545`.
- **Brownouts:**
  - Since the HEAD flash, there have been 5 brownout resets on USB, each about 8 s into a boot, right after
    Wi-Fi's start event: two right after the flash, one at the end of the bench, and two after a Step 11 wake.
    Every boot got live on a later try.
  - On the Task 6 builds there was 1 in about 12 boots.

### Step 12: the Hibernate default

PT26: the mode is deep sleep (`power hib` prints `mode=deepsleep sources=timer,button`), and the text below
describes the earlier power-off default. `hibernate_h` stays 2 (P60). The path: power-off with the side button (on paper; the device's normal power-on),
plus a USB plug-in edge on the same trigger. Task 10's press confirms it, and that press is also what brings this
device back now. The battery ≤ 10 % trigger stays on (`hibernate_supported()` is true).

### Bench notes

- The probe steps keep the INA226 on (it is the meter), so their rows include its own ~330 µA (datasheet typical),
  which a shipped Hibernate doesn't pay. Resolution is the shunt register's 0.5 mA (≈ 4 mW at 7.6 V).
- `hibernate` equals `floor` by design: both are timed deep sleeps with every cut rail cut and the BMI270 suspended.
  Power-off draw is not measurable on the device: the INA226 loses power with the board and only a press wakes it.
