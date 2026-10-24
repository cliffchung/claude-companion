# Local Link and Tailscale Switch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Mac link and every check-in run over the home LAN. Tailscale becomes an on/off switch, off by default, that never starts MicroLink while it is off.

**Architecture:**
- `net/tailscale.c` stays the only owner of MicroLink. Every start path is gated on `g_settings.ts_enable`.
- The companion link resolves the Mac's `.local` name, with a fixed-address fallback. It carries the host-key pin over from the tailnet address when the keys match.
- The check-in and reconnect paths skip the tunnel when the Mac host is not a tailnet address.

**Tech Stack:** ESP-IDF v5.5.6, LVGL 9.4, libssh 0.11, MicroLink, lwIP, Unity host tests, Python unittest.

**Spec:** docs/superpowers/specs/2026-10-23-local-link-design.md

## Global Constraints

- Every Plan 2 global constraint holds unchanged (docs/superpowers/plans/2026-10-23-power-vitals.md, Global Constraints), including:
  - test only against `companion-test-*` sessions;
  - never open a login shell on the Mac;
  - every Mac command line comes from a `cc_cmd_*` function;
  - never flash the C6, never erase NVS, never run `sshkey regenerate`, never change sshd config;
  - never drive 0x44 P4 outside `power hib test off`;
  - no secrets in logs.
- **The switch is off:** while `g_settings.ts_enable == 0`, nothing calls `microlink_init`, `microlink_start` or `microlink_rebind`.
- **No silent trust:** a LAN address with no pin is pinned without asking only if its key equals the tailnet pin of the same server entry. A different key is refused as `KH_CHANGED`.
- **Settings:** `CFG_VERSION` goes from 20 to 21. Migrating from 20 or older sets `ts_enable = 0` once.
- **The switch is sticky (user, 2026-10-23):** `tailscale_set_enabled` saves to NVS at once. Every boot reads the saved value: cold boots, deep-sleep and check-in wakes, probe wakes and button wakes. Nothing resets the switch except the user, and the one-time v20→21 migration never runs again on a v21 blob.
- **Mac side:** the `authorized_keys` change to `from="100.64.0.0/10,192.168.1.0/24"` is made only after the user's explicit OK. Until then, device tests that need the LAN path are PENDING.
- **Internal RAM:** about 45 KB is free. mDNS `.local` resolution is used only if `CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES` costs under 2 KB of internal RAM, measured. Otherwise use the fallback address only.

## Review Focus

1. **The switch is turned off while MicroLink is mid-connect.** Expected: it stops cleanly, and nothing restarts it on the next Wi-Fi event.
2. **The Mac's DHCP address changes and `.local` doesn't resolve.** Expected: the link fails with a clear "Mac not found at <host>" reason, and the user can set the new address from the console or Settings. It is never a silent hang.
3. **A check-in boot with Tailscale off.** Expected: no tunnel wait at all. The check-in awake time is far below Plan 2's.
4. **The SSH terminal to a `100.x` or `*.ts.net` host with the switch off.** Expected: an immediate clear error, and Tailscale does not start.
5. **The pin carry-over when the tailnet pin is missing,** for example after a forget. Expected: the normal unknown-key path, with the fingerprint shown, as Plan 2's P79 does.

---

### Task 1: Tailscale switch, LAN-first Mac link, pin carry-over

**Files:**
- Modify: `firmware/main/net/tailscale.c`, `tailscale.h`: gate every start path on `ts_enable`. Add `tailscale_set_enabled(bool)`, which saves the setting and starts or stops MicroLink.
- Modify: `firmware/main/app_main.c`: the boot and Wi-Fi-event paths go through the gate.
- Modify: `firmware/main/sys/checkin.c`: skip `tailscale_checkin` when the Mac host is not a tailnet address.
- Modify: `firmware/main/companion/cc_link.c`, `cc_link.h`:
  - resolve the Mac host as `.local`, then the fallback address;
  - the pin carry-over;
  - `companion status` prints the path;
  - the console `companion server-host`.
