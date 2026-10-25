#include "sdkconfig.h"
#include "cc_link.h"
#include "cc_cmd.h"
#include "cc_sessions.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "sys/debug_console.h"
#include "net/wifi_mgr.h"
#include "net/tailscale.h"
#include "settings/settings.h"
#include "ui/ui.h"
#include "ui/ui_sessions.h"
#include "net/ts_host.h"
#include "net/fast_rules.h"
#include "esp_netif.h"
#include "ssh/ssh_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "i18n/i18n.h"
#include "voice/voice_rules.h"

/* Console commands for the companion link: companion, sessions, reply, logs, wifi, ts.
 * Everything that reaches the Host goes through cc_link and a cc_cmd_* builder. */

#define SNAP_CAP 128

#if CONFIG_COMPANION_DEBUG_COMMANDS
#define DBG_WATCH "|test-fallback"
#define DBG_WIFI "wifi test drop | "
#define DBG_WIFI_HELP " | debug: wifi test drop"
#define DBG_COMPANION " | debug: takeover <id> | hostcache set <ip> | watch test-fallback | hostkey corrupt | kick on|off | voice-upload-test <kb> [delay-ms]"
#else
#define DBG_WATCH ""
#define DBG_WIFI ""
#define DBG_WIFI_HELP ""
#define DBG_COMPANION ""
#endif
#define LOGS_CAP (16 * 1024)

static const char *state_name(cc_state_t s)
{
    switch (s) {
    case CC_NEEDS_YOU: return "NEEDS_YOU";
    case CC_WORKING:   return "WORKING";
    case CC_IDLE:      return "IDLE";
    case CC_DONE:      return "DONE";
    default:           return "ENDED";
    }
}

/* Joins argv[from..] with single spaces. */
static char *join_args(int argc, char **argv, int from)
{
    size_t n = 1;
    for (int i = from; i < argc; i++) n += strlen(argv[i]) + 1;
    char *s = malloc(n);
    if (!s) return NULL;
    s[0] = 0;
    for (int i = from; i < argc; i++) {
        if (i > from) strcat(s, " ");
        strcat(s, argv[i]);
    }
    return s;
}

#if CONFIG_COMPANION_DEBUG_COMMANDS
/* Test-only commands (CONFIG_COMPANION_DEBUG_COMMANDS, off by default): takeover, voice-upload-test,
   hostcache set, watch test-fallback, hostkey corrupt, kick, wifi test drop. */
static char s_takeover_id[16];
static void takeover_async(void *arg)
{
    (void)arg;
    int r = ui_sessions_open_test_session(s_takeover_id);
    if (r == UI_SESS_TEST_NOT_SHOWING) printf("takeover: Sessions not showing\n");
    else if (r != UI_SESS_TEST_OK) printf("takeover: no companion-test-* row %s on Sessions\n", s_takeover_id);
}

/* `companion voice-upload-test <kb> [delay-ms]` (Plan 5 Task 3): a WAV of <kb> KB of silence through
   cc_link_transcribe, on its own task so the console stays free (a paste or `power go radio-off` can follow it on
   the next line). delay-ms waits before the call, so a paste typed next gets queued first. Prints lengths,
   durations and the exit status, never the text. */
static volatile bool s_upload_running;
static struct { size_t bytes; int delay_ms; } s_upload;

static void upload_task(void *arg)
{
    (void)arg;
    size_t len = s_upload.bytes;
    if (s_upload.delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(s_upload.delay_ms));
    uint8_t *wav = heap_caps_calloc(1, len, MALLOC_CAP_SPIRAM);   /* calloc: the samples are silence */
    char *out = heap_caps_calloc(1, 2048, MALLOC_CAP_SPIRAM);
    if (!wav || !out) {
        printf("voice-upload-test: no memory\n");
    } else {
        vr_wav_header(wav, (uint32_t)(len - VR_WAV_HDR), VR_RATE, 1, 16);
        int rc = -1, up = -1;
        int64_t t0 = esp_timer_get_time();
        esp_err_t e = cc_link_transcribe(wav, len, out, 2048, &rc, &up, CC_TRANSCRIBE_TIMEOUT_MS);
        int total = (int)((esp_timer_get_time() - t0) / 1000);
        printf("upload %d ms, total %d ms, rc %d, %u chars%s%s\n", up, total, rc, (unsigned)strlen(out),
               e == ESP_OK ? "" : ", ", e == ESP_OK ? "" : esp_err_to_name(e));
    }
    if (wav) { memset(wav, 0, len); heap_caps_free(wav); }
    if (out) { memset(out, 0, 2048); heap_caps_free(out); }
    s_upload_running = false;
    vTaskDelete(NULL);
}

