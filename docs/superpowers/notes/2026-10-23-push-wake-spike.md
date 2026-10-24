# Plan 3 Task 4: push-wake spike (paper, Steps 1–2)

**Question.** Can the Tab5 learn at once that a session needs input, without the 15-minute check-in? The idea is to
keep the C6 powered and associated in Hibernate, let the P4 deep-sleep, and have the C6 raise a GPIO when a knock
packet arrives.

**Verdict: not feasible.** The board is wired for it, but the stock C6 firmware has no host-wakeup feature. Its
only Wi-Fi power save is modem sleep, at about 27 mA, which costs about 3× today's check-in average. Making it work
needs a C6 reflash, and that is forbidden. Step 3 (the prototype) does not run. Nothing touched the device or
the product code.

## Step 1: the answers

### 1. Does esp_hosted support host power save with a wakeup GPIO?

**Yes, on the host side (3.0.10). No, on this board's C6.**

- **The host side (vendored 3.0.10).** This is `ESP_HOSTED_HOST_FEAT_POWER_SAVE` →
  `…_DEEP_SLEEP_ALLOWED` → `…_WAKEUP_GPIO`, with the level High by default
  (`host/features/eh_host_feat_power_save/Kconfig.ext`).
  - Its defaults already name the Tab5: under `ESP32P4_TAB5_C6_BOARD`, which our sdkconfig sets, the range min, the
    range max and the default are all **14**. Only deep sleep is accepted. `eh_host_power_save_start()` rejects
    light sleep.
  - The flow is in `docs/features/power-save.md`:
    1. The host tells the CP "PS enter".
    2. The CP acks, unloads its bus and holds the reset GPIO.
    3. The host arms ext1 and deep-sleeps.
    4. On a wake event, the CP's `wakeup_host_mandate()` toggles the line until the host re-inits the bus.
  - The feature is advertised as `ESP_EXT_CAP_HOST_PS` (bit 10 of `ext_cap`). The host logs it at boot as
    `Extended Features supported: * Host power-save` (`eh_host_mcu_transport_init_event.c`).
- **The CP side (3.0.10 sources).** This is `ESP_HOSTED_CP_FEAT_HOST_PS`, **default n**
  (`coprocessor/features/eh_cp_feat_host_ps/Kconfig.ext`). Its Tab5 default is **"Slave out" GPIO 2**. The feature
  is compiled into the C6 image, so it exists only if the C6 firmware was built with it.
