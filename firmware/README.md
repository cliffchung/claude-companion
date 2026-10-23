# Tab5 firmware (M2)

Native app for the **M5Stack Tab5**: the dashboard from `web/`, rendered with LVGL,
driven by the physical keyboard, with camera / mic / IMU wired in.

Not started yet. This is the plan.

## Target

- **SoC:** ESP32-P4 (RISC-V, 400 MHz, 32 MB PSRAM). Wi-Fi/BLE come from the on-board
  ESP32-C6 over esp-hosted (SDIO). Normal sockets work; nothing exotic needed.
- **Toolchain:** ESP-IDF 5.5, target `esp32p4`. Arduino on the P4 is still beta; avoid it.
- **Board support:** the `espp/m5stack-tab5` component (display, touch, audio, camera, IMU, power).
- **Board revision:** prior art is only tested on Tab5 board v3. Check yours before flashing.

## Two transport modes (same UI)

| Mode | How | Library |
|---|---|---|
| **SSH-direct** (default) | Persistent SSH channel to the host; run `claude agents --json --all` on a timer, `claude -p --resume <id> "…"` to reply | `skuodi/libssh2_esp` |
| **Bridge** | SSE from `/events`, POST to `/api/reply` and `/api/uploads/*` | `esp_http_client` |

See [`../docs/PROTOCOL.md`](../docs/PROTOCOL.md) for the bridge API.

## Inputs and sensors

| Part | Use | Notes |
|---|---|---|
| A164 keyboard | Navigate, quick reply, type | I²C addr `0x6D`; SDA G0, SCL G1, INT G50 (per M5Stack docs). Use Normal mode (row/col + modifiers), not HID mode |
| GT911 touch | Tap cards, chips | |
| SC2356 camera | "Show Claude this" | Capture JPEG → `scp` (SSH-direct) or `/api/uploads/image` |
| Dual mics (I²S) | Voice reply | Record WAV → host speech-to-text → reply |
| Speaker | Read replies aloud | TTS on host, stream back |
| IMU | Lift-to-wake, double-tap to ack | Wake source for light sleep |
| PMIC | Battery %, charging | Shown in the vitals bar |

## Power

Dim the backlight after inactivity, then light-sleep the P4. Wake on keyboard INT, touch,
or IMU motion. While asleep, poll the host on a slow timer and wake the screen when a session
flips to *waiting*.

## Prior art worth reading

- [Clientre5](https://github.com/Mozgi512/Clientre5): SSH terminal OS for the Tab5, with Tailscale (MicroLink)
- [Tab5 Portable Terminal](https://www.hackster.io/ciniml/tab5-portable-terminal-112cdc): VT100 terminal good enough for vim/htop
- [esp32p4-c6-wifi-test](https://github.com/r4d10n/esp32p4-c6-wifi-test): P4 + C6 over esp-hosted, with throughput numbers