- Modify: `firmware/main/ssh/ssh_client.c`: the tailnet-host-while-off error.
- Create: `firmware/main/companion/kh_carry.c`, `kh_carry.h`: the pure carry-over decision.
- Modify: `firmware/main/settings/settings.c`, `settings.h`: CFG 21, the migration, and the fallback address field (`char mac_lan_ip[16]`, appended).
- Modify: `firmware/main/ui/ui_settings.c`: Settings → Network → Tailscale switch, and the Mac fallback address row.
- Modify: `firmware/main/net/net_console.c`, or wherever the `ts` console command lives: `ts on|off`.
- Modify: `firmware/sdkconfig.defaults`: `CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES=y`, only if it passes the RAM rule.
- Modify: `firmware/tools/ui_preview/preview.c`: stubs.
- Test: `firmware/test/host/test_kh_carry.c`, and `test_settings_migrate`, which exists already or is added by Plan 2 9c M2.
- Modify: `docs/superpowers/plans/2026-10-23-power-vitals.md`: Task 10's bump becomes CFG 22.

**Interfaces:**
- Produces:
  - `void tailscale_set_enabled(bool on);`
  - `bool tailscale_enabled(void);`
  - `typedef enum { KHC_PIN_SILENT, KHC_UNKNOWN, KHC_REFUSE } khc_t;` and `khc_t kh_carry_decide(bool lan_pinned, bool tailnet_pinned, bool keys_equal);`
    - lan_pinned → the caller uses the normal check;
    - otherwise tailnet_pinned && keys_equal → PIN_SILENT;
    - tailnet_pinned && !keys_equal → REFUSE;
    - !tailnet_pinned → UNKNOWN.
  - `bool cc_link_mac_path(char *out, size_t cap);` returns `"lan <ip>"`, `"tailnet <ip>"` or `"none"`.

- [ ] **Step 1: Write the failing host tests**
  - `test_kh_carry`, covering four cases: lan pinned; tailnet pinned with equal keys (PIN_SILENT); tailnet pinned with different keys (REFUSE); no tailnet pin (UNKNOWN).
  - `test_settings_migrate_v20_turns_tailscale_off`: a v20 blob with `ts_enable = 1` loads as `ts_enable = 0`, `mac_lan_ip = ""`, every other field unchanged, and version 21.
  - `test_settings_v21_keeps_tailscale_choice`: a v21 blob with `ts_enable = 1` loads with `ts_enable = 1` (sticky).
- [ ] **Step 2: Run them and see them fail.** Command: `cmake --build <hostbuild> && ctest -R "kh_carry|settings"`. Expect: link errors or failures.
- [ ] **Step 3: Implement the code**, following the Files list and Interfaces above. Find every MicroLink start path with `grep -rn "tailscale_start\|microlink_start\|microlink_rebind\|tailscale_on_wifi" firmware/main` and gate each one.
  - The SSH terminal error text is exactly `Tailscale is off. Turn it on in Settings → Network.`
  - Resolving: try the entry's host through `getaddrinfo` (with mDNS queries enabled, `.local` resolves). If that fails, and `mac_lan_ip` is set, use it. Log `mac: <host> -> <ip> (mdns|dns|fallback)`.
- [ ] **Step 4: Measure the mDNS cost.** Build with and without `CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES`. Compare `idf.py size` for internal DRAM and BSS, plus internal free heap at steady state on the device. Keep the option only if the cost is under 2 KB. Record the numbers as a ruling.
- [ ] **Step 5: Make the tests pass.** Run the full host suite, the Python tests, `idf.py build` and the preview.
- [ ] **Step 6: Check on the device, without the LAN key change.** Flash, then check:
  - `ts off`, reboot, then `power` and the log: no `ml_` lines after boot, and internal free heap noted.
  - `ts on`: MicroLink starts and connects, and the link still works over the tailnet entry.
  - `ts off` mid-connect: it stops, and a `wifi leave`/`wifi join` doesn't restart it.
  - Sticky: `ts on`, then `power hib test timer 20`. After the wake MicroLink starts again, and `ts` shows on. Then `ts off`, the same sleep, and after the wake there are no `ml_` lines.
  - SSH terminal to a `100.x` host with the switch off: the exact error.
