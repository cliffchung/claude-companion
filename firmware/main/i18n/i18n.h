#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LANG_EN = 0,
    LANG_ZH_CN,
    LANG_ZH_TW,
    LANG_JA,
    LANG_COUNT,
    LANG_UNSET = 0xFF,
} lang_t;

/* X(id, en): English only */
#define STR_TABLE(X) \
X(APP_TITLE, "Claude Companion") \
X(OK, "OK") \
X(CANCEL, "Cancel") \
X(DELETE, "Delete") \
X(EDIT, "Edit") \
X(CONNECT, "Connect") \
X(DISCONNECT, "Disconnect") \
X(PLEASE_WAIT, "Please wait\xe2\x80\xa6") \
X(OFF, "Off") \
X(NONE, "None") \
X(UNKNOWN, "Unknown") \
X(SSH_CONN_FAILED, "The session didn't open.") \
X(SSH_AUTH_FAILED, "Authentication failed:") \
X(SSH_HOSTKEY_NEW, "New host key. Trust and continue?") \
X(SSH_HOSTKEY_CHANGED, "This host's key changed. That can mean an attack. Continue only if you changed it.") \
X(KEYBOARD, "Keyboard") \
X(SETTINGS, "Settings") \
X(SET_SYSTEM, "System") \
X(SET_NETWORK, "Network") \
X(SET_POWER, "Power") \
X(SET_BRIGHTNESS, "Brightness") \
X(SET_KBD_NEEDS_LIGHT, "Show needs-you on keyboard") \
X(PWR_USB_C, "USB-C") \
X(PWR_USB_C_CHARGE, "USB-C + battery charging") \
X(PWR_USB_C_STANDBY, "USB-C + battery standby") \
X(PWR_BATTERY, "Battery") \
X(REBOOT, "Reboot") \
X(REBOOT_Q, "Reboot now?") \
X(ABOUT, "About") \
X(SET_HOST, "Host") \
X(SET_KBD_LIGHT, "Keyboard light") \
X(SET_DISPLAY_TIME, "Display & time") \
X(SET_BACK_SESSIONS, "Sessions") \
X(SET_KEY_HINT, "\xe2\x86\x91\xe2\x86\x93 move \xc2\xb7 Enter choose \xc2\xb7 Esc back") \
X(SET_ADD_HOST, "Add host\xe2\x80\xa6") \
X(SET_NOT_SET, "Not set") \
X(SET_ADDRESS, "Address") \
X(SET_LINK, "Link") \
X(SET_LINK_UP_LAN, "connected over home Wi-Fi") \
X(SET_LINK_UP_TS, "connected over Tailscale") \
X(SET_FALLBACK, "Fallback address") \
X(SET_FALLBACK_PH, "192.168.1.50") \
X(SET_FALLBACK_BAD, "Use a home network address like 192.168.1.50, or leave it empty") \
X(SET_CLAUDE_PATH, "Claude path on Host") \
X(SET_CLAUDE_PATH_PH, "~/.local/bin/claude") \
X(SET_CLAUDE_PATH_BAD, "Use only letters, digits and _ . / ~ - (1 to 63 characters)") \
X(SET_WHISPER_PATH, "Whisper path on Host") \
X(SET_WHISPER_PATH_PH, "/opt/homebrew/bin/whisper-cli") \
X(SET_WHISPER_MODEL, "Whisper model on Host") \
X(SET_WHISPER_MODEL_PH, "~/.local/share/whisper/ggml-base.en.bin") \
X(SET_VOICE, "Voice input") \
X(SET_VOICE_CHECK, "Check") \
X(SET_VOICE_READY, "Voice input is ready on the Host") \
X(SET_HOST_KEY, "Host key") \
X(SET_NO_PIN, "No key pinned yet") \
X(SET_NEW_KEY_SEEN, "The key the Host presented. It isn't trusted until you check it on the Host and trust it.") \
X(SET_TRUST, "Trust this key") \
X(SET_TRUST_Q, "Trust this key?") \
X(SET_TRUST_BODY, "%s\n\nOn the Host, run ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub and compare. Trust it only if they match.") \
X(SET_TRUST_ACTION, "Trust") \
X(SET_TRUST_STALE, "Not trusted: the Host now presents a different key. Check it again.") \
X(SET_FORGET, "Forget") \
X(SET_FORGET_Q, "Forget the Host's key?") \
X(SET_FORGET_BODY, "Only do this if the Host's SSH key changed. The Tab5 will trust the next key it sees.") \
X(SET_BUSY, "Busy, try again") \
X(SET_DEVICE_KEY, "This device's key") \
X(SET_PUBLIC_KEY, "Public key") \
X(SET_SHOW, "Show") \
X(SET_HIDE, "Hide") \
X(SET_KEY_UNREADABLE, "The device key can't be read") \
X(SET_DEVICE_KEY_NOTE, "Add this line to ~/.ssh/authorized_keys on each host this Tab5 signs in to.") \
X(SET_HOST_NOTE, "The Host is one of your SSH hosts. The Tab5 lists its Claude Code sessions, runs claude from this path, and runs whisper-cli for voice input.") \
X(SET_PW_SCREEN, "Screen") \
X(SET_PW_DIM, "Dim after") \
X(SET_PW_OFF, "Screen off after") \
X(SET_PW_SLEEP_USB, "Sleep on USB power") \
X(SET_PW_SSH_AWAKE, "Stay awake in Terminal") \
X(SET_PW_SCREEN_NOTE, "Any key or touch wakes the screen. The key or touch that wakes it is not passed on.") \
X(SET_PW_WAKE, "Wake") \
X(SET_PW_WAKE_NEEDS, "Wake when a session needs you") \
X(SET_PW_WAKE_NOTE, "A session that needs you turns the screen back on.") \
X(SET_PW_SAVER, "Battery saver") \
X(SET_PW_RADIO_OFF, "Radio off") \
X(SET_PW_RADIO_AFTER, "Radio off after") \
X(SET_PW_CHECKIN, "Check in every") \
X(SET_PW_DEEP, "Deep idle after") \
X(SET_PW_HIB, "Hibernate after") \
X(SET_PW_HIB_CHECKIN, "Hibernate check-in every") \
X(SET_PW_SAVER_NOTE, "Radio off, Deep idle and Hibernate apply on battery only. With Radio off the Tab5 turns Wi-Fi off while the screen is off and checks in with your Host at the set interval; Hibernate checks in too.") \
X(SET_NEVER, "Never") \
X(SET_NOT_AVAILABLE, "not available yet") \
X(SET_KBD_CONNECTED, "Connected") \
X(SET_KBD_USB, "USB keyboard") \
X(SET_KBD_NONE, "Not connected") \
X(SET_COLOUR, "Colour") \
X(SET_KBD_OWN, "Keyboard's own") \
X(SET_CUSTOM, "Custom") \
X(SET_NEEDS_YOU, "Needs you") \
X(SET_DARK_LIGHT, "Brightness when the screen is dark") \
X(SET_DARK_LOWEST, "Lowest 2 %") \
X(SET_DARK_LOW, "Low 5 %") \
X(SET_DARK_MEDIUM, "Medium 15 %") \
X(SET_KBD_NOTE, "Amber on the keyboard always means a session needs you.") \
X(SET_TZ, "Time zone") \
X(SET_TZ_CUSTOM, "Custom (console)") \
X(SET_CLOCK, "Clock") \
X(SET_SYNC_NOW, "Sync now") \
X(SET_SYNCING, "Syncing the clock\xe2\x80\xa6") \
X(SET_NO_WIFI, "Wi-Fi not connected") \
X(SET_CLOCK_NOTE, "The clock also syncs each time Wi-Fi connects.") \
X(SET_VERSION, "Version") \
X(SET_CREDITS, "Credits") \
X(SET_CREDIT_CLIENTRE, "Based on Clientre 5 by Mozgi512 (MIT)") \
X(SET_CREDIT_LIBSSH, "SSH by libssh (LGPL-2.1)") \
X(SET_DIAGNOSTICS, "Diagnostics") \
X(SET_RESET_SETTINGS, "Reset settings") \
X(SET_RESET_Q, "Reset settings?") \
X(SET_RESET, "Reset") \
X(SET_RESET_BODY, "Power, keyboard light and display brightness go back to their defaults. Wi-Fi networks, the Host and the Terminal hosts, the host-key pins, the Tailscale identity and auth key, and the device key stay. It also keeps the time zone.") \
X(SET_RESET_NOTE, "Reset settings puts power, the keyboard light and brightness back to their defaults.") \
X(SET_RESET_DONE, "Settings reset") \
X(LINK_NO_HOST, "No Host set up") \
X(DIAG_CHIP, "Chip: ESP32-P4 rev %d.%d \xc2\xb7 IDF %s\n") \
X(DIAG_INTERNAL, "Internal: %u KB free, min %u KB, largest %u KB\n") \
X(DIAG_PSRAM, "PSRAM: %u.%u MB free\n") \
X(DIAG_BATTERY, "Battery: %.2f V, %d mA, %.2f W, %s\n") \
X(DIAG_BATTERY_NONE, "Battery: no reading\n") \
X(DIAG_CHARGING, "charging") \
X(DIAG_NOT_CHARGING, "not charging") \
X(DIAG_WIFI, "Wi-Fi: %d dBm, channel %d, %s\n") \
X(DIAG_WIFI_NONE, "Wi-Fi: not connected\n") \
X(DIAG_REST, "Tailscale: %s\nKeyboard: %s\nUptime: %s\nLast reset: %s") \
X(RST_POWERON, "power-on") \
X(RST_EXT, "external pin") \
X(RST_SW, "software") \
X(RST_PANIC, "panic") \
X(RST_INT_WDT, "interrupt watchdog") \
X(RST_TASK_WDT, "task watchdog") \
X(RST_WDT, "watchdog") \
X(RST_DEEPSLEEP, "deep sleep") \
X(RST_BROWNOUT, "brownout") \
X(RST_USB, "USB") \
X(RST_OTHER, "other") \
X(LINK_CANT_REACH, "Can't reach") \
X(LINK_IS_IT_AWAKE, "Is it awake?") \
X(NET_WIFI, "Wi-Fi") \
X(NET_CONNECTED, "\xe2\x97\x8f connected \xc2\xb7 %s") \
X(NET_SIG_STRONG, "strong") \
X(NET_SIG_GOOD, "good") \
X(NET_SIG_FAIR, "fair") \
X(NET_SIG_WEAK, "weak") \
X(NET_NOT_CONNECTED, "Not connected") \
X(NET_NETWORKS, "Networks") \
X(NET_SAVED_N, "%d saved") \
X(NET_ADVANCED, "Advanced") \
X(NET_OTHER_NETWORKS, "Other networks") \
X(NET_DISCONNECT, "Disconnect") \
X(NET_DISCONNECT_Q, "Disconnect from %s?") \
X(NET_DISCONNECT_BODY, "The Tab5 stays off Wi-Fi until the next boot, or until you join a network.") \
X(NET_FORGET, "Forget") \
X(NET_FORGET_Q, "Forget %s?") \
X(NET_FORGET_BODY, "The Tab5 won't join it again until you enter its password.") \
X(NET_FORGOTTEN, "Forgot %s") \
X(NET_SAVED, "Saved") \
X(NET_AVAILABLE, "Available") \
X(NET_SCANNING, "Scanning\xe2\x80\xa6") \
X(NET_SCAN, "Scan") \
X(NET_SCAN_NONE, "No networks found") \
X(NET_SCAN_FAILED, "The scan didn't start. Tap Scan to try again.") \
X(NET_NO_SAVED, "No saved networks") \
X(NET_JOIN_OTHER, "Join other\xe2\x80\xa6") \
X(NET_SSID, "Network name") \
X(NET_PASSWORD_FOR, "Password for %s") \
X(NET_PASSWORD_HINT, "Empty for an open network") \
X(NET_JOINING, "Joining %s\xe2\x80\xa6") \
X(NET_JOINED, "Connected to %s") \
X(NET_JOIN_FAILED, "Couldn't join. Check the password.") \
X(NET_STATIC, "Static address") \
X(NET_STATIC_OFF, "Off (DHCP)") \
X(NET_STATIC_VALUE, "%s/%d \xc2\xb7 gw %s") \
X(NET_STATIC_PROMPT, "Static address (empty turns it off)") \
X(NET_STATIC_NOTE, "Applies from the next join. Use an address your router won't hand to another device: it is used on every Wi-Fi network.") \
X(NET_STATIC_BAD, "Use address/prefix gateway [dns], like 192.168.1.50/24 192.168.1.1") \
X(NET_STATIC_ON, "Static address on from the next join") \
X(NET_STATIC_OFF_DONE, "Static address off: DHCP from the next join") \
X(NET_STATIC_NOT_SAVED, "Static address not changed (not saved)") \
X(NET_STATIC_REFUSED, "Static address refused") \
X(NET_TS, "Tailscale") \
X(NET_TS_OFF_SAVED, "off \xc2\xb7 saved") \
X(NET_TS_ON, "on") \
X(NET_TS_TOAST_CONNECTING, "Tailscale on: connecting") \
X(NET_TS_TOAST_WITH_WIFI, "Tailscale on: starts with Wi-Fi") \
X(NET_TS_TOAST_AFTER_STOP, "Tailscale on: starts when it has finished turning off") \
X(NET_TS_TOAST_TURNING_OFF, "Turning Tailscale off\xe2\x80\xa6") \
X(NET_TS_TOAST_OFF, "Tailscale off: tailnet hosts are out of reach") \
X(NET_TS_STATUS, "Status") \
X(NET_TS_OFF_TEXT, "Off: doesn't run, and check-ins don't wait on it.") \
X(NET_TS_ST_OFF, "off") \
X(NET_TS_ST_WAIT_WIFI, "waiting for Wi-Fi") \
X(NET_TS_ST_CONNECTING, "connecting") \
X(NET_TS_ST_REGISTERING, "registering") \
X(NET_TS_ST_CONNECTED, "connected") \
X(NET_TS_ST_RECONNECTING, "reconnecting") \
X(NET_TS_ST_ERROR, "error") \
X(NET_TS_ST_STOPPING, "turning off") \
X(NET_TS_DERP, "DERP %s") \
X(NET_TS_PEERS_ONLINE, "%d peers online") \
X(NET_TS_PEER_ONLINE, "1 peer online") \
X(NET_TS_AUTH_KEY, "Auth key") \
X(NET_TS_KEY_SET, "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2 set") \
X(NET_TS_KEY_NOT_SET, "Not set") \
X(NET_TS_KEY_ENTER, "Enter key") \
X(NET_TS_KEY_SD, "Load from SD card") \
X(NET_TS_KEY_CLEAR, "Clear key") \
X(NET_TS_KEY_PLACEHOLDER, "tskey-auth-\xe2\x80\xa6") \
X(NET_TS_KEY_SAVED, "Auth key saved") \
X(NET_TS_KEY_EMPTY, "Nothing saved: the key was empty") \
X(NET_TS_KEY_LOADED, "Auth key loaded from the SD card") \
X(NET_TS_NO_SD, "No SD card") \
X(NET_TS_NO_KEY_FILE, "No key file") \
X(NET_TS_KEY_NOT_A_KEY, "The key file doesn't hold a Tailscale auth key") \
X(NET_TS_KEY_TOO_LONG, "The key in the file is too long") \
X(NET_TS_KEY_CLEAR_Q, "Clear the auth key?") \
X(NET_TS_KEY_CLEAR_BODY, "This device stays on the tailnet. To register it again you'll need a new key.") \
X(NET_TS_KEY_CLEAR_ACTION, "Clear") \
X(NET_TS_KEY_CLEARED, "Auth key cleared") \
X(NET_TS_NAME, "Device name") \
X(NET_TS_NAME_DEFAULT, "claude-companion") \
X(NET_TS_NAME_EMPTY, "A device name can't be empty") \
X(NET_TS_APPLIES, "Applies the next time Tailscale starts") \
X(NET_TS_CTRL, "Control server") \
X(NET_TS_CTRL_DEFAULT, "Default (Tailscale)") \
X(NET_TS_CTRL_PLACEHOLDER, "https://headscale.example") \
X(NET_TS_CONN, "Connection") \
X(NET_TS_RELAY, "Relay only") \
X(NET_TS_DIRECT, "Try direct") \
X(NET_TS_PEERS, "Peers") \
X(NET_TS_N_ONLINE, "%d online") \
X(NET_TS_FORGET, "Forget this device on the tailnet") \
X(NET_TS_FORGET_Q, "Forget this device on the tailnet?") \
X(NET_TS_FORGET_BODY, "Removes this device's tailnet identity. It registers again with the saved auth key the next time Tailscale starts.") \
X(NET_TS_FORGET_BODY_NO_KEY, "Removes this device's tailnet identity. You'll need a new auth key to register it again.") \
X(NET_TS_FORGET_ACTION, "Forget") \
X(NET_TS_FORGETTING, "Forgetting this device\xe2\x80\xa6") \
X(NET_TS_FORGOTTEN, "Forgotten: it registers again the next time Tailscale starts") \
X(NET_TS_FORGET_FAILED, "Couldn't forget this device. Try again.") \
X(NET_TS_FORGET_BUSY, "Already forgetting this device") \
X(NET_PEERS_NONE, "No peers yet") \
X(NET_PEERS_TS_OFF, "Tailscale is off. Turn it on to see your peers.") \
X(NET_PEER_LINE, "%s \xc2\xb7 %s \xc2\xb7 %s") \
X(NET_PEER_ONLINE, "online") \
X(NET_PEER_OFFLINE, "offline") \
X(NET_PEER_DIRECT, "direct") \
X(NET_PEER_RELAY, "relay") \
X(NET_REFRESH, "Refresh") \
X(NET_ADD_SSH_HOST, "Add as SSH host") \
X(NET_BACK_SETTINGS, "Settings") \
X(TS_OFF_BANNER, "Tailscale is off. These hosts are reachable only over your tailnet.") \
X(TS_TURN_ON, "Turn on") \
X(HOSTS_TITLE, "Terminal") \
X(HOSTS_CONNECT_HINT, "\xe2\x86\xb5 Connect \xc2\xb7 \xe2\x8c\x83\xe2\x86\xb5 Edit") \
X(HOSTS_CONNECTING, "Connecting\xe2\x80\xa6") \
X(HOSTS_ADD, "+ Add host") \
X(HOSTS_ADD_TITLE, "Add host") \
X(HOSTS_FROM_TAILNET, "From your tailnet") \
X(HOSTS_MANUAL, "Enter manually") \
X(HOSTS_NO_PEERS, "No tailnet peers online") \
X(HOSTS_NOT_LISTED, "The Host isn't listed here: its sessions are on the Sessions screen.") \
X(HOSTS_LEAVE_NOTE, "Ctrl+\xe2\x86\x90 leaves an SSH session.") \
X(HOSTS_NO_WIFI, "No Wi-Fi") \
X(HOSTS_GONE, "This host was removed") \
X(TERM_LEAVE, "\xe2\x86\x90 Leave") \
X(TERM_KEY_ESC, "Esc") \
X(TERM_KEY_TAB, "Tab") \
X(TERM_KEY_CTRL_C, "^C") \
X(TERM_KEY_UP, "\xe2\x86\x91") \
X(TERM_KEY_DOWN, "\xe2\x86\x93") \
X(TERM_STILL_RUNNING, "Session still running on the Host") \
X(TERM_OPENING, "Opening the session on the Host\xe2\x80\xa6") \
X(TERM_OPENING_HINT, "Ctrl+\xe2\x86\x90 returns to Sessions; the session keeps running.") \
X(TERM_CONNECTING_TO, "Connecting to %s@%s:%d\xe2\x80\xa6") \
X(TERM_SSH_HINT, "Ctrl+\xe2\x86\x90 disconnects.") \
X(TERM_DISCONNECT_Q, "Disconnect from %s?") \
X(TERM_LINK_LOST, "Link lost, reconnecting\xe2\x80\xa6") \
X(TERM_DISCONNECTED_FROM, "Disconnected from %s") \
X(TERM_SESSION_CLOSED, "Session closed") \
X(TERM_SESSION_ENDED, "The session ended (status %d)") \
X(TERM_CANT_OPEN, "Can't open the session") \
X(TERM_NO_SSH_TASK, "Couldn't start the connection. Try again.") \
X(TERM_NOT_VERIFIED, "The Host link isn't verified yet; try again in a moment") \
X(BOOT_STARTING, "Starting\xe2\x80\xa6") \
X(SLEEP_NOTICE, "Sleeping to save battery. Press the side button to wake.") \
X(UNIT_S, "s") \
X(UNIT_MIN, "min") \
X(UNIT_H, "h") \
X(HDR_NEEDS_YOU_1, "needs you") \
X(HDR_NEEDS_YOU_N, "need you") \
X(PILL_NEEDS_YOU, "NEEDS YOU") \
X(PILL_WAITING, "waiting on Host") \
X(PILL_WORKING, "WORKING") \
X(PILL_IDLE, "IDLE") \
X(PILL_DONE, "DONE") \
X(PILL_ENDED, "ENDED") \
X(SES_TITLE, "Sessions") \
X(SES_SELECT, "\xe2\x86\x91\xe2\x86\x93 Select") \
X(SES_SEND, "\xe2\x86\xb5 Send") \
X(SES_OPEN, "\xe2\x8c\x83O Open") \
X(SES_TERMINAL, "\xe2\x8c\x83T/\xe2\x8c\x83M Terminal") \
X(SES_SETTINGS, "\xe2\x8c\x83P Settings") \
X(SES_KEYS, "\xe2\x8c\x83K Keys") \
X(SES_NO_HOST_BODY, "Set up your Host to see its Claude sessions.") \
X(SES_SET_UP_HOST, "Set up Host") \
X(SES_EMPTY, "No sessions on this Host. Start one with `claude --bg`.") \
X(SES_WIFI_SETTINGS, "Wi-Fi settings") \
X(SES_HOST_SETTINGS, "Host settings") \
X(SES_BATTERY, "Battery") \
X(SES_SLEEPS_AT, "The Tab5 sleeps at 10 %.") \
X(SES_DISMISS, "Dismiss") \
X(SES_RECONNECTING, "Reconnecting to Host\xe2\x80\xa6") \
X(SES_LOADING, "Loading sessions\xe2\x80\xa6") \
X(SES_UPDATING, "Updating\xe2\x80\xa6") \
X(SES_LIST_TOO_BIG, "Session list too large to load") \
X(SES_CANT_LOAD, "Can't load sessions from the Host\xe2\x80\xa6") \
X(SES_LOADING_OUTPUT, "Loading recent output\xe2\x80\xa6") \
X(SES_NO_OUTPUT, "No output to show") \
X(SES_NO_OUTPUT_YET, "No output yet") \
X(SES_INTERACTIVE, "Running in a terminal on the Host. Start sessions with claude --bg to open them here.") \
X(SES_ON_HOST, "on Host") \
X(SES_STOPPED_OPEN, "This session has stopped. Open it to start it again.") \
X(SES_TYPE_ANSWER, "Type an answer\xe2\x80\xa6") \
X(SES_TYPE_MESSAGE, "Type a message\xe2\x80\xa6") \
X(SES_SENT, "Sent \xe2\x9c\x93") \
X(SES_OPEN_TO_APPROVE, "Open it to approve") \
X(SES_OPEN_TO_ANSWER, "Open the session to answer") \
X(SES_PICKS_OPTION, "picks an option") \
X(SES_STILL_WORKING, "Still working on the last request") \
X(SES_SENDING, "Sending\xe2\x80\xa6") \
X(SES_OPENING, "Opening\xe2\x80\xa6") \
X(SES_CANT_OPEN, "Can't open: set up the Host in Settings \xe2\x80\xba Host") \
X(SES_NOT_CONNECTED, "Not connected to the Host") \
X(SES_FORGET_RECONNECTING, "Old key forgotten; reconnecting\xe2\x80\xa6") \
X(SES_NEW_KEY_PINNED, "New host key pinned:") \
X(SES_NEW_HOST_KEY, "The Host has a new host key. Check and trust it in Settings \xe2\x80\xba Host.") \
X(SES_TRUST_WAITING, "Waiting for the Host to connect with that key\xe2\x80\xa6") \
X(SES_TRUST_REFUSED, "Not trusted: a key is already pinned for the Host. Check Settings \xe2\x80\xba Host.") \
X(SES_TRUST_ENDED, "Not trusted: the Host didn't connect with that key in time. Check Settings \xe2\x80\xba Host again.") \
X(SES_FORGET_ENDED, "Forget cancelled: the Host changed") \
X(SES_KEYS_TITLE, "Keyboard shortcuts") \
X(SES_KEYS_CLOSE, "Any key or a touch closes this") \
X(HK_K_UPDOWN, "Up / Down") \
X(HK_W_UPDOWN, "move between sessions") \
X(HK_K_ENTER, "Enter") \
X(HK_W_ENTER, "send the answer") \
X(HK_K_CTRL_O, "Ctrl+O") \
X(HK_W_CTRL_O, "open the session") \
X(HK_K_CTRL_T, "Ctrl+T") \
X(HK_W_CTRL_T, "terminal (Ctrl+M also)") \
X(HK_K_CTRL_P, "Ctrl+P") \
X(HK_W_CTRL_P, "settings") \
X(HK_K_CTRL_K, "Ctrl+K") \
X(HK_W_CTRL_K, "this list") \
X(HK_K_CTRL_SPACE, "Ctrl+Space") \
X(HK_W_CTRL_SPACE, "voice input: press to start, again to stop") \
X(HK_K_ESC, "Esc") \
X(HK_W_ESC, "clear the answer, or go back") \
X(HK_K_CTRL_BRACKET, "Ctrl+\xe2\x86\x90") \
X(HK_W_CTRL_BRACKET, "leave a take-over or SSH session") \
X(HK_K_SHIFT_UPDOWN, "Shift+Up / Down") \
X(HK_W_SHIFT_UPDOWN, "scroll back in a take-over") \
X(TERM_KEEPS_RUNNING, "The session keeps running on the Host.") \
X(SSH_NO_SESSION, "Couldn't start SSH. Try again.") \
X(SSH_CANT_CONNECT, "Couldn't connect:") \
X(SSH_PIN_UNREADABLE, "Can't read the saved host key; not connecting") \
X(SSH_HOSTKEY_REJECTED, "Host key rejected") \
X(SSH_NO_CHANNEL, "Couldn't open an SSH channel. Try again.") \
X(SSH_CMD_REFUSED, "The host refused the command") \
X(SSH_SHELL_REFUSED, "The host refused a shell") \
X(SSH_NET_DOWN, "The network went down.") \
X(SSH_SERVER_CLOSED, "The host closed this connection") \
X(SSH_DEVICE_KEY_UNAVAILABLE, "The device key isn't available") \
X(SSH_KEY_IMPORT_FAILED, "Couldn't read the key file:") \
X(HF_KEY_FILE_MISSING, "Key file not found on SD card") \
X(HF_KEY_BAD_PATH, "The key file must be on the SD card") \
X(HF_KEY_REFUSED, "The host refused the key") \
X(HF_HOSTKEYS_UNCHECKED, "Couldn't check this host against the Host's keys") \
X(HF_TITLE_ADD, "Add host") \
X(HF_TITLE_EDIT, "Edit host") \
X(HF_BACK_TERMINAL, "Terminal") \
X(HF_NAME, "Name") \
X(HF_ADDRESS, "Address") \
X(HF_PORT, "Port") \
X(HF_USER, "User") \
X(HF_SIGN_IN, "Sign in with") \
X(HF_DEVICE_KEY, "Device key") \
X(HF_PASSWORD, "Password") \
X(HF_SD_KEY, "Key file on SD card") \
X(HF_PW_SET, "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2 set") \
X(HF_HOST_KEY, "Host key") \
X(HF_FORGET_Q, "Forget this host's key?") \
X(HF_FORGET_BODY, "Only do this if the host's SSH key changed. The Tab5 will trust the next key it sees.") \
X(HF_KEY_FORGOTTEN, "Host key forgotten. The next connect asks you to trust the key.") \
X(HF_DELETE, "Delete host") \
X(HF_DELETE_Q, "Delete %s?") \
X(HF_DELETE_BODY, "It goes from the Terminal list. Its saved host key stays.") \
X(HF_DISCARD_Q, "Discard changes?") \
X(HF_DISCARD, "Discard") \
X(HF_SAVE, "Save") \
X(HF_SAVED, "Saved") \
X(HF_SAVE_FAILED, "Couldn't save the host. Try again.") \
X(HF_LIST_FULL, "The list is full: it holds 24 hosts. Delete one first.") \
X(HF_DELETE_FAILED, "Couldn't delete the host. Try again.") \
X(HF_ERR_NAME_TAKEN, "Another host already has that name") \
X(HF_NAME_PH, "user@host") \
X(HF_ADDRESS_PH, "box.local") \
X(HF_PORT_PH, "22") \
X(HF_USER_PH, "user") \
X(HF_HOST_PH, "host") \
X(HOSTS_CONNECT_FAILED, "Couldn't start the connection. Try again.") \
X(HF_ERR_NO_ADDRESS, "Enter the host's address") \
X(HF_ERR_NO_USER, "Enter a user") \
X(HF_ERR_BAD_PORT, "The port is a number from 1 to 65535") \
X(HF_ERR_IS_HOST, "That's the Host: its sessions are on the Sessions screen.") \
X(VOICE_RECORDING, "Recording") \
X(VOICE_HINT, "Ctrl+Space stops \xc2\xb7 Esc cancels") \
X(VOICE_TRANSCRIBING, "Transcribing on the Host\xe2\x80\xa6") \
X(VOICE_REVIEW, "Check the text, then press Enter to send") \
X(VOICE_CUT, "The text was cut to fit the answer field") \
X(VOICE_AT_LIMIT, "Stopped at the 30 s limit") \
X(VOICE_SESSION_GONE, "That session is gone, so the text was dropped") \
X(VOICE_TOO_SHORT, "Too short. Press Ctrl+Space, speak, then press it again.") \
X(VOICE_NO_SPEECH, "No speech heard. Try again closer to the Tab5.") \
X(VOICE_BUSY, "Still transcribing\xe2\x80\xa6") \
X(VOICE_NEEDS_FIELD, "Voice input works in a session's answer field") \
X(VOICE_NO_WHISPER, "whisper-cli isn't on the Host at the Whisper path. See README \xe2\x80\xba Set up voice input.") \
X(VOICE_NO_MODEL, "The whisper model isn't on the Host at the model path. See README \xe2\x80\xba Set up voice input.") \
X(VOICE_BAD_MODEL, "The Host couldn't load the whisper model. Download it again (README \xe2\x80\xba Set up voice input).") \
X(VOICE_CANT_RUN, "whisper-cli didn't run on the Host. Check the Whisper path in Settings \xe2\x80\xba Host.") \
X(VOICE_AUDIO_BAD, "The Host couldn't read the recording. Try again.") \
X(VOICE_FAILED, "Transcription failed on the Host. Try again.") \
X(VOICE_TIMEOUT, "The Host took too long to transcribe. Try again.") \
X(VOICE_LINK_LOST, "Lost the connection to the Host. Try again.") \
X(VOICE_MIC_FAILED, "The microphone didn't start. Restart the Tab5 and try again.") \
X(VOICE_NO_MEMORY, "Not enough memory to record. Restart the Tab5 and try again.")

typedef enum {
#define X(id, en) STR_##id,
    STR_TABLE(X)
#undef X
    STR_COUNT
} str_id_t;

lang_t i18n_get_lang(void);   /* always LANG_EN: the UI is English only */
const char *tr(str_id_t id);

#ifdef __cplusplus
}
#endif
