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
X(HOME_SSH, "SSH Terminal") \
X(HOME_FILES, "File Manager") \
X(HOME_MUSIC, "Music") \
X(HOME_CAMERA, "Camera") \
X(HOME_WIFI, "WiFi") \
X(HOME_SETTINGS, "Settings") \
X(HOME_DEVINFO, "Device Info") \
X(HOME_USB, "USB") \
X(HOME_TOOLS, "Tools") \
X(HOME_OTA, "Firmware") \
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
X(FILES, "Files") \
X(TF_CARD, "TF Card") \
X(USB_DRIVE, "USB Drive") \
X(TF_NOT_MOUNTED, "TF card not mounted") \
X(EMPTY_FOLDER, "Empty folder") \
X(COPY, "Copy") \
X(CUT, "Cut") \
X(PASTE, "Paste") \
X(RENAME, "Rename") \
X(NEW_FOLDER, "New Folder") \
X(NEW_FILE, "New File") \
X(SELECT, "Select") \
X(SELECT_ALL, "Select All") \
X(CANCEL_SELECT, "Cancel Select") \
X(DELETE_SELECTED, "Delete Selected") \
X(DELETE_Q, "Delete %d item(s)? This cannot be undone.") \
X(CLIPBOARD_EMPTY, "Clipboard is empty") \
X(PASTE_DONE, "Paste done") \
X(COPYING, "Copying %s") \
X(OP_FAILED, "Operation failed: %s") \
X(TARGET_EXISTS, "Target exists") \
X(INTO_ITSELF, "Cannot move into itself") \
X(FILE_NAME, "File name") \
X(FOLDER_NAME, "Folder name") \
X(ITEMS_TOTAL, "%u items, %s") \
X(FREE_SPACE, "Free %s / %s") \
X(OPEN_WITH, "Open") \
X(IMAGE_VIEWER, "Image Viewer") \
X(TEXT_VIEWER, "Text Viewer") \
X(FILE_TOO_LARGE, "File too large") \
X(CANNOT_READ, "Cannot read file.") \
X(LOADING_IMAGE, "Loading image...") \
X(IMAGE_LOAD_FAILED, "Image load failed") \
X(EXIF_INFO, "EXIF") \
X(NO_EXIF, "No EXIF data") \
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
X(SET_BACKUP, "Backup") \
X(SET_BRIGHTNESS, "Brightness") \
X(SET_ROTATION, "Screen rotation") \
X(SET_TIME, "Time") \
X(SET_UTC_OFFSET, "UTC offset") \
X(SET_UTC_NOTE, "UTC+ means local time is ahead of UTC. Japan is UTC+9.") \
X(SET_NTP, "Sync time (NTP)") \
X(SET_TIME_SYNCED, "Time synchronized") \
X(SET_SCREENSHOT_NOTE, "Three-finger tap saves a screenshot to SD/ScreenShots.") \
X(SCREENSHOT_SAVED, "Saved to SD/ScreenShots") \
X(SCREENSHOT_FAILED, "Screenshot failed") \
X(SET_STATUSBAR, "Status bar") \
X(SB_WIFI, "WiFi icon") \
X(SB_TAILSCALE, "Tailscale icon") \
X(SB_IP, "IP address") \
X(SB_BATTERY, "Battery") \
X(SB_KEYBOARD, "Keyboard icon") \
X(SB_CLOCK, "Clock") \
X(SB_DATE, "Date") \
X(SB_YEAR, "Year in the date") \
X(SB_NOTE, "Icons blink while the link is coming up.") \
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
X(SET_KBD_NEEDS_LIGHT, "Keyboard light shows needs-you") \
X(SET_KBD_STATUS, "Keyboard: %s") \
X(SET_KBD_LAYOUT, "Keyboard layout") \
X(KBD_LAYOUT_STOCK, "As printed") \
X(KBD_LAYOUT_JIS, "JIS (Japanese)") \
X(SET_KBD_LAYOUT_NOTE, "JIS reads the raw key matrix and maps the layout on this device, so the keys no longer match the printed caps. Fn gives F1-F12 on the number row.") \
X(PRESENT, "present") \
X(ABSENT, "not detected") \
X(SET_TERM_FONT, "Terminal font size") \
X(BACKUP_EXPORT_ENC, "Export Encrypted") \
X(BACKUP_EXPORT_PLAIN, "Export Plain") \
X(BACKUP_IMPORT, "Import from SD") \
X(BACKUP_PASSWORD, "Backup password") \
X(BACKUP_PW_NOTE, "Enter 4-64 letters/digits. This password protects SSH/WiFi passwords.") \
X(BACKUP_WRITTEN, "Backup written to %s") \
X(BACKUP_IMPORTED, "Backup imported. Rebooting...") \
X(BACKUP_NOT_FOUND, "No backup found on SD.") \
X(BACKUP_ENC_FOUND, "Encrypted backup found. Enter password to import.") \
X(BACKUP_BAD_PW, "Wrong password or damaged data") \
X(BACKUP_PLAIN_WARN, "Plain backups contain passwords in clear text.") \
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
X(STATUS_BAR_TIME, "Time") \
X(BATTERY_PCT, "Battery %d%%") \
X(TERM_HINT, "Edges: top=status, left=controls, right=keyboard") \
X(SOFT_KBD, "Soft Keyboard") \
X(REBOOT, "Reboot") \
X(REBOOT_Q, "Reboot now?") \
X(ABOUT, "About") \
X(ABOUT_TEXT,       "Claude Companion: session control for Claude Code on M5Stack Tab5.\nlibssh (LGPL 2.1), MicroLink (MIT), LVGL (MIT), Noto Sans CJK (OFL).") \
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
X(QUERYING, "querying...")

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