- **The stock C6 firmware does not have it.**
  - M5Stack's factory C6 image is `platforms/tab5/wifi_c6_fw/ESP32C6-WiFi-SDIO-Interface-V1.4.1-96bea3a_0x0.bin` in
    github.com/m5stack/M5Tab5-UserDemo (flashed at 0x0 by its `flash.sh`; restore guide:
    docs.m5stack.com/en/guide/tab5/c6_wifi_restore).
  - Its strings identify it:
    - project `fg_mcu_slave`;
    - `ESP-Hosted-MCU Slave FW version :: %d.%d.%d`;
    - the old slave layout (`./main/app_main.c`, `sdio_slave_api.c`, `slave_control.c`, `slave_bt.c`, `stats.c`);
    - IDF `v5.5-dev-407-gf420609c332-dirty`;
    - built from `/Users/imliubo/.../esp-hosted-mcu` (M5Stack's engineer).
  - It contains **none** of these strings: `host_power_save`, `power_save`, `wakeup_host`, `Host wakeup`,
    `host_ps`, `network_split`, `esp_pm`, `esp_sleep_enable`, `gpio_hold`.
  - Its only power-save path is the Wi-Fi RPC `req_wifi_set_ps` (`RPC__WIFI_POWER_SAVE__MIN_MODEM`/`MAX_MODEM`),
    which is plain modem sleep.
  - It has no `esp_pm` either, so the C6 can never auto-light-sleep.
- **The "FW 0.0.0" this board reports is consistent with that image.** The 3.0.10 host reads the CP version over
  RPC V2 (`eh_host_feat_system_v2_resp_evt.c`), and this old slave speaks the V1 proto
  (`esp_hosted_rpc.pb-c.c`), so the host gets zeros. That is an inference.
  - Not checked on this unit: whether it still runs exactly V1.4.1 (the device is busy).
  - The confirming check costs nothing: in any captured boot log, `eh_init_evt` either lists `* Host power-save`
    or doesn't. I expect it doesn't.

### 2. Which C6 GPIO goes to which P4 pin?

From the Tab5 schematic (`Tab5_Schematics_PDF.pdf`, docs.m5stack.com/en/core/Tab5, page 1; U1 is the P4 and U2 the
ESP32-C6-MINI-1U), read from the rendered page:

| C6 (U2) | Net | P4 (U1) |
|---|---|---|
| IO18 (pin 24) | SDIO2_CMD | pin 14, GPIO13 |
| IO19 (pin 25) | SDIO2_CLK | pin 13, GPIO12 |
| IO20 (pin 26) | SDIO2_D0 | pin 12, GPIO11 |
| IO21 (pin 27) | SDIO2_D1 | pin 11, GPIO10 |
| IO22 (pin 28) | SDIO2_D2 | pin 10, GPIO9 |
| IO23 (pin 29) | SDIO2_D3 | pin 8, GPIO8 |
| **IO2 (pin 5)** | **RF_C6_IO2** | **pin 15, GPIO14** |
| EN (pin 8) | RF_C6_RST ← SOC_EXTRF_RST (through R4) | pin 16, GPIO15 (`CONFIG_ESP_HOSTED_HOST_RESET_GPIO=15`) |
| TXD0, RXD0, IO9 | RF_C6_TXD/RXD/IO9 | C6_ISP header J1 only (no P4 pin) |
| IO8, IO9 | strap pull-ups | – |

The SDIO lines carry 22 Ω series resistors (R14, R15) and 5.1 kΩ pull-ups (R17, R18). EN has the usual RC (R6 10 kΩ,
C32 1 µF). **Besides SDIO and EN, exactly one C6 pin reaches the P4: C6 IO2 → P4 GPIO14.** That matches esp_hosted's
Tab5 defaults (CP "Slave out" 2, host "Host in" 14). Espressif clearly wired the preset to this net. The BSP header
(`m5stack_tab5.h`) names none of these pins.

### 3. Is that P4 pin an LP GPIO that ext1 can wake on?

**Yes.**
- P4 GPIO0–15 are the LP IOs. The schematic groups pins 1–16 under `VDDPST_1 [LP_IO]`.
- esp_hosted's P4 range for the wake pin is ≤ 15, and it sets 14..14 for the Tab5.
- esp_hosted arms it with `ESP_EXT1_WAKEUP_ANY_LOW/HIGH` on post-ESP32 targets (CHANGELOG 3.0.4).

So the hardware passes. One caveat from the light-sleep spike: on this rev < 3 P4, every deep-sleep wake arrives as
an HP-watchdog reset with no wakeup cause. A GPIO wake would have to be inferred by reading GPIO14's level early in
boot.

### 4. The C6's documented associated-sleep current

ESP-IDF *Low Power Mode in Wi-Fi Scenarios*, ESP32-C6
(docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/low-power-mode/low-power-mode-wifi.html).
These are average currents, measured in a shielded box. The supply voltage is not stated; I assume 3.3 V.

| Mode | DTIM 1 | DTIM 3 | DTIM 10 | Needs on the C6 |
|---|---|---|---|---|
| Modem sleep (160 MHz, no DFS) | 27.48 mA | 27.08 mA | 26.85 mA | `esp_wifi_set_ps`: **stock firmware has this** |
| Modem sleep + DFS | 14.37 mA | 13.90 mA | 13.71 mA | `CONFIG_PM_ENABLE`: reflash |
| Auto light sleep | 0.919 mA | 0.368 mA | 0.172 mA | `CONFIG_PM_ENABLE` + light sleep: reflash |
| Deep sleep (not associated) | 6.7 µA | | | – |

- The ESP32-C6 datasheet (documentation.espressif.com/esp32-c6_datasheet_en.pdf) gives the chip-level mode tables.
  The DTIM figures above are the ones Espressif publishes per scenario.
- Modem sleep is nearly flat across DTIM: the CPU at 160 MHz dominates, not the radio. So a longer DTIM buys
  almost nothing on the stock firmware.

## Step 2: the decision

The wake line passes: GPIO14 is an LP GPIO and ext1 can use it. **Host power save needs something this board
lacks.** Two pieces are missing, and both live in the C6 image:
- the CP host-PS feature (`ESP_EXT_CAP_HOST_PS`, which drives IO2);
- a C6 power mode cheap enough to beat polling (auto light sleep).

Both mean a reflash, which is forbidden. **Recorded: not feasible. Stop: Step 3 is not run.**

**A no-reflash workaround, considered and rejected.**
- **The idea.** Wake on SDIO D1 (P4 GPIO10, also an LP GPIO), the line the SDIO slave uses to raise its interrupt
  to the host when it has a packet.
- **Rejected for three reasons:**
  1. In 4-bit mode the card signals that interrupt only in the bus's interrupt period. That needs the host clock,
     and a deep-sleeping P4 isn't driving it. Asynchronous interrupts are an optional SDIO 3.0 feature, and I found
     no documentation of them on the C6.
  2. The stock slave doesn't know the host is asleep, so it would queue, and "wake" on, every broadcast and ARP.
  3. On wake, our host config resets the C6 (`CP_RESET_STRATEGY_ALWAYS`), which drops the association anyway.
- **It also fails on power.** It would still pay modem sleep's 27 mA.

## Standby power vs the check-in

Assumptions:
- The C6 runs from WLAN_3.3V (SOC_3.3V through a load switch).
- 3.3 V at the chip, and about 90 % conversion from the 2S pack, so battery mW ≈ 3.3 × mA / 0.9.
- The P4 deep-sleep floor is the same in both schemes, so it cancels out.
- The check-in average is 32 mW (8 mWh per 15 min).

| C6 mode while the P4 sleeps | DTIM 1 | DTIM 3 | DTIM 10 | vs 32 mW |
|---|---|---|---|---|
| Modem sleep (**stock**) | 101 mW | 99 mW | 98 mW | **3.1× worse** at every DTIM |
| Modem sleep + DFS (reflash) | 53 mW | 51 mW | 50 mW | 1.6× worse |
| Auto light sleep (reflash) | 3.4 mW | 1.3 mW | 0.6 mW | 9–50× better |

**Break-even.**
- 32 mW at the battery is about **8.7 mA** at the C6.
- With the stock firmware, **no DTIM breaks even.** Modem sleep bottoms out near 26.8 mA.
- With auto light sleep, **every DTIM from 1 up breaks even** with a wide margin. At DTIM 1 it is about 10× under.
- Even if Task 3 cuts the check-in to about 8 mW, auto light sleep at DTIM 3 still wins. Modem sleep never does.
- These are shielded-box figures. A real LAN's broadcast and multicast traffic raises auto-light-sleep current, but
  not by the 3–9× it would take to lose.

## If it ever became feasible (record only; needs the reflash ruling reversed)

What a future plan would need:
- **The C6.** Build esp_hosted 3.0.10's slave with:
  - `ESP32P4_TAB5_C6_BOARD` (IO2 preset);
  - `ESP_HOSTED_CP_FEAT_HOST_PS`;
  - `CONFIG_PM_ENABLE` + auto light sleep;
  - `…_IGNORE_NON_UNICAST`;
  - Network Split, so the C6 can filter for the knock port and keep its own IP stack and DHCP lease while the P4
    sleeps.
- **The P4.**
  - `ESP_HOSTED_HOST_FEAT_POWER_SAVE` with GPIO14.
  - `CP_RESET_STRATEGY_ONLY_IF_NECESSARY`, so a wake doesn't reset the C6.
  - WIFI_EN kept on in Hibernate.
  - GPIO15 (C6 EN) held high through the sleep (the library's `hold_slave_reset_gpio_pre_power_save`).
- **The host-side sender.** It conflicts with "nothing extra runs on the host", so the user decides it.
  - A small notifier: a Claude Code `Notification`/`Stop` hook, or a launchd agent watching session state.
  - When a session enters NEEDS YOU, it sends one UDP datagram, the knock, to the Tab5's LAN IP on a fixed port.
    It repeats 3× at 1 s intervals to cover a missed beacon.
  - The knock carries no data. The Tab5 then does its normal authenticated SSH poll.
- **Security.** Anything on the LAN can send a knock.
  - **Access is not at risk.** A knock only makes the Tab5 boot and run its normal authenticated poll: it carries
    no data or commands, and the SSH host-key check stays the trust boundary.
  - **The risk is battery drain.** Each wake costs about 8 mWh, so an attacker could flood it.
  - **Proposed rate limit, on the C6:** at most one wake per 60 s. After 10 wakes in an hour that found nothing,
    back off to one per 15 min, and reset on a poll that finds news.
  - Optionally, drop knocks that don't come from the host's IP, or that don't carry a 16-byte MAC over a counter
    (HMAC with a key shared at pairing). The MAC stops spoofed drain, but not a flood of unauthenticated packets,
    which the rate limit caps.
  - Keep the 15-min (or longer) check-in as a backstop for lost knocks.

## Recommendation

- **Drop push-wake for this hardware as configured.**
  - The wake wiring is ideal: C6 IO2 → P4 GPIO14, an LP GPIO, preset in esp_hosted.
  - But the stock C6 can neither drive it nor sleep cheaply. Kept associated, it would cost about 100 mW against
    32 mW for polling.
- **Keep the check-in, and put the effort into Task 3's shrink.**
- **Reopen only if the user lifts the no-reflash rule.** Then push-wake beats polling at any DTIM: about 1 mW at
  DTIM 3, with instant amber. It would need the C6 build above, a recovery path through the C6_ISP header, and the
  user's call on the host notifier.
- **Zero-cost confirmation, next time a boot log is captured:** check for the absence of `* Host power-save` under
  `eh_init_evt`'s extended features.

## Sources

- `firmware/managed_components/espressif__esp_hosted` (3.0.10): `docs/features/power-save.md`,
  `host/features/eh_host_feat_power_save/Kconfig.ext`, `coprocessor/features/eh_cp_feat_host_ps/Kconfig.ext`,
  `host/Kconfig.ext` (the board choice), `common/eh_common/include/eh_common_caps.h`,
  `host/mcu/eh_host_mcu_transport/src/eh_host_mcu_transport_init_event.c`, `CHANGELOG.md`.
- `firmware/sdkconfig`: `CONFIG_ESP32P4_TAB5_C6_BOARD=y`, `HOST_RESET_GPIO=15`, SDIO on GPIO8–13,
  `CP_RESET_STRATEGY_ALWAYS`.
- The Tab5 schematic: https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/Tab5_Schematics_PDF.pdf (page 1, U1/U2/J1),
  linked from https://docs.m5stack.com/en/core/Tab5.
- The factory C6 firmware: https://github.com/m5stack/M5Tab5-UserDemo/tree/main/platforms/tab5/wifi_c6_fw
  (`ESP32C6-WiFi-SDIO-Interface-V1.4.1-96bea3a_0x0.bin`, string scan). M5Stack's restore guide:
  https://docs.m5stack.com/en/guide/tab5/c6_wifi_restore.
- The C6 currents:
  https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/low-power-mode/low-power-mode-wifi.html.
  The ESP32-C6 datasheet: https://documentation.espressif.com/esp32-c6_datasheet_en.pdf.
- `docs/superpowers/notes/2026-10-23-light-sleep-spike.md` (the rail map, WIFI_EN, the deep-sleep wake reset) and
  `docs/superpowers/specs/2026-10-22-claude-tab5-design.md` (C6 FW 0.0.0, "don't update the C6").
