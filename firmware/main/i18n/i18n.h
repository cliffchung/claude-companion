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
X(YES, "Yes") \
X(NO, "No") \
X(BACK, "Back") \
X(SAVE, "Save") \
X(DELETE, "Delete") \
X(EDIT, "Edit") \
X(CONNECT, "Connect") \
X(PLAY, "Play") \
X(PAUSE, "Pause") \
X(TERM_MENU, "Terminal") \
X(TERM_CLEAR, "Clear the screen") \
X(TERM_SCROLL_END, "Jump to the newest line") \
X(GESTURE_NOTE, "Swipe right to go back. Long-press a tile for its shortcut.") \
X(SET_SECURITY, "Security") \
X(CREDS_ON_SD, "Keep SSH servers on the TF card") \
X(CREDS_NOTE, "Hosts, user names and passwords are encrypted with the device key and written to the card; nothing is left in flash. The key stays on the device, so the card alone cannot be read. Without the card the list starts empty.") \
X(CREDS_RELOAD, "Re-read from the card") \
X(CREDS_NO_SD, "The TF card is not available") \
X(DISCONNECT, "Disconnect") \
X(CLOSE, "Close") \
X(DONE, "Done") \
X(PLEASE_WAIT, "Please wait...") \
X(SERVER_SAVE_FAILED, "Could not save the connection. Check storage and the server limit.") \
X(ERROR, "Error") \
X(FAILED, "Failed") \
X(BAD_COLOR, "Use #RRGGBB") \
X(SUCCESS, "Success") \
X(ON, "On") \
X(OFF, "Off") \
X(NONE, "None") \
X(UNKNOWN, "Unknown") \
X(CAMERA_STARTING, "Starting camera...") \
X(CAMERA_READY, "Ready") \
X(CAMERA_CAPTURE, "Take photo") \
X(CAMERA_SAVING, "Saving photo...") \
X(CAMERA_SAVED, "Saved to TF/DCIM/ClaudeCompanion") \
X(CAMERA_FAILED, "Camera could not start") \
X(CAMERA_SAVE_FAIL, "Could not save photo") \
X(SERVERS, "SSH Servers") \
X(ADD_SERVER, "Add Server") \
X(EDIT_SERVER, "Edit Server") \
X(DEL_SERVER, "Delete Server") \
X(DEL_SERVER_Q, "Delete this server?") \
X(NO_SERVERS, "No servers yet. Tap + to add one.") \
X(NAME, "Name") \
X(HOST, "Host / IP") \
X(PORT, "Port") \
X(USERNAME, "Username") \
X(PASSWORD, "Password") \
X(AUTH_METHOD, "Auth") \
X(AUTH_PASSWORD, "Password") \
X(AUTH_KEY, "Private key (SD)") \
X(KEY_PATH, "Key file path") \
X(HOST_USER_REQ, "Host and user required.") \
X(SAVED, "Saved.") \
X(CONNECTING, "Connecting...") \
X(CONNECTED, "Connected") \
X(DISCONNECTED, "Disconnected") \
X(SSH_CONN_FAILED, "SSH connection failed.") \
X(SSH_AUTH_FAILED, "Authentication failed.") \
X(SSH_HOSTKEY_NEW, "New host key. Trust and continue?") \
X(SSH_HOSTKEY_CHANGED, "HOST KEY CHANGED! Possible attack. Continue?") \
X(WIFI_NOT_CONNECTED, "WiFi not connected") \
X(TERM_CLOSE_Q, "Disconnect SSH and return?") \
X(KEYBOARD, "Keyboard") \
X(PHYS_KBD_DETECTED, "Physical keyboard detected") \
X(USB_DRIVE, "USB Drive") \
X(TF_NOT_MOUNTED, "TF card not mounted") \
X(COPYING, "Copying %s") \
X(OPEN_WITH, "Open") \
X(IMAGE_VIEWER, "Image Viewer") \
X(CANNOT_READ, "Cannot read file.") \
X(EXIF_INFO, "EXIF") \
X(ROTATE, "Rotate") \
X(MUSIC_PLAYER, "Music Player") \
X(NOW_PLAYING, "Now Playing") \
X(FAVORITES, "Favorites") \
X(ALL_MUSIC, "All Music") \
X(ADD_FAV, "Add to Favorites") \
X(REMOVE_FAV, "Remove from Favorites") \
X(ADDED_FAV, "Added to favorites") \
X(REMOVED_FAV, "Removed from favorites") \
X(CLEAR_FAV, "Clear Favorites") \
X(SCANNING_AUDIO, "Scanning audio files...") \
X(NO_AUDIO, "No audio files found") \
X(DECODE_FAILED, "Decode failed") \
X(VOLUME, "Volume") \
X(MUSIC_STOPPED, "Music stopped") \
X(WIFI_MANAGER, "WiFi Manager") \
X(WIFI_STATUS, "Status") \
X(WIFI_SCAN, "Scan") \
X(WIFI_SCANNING, "Scanning...") \
X(WIFI_SAVED, "Saved Networks") \
X(WIFI_MANUAL, "Manual WiFi") \
X(WIFI_SSID, "SSID") \
X(WIFI_SAVE_CONNECT, "Save & Connect") \
X(WIFI_FORGET, "Forget") \
X(WIFI_OPEN_NOTE, "Leave the password blank for open networks.") \
X(WIFI_CONNECT_FAILED, "Connect failed. Check password.") \
X(WIFI_SAVED_DELETED, "Saved WiFi deleted.") \
X(WIFI_ENABLE, "WiFi enabled") \
X(IPV4, "IPv4") \
X(IPV6, "IPv6") \
X(RSSI, "Signal") \
X(CHANNEL, "Channel") \
X(POWER_NOTE, "WiFi upload draws up to 11W. Use a 5V 2A adapter.") \
X(SETTINGS, "Settings") \
X(SET_SYSTEM, "System") \
X(SET_DISPLAY, "Display") \
X(SET_NETWORK, "Network") \
X(SET_POWER, "Power") \
X(SET_INPUT, "Input") \
X(SET_BRIGHTNESS, "Brightness") \
X(SET_ROTATION, "Screen rotation") \
X(SET_TIME, "Time") \
X(SET_UTC_OFFSET, "UTC offset") \
X(SET_UTC_NOTE, "UTC+ means local time is ahead of UTC. Japan is UTC+9.") \
X(SET_NTP, "Sync time (NTP)") \
X(SET_TIME_SYNCED, "Time synchronized") \
X(SET_STATUSBAR, "Status bar") \
X(SET_THEME, "Appearance") \
X(TH_PRESET, "Theme") \
X(TH_CUSTOM, "Custom") \
X(TH_RADIUS, "Corner rounding") \
X(TH_DENSITY, "Spacing") \
X(TH_COMPACT, "Compact") \
X(TH_NORMAL, "Normal") \
X(TH_ROOMY, "Roomy") \
X(TH_TILE_ACCENT, "Tint home tiles with the accent") \
X(TH_GLOW, "Glow behind cards") \
X(TH_SCANLINES, "Scan lines") \
X(TH_COLORS, "Colours") \
X(TH_BG, "Background") \
X(TH_CARD, "Card") \
X(TH_CARD_HI, "Card highlight") \
X(TH_ACCENT, "Accent") \
X(TH_ACCENT2, "Secondary accent") \
X(TH_DANGER, "Danger") \
X(TH_TEXT, "Text") \
X(TH_MUTED, "Secondary text") \
X(TH_TERMINAL, "Terminal") \
X(TH_TERM_FG, "Foreground") \
X(TH_TERM_BG, "Background") \
X(TH_TERM_CURSOR, "Cursor") \
X(TH_CURSOR_STYLE, "Cursor shape") \
X(TH_CUR_BLOCK, "Block") \
X(TH_CUR_UNDER, "Underline") \
X(TH_CUR_BAR, "Bar") \
X(TH_CURSOR_BLINK, "Blink the cursor") \
X(TH_LINE_HEIGHT, "Line height") \
X(TH_ANSI, "ANSI palette") \
X(TH_RESET, "Reset to the preset") \
X(SET_KBD_LED, "Keyboard LED") \
X(SET_KBD_LED_BRIGHT, "LED brightness") \
X(SET_KBD_NEEDS_LIGHT, "Show needs-you on keyboard") \
X(SET_KBD_STATUS, "Keyboard: %s") \
X(SET_KBD_LAYOUT, "Keyboard layout") \
X(KBD_LAYOUT_STOCK, "As printed") \
X(KBD_LAYOUT_JIS, "JIS (Japanese)") \
X(SET_KBD_LAYOUT_NOTE, "JIS reads the raw key matrix and maps the layout on this device, so the keys no longer match the printed caps. Fn gives F1-F12 on the number row.") \
X(PRESENT, "present") \
X(ABSENT, "not detected") \
X(SET_TERM_FONT, "Terminal font size") \
X(OTA_TITLE, "Firmware Update") \
X(OTA_CURRENT, "Current: %s") \
X(OTA_LATEST, "Latest: %s") \
X(OTA_CHECK, "Check Update") \
X(OTA_CHECKING, "Checking update...") \
X(OTA_LATEST_ALREADY, "Already latest.") \
X(OTA_FOUND, "Update found") \
X(OTA_DOWNLOAD, "Download to SD") \
X(OTA_DOWNLOADING, "Downloading %d%%") \
X(OTA_DOWNLOADED, "Firmware downloaded. Ready to install.") \
X(OTA_INSTALL, "Install") \
X(OTA_INSTALL_NOTE, "Install will reboot to the updater and flash the app. Keep power stable.") \
X(OTA_LAUNCHER_MODE, "Booted by Launcher: install via Launcher.") \
X(OTA_VERIFY_FAILED, "Package verification failed") \
X(OTA_HTTP_FAILED, "HTTP request failed") \
X(OTA_UPDATER, "Updater (UPLOAD) firmware") \
X(OTA_UPDATE_UPDATER, "Update Updater") \
X(OTA_UPDATER_DONE, "Updater firmware updated.") \
X(OTA_SD_PACKAGE, "Update package found on SD. Ready to install.") \
X(OTA_URL, "Update URL") \
X(DEVINFO, "Device Info") \
X(DEV_CHIP, "Chip") \
X(DEV_IDF, "IDF version") \
X(DEV_APP_VER, "App version") \
X(DEV_UPTIME, "Uptime") \
X(DEV_HEAP, "DRAM free / total") \
X(DEV_DMA, "DMA free / total") \
X(DEV_PSRAM, "PSRAM free / total") \
X(DEV_FLASH, "Flash") \
X(DEV_TF, "TF card") \
X(DEV_BATTERY, "Battery") \
X(DEV_POWER_SRC, "Power") \
X(PWR_USB_C, "USB-C") \
X(PWR_USB_C_CHARGE, "USB-C + Battery charging") \
X(PWR_USB_C_STANDBY, "USB-C + Battery standby") \
X(PWR_BATTERY, "Battery") \
X(DEV_PERIPHERALS, "Peripherals") \
X(USB_TITLE, "USB") \
X(USB_DISK_MODE, "USB Disk Mode") \
X(USB_DISK_START, "Start USB Disk Mode") \
X(USB_DISK_STOP, "Stop USB Disk Mode") \
X(USB_DISK_NOTE, "Connect USB-C to the PC. The PC gets exclusive access to the TF card, and the serial port is unavailable until you stop it.") \
X(USB_DISK_ON, "USB disk mode active: PC has the TF card") \
X(USB_DISK_OFF, "USB disk mode off. TF card restored.") \
X(USB_HOST, "USB Flash Drive") \
X(USB_MOUNT, "Mount USB") \
X(USB_EJECT, "Eject USB") \
X(USB_MOUNTED, "USB drive mounted") \
X(USB_NOT_MOUNTED, "USB drive not mounted") \
X(USB_WAITING, "Waiting for USB device...") \
X(USB_FORMAT, "Format USB (FAT32)") \
X(USB_FORMAT_WARN, "Formatting will erase all files on the USB drive.") \
X(USB_FORMATTED, "USB drive formatted") \
X(BATTERY_PCT, "Battery %d%%") \
X(TERM_HINT, "Edges: top=status, left=controls, right=keyboard") \
X(SOFT_KBD, "Soft Keyboard") \
X(REBOOT, "Reboot") \
X(REBOOT_Q, "Reboot now?") \
X(ABOUT, "About") \
X(ABOUT_TEXT,       "Claude Companion: session control for Claude Code on M5Stack Tab5.\nlibssh (LGPL 2.1), MicroLink (MIT), LVGL (MIT), Noto Sans CJK (OFL).\nBased on Clientre 5 by Mozgi512 (MIT)") \
X(SD_LOW_SPACE, "TF card has less than 20MB free") \
X(TAILSCALE, "Tailscale") \
X(TS_ENABLE, "Join tailnet") \
X(TS_AUTH_KEY, "Auth key (tskey-auth-...)") \
X(TS_DEVICE_NAME, "Device name") \
X(TS_CTRL_HOST, "Control server (blank = Tailscale)") \
X(TS_STATUS, "Status: %s") \
X(TS_VPN_IP, "Tailnet IP: %s") \
X(TS_PEERS, "Peers") \
X(TS_NO_PEERS, "No peers yet") \
X(TS_KEY_FROM_SD, "Load key from SD (/tailscale/authkey.txt)") \
X(TS_KEY_LOADED, "Auth key loaded") \
X(TS_KEY_MISSING, "No auth key. Create one at login.tailscale.com/admin/settings/keys") \
X(TS_DIRECT, "Direct connections (experimental)") \
X(TS_DIRECT_NOTE, "Off = always relay via DERP (reliable). On = try direct UDP paths; may stall behind strict NAT.") \
X(TS_FORGET, "Forget this node") \
X(TS_FORGET_Q, "Remove node keys? You will need a new auth key.") \
X(TS_ADD_SERVER, "Add as SSH server") \
X(TS_NOTE, "Unofficial client (MicroLink). Hostnames are resolved from the peer list; connections may relay via DERP.") \
X(PUBLIC_IP, "Public IP") \
X(QUERYING, "querying...") \
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
X(SET_FALLBACK_BAD, "Use a home network address like 192.168.1.50, or leave it empty") \
X(SET_CLAUDE_PATH, "Claude path on host") \
X(SET_CLAUDE_PATH_BAD, "Use only letters, digits and _ . / ~ - (1 to 63 characters)") \
X(SET_HOST_KEY, "Host key") \
X(SET_NO_PIN, "No key pinned yet") \
X(SET_FORGET, "Forget") \
X(SET_FORGET_Q, "Forget this host's key?") \
X(SET_FORGET_BODY, "Only do this if the host's SSH key changed. The Tab5 will trust the next key it sees.") \
X(SET_BUSY, "Busy, try again") \
X(SET_DEVICE_KEY, "This device's key") \
X(SET_PUBLIC_KEY, "Public key") \
X(SET_SHOW, "Show") \
X(SET_HIDE, "Hide") \
X(SET_KEY_UNREADABLE, "The device key can't be read") \
X(SET_DEVICE_KEY_NOTE, "Add this line to ~/.ssh/authorized_keys on each host this Tab5 signs in to.") \
X(SET_HOST_NOTE, "The host is one of your SSH hosts. The Tab5 lists its Claude Code sessions and runs claude from this path.") \
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
X(LINK_CANT_REACH, "Can't reach %s. Is it awake?") \
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
X(NET_TS_KEY_CLEAR_Q, "Clear the auth key?") \
X(NET_TS_KEY_CLEAR_BODY, "This device stays on the tailnet. To register it again you'll need a new key.") \
X(NET_TS_KEY_CLEAR_ACTION, "Clear") \
X(NET_TS_KEY_CLEARED, "Auth key cleared") \
X(NET_TS_NAME, "Device name") \
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
X(NET_TS_FORGET_BODY_NO_KEY, "Removes this device's tailnet identity. It registers again with the saved auth key the next time Tailscale starts. You'll need a new auth key.") \
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
X(NET_ADD_FROM_TERMINAL, "Add it from Terminal") \
X(NET_BACK_SETTINGS, "Settings") \
X(TS_OFF_BANNER, "Tailscale is off. These hosts are reachable only over your tailnet.") \
X(TS_TURN_ON, "Turn on")

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
