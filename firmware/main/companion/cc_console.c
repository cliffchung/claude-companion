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

/* Console commands for the companion link: companion, sessions, reply, logs, wifi.
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
        return 0;
    }
    printf("usage: companion host <server name> | companion claude-path <path> | companion status\n");
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
    printf("usage: wifi leave | wifi join [ssid]   (join uses the saved password)\n");
    return 1;
}

void cc_console_init(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "companion", .help = "companion host <server name> | claude-path <path> | status", .func = cmd_companion },
        { .command = "sessions", .help = "List Claude sessions from the last poll: <STATE> <id> <repo> <name>", .func = cmd_sessions },
        { .command = "reply", .help = "reply <id> <text...>: type a quick answer into a running background session", .func = cmd_reply },
        { .command = "logs", .help = "logs <id>: recent output of a session, ANSI stripped", .func = cmd_logs },
        { .command = "wifi", .help = "wifi leave | wifi join [ssid]", .func = cmd_wifi },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) debug_console_register(&cmds[i]);
}
