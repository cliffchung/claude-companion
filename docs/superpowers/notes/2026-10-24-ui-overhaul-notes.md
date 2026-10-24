# Plan 4 (UI overhaul): measurements and rulings

**Device:** M5Stack Tab5 on USB (`/dev/cu.usbmodem1101`, ruling P4C-1). **Host:** `my-host.local`, 192.168.1.50, LAN Host entry, Tailscale off.
Each task appends its section. Screenshots and raw size reports stay in scratch storage, not in git.

## Task 1: measurement verbs, baseline, Music / Camera / USB MSC removed

### The verbs (every later gate uses them)

- `ui heapmin start` starts `heap_caps_monitor_local_minimum_free_size_start()`. `ui heapmin stop` prints
  `heapmin: internal min <N> B, largest <L> B, now <F> B`. N is `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)`,
  read before the monitor stops. It takes no LVGL lock. A second `start` prints `heapmin: already started`, and a `stop`
  with no `start` prints `heapmin: not started`.
- `ui timers` takes `ui_lock()`, walks `lv_timer_get_next`, and prints `timer <period_ms> ms, last run <age_ms> ms ago, paused=<0|1>`
  per timer, then `timers: <n>, under 300 ms: <k>`. Period and last run come from `src/misc/lv_timer_private.h`, the same
  private-header route `ui_core.c` already takes. LVGL 9.6 has no public getter for them.
- `key ctrl space` posts `KEY_EV_CHAR cp=' '` with `MOD_CTRL`. `key log on` shows it as `type=char cp=' ' key=none mods=ctrl`.

**How to read `ui timers`.** With the screen off, every timer reads `paused=1`, because power.c pauses them in Off (power.c:584).
Read it with the screen on. The 2-minute idle step below ends with the screen off (dim 30 s, off 120 s under `power set defaults`).

**Driving Settings › Network without a tap.** `ui settings`, then `key down` three times and `key enter`. The first arrow
focuses the back button, and the next two reach System and then Network. If the screen has dimmed, the first key only wakes it
(rule 7), so send the keys within 30 s of the last input. Don't wake the screen with `power sim touch` while it is on: that
press lands on the dashboard at (639, y).

### Baseline (Plan 3 build)

This is the Plan 3 code at 8b94efb plus only the three verbs. Tailscale is off, the Host link is on the LAN, and the device is on USB.

| Item | Baseline |
|---|---|
| `idf.py size`, total image | 8,317,249 B |
| DIRAM used | 214,126 B (`.text` 122,390, `.bss` 58,656, `.data` 33,080) |
| `.iram0.text` / `.dram0.data` / `.dram0.bss` / `.dram1.bss` | 122,390 / 33,080 / 9,688 / 48,968 B |
| `ts` after 2 min idle on the dashboard, 3 `companion-test-*` sessions (screen had gone off) | free 123,495 B, min 34,156 B, largest 34,816 B |
| `ts` on the dashboard with the screen on (link up) | free 122,607 to 122,683 B, largest 32,768 B |
| `ui heapmin` over opening Settings › Network | min 101,171 B, largest 34,816 B, now 134,919 B |
| `ui heapmin` over a take-over (`companion takeover`, 60 s, `key ctrl ]`) | min 64,115 B, largest 47,104 B, now 115,971 B |
| `ui timers`, dashboard, screen on | `timers: 8, under 300 ms: 6` (300, 500, 60, 10, and four 16 ms timers, three of them paused) |
| Idle power on USB, 5 min on the dashboard, no input (`power_report.py --log --include-charging`) | 609 samples: tag 0x00 n=6, 8 mW; 0x01 n=90, 9 mW, 0.23 mWh; 0x02 n=513, 9 mW, 1.26 mWh; estimate 1668.2 h. `src=usb-standby`, so the INA226 reads about 1 mA, and the figure says little about battery draw |
| Last check-in awake (`power checkins`) | `hib-lan` 6,354 ms, 1.461 mWh (10-24 01:57:54, the Plan 3 build before the verbs; 6,223 to 8,372 ms across the last 8) |
| Boot (C21) | `boot complete in 2791 ms`; `hib: boot to live: 7989 ms` (kind 0) |

### After the removals (this commit)

| Item | After | Delta |
|---|---|---|
| Total image | 7,439,121 B | −878,128 B (−0.84 MiB) |
| DIRAM used | 199,582 B (`.text` 115,894, `.bss` 55,568, `.data` 28,120) | −14,544 B static internal |
| `.iram0.text` / `.dram0.data` / `.dram0.bss` / `.dram1.bss` | 115,894 / 28,120 / 29,744 / 25,824 B | the linker moved `.bss` between dram0 and dram1; the sum is −3,088 B |
| `ts` after 2 min idle on the dashboard, 3 sessions (screen off) | free 144,395 B, largest 47,104 B | **+20,900 B** |
| `ts` on the dashboard with the screen on | free 143,807 B, largest 47,104 B | +21,124 B or more |
| `ui heapmin` over Settings › Network | min 121,979 B, largest 47,104 B, now 157,407 B | +20,808 B |
| `ui heapmin` over a take-over | min 57,763, 63,699 and 59,771 B (three runs), largest 47,104 B | −352 to −6,352 B against one baseline run (see below) |
| `ui timers`, dashboard, screen on | `timers: 8, under 300 ms: 6` | none |
| Boot | `boot complete in 2792 ms`; boot to live 7,898 and 7,519 ms | about the same |

