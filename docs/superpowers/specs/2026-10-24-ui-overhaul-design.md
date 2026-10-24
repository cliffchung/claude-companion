# Plan 4: UI overhaul: Design

**Date:** 2026-10-24
**Status:** approved by direction (the user, 2026-10-23). The audit's verdicts, rulings U1–U11 and
the five mockups are settled. Calls this spec makes that the inputs don't settle are marked
**Controller call** and collected in §14.
**Builds on:** Plan 2 (power & vitals, on main at e1d0678) and Plan 3 (local link, branch
`plan3-local-link`). Plan 3 merges to main before Plan 4 is built.

**Inputs this spec is based on.** These are in the controller's scratchpad and are not in the repo.
Every number and verdict needed to build is copied below.
- `uiaudit/ui-audit.md`: 44 screens and features (KEEP 18, MERGE 8, HIDE 4, REMOVE 14), with the
  runtime cost of each and a proposed information architecture.
- `uiaudit/settings-inventory.md`: all 101 settings.
- `uiaudit/settings-decisions.md`: rulings U1–U11.
- `uidesign/project/*.dc.html`: the approved mockups (Sessions home, Settings › Host,
  Settings › Network, Terminal, States).
- The current screens, rendered with the host preview.

---

## 1. Intent and why

The user's words (2026-10-23): "We should do a thorough review of the UI and remove all the
unnecessary crap and optimize our UI so that it is beautiful and fits with exactly what we need to
get this purpose built device working really well for us." Also: "I worry about power the most
here."

The Tab5 does one job. It shows the Claude Code sessions on the user's computer (the **Host**),
shows which ones need you, lets you answer one, and lets you take one over. A few times it also
opens an SSH shell to a tailnet box. Today's UI is a fork of a general-purpose device shell
(Clientre 5). It has 18 screens, an app launcher, a music player, a camera, a file manager, nine
themes and two status bars. It also uses two visual systems. The default theme is amber-orange, so
the colour that is meant to say "this needs you" floods every screen that isn't the dashboard,
including the take-over terminal.

This plan makes the device:
- **Single-purpose:** one home screen (Sessions), with Terminal and Settings one level down.
- **One visual system:** fixed tokens, where amber means NEEDS YOU and nothing else.
- **Cheaper to run:** it removes the code, tasks, timers and internal RAM of everything it drops.
  With Tailscale on, a take-over drives internal free heap down to 8,116 B (Plan 3 ruling L28),
  which drops the link. The removals are the first step in getting that back.
- **Vendor-neutral:** the machine running Claude Code is called "Host" everywhere the user can see
  it. Windows and Linux hosts may come later (U11).

## 2. Goals, non-goals, anti-goals

### Goals
1. **Six screens.** Today there are 18. The new set:
   - Sessions (home);
   - Take-over;
   - Terminal (host list);
   - SSH session;
   - Host form;
   - Settings, with six sections: Host, Network, Power, Keyboard light, Display & time, About.
2. **The Sessions home is master-detail.** The list stays put while you answer. The detail pane
   holds the log tail, the answer field, Send and Open, and a reserved mic slot.
3. **Touch can finish the core loop:** select, answer, send, open and leave. Keyboard-first (R4)
   still holds, and every touch action also has a key.
4. **Every one of the 101 settings has a home**, a console-only status or a removal, with a reason
   (§6). This includes the "lost" ones: the device's public key (U1), Forget host key for every
   host (U2), Reset settings (U3), and every Tailscale setting (U10).
5. **Removed means removed.** A removed screen loses its code, its components, its strings, its
   Kconfig, its tasks and its timers. It is not just hidden from the UI (§7).
6. **Internal RAM goes up and stays up.** The expected gain is about 18–24 KB of internal RAM at
   steady state (§7.2). No new screen may cost more internal RAM than the screen it replaces.
7. **No new idle cost.** Removing the 60 ms screenshot poll, the status-bar timers, the music-bar
   hook and the MSC task leaves the UI idle when nothing changes (§11).
8. **"Host", never "Mac",** in every string the user can see (U11, §9).

### Non-goals
- **Voice input.** It is the next plan, using whisper.cpp on the Host over SSH. This plan only
  reserves the mic slot and the Ctrl+Space hotkey, and keeps the BSP mic codec.
- **Camera** comes last, if ever. Plan 6 re-adds a purpose-built camera from git history.
- **An OTA UI.** It returns as "Check for update" in About in Plan 7.
- **Portrait rotation, a light mode, themes or user colours.**
- **More than one Host for sessions.** Support for Windows or Linux Hosts is also out of scope;
  this plan only makes the wording ready for them.
- **Changes to the link, poll, power ladder or check-in behaviour.** Plans 2 and 3 own those.
- **Languages other than English.** `lang` stays reserved.
- **A new typeface.** See the Controller call in §8.3.

### Anti-goals (a failure even if it "works")
- **Amber anywhere outside a NEEDS YOU state:** a button, a focus ring, a selected tab, a
  terminal palette, a keyboard-light preset, the boot splash.
- **An input that wakes the device and then acts:** a wake press that reaches the Host, or a wake
  tap that selects, opens or sends.
- **An answer that reaches a session other than the one on screen.** For example, after a re-sort
  moved the selection.
- **Hidden controls:** invisible hot zones, long-press shortcuts, or a control you can only find
  from a toast.
- **A new always-running timer**, or any idle redraw on a static screen.
- **A UI path that opens a login shell on the Host, or lists the Host in Terminal.**
- **A runtime theme engine,** in any form.

## 3. Constraints

### Hard project rules (unchanged; restated because this plan touches every screen)
1. **Never open a login shell on the Host, and never touch the user's tmux.**
   - Sessions open through a pty exec of an explicit `claude attach <id>`.
   - Terminal never lists the Host and refuses to connect to it.
2. **Every command line sent to the Host comes from a `cc_cmd_*` function.** That includes
   answers, opens and log fetches. No UI code formats a command line.
3. **No secrets in the repo or in logs.** The secrets are:
   - the Tailscale auth key;
   - Wi-Fi passwords;
   - host passwords;
   - the device's private key;
   - typed answer text (the existing masking in the key log stays).

   Secrets are typed masked and shown masked.
4. **No GPL code.** libssh (LGPL-2.1) stays.
5. **Keep `firmware/LICENSE.clientre5`** and the line "Based on Clientre 5 by Mozgi512 (MIT)".
   About shows that line.
6. **Never flash the C6, erase NVS, or run `sshkey regenerate`.**
7. **A wake input never reaches the Host and never acts on a card.** The first key or tap that wakes
   the screen from dim or off is swallowed on every screen, terminals included.
8. **Real-session screenshots stay out of git.** Device tests use only `companion-test-*`
   sessions. Their screenshots go to scratch storage, not the repo.
9. **Amber `#f4b545` means NEEDS YOU only.**

### Technical constraints
- **Stack:** ESP-IDF v5.5.6, LVGL 9.4, a 1280 × 720 landscape panel, the A164 keyboard (it reports
  no key-up), and touch. Internal free heap at steady state is about 45 KB with Tailscale on and
  about 129 KB on the LAN with it off (L28).
- **LVGL allocations of 512 B or less land in internal RAM** (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`).
  Every widget, and every local style, costs internal RAM while it exists. So:
  - share static `lv_style_t` styles from the token module instead of per-object
    `lv_obj_set_style_*` local styles;
  - build only the visible Settings section (P54 stays);
  - update the Sessions screen in place. Never rebuild it on a poll.
- **Settings blob.** `app_settings_t` is written whole as one NVS blob. No field is removed or
  reordered; dropped fields become reserved (U8, §10).
- **Plan 3's surfaces are used as they are:**
  - `tailscale_set_enabled()`, with `ts_enable` sticky and off by default;
  - `cc_link_set_lan_fallback()` / `cc_link_lan_fallback()` for `mac_lan_ip`, the Host's fallback
    LAN address;
  - `cc_link_mac_path()` for the link path (LAN or tailnet);
  - `wifi_mgr_static_ip_get/set()`, the static address. It is stored in NVS `wifi`/`sip`, not in
    the blob, and applies from the next join;
  - the host-address cache (`companion hostcache`).
- **CFG version.** CFG 21 is Plan 3's. CFG 22 is reserved for Plan 2's Task 10 battery visit.
  **Plan 4 takes CFG 23** (§10).

## 4. Information architecture

```
Sessions (home, level 0)
 ├─ Enter on an empty field / Ctrl+O / tap the selected row / Open ─▶ Take-over (full screen) ─ Ctrl+] ─▶ back
 ├─ Ctrl+T / footer "Terminal" ─▶ Terminal (hosts) ─ Enter / tap ─▶ SSH session (full screen) ─ Ctrl+] + confirm ─▶ back
 │                                  └─ "+ Add host", or a row's Edit ─▶ Host form
 └─ Ctrl+P / footer "Settings" ─▶ Settings: Host · Network · Power · Keyboard light · Display & time · About
                                     └─ one level of sub-pages (Wi-Fi networks, Peers), pickers, prompts and confirms