static int voice_upload_test(int argc, char **argv)
{
    int kb = atoi(argv[2]);
    int max_kb = (VR_WAV_HDR + VR_PCM_MAX) / 1024;   /* 937: 30 s at 16 kHz mono */
    if (kb < 1 || kb > max_kb) { printf("voice-upload-test: 1 to %d KB\n", max_kb); return 1; }
    if (s_upload_running) { printf("voice-upload-test: one is running\n"); return 1; }
    s_upload.bytes = (size_t)kb * 1024;
    s_upload.delay_ms = argc == 4 ? atoi(argv[3]) : 0;
    s_upload_running = true;
    if (xTaskCreate(upload_task, "vupload", 4096, NULL, 5, NULL) != pdPASS) {
        s_upload_running = false;
        printf("voice-upload-test: no task\n");
        return 1;
    }
    printf("voice-upload-test: %d KB started\n", kb);
    return 0;
}
#endif

static int cmd_companion(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "voice-check") == 0) {
        int rc = -1;
        esp_err_t e = cc_link_voice_check(&rc);
        if (e == ESP_OK) printf("voice-check: rc %d\n", rc);
        else printf("voice-check: %s\n", esp_err_to_name(e));
        return e == ESP_OK && rc == 0 ? 0 : 1;
    }
    if (argc == 3 && (strcmp(argv[1], "voice-path") == 0 || strcmp(argv[1], "voice-model") == 0)) {
        bool model = strcmp(argv[1], "voice-model") == 0;
        if (!(model ? cc_link_set_whisper_model(argv[2]) : cc_link_set_whisper_path(argv[2]))) {
            printf("%s\n", tr(STR_SET_CLAUDE_PATH_BAD));
            return 1;
        }
        printf("ok\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "voice-paths") == 0) {
        char w[64], m[64];
        printf("whisper %s, model %s\n", cc_settings_whisper_path(w, sizeof(w)), cc_settings_whisper_model(m, sizeof(m)));
        return 0;
    }
#if CONFIG_COMPANION_DEBUG_COMMANDS
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "voice-upload-test") == 0) return voice_upload_test(argc, argv);
#endif
    if (argc >= 3 && strcmp(argv[1], "host") == 0) {
        char *name = join_args(argc, argv, 2);
        if (!name) return 1;
        cc_link_set_server(name);
        if (cc_settings_server() < 0)
            printf("companion: no server named '%s' yet; retrying until there is one\n", name);
        else
            printf("companion: host %s\n", name);
        free(name);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "claude-path") == 0) {
        if (!cc_link_set_claude_path(argv[2])) {
            printf("companion: rejected: only [A-Za-z0-9_./~-], 1-63 chars\n");
            return 1;
        }
        char path[64];
        printf("companion: claude-path %s\n", cc_settings_claude_path(path, sizeof(path)));
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        char st[CC_STATUS_MAX];
        printf("%s\n", cc_link_status_text(st, sizeof(st)));
        printf("%s\n", cc_watch_status_text(st, sizeof(st)));
        char fp[80];
        printf("host key: %s\n", cc_link_pinned_fingerprint(fp, sizeof(fp)) ? fp : "none pinned");
        char path[40], fb[16], ts[16];
        cc_link_mac_path(path, sizeof(path));
        cc_link_lan_fallback(fb, sizeof(fb));
        cc_link_tailnet_pin_source(ts, sizeof(ts));
        printf("path: %s (fallback %s; tailnet pin source %s; Tailscale %s)\n", path, fb[0] ? fb : "none",
               ts[0] ? ts : "none", tailscale_enabled() ? "on" : "off");
        return 0;
    }