- [ ] **Step 7: Commit.**

### Task 2: Cutover and measurement (needs the user's OK for the Mac key line)

**Files:**
- Modify: `docs/superpowers/notes/2026-10-23-local-link-notes.md` (create it): the cutover steps and the numbers.

- [ ] **Step 1: The user's OK.** With the user's explicit OK only, change the `claude-companion` line in `~/.ssh/authorized_keys`:
  - from `from="100.64.0.0/10"`
  - to `from="100.64.0.0/10,192.168.1.0/24"`

  Touch nothing else. Back up the file first, to `~/.ssh/authorized_keys.bak-2026-10-23`.
- [ ] **Step 2: Point the Mac entry at the LAN.** Run `companion server-host my-host.local 192.168.1.50`, then `ts off`.
- [ ] **Step 3: Check the link and the pin carry-over.**
  - `companion status` shows `connected` and the path `lan 192.168.1.50`.
  - The log shows the pin carry-over as `kh: carried tailnet pin to 192.168.1.50` with no unknown-key prompt.
  - The cards go live.
- [ ] **Step 4: Measure on USB, against Plan 2:**
  - cold boot to live (`power boot` `to_live`);
  - a check-in boot's awake time and energy (`power hib test-wake 30` then `power checkin hibernate nokeep`, then `power checkins`);
  - a wake reconnect after `wifi leave`/`wifi join`;
  - time to open a take-over;
  - internal free heap.
- [ ] **Step 5: Check the safety rules.**
  - A `companion-test-*` session blocking during a check-in still lights amber.
  - Five reboots with no new brownout or panic.
- [ ] **Step 6: Write the notes, then commit.**

### Task 3: Fast check-in (Wi-Fi join, address, host lookup)

The user, 2026-10-23: "consider maybe using a static IP address or something if that speeds up wifi connection or see if there is _anything_ else we can do like caching or something that might help save power. I worry about power the most here."

Every second awake on a check-in costs about 0.7 mWh. At 96 check-ins a day, cutting 5 s saves about 0.3 Wh a day (about 2% of the pack).

**Files:** `firmware/main/net/wifi_mgr.c`, `wifi_mgr.h`, `firmware/main/companion/cc_link.c`, `firmware/main/sys/checkin.c`, `firmware/main/sys/hibernate.c` (the hib record), `firmware/main/settings/settings.*`, `firmware/main/ui/ui_settings.c` (Network › Wi-Fi › Advanced), `firmware/sdkconfig.defaults`, and host tests for any pure rule.

- [ ] **Step 1: Time each phase first.** On a check-in boot, log ms from reset for each of:
  - bootloader done
  - app start
  - esp_hosted/C6 ready
  - Wi-Fi start
  - scan done
  - associated
  - got IP
  - host resolved
  - TCP connected
  - SSH kex done
  - auth done
  - poll done
  - sleep

  Run 5 check-in boots on USB and record the median per phase. Every later step is judged against this table.
- [ ] **Step 2: Fast join.**
  - After every successful join, save the AP's BSSID, channel and auth mode. Put them in the hib record (RTC or NVS) and in NVS.
  - On a check-in or wake boot, connect with `bssid_set` and `channel` set, `scan_method = WIFI_FAST_SCAN` and the cached auth mode as the threshold.
  - If that join fails within 3 s, fall back to today's full scan. Clear the cache after 2 consecutive failures.
  - Normal boots also use the cache.
  - Host test: the cache-use decision.