```
- **Sessions is the only home.** There is no launcher, and Esc never leaves Sessions.
- **Recovery without a Host.** Settings › Network works with no Host set and no tailnet. The
  Sessions screen's no-Wi-Fi and Host-not-found banners link straight to it.
- **Back** works three ways: Esc, swiping right, or the header's "← Sessions" (or "← Terminal" on
  the host form).

### Hotkeys

All of these are handled on the device and never sent to the Host, as Ctrl+P and Ctrl+M are today
(`hk_dashboard`).

| Where | Key | Action |
|---|---|---|
| Sessions | ↑ / ↓ | Move the selection |
| Sessions | Enter | Send the answer. On an empty field, open the session (as today). |
| Sessions | Ctrl+O | Open the selected session |
| Sessions | **Ctrl+T** | Terminal. It replaces Ctrl+M "More". |
| Sessions | Ctrl+M | Terminal: an alias. **Controller call:** keep Ctrl+M as an alias, because the Plan 2 two-thumb rule pairs Ctrl with a key on the right half, and T is on the left (reason: no thumb has to reach across; cost if wrong: one alias line). |
| Sessions | Ctrl+P | Settings |
| Sessions | Ctrl+K | The shortcuts overlay |
| Sessions | **Ctrl+Space** | **Reserved for voice, as a toggle.** In Plan 4 it is swallowed, so no space reaches the field, and it shows the toast "Voice input comes in a later update". Nothing else is built. |
| Settings, Terminal | ↑ ↓ ← → / Enter / Esc | Move focus, choose, back. **Controller call:** these screens get a key sink, so the arrow keys work without pressing Ctrl+Tab first. Ctrl+Tab "buttons mode" stays as an alias (reason: audit §2.6, Settings without touch is awkward today; cost if wrong: one routing path). |
| Take-over, SSH session | Ctrl+] (alias Ctrl+Q) | Leave. An SSH session asks to confirm first, because leaving disconnects it. |
| Take-over | Shift+↑/↓ | Scroll back (unchanged) |

Inside a terminal, no other key is taken. Ctrl+K and Ctrl+T reach the Host there, because Claude
Code and shells use them.

## 5. Screens

**Shared frame.** Every screen except the two full-screen terminals has:
- a **header**: 56 px, `panel`, with a 1 px `line` underneath;
- an optional **banner**: 48 px;
- a body.

The left side of the header shows the title or the back button. The right side is always the same
vitals cluster, in this order:
1. battery glyph and percentage, with a bolt while charging, red at 15 % or less when not charging;
2. Wi-Fi bars;
3. the Tailscale mark, only while the switch is on;
4. the link dot and the word "Host";
5. the clock.

**Controller call:** every screen shows the full cluster. The Settings mockup shows only the
battery and the clock (reason: the audit keeps one vitals header everywhere, so state never
disappears in Settings; cost if wrong: hiding three glyphs on two screens).

### 5.1 Sessions (home)
**Purpose:** see every session, see which ones need you, answer, and open.

**Layout (Main mockup):**

| Region | Size and content |
|---|---|
| Header | "Sessions". Then "● N need you" in amber, shown only when N > 0, never in any other colour. Then the vitals. |
| Banner | Only when the state isn't live. See the states table below. |
| List (left) | 440 px wide; 72 px rows with 8 px gaps; 7 rows fit before it scrolls. |
| Detail (right) | The rest of the width, with 24 px margins. |
| Footer | 64 px, `panel`, with chips. |

**Session row** (72 px, radius 12):
- Contents:
  - the name, in `body` bold, cut with "…" in the list only;
  - "repo · age" in `label` muted;
  - the state pill on the right.
- Background `panel` and border `line`.
- A NEEDS YOU row has a 4 px amber left edge and a 1 px amber border at 60 %.
- The selected row has `panel-2` and a 2 px `fg` outline at 50 %. A selected NEEDS YOU row keeps
  its amber border at 100 % (as in the mockup).
- **Order:** NEEDS YOU first, then WORKING, IDLE, DONE, ENDED. Within a state, the most recent
  activity comes first.
- **Controller call:** the selection follows the session id, not the list position.
  - When a poll re-sorts the list, the same session stays selected.
  - If the selected session disappears, the row now at its position is selected.
  - Reason: this is the wrong-session anti-goal. Cost if wrong: none; it is strictly safer.

**Detail pane:**
1. **Title row:**
   - the full session name, wrapped and never cut;
   - the path and age, muted;
   - the state pill.
2. **Log block** (`panel`, border `line`, radius 12):
   - the tail of the log in `mono` 24/32. At the pane's width that is about 62 columns × 10
     lines;
   - long lines wrap, with a 2-cell hanging indent, and the newest rows that fit are shown.
   - **Controller call:** wrap instead of clip. Today the tail is cut to 96 columns (reason: the
     end of a question is the part you answer; cost if wrong: fewer distinct lines).
3. **Answer row:**
   - the answer field: 64 px, `panel-2`, radius 12, with a 2 px `fg` border while focused;
   - the mic slot, 64 × 64, at its right.
4. **Action row:**
   - **↵ Send** (primary) and **⌃O Open** (secondary), both 64 px;
   - a muted note on the right. It shows "1–3 picks an option" when the tail ends in a numbered
     list, or "Sent ✓" in green for 3 s after a send.
   - **Controller call:** a digit is typed into the field and needs Enter. Digits are never
     single-key picks (reason: a stray or waking key must never answer; cost if wrong: one Enter
     press).

**Interactive sessions.** These are sessions that aren't running in the background. Only
background sessions can be taken over (D2), so the detail pane shows the log and the line "This
session runs in a terminal on the host. Type /bg there to answer or open it from here." There is no
field and no Open.

**Drafts.** **Controller call:** a draft belongs to its session id. Moving the selection keeps it,
and it is cleared after it is sent or when the session goes away. Drafts are kept in PSRAM, up to
16 (reason: checking another card mid-answer shouldn't lose the typing; cost if wrong: a small
map).

**Mic slot (reserved).** A 64 × 64 button with a dashed `line` border and a muted mic glyph, as in
the mockup.
- A tap shows the same toast as Ctrl+Space. It never records.
- The later plan fills in the behaviour the audit designed:
  - on touch, hold to talk;
  - on the keyboard, Ctrl+Space toggles;
  - REC in red, never amber.
- **Controller call:** draw the slot disabled, rather than hiding it (reason: the approved mockup
  shows it, and keeping the field's width fixed now avoids a relayout later; cost if wrong: hide
  one widget).

**Footer chips** (48 px visible, hit area extended to 80, 8 px apart):
- On the left: ↑↓ Select, ↵ Send, ⌃O Open.
- On the right: ⌃T Terminal, ⌃P Settings, ⌃K Keys.
- Every chip does what its key does.
- **Controller call:** tapping "↑↓ Select" moves to the next NEEDS YOU session, or to the next
  session if none needs you (reason: a chip has to do something, and that is the useful jump;
  cost if wrong: one handler).

**Touch:**
- A tap on a row selects it.
- A tap on the already-selected row opens it, if it runs in the background.
- A tap on the field focuses it. The soft keyboard appears only when no physical keyboard is
  present.

**States.** One banner slot. The table is in precedence order, highest first.

| State | Presentation |
|---|---|
| No Wi-Fi | Red banner: "No Wi-Fi", with a [Wi-Fi settings] action. The last cards stay, dimmed to 50 % and inert. |
| No Host set | No banner. The body is centred: "Set up your host to see its Claude sessions." and a primary button, **Set up host**, that goes to Settings › Host. |
| Host unreachable | Red banner with the reason. Examples: "Host not found at my-host.local", "Host refused the key". It has a [Host settings] action. The cards are dimmed and inert. |
| Low battery | Red banner: "Battery 14 %. The Tab5 sleeps at 10 %." It shows once per discharge below 15 %, and a tap dismisses it. |
| Reconnecting | Muted banner: "Reconnecting to host…". The list is at 50 % and inert. |
| Loading | Muted banner: "Loading sessions…", over 3 skeleton rows in `panel-2`. |
| Stale | Muted banner: "Updating…". |
| Empty (live) | No banner. The body is centred and muted: "No sessions on this host. Start one with `claude --bg`." |
| Error on send | A red note in the detail pane, giving the reason. A toast is used only if the pane isn't visible. |
| Live | No banner. |

**Controller call:** low battery ranks below the offline and error states and above the others
(reason: being offline blocks everything, and low battery is a warning; cost if wrong: the order of
one table).

**Data sources:**
- the session list, the states, the needs-you count and the log tail: the `cc_sessions` poll
  snapshot and its log buffer, fed by the long poll and the watch;
- link state and reasons: `cc_link`;
- answers and opens: through `cc_cmd_*` only;
- vitals: `sys/vitals`;
- the keyboard light: the existing power/vitals owner. It is unchanged.

### 5.2 Take-over (full screen)
- **Purpose:** type into a Claude session. The pty pipeline and scroll-back are unchanged.
- **Palette:** it moves to the tokens: background `bg`, text `fg`, cursor `fg`, with VS Code ANSI
  colours. Claude Code's own coloured output is content and is drawn as sent. The chrome never
  uses amber.
- **Session bar.** It replaces today's "Ctrl+] to leave" toast and the invisible hot zones.
  - 56 px, `panel`, over the top of the terminal.
  - Left to right: **← Leave**, the session name, the link dot, ⌨ (only when no physical keyboard
    is present), then the chips Esc, Tab, ^C and ↑ ↓.
  - It shows for 3 s when the session opens, then hides. A tap in the top 80 px brings it back for
    3 s.
  - **Controller call:** no key brings the bar back, because inside a terminal every key belongs to
    the Host (reason: the Plan 2 rule "only keys Claude Code doesn't use are taken"; cost if wrong:
    one binding).
- **States:**
  - connecting: the bar shows "Connecting…", muted;
  - link lost: the bar stays up with a red "Link lost, reconnecting…";
  - session ended: a toast, then back to Sessions.
- **Data source:** the existing terminal module, `term/`.

### 5.3 Terminal (host list)
- **Purpose:** SSH to a few tailnet or LAN boxes. It replaces More › SSH Terminal.
- **Header:** "← Sessions", "Terminal", then the vitals.
- **Banner:** shown when the Tailscale switch is off and at least one listed host is a tailnet
  address (100.64.0.0/10 or `*.ts.net`). It reads "Tailscale is off. These hosts are reachable only
  over your tailnet." with a primary **Turn on**. Turn on calls `tailscale_set_enabled(1)`: this is
  U7's second home for the switch.
- **Rows** (72 px):
  - an online dot;
  - the name in `body` bold, and `user@host` in `label` muted;
  - "↵ Connect" on the focused row.
  - **Controller call:** the dot is shown only for tailnet hosts while Tailscale is on. Green means
    online in the peer list; `line` grey means offline. A LAN host shows no dot (reason: there is
    no cheap liveness signal for a LAN host, and a ping timer would cost power; cost if wrong: a
    dot that says nothing).
  - Each row has a trailing **Edit** chip (48 px visible, 80 hit), which opens the host form. The
    long-press menu goes.
- **Last row:** "+ Add host" (72 px, dashed). It opens a sheet:
  - "From your tailnet" lists the online peers from `tailscale.c`, if the switch is on;
  - "Enter manually".

  Either one opens the host form, prefilled from the peer if one was chosen.
- **Footnote** (muted): "Your Claude host isn't listed here: its sessions are on the Sessions
  screen. Ctrl+] leaves an SSH session."
- **The Host is never listed.** Connecting to it is refused with the same footnote.
- **States:**
  - empty: just the Add host row and the footnote;
  - connecting: the row shows a spinner;
  - failure: a red note on the row, giving the reason.
- **Data sources:** `server_store`, minus the Host's entry; the `tailscale.c` peer list.
  **Controller call:** the 2 s status timer goes. The list refreshes when the screen is shown and
  on Tailscale peer events (reason: no idle timer; cost if wrong: a dot that is up to one event
  late).

### 5.4 SSH session (full screen)
- It uses the same terminal and the same session bar as §5.2. The bar shows the host name.
- **Leaving:** "← Leave" or Ctrl+] asks "Disconnect from <host>?". Leaving disconnects.
- **What goes:** the 21-key side column, the three invisible 28 px hot zones, the status strip
  with the red X, and `ui_status_text`.
- **Power:** "Stay awake in Terminal" (Power) holds the screen on while this is open, as today.

### 5.5 Host form
Opened from Terminal's Add host or Edit, and from Settings › Host › Address. Rows are 64 px, one
column; the form scrolls if it has to.

1. **Name.** It defaults to user@host.
2. **Address:** a host name or IP. Required.
3. **Port:** 22.
4. **User.** Required.
5. **Sign in with:** a picker of **Device key** (the default) or **Password**. The password row
   shows only for Password; it is masked when typed and when shown.
   - This replaces the "Private key (SD)" switch and the free-text key path.
   - **Controller call:** an existing entry whose key path isn't "device" shows a third, read-only
     choice, "Key file on SD card", until the user picks another (reason: nothing breaks for an old
     entry; cost if wrong: one read-only label).
6. **Host key** (saved hosts with a pin only): the fingerprint in `label` mono, and a destructive
   **Forget** (U2).
   - The confirm reads: "Forget this host's key? Only do this if the host's SSH key changed. The
     Tab5 will trust the next key it sees."
7. **Delete host** (destructive, with a confirm). Not shown for the Host's own entry in Settings.

The header has "← Terminal" (or "← Settings"), a title of "Add host" or "Edit host", and a primary
**Save**. Esc with unsaved changes asks "Discard changes?".

### 5.6 Settings shell
- **Layout (Settings mockup):**
  - header: "← Sessions", "Settings", then the vitals;
  - a rail of 6 sections: 320 px wide, 72 px rows;
  - a panel with a `title` heading and 64 px rows.
- **The rail.** The selected section has `panel-2` and a 50 % `fg` outline. There is no hue.
- **Where Ctrl+P lands:** on Host the first time after a boot, then on the last-used section. This
  is kept in RAM only.
- **The panel.** Only the visible section is built (P54). The panel scrolls when it doesn't fit.
- **Row kinds:**
  - value (label on the left, muted value on the right, tap to edit);
  - switch (the whole row toggles);
  - slider (it saves on release);
  - nav (a "›" at the end; it opens a sub-page or a sheet);
  - action (an action chip on the right).

  The whole row is the hit target.
- **Section headers** use `label` muted, with 24 px above and 8 px below. A footnote under a group
  uses `label` muted.
- **Pickers** are a sheet of 64 px radio rows, with the current value marked by an `fg` dot.
  **Prompts** use the soft keyboard only when no keyboard is present. A secret prompt is masked.

### 5.7 Settings › Host (Settings mockup)

| Row | Kind | Value and behaviour |
|---|---|---|
| Host | nav | The entry's address. Opens a picker: the saved hosts (the list `server_store` holds), "Add host…" (a new host form) and "None". Sets `companion_server`. |
| Address | nav | Opens the host form (§5.5) for the Host's entry, which holds the address, port, user and sign-in. |
| Link | read-only | `cc_link` status: "● connected over home Wi-Fi" in green, or the reason in red. A second line shows the path (`lan <ip>` or `tailnet <ip>`) and the watch state. |
| Fallback address | value | `mac_lan_ip`, through `cc_link_set_lan_fallback`. The placeholder is 192.168.1.50. Empty means none. An invalid value toasts "Use a home network address like 192.168.1.50, or leave it empty". |
| Claude path on host | value | `claude_path`, which must pass `cc_path_valid`. The default is `~/.local/bin/claude`. |
| *Host key* (section) | | |
| SHA256:… fingerprint | action | Shows the whole fingerprint, wrapped, so it can be compared with `ssh-keygen -lf` on the Host. A destructive **Forget** chip confirms first (wording as in §5.5). |
| *This device's key* (section) | | |
| Public key | action | "ssh-ed25519 … claude-companion · Show" (U1). Show opens a sheet with the whole public key line in mono, wrapped, and the note "Add this line to ~/.ssh/authorized_keys on each host this Tab5 signs in to." |

- **No Regenerate in the UI.** It stays console-only (U1).
- **Controller call:** text only, with no QR code (reason: no QR library in the build; cost if
  wrong: a QR component later).
- **Controller call:** no "(macOS)" suffix on the Host row, which the mockup shows (reason: nothing
  detects the OS, and that would need a new Host command; cost if wrong: one label).
- **Footnote:** "The host is one of your SSH hosts. The Tab5 lists its Claude Code sessions and runs
  claude from this path."

### 5.8 Settings › Network (Network mockup)

**Wi-Fi:**

| Row | Kind | Behaviour |
|---|---|---|
| <current network> | nav | "● connected · strong" in green, or "Not connected" muted. Opens a sheet with **Disconnect** (until the next boot, RAM only, as today) and **Forget**, both confirmed. |
| Networks | nav | A sub-page with three parts. **Saved** (tap to connect; a trash chip forgets, with a confirm). **Available**, from a scan: a lock glyph means secured, and a tap asks for the password, masked. **Join other…**: SSID, then password. |
| Advanced | nav | A sub-page with one row, **Static address**: "Off (DHCP)" or "192.168.1.50/24 · gw 192.168.1.1". The prompt format is `address/prefix gateway [dns]`. Empty turns it off. The note reads "Applies from the next join." This is Plan 3's setting, stored in NVS `wifi`/`sip`. |

**Tailscale.** The rows are in U10's order. All of them stay visible while the switch is off, so a
key can be set before turning it on.

| # | Row | Kind | Behaviour |
|---|---|---|---|
| 1 | Tailscale | switch | `tailscale_set_enabled()`: saved at once and sticky. The value reads "off · saved" or "on". The toasts are Plan 3's, reworded per §9. |
| 2 | Status | read-only | The state, then the IP and the DERP region, e.g. "connected · 100.x.y.z · DERP sea · 3 peers online". While the state is TS_ERROR, the last error shows in red. While off: "Off: doesn't run, and check-ins don't wait on it." |
| 3 | Auth key | nav | "•••••••• set" or "Not set" (U6). The sheet has **Enter key** (a masked prompt), **Load from SD card** (reads `/tailscale/authkey.txt` once; it toasts "No SD card" or "No key file") and **Clear key** (destructive, with a confirm; U5). The key is never logged or shown. |
| 4 | Device name | value | `ts_device_name`. An empty name is refused. |
| 5 | Control server | value | `ts_ctrl_host`. "Default (Tailscale)" when blank. The placeholder is `https://headscale.example`. |
| 6 | Connection | picker | **Relay only** (the default) or **Try direct**, from `ts_direct`, with the note "Applies the next time Tailscale starts". |
| 7 | Peers | nav | "3 online ›". A sub-page lists the peers: hostname, IP, online or offline, direct or relay. A Refresh chip sits in the header. Tapping a peer gives **Add as SSH host**, which opens the Terminal host form prefilled. |
| 8 | Forget this device on the tailnet | action, destructive, last | The confirm reads "Removes this device's tailnet identity. It registers again with the saved auth key the next time Tailscale starts." If no key is saved, it adds "You'll need a new auth key." It **keeps the auth key** (U5). |