#if CONFIG_COMPANION_DEBUG_COMMANDS
    if (argc == 3 && strcmp(argv[1], "takeover") == 0) {
        /* debug: open a companion-test-* session's take-over the way Ctrl+O does, for the timing */
        if (!cc_id_valid(argv[2])) { printf("takeover: bad id\n"); return 1; }
        strlcpy(s_takeover_id, argv[2], sizeof(s_takeover_id));
        ui_async(takeover_async, NULL);
        printf("takeover: opening %s (companion-test-* only)\n", argv[2]);
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "hostcache") == 0 && strcmp(argv[2], "set") == 0) {
        printf("hostcache: %s\n", cc_link_host_cache_test_set(argv[3]) ? "set (RAM only; reconnect to use it)" : "refused");
        return 0;
    }
#endif
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "hostcache") == 0) {
        if (argc == 3 && strcmp(argv[2], "on") == 0) cc_link_host_cache_enable(true);
        else if (argc == 3 && strcmp(argv[2], "off") == 0) cc_link_host_cache_enable(false);
        else if (argc == 3 && strcmp(argv[2], "clear") == 0) cc_link_host_cache_clear();
        else if (argc == 3) return 1;
        char t[200];
        cc_link_host_cache_text(t, sizeof(t));
        printf("%s\n", t);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "resolve") == 0) {
        /* how the link would resolve the Host's name: literal, MagicDNS, mDNS, DNS or the fallback */
        char ip[16];
        const char *how = "";
        if (cc_link_resolve_test(argv[2], ip, sizeof(ip), &how)) printf("host: %s -> %s (%s)\n", argv[2], ip, how);
        else printf("host: %s doesn't resolve (no fallback address)\n", argv[2]);
        return 0;
    }
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "server-host") == 0) {
        esp_err_t e = cc_link_set_server_host(argv[2], argc == 4 ? argv[3] : NULL);
        if (e == ESP_ERR_INVALID_ARG) {
            printf("companion: rejected: the host has no spaces; the fallback is a private IPv4 address or none\n");
            return 1;
        }
        if (e == ESP_ERR_INVALID_STATE) { printf("No Host set up\n"); return 1; }
        if (e != ESP_OK) { printf("companion: the server list didn't save\n"); return 1; }
        char fb[16];
        cc_link_lan_fallback(fb, sizeof(fb));
        printf("companion: Host entry host %s, fallback %s; reconnecting\n", argv[2], fb[0] ? fb : "none");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "watch") == 0) {
        char st[CC_STATUS_MAX];
        if (strcmp(argv[2], "on") == 0) cc_link_watch_enable(true);
        else if (strcmp(argv[2], "off") == 0) cc_link_watch_enable(false);
#if CONFIG_COMPANION_DEBUG_COMMANDS
        else if (strcmp(argv[2], "test-fallback") == 0) {
            cc_link_watch_test_fallback();
            printf("watch: the next start gives up on its banner after 1 ms\n");
            return 0;
        }
#endif
        else if (strcmp(argv[2], "status") != 0) {
            printf("usage: companion watch on|off|status" DBG_WATCH "\n");
            return 1;
        }
        printf("%s\n", cc_watch_status_text(st, sizeof(st)));
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "hostkey") == 0 && strcmp(argv[2], "forget") == 0) {
        char fp[80];
        esp_err_t e = cc_link_forget_host_key(fp, sizeof(fp), CC_FORGET_TIMEOUT_MS);
        if (e == ESP_OK) printf("New host key pinned: %s\n", fp);
        else if (e == ESP_ERR_TIMEOUT) printf("Old key forgotten; reconnecting...\n");
        else printf("No Host set up\n");
        return e == ESP_OK || e == ESP_ERR_TIMEOUT ? 0 : 1;
    }
#if CONFIG_COMPANION_DEBUG_COMMANDS
    if (argc == 3 && strcmp(argv[1], "hostkey") == 0 && strcmp(argv[2], "corrupt") == 0) {
        /* debug: shows the changed-key path without touching the Host */
        char hp[160];
        if (cc_link_debug_corrupt_host_key(hp, sizeof(hp)) != ESP_OK) { printf("No Host set up\n"); return 1; }
        printf("debug: host key pin for %s overwritten; reconnecting\n", hp);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "kick") == 0 && (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0)) {
        cc_link_set_kick(strcmp(argv[2], "on") == 0);   /* RAM only: Task 10 measures with and without */
        printf("companion: kick a handshake after a failed connect: %s\n", cc_link_kick_on() ? "on" : "off");
        return 0;
    }
