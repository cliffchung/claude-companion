# Local link and a Tailscale switch: design

**Date:** 2026-10-23
**Status:** approved by direction (user, 2026-10-23); the author's calls are marked *(my call)*.
**Builds on:** Plan 2 (power & vitals), merged to main as e1d0678.

## Why

The user keeps the Tab5 at home, on the same Wi-Fi as the Mac. In their words: "Why do we worry about reconnecting to tailscale if that's no longer a priority? Can we just have a toggle to turn tailscale on/off? That way we can save a ton of battery."

On this board, Tailscale (MicroLink) is the slow and expensive part of nearly every measurement:

| Measurement | Time |
|---|---|
| Cold connect after a boot | 50–130 s (about 70 s measured) |
| Check-in handshake | about 45 s |
| Rebind after a Wi-Fi drop | about 95 s |
| Opening a session | about 15 s |

It also holds a large share of the scarce internal RAM. While the radio is parked it retries dead sockets about 1.4 times a second (P117). The 15-minute Hibernate check-in (PT18) is estimated at about 40% of the battery a day, and nearly all of that is the Tailscale connect.

The user may still SSH into a few devices that are reachable only over Tailscale. So Tailscale stays, but behind a switch.

## Goals

1. **A Tailscale on/off switch, off by default, and sticky.** The choice is saved at once and honoured on every boot and wake (user, 2026-10-23). While it is off, MicroLink never starts: not at boot, not on a Wi-Fi change, not on a check-in, not on a reconnect. Turning it on starts it at once. Turning it off stops it at once.
2. **The Mac link always uses the home LAN.** This covers the dashboard, the long poll, take-over, answers and every check-in.
   - The Mac's server entry points at its LAN name, `my-host.local`.
   - If the name doesn't resolve, the entry falls back to a fixed LAN address the user sets (today 192.168.1.50). *(my call)*
3. **Check-ins and wake reconnects get fast.** With Tailscale off, a check-in is Wi-Fi join, then SSH, then poll. It never waits on a tunnel. Target: under 15 s awake on a check-in boot, measured on USB.
4. **The host key carries over safely.** The Mac's host key is already pinned under its tailnet address.
   - When the LAN address has no pin yet, and the key it presents matches the existing tailnet pin for the same server entry, the Tab5 pins it silently.
   - If the key differs, the connect is refused as a changed key, exactly as today.
   - The first LAN connect is therefore never trust-on-first-use. *(my call)*
5. **Tailscale-only hosts give a clear message.** With the switch off, connecting the SSH terminal to a tailnet address or `*.ts.net` name fails at once with "Tailscale is off. Turn it on in Settings → Network." It does not hang or start Tailscale on its own.

## Non-goals

- Using the Mac away from home. If that is ever needed, it is a later "Mac over Tailscale" setting.
- Auto-starting Tailscale on demand. The user asked for a switch.
- Removing MicroLink from the build.
- A full mDNS responder. Name lookup for `.local` only, and only if it is cheap (see Constraints).

## Anti-goals

- **Any path that starts MicroLink while the switch is off.** That would bring back the battery cost this design exists to remove.
- **A first LAN connect that silently trusts an unknown key.** Goal 4 rules this out.
- **Losing the user's Tailscale identity.** MicroLink's keys in NVS stay. Turning the switch back on reuses the registered node, with no new auth key.

## Constraints

- All Plan 1 and Plan 2 global constraints still hold (docs/superpowers/plans/2026-10-23-power-vitals.md, Global Constraints). In particular:
  - never erase NVS;
  - never flash the C6;
  - every Mac command comes from `cc_cmd_*`;
  - no secrets in logs;
  - amber means NEEDS YOU only.
- **Internal RAM** has about 45 KB free. Resolve `.local` names with lwIP's built-in mDNS query support (`CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES`) rather than the full `espressif/mdns` component, and only if the measured internal-RAM cost is under 2 KB. Otherwise use the fixed address. *(my call)*
- **Settings.** Bump `CFG_VERSION` from 20 to 21. The migration sets `ts_enable = 0` once, because the user asked for off. Plan 2's Task 10 tuning bump moves to CFG 22.
- **Mac side.**
  - Remote Login already listens on all interfaces, so sshd needs no change.
  - The Tab5's key line in `~/.ssh/authorized_keys` must also allow the LAN: `from="100.64.0.0/10,192.168.1.0/24"`. This is a security-relevant edit on the user's Mac. It is made only after the user's explicit OK, with the exact line shown first.
- **No sshd config change.**

## Behaviour details

- **Settings → Network** gets a "Tailscale" switch, with its state shown, and the existing auth key, device name and control host rows under it.
  - The existing home-screen quick toggle keeps working until the home-screen stage replaces it.
  - The vitals bar's Tailscale mark shows only while the switch is on.
- **Dashboard "net" readiness** needs Wi-Fi only while Tailscale is off. Today the code already reads `!g_settings.ts_enable || connected`.
- **Check-ins and reconnects.** When the Mac host is not a tailnet address, they skip `tailscale_checkin` entirely: no connect wait, no handshake, no rebind. They go straight to the link and the poll.
- **Console.**
  - `ts on|off` sets the switch and saves it.
  - `companion server-host <host> [fallback-ip]` edits the Mac entry's host and fallback, for the cutover.
  - `companion status` prints which path the Mac link used: `lan <ip>` or `tailnet <ip>`.

## What's left to the builder

- Where the switch logic lives. Keep `tailscale.c` as the only owner of MicroLink.
- How the fallback address is stored. A new field next to the server entry, or in companion settings.
- Exactly where to make the pin-carry-over check (Goal 4). Probably in `cc_link`'s connect path, next to `ssh_known_host_check`. Pure logic gets a host test.

## Measurement (in the plan's device task)

On USB, Tailscale off, compared with the Plan 2 numbers:
- cold boot to live;
- check-in boot awake time and energy (`power checkins`);
- wake reconnect after a Wi-Fi drop;
- time to open a session;
- internal free heap at steady state.