**Controller call:** one column of 64 px rows that scrolls, instead of the mockup's two-column
grid of 56 px rows (reason: the row-height token is 64 and U10 is a linear order; cost if wrong:
the Network panel needs one scroll to reach Peers and Forget).

U10 overrides the audit's HIDE of the Control server and Connection rows (audit #29). Both are now
in the UI.

### 5.9 Settings › Power
Three groups. Each has a footnote under it. Units go in the values, never in the labels.

| Group | Rows (field) |
|---|---|
| **Screen** | Dim after (`pw_dim_s`, 10 s–10 min) · Screen off after (`pw_off_s`) · Sleep on USB power (`pw_sleep_on_usb`) · Stay awake in Terminal (`ss_disable_in_ssh`) |
| **Wake** | Wake when a session needs you (`pw_wake_needs_you`) |
| **Battery saver** | Radio off (`pw_radio_off`) · Radio off after (`pw_radio_off_min`) · Check in every (`pw_checkin_min`) · Deep idle after (`pw_deep_idle_min`, Never or 15 min–12 h) · Hibernate after (`pw_hibernate_h`, Never or 1–48 h; it reads "not available yet" if `!hibernate_supported()`) · Hibernate check-in every (`pw_hibernate_checkin_min`, Off or 15 min–12 h) |