#endif
    if (argc == 2 && strcmp(argv[1], "reconnect-log") == 0) {
        char *b = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
        if (!b) return 1;
        printf("%s\n", cc_link_reconnect_log(b, 1024));
        heap_caps_free(b);
        return 0;
    }
    printf("usage: companion host <server name> | companion claude-path <path> | companion status |\n"
           "       companion server-host <host> [fallback-ip|none] | companion resolve <host> |\n"
           "       companion hostcache [on|off|clear] | companion watch on|off|status | companion hostkey forget |\n"
           "       companion reconnect-log | companion voice-check |\n"
           "       companion voice-path|voice-model <path> | companion voice-paths\n"
#if CONFIG_COMPANION_DEBUG_COMMANDS
           "debug: companion takeover <id> | companion hostcache set <ip> | companion watch test-fallback |\n"
           "       companion hostkey corrupt | companion kick on|off | companion voice-upload-test <kb> [delay-ms]\n"
#endif
           );
    return 1;
}

static cc_session_t *snapshot(int *n)
{
    cc_session_t *s = heap_caps_malloc(sizeof(cc_session_t) * SNAP_CAP, MALLOC_CAP_SPIRAM);
    if (!s) { *n = -1; return NULL; }
    *n = cc_poll_snapshot(s, SNAP_CAP);
    return s;
}

static int cmd_sessions(int argc, char **argv)
{
    (void)argc; (void)argv;
    int n;
    cc_session_t *s = snapshot(&n);
    if (n < 0) {
        char st[CC_STATUS_MAX];
        printf("no session list yet (%s)\n", cc_link_status_text(st, sizeof(st)));
        free(s);
        return 1;
    }
    for (int i = 0; i < n; i++)
        printf("%s %s %s %s\n", state_name(s[i].state), s[i].id[0] ? s[i].id : "-", s[i].repo, s[i].name);
    if (n == 0) printf("no sessions\n");
    free(s);
    return 0;
}

static int cmd_reply(int argc, char **argv)
{
    if (argc < 3) { printf("usage: reply <id> <text...>\n"); return 1; }
    /* only background sessions have ids; interactive ones ("-") can't be answered */
    if (!cc_id_valid(argv[1]) && !cc_session_id_valid(argv[1])) { printf("no such session\n"); return 1; }
    int n;
    cc_session_t *s = snapshot(&n);
    const cc_session_t *hit = NULL;
    for (int i = 0; i < n; i++) {
        if (strcmp(s[i].id, argv[1]) == 0 || strcmp(s[i].session_id, argv[1]) == 0) { hit = &s[i]; break; }
    }
    int rc = 1;
    if (!hit) {
        printf("no such session\n");
    } else if (!hit->background) {
        printf("error: not a background session (run /bg in it first)\n");
    } else if (hit->state == CC_ENDED || !hit->live) {
        printf("error: session has stopped\n");
    } else {
        char *text = join_args(argc, argv, 2);
        char err[160];
        esp_err_t e = text ? cc_link_paste(hit->id, text, err, sizeof(err)) : ESP_ERR_NO_MEM;
        free(text);
        if (e == ESP_OK) { printf("sent\n"); rc = 0; }
        else if (e == ESP_ERR_NOT_FOUND && strcmp(err, "no such session") == 0) printf("no such session\n");
        else if (e == ESP_ERR_INVALID_STATE) { char st[CC_STATUS_MAX]; printf("error: %s\n", cc_link_status_text(st, sizeof(st))); }
        else printf("error: %s\n", err[0] ? err : esp_err_to_name(e));
    }
    free(s);
    return rc;
}