- [ ] **Step 3: Fast address.**
  - Turn on `CONFIG_LWIP_DHCP_RESTORE_LAST_IP=y`, so DHCP reuses the last lease (INIT-REBOOT) instead of a full discover.
  - Add an optional static IP setting, off by default, under Network › Wi-Fi › Advanced: address, mask, gateway and DNS. When it's set, skip DHCP entirely.
  - Measure both against plain DHCP. If the static IP saves less than 300 ms over the restored lease, say so in the notes and keep the setting off by default.
- [ ] **Step 4: Fast host lookup.**
  - Cache the host's last resolved address, with its source name, in NVS and the hib record.
  - Connect to the cached address first. Use mDNS only when that connect fails or the host key doesn't match the pin.
  - A changed key still refuses, as before. The address cache only skips the lookup and never weakens the pin check.
- [ ] **Step 5: Other quick wins.** Measure each one; keep it only if it helps and is safe. Record every outcome as a ruling.
  - **WPA2 vs WPA3:** if the router offers WPA3 transition mode, does SAE add time? If so, add a cached "prefer WPA2-PSK" option. Never weaken an AP that is WPA3-only.
  - **Wi-Fi power while connected:** `WIFI_PS_MAX_MODEM` plus `listen_interval`.
  - **CPU clock:** run the boot and the SSH key exchange at 360 MHz, so the work finishes sooner.
  - **Skip non-essential init on a check-in boot:** the display, the LVGL screen build, fonts and keyboard layout loading, keeping only what the poll and the light need.
  - **Bootloader validation:** `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP` (only after deep sleep, not on a cold boot or OTA). It saved about 1.3 s in Plan 2's P125. That ruling's objection was about skipping validation on every boot, which doesn't apply to the deep-sleep-only option.
- [ ] **Step 6: Measure again.** Re-run the Step 1 table with everything kept, record the before and after per phase, and estimate the new check-in mWh and the Wh per day.
- [ ] **Step 7: Commit**, with the notes and the rulings.

### Task 4: Spike — instant needs-you without polling (C6 stays associated, wakes the P4)

This is a throwaway spike. It decides whether a later plan replaces the 15-minute polling.

**The idea:**
- In Hibernate, keep the C6 powered and associated in its own Wi-Fi power save (DTIM sleep). Today WIFI_EN is cut, so every check-in cold-boots the C6.
- The P4 deep-sleeps.
- When the host has something new, a packet for the Tab5 arrives (for example a UDP or TCP knock to a port), and the C6 wakes the P4 over a GPIO.

That would give instant amber with no periodic boots, if the C6's average standby draw is below today's check-in average. That average is about 8 mWh per 15 min, about 32 mW, and Task 3 should lower it.

**Steps:**
- [ ] **Step 1: On paper.** Answer these from the esp_hosted 3.0.10 Kconfig and docs, and from the Tab5 schematic and spike notes:
  - Does esp_hosted support host power save with a wakeup GPIO?
  - Which C6 GPIO goes to which P4 pin?
  - Is that P4 pin an LP GPIO that can wake deep sleep through ext1?
  - What is the C6's documented associated-sleep current at DTIM 1, 3 and 10?
- [ ] **Step 2: Decide.** If the wake line isn't an LP GPIO, or host power save needs something this board lacks, record "not feasible" and stop.
- [ ] **Step 3: Only if Step 2 passes, prototype it in a scratch build.** Keep WIFI_EN on in Hibernate. Arm ext1 on the wake line. Send a test knock from the Mac with `nc -u <tab5-ip> <port>` and show the P4 wakes.
  - Measure on USB: standby with the C6 associated vs cut.
  - Real battery numbers come at the Task 10 visit.
- [ ] **Step 4: Write it up** in `docs/superpowers/notes/2026-10-23-push-wake-spike.md`: feasible yes/no, the numbers, and a recommendation. The push model would need a host-side sender, which means running something on the host, so it would need its own design pass. Nothing ships from this task.