- The ranges, clamps and defaults are Plan 2's and are unchanged.
- Time zone leaves this section (U4).
- The two needs-you light rows move to Keyboard light.

### 5.10 Settings › Keyboard light

| Row | Kind | Field |
|---|---|---|
| Keyboard | read-only | Connected / USB keyboard / Not connected |
| Colour | picker | **Controller call:** one picker merges `kbd_led_mode` and `kbd_led_rgb`. **Keyboard's own** sets mode 0 (bind). Picking a swatch (Blue, the default; White, Red, Green, Magenta, Cyan) sets mode 1 and the colour. Swatches are drawn, never shown as hex, and there are no amber-like presets (reason: today two rows are both labelled "Keyboard LED"; cost if wrong: two rows again). |
| Brightness | slider | `kbd_led_bright` |
| *Needs you* (section) | | |
| Show needs-you on keyboard | switch | `kbd_needs_light` |
| Brightness when the screen is dark | picker | `kbd_dark_light`: Lowest 2 % / Low 5 % / Medium 15 %. Any other value set from the console shows as "N %". |

**Footnote:** "Amber on the keyboard always means a session needs you."

### 5.11 Settings › Display & time
- **Brightness**, a slider: `brightness`, 5–100, saved on release.
- **Time zone**, a picker: `vitals_tz_presets`. A custom rule set from the console shows as
  "Custom (console)" (U4).
- **Clock**: "2026-10-24 21:20", with a **Sync now** action chip that starts SNTP. SNTP already
  runs on every Wi-Fi connect.

### 5.12 Settings › About
1. **Version:** the app version and build date.
2. **Credits** (read-only, muted): "Based on Clientre 5 by Mozgi512 (MIT)" and "SSH by libssh
   (LGPL-2.1)".
3. **Diagnostics:** collapsed by default (it was Device info). Expanded, it shows:
   - the chip and IDF version;
   - internal free, minimum and largest block;
   - PSRAM free;
   - battery V, mA, W and charging;
   - Wi-Fi RSSI, channel and IP;
   - Tailscale state;
   - the keyboard;
   - uptime;
   - the last reset reason.

   It refreshes every 2 s, only while it is expanded and the screen is on.
4. **Reboot** (with a confirm).
5. **Reset settings** (destructive, with a confirm; U3).
   - The confirm lists the scope. It resets power, keyboard light and display brightness to
     their defaults.
   - It keeps Wi-Fi networks, the Host and the Terminal hosts, the host-key pins, the Tailscale
     identity and auth key, and the device key.
   - **Controller call:** it also keeps the time zone (reason: the time zone records where the user
     is, like Wi-Fi; cost if wrong: one field in the scope).

"Check for update" returns here in Plan 7.

### 5.13 Shared system UI
- **Boot splash:** the monitor logo and "Claude Companion" in `fg`, and "Starting…" muted, on `bg`.
  No amber.
- **Sleep notice:** kept and restyled. "Sleeping to save battery. Press the side button to wake."
- **Ctrl+K overlay:** a sheet listing the hotkeys in §4, in two columns that never wrap. Any key or
  touch closes it.
- **Toasts:** one line at the bottom centre, `panel`, `fg`, 3 s.
- **Confirm:** a title (`body` bold, wrapped), a body, then **Cancel** (secondary) and the action
  (primary, or destructive in `red`).
- **Busy:** a spinner and a muted label.
- **Soft keyboard:** restyled to the tokens. It is only a fallback, for when no physical keyboard is
  present, e.g. a Wi-Fi password with the keyboard detached.
- **Focus:** a 2 px `fg` ring with a 2 px gap, on every screen. In Settings and Terminal, a muted
  hint "↑↓ move · Enter choose · Esc back" stays in the footer area while the keyboard is in use.
  It replaces the 2 s "UI: Tab / Enter / Esc" toast.
- **Motion:** one 120 ms fade or slide between screens. No phosphor sweep, no scan lines, no glow,
  no shadows.

## 6. Settings map (all 101)

**Disposition key:**
- **UI**: it has a home in the UI.
- **Console**: console-only.
- **Removed**: it has no UI. The sub-reason says why:
  - *internal*: never a user setting;
  - *reserved*: the blob field is kept but unused (U8);
  - *gone*: the feature is removed.

