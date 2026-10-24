# Claude Companion

Claude Companion is firmware for the **M5Stack Tab5** (ESP32-P4) that turns it into a desk or
couch screen for the Claude Code sessions on your computer. We call that computer **the Host**.
The Tab5 signs in to the Host over SSH, by default on your home Wi-Fi. It lists your Claude Code
sessions and glows **amber** only when a session needs you. You can type a quick answer, or open a
background session full screen and take it over with `claude attach`. It uses your existing
Claude Code install and subscription: no API key, no phone, nothing extra installed on the Host.
Tailscale is optional and off by default.

The firmware is a fork of [Clientre 5](https://github.com/Mozgi512/Clientre5) (MIT). It is a
working project, not a finished product: see [Status](#status).

## What you need

- An **M5Stack Tab5**, ideally with its keyboard (touch and an on-screen keyboard work without
  it). A microSD card is **not** needed.
- A **Host**: a computer running **Claude Code**, with an SSH server you can sign in to with a
  key. macOS is the Host OS this has been built and tested against (with Remote Login on).
- A **2.4 GHz Wi-Fi network** that both the Tab5 and the Host are on. The Tab5's radio (an
  ESP32-C6) is 2.4 GHz only.
- A computer with **ESP-IDF v5.5** to build and flash the firmware, and a USB-C cable.
- Optional: a **Tailscale** account, if you also want to reach machines over your tailnet.

## Contents

1. [Build and flash](#1-build-and-flash)
2. [Set up the Host](#2-set-up-the-host)
3. [First boot on the Tab5](#3-first-boot-on-the-tab5)
4. [Daily use](#4-daily-use)
5. [Settings](#5-settings)
6. [Sleep, wake and battery](#6-sleep-wake-and-battery)
7. [Tailscale (optional)](#7-tailscale-optional)
8. [Terminal: other SSH hosts](#8-terminal-other-ssh-hosts)
9. [Serial console](#9-serial-console)
10. [Troubleshooting](#10-troubleshooting)
11. [Development](#11-development)
12. [Security model](#12-security-model)
13. [Status](#status), [Roadmap](#roadmap), [Repository layout](#repository-layout), [License](#license)

---

## 1. Build and flash

### Install ESP-IDF

The firmware is built and tested with **ESP-IDF v5.5.6** (the `release/v5.5` branch). Follow
Espressif's install guide for your OS, or on macOS:

```sh
mkdir -p ~/esp
git clone -b release/v5.5 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32p4
. ~/esp/esp-idf/export.sh        # in every new terminal, before idf.py
```

### Optional: back up the Tab5's current flash

Flashing replaces whatever the Tab5 runs now. To keep a copy of the whole 16 MB flash first:

```sh
python -m esptool --chip esp32p4 -p /dev/cu.usbmodemXXXX read-flash 0x0 0x1000000 tab5-backup.bin
```

To put it back later:

```sh
python -m esptool --chip esp32p4 -p /dev/cu.usbmodemXXXX write-flash 0x0 tab5-backup.bin
```

### Build

```sh
cd firmware
idf.py set-target esp32p4     # once; creates sdkconfig from sdkconfig.defaults
idf.py build
```

The first build downloads the managed components (LVGL, the Tab5 board support package,
esp_hosted and others), so it needs network access.

### Flash and watch the log

Connect the Tab5 with USB-C. On macOS its port shows up as `/dev/cu.usbmodem*`:

```sh
ls /dev/cu.usbmodem*
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

`idf.py flash` writes the bootloader, the partition table and the app; that is all you need. (The
factory-updater partition is used only by the firmware-update path, which is not in use.) Leave
`idf.py monitor` with **Ctrl+]**.

**If the screen stays dark after flashing**, reset the chip once:

```sh
python -m esptool --chip esp32p4 -p /dev/cu.usbmodemXXXX run
```

During development a few flashes needed this extra reset before the app started; later flashes
booted on their own. See [Troubleshooting](#10-troubleshooting).

**Don't flash the ESP32-C6** (the Wi-Fi co-processor). The log shows it reporting firmware
`0.0.0` and a "major version mismatch". That is harmless: scanning and joining work.

---

## 2. Set up the Host

These steps are for a macOS Host.

### Turn on Remote Login

System Settings › General › Sharing › **Remote Login**: on. Allowing only your own user is enough.

### Find where `claude` is installed

The Tab5 never opens a login shell on the Host. Everything it does is one fixed command run over
SSH (see [Security model](#12-security-model)). A non-interactive shell like that doesn't read
`~/.zshrc`, so `claude` is usually **not on its PATH**. The Tab5 therefore runs Claude Code by an
explicit path, the **Claude path on Host** setting. The default is `~/.local/bin/claude`, where
Claude Code's native installer puts it. To check yours, run this in a normal terminal on the Host:

```sh
command -v claude
```

If it prints something else (a Homebrew path, say), you'll enter that path on the Tab5 in step 3.
The path may use only letters, digits and `_ . / ~ -`, up to 63 characters.

### Give the Host a stable address

The Tab5 finds the Host by its name on the local network. On macOS that's the `.local` name shown
in System Settings › General › Sharing, such as `my-host.local`. If that name doesn't always
resolve, also give the Host a fixed address on your router (a DHCP reservation, such as
`192.168.1.50`). You can enter it on the Tab5 as the **Fallback address**.

### Run sessions in the background

The Tab5 can open and answer only **background** sessions: ones you start with `claude --bg` or
dispatch from agent view (`claude agents`). A plain interactive `claude` in a terminal window shows
on the Tab5 read-only, marked **on Host**.

```sh
claude --bg "tidy up the imports in this repo"   # start a background session
claude agents                                     # agent view: see and dispatch sessions
claude attach <id>                                # open one at your desk
```

Finished sessions stay in the list (the Tab5 reads `claude agents --json --all`). Remove the ones
you no longer need, on the Host:

```sh
claude rm <id>
```

If a session ran in its own git worktree, check before you remove it: removing the session can
remove that worktree's directory as well.

### Authorize the Tab5's key

You do this once the Tab5 has made its key (step 3). The Tab5 creates its own ed25519 key on first
use, and the private half never leaves it. The public key line looks like this:

```
ssh-ed25519 AAAAC3Nza... claude-companion@claude-companion
```

Add that line to `~/.ssh/authorized_keys` on the Host. Limiting it to your home network with a
`from=` option is a good idea (use your own subnet). Add `100.64.0.0/10` only if you'll use
Tailscale:

```
from="192.168.1.0/24" ssh-ed25519 AAAAC3Nza... claude-companion@claude-companion
from="192.168.1.0/24,100.64.0.0/10" ssh-ed25519 AAAAC3Nza... claude-companion@claude-companion
```

The directory and file must be private: `chmod 700 ~/.ssh` and `chmod 600 ~/.ssh/authorized_keys`.

`firmware/tools/authorize_device_key.sh "<public key line>"`, run on the Host, does this for you.
It creates `~/.ssh` and the file with the right permissions if needed, refuses a key that is
already there, and appends the line. It writes `from="100.64.0.0/10"`, which allows the Tab5
**over Tailscale only** (it dates from the earlier Tailscale-only design). For the default
home-network setup, edit the line afterwards and add your LAN subnet to `from=`, as above.

---

## 3. First boot on the Tab5

After the "Claude Companion / Starting…" splash, the Tab5 shows **Sessions**, its only home
screen. Esc, a swipe to the right, or the header's "← Sessions" go back one level.

### Join Wi-Fi

Sessions shows a red **No Wi-Fi** banner. Tap **Wi-Fi settings** (or press Ctrl+P for Settings,
then **Network**). Open **Networks**, pick your network under **Available** and type its password.
**Join other…** is for a hidden network. The Tab5 saves the network and rejoins it on every boot.

### Add the Host

On Sessions, tap **Set up Host** (or go to Settings › **Host** › **Host** › **Add host…**). Fill
in the host form:

| Field | What to enter |
|---|---|
| Name | Any name for the entry. It defaults to `user@host`. |
| Address | The Host's local name, such as `my-host.local`, or its IP address. |
| Port | `22` |
| User | Your user name on the Host. |
| Sign in with | **Device key** (the default). The Host link always signs in with the device key. |

Tap **Save**; the new entry becomes the Host. Then, still in Settings › Host:

- **Claude path on Host**: change it if `command -v claude` printed something other than
  `~/.local/bin/claude`.
- **Fallback address**: optional. The Host's fixed LAN address (such as `192.168.1.50`), used
  only when its name doesn't resolve.

### Copy the device's key to the Host

Settings › Host › **This device's key** › **Public key** › **Show** displays the whole public key
line. Copying it by hand is tedious, so the easier way is the USB serial console (see
[Serial console](#9-serial-console)):

```
tab5> sshkey
@@PUBKEY ssh-ed25519 AAAAC3Nza... claude-companion@claude-companion
```

Copy everything after `@@PUBKEY ` and add it to `~/.ssh/authorized_keys` on the Host, as described
in [Authorize the Tab5's key](#authorize-the-tab5s-key).

> **Don't run `sshkey regenerate`** unless you mean to. It replaces the device key, and every host
> that trusted the old one refuses the Tab5 until you authorize the new line.

### Trust the Host's host key

The Tab5 pins each SSH server's host key and refuses a key that changes. On the home network it
does **not** trust an unknown Host key by itself: the first connection is refused, and the
Sessions banner reads "The Host has a new host key. Check and trust it in Settings › Host."
Tap **Host settings** on the banner. Settings › Host › **Host key** shows the fingerprint the Host
presented, with **Trust this key**. Before you trust it, check that fingerprint on the Host:

```sh
ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub
```

If it matches, choose **Trust this key**; the confirm shows the fingerprint again. Choose
**Trust**: the link reconnects and pins that key, and only that key (if the Host presents a
different one, it is refused and shown in its place). The toast reads "New host key pinned:
SHA256:...". From then on Settings › Host › Host key shows the pinned fingerprint, with a
**Forget** button for when the Host's key really changes. A *changed* key is never offered for
trust: see [Troubleshooting](#10-troubleshooting).

The serial console does the same with `companion hostkey forget`, which pins whatever key the Host
presents next, so check the fingerprint first.

When the link is up, Settings › Host › Link reads "● connected over home Wi-Fi" and your sessions
appear on the Sessions screen.

---

## 4. Daily use

### The Sessions screen

The left side lists the Host's sessions: **NEEDS YOU** first, then WORKING, IDLE, DONE and ENDED,
the most recent first within each. The right side shows the selected session: its name, the tail
of its output, an answer field, **↵ Send** and **⌃O Open**. The header shows "● N need you" in
amber when any session needs you, then the vitals: battery, Wi-Fi, the Tailscale mark (only while
Tailscale is on), the Host link dot and the clock.

**Amber means a session needs you, and nothing else.** A NEEDS YOU row has an amber edge, and the
keyboard's light turns amber too. Low battery and errors are red, never amber. "Needs you" means a
live background session with a question on screen (Claude Code reports `status: "waiting"`). A
session that has finished its turn and is only waiting for your next message is IDLE, not NEEDS
YOU, and doesn't light anything.

- **Interactive sessions** (a plain `claude` in a terminal) show a muted **on Host** marker. Their
  detail pane reads "Running in a terminal on the Host. Start sessions with claude --bg to open
  them here." They have no answer field and can't be opened.
- **Stopped and finished sessions** can be opened: Open starts them again with their history, as
  agent view does. Quick answers need a running session.

### Answering and opening

- **Quick answer:** type in the answer field and press **Enter**. The Tab5 attaches to the session
  (`claude attach <id>`), pastes your text into Claude Code's input box, presses Enter and
  detaches. It types only when the session's input box is empty and ready. If the session shows a
  permission dialog or anything else, it doesn't type, and says "Open it to approve" or "Open the
  session to answer" instead. A digit is typed like any other text and needs Enter, so a stray key
  never picks an option.
- **Open (take over):** **Ctrl+O**, **Enter** on an empty field, or a tap on the already selected
  row opens the session full screen. This is Claude Code's own screen over an exec'd `claude
  attach <id>`: never a login shell, and no tmux. Every key goes to Claude Code, Esc and Ctrl+C
  included.
- **Leave a take-over:** **Ctrl+←** (Ctrl and the `<` key), or **← Leave** on the session bar. Ctrl+Q
  also works, and Ctrl+] on a USB keyboard (the Tab5 keyboard can't send it). The session
  keeps running on the Host. The session bar shows for 3 seconds when a session opens; tap the top
  of the screen to bring it back. Its chips send Esc, Tab, ^C, ↑ and ↓, and ⌨ opens the on-screen
  keyboard when no physical keyboard is attached.
- **Scroll back in a take-over:** Shift+↑ / Shift+↓ (or Shift+PgUp / Shift+PgDn), or drag.

**If the session is also attached at your desk,** both screens mirror the same session, and it is
resized to the Tab5's width (about 106 columns). After you leave on the Tab5, the Host terminal
stays at that width until you resize its window (by any amount, even one column).

### Keyboard shortcuts

On the Sessions screen (**Ctrl+K** shows this list on the device):

| Key | Action |
|---|---|
| ↑ / ↓ | Move between sessions |
| Enter | Send the answer; on an empty field, open the session |
| Ctrl+O | Open the selected session |
| Ctrl+T (or Ctrl+M) | Terminal (other SSH hosts) |
| Ctrl+P | Settings |
| Ctrl+K | The shortcuts list |
| Esc | Clear the answer, or go back |
| Ctrl+Space | Reserved for voice input; for now it shows "Voice input comes in a later update" |
| Ctrl+← | Leave a take-over or SSH session (Ctrl+Q too; Ctrl+] on a USB keyboard) |
| Shift+↑ / ↓ | Scroll back in a take-over |

In Settings and Terminal, the arrow keys move the focus, Enter chooses and Esc goes back. Inside a
take-over or SSH session the Tab5 keeps only Ctrl+← / Ctrl+Q / Ctrl+] and the Shift scroll keys; every
other key goes to the Host.

---

## 5. Settings

Ctrl+P (or the footer's **⌃P Settings**) opens Settings. It has six sections:

| Section | What's in it |
|---|---|
| **Host** | Which saved SSH host is the Host, its address and sign-in (the host form), the link status, the fallback address, the Claude path, the pinned host key (with Forget; on first use, the key the Host presented, with Trust this key), and this device's public key (Show). |
| **Network** | Wi-Fi: the current network, Networks (saved, available, Join other…) and Advanced › Static address. Tailscale: the switch, status, auth key, device name, control server, connection mode, peers, and Forget this device on the tailnet. |
| **Power** | Screen: Dim after, Screen off after, Sleep on USB power, Stay awake in Terminal. Wake: Wake when a session needs you. Battery saver: Radio off, Radio off after, Check in every, Deep idle after, Hibernate after, Hibernate check-in every. |
| **Keyboard light** | Whether a keyboard is connected; its colour (Keyboard's own, or a swatch; Blue is the default) and brightness; Show needs-you on keyboard; Brightness when the screen is dark (2, 5 or 15 %). |
| **Display & time** | Screen brightness, time zone, and the clock with Sync now. The clock also syncs each time Wi-Fi connects. |
| **About** | Version, credits, Diagnostics (chip, memory, battery, Wi-Fi, Tailscale, keyboard, uptime, last reset), Reboot, and Reset settings. |

**Reset settings** puts power, the keyboard light and display brightness back to their defaults.
It keeps the Wi-Fi networks, the Host and the Terminal hosts, the host-key pins, the Tailscale
identity and auth key, the device key and the time zone.

---

## 6. Sleep, wake and battery

The Tab5 steps down by itself when nobody is using it. With the default settings:

| After | What happens |
|---|---|
| 30 s idle | The screen dims. |
| 2 min idle | The screen turns off. Wi-Fi and the Host link stay up, so it wakes to fresh sessions. |
| 60 min idle (battery only) | Deep idle: the keyboard light stays off unless a session needs you. |
| 2 h idle, or the battery at 10 % (battery only) | **Hibernate**: the chip goes into deep sleep. Every **15 minutes** it wakes to check in: it joins Wi-Fi, connects to the Host, reads the session list and sleeps again. If a session needs you, the keyboard light turns amber (dimly) and stays on through the sleep. |

- **Waking:** any key or touch wakes a dim or dark screen. **The key or touch that wakes it is
  swallowed**: it never types, sends or opens anything. From Hibernate, press the Tab5's **side
  button**; the screen says so before it sleeps ("Sleeping to save battery. Press the side button
  to wake."). A wake from Hibernate is a fresh boot; on the home network the sessions are live
  again in about 9 seconds.
- **Wake when a session needs you** (on by default): if a session turns NEEDS YOU while the screen
  is off, the screen comes back on, dimmed.
- **Radio off** (off by default) turns Wi-Fi off while the screen is off and checks in at the set
  interval instead. Radio off, Deep idle and Hibernate apply on battery only.
- Nothing dims or sleeps while a take-over is open. **Stay awake in Terminal** keeps the screen on
  during an SSH session.
- Below 15 % a red banner warns: "The Tab5 sleeps at 10 %."

**Battery life** hasn't been measured on the battery yet. Measured on USB, a check-in is awake for
about 10 seconds. From that, the 15-minute check-ins are estimated at roughly 1 Wh a day: an
estimate, not a measurement. You can lengthen or turn off the Hibernate check-in in Settings ›
Power.

---

## 7. Tailscale (optional)

The Tab5 can join your tailnet through [MicroLink](https://github.com/Csontikka/microlink), an
unofficial Tailscale client built into the firmware. It is **off by default**, and the Host link
doesn't need it. Turn it on only to reach machines that are only on your tailnet, or the Host when
you're away from home.

While the switch is off, Tailscale never starts: not at boot, not on a check-in, not on a
reconnect. That saves a lot of battery and memory (a cold Tailscale connect on this board takes
about a minute). The switch is saved at once and kept across reboots and wakes.

### Set it up

1. Create an auth key in the Tailscale admin console (Settings › Keys). A reusable, pre-authorized
   key is easiest.
2. On the Tab5, open Settings › **Network** › **Auth key**. Choose **Enter key** (a masked field),
   or **Load from SD card**, which reads the first line of `/tailscale/authkey.txt` on a microSD
   card once. The key is never shown or logged.
3. Turn on the **Tailscale** switch. **Status** shows the progress, then "connected", the device's
   tailnet address, the DERP region and how many peers are online.

The other rows:

- **Device name**: the node name on your tailnet, `claude-companion` by default.
- **Control server**: leave it at "Default (Tailscale)", or enter a Headscale URL.
- **Connection**: **Relay only** (the default) or **Try direct**, applied the next time Tailscale
  starts. Relay only keeps all traffic on Tailscale's DERP relays. That adds some latency but
  avoids a MicroLink problem where a direct path can silently drop traffic behind a strict NAT.
- **Peers**: the machines on your tailnet. Tap one and choose **Add as SSH host** to open the
  Terminal host form, filled in.
- **Forget this device on the tailnet**: removes the device's tailnet identity. It **keeps the
  auth key**, so the Tab5 registers again with it the next time Tailscale starts. (**Clear key**,
  under Auth key, removes the key itself.)

With the switch off, a connection to a tailnet address (`100.64.0.0/10`) or a `*.ts.net` name fails
at once with "Tailscale is off. Turn it on in Settings › Network › Tailscale." The Terminal screen
also shows a banner with a **Turn on** button when it lists tailnet hosts.

To reach the **Host** over your tailnet, set its address in the host form to its tailnet name or
address, add `100.64.0.0/10` to the key's `from=` in `authorized_keys`, and turn Tailscale on.

### Tailscale SSH targets

A machine running Tailscale SSH (its banner reads `SSH-2.0-Tailscale`) signs the Tab5 in by its
tailnet identity, before any key or password is tried. Tailscale's default SSH rule is
`"action": "check"`, which needs a browser confirmation the Tab5 can't give. Tag the Tab5 and add
an `accept` rule, for example:

```jsonc
"tagOwners": { "tag:tab5": ["autogroup:admin"], "tag:server": ["autogroup:admin"] },
"ssh": [
  { "action": "accept", "src": ["tag:tab5"], "dst": ["tag:server"], "users": ["autogroup:nonroot", "root"] },
  { "action": "check",  "src": ["autogroup:member"], "dst": ["autogroup:self"], "users": ["autogroup:nonroot", "root"] }
]
```

Then apply `tag:tab5` to the Tab5 and `tag:server` to the target on the Machines page. (SSH rules
can't use `autogroup:member` or a user as `dst` when the source is a tag.)

---

## 8. Terminal: other SSH hosts

**Ctrl+T** (or the footer's **⌃T/⌃M Terminal**) opens a list of other machines you can open a
plain SSH shell on, over your LAN or tailnet.

- **+ Add host** › **Enter manually**, or **From your tailnet** (online peers, while Tailscale is
  on), opens the host form: Name, Address, Port, User, and **Sign in with**: **Device key** (the
  default; authorize the same public key line on that machine) or **Password** (masked). An entry
  from an older build that uses a key file on the SD card shows a third, read-only choice, **Key
  file on SD card**. The list holds up to 24 hosts.
- **Enter** (or a tap) connects. **Ctrl+Enter** or the row's **Edit** opens the form.
- **Host keys:** the first connection to a machine asks "New host key. Trust and continue?". If a
  saved key changes, the Tab5 warns: "This host's key changed. That can mean an attack. Continue
  only if you changed it." The host form shows the pinned fingerprint, with **Forget** for a
  machine whose key really changed.
- **Leaving:** Ctrl+← or **← Leave** asks "Disconnect from <host>?", because leaving ends the
  session.
- **The Host is never a Terminal host.** It isn't listed, and the Tab5 refuses to open a shell on
  it: "That's the Host: its sessions are on the Sessions screen." The check is by host key, not
  only by address: a machine that presents any key ever pinned for the Host link is refused, under
  whatever name or address. This keeps the Tab5 from ever starting a login shell on the Host.

---

## 9. Serial console

The Tab5 has a debug console on its USB-C port (USB-Serial-JTAG, prompt `tab5> `). Open it from
`firmware/` with:

```sh
. ~/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbmodemXXXX monitor     # Ctrl+] leaves
```

Type `help` for the full list. The useful commands:

| Command | What it does |
|---|---|
| `help` | Lists every command and its options. |
| `sshkey` | Prints the device's public key line as `@@PUBKEY <line>`. |
| `sshkey regenerate` | ⚠️ **Replaces the device key.** Every host that trusted the old key refuses the Tab5 until you authorize the new one. |
| `companion status` | The Host link: connected or why not, the watch, the pinned host key, the path used (`lan <ip>` or `tailnet <ip>`) and the fallback. |
| `companion host <entry name>` | Picks which saved host is the Host. |
| `companion claude-path <path>` | Sets the Claude path on the Host. |
| `companion server-host <host> [fallback-ip\|none]` | Changes the Host entry's address and the fallback address. |
| `companion resolve <host>` | Shows how a name resolves (literal, MagicDNS, mDNS, DNS or the fallback). |
| `companion hostkey forget` | ⚠️ Forgets the Host's pinned key; the next connection pins whatever key the Host presents. Check the fingerprint first. |
| `sessions` | Lists the sessions from the last poll: state, id, repo, name. |
| `logs <id>` | A session's recent output, without colours. |
| `reply <id> <text>` | ⚠️ Types a quick answer into a running background session, as the answer field does. |
| `wifi status` | Wi-Fi state, network, address, channel and signal. |
| `wifi leave`, `wifi join [ssid]` | Leaves Wi-Fi, or rejoins a saved network with its saved password. (New networks are added on the device.) |
| `wifi static [off \| <ip/prefix> <gateway> [dns]]` | Shows or sets the static address, applied from the next join. |
| `ts`, `ts on`, `ts off` | Tailscale status, or the switch (saved). The auth key only ever shows as "set" or "not set". |
| `power`, `power settings`, `power set defaults` | The power state and settings; put the power settings back to their defaults. |
| `power checkins`, `power resets` | The last check-ins (time, path, duration, energy), and the reset-reason counts. |
| `clock`, `clock tz <rule>` | The time, or set a custom POSIX time-zone rule. |
| `ui home`, `ui settings [network]` | Shows Sessions, or Settings. |
| `screenshot` | Dumps the screen as a JPEG frame. `python3 firmware/tools/screenshot.py --port /dev/cu.usbmodemXXXX --out shot.jpg` saves it (needs `pyserial`; close the monitor first). |
| `reboot` | Restarts the Tab5, closing its SSH sessions first. |

More commands exist for development and measurement (`key`, `power log`, `power bench`,
`ui heapmin`, `ui timers`, `ota url` and others); `help` describes them. `key` injects key presses
as if typed, so it acts on whatever is on screen.

---

## 10. Troubleshooting

**"Host not found at my-host.local".** The Host's name didn't resolve. Check that the Host is awake
and on the same Wi-Fi, and that the name is right (`companion resolve my-host.local` on the console
shows what the Tab5 gets). Set a **Fallback address** in Settings › Host to the Host's fixed LAN
address, so a failed name lookup doesn't stop the link.

**"Can't reach <name>. Is it awake?"** The Host didn't answer. A Host that is asleep doesn't accept SSH.
Check that Remote Login is still on.

**"<name> refused the device key".** The public key line isn't in `~/.ssh/authorized_keys`, or its
`from=` doesn't include the address the Tab5 connects from (for example, a line with only
`100.64.0.0/10` while the Tab5 is on the LAN). Check the file permissions too.

**"The Host has a new host key. Check and trust it in Settings › Host."** Expected on the first
connection over the home network, while no key is pinned: see
[Trust the Host's host key](#trust-the-hosts-host-key). The console's `companion status` shows the
same refusal as "new host key SHA256:… for <address>; check and trust it in Settings › Host".

**"host key changed for …; not connecting".** The Host presented a different key from the pinned
one. If you know why (a reinstall, a new machine), compare the fingerprint with
`ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub`, then use Settings › Host › Host key › **Forget**.

**"Can't load sessions from the Host…".** The link is up but listing sessions keeps failing. The
usual cause is a wrong **Claude path on Host**: compare it with `command -v claude` on the Host.

**Sessions show but can't be opened.** They're interactive sessions (marked **on Host**). Start the
work with `claude --bg` instead, or dispatch it from `claude agents`.

**The Host terminal is narrow after using the Tab5.** Opening a session on the Tab5 while it is
also attached at your desk resizes it to the Tab5's width. Resize the Host terminal window (by any
amount) and it redraws at full width.

**"Session list too large to load".** The list, which includes finished sessions, outgrew the
Tab5's buffer. Remove finished sessions on the Host with `claude rm <id>`.

**"No SD card".** A card is needed only for Load from SD card (the Tailscale auth key), Terminal
hosts that use a key file on the card, and the one-time import of hosts saved on the card by an
older Clientre 5 build. Everything else works without one. The card is mounted only when one of
those needs it.

**The Tab5 doesn't start after flashing.** Run
`python -m esptool --chip esp32p4 -p /dev/cu.usbmodemXXXX run` to reset it, then open the monitor.
If the first ROM line says `boot:0x...(DOWNLOAD...)` or `waiting for download`, the chip stayed in
download mode: reset it again. A normal ROM line followed by no app output means the firmware hung:
keep the log and open an issue.

**Tailscale is slow.** The first connect after a boot can take about a minute, and the first
connection to a peer 5 to 30 seconds. Relay only also adds some latency.

---

## 11. Development

Firmware internals (source layout, Tailscale internals, fonts, release images and licences) are
in [firmware/README.md](firmware/README.md). The design specs, plans and rulings are in
[docs/superpowers/](docs/superpowers/).

### Host unit tests

The pure modules (session parsing, command builders, the watch-loop parser, host-key rules, power
and hibernate rules, settings migration, the Sessions model, i18n and more) build and run on your
computer with Unity and CTest. They take cJSON and Unity from ESP-IDF (`$IDF_PATH`, or
`~/esp/esp-idf`):

```sh
export IDF_PATH=~/esp/esp-idf
cmake -S firmware/test/host -B firmware/test/host/build
cmake --build firmware/test/host/build
ctest --test-dir firmware/test/host/build --output-on-failure
```

The CTest run includes the UI lint.

### Python tool tests

```sh
cd firmware
python3 -m unittest discover -s tools -p 'test_*.py'
```

Some tests are skipped when an optional dependency is missing.

### UI lint

```sh
python3 firmware/tools/ui_lint.py firmware/main
```

It keeps amber for NEEDS YOU only, every command line for the Host in `cc_cmd`, "Host" as the only
name for the computer running Claude Code in everything the user sees, and every UI string in the one `tr()` table
(`firmware/main/i18n/i18n.h`).

### UI preview

A headless renderer for macOS compiles the real LVGL screens at 1280 × 720 against stub data and
checks them: amber use, touch-target sizes, text fit, idle redraws and timers. It uses the managed
LVGL component, so run `idf.py build` once first. From the repository root:

```sh
PV=firmware/tools/ui_preview/build
cmake -S firmware/tools/ui_preview -B "$PV" -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build "$PV" -j 8
firmware/tools/ui_preview/run_all.sh "$PV"     # every case; PNGs in $PV/png
```

Use a Debug build: a Release build of the preview crashes in one case. The cases are listed in
[firmware/tools/ui_preview/README.md](firmware/tools/ui_preview/README.md).

---

## 12. Security model

- **The SSH key stays on the device.** The Tab5 makes its own ed25519 key and keeps it in flash
  (NVS). Only the public line is ever shown or printed. NVS encryption is not enabled, so someone
  with the device and a USB cable could read the key out; the `from=` limit in `authorized_keys`
  narrows where it can be used.
- **Every Host command comes from a fixed builder.** Each command line the Tab5 sends to the Host
  is built by a `cc_cmd_*` function from validated parts only: the Claude path
  (`[A-Za-z0-9_./~-]`, 1 to 63 characters) and session ids checked against a strict pattern. Your
  answer text is never part of a command line. It reaches Claude Code as pasted keystrokes on a
  `claude attach` session, with control characters removed, so no shell ever parses it.
- **No login shell on the Host.** The Host link runs only exec channels: `claude agents --json
  --all`, `claude logs <id>`, `claude attach <id>`, and the session watch. The Terminal screen
  refuses to open a shell on the Host, which it recognizes by host key.
- **Nothing is left running on the Host.** There is no daemon or helper. The session watch is a
  short shell loop inside the Tab5's own SSH channel. It runs `claude agents --json --all` and
  sends only changes, and it exits when the channel closes, when the Tab5 goes quiet for a minute,
  or after 30 minutes. The Tab5 closes its SSH sessions before Wi-Fi goes down, before it sleeps,
  and before it restarts.
- **Host keys are pinned.** The Host link never trusts an unknown key on the home network without
  you (see [first boot](#trust-the-hosts-host-key)): Trust this key pins only the fingerprint you
  confirmed, and only while no key is pinned. It refuses a changed key. Terminal hosts ask
  before trusting a new key and warn on a changed one.
- **No secrets in logs.** The Tailscale auth key is never logged or shown, and the console's key
  log masks typed characters.

---

## Status

Tested on one Tab5 (the board support package reports board version 2) with a macOS Host running
Claude Code 2.1.309, on a home network, with Tailscale both off and on. Not yet tried on the
hardware with this build: a Terminal session to another machine, the microSD card paths (Load from
SD card, key files, the host import), Forget this device on the tailnet, a USB keyboard, and
battery life on the battery. The firmware-update (OTA) code is still in the tree, but it has no
screen and isn't used.

## Roadmap

| Plan | What it delivers | State |
|---|---|---|
| 1 Session control | Debug console, trimmed fork, session dashboard, quick answers, take-over | done |
| 2 Power & vitals | Dim, screen off, deep idle, Hibernate with check-ins, wake handling, vitals bar | done |
| 3 Local link | The Host link over the home LAN; Tailscale becomes an off-by-default switch; fast check-ins | done |
| 4 UI overhaul | Sessions home, six-section Settings, Terminal hosts; music, camera, USB drive, files and the launcher removed | done |
| 5 Voice input | Dictate answers with the mic: the Tab5 records, sends the audio over SSH to a local transcriber on the Host (whisper.cpp), and puts the text in the answer field | next |
| 6 Camera | Take a photo, copy it to the Host, and ask the open session about it | last |
| 7 Release | Final name, public release, firmware updates | later |

The full table is in the [design spec](docs/superpowers/specs/2026-10-22-claude-tab5-design.md).

## Repository layout

```
firmware/   the Tab5 firmware (ESP-IDF + LVGL); internals in firmware/README.md
docs/       design specs, plans and notes (docs/superpowers/); PROTOCOL.md is the legacy bridge's
bridge/     legacy: an optional HTTP bridge from an earlier design; the firmware doesn't use it
web/        legacy: the bridge's browser dashboard
```

## License

MIT; see [LICENSE](LICENSE).

Based on Clientre 5 by Mozgi512 (MIT)

The upstream licence is kept in [firmware/LICENSE.clientre5](firmware/LICENSE.clientre5). The
firmware includes libssh, which is LGPL-2.1 (`firmware/components/libssh`, full source included).
The other bundled components and fonts are listed in
[firmware/README.md](firmware/README.md#licenses).
