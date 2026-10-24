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
#include "net/ts_host.h"
#include "ssh/ssh_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Console commands for the companion link: companion, sessions, reply, logs, wifi, ts.
 * Everything that reaches the Mac goes through cc_link and a cc_cmd_* builder. */

#define SNAP_CAP 128
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

static int cmd_companion(int argc, char **argv)
{
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
    if (argc == 3 && strcmp(argv[1], "resolve") == 0) {
        /* how the link would resolve a Mac host: literal, MagicDNS, mDNS, DNS or the fallback */
        char ip[16];
        const char *how = "";
        if (cc_link_resolve_test(argv[2], ip, sizeof(ip), &how)) printf("mac: %s -> %s (%s)\n", argv[2], ip, how);
        else printf("mac: %s doesn't resolve (no fallback address)\n", argv[2]);
        return 0;
    }
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "server-host") == 0) {
        esp_err_t e = cc_link_set_server_host(argv[2], argc == 4 ? argv[3] : NULL);
        if (e == ESP_ERR_INVALID_ARG) {
            printf("companion: rejected: the host has no spaces; the fallback is a private IPv4 address or none\n");
            return 1;
        }
        if (e == ESP_ERR_INVALID_STATE) { printf("No Mac set up\n"); return 1; }
        if (e != ESP_OK) { printf("companion: the server list didn't save\n"); return 1; }
        char fb[16];
        cc_link_lan_fallback(fb, sizeof(fb));
        printf("companion: Mac entry host %s, fallback %s; reconnecting\n", argv[2], fb[0] ? fb : "none");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "watch") == 0) {
        char st[CC_STATUS_MAX];
        if (strcmp(argv[2], "on") == 0) cc_link_watch_enable(true);
        else if (strcmp(argv[2], "off") == 0) cc_link_watch_enable(false);
        else if (strcmp(argv[2], "test-fallback") == 0) {
            cc_link_watch_test_fallback();
            printf("watch: the next start gives up on its banner after 1 ms\n");
            return 0;
        } else if (strcmp(argv[2], "status") != 0) {
            printf("usage: companion watch on|off|status|test-fallback\n");
            return 1;
        }
        printf("%s\n", cc_watch_status_text(st, sizeof(st)));
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "hostkey") == 0 && strcmp(argv[2], "forget") == 0) {
        char fp[80];
        esp_err_t e = cc_link_forget_host_key(fp, sizeof(fp), CC_FORGET_TIMEOUT_MS);
        if (e == ESP_OK) printf("New Mac key pinned: %s\n", fp);
        else if (e == ESP_ERR_TIMEOUT) printf("Old key forgotten; reconnecting...\n");
        else printf("No Mac set up\n");
        return e == ESP_OK || e == ESP_ERR_TIMEOUT ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "hostkey") == 0 && strcmp(argv[2], "corrupt") == 0) {
        /* debug: shows the changed-key path without touching the Mac */
        char hp[160];
        if (cc_link_debug_corrupt_host_key(hp, sizeof(hp)) != ESP_OK) { printf("No Mac set up\n"); return 1; }
        printf("debug: host key pin for %s overwritten; reconnecting\n", hp);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "kick") == 0 && (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0)) {
        cc_link_set_kick(strcmp(argv[2], "on") == 0);   /* RAM only: Task 10 measures with and without */
        printf("companion: kick a handshake after a failed connect: %s\n", cc_link_kick_on() ? "on" : "off");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "reconnect-log") == 0) {
        char *b = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
        if (!b) return 1;
        printf("%s\n", cc_link_reconnect_log(b, 1024));
        heap_caps_free(b);
        return 0;
    }
    printf("usage: companion host <server name> | companion claude-path <path> | companion status |\n"
           "       companion server-host <host> [fallback-ip|none] | companion resolve <host> |\n"
           "       companion watch on|off|status|test-fallback | companion hostkey forget|corrupt |\n"
           "       companion kick on|off | companion reconnect-log\n");
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
    if (argc == 3 && strcmp(argv[1], "test") == 0 && strcmp(argv[2], "drop") == 0) {
        /* debug: a drop the user didn't ask for, so the background reconnect takes it */
        wifi_mgr_test_drop();
        printf("wifi: test drop\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        char rc[96];
        wifi_mgr_reconnect_text(rc, sizeof(rc));
        printf("wifi: %s %s; %s\n", wifi_mgr_is_connected() ? "connected" : "not connected", wifi_mgr_ssid(), rc);
        return 0;
    }
    printf("usage: wifi leave | wifi join [ssid] | wifi status | wifi test drop   (join uses the saved password)\n");
    return 1;
}

/* `ts term-test <tailnet host>`: runs the SSH terminal's client on a tailnet host with the switch off and
   prints its error. Only for that case: it never connects anywhere (no session, no login shell). */
static volatile int s_tt_state = -1;
static char s_tt_msg[160];
static void tt_state_cb(ssh_state_t st, const char *msg, void *user)
{
    (void)user;
    if (msg) strlcpy(s_tt_msg, msg, sizeof(s_tt_msg));
    s_tt_state = st;
}
static int ts_term_test(const char *host)
{
    if (tailscale_enabled() || !ts_host_needs_tailscale(host)) {
        printf("ts term-test: only for a tailnet host with the switch off\n");
        return 1;
    }
    ssh_params_t p = { .port = 22, .use_key = true, .cols = 80, .rows = 24 };
    strlcpy(p.host, host, sizeof(p.host));
    strlcpy(p.user, "nobody", sizeof(p.user));
    strlcpy(p.cmd, "true", sizeof(p.cmd));
    s_tt_state = -1; s_tt_msg[0] = 0;
    ssh_client_t *c = ssh_client_start(&p, NULL, tt_state_cb, NULL, NULL);
    if (!c) { printf("ts term-test: no SSH task\n"); return 1; }
    for (int i = 0; i < 50 && s_tt_state != SSH_ST_ERROR && s_tt_state != SSH_ST_CLOSED; i++) vTaskDelay(pdMS_TO_TICKS(20));
    ssh_client_stop(c);
    printf("ts term-test %s: %s: %s\n", host, s_tt_state == SSH_ST_ERROR ? "error" : "no error", s_tt_msg);
    return s_tt_state == SSH_ST_ERROR ? 0 : 1;
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
    /* what MicroLink costs: compare with the switch off and on */
    printf("heap: internal free %u B, min %u B, largest %u B\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return 0;
}

void cc_console_init(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "companion", .help = "companion host <server name> | claude-path <path> | status | server-host <host> [fallback-ip|none] | resolve <host> | watch on|off|status|test-fallback | hostkey forget | hostkey corrupt (debug) | kick on|off | reconnect-log", .func = cmd_companion },
        { .command = "sessions", .help = "List Claude sessions from the last poll: <STATE> <id> <repo> <name>", .func = cmd_sessions },
        { .command = "reply", .help = "reply <id> <text...>: type a quick answer into a running background session", .func = cmd_reply },
        { .command = "logs", .help = "logs <id>: recent output of a session, ANSI stripped", .func = cmd_logs },
        { .command = "wifi", .help = "wifi leave | wifi join [ssid] | wifi status | wifi test drop (debug)", .func = cmd_wifi },
        { .command = "ts", .help = "ts on | ts off | ts [status] | ts term-test <host>: the Tailscale switch (saved; sticky across boots and wakes)", .func = cmd_ts },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) debug_console_register(&cmds[i]);
}