| # | Setting (today) | Disposition | Home or status | Reason |
|---|---|---|---|---|
| 1 | Mac (which server is the Host) | UI | Host › Host | the core link (U11 renames it) |
| 2 | Host's entry: address, port, user, sign-in | UI | Host › Address → host form | today it can only be reached through Servers; the Host isn't listed in Terminal |
| 3 | Mac fallback address (`mac_lan_ip`) | UI | Host › Fallback address | Plan 3's LAN fallback |
| 4 | Claude path | UI | Host › Claude path on host | it is specific to the host (U11) |
| 5 | Forget Mac host key | UI | Host › Host key › Forget | a recovery action |
| 6 | Pinned key fingerprint | UI | Host › Host key (read-only) | to compare with the host |
| 7 | Mac link path (lan or tailnet) | UI | Host › Link | shows the link state |
| 8 | `mac_ts_ip` | Removed (internal) | — | bookkeeping for carrying the pin over; the field stays |
| 9 | Carry source (NVS `cc`/`carry`) | Removed (internal) | — | internal state |
| 10 | `companion watch on\|off` | Console | — | a test knob; on is the right default |
| 11 | `companion kick on\|off` | Console | — | a measurement toggle |
| 12 | `companion resolve/status/reconnect-log/hostkey corrupt/hostcache` | Console | — | diagnostics; the status text also feeds Host › Link |
| 13 | This device's SSH public key | UI | Host › This device's key › Show | U1. Regenerate stays console-only |
| 14 | Wi-Fi status | UI | Network › Wi-Fi › current network | recovery |
| 15 | Saved networks | UI | Network › Wi-Fi › Networks › Saved | recovery |
| 16 | Last connected SSID | Removed (internal) | — | used automatically by `wifi_mgr` |
| 17 | Scan and connect | UI | Network › Wi-Fi › Networks › Available | recovery |
| 18 | Manual SSID | UI | Network › Wi-Fi › Networks › Join other | hidden networks |
| 19 | Disconnect | UI | Network › Wi-Fi › current network › Disconnect | an action, RAM only, as today; the home-tile long-press goes |
| 20 | Forget saved network | UI | Networks › Saved (trash chip) | an action on each row |
| 21 | Tailscale switch (`ts_enable`) | UI | Network › Tailscale › Tailscale, plus Terminal's Turn on | U7. The console `ts` stays. The tile long-press and backup restore go |
| 22 | Tailscale status and last error | UI | Network › Tailscale › Status | U10 |
| 23 | Tailnet IP | UI | Status row | U10 |
| 24 | DERP home region | UI | Status row | U10 |
| 25 | Auth key | UI | Network › Tailscale › Auth key (masked) | U6: masked, never logged |
| 26 | Load key from SD | UI | Auth key sheet › Load from SD card | U6. **Auto-loading at boot is removed** |
| 27 | Device name | UI | Network › Tailscale › Device name | U10 |
| 28 | Control server | UI | Network › Tailscale › Control server | U10 (overrides audit #29 HIDE) |
| 29 | Direct connections (`ts_direct`) | UI | Network › Tailscale › Connection | U10. It applies the next time Tailscale starts |
| 30 | Forget this node | UI | Network › Tailscale › Forget… (last) | U5: it keeps the auth key |
| 31 | Peers list | UI | Network › Tailscale › Peers | U10 |
| 32 | Add a peer as an SSH host | UI | Peers › Add as SSH host; Terminal › Add host › From your tailnet | one flow, two entry points |
| 33 | Refresh peers | UI | The Peers sub-page header | an action |
| 34 | MicroLink NVS (keys, peer cache, config) | Removed (internal) | — | reset only through #30 |
| 35 | `ts term-test <host>` | Console | — | a test |
| 36 | Dim after | UI | Power › Screen | Plan 2 |
| 37 | Screen off after | UI | Power › Screen | Plan 2 |
| 38 | Sleep on USB power | UI | Power › Screen | Plan 2 |
| 39 | Wake when a session needs you | UI | Power › Wake | Plan 2 |
| 40 | Stay awake in SSH Terminal | UI | Power › Screen › Stay awake in Terminal | renamed |
| 41 | Radio off | UI | Power › Battery saver | Plan 2 |
| 42 | Radio off after | UI | Power › Battery saver | Plan 2 |
| 43 | Check in every | UI | Power › Battery saver | Plan 2 |
| 44 | Deep idle after | UI | Power › Battery saver | Plan 2 |
| 45 | Hibernate after | UI | Power › Battery saver | Plan 2 |
| 46 | Hibernate check-in every | UI | Power › Battery saver | Plan 2 |
| 47 | `pw_light_sleep` | Removed (reserved) | — | U8: gated and never shipped |
| 48 | `pw_wake_lift` | Removed (reserved) | — | dropped in PT15 |
| 49 | `pw_hibernate_checkin_h` | Removed (reserved) | — | replaced by `_min` |
| 50 | `power set defaults` | Console | — | **Controller call:** About › Reset settings (U3) covers this in the UI, so a second Power reset row would be a duplicate (cost if wrong: one row) |
| 51 | Power log, bench and sim tools | Console | — | engineering diagnostics. About › Diagnostics shows live values only |
| 52 | Keyboard LED mode (bind or user) | UI | Keyboard light › Colour ("Keyboard's own") | merged with #54 (§5.10) |
| 53 | LED brightness | UI | Keyboard light › Brightness | |
| 54 | Keyboard LED colour | UI | Keyboard light › Colour (swatches) | no amber presets |
| 55 | Keyboard light shows needs-you | UI | Keyboard light › Needs you | it was misfiled in Power |
| 56 | Needs-you light when dark | UI | Keyboard light › Needs you | other percentages stay console-only |
| 57 | Keyboard layout (Stock or JIS) | Console | — | audit HIDE #17. **Controller call:** add `key layout stock\|jis`, because no console setter exists today (cost: a few lines) |
| 58 | Keyboard status | UI | Keyboard light › Keyboard (read-only); Diagnostics | |
| 59 | Key log and injection | Console | — | test tooling |
| 60 | `term_font` | Removed (reserved) | — | U8 |
| 61 | Brightness | UI | Display & time › Brightness | |
| 62 | Screen rotation | Removed (gone) | — | landscape only; pinned to 1, and the field stays |
| 63 | Theme preset | Removed (gone) | — | one fixed token set; the field is reserved |
| 64 | Corner rounding | Removed (gone) | — | tokens |
| 65 | Spacing (density) | Removed (gone) | — | tokens |
| 66 | Tint home tiles | Removed (gone) | — | the launcher is gone |
| 67 | Glow | Removed (gone) | — | tokens; it also costs power to render |
| 68 | Scan lines | Removed (gone) | — | tokens; it also costs a redraw |
| 69 | Theme colours ×8 | Removed (gone) | — | tokens |
| 70 | Terminal foreground, background and cursor | Removed (gone) | — | fixed values; the migration writes the token values into the slots |
| 71 | Cursor shape | Removed (gone) | — | fixed: block |
| 72 | Cursor blink | Removed (gone) | — | fixed: off (B1: no idle redraw) |
| 73 | Line height | Removed (gone) | — | fixed: 32 |
| 74 | ANSI palette ×16 | Removed (gone) | — | fixed: VS Code ANSI |
| 75 | Reset to preset | Removed (gone) | — | no presets |
| 76 | Status-bar items ×8 | Removed (gone) | — | one fixed header; `status_items` is reserved |
| 77 | Time zone | UI | Display & time › Time zone | U4. Custom rules stay console-only (`clock tz`) |
| 78 | Sync time (NTP) | UI | Display & time › Clock › Sync now | |
| 79 | `utc_offset_min` | Removed (reserved) | — | replaced by `tz_rule` |
| 80 | Volume | Removed (gone from the UI) | — | **Controller call:** nothing plays sound once Music goes, so there is no row. The field stays live, not reserved, for voice feedback, and U9's save-on-change rule binds whichever setter comes back (cost if wrong: one row) |
| 81 | Screenshot note | Removed (gone) | — | the SD screenshot is gone |
| 82 | Version | UI | About › Version | |
| 83 | Device info | UI | About › Diagnostics | it was a launcher tile |
| 84 | Reboot | UI | About › Reboot | |
| 85 | Update URL (`ota_url`) | Console | — | OTA is hidden until Plan 7. **Controller call:** add `ota url <url>`. The value is masked in logs if it has a query string (cost: a few lines) |
| 86 | Firmware tile and OTA | Removed (gone from the UI) | — | the screen goes; `ota.c` and `sd.c` stay for Plan 7 |
| 87 | Backup export and import | Removed (gone) | — | it needs a card and isn't recovery; plain exports held secrets |
| 88 | Keep SSH hosts on the TF card | Removed (gone) | — | NVS is the only store. Migration in §10 |
| 89 | Re-read hosts from the card | Removed (gone) | — | goes with #88 |
| 90 | Device secret key (NVS `sec`/`devkey`) | Removed (internal) | — | it still encrypts the stored host passwords. Never shown |
| 91 | Diagnostics (vitals, power log) | UI | About › Diagnostics | merged |
| 92 | SSH server list | UI | Terminal | renamed "hosts"; the Host is hidden |
| 93 | Add and edit host form | UI | Host form | §5.5 |
| 94 | Host-key trust and per-host pins | UI | Host form › Host key › Forget | U2. The trust prompt on first connect is unchanged. **Controller call:** no "Forget all host keys" row (reason: per-host forgetting is enough, and a mass forget turns on trust-on-first-use everywhere; cost if wrong: one row) |
| 95 | `ssh_bell` | Removed (reserved) | — | U8: a dead field |
| 96 | Tailscale-off message in Terminal | UI | The Terminal banner with Turn on | U7. Reworded (§9) |
| 97 | Home tile long-press shortcuts | Removed (gone) | — | the launcher is gone (U7); one of them was a blind Wi-Fi disconnect |
| 98 | Dashboard hotkeys | UI | The Ctrl+K overlay (fixed, not settings) | §4 |
| 99 | `lang` | Removed (reserved) | — | English only |
| 100 | Reserved `ss_*`, `reserved_*` and similar fields | Removed (reserved) | — | blob layout |
| 101 | Music favourites (NVS `music`/`fav`) | Removed (gone) | — | Music is gone. **Controller call:** the namespace is left orphaned (about 300 B), not erased (reason: stay clear of "never erase NVS"; cost if wrong: 300 B of NVS) |

**Counts:**
- **UI 56**
- **Console 9**
- **Removed 36:** 23 gone, 8 reserved, 5 internal.
- **Total 101.**

**Items the inventory doesn't number.** These arrived after it, or are new in this plan.

| # | Item | Disposition | Home |
|---|---|---|---|
| A1 | Static address (Plan 3, NVS `wifi`/`sip`) | UI | Network › Wi-Fi › Advanced › Static address |
| A2 | Wi-Fi fast-join cache and host-address cache (Plan 3 Task 3) | Removed (internal) and Console | `companion hostcache` |
| A3 | Reset settings (U3) | UI | About › Reset settings |
| A4 | Clear the auth key (U5) | UI | Auth key sheet › Clear key |
| A5 | The mic slot and Ctrl+Space | UI (reserved) | Sessions detail pane; §4 |

## 7. Removals and savings

### 7.1 What goes

Two sources are used:
- **Flash and static internal RAM:** measured with `esp_idf_size` on Plan 3's build map
  (`claude-companion-plan3/firmware/build/claude_companion.map`, 2026-10-24). "Static int"
  counts `.dram0`, `.dram1` and `.iram0`. On the P4, `.dram1` is `sram_high`, which is internal.
- **Tasks and timers:** from the audit.

All the numbers are expectations. The plan re-measures them (§12).

| Audit # | Removal | What is removed | Flash | Static int | Tasks / timers / PSRAM removed |
|---|---|---|---|---|---|
| 9 | More launcher | `ui_home.c`; `HK_D_MORE`; the footer "More"; `HOME_*` strings | 1.9 KB | — | 400 ms status-bar timer |
| 18 | Rotation | the rotation rows; fixed at rotation 1 | ~0 | — | — |
| 19, 20 | Status-bar settings and the fork status bar | `ui_statusbar_attach`, `statusbar_tick`, `ui_status_text`; `SB_*`; `STATUS_*` | ~2 KB (part of `ui_core.c`) | — | a 400 ms timer on every screen that has the bar |
| 21 | Hosts on the TF card | the SD path in `server_store` | <1 KB | — | — |
| 22 | Backup | `ui_backup.c`, `storage/backup.*`, `wifi_mgr_export/import_json` | 2.6 KB | — | — |
| 23–25 | Appearance, theme presets, motion effects | `ui_appearance.c`; presets, glow, scan lines and pixel switch in `ui_theme.c` and `ui_motion.c`; the `TH_*` strings; Montserrat 22 and 30 once unused | 8.2 KB + 70 KB of fonts | 0.2 KB | theme rebuilds; scan-line overlay redraws; glow and shadow rendering |
| 31 | Files | `ui_files.c` | 6.4 KB | **5.1 KB** | `paste` task, 6 KB, during a paste |
| 32, 33 | Image and text viewers | `ui_image.c`, `ui_text.c`, `media/image_loader`, `exif`, `tjpgd`; LVGL LodePNG, BMP, TJPGD, GIF and FS_STDIO; `LV_CACHE_DEF_SIZE` from 4 MB to 0 | 17.1 KB + 40.9 KB | 0.6 KB | a 100 KB name list and decode buffers (PSRAM); the 4 MB image cache cap |
| 34 | Music | `ui_music.c`, `media/audio_player.*`, `audio_player_init()`, the `ui_music_bar_update()` call on every navigation, the mini-player walk in key routing; `espressif/esp_audio_codec` (the BSP `esp_codec_dev` stays, because the mic needs it) | 10.4 KB + 580 KB + 25 KB of codec libraries | 1.0 KB | `audio` task, 16 KB, while playing; a 120 KB track table; a 500 ms timer; the decoder registries at boot |
| 35 | Camera | `ui_camera.c`, `media/camera.*`; `esp_video` dropped from `PRIV_REQUIRES`; the SC202CS Kconfig. The hibernate `camera-rst` rail entry stays | 4.1 KB (+83 KB if the linker drops `esp_video`, ISP and CAM) | 0.5 KB (+4.4 KB, same condition) | `camera` task, 8 KB; 2 × 1.8 MB PSRAM frame buffers; an 80 ms timer |
| 36 | USB disk mode / MSC | `ui_usb.c`; MSC and disk mode in `usb_mgr.c`; `usb_host_msc` and `esp_tinyusb` (yml and CMake); `TINYUSB_MSC` Kconfig. The USB host's **HID** part stays, so a USB keyboard still works | ~50 KB | **5.6 KB** | **the MSC driver task, 4 KB plus its TCB, created at every boot**; the TinyUSB task in disk mode |
| 37 | SD screenshot | `screenshot_take`, `write_bmp`, `poll_cb`, the `screenshot_init` timer. The console `screenshot` stays | ~2 KB | — | **a 60 ms LVGL timer that runs for the whole life of the app** |
| merges | Device info, Wi-Fi and Tailscale screens, OTA screen | the screens fold into Settings or go; their 2 s and 3 s timers go | ~5 KB | 1.9 KB (`ui_ota` 1.0, `ui_wifi`/`ui_tailscale` ~0.9) | 2 s and 3 s timers |

**Totals:**
- **Flash:** about 0.82 MB, or about 0.9 MB if the camera pipeline unlinks, out of an 8.3 MB
  image.
- **Five tasks go.** The MSC driver task is always running. The `audio`, `camera`, `paste` and
  TinyUSB tasks run on demand.
- **Seven timer kinds go.** The 60 ms screenshot poll is the expensive one, because it wakes LVGL 16
  times a second.

**What stays, and why:**
- `esp_driver_jpeg`, for the console screenshot;
- `json`, used by `cc_sessions` and `server_store`;
- `storage/sd.c`, for U6's Load from SD and Plan 7's OTA. **Controller call:** mount it on demand,
  never at boot (reason: no card is inserted, and a mount at boot costs boot time; cost if wrong:
  one mount call);
- the 3 MB CJK font. **Controller call:** keep it (reason: flash isn't the constraint, it costs no
  RAM or power, and the terminal may show CJK text; cost if wrong: 3 MB of flash, revisit in
  Plan 7).

### 7.2 Expected internal-RAM recovery

| Source | Expected |
|---|---|
| Static `.bss`, `.data` and `.iram` of the removed code and components (Files 5.1, TinyUSB/MSC 5.6, OTA UI 1.0, Music + codec 1.0, viewers 0.6, camera 0.5, the rest ~0.4) | ~14.2 KB |
| The camera pipeline (`esp_video`, ISP, CAM), if it unlinks | +4.4 KB |
| The MSC host driver task's stack and TCB, never created | ~4.4 KB |
| The audio decoder registries, never set up | <1 KB (measure) |
| **Total at steady state** | **~18–24 KB** |

**What this means for the 8,116 B minimum.** That is the internal free-heap minimum from L28, with
Tailscale on during a take-over.
- The recovered bytes enlarge the same internal heap, so the expected minimum moves to about
  26–32 KB.
- That is an expectation, not proof. The failure in L28 was `ERR_BUF` in WireGuard sends, which
  depends on the largest free block (31,744 B in L28) as much as on the total.
- **Controller call:** the acceptance target for Tailscale on is an internal minimum of at least
  20 KB, and no `ERR_BUF` or link drop, over a 10-minute take-over (reason: it leaves margin above
  TLS's DMA descriptor needs, see P54; cost if wrong: a further RAM pass, e.g. P108's PSRAM stacks
  for `wifi_rc` and `checkin`, in a follow-up).
- With Tailscale off (the default), the LAN figure of about 129 KB free should rise by the same
  amount.

## 8. Token and component system

### 8.1 Tokens

There is one header, `ui_tokens.h`, and no runtime themes. The `UI_*` theme macros and
`ui_theme.c`'s preset engine are replaced. Every screen draws from the tokens.

| Token | Value | Use |
|---|---|---|
| `bg` | `#0e1116` | Screens; the terminal background |
| `panel` | `#161b22` | Header, footer, banner, session bar, sheets, idle rows |
| `panel-2` | `#1b212b` | Selected rows, fields, secondary buttons, chips, skeletons |
| `line` | `#2a323d` | Hairlines, idle borders, the off switch track, offline dots |
| `fg` | `#e9e7e1` | Text; the primary-button fill (with `bg` text); the focus ring; the on switch |
| `muted` | `#8a94a2` | Secondary text, hints, placeholders, footnotes |
| `amber` | `#f4b545` | **NEEDS YOU only:** the pill (with `bg` text), the row's left edge and border (60 % idle, 100 % selected), the header count, the keyboard light. Nothing else, ever. |
| `green` | `#54d08a` | WORKING, link up, Tailscale up, "Sent ✓", the battery fill while charging |
| `red` | `#ef6f6f` | Errors, unreachable, low battery, destructive buttons (red text and border), REC later |
| `red-bg` | `#ef6f6f` at 12 % | Behind red banners |

- **Enforcing the amber rule.** The amber token is exported only through one accessor,
  `ui_needs_you_color()`. The only things that call it are the NEEDS YOU components (pill, row
  edge, header count) and the keyboard-light owner. A host test greps the UI sources for the hex
  value and for any other use (§12).
- **Spacing:**
  - an 8 px grid, with 24 px screen margins and gaps of 8, 16 and 24;
  - row heights: rows 64, session rows and the rail 72, header and session bar 56, footer 64,
    banner 48;
  - radius 12 on rows, cards, fields and buttons; 8 on chips and pills; full on dots.
- **Touch targets:**
  - in the body (rows, buttons, fields, the mic slot), at least 64 px visible and an 80 px hit
    area, 8 px apart;
  - **Controller call:** controls inside the 56 and 64 px bars (header back button, footer chips,
    banner action, session-bar chips) are 48 px visible, with the hit area extended to 80 px (the
    mockups draw them at 44–48 px, which conflicts with "64 visible"; reason: a 64 px control
    can't fit in a 56 px bar, and growing the bars costs a session row; cost if wrong: the bars grow
    to 80 px and the list shows 6 rows).
- **Focus and selection:**
  - focus is a 2 px `fg` ring with a 2 px gap;
  - selection is `panel-2` with a 50 % `fg` outline;
  - neither uses a hue.

### 8.2 Components
These are built once and reused everywhere. Each is a function plus shared static styles.
- **Bars:** Header (with the vitals cluster), Banner (muted or red, with an optional action),
  Footer chip bar, Session bar.
- **Sessions:** Session row, State pill, Detail pane, Log block, Answer field, Mic slot.
- **Buttons:**
  - primary: `fg` fill with `bg` text;
  - secondary: `panel-2` with a `line` border;
  - destructive: `red` text with a `red` border;
  - chip: 48 px, `panel-2`, radius 8.
- **Settings:** Setting row (value, switch, slider, nav or action), Section header, Footnote.
- **Dialogs:** Picker sheet, Confirm, Prompt (masked for secrets), Toast, Busy.
- **States:** Empty state, Skeleton row.
- **System:** Sleep notice, Boot splash, Ctrl+K overlay.

`ui_button()` defaults to secondary. Today it defaults to `UI_ACCENT`, which is orange. Switches
are drawn with `fg` for on and `line` for off.

### 8.3 Type

There is one family, with these roles:

| Role | Size / line | Use |
|---|---|---|
| `title` | 36 / 44 | Section headings in Settings; screen titles |
| `body` | 24 / 32 | Row labels, session names, field text, buttons |
| `label` | 20 / 28 | Values, meta, pills, chips, footnotes, banners |
| `mono` | 24 / 32, 12 px cell | Log tail, terminal |

**Controller call:** keep the MaruMinya pixel family that the dashboard and terminal already use
(`font_px20`, `font_px24`, `font_px36b`). Drop the pixel/Montserrat switch. The mockups'
JetBrains Mono is a browser stand-in (reason: it is already in flash, its metrics are known and it
adds no flash; cost if wrong: one font-generation task with `gen_fonts.sh` and a re-check of the
cell width). Whether the user wants a different face is in §15.

## 9. Copy rules

1. **Host, never Mac.** This applies to every string the user can see (U11). Code identifiers
   such as `mac_lan_ip` and `companion_server` stay. Host-specific details, such as
   `~/.local/bin/claude`, belong in Host settings and are never assumed in text.
2. **Sentence case.** Plain words, no exclamation marks.
   - The label says what a setting is. The value says its state.
   - Units go in the value, never in the label: "Dim after · 30 s", not "Dim after (s)".
3. **Errors say what happened and what to do next**, with an action where one exists. Examples:
   "Host not found at my-host.local" with [Host settings]; "No Wi-Fi" with [Wi-Fi settings].
4. **State words:**
   - in pills, in caps: NEEDS YOU, WORKING, IDLE, DONE, ENDED;
   - "…" means in progress (Loading…, Reconnecting to host…, Updating…).
5. **Paths:** "Settings › Network › Tailscale"; "›" marks navigation.
6. **Shortcuts:** chips use glyphs (⌃O, ↵, ↑↓) when the font has them, otherwise "Ctrl+O". Prose
   always says "Ctrl+O".
7. **No hex codes, raw keys or internal names** in the UI. One exception: the DERP region is shown
   as "DERP sea", muted.
8. **Secrets:**
   - auth key: "•••••••• set" or "Not set";
   - passwords: always masked.
9. **One source for strings.** **Controller call:** every UI string goes through the existing
   `tr(STR_*)` table. The removed `STR_*` entries are deleted, and the dashboard's inline literals
   move into the table (reason: copy review and the "Mac" check happen in one file; cost if wrong:
   a trivial indirection).
10. **Rewrites from the inventory:**

    | Today | New |
    |---|---|
    | Companion (section), Mac (row) | Host |
    | Claude path | Claude path on host |
    | Forget Mac host key? / "…your Mac's SSH host key changed…" | Forget host key? / "Only do this if the host's SSH key changed. The Tab5 will trust the next key it sees." |
    | Pinned key: … | Host key (the fingerprint row) |
    | Mac fallback address | Fallback address |
    | Mac link: … | Link |
    | "The Mac is one of your SSH servers…" | "The host is one of your SSH hosts. The Tab5 lists its Claude Code sessions and runs claude from this path." |
    | No Mac set up / Set up your Mac in Settings → Companion | No host set up / **Set up host** |
    | Reconnecting to Mac… / Can't load sessions from Mac… | Reconnecting to host… / Can't load sessions from the host… |
    | Tailscale is off. Turn it on in Settings → Network. | "Tailscale is off. These hosts are reachable only over your tailnet." with [Turn on] |
    | Stay awake in SSH Terminal | Stay awake in Terminal |
    | Private key (SD) | Sign in with: Device key |
    | Console: "mac: …", "Mac entry host …", "New Mac key pinned" | host: …, Host entry …, New host key pinned |

## 10. Migration: CFG 23

- **The bump.** `CFG_VERSION` goes from 21 (Plan 3) to **23**. CFG 22 stays reserved for Plan 2's
  Task 10 battery visit.
  - Each step is keyed "if `old_version` < N", so the order in which the two plans land doesn't
    matter.
  - If Task 10 lands after this plan, it takes the next free number (24) and adds its own step.
  - The pure part stays in `settings_migrate.c`, which has host tests.
- **Steps for `old_version` < 23:**
  1. Write the token terminal values into `term_fg`, `term_bg`, `term_cursor` and `term_ansi[16]`;
     set `term_cursor_style` to 0 (block), `term_cursor_blink` to 0 (B1) and `term_line_height` to 32.
     These slots stay in use with fixed values, and nothing edits them.
  2. Set `rotation` to 1.
  3. `creds_on_sd`:
     - If it is 1 and the card can be read, copy the card's host list into NVS once, then set it
       to 0.
     - If it is 1 and there is no card, set it to 0, keep the NVS list, and log
       "hosts on SD not imported: no card". The SD file is left untouched.
     - This step needs ESP-IDF, so it runs in `settings.c`'s post-load step, not in the pure
       function.
  4. Reset any amber-like `kbd_led_rgb` to blue. This already happens on every load and is
     restated here.
- **Fields that become reserved, kept in place and never read:**
  - `theme_preset`, `theme_colors[8]`, `theme_radius`, `theme_density`, `theme_tile_accent`,
    `theme_glow`, `theme_scanlines`;
  - `status_items`;
  - `creds_on_sd`, after step 3;
  - `ssh_bell`, `pw_light_sleep` and `term_font` (U8).

  Each keeps a `reserved` comment saying which plan retired it.
- **Fields that stay live with no UI:** `volume` (for voice later), `kbd_layout` (console),
  `ota_url` (console).
- **No new blob fields.** The last-used Settings section is kept in RAM only.
- **NVS outside the blob:**
  - `music`/`fav` is orphaned, not erased (§6 #101);
  - the export and import of `wifi` goes with Backup;
  - `wifi`/`sip` (Plan 3) is unchanged.
- **Downgrade.** An older build reading a v23 blob takes its existing "newer version" path, as it
  already does for every bump.

## 11. Power and RAM rules for the new UI

- **No LVGL timer runs faster than the Sessions tick** (300 ms), and only while Sessions is shown.
  The other refresh rates:
  - the clock label changes only on the minute;
  - the vitals cluster updates at most once a second;
  - Diagnostics refreshes every 2 s, only while it is expanded.
- **Controller call:** the dashboard's 500 ms forget-key timer, which runs for the whole life of
  the app, folds into the Sessions tick (reason: one fewer always-on timer; cost if wrong: the
  forget job polls at 300 ms only while Sessions is shown). The builder checks that the job doesn't
  need to finish while another screen is shown.
- **With the screen off,** no UI timer fires. Build a preview assertion and an on-device timer
  count for this (§12).
- **No idle redraw on a static screen.** The preview's existing motion check, which asserts that
  idle invalidation stops, extends to every screen. The only animation is the 120 ms transition,
  and spinners only while busy.
- **No shadows, glow, gradients or opacity animations.** They cost render time and power on the
  RGB565 path.
- **The Sessions screen is built once.** Its up to 16 rows are recycled and updated in place on
  each poll.
- **Internal RAM budget.** With 16 sessions shown, Sessions must leave as much internal free heap
  as today's dashboard with 16 cards, or more. Settings with its largest section (Network) open
  must leave at least the 65 KB recorded in P54.
- **Check-in boots** must not build the UI, if Plan 3 Task 3 kept that path. The new screens must
  not add a UI dependency to it.

## 12. Testing

### Host UI preview (`firmware/tools/ui_preview`)
- **Cases:**
  - Remove: the preset cycling, the portrait sizes, the `appearance` and `home` cases, and the
    tile-fit check.
  - Add cases that render with stubs:
    - `sessions` in every state in §5.1: live with needs-you, selected needs-you, working-only,
      loading, stale, reconnecting, no Wi-Fi, Host not found, no Host, empty, interactive session,
      sent, low battery, 16 sessions;
    - `terminal`, with Tailscale off and on, and empty;
    - `hostform`, new and edit-with-pin;
    - `settings 0..5` with `PREVIEW_CLICK` and `PREVIEW_SCROLL`, and every sheet;
    - `ctrlk`, `sleep` and `boot`.
- **Automated checks.** The preview fails when any of these fails:
  1. **Amber pixel scan.** Every render with no NEEDS YOU session has zero pixels within ΔE ≈ 10 of
     `#f4b545`. A render with one has amber only inside the pill, row-edge and header-count
     rectangles.
  2. **Touch targets.** Walk every clickable object. In the body, the visible size is at least 64
     and the extended hit area at least 80. In bars, at least 48 and 80. Neighbours are at least
     8 px apart.
  3. **Fit.** No label overflows its parent. The detail-pane name and the confirm titles wrap and
     never clip.
  4. **Settings.** Clicking each rail entry leaves exactly one visible panel and keeps the same
     screen object (the existing check).
  5. **Idle.** Invalidation stops on every screen once it is drawn (the motion check, extended).
- **Images.** The rendered PNGs are compared by eye with the five mockups. They use stub data only,
  never real sessions.

### Host unit tests (`firmware/test/host`)
- **Migration:** v20, v21 and v22 blobs go to v23 (term values, rotation, the `creds_on_sd` pure
  part). A v23 blob stays unchanged.
- **Sessions model:**
  - the sort order;
  - the selection follows the id across a re-sort and a removal;
  - drafts are kept per id and cleared on send and on removal;
  - the banner precedence table (a pure function from the Wi-Fi, link, poll-age, battery and
    loading inputs to the banner).
- **Hotkeys** (extend `test_hotkeys.c`):
  - Ctrl+T and Ctrl+M go to Terminal; Ctrl+Space is swallowed and never reaches the field;
  - Ctrl+K, Ctrl+T and Ctrl+Space pass through inside a terminal;
  - the waking key is swallowed (the existing rule, now on every screen).
- **Reset settings scope (U3):** a pure function. The kept fields are unchanged and the reset
  fields equal the defaults.
- **Keyboard colour mapping:** picker index ↔ (`kbd_led_mode`, `kbd_led_rgb`). No amber-like value
  can be produced.
- **Copy and rules lint** (a host script, run in the test suite):
  - no user-visible string in the `tr()` table or the UI sources contains "Mac" as a word;
  - the amber hex value appears only in `ui_tokens`;
  - no UI source formats a command line, i.e. every `ssh_*exec` call site goes through
    `cc_cmd_*`.

### On-device check (one session; flash the P4 only, never the C6)
1. **Sizes and boot.**
   - `idf.py size` before and after: record flash and static internal RAM against §7.
   - Boot to live, on USB.
2. **Heap:**
   - Tailscale off: steady internal free and minimum, with Sessions, Settings › Network and a
     take-over.
   - **Tailscale on:** a 10-minute take-over of a `companion-test-*` session. Record the internal
     minimum and the largest block, and check for `ERR_BUF`. The target is in §7.2. Compare with
     L28's 8,116 B.
3. **Timers.** Count the LVGL timers with the screen off: no UI timer fires. Check that the 60 ms
   screenshot poll is gone.
4. **Keys.** Use the console `key` tool for injection; the user's real presses come in the visit.
   - Ctrl+T, Ctrl+M and Ctrl+Space arrive as `cp` with `MOD_CTRL` in both layouts. Ctrl+Space must
     arrive as `cp=' '`.
   - Ctrl+] leaves both terminals.
   - The first key and the first tap after a dim or off are swallowed on Sessions, Settings and in
     a take-over, and nothing reaches the Host.
5. **Screens.** Take a console `screenshot` of every screen using only `companion-test-*` sessions.
   The images stay in scratch storage, out of git.
6. **Flows:**
   - join Wi-Fi from Settings with no Host set;
   - Tailscale on and off from Network and from Terminal's Turn on;
   - Forget node keeps the key and registers again;
   - Forget host key on a Terminal host;
   - Reset settings scope;
   - the device key Show matches `sshkey`.
7. **Power.** On USB: the idle current on Sessions with the screen on, before and after. The
   check-in awake time must not get worse than Plan 3's number.
8. **The user's visit** (batched): real-finger touch targets, two-thumb reach for the footer and
   session bar, and a look at the screens.

## 13. Builder's order of work (suggested, not binding)

1. Tokens and components, with the preview.
2. Removals. Each is its own commit with its size delta, and Music, Camera and USB MSC go first,
   because they carry the RAM.
3. The Sessions master-detail.
4. Settings, in six sections.
5. Terminal and the host form.
6. The session bar for both terminals.
7. The copy pass and the lint.
8. The migration.
9. The device check.

## 14. Controller calls (index)
1. Ctrl+M stays as an alias for Terminal (the two-thumb rule).
2. Settings and Terminal get an arrow-key sink without Ctrl+Tab.
3. The full vitals cluster in every header.
4. The selection follows the session id.
5. The log tail wraps instead of clipping.
6. Digits never act as single-key picks.
7. Drafts are kept per session.
8. The mic slot is drawn, disabled.
9. The "↑↓ Select" chip jumps to the next session that needs you.
10. Low-battery banner precedence.
11. No key brings back the terminal session bar.
12. Terminal online dots only for tailnet hosts with Tailscale on.
13. No 2 s Terminal timer.
14. The "Key file on SD card" read-only sign-in choice.
15. The device key is shown as text with no QR.
16. No OS suffix on the Host row.
17. Network as one scrolling column of 64 px rows.
18. The Keyboard light colour picker merges mode and colour.
19. Reset settings keeps the time zone.
20. No Power reset row (`power set defaults` stays console-only).
21. Add a `key layout` console verb.
22. Add an `ota url` console verb.
23. No Volume row; the field stays live.
24. No "Forget all host keys" row.
25. Music favourites are orphaned, not erased.
26. SD is mounted on demand.
27. The CJK font is kept.
28. The Tailscale-on take-over target: internal minimum ≥ 20 KB.
29. Bar controls are 48 px visible with an 80 px hit area.
30. The MaruMinya pixel family is kept.
31. One `tr()` string table.
32. The forget-key timer folds into the Sessions tick.

## 15. Left to the builder, and open questions

**Left to the builder:**
- File layout:
  - whether Sessions is a rewrite of `ui_dashboard.c` or a new `ui_sessions.c`;
  - where the components live (e.g. `ui_comp.c`);
  - how `ui_core.c` is split.
- Which glyphs the pixel fonts have: ⌃ ↵ ↑↓ ⌨, the mic, the lock. Missing ones are generated with
  `gen_fonts.sh` / `gen_ui_icons.py`, or replaced with text.
- The exact log-tail line count and column width, from the measured font metrics.
- The wording of the footnotes, within §9.
- How the amber pixel scan and the hit-area walk are written in the preview.
- Whether `esp_video`, ISP and CAM actually unlink. Record what happens.
- Whether the forget-key job can fold into the Sessions tick (§11).
- The order of the removal commits. Each must still build and pass the preview.
- Updating the main spec's roadmap table:
  - Plan 3 is now "Local link";
  - this plan folds in the home screen and the design pass;
  - voice is next; camera is last.

**Only the user can answer.** None of these blocks the build. Each has a default the build uses.
1. **Typeface.** The default keeps today's pixel font. A cleaner face, such as the mono face in the
   mockups, is a small follow-up. It is a matter of taste.
2. **The real-finger and two-thumb checks** in the device visit (§12, step 8).

## 16. Builder-check rulings (B1–B6)

A fresh builder read this spec cold and raised the questions below. Each ruling below binds the plan. Each one gives a reason and the cost if it turns out wrong.

- **B1: what the timer ceiling covers.** The §11 ceiling (no LVGL timer faster than 300 ms, only while Sessions is shown) applies to presentation timers: animation, refresh and polling of UI data.
  - Exempt, and unchanged by this plan: the keyboard key drain (`keyboard.c`, 10 ms, which carries the wake-swallow logic) and the pty pump (`ui_terminal.c`, 16 ms). The pty pump may run only while an SSH session or a take-over is live, and it is deleted when that session closes.
  - The header's vitals cluster updates from one shared 30 s timer, on every screen, only while the screen is on.
  - With the screen off, no UI timer fires. LVGL is already paused there.
  - The cursor does not blink. It is fixed off, and the CFG 23 migration sets `term_cursor_blink` to 0. A steady block cursor means no idle redraw.
  - Why: rewriting Plan 2's input path to be event-driven risks the wake-swallow logic for almost no power gain.
  - Cost if wrong: a later pass to make input event-driven.
- **B2: RAM budget conditions.**
  - Every gate is a measured minimum internal free (the `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)` delta over the action), not a total.
  - With Tailscale off: opening Settings › Network leaves a minimum of at least 65 KB, and a take-over leaves at least 40 KB.
  - With Tailscale on, the targets from controller call 28 apply: a take-over minimum of at least 20 KB and no ERR_BUF. Opening Settings › Network leaves at least 20 KB.
  - Screen changes are instant: `lv_screen_load` with the old screen deleted first, and no fade or slide. This replaces §5.13's 120 ms transition.
  - Why: two screens alive at once was P54's failure, and instant changes also save power.
  - Cost if wrong: transitions look less polished. A fade could come back later, with Tailscale off only.
- **B3: interactive sessions** (empty `id`).
  - Row key: `"i:" + name + ":" + started_ms`. Selection and drafts use this key.
  - Detail pane: state, cwd and start time, plus the muted line "Interactive — answer it on the Host". No log fetch, no reply box and no take-over.
  - Amber goes only to what `cc_session_needs_you()` reports, which keeps today's rule. A blocked interactive row shows the muted state text "waiting on Host", with no amber pill or edge.
  - Why: the device can't act on these sessions, and the NEEDS YOU light already follows `cc_session_needs_you()`. A row and an LED that disagree would mislead.
  - Cost if wrong: one rule change in the session model, plus its test.
- **B4: mockups.** The five mockups are committed under `docs/superpowers/specs/mockups/`. They are visual references, opened with `support.js` absent: the layout is readable, but the canvas chrome is missing. The spec text wins wherever the two disagree.
- **B5: sort order.** "Most recent activity" means `started_ms`, newest first, within each state group. The Host JSON gets no new field.
  - Why: no parser or Host change, and fewer re-sorts.
  - Cost if wrong: a field gets added later.
- **B6: SD key-file entries.** Before connecting with a host entry whose key path is on the SD card, mount the card on demand. If the key file is then missing, fail with "Key file not found on SD card". Never fall back to the device key for that entry.
  - Why: today `ssh_client.c:214` quietly signs in with a different key on ENOENT, and an unmounted card would make that the normal case.
  - Cost if wrong: none for safety. A user with a broken key file sees an error instead of a sign-in with the wrong key.