**Against §7.2.** These three removals' share of §7.2 is Music + codec 1.0, camera 0.5 (+4.4 if the pipeline unlinks),
MSC 5.6, plus the MSC driver task's 4.4. That makes 11.5 KB, or 15.9 KB with the camera pipeline. The camera pipeline did
unlink (below), so the expected figure is 15.9 KB. The measured steady-state gain is about 20.9 KB, about 5 KB more.
The likely source is a second task that §7.2 doesn't count: `usb_mgr.c`'s own `usb_mgr` polling task (4 KB stack plus its
TCB), which existed only to mount and unmount the flash drive. It goes together with the MSC driver's background task.

**Unlinked (`idf.py size-components`).** These are gone from the link:
- `libespressif__esp_video.a` (43.4 KB), `libesp_driver_isp.a` (25.8 KB) and `libesp_driver_cam.a` (14.7 KB): `esp_video`, ISP and CAM unlinked;
- `libesp_audio_codec.a` (580 KB) and `libesp_audio_simple_dec.a` (24.8 KB);
- `libespressif__tinyusb.a` (24.2 KB, 5.3 KB of it internal), `libespressif__esp_tinyusb.a` (13.1 KB) and `libespressif__usb_host_msc.a` (9.6 KB).

`libespressif__esp_codec_dev.a` (13.2 KB) also left the link, because nothing calls it now. The BSP still depends on it,
so the mic work can use it again. `managed_components` no longer holds `espressif__esp_audio_codec`, `espressif__usb_host_msc`
or `espressif__esp_tinyusb`.

**Take-over minimum (open).** The steady-state and Settings minimums rose by about 21 KB, but the take-over minimum didn't
(57.8 to 63.7 KB after, against one 64.1 KB baseline run). The trough during a take-over isn't set by the free total alone,
and one baseline sample can't show the spread. All runs clear B2's 40,960 B. T13's final comparison should take three
take-over runs on each side.

**USB keyboard.** PENDING for the user's visit. No USB keyboard is attached: the boot log has no `USB keyboard connected`.
The HID host is unchanged and still installs at boot.

### Rulings and deviations (Task 1)

- **C1.** `usb_mgr.h` keeps `usb_mgr_status_text()`, now a constant `"USB host"`, for `ui_devinfo.c` until T3 deletes that
  screen. `ui_files.c` loses its `USB_MOUNT` and `usb_mgr_msc_mounted` branches and its audio branch, so it browses the TF
  card only until T2. `USB_MOUNT` is gone from `storage/sd.h`.
- **C11.** The preview CMake drops `ui_camera.c`.
- **Preview link fix (found here).** The preview didn't link at the base 8b94efb. Plan 3's `ui_settings.c` calls `sip_parse`,
  `wifi_mgr_static_ip_get/set` and `link_down_restart`, and the preview had none of them. The preview now compiles
  `net/fast_rules.c`, a pure-C file, and stubs the other three. The `home` case's tile assert drops from 9 to 6.
- **Left for later tasks.** `sd_unmount_keep_card()` and `sd_remount_after_msc()` in `storage/sd.c` have no callers now.
  T11 owns `sd.c`. The unused `HOME_MUSIC`, `HOME_CAMERA`, `HOME_USB`, `USB_*` and `PLAY`/`PAUSE` strings stay in `i18n.h`
  for T3 and T12. The More launcher still reserves its 56 px bottom strip, which the mini player used to fill. T3 deletes the launcher.

## Task 2: Files, the viewers, the SD screenshot and Backup

**Removed.** `ui_files.c`, `ui_image.c`, `ui_text.c`, `ui_backup.c`, `storage/backup.*`, `media/` (image_loader, exif, tjpgd),
the three-finger SD screenshot (`screenshot_init`, `screenshot_take`, `write_bmp`, the 60 ms poll), `wifi_mgr_export_json` /
`wifi_mgr_import_json`, the Files tile, the Backup category (rail is 8 entries), the "three-finger tap" note in Display, 44 `STR_*`
entries, and LVGL's LODEPNG, BMP, TJPGD, GIF and FS_STDIO (`CONFIG_LV_CACHE_DEF_SIZE=0`). The `screenshot` console verb
(`screenshot_dump_usb`) stays. `ui_files.c`'s stale header comment and its audio-to-text-viewer routing went with the file.

**Ruling C2 applied.** `server_store_export_json` / `server_store_import_json` stay: `save()`, `server_store_init()` and
`set_on_sd` in `server_store.c` persist the host list through them, and T11's `server_store_import_sd_once()` needs the parser.
Only the Backup callers are gone, so the plan's Step 1 grep still finds those two names (expected). `ui_preview/CMakeLists.txt`
(C11) listed none of the removed sources, so it needed no edit. The preview's tile assert drops 6 to 5, and the settings loops go from 9 to 8.

**Measurements** (Task 1 values from the table above).

| Item | Task 1 | Task 2 | Delta |
|---|---|---|---|
| Total image | 7,439,121 B | 7,341,993 B | −97,128 B (−975,256 B against the Plan 3 baseline) |
| `ts` free internal, dashboard, about 1 to 2 min after boot (no sessions on screen) | 144,395 B (after 2 min idle) | 160,899 B | +16,504 B |
| `ui timers` | 8 timers, 6 under 300 ms | 6 timers, 4 under 300 ms (300, 500, 10 ms and the pty pump set); no 60 ms | the 60 ms timer is gone |
| `screenshot` | `@@SHOT jpeg` | `@@SHOT jpeg` | same |

The measured gain (+16.5 KB) is well over the spec's expected 5.7 KB (Files 5.1 + viewers 0.6). The extra is most likely LVGL's
decoders and file-system driver state plus the image cache bookkeeping, now compiled out. Caveat: the Task 1 figure came after
2 min idle with three test sessions and the screen off, and this one after a fresh flash, so the two are not like for like.
After the flash `companion status` is unchanged (connected, LAN 192.168.1.50, host key SHA256:EXAM...), and the heartbeat resumed.
