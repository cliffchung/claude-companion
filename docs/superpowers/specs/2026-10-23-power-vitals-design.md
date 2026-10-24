# Plan 2: Power & vitals: Design

Status: written 2026-10-23 as a follow-on to the main spec
(`docs/superpowers/specs/2026-10-22-claude-tab5-design.md`, which stays binding). Roadmap row:
"backlight dim → light sleep; wake on keyboard or touch (lift dropped, PT15); fast reconnect on wake; full vitals
bar". The user said "keep going with stage 2" and gave no further direction, so every choice below
marked *(my call)* is mine and can be overruled.

## Intent

The Tab5 lives on the couch, mostly unattended. It should:

- last a long time on battery;
- go dark when nobody is looking at it;
- come back the instant you touch it, with the Mac link already live;
- still tell you when a session needs you.

At a glance it shows the vitals you expect from a handheld: battery, charging, Wi-Fi, the Mac link
and the clock. This serves R4 (keyboard-first), R5 (a beautiful native UI) and R6 (power-efficient,
sleeps on its own, shows battery %, charging, Wi-Fi and the clock).

## Priority (the user, 2026-10-23)

**Maximum battery life over long idle**, such as overnight or a day out, is the top priority. When a
default trades battery against responsiveness after more than an hour without input, it leans
toward battery. The first minutes of idle stay responsive.

## Goals

1. **Idle ladder.** Active → Dim → Screen off → Radio off → Deep idle → Hibernate (and automatic
   light sleep only if the spike proves it). Any key or touch
   goes straight back to Active. (PT15: lift removed.)
2. **Wake on keyboard and touch (PT15: lift removed; Hibernate wakes on the side button).** The first key press that wakes the device is swallowed. It
   wakes the device and does not type or act, so a sleepy keypress can never answer a session.
3. **A needs-you wake** *(my call)*. When a background session turns NEEDS YOU while the screen is
   off, the screen comes back at the dim level showing the dashboard. This is the companion's whole
   point. It is a setting, on by default, and it never fires for a session that was already NEEDS
   YOU when the screen went off.
4. **Fast reconnect on wake.**
   - From Off, the link never dropped, so the dashboard is fresh within 2 s.
   - From Radio off, it is live again within the measured check-in time: target 10 s on the
     WireGuard-handshake path.
   - Also measure the Mac sleep/wake case that the main spec's known issues leave open.
5. **Full vitals bar** on the dashboard: battery % with a level glyph, a charging bolt, Wi-Fi
   strength from RSSI, Tailscale, the Mac link dot and the clock. Low battery (≤15%, not charging)
   is shown in red, never amber.
6. **Correct local time.** Replace the fixed UTC offset with a POSIX TZ rule so PDT→PST
   (2026-11-01) is right. The default is US Pacific `PST8PDT,M3.2.0,M11.1.0`.
