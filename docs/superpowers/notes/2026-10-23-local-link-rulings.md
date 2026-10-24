# Plan 3 (local link and the Tailscale switch): rulings

Decisions made while executing Plan 3, each with what it costs if wrong. Overrule any of them in review. Format as Plan 1 and Plan 2: `Ruling Ln: … — why — cost if wrong`.

## Rulings

- Ruling L1 (Task 1): the switch is `tailscale_set_enabled(on)`: it saves `ts_enable` to NVS first, then starts MicroLink (Wi-Fi up; otherwise the next Wi-Fi connect starts it) or tears it down. `tailscale_start` itself refuses while the switch is off, so every start path (boot, Wi-Fi event, check-in, the UI toggles) is gated in one place as well as at its caller. Switching off stops MicroLink and frees the instance (`microlink_stop` + `microlink_destroy_finish`), so the internal RAM comes back as if it never started (measured: 84 KB -> 142-148 KB free right after the off, link up before it). MicroLink's node keys stay in NVS: turning it on again logged `Keys loaded from NVS` and reused the node (same VPN IP 100.64.0.20), no new auth key — why: the switch exists to save power and RAM; a stopped-but-allocated instance would keep ~60 KB of the scarce internal heap — costs a full MicroLink init (a few hundred ms) on each switch-on.
- Ruling L2 (Task 1): the link resolves the Mac's host in this order: a literal IPv4 as is; MagicDNS (only when Tailscale is connected); `getaddrinfo` (lwIP sends an mDNS query for `*.local`, DNS for the rest; `*.ts.net` names skip it); and only when all of that fails, the fallback LAN address `mac_lan_ip`. The fallback is for a name that doesn't resolve, never for a connect that fails. It logs `mac: <host> -> <ip> (mdns|dns|fallback|magicdns)`; a literal IP logs nothing. Pins stay per resolved address (P76) — why: the spec's order, and a fallback on connect failure could mask a moved Mac — costs a stale fallback address being tried if the name stops resolving and the Mac moved (the host-key check still guards it).
- Ruling L3 (Task 1, beyond the brief's field list): the settings gain a second appended field, `char mac_ts_ip[16]`, the Mac entry's last tailnet address — the source of the pin carry-over. The link saves it on every successful connect over a tailnet address (only when it changed, so NVS writes are rare), and `companion server-host` captures it before it rewrites the host (the link's last tailnet address, else the old entry host if it is a tailnet IP, else its MagicDNS lookup). Changing the companion server to another name clears it. Forget Mac host key erases its pin too (`pin_targets` grew to 4). On the device it is already `100.64.0.18` (`companion status` prints `tailnet pin source`) — why: without a persisted source the carry-over would only work if the cutover happened without a reboot in between, and a server entry edited in the UI would have none — costs 16 bytes of the settings blob.
- Ruling L4 (Task 1): a LAN address with no pin of its own: the key equal to the `mac_ts_ip` pin is pinned silently (`kh: carried tailnet pin to <ip> (from <tailnet ip>)`); a different key is refused as `KH_CHANGED`; with no tailnet pin it is refused too, with the status `new host key for <ip>; Forget Mac host key to trust it`. The user's Forget Mac host key (Settings or `companion hostkey forget`) is the explicit trust: the next connect after it pins the key it meets, as today. An unreadable tailnet pin is `KH_PIN_ERROR`, never unknown. A tailnet address with no pin still pins on first contact as in Plan 2 (unchanged). `khc_t` gets a fourth value, `KHC_NORMAL`, for "the LAN address has its own pin: use the normal check"; the brief's three values keep their names and order — why: the global constraint "no silent trust", and the link has no screen to ask on — costs a manual Forget for a brand-new Mac set up over the LAN only.
- Ruling L5 (Task 1, mDNS measurement): `CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES` was already `y` (the ESP-IDF 5.5 default) in the generated sdkconfig, so it costs nothing new; it is now pinned in `sdkconfig.defaults`. Measured by building the same tree with it off and on: `idf.py size` DIRAM 213318 B both, `.bss` 57848 B both, `.data` 33080 B both (internal RAM: identical); only external (PSRAM-XIP) `.text` grows 256 B (2333408 -> 2333664) and the image 304 B. On the device, steady internal free heap with Tailscale off ~60 s after boot: 159335 B without, 159331 B with (4 B, noise). `companion resolve my-host.local`: with it on, `mac: my-host.local -> 192.168.1.50 (mdns)` in 398 ms; with it off, no answer in 295 ms (the router's DNS doesn't know the name). Kept: well under the 2 KB rule — costs nothing measurable.
- Ruling L6 (Task 1): a switch-off that catches MicroLink mid-connect: `microlink_stop` waits 15 s, and on the device the coord task was inside a 28 s MapResponse read and outlived it. The vendored `microlink_destroy` would then leak the instance on purpose, and its last task went on to fire `connected` into our state callback while the switch was off (`ts: off (Connected ...)`). Now: callbacks from an instance that isn't `s_ml` are ignored; the stopped instance is kept as a "zombie" and a small reaper task (3 KB stack, exits when done, 300 s cap) frees it once its tasks have exited and no caller holds it; nothing starts a second instance meanwhile (`Tailscale is still stopping; try again in a few seconds`). This adds `microlink_destroy_finish()` to the vendored MicroLink (the second half of `microlink_destroy`, without a second `microlink_stop`, which could `shutdown()` a stale socket fd that now belongs to someone else). Device: `switch off` at 61.0 s, stop timed out at 76.1 s, `the stopped MicroLink instance is freed` at 93.1 s, then `wifi leave`/`wifi join` with no MicroLink line at all — costs a 21-line change in a vendored component and a switch-on refused for up to ~30 s after an off mid-connect.
- Ruling L7 (Task 1): because the instance can now be freed at runtime, tailscale.c serializes init/start/stop/free under one mutex (never held across a long wait) and every caller that uses the instance outside it takes a reference (`ml_get`/`ml_put`). The long waits (a check-in's 8 s handshake and 60 s rebind wait, the link's 20 s tunnel wait, the DERP-region switch) poll in 250-500 ms steps and stop once `s_ml` is no longer their instance, so a switch-off ends them within half a second — costs a little bookkeeping; a caller that forgets `ml_put` would keep a zombie allocated (the reaper gives up after 300 s and logs it).
- Ruling L8 (Task 1): check-ins bring the tunnel up only with the switch on and a Mac host that may be on the tailnet (`ts_host_maybe_tailnet`: a tailnet IP, a `*.ts.net` name or a bare MagicDNS name with no dot). A LAN IP, a `.local` name or another DNS name goes straight to the link and the poll: no connect wait, no handshake, and no rebind escalation when SSH is slow. With the switch off and a Mac entry that needs Tailscale, the check-in fails at once (no 15 s link wait for a link that can't come up). `tailscale_checkin` also returns before any start or connect wait for such hosts: before this change, with the switch on, it waited up to 60 s for MicroLink to connect even for a LAN host, and only then noticed the host wasn't on the tailnet — costs nothing.
- Ruling L9 (Task 1): the link never starts Tailscale. With the switch off and a Mac entry that is a tailnet address or `*.ts.net` name, `connect_once` fails at once with a new `CC_LINK_FAIL_TS_OFF`; the dashboard shows it in red as `The Mac is a tailnet host and Tailscale is off (Settings → Network) (retry in Ns)` — red, never amber (amber is NEEDS YOU only). The link keeps its normal backoff (30 s cap), so a switch-on reconnects within 30 s of MicroLink being up — costs up to 30 s of extra wait after a switch-on.
- Ruling L10 (Task 1): Settings → Network is a new category at index 1 (System, Network, Display, …): the Tailscale switch with its state under it, a "Tailscale details" row that opens the existing Tailscale screen (auth key, device name, control host, peers) instead of copying those rows, the "Mac fallback address" row (a private IPv4 address or empty) and the Mac link path (`Mac link: lan <ip>` / `tailnet <ip>` / `none`). The old Tailscale screen's own switch and the home screen's quick toggle both call `tailscale_set_enabled` now. The preview loops over nine categories and renders the switch from `PREVIEW_TS` — why: one switch semantics everywhere, and no duplicated auth-key UI — costs one more tap to reach the auth key.
- Ruling L11 (Task 1): there was no `net_console.c`; `ts on|off|status` lives in `cc_console.c` next to `wifi`. `ts` also prints the internal heap (free, minimum, largest block), which is how the off/on numbers were taken. Two debug helpers came with it: `companion resolve <host>` (the link's resolver, for the mDNS check) and `ts term-test <host>`, which runs the SSH terminal's own client on a tailnet host and prints its error; it refuses unless the switch is off and the host needs Tailscale, so it can never connect anywhere or open a shell — costs two small console commands.
- Ruling L12 (Task 1): the settings blob's layout and its pure migration moved to `settings/settings_migrate.c` (`CFG_MAGIC`, `CFG_VERSION 21`, `cfg_blob_t`, `settings_blob_load`, `settings_blob_make`), so `test_settings_migrate` runs on the host; the steps that need ESP-IDF (theme, keyboard layout, Plan 2 defaults) stay in `settings_init`. The v20->21 step sets `ts_enable = 0` and clears both new address fields (the old blob's tail padding lands on `mac_lan_ip[0..1]`, which the test fills on purpose). It ran on the device: the switch was on under Plan 2 and read `ts: off` on the first boot of this build — costs nothing.
- Ruling L13 (Task 1): `companion server-host <host> [fallback-ip|none]`: the fallback is optional (omitted keeps the current one, `none` clears it) and must be a private IPv4 address (10/8, 172.16/12, 192.168/16); the host is 1-127 printable characters without spaces. It writes the server entry through `server_store_update` and reconnects — costs nothing.
- Ruling L14 (Task 1): `companion status` gains `path: <lan|tailnet ip|none> (fallback …; tailnet pin source …; Tailscale on|off)` as its last line; the brief's `cc_link_mac_path` returns `"lan <ip>"`, `"tailnet <ip>"` or `"none"` (none while not connected) — costs nothing.

## Fix round 1 (review: a local review file (not kept))

- Ruling L15 (I1, I3, M6, M11): **a switch-off detaches the instance in one locked step and never waits.** One section under `s_ctl` sets `s_ml = NULL`, publishes the instance as `s_zombie` and sets `s_stopping`. A short-lived worker task (`ts_stop`, 6 KB internal stack, created while the old instance still holds its RAM) then runs `microlink_stop` and frees the instance once no task or reference is left.
  - **Callers:** the LVGL task and the console return at once. The UI toasts "Turning Tailscale off…", and the state text reads "Turning off".
  - **Starting while the old instance isn't freed:** the start is refused ("Starting after the stop"), and the worker starts MicroLink itself after the free if the switch is on again by then.
  - **Forget:** `tailscale_forget_node` holds starts off (`s_hold`) and waits up to 40 s for the free before the factory reset, and keeps the node keys if the free doesn't happen. Its UI call runs on its own task.
  - **Rebinds:** both rebinds (the check-in's and the DERP switch's) and the Wi-Fi rebind re-check the switch under `s_ctl`.
  - **`s_worker`** is cleared under `s_ctl`.
  - **Cost if wrong:** after a slow stop, a switch-on starts up to ~30 s late.
- Ruling L16 (I2): **a Mac that isn't found gets its own red reason, `CC_LINK_FAIL_NOT_FOUND`:**
  - when the name doesn't resolve and there is no fallback: `Mac not found at <host> (set its address in Settings → Network)`;
  - when the name didn't resolve and the fallback address doesn't answer: `Mac not at <fallback> (fallback for <host>)`.

  The normal backoff stays. A name that resolves but doesn't answer is still the "is your Mac awake?" case. Cost if wrong: none.
- Ruling L17 (M1): **the carry-over source is the key itself, not a pointer to a pin.** It is saved in NVS `cc`/`carry` as `{entry host, tailnet ip, SHA256 of the key}`.
  - **When it is saved:** on every successful connect to a tailnet address, from the session's own server key.
  - **When it moves:** `companion server-host` moves it to the new host. That is the only host edit it survives; after any other edit of the entry's host it no longer counts, and the connect is refused as unknown. Forget clears it, and so does picking another server as the Mac.
  - `g_settings.mac_ts_ip` is kept as its address, for display and for Forget.
  - **Why:** the terminal shares the address-keyed pins and can rewrite one for another device that later gets the same 100.x address.
  - **Cost if wrong:** an NVS blob of 176 B.
- Ruling L18 (M2): **Forget erases every pin the Mac could have:**
  - the entry's host string;
  - its MagicDNS address;
  - the carry source's tailnet address;
  - the fallback address;
  - the last LAN address the name resolved to;
  - a fresh lookup of the name made at forget time (it can block for seconds; Forget is never called on the LVGL task);
  - the link's own address.

  `PIN_TARGETS` is 7, with duplicates removed. Cost if wrong: about 900 B of stack in the forget caller.
- Ruling L19 (M10): **the whole host-key decision of the link's connect is the pure `kh_carry_action`, which is host-tested.** Its inputs are the LAN pin result, whether the address is a tailnet one, whether a forget is pending, and the carry source (none, equal, different or error). Its outcomes are NORMAL, PIN_NEW, PIN_SILENT, REFUSE_CHANGED, REFUSE_UNKNOWN or PIN_ERROR. `connect_once` only acts on the outcome. A sweep test checks that nothing outside PIN_NEW and PIN_SILENT ever pins, and that PIN_SILENT needs an identical key. Cost if wrong: none.
- Ruling L20 (M3, part): **the "new host key" refusal now carries the fingerprint**, for comparison with `ssh-keygen -lf`. *Skipped:* using the entry's previous LAN pin as a second carry source. The binding constraint allows a silent pin only against the tailnet pin, so a DHCP move after a Forget still needs another Forget. Cost if wrong: one extra Forget in that case.
- Ruling L21 (M5): **a check-in that needed no tunnel is recorded as its own path.** It returns `TS_CHECKIN_SKIPPED`, named "lan", and is recorded as `lan` / `hib-lan`. The new paths are appended (8 and 9), so records already saved keep their meaning. Cost if wrong: none.
- Ruling L22 (M8, M9): **two small fixes:**
  - restoring a backup acts on the restored switch, through `tailscale_set_enabled`;
  - dashboard readiness needs Wi-Fi only for a Mac that can't be on the tailnet, whatever the switch says.

  Cost if wrong: none.
- Ruling L23 (M7): **I measured the fragmentation instead of fixing it.**
  - **The soak:** 10 on→off cycles, with the switch-off at 8, 14 or 20 s into the connect. The largest free internal block held steady at 31744 B through every cycle, and total free held at 139.7–145.9 KB. There were no crashes.
  - **The start:** a fresh boot has 63488 B as its largest block. The first cycle takes it down to about 32 KB, and it stays there.
  - *Skipped:* allocating MicroLink's task stacks once. That would be a larger change to the vendored code, and the measurement shows no slow decline.
  - **Cost if wrong:** a later need for a single internal block over 31 KB after a Tailscale cycle would fail until a reboot or hibernate.

## Re-review quick fixes (task-1-rereview-1.md)

- Ruling L24 (N1–N6 and the strings): **six small fixes from the re-review:**
  - **N1:** the stop worker clears `s_worker` in the same locked step that sees the instance freed. A switch-off a moment later therefore starts a new worker. On the device, a switch-off 6 ms after the worker's auto-start was freed 114 ms later, with no MicroLink lines afterwards.
  - **N2:** a worker that can't be created is created again by the next start, switch-off or forget wait (`ensure_worker_locked`).
  - **N3:** `companion server-host` no longer reads a key from the address-keyed pin. With no saved key for the entry, the carry source is cleared, and the next tailnet connect captures it.
  - **N4:** a second Forget while one runs is a no-op. The log says "forget: already forgetting" and the UI toasts "Already forgetting".
  - **N5:** a check-in while an instance is still stopping fails at once instead of waiting 60 s.
  - **N6:** the unused flag is dropped.
  - **Strings:** the new user-visible texts say "Host": "Host not found at …", "Host not at … (fallback for …)", "Forget host key to trust it", and the log line "Host entry host -> …". The full-UI rename is out of scope.

  Cost if wrong: none.

## Task 2 (cutover and measurement)

- Ruling L25 (Task 2): **a take-over of the companion's host connects to the address the link resolved and pinned** (`cc_link_mac_address`: the mDNS, DNS or fallback address), not to the entry's name.
  - **Why:** pins are per address (P76). With the entry on `my-host.local`, the terminal looked up the pin for the name, found none and asked to trust a key the link had already carried over. That breaks "no unknown-key prompt" for the dashboard's take-over.
  - **Device:** after the fix, three take-overs opened with no prompt, connected in 0.62–0.68 s.
  - **Scope:** a plain SSH Terminal session to other servers still pins by the entry's host string, as before.
  - **Cost if wrong:** if the link hasn't resolved yet, the take-over falls back to the name, and asks.
- Ruling L26 (Task 2): **two debug hooks for the measurements:**
  - `power checkin hibernate [keep|nokeep] usb` runs a check-in boot on USB as if on battery. Without it, the PT28 I2 gate wakes fully on USB (`hib-usb`) and nothing is measured.
  - `companion takeover <id>` selects that card and opens it as Ctrl+O does. It works only for a `companion-test-*` session, so the timing never risks the user's sessions.

  Cost if wrong: two debug commands.
- Ruling L27 (Task 2): **`power boot`'s `to_live` holds only the last hibernate wake** (`on_sessions` stores it only when the boot kind isn't normal). Normal-boot figures come from the `hib: boot to live: N ms (kind 0)` log line. Plan 2's notes read the same log, so their 45.6–67.8 s compares directly. Not changed. Cost if wrong: a misleading console value; the notes say how to read it.
- Ruling L28 (Task 2, observation): **with Tailscale on, two SSH sessions over the tunnel starve internal RAM.** A take-over while the link was up drove the internal minimum free heap to 8116 B (largest block 31744 B). The link dropped, and MicroLink's WireGuard sends failed with `err=-2` (`ERR_BUF`).
  - **Likely the same cause as Task 1's DNS outage:** that one had a 22.7 KB minimum, and both DNS and mDNS were dead.
  - **On the LAN with Tailscale off,** internal free stays at about 129 KB (minimum 35.9 KB, largest 63488 B) with the link, the watch and take-overs.
  - **Who it hits:** the user only while the switch is on. Worth a follow-up if the switch is used for long tailnet sessions.
  - **Cost if wrong:** none; it is a measurement.

## Task 3 (fast check-in)

Numbers are medians over 5 check-in boots on USB (`power checkin hibernate keep usb chain 4`), in ms from the timer wake. The full tables are in the notes.

- Ruling L29 (Task 2 review I1, M1, M2): **carried fixes.**
  - `companion takeover` acts only with the dashboard on screen, its cards live and the link connected (`!s_scr || s_scr != lv_screen_active() || !s_shown_live || !cc_link_connected()`), and the console then prints `takeover: dashboard not showing`. The function returns an enum, so "no card" and "not showing" read differently.
  - `s_pin_host` is cleared on every host-key refusal (changed, unreadable, unknown), so a take-over can never be handed a refused address.
  - `cc_link_set_server_host` clears `s_pin_host` at once, under the lock, after the carry source has been read from it.
  - Cost if wrong: none.
- Ruling L30 (Step 1): **how the phases are timed.**
  - `sys/boot_phase.c` marks 15 phases on esp_timer.
  - esp_timer starts at about 0 ms when the app starts, not at the reset. The offset comes from the RTC timer: on the way down the RTC time of the armed wake is saved, and `app_main` reads the time since then.
  - Reset to `app_main` is 3.40 s on every boot. The bootloader log shows where it goes: about 0.67 s before the bootloader (on this P4 rev 1.3 the wake goes through an HP watchdog reset), 1.43 s hashing segments 0 and 4 for validation, 0.17 s PSRAM tuning, and 1.0 s copying .rodata and .text to PSRAM (XIP).
  - **`power checkins` undercounts:** its ms is esp_timer, so Task 2's "8.0–8.4 s" check-in was really about 12.4 s awake.
  - The ring of 8 tables lives in RTC memory. `... chain <n>` re-hibernates n more check-in boots with the 60 s test timer, so every sleep has a timer. `... exp <mask>` turns single changes off for A/B runs.
  - "scan done" can't be seen from the P4 (esp_hosted gives no event). It is folded into join→assoc.
  - The TCP mark is a 3-line hook in the vendored libssh (`ssh_companion_tcp_connected_hook`).
  - Cost if wrong: about 600 B of RTC memory and two debug console options.
- Ruling L31 (Step 2): **the Wi-Fi join cache was built, measured and dropped.**
  - The cache held BSSID, channel and auth mode, never the PSK. Joins used `WIFI_FAST_SCAN` with `bssid_set` and `channel`, and fell back to the full scan.
  - The C6 associates in about 3.1 s either way:
    - warm, cached: 3105 and 3131 ms; warm, full scan: 3158 and 3184 ms;
    - cold check-in boots: 3.16–3.19 s both ways;
    - the C6 reports its home-channel change about 2.7 s after the connect, even with the channel set.
  - The C6 firmware (reports 0.0.0) seems to ignore the scan method and channel, and it can't be flashed (global constraint).
  - The brief's 3 s fallback made things worse: it fired at 3.0 s, just before the 3.1 s association (chain A: 13.9 s against 12.5 s).
  - The code and its host tests were removed. The test build left a 52-byte NVS key `wifi`/`jc` with no secret in it; it is harmless and was not erased (no NVS erase).
  - Cost if wrong: none now; a future C6 image that honours the fields could save about 2.7 s.
- Ruling L32 (Step 3): **DHCP: restore the lease and skip lwIP's own ARP probe.**
  - `CONFIG_LWIP_DHCP_RESTORE_LAST_IP=y` alone gave about 40 ms (assoc→IP 1053 ms against 1096). lwIP's DHCP ARP check (about 1 s) was the real cost.
  - `CONFIG_LWIP_DHCP_DOES_NOT_CHECK_OFFERED_IP=y` brings assoc→IP to 33–40 ms.
  - The router's DHCP server checks an address before it offers or ACKs it. The lease is written to NVS only when it changes.
  - Cost if wrong: a duplicate address on the LAN isn't caught by the Tab5.
- Ruling L33 (Step 3): **the static address is optional and off by default** (Settings › Network › Wi-Fi advanced, "address/prefix gateway [dns]", the pure `sip_parse`; console `wifi static`).
  - It is stored in NVS `wifi`/`sip`, not the settings blob, so there is no CFG bump (CFG 22 stays Plan 2 Task 10's).
  - Against the restored lease without the ARP check it saved 58 ms (9742 against 9800 ms): under the 300 ms bar, so it stays off. With the ARP check still on it saved about 0.9 s.
  - Cost if wrong: none (off).
- Ruling L34 (Step 4): **the Host address cache** (NVS `cc`/`hcache`: entry host and its last mDNS/DNS/fallback address, never a MagicDNS one).
  - The pure `hc_use` allows it only for the same entry, a name rather than a literal, no pending forget, and a tailnet address only with the switch on.
  - `hc_cached_key` keeps the cached address only when its own pin matches (KH_OK). Any other result is dropped without pinning or refusing, and the normal lookup and check decide, so a changed key still refuses as before.
  - A cached address that doesn't answer leads to a lookup. A lookup that gives the same dead address fails at once instead of a second 10 s connect.
  - Measured: resolve goes from 142 ms (mDNS) to 0. The first mDNS after a flash took 1.3 s. A moved Host costs one 10 s connect timeout once (tested with `companion hostcache set 192.168.1.250`: timeout, then `mdns`, connected, cache restored).
  - The whole-check-in median moved only 24 ms (9800 against 9824), which is within the noise, but kept: it removes the mDNS step and its failure modes.
  - Cost if wrong: one 10 s timeout after the Host's address changes.
- Ruling L35 (Step 5, WPA2/WPA3): **nothing to do.** The router is WPA2 Personal only: the Mac reports "WPA2 Personal", and the Tab5 associates with auth 3 (WPA2_PSK). There is no SAE and no transition mode, so no "prefer WPA2" option, and no threshold was changed. Cost if wrong: none.
- Ruling L36 (Step 5, Wi-Fi power): **no modem sleep during a check-in boot.**
  - `wifi_mgr_ps_hold_none` holds `WIFI_PS_NONE` from `checkin_init` until the sleep. It is released on any path that stays up (USB wake, give-up, hold, input, debug stay-awake), and the power task's last wish is then applied.
  - Max modem made every downlink wait for a beacon: kex 684 ms against 335, poll 463 ms against 270.
  - Measured 10505 ms with max modem against 9800 ms: saves 0.70 s. `listen_interval` is left alone: it only matters while the radio may sleep.
  - Cost if wrong: radio current for about 6 s per check-in, much smaller than 0.7 s of the whole device.
- Ruling L37 (Step 5, CPU clock): **already 360 MHz** (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360`, no PM, so nothing lowers it). Nothing changed. The bootloader's hashing is bound by flash speed (40 MHz DIO; 80 MHz isn't offered for P4 rev < 3). Cost if wrong: none.
- Ruling L38 (Step 5, lean init): **dropped.**
  - Starting Wi-Fi before the display on a check-in boot didn't move the join (9815 against 9800 ms). esp_hosted resets the C6 at transport init (app +0 ms), and the C6 needs about 1.5 s before `esp_wifi_init` can talk to it. The display and UI init already run inside that wait.
  - The code was reverted. Skipping LVGL or fonts would gain nothing on the critical path.
  - Cost if wrong: none.
- Ruling L39 (Step 5, bootloader validation): **`CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP` was measured and dropped** (L-C1 allowed it).
  - The bootloader skips validation only when the reset reason is `CORE_DEEP_SLEEP`. On this chip every timer wake arrives as `HP_SYS_HP_WDT_RESET` (rst:0x7), so there was no "Fast booting" line, and reset→app stayed 3397 ms.
  - The validation still costs 1.43 s. Only `SKIP_VALIDATE_ON_POWER_ON` or `ALWAYS` would remove it, and P125 objected to those, so they were not tried.
  - Cost if wrong: none.
- Ruling L40 (Step 5): **shorter waits.**
  - The check-in's `wait_for` polls every 20 ms, not 200 (poll→judged 200 → 20 ms).
  - A check-in boot's way down skips the 300 ms wait for the watch loop (it never runs on a check-in boot, M3) and waits 50 ms, not 200, after the deauth.
  - Measured 10539 ms with the old waits against 9800: saves 0.74 s.
  - Cost if wrong: a deauth cut short.
- Ruling L41 (found while measuring): **the SSH session is closed before the Wi-Fi goes down.**
  - `go_down` now parks the link (a clean `ssh_disconnect`) and waits up to 1 s before the Wi-Fi disconnect. Before, the link noticed only after the Wi-Fi was gone, so every check-in left an idle `sshd-session` on the Host. macOS sshd here has no ClientAlive, so they last until TCP keepalive.
  - After about 60 test check-ins, 42 of them had piled up (launchd `copy count = 42`). New connects were then reset ("Connection reset by peer"), from the LAN and from localhost too.
  - I ended those 42 orphaned sessions. Each was user-owned `sshd-session: you@notty` with peer 192.168.1.60 (the Tab5), and nothing ran under it. sshd answered again at once. After the fix, a 5-boot chain left 0 sessions.
  - This was pre-existing (Plan 2): at the 15-minute cadence a few idle sessions were always there.
  - Cost if wrong: up to 1 s more awake if the close hangs (measured: about 30 ms).
- Ruling L42 (Step 5, battery settle): **a check-in boot calls battery after 3 battery-like samples, not 10** (`vitals_src_seed_battery`, host test `test_checkin_prior_battery`).
  - A check-in boot only follows a sleep that began on battery: the ladder and the check-in's re-hibernate both need battery.
  - USB still wins at once, and a no-evidence sample needs the full count again.
  - Why: Plan 2 measured the settle at 10–12 s after the wake, and the gate waits for it. On battery that, not the network, decided the awake time. The USB `usb` hook can't show it, because USB is known at once.
  - **Estimated, not measured on battery:** about 15.4 s → about 9.8 s awake.
  - Cost if wrong: a USB plug-in during the sleep, under a load the USB supply can't carry, is read as battery for 3 s. That check-in then re-hibernates instead of waking fully. It still charges asleep, and the next check-in decides again.
