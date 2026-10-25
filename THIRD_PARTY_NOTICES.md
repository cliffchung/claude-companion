# Third-party notices

Claude Companion's own code is MIT ([LICENSE](LICENSE)). The firmware is built from, or
together with, the third-party software below. Each licence was checked against the licence
file in this repository, or in the component the ESP-IDF component manager downloads at build
time (versions pinned in [firmware/dependencies.lock](firmware/dependencies.lock)).

## In this repository

| Component | Where | Licence | Licence text |
|---|---|---|---|
| Clientre 5 by Mozgi512 (the fork base) | `firmware/` | MIT | [firmware/LICENSE.clientre5](firmware/LICENSE.clientre5) |
| libssh 0.11 (ESP-IDF port from ewpa/LibSSH-ESP32) | `firmware/components/libssh` | LGPL-2.1; the full source is included | [COPYING](firmware/components/libssh/COPYING) |
| MicroLink, an unofficial Tailscale client | `firmware/components/microlink` | MIT | [LICENSE](firmware/components/microlink/LICENSE) |
| wireguard-lwip | `firmware/components/wireguard_lwip` | BSD-3-Clause | [LICENSE](firmware/components/wireguard_lwip/LICENSE) |
| Crypto code inside wireguard-lwip (x25519, ChaCha20, Poly1305) | `firmware/components/wireguard_lwip/src/crypto` | public domain, CC0 or MIT, as marked in each file | [x25519-license.txt](firmware/components/wireguard_lwip/src/crypto/refc/x25519-license.txt) and the file headers |

libssh is linked statically into the firmware image. Its complete source, including the ESP-IDF
port changes, is in `firmware/components/libssh`, so you can change it and rebuild the image with
`idf.py build` (LGPL-2.1, section 6).

## Downloaded at build time (not in this repository)

| Component | Licence |
|---|---|
| ESP-IDF v5.5 | Apache-2.0 |
| LVGL 9 (`lvgl/lvgl`) | MIT |
| Espressif components: `m5stack_tab5` (board support), `esp_hosted`, `esp_wifi_remote`, `esp_lvgl_port`, the LCD, touch, I/O-expander, codec, camera-sensor, USB, `bmi270`, `i2c_bus`, `sensor_hub` and other helper components | Apache-2.0 |
| Espressif `esp_video` and `esp_ipa` | ESPRESSIF MIT License (MIT terms, for use on Espressif chips) |

## Fonts (compiled into `firmware/main/fonts`)

| Font | Author | Licence |
|---|---|---|
| x12y12pxMaruMinya (the UI and terminal faces) | hicc | Free font from its author. The terms aren't in this repository, so they weren't checked here: read them at the author's site before you redistribute. The TTF itself isn't committed. |
| Noto Sans Mono CJK JP (the CJK fallback) | Google / Adobe | SIL Open Font License 1.1 |
| DejaVu Sans Mono (glyphs Noto lacks) | DejaVu fonts team | Bitstream Vera / Arev font licence (permissive), public-domain changes |

The 16 px icon font is generated from bit masks in `firmware/tools/gen_ui_icons.py` and is part
of this project.