7. **Long poll** (the user's idea, 2026-10-23). Instead of the Tab5 asking every few seconds, one SSH
   exec channel runs a short loop on the Mac. The loop checks `claude agents --json --all` itself
   and sends output only when it changes, plus a heartbeat. The Tab5's radio and CPU stay idle
   between changes.
8. **Wi-Fi toggling** (the user's idea). After a long idle, turn the radio off and check in on a
   timer: Wi-Fi on → link → one poll → Wi-Fi off. It ships on by default only if measurement shows it
   saves energy compared with staying connected on the long poll (see Radio-off tier).
9. **Keyboard light shows needs-you** (the user's idea, 2026-10-23). The A164's RGB light turns
   amber (D9 `#f4b545`) while any background session needs you, so you can tell at a glance, even
   with the screen off. It goes back to the user's own light setting when none does. Details are
   under Keyboard light.
10. **Measured, not guessed.** The INA226 measures the device's draw in each idle state, and the plan
   reports the estimated battery life from those numbers. Per the user's "never wait" rule, there
   is no draining the battery to find out. Label it as an estimate.

## Plan 1 follow-ups folded in

They touch the same code (cc_link, reconnect), so they belong here:

- **Forget the Mac's host key:** an on-device action with a confirm step (Settings → Companion).
  It clears the link's pin and the terminal's pin for that host, then reconnects and pins the new
  key, with the new fingerprint shown in the confirm toast. This is the only recovery from a changed
  host key today.
- **cc_link minors:**
  - `cc_link_failure()` takes the lock;
  - an `ssh_new()` failure clears or sets the failure reason.
- **Brownout resets on USB power:**
  - log `esp_reset_reason()` at boot, and keep a reset-reason counter in NVS that the console shows;
  - stagger the boot-time rail enables (LCD, touch, Wi-Fi, USB);
  - set the brownout level explicitly.
  - Don't guess at more: report what the counter shows.
- **Real permission-dialog capture (R19):** create a `companion-test-perm*` session that asks for a
  tool permission, capture the real attached screen, and add it as a host-test fixture so the gate's
  "refuse" case uses real bytes.

## Non-goals

- Deep sleep during short idle. Waking means a full reboot and a 50–130 s Tailscale reconnect, so
  it is used only for long idle (see Long idle).
- Updating or reflashing the C6, or using esp_hosted's own host power-save protocol. The C6 stays
  untouched, as the main spec says.
- Camera, voice or the typography pass (Plans 3 and 4).
- Motion wake (PT15): lift wake, stillness-driven Hibernate, IMU wake from Hibernate and anticipatory
  wake were dropped at the user's request on 2026-10-23 ("forced to press the button"). They can
  come back later as their own task.
- Notifications other than the screen waking: no sound or vibration *(my call)*.
- Battery life on any battery other than the one fitted.

## Anti-goals (a failure even if it "works")

- A wake keypress, or a touch on a dimmed screen, that reaches the Mac or acts on a card.
- Amber used for anything other than NEEDS YOU, on the screen or the keyboard light, including low
  battery.
- Sleep that silently kills the link and comes back showing stale cards as if they were live. After
  any wake, cards show as stale until the first fresh poll.
- Sleeping while a session is open full-screen (take-over) or during a quick-answer send.
- Touching the user's tmux or a non-test session while testing.

## Constraints

- Everything in the main spec and Plan 1's global constraints carries over:
  - test only on `companion-test-*`;
  - no secrets in the repo or logs;
  - no GPL;
  - never open a login shell;
  - never flash the C6 or erase NVS;
  - never run `sshkey regenerate`.
- ESP-IDF 5.5.6, ESP32-P4 rev v1.3, esp_hosted 3.0.10 over SDIO, PSRAM XIP, USB host in use.
- New third-party code must be MIT/BSD/Apache. The BMI270 driver is `espressif/bmi270` 1.1.0,
  Apache-2.0 (verified by the builder check).

## How it works (decided parts)

### Power states (`sys/power.c`, new; one owner task)

| State | Enter when | Display | Link |
|---|---|---|---|
| Active | any input, a needs-you wake | brightness setting | long poll, `I` = 3 s |
| Dim | idle ≥ `dim_s` (default 30 s) | 20% of setting, minimum 5% | unchanged |
| Off | idle ≥ `off_s` (default 120 s) | panel display-off, backlight 0, **touch stays awake** | long poll, `I` = 15 s |
| Radio off | in Off for `radio_off_min`, on battery (see Radio-off tier) | as Off | radio parked, timed check-ins |

- **Touch stays awake in Off** *(builder check)*:
  - a sleeping GT911 doesn't report touches or pulse INT;
  - on this board revision the BSP holds INT low and skips the INT-high wake pulse, so
    `bsp_display_exit_sleep` could leave touch dead.
  - Off therefore uses `esp_lcd_panel_disp_on_off(false)` plus backlight 0, never
    `bsp_touch_enter_sleep`. Measure what keeping touch awake costs.
- **Holds that block dimming and sleeping:**
  - a take-over terminal is open;
  - a quick answer or action is in flight;
  - the screensaver's "disable in SSH" setting applies to the generic terminal.
- **On USB power** the screen still dims and turns off. Radio off applies on battery only.
- **The fork's LVGL screensaver is removed** *(my call, after the builder check)*. The ladder
  replaces it. Its standby clock draws in the theme accent, which is orange in the default preset
  and so breaks the amber rule; Settings drops its rows. Keep `ss_*` NVS fields readable so old
  settings blobs still load.
- **Quiet tasks in Off.** These keep the CPU awake for nothing, so they go quiet:
  - the keyboard task stops its 20 ms I²C poll and waits on GPIO50 INT (with a 1 s safety poll);
  - LVGL's refresh is paused, or its timer period raised;
  - the dashboard's 300 ms tick stops;
  - `idle_poll` slows to 10 s.
- **On wake:**
  1. panel on;
  2. restore the saved brightness;
  3. `lv_display_trigger_activity`;
  4. swallow the waking input;
  5. mark the cards stale;
  6. restart the watch at `I` = 3 s.
  - A needs-you wake from another screen switches to the dashboard, because off the dashboard
    polling is slow.

### Wake sources

- **Wake is level-triggered.** `gpio_wakeup_enable` accepts only levels, so wake uses
  `GPIO_INTR_LOW_LEVEL`.
  - Before any sleep, clear the A164's `REG_INT_STA`, or it re-wakes at once.
  - GPIO50 and GPIO23 are outside the LP IO range (0–15), so the peripheral power domain stays on.
- **Keyboard:** A164 INT on GPIO50.
- **Touch:** GT911 INT on GPIO23. The BSP drives it low at init; reconfigure it as an input with
  pull-up after the display starts, as `~/workspace/generative-tab5` `prov_device.c`
  `touch_interrupt_init` does. While the CPU stays awake in Off, the INT is read as an ordinary
  interrupt.
- **Lift (PT15): removed.** No accelerometer polling and no IMU wake. The BMI270 is only suspended (see Rails).
- **Needs-you:** a watch block that shows a new NEEDS YOU.

### Light sleep and DFS: a spike, expected to fail (builder check)

- **Why it's expected to fail.** The DSI bus holds `ESP_PM_NO_LIGHT_SLEEP` and the DPI panel holds
  `ESP_PM_CPU_FREQ_MAX` until they are deleted. A display that is merely switched off therefore
  allows neither light sleep nor DFS.
- **What the spike tries, once.** Delete and re-create the LVGL display, the DPI panel and the DSI
  bus around Off, with `CONFIG_PM_ENABLE` + tickless idle. It checks:
  - re-creation works ten times in a row, with no leak;
  - wake-to-picture time is under 500 ms;
  - the SSH link and Wi-Fi survive 10 minutes;
  - no crashes or brownouts.
- **Known costs of light sleep:**
  - the USB-Serial-JTAG console doesn't survive light sleep on the P4
    (`SOC_USB_SERIAL_JTAG_SUPPORT_LIGHT_SLEEP` is not set), so light sleep is only allowed on
    battery;
  - chip revisions below v3 can't power the CPU down in light sleep.
- **If it fails** (the expected result), Off ships as:
  - panel off;
  - quiet tasks;
  - Wi-Fi modem power save (`esp_wifi_set_ps(WIFI_PS_MAX_MODEM)` via esp_wifi_remote);
  - the long poll;
  - the Radio-off tier.

  DFS is dropped too, because the panel's lock pins the CPU at maximum frequency. The notes file
  records why.

### Long poll (replaces the fixed-interval poll)

- **The command.** `cc_cmd_watch` builds it from validated parts only (the Claude path plus fixed
  integers), like every command line.
  - sshd runs commands through the user's login shell (`zsh -c`), so the command is
    `exec /bin/sh -c '<loop>'` with fixed loop text.
  - The loop:
    1. runs `<claude> agents --json --all` every `I` seconds;
    2. prints the result between fixed marker lines when it differs from the last one sent;
    3. prints a heartbeat line when nothing has been sent for `H` seconds.
  - `I` is 3 s while Active or Dim and 15 s while Off. The Tab5 restarts the channel when `I`
    changes; the Mac's load matches Plan 1's 3 s poll.
  - `H` is 60 s.
- **The loop must die on its own** *(builder check)*. A Tab5 reboot or a Wi-Fi drop doesn't close
  the channel: macOS sshd has `ClientAliveInterval 0`, and TCP keepalive takes about 2 h. So the
  liveness runs both ways:
  - The Tab5 writes one keepalive byte on the channel's stdin every `K` = 20 s.
  - A reader inside the loop waits on stdin with `read -t 3K`. If no byte has arrived for `3K`
    seconds, or stdin hits EOF, it kills the loop's whole process group, so the loop dies even
    while its output is blocked because the Tab5 vanished mid-write (ruling P28; the first
    design slept in `read -t <I>` and hung in a blocked write on the Wi-Fi drop check).
  - The loop also has a hard maximum lifetime of 30 min; the Tab5 simply restarts the channel.
  - The channel uses a pty, so a clean close also sends SIGHUP.
  - **Test:** check with `ps` on the Mac that no loop survives a clean close, a Wi-Fi drop or a
    Tab5 reboot for more than `3K + I` seconds. Never change sshd config (that needs sudo and the
    user).
- **Liveness on the Tab5 side.** No block or heartbeat for `2H + I` seconds means the link is dead,
  so the Tab5 reconnects.
- **Channels.** Quick answers and take-over keep their own channels on the same SSH session, as in
  Plan 1. Blocks that arrive while a take-over is open are parsed without redrawing the hidden
  dashboard.
- **Fallback.** If the watch can't start, use the Plan 1 fixed poll at the state's interval and
  log why.

### Radio-off tier (Wi-Fi toggling)

- **Entry.** Radio off is entered after `radio_off_min` in Off (default 20 min), on battery only,
  and only when no take-over or send is in flight.
- **Parking the radio:**
  1. close the link;
  2. stop Tailscale traffic;
  3. call `esp_wifi_stop()`, which esp_hosted sends to the C6 as an RPC.
  - `wifi_mgr` gets a **parked** state. In it, the disconnect handler doesn't reconnect and
    `STA_START` doesn't auto-connect. Today it would undo a stop on its own.
  - If the C6 (firmware 0.0.0) refuses stop/start, park with `esp_wifi_disconnect()` plus
    `WIFI_PS_MAX_MODEM` instead, and record it.
  - The C6 stays powered and is not reflashed. Cutting WIFI_EN is out of scope.
- **Check-in** every `checkin_min` (default 5 min):
  1. radio up;
  2. associate;
  3. a **WireGuard handshake to the known Mac peer first**; fall back to `microlink_rebind`
     (the full coordination flow) only if the handshake fails;
  4. SSH connect;
  5. one plain poll;
  6. a new NEEDS YOU wakes the screen dimmed and lights the keyboard; otherwise park again.
  - Measure both paths. A check-in that needs a rebind can take a minute or more, which is too
    long to be worth it.
- **Input during Radio off.** A key or touch goes to Active (PT15) and brings the radio up at once.
  The dashboard shows "Reconnecting…" with stale cards until the first fresh result.
- **Decided by measurement** (see Measurement):
  - (a) average mW in Off with the long poll and modem power save;
  - (b) average mW in Radio off;
  - (c) the energy and duration of one check-in, for each path.
  - Toggling ships on by default only if `(a − b) × checkin_period > c` and average draw drops at
    least 20%, with check-ins under 30 s. Otherwise the setting exists, is off by default, and
    the notes say why.

### Long idle: Deep idle and Hibernate (the user's priority)

After an hour without input, responsiveness matters little and battery matters most. Two more
states follow Radio off.

| State | Enter when (on battery only) | What changes |
|---|---|---|
| Deep idle | no input for `deep_idle_min` (default 60) | Check-ins back off: 5 → 15 → 30 min. The keyboard light is off unless something needs you, then at the dark brightness (PT24). The display is torn down and the CPU runs at its lowest frequency, if the spike shows that works. |
| Hibernate | no input for `hibernate_h` (default 2 h), or battery ≤ 10% | The whole device powers down: a full power-off if the board supports it, else ESP deep sleep. Wi-Fi and the link are dropped. |

- **Waking from Hibernate.** The keyboard INT (GPIO50) and the touch INT (GPIO23) are not LP GPIOs,
  so neither can wake the P4 from deep sleep. Wake comes from:
  - the side power or reset button, the expected answer;
  - an RTC (RX8130) alarm, if its IRQ reaches the power circuit or an LP GPIO (used for the
    Hibernate check-in, PT16).

  There is no IMU wake (PT15).

  The spike finds which of these exist on this board. The screen tells you before it hibernates:
  "Sleeping to save battery. Press the side button to wake."
- **After wake.** The device cold-boots straight to the dashboard. It shows "Waking: reconnecting
  to Mac" until the link is live, measured at 50–130 s today. The cards come back from the first
  poll; nothing stale is shown as live.
- **Needs-you during Hibernate: a periodic check-in, on by default (PT16).** It runs every 15 min
  by default (PT18: user set 15 min, 2026-10-23) (setting: 15 min to 12 h, or Off). A check-in cold-boots, connects and polls, then:
  - **Something needs you:** it lights the keyboard amber (the dark brightness, PT24). If Task 6 Step 4c shows the
    keyboard rail survives deep sleep with the light held, it goes back to Hibernate with that rail
    kept. Otherwise it stays in Deep idle (radio parked, light amber) until input, or until a later
    poll shows nothing needs you, and then hibernates again.
  - **Nothing needs you:** straight back to Hibernate.
  - Cost: each check-in is a cold boot plus a Tailscale connect, about 2.5 W for ~90 s, roughly
    60 mWh. At 15 min that is 96 a day, about 5.8 Wh/day (estimate; Task 10 measures it), roughly 40%
    of a ~15 Wh pack per day. That leans against the max-battery priority, so the bench (Task 10)
    measures it and reports it to the user; the user chose 15 min, so it is not lengthened automatically.
- **Why Hibernate exists.** With the display off, the P4 plus the C6 still draw on the order of
  hundreds of mW, while deep sleep or power-off draws close to nothing. For a night, the difference
  is most of the battery. The bench gives the real numbers, and the defaults (60 min and 2 h) are
  revised from them.
- **The spike (Task 6) gains a hibernate part.** It:
  - finds the wake sources above (no IMU INT probe, PT15);
  - measures deep-sleep or power-off current on battery;
  - measures cold boot to a live dashboard;
  - checks that NVS, the Tailscale identity and the host-key pins survive.

  If none of the wake paths works without the user pressing reset, Hibernate wakes on reset and
  the notes say so.
- **Follow-up, not in this plan:** a faster cold connect, by caching MicroLink's coordination
  state. That would make Hibernate cheaper to leave.

### Rails and the power floor (PT15: IMU wake removed; this section was "IMU-driven deep sleep")

**Where the power actually goes.** These are rough guesses to rank the work. The bench replaces them.

| Part | State | Rough draw | What it means |
|---|---|---|---|
| Backlight | on | 0.5-1.5 W | already off in Off |
| P4 | awake at 360 MHz, display pinning max frequency | 100-250 mW | the biggest cost in Off and Deep idle. Only P4 deep sleep or power-off removes it |
| C6 Wi-Fi | associated, modem power save | 20-80 mW average | Radio off cuts most of it; cutting `WIFI_EN` cuts all of it |
| Panel | display-off but powered | ~10-50 mW | the `LCD_EN` rail can cut it once the display is torn down |
| Keyboard (A164 MCU + light) | powered | ~5-50 mW (light on) | it matters in Hibernate |
| Touch controller | powered, idle | ~1-10 mW | small. Cutting it alone saves little while the P4 is awake |
| BMI270 | suspended (`imu_suspend`, PT15) | ~0.003 mW | negligible |
| Board floor: charger, regulators, IO expanders, INA226, RTC | always on | unknown, maybe 2-20 mW | **This sets Hibernate's battery life.** Measure it first |

**Hibernate cuts every rail it can** (rail cutting stays; PT15):
- `LCD_EN`, `TOUCH_EN`, `USB_EN`, the speaker and the camera;
- **`WIFI_EN`, so the C6 is fully off.** In Hibernate this is safe, because waking is a cold boot
  that re-initialises SDIO anyway. Radio off still doesn't cut it.
- the keyboard's power, if a switch reaches it (see the check-in bullet: a check-in that finds a
  needs-you may need this rail kept);
- the BMI270 is put into suspend (`imu_suspend`, raw I2C, chip ID read first) and stays there in
  Hibernate; there is no IMU wake (PT15);
- the INA226 goes into power-down mode, except during the bench.

The floor that's left is what the bench must measure (`floor`, including `imu_suspend`).

**What must be found first** (in the spike, Task 6). Each is a yes/no on this board:
- Does the board have a real power-off with a wake source (a power latch, or an RTC IRQ into the
  power circuit)? Power-off beats deep sleep.
- Which rails the IO expanders can cut, and what the floor is with everything cut.
- Whether the keyboard rail survives deep sleep with the light held (Task 6 Step 4c, PT16).

Touch stays on in Deep idle (PT15: the "touch off in Deep idle" idea depended on IMU lift wake and
is dropped).

**Follow-up made more important by this:** faster cold connect, by caching MicroLink's coordination
state. Every Hibernate exit pays the 50-130 s Tailscale connect. That stays out of this plan; it is
the first thing to do after it.

### Measurement (one battery session with the user)

- **Why a session is needed.** The INA226 sits on the battery path. On USB it reads charge
  current, not the device's draw, and the USB console doesn't run on battery. So:
  - A **`power log`** feature samples volts, mA and mW, with the power state, once a second
    into a PSRAM ring. It saves a summary per state to NVS. The console dumps it after USB comes
    back.
  - A **`power bench`** command runs a scripted sequence on battery and records each step:
    Active, Dim, Off with touch awake, Off with touch asleep, Off with the keyboard light amber
    or off, Radio off, one check-in per path, and the light-sleep spike if it built.
- **The user's part.** The user unplugs USB once, for about 20 minutes (the timings are in the
  plan), then plugs back in. In the same visit, they put the Mac to sleep and wake it, for the
  Mac-side reconnect, and confirm whether a battery is fitted at all. Gated defaults (light sleep,
  Radio off, the light's off-state brightness) are decided from that log.
- **Before the user's session.** Build every gated feature behind its setting, default off, so
  nothing waits on the measurement.

### Keyboard light

- **Hardware.** The A164 has an RGB light: `REG_BRIGHTNESS` 0x03, `REG_RGB_MODE` 0x11 and
  `REG_RGB_BASE` 0x60. The fork already drives it with `keyboard_set_led(mode, bright, r, g, b)`
  and saves the user's choice in `kbd_led_mode`, `kbd_led_bright` and `kbd_led_rgb`.
- **Rule** (one owner, the power/vitals module, fed by the session list):
  - any live background session NEEDS YOU → steady amber `#f4b545` (mode 1, the user's colour)
    at the user's light brightness, minimum 30%, even if the user set brightness 0, while the screen is
    on; with the screen dark (Off and below) at "Needs-you light when dark", Lowest 2%, Low 5% (default)
    or Medium 15% *(PT24, P122)*;
  - otherwise → the user's own setting, unchanged.
  - Steady, not blinking *(my call)*: amber is a state, not an alarm. (PT24: the user asked for a slow
    pulse if possible; the A164 has no breathe mode, only bind and custom, so it stays steady, P120.) It uses no colour other than
    amber, so the keyboard light never says anything the dashboard doesn't.
- **It holds in every power state.** In Off and Radio off this is the main way to see that
  something needs you. During Radio off it reflects the last check-in, so it can be up to
  `checkin_min` old.
- **It clears** when no session needs you any more. That happens through an answer, an open, the
  session finishing, or the session going away.
- **When the link is down**, the light keeps its last state, but only while the dashboard has a
  result younger than 5 min. After that it goes back to the user's setting, so a stale amber
  doesn't linger for hours.
- **Hot-plug.** A keyboard plugged in later, or re-probed, picks up the current state. Today
  `keyboard_set_led` runs only from probe with the saved settings, so the probe path must ask
  the owner for the current colour.
- **Registers (PT24, from M5Stack's A164 firmware and protocol doc, P120, P121).** 0x03 brightness
  0..100 scales every channel (`c * bright / 100`); 0x11 mode 0 bind (the keyboard's own
  indicators) or 1 custom, nothing else; 0x60..0x67 two LEDs as B, G, R, unused. The fork wrote
  `0,r,g,b`, which swapped the colours; it now writes `b,g,r,0`.
- **Drop the gold (0xFFD700) and orange (0xFF8000) light presets**, so the user's own colour can't
  pass for amber.
- **Setting:** "Keyboard light shows needs-you", on by default.
- **Measure** the light's extra draw at amber with the INA226. If it matters on battery in Off, cap
  its brightness there and record the numbers.

### Reconnect

- **`wifi_mgr`:**
  - background reconnect retries with a backoff (1, 2, 4 … 30 s, without cap on attempts) from a
    task, never `vTaskDelay` inside the event handler;
  - fixes the case where one failed background attempt stopped retrying for good.
- **On wake from a sleep long enough to drop the link** (or on any detected Wi-Fi drop):
  - `cc_link_on_wifi(false/true)`;
  - Tailscale `microlink_rebind`;
  - SNTP restart.
- **Mac sleep/wake:** measure the reconnect time with the Mac put to sleep by the user. That needs
  the user, so batch it into the one end-of-plan question if it can't be simulated. Fix the
  stale-peer-up shortcut (main spec known issues) if the measurement shows a long wait: on a failed
  connect, kick a handshake before the next retry.

### Vitals bar (dashboard header)

- Left to right: battery glyph + % (bolt when charging; red ≤ 15% when not charging), Wi-Fi bars
  (4 levels from RSSI), Tailscale state, the Mac link dot (green/red, as the dashboard has today),
  and the clock.
- D9 colors only. Glyphs come from the existing icon fonts or new ones generated with the fork's
  font tools (`gen_fonts.sh`).
- The full-screen terminal keeps no bar. Its existing status text stays.
- **Battery % estimate:**
  - replace the linear 6.4–8.35 V map with a 2S Li-ion discharge curve;
  - smooth it (EMA over about 30 s);
  - compensate for load with the INA226 current reading;
  - verify the charge-status pin (IO expander 0x44, pin 4) on hardware by plugging and unplugging
    USB, which needs the user; otherwise infer charging from the current direction.
- A `power` console command prints volts, mA, mW, %, charging, source and state, for tests and
  measurement.

### Settings (Settings → Power, new section)

- Dim after (s);
- Screen off after (s);
- Wake when a session needs you;
- Keyboard light shows needs-you;
- Radio off after (min) and check-in every (min);
- Deep idle after (min), Hibernate after (h), Hibernate check-in every (min, default 15; 15 min-12 h or Off) (PT16, PT18);
- (PT15: "Wake on lift", "Hibernate when still for" and "Wake on pick-up (IMU)" rows removed.)
- Sleep on USB power;
- Time zone (a short list of POSIX rules, with US Pacific as default);
- Forget Mac host key (under Companion).

### Hotkeys for two thumbs (the user, 2026-10-23)

**The problem.** The Tab5 is held in both hands and typed with two thumbs. A shortcut whose
modifier and key are both on the left half needs a hand to reach across. Ctrl sits bottom-left, and
Ctrl+Q (leave a session) and Ctrl+Tab (buttons mode) both put the key there too.

**The rule.** Each shortcut pairs the left-hand Ctrl with a key on the right half (Y U I O P,
H J K L, N M, the arrows, `]`, Enter, Backspace), or is a single key. Inside a take-over, only keys
Claude Code doesn't use are taken.

| Where | Action | New | Old (kept as an alias) |
|---|---|---|---|
| Take-over | Leave the session (it keeps running) | **Ctrl+]** (the telnet/ssh escape key; Claude Code doesn't use it) | Ctrl+Q |
| Take-over | Scroll back | Shift+▲/▼ *(my call)* | Shift+PgUp/PgDn |
| Dashboard | Open the selected session | Ctrl+O (unchanged; O is on the right) | |
| Dashboard | Move between cards | ▲/▼ (right hand, unchanged) | |
| Dashboard | Send the answer | Enter (unchanged) | |
| Dashboard | Settings | **Ctrl+P** | Ctrl+Tab buttons mode |
| Dashboard | More | **Ctrl+M** | Ctrl+Tab buttons mode |
| Dashboard | Show the shortcuts | **Ctrl+K**: an overlay listing every shortcut; any key closes it | |
| Settings, More | Back | Esc (unchanged) | |

**Supporting changes:**
- Ctrl+Tab keeps working everywhere it does today.
- The dashboard footer hint becomes "Ctrl+O open · Ctrl+P settings · Ctrl+K keys". A take-over
  shows a toast for 3 s when it opens: "Ctrl+] to leave".
- Before taking Ctrl+P, Ctrl+M and Ctrl+K on the dashboard, check that they never reach a card's
  answer field. They are local, like Ctrl+O.
- Verify on hardware with real presses, in the user's visit, that the A164 reports `]` with Ctrl
  held as `cp=']'` with `MOD_CTRL`, in both layouts the fork supports. If it doesn't, use Ctrl+P to
  leave a take-over instead, and say so in the hint.

## Left to the builder

- Task breakdown and order. The feasibility spike goes early because later tasks branch on it.
- Exact glyph art, the discharge-curve table and the EMA constant.
- Whether the BMI270 driver comes from generative-tab5 (after a license check) or a minimal
  register driver.
- How power state reaches the dashboard: subscriber callback or polled getter. No cc_link calls
  from callbacks, same rule as Plan 1.

## Things only the user can do (one visit, when the plan reaches Measurement)

1. Confirm that a battery is fitted.
2. Unplug USB for about 20 minutes while `power bench` runs, then plug back in. This also checks
   the charge pin.
3. Put the Mac to sleep and wake it, for the Mac-side reconnect measurement.

Everything else runs without the user, and every gated feature is built before that visit.