static int cmd_logs(int argc, char **argv)
{
    if (argc != 2) { printf("usage: logs <id>\n"); return 1; }
    char cmd[160], path[64];
    if (!cc_cmd_logs(cc_settings_claude_path(path, sizeof(path)), argv[1], cmd, sizeof(cmd))) {
        printf("error: bad session id (interactive sessions have none)\n");
        return 1;
    }
    char *out = heap_caps_malloc(LOGS_CAP, MALLOC_CAP_SPIRAM);
    if (!out) return 1;
    char err[160];
    int status = -1;
    esp_err_t e = cc_link_exec_tail(cmd, out, LOGS_CAP, &status, err, sizeof(err), 10000);
    int rc = 1;
    if (e == ESP_ERR_INVALID_STATE) {
        char st[CC_STATUS_MAX];
        printf("error: %s\n", cc_link_status_text(st, sizeof(st)));
    } else if (e != ESP_OK && e != ESP_ERR_INVALID_SIZE) {
        printf("error: %s\n", esp_err_to_name(e));
    } else {
        char *start = out;
        if (e == ESP_ERR_INVALID_SIZE) {
            /* only the tail was kept: don't start inside a UTF-8 character */
            while ((*start & 0xC0) == 0x80) start++;
        }
        memmove(out, start, strlen(start) + 1);
        cc_strip_ansi(out);
        printf("%s%s", out, (out[0] && out[strlen(out) - 1] != '\n') ? "\n" : "");
        if (err[0]) printf("error: %s\n", err);
        rc = status == 0 ? 0 : 1;
    }
    free(out);
    return rc;
}

static char s_left_ssid[33];

/* Test helpers for the reconnect check: leave and rejoin a saved network. */
static int cmd_wifi(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "leave") == 0) {
        if (wifi_mgr_is_connected()) strlcpy(s_left_ssid, wifi_mgr_ssid(), sizeof(s_left_ssid));
        wifi_mgr_disconnect();
        printf("wifi: left %s\n", s_left_ssid[0] ? s_left_ssid : "(none)");
        return 0;
    }
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "join") == 0) {
        const char *want = argc == 3 ? argv[2] : s_left_ssid;
        for (int i = 0; i < wifi_mgr_saved_count(); i++) {
            const wifi_saved_t *w = wifi_mgr_saved(i);
            if (w && (!want[0] || strcmp(w->ssid, want) == 0)) {
                wifi_mgr_connect(w->ssid, w->pass, false);
                printf("wifi: joining %s\n", w->ssid);
                return 0;
            }
        }
        printf("wifi: no saved network%s%s\n", want[0] ? " named " : "", want);
        return 1;
    }
#if CONFIG_COMPANION_DEBUG_COMMANDS
    if (argc == 3 && strcmp(argv[1], "test") == 0 && strcmp(argv[2], "drop") == 0) {
        /* debug: a drop the user didn't ask for, so the background reconnect takes it */
        wifi_mgr_test_drop();
        printf("wifi: test drop\n");
        return 0;
    }
#endif
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        char rc[96], ip[20];
        wifi_mgr_reconnect_text(rc, sizeof(rc));
        wifi_mgr_get_ip4(ip, sizeof(ip));
        wifi_static_ip_t sip;
        bool st = wifi_mgr_static_ip_get(&sip);
        printf("wifi: %s %s %s (ch %d, rssi %d); %s; address %s\n", wifi_mgr_is_connected() ? "connected" : "not connected",
               wifi_mgr_ssid(), ip, wifi_mgr_channel(), wifi_mgr_rssi(), rc, st ? "static" : "dhcp");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "static") == 0) {
        wifi_static_ip_t cur;
        bool on = wifi_mgr_static_ip_get(&cur);
        esp_ip4_addr_t a = { cur.ip }, m = { cur.mask }, g = { cur.gw }, d = { cur.dns };
        printf("wifi static: %s " IPSTR " / " IPSTR " gw " IPSTR " dns " IPSTR " (one address for every network)\n",
               on ? "on" : "off", IP2STR(&a), IP2STR(&m), IP2STR(&g), IP2STR(&d));
        return 0;
    }
    if ((argc == 4 || argc == 5) && strcmp(argv[1], "static") == 0) {
        /* wifi static <ip/prefix> <gateway> [dns]: the Settings row's rules (sip_parse), review m3 */
        char line[80];
        snprintf(line, sizeof(line), "%s %s%s%s", argv[2], argv[3], argc == 5 ? " " : "", argc == 5 ? argv[4] : "");
        sip_t p;
        if (!sip_parse(line, &p)) {
            printf("wifi static: use <address/prefix> <gateway> [dns], like 192.168.1.50/24 192.168.1.1\n");
            return 1;
        }
        wifi_static_ip_t st = { .on = 1, .ip = p.ip, .mask = p.mask, .gw = p.gw, .dns = p.dns };
        printf("wifi static: %s\n", wifi_mgr_static_ip_set(&st) ? "on (from the next join)" : "not saved");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "static") == 0 && strcmp(argv[2], "off") == 0) {
        /* review N2: a failed save leaves the static address on: say so */
        bool ok = wifi_mgr_static_ip_set(NULL);
        printf("wifi static: %s\n", ok ? "off (from the next join)" : "not saved (still on)");
        return ok ? 0 : 1;
    }
    printf("usage: wifi leave | wifi join [ssid] | wifi status | " DBG_WIFI
           "wifi static [off | <ip/prefix> <gw> [dns]]   (join uses the saved password)\n");
    return 1;
}

/* `ts term-test <tailnet host>`: prints the SSH terminal's refusal for a tailnet host with the switch off.
   Ruling L48 (final review M2): it runs only the client's pure first check (ts_host_term_refusal, the same
   call ssh_task makes before any socket) and starts no client, so no command line reaches any host. */
static int ts_term_test(const char *host)
{
    const char *refused = ts_host_term_refusal(host, tailscale_enabled());
    if (!refused) {
        printf("ts term-test: only for a tailnet host with the switch off\n");
        return 1;
    }
    printf("ts term-test %s: error: %s\n", host, refused);
    return 0;
}

/* The Tailscale switch: saved at once, sticky across every boot and wake. */
static int cmd_ts(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "term-test") == 0) return ts_term_test(argv[2]);
    if (argc == 2 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0)) {
        tailscale_set_enabled(strcmp(argv[1], "on") == 0);
    } else if (argc != 1 && !(argc == 2 && strcmp(argv[1], "status") == 0)) {
        printf("usage: ts on | ts off | ts [status] | ts term-test <tailnet host> (switch off only)\n");
        return 1;
    }
    const char *ip = tailscale_vpn_ip();
    printf("ts: %s (%s%s%s)\n", tailscale_enabled() ? "on" : "off", tailscale_state_text(), ip[0] ? " " : "", ip);
    /* set or not set only: the key itself is never printed (spec §3 rule 3) */
    int peers = tailscale_peer_count(), online = 0;
    for (int i = 0; i < peers; i++) {
        ts_peer_t p;
        if (tailscale_peer(i, &p) && p.online) online++;
    }
    printf("auth key: %s; peers: %d, %d online\n", g_settings.ts_auth_key[0] ? "set" : "not set", peers, online);
    /* what MicroLink costs: compare with the switch off and on */
    printf("heap: internal free %u B, min %u B, largest %u B\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return 0;
}

void cc_console_init(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "companion", .help = "companion host <server name> | claude-path <path> | status | server-host <host> [fallback-ip|none] | resolve <host> | hostcache [on|off|clear] | watch on|off|status | hostkey forget | reconnect-log | voice-check | voice-path|voice-model <path> | voice-paths" DBG_COMPANION, .func = cmd_companion },
        { .command = "sessions", .help = "List Claude sessions from the last poll: <STATE> <id> <repo> <name>", .func = cmd_sessions },
        { .command = "reply", .help = "reply <id> <text...>: type a quick answer into a running background session", .func = cmd_reply },
        { .command = "logs", .help = "logs <id>: recent output of a session, ANSI stripped", .func = cmd_logs },
        { .command = "wifi", .help = "wifi leave | wifi join [ssid] | wifi status | wifi static [off | <ip/prefix> <gw> [dns]]" DBG_WIFI_HELP, .func = cmd_wifi },
        { .command = "ts", .help = "ts on | ts off | ts [status] | ts term-test <host>: the Tailscale switch (saved; sticky across boots and wakes)", .func = cmd_ts },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) debug_console_register(&cmds[i]);
}
