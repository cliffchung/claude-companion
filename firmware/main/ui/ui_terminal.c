/* Full-screen terminal: the take-over of a Claude session on the Host, or an SSH session to another box
 * (spec §5.2, §5.4). One session bar over the top of the terminal; every key goes to the remote side except
 * Ctrl+] / Ctrl+Q (Leave). */
#include "ui.h"
#include "ui_comp.h"
#include "ui_softkbd.h"
#include "servers/server_store.h"
#include "ssh/ssh_client.h"
#include "term/vt.h"
#include "term/term_view.h"
#include "term/term_keys.h"
#include "ui/hotkeys.h"
#include "input/keyboard.h"
#include "companion/cc_cmd.h"
#include "companion/cc_link.h"
#include "net/ts_host.h"
#include "net/wifi_mgr.h"
#include "sys/power.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "term_ui";
#define TAIL_CAP 2048
#define PUMP_MS 16              /* the pty pump (B1: exempt; live sessions only) */
#define BAR_SHOW_MS 3000        /* the session bar shows this long, then hides */
#define BAR_TAP_ZONE 80         /* a tap this near the top shows the bar again */

/* Command mode (ui_terminal_open_command): one remote command on a pty, full screen.
 * Ctrl+] (and the old Ctrl+Q) is taken here and never sent (hotkeys.c); leaving, or the
 * command ending, returns to the screen below with a toast. */
typedef struct {
    char cmd[256];
    char title[96];         /* the session's name, for the bar */
    void (*on_close)(void *ctx);
    void *ctx;
} term_cmd_t;
static term_cmd_t s_pending_cmd;

typedef enum { BAR_CONNECTING, BAR_LIVE, BAR_LINK_LOST } bar_state_t;

typedef struct {
    lv_obj_t *scr, *bar, *bar_dot, *bar_state, *kbd;
    term_view_t *tv;
    vt_t *vt;
    ssh_client_t *ssh;
    int server_idx;
    char name[96];              /* the session's or the host's name, in the bar */
    bool kbd_shown;
    lv_timer_t *timer;          /* the pty pump: made at open, deleted in scr_delete_cb */
    lv_timer_t *bar_timer;      /* the bar's 3 s hide: one-shot, NULL once it has run */
    bar_state_t bar_st;
    bool connected, link_lost;
    /* thread hand-off */
    SemaphoreHandle_t lock;
    uint8_t *rx; size_t rx_len, rx_cap;
    volatile ssh_state_t pending_state; volatile bool state_dirty; char state_msg[160];
    /* host key prompt */
    SemaphoreHandle_t hk_sem; volatile int hk_answer; char hk_fp[120]; bool hk_changed;
    bool closing;
    /* command mode (the take-over) */
    bool cmd_mode, cmd_done, got_data;
    void (*on_close)(void *ctx); void *close_ctx;
    char *tail; size_t tail_len;    /* newest output, for the error line */
    int64_t t_open_us;
} term_ctx_t;

static term_ctx_t *s_ctx = NULL;
bool g_ssh_session_active = false;

/* An SSH session is up: also the power hold (it counts while "Stay awake in Terminal" is on).
   A take-over (command mode) has its own hold, POWER_HOLD_TAKEOVER, set by the dashboard. */
static void ssh_active(const term_ctx_t *c, bool on)
{
    g_ssh_session_active = on;
    if (!on || !c->cmd_mode) power_hold(POWER_HOLD_SSH, on);
}

/* -------------------------------------------------------- ssh callbacks (ssh task) */
static void on_data(const uint8_t *d, size_t n, void *user)
{
    term_ctx_t *c = user;
    xSemaphoreTake(c->lock, portMAX_DELAY);
    if (c->rx_len + n > c->rx_cap) {
        size_t nc = c->rx_cap * 2; while (nc < c->rx_len + n) nc *= 2;
        if (nc > 1 << 20) { c->rx_len = 0; nc = c->rx_cap; }   /* drop on overflow */
        else { c->rx = realloc(c->rx, nc); c->rx_cap = nc; }
    }
    if (c->rx_len + n <= c->rx_cap) { memcpy(c->rx + c->rx_len, d, n); c->rx_len += n; }
    if (c->tail) {
        size_t k = n > TAIL_CAP - 1 ? TAIL_CAP - 1 : n;
        const uint8_t *src = d + (n - k);
        if (c->tail_len + k > TAIL_CAP - 1) {
            size_t drop = c->tail_len + k - (TAIL_CAP - 1);
            memmove(c->tail, c->tail + drop, c->tail_len - drop);
            c->tail_len -= drop;
        }
        memcpy(c->tail + c->tail_len, src, k);
        c->tail_len += k;
        c->tail[c->tail_len] = 0;
    }
    xSemaphoreGive(c->lock);
}
static void on_state(ssh_state_t st, const char *msg, void *user)
{
    term_ctx_t *c = user;
    xSemaphoreTake(c->lock, portMAX_DELAY);
    c->pending_state = st; c->state_dirty = true;
    if (msg) strlcpy(c->state_msg, msg, sizeof(c->state_msg)); else c->state_msg[0] = 0;
    xSemaphoreGive(c->lock);
}
static void hk_confirm_cb(bool yes, void *ud) { term_ctx_t *c = ud; c->hk_answer = yes ? 1 : 2; xSemaphoreGive(c->hk_sem); }
static void hk_show(void *arg)
{
    term_ctx_t *c = arg;
    char txt[320];
    snprintf(txt, sizeof(txt), "%s\n\nSHA256:%s", tr(c->hk_changed ? STR_SSH_HOSTKEY_CHANGED : STR_SSH_HOSTKEY_NEW), c->hk_fp);
    ui_confirm(tr(STR_CONNECT), txt, hk_confirm_cb, c);
}
static bool on_hostkey(const char *host, int port, const char *fp, bool changed, void *user)
{
    term_ctx_t *c = user;
    (void)host; (void)port;
    strlcpy(c->hk_fp, fp, sizeof(c->hk_fp)); c->hk_changed = changed; c->hk_answer = 0;
    ui_async(hk_show, c);
    xSemaphoreTake(c->hk_sem, portMAX_DELAY);
    return c->hk_answer == 1;
}

/* -------------------------------------------------------- vt callbacks */
static void vt_write_cb(const uint8_t *d, size_t n, void *user) { term_ctx_t *c = user; if (c->ssh) ssh_client_send(c->ssh, d, n); }
static void vt_bell_cb(void *user) { (void)user; }
static void vt_title_cb(const char *title, void *user) { (void)title; (void)user; }   /* the bar shows the name */

/* -------------------------------------------------------- command mode */
/* Leaves the command session: back to the screen below, then the toast and the owner's
 * callback. c may be freed by the navigation, so nothing touches it afterwards. */
static void cmd_close(term_ctx_t *c, const char *toast)
{
    if (c->cmd_done) return;
    c->cmd_done = true;
    void (*cb)(void *) = c->on_close;
    void *ctx = c->close_ctx;
    c->on_close = NULL;
    char msg[200];
    strlcpy(msg, toast ? toast : "", sizeof(msg));
    ui_nav_back();
    if (msg[0]) ui_toast(msg);
    if (cb) cb(ctx);
}

/* The last readable line the command printed. The brief asks for the first stderr line,
 * but on a pty stderr and stdout arrive merged on one stream, so the last readable line
 * is the closest stand-in (ruling R21). */
static void cmd_error_line(term_ctx_t *c, char *out, size_t cap)
{
    out[0] = 0;
    xSemaphoreTake(c->lock, portMAX_DELAY);
    char *t = c->tail ? strndup(c->tail, c->tail_len) : NULL;
    xSemaphoreGive(c->lock);
    if (!t) return;
    char *start = t;
    while ((*start & 0xC0) == 0x80) start++;   /* don't begin inside a UTF-8 character */
    if (cc_log_tail(start, 1) > 0) strlcpy(out, start, cap);
    free(t);
}

/* -------------------------------------------------------- session bar */
static void bar_hide_cb(lv_timer_t *t)
{
    term_ctx_t *c = lv_timer_get_user_data(t);
    c->bar_timer = NULL;   /* one-shot: LVGL deletes it after this call */
    if (c->bar_st == BAR_LIVE && !lv_obj_has_flag(c->bar, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(c->bar, LV_OBJ_FLAG_HIDDEN);
}

/* Shows the bar; while the session is live it hides again after ms (one one-shot timer, re-armed by a
 * later show). Connecting and a lost link keep it up with no timer. */
static void bar_show(term_ctx_t *c, uint32_t ms)
{
    if (c->closing) return;
    if (lv_obj_has_flag(c->bar, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(c->bar, LV_OBJ_FLAG_HIDDEN);
    if (c->bar_st != BAR_LIVE) {
        if (c->bar_timer) { lv_timer_delete(c->bar_timer); c->bar_timer = NULL; }
        return;
    }
    if (c->bar_timer) {
        lv_timer_set_period(c->bar_timer, ms);
        lv_timer_reset(c->bar_timer);
    } else {
        c->bar_timer = lv_timer_create(bar_hide_cb, ms, c);
        if (c->bar_timer) lv_timer_set_repeat_count(c->bar_timer, 1);
    }
}

/* The bar's state from the session and the link: "Connecting…" muted until the session is up, a red
 * "Link lost, reconnecting…" while the link is down. Only a change writes anything. */
static void bar_update(term_ctx_t *c)
{
    bar_state_t st = !c->connected ? BAR_CONNECTING : c->link_lost ? BAR_LINK_LOST : BAR_LIVE;
    if (st == c->bar_st) return;
    c->bar_st = st;
    ui_dot_tone(c->bar_dot, st == BAR_LIVE ? UI_TONE_GREEN : st == BAR_LINK_LOST ? UI_TONE_RED : UI_TONE_MUTED);
    ui_text_tone(c->bar_state, st == BAR_LINK_LOST ? UI_TONE_RED : UI_TONE_MUTED);
    lv_label_set_text(c->bar_state, st == BAR_CONNECTING ? tr(STR_HOSTS_CONNECTING)
                                    : st == BAR_LINK_LOST ? tr(STR_TERM_LINK_LOST) : "");
    bar_show(c, BAR_SHOW_MS);
}

/* The link this session rides on: Wi-Fi, and for a take-over the Host link too. */
static bool link_down(const term_ctx_t *c)
{
    return !wifi_mgr_is_connected() || (c->cmd_mode && !cc_link_connected());
}

/* -------------------------------------------------------- leaving */
/* The confirm's answer. The session may have ended while it was up (its screen gone): s_ctx says. */
static void disconnect_cb(bool yes, void *ud)
{
    (void)ud;
    if (yes && s_ctx && !s_ctx->closing) ui_nav_back();
}

/* "← Leave" and Ctrl+]: a take-over closes at once (the session keeps running on the Host); an SSH session
 * asks first. While the confirm is up, keys go to it (ui_keyboard_route), never to the remote side. */
static void leave(term_ctx_t *c)
{
    if (c->cmd_mode) { cmd_close(c, tr(STR_TERM_STILL_RUNNING)); return; }
    char q[160];
    snprintf(q, sizeof(q), tr(STR_TERM_DISCONNECT_Q), c->name);
    ui_confirm_ex(q, NULL, tr(STR_DISCONNECT), false, disconnect_cb, NULL);
}

/* -------------------------------------------------------- key sink */
static void send_key(term_ctx_t *c, const key_event_t *e)
{
    char buf[32];
    size_t n = term_keys_encode(e, c->vt, buf, sizeof(buf));
    if (n && c->ssh) { ssh_client_send(c->ssh, (const uint8_t *)buf, n); vt_scroll_view_reset(c->vt); }
}

static bool term_key(const key_event_t *ev, void *user)
{
    term_ctx_t *c = user;
    hk_term_t hk = hk_takeover(ev, c->cmd_mode);
    if (hk == HK_T_LEAVE) { leave(c); return true; }   /* never sent; c may be gone */
    if (hk == HK_T_SCROLL_UP || hk == HK_T_SCROLL_DOWN) {
        vt_scroll_view(c->vt, hk == HK_T_SCROLL_UP ? vt_rows(c->vt) / 2 : -vt_rows(c->vt) / 2);
        lv_obj_invalidate(term_view_obj(c->tv));
        return true;
    }
    send_key(c, ev);   /* no key shows the bar: every key belongs to the remote side (§5.2) */
    return true;
}

/* -------------------------------------------------------- layout */
static void layout(term_ctx_t *c)
{
    int W = lv_display_get_horizontal_resolution(NULL), H = lv_display_get_vertical_resolution(NULL);
    int h = H;
    if (c->kbd_shown && softkbd_visible()) h -= softkbd_height();
    lv_obj_set_pos(term_view_obj(c->tv), 0, 0);   /* the bar lies over the top: showing it resizes nothing */
    lv_obj_set_size(term_view_obj(c->tv), W, h);
    int cols, rows;
    if (term_view_fit(c->tv, &cols, &rows)) {
        vt_resize(c->vt, cols, rows);
        if (c->ssh) ssh_client_resize(c->ssh, cols, rows);
    }
    lv_obj_invalidate(term_view_obj(c->tv));
}

/* -------------------------------------------------------- bar controls */
typedef struct { str_id_t label; key_event_t ev; } bar_key_t;
static const bar_key_t k_bar_keys[] = {
    { STR_TERM_KEY_ESC,    { .type = KEY_EV_SPECIAL, .key = SK_ESC } },
    { STR_TERM_KEY_TAB,    { .type = KEY_EV_SPECIAL, .key = SK_TAB } },
    { STR_TERM_KEY_CTRL_C, { .type = KEY_EV_CHAR, .cp = 'c', .mods = MOD_CTRL } },
    { STR_TERM_KEY_UP,     { .type = KEY_EV_SPECIAL, .key = SK_UP } },
    { STR_TERM_KEY_DOWN,   { .type = KEY_EV_SPECIAL, .key = SK_DOWN } },
};

static void key_chip_cb(lv_event_t *e)
{
    term_ctx_t *c = s_ctx;
    const bar_key_t *k = lv_event_get_user_data(e);
    if (!c || c->closing) return;
    send_key(c, &k->ev);
    bar_show(c, BAR_SHOW_MS);   /* in use: keep it up */
}

static void leave_cb(lv_event_t *e) { (void)e; if (s_ctx && !s_ctx->closing) leave(s_ctx); }

static void kbd_cb(lv_event_t *e)
{
    (void)e;
    term_ctx_t *c = s_ctx;
    if (!c || c->closing) return;
    if (c->kbd_shown) { softkbd_hide_all(); c->kbd = NULL; c->kbd_shown = false; }
    else { c->kbd = softkbd_create(c->scr); c->kbd_shown = true; }
    layout(c);
    bar_show(c, BAR_SHOW_MS);
}

/* 56 px over the top of the terminal: ← Leave, the name, the link dot, the state, ⌨ (no physical keyboard
 * only), then Esc, Tab, ^C, ↑ and ↓. Every control is a bar chip: the bar's full height, >= 80 px across. */
static lv_obj_t *session_bar(term_ctx_t *c, const char *name)
{
    lv_obj_t *b = ui_session_bar(c->scr);
    ui_chip(b, tr(STR_TERM_LEAVE), leave_cb, c);
    lv_obj_t *n = ui_text(b, name, UI_TONE_FG, false);
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(n, 400, 0);
    lv_obj_set_height(n, lv_font_get_line_height(FONT_BODY));
    c->bar_dot = ui_dot(b);
    c->bar_state = ui_text(b, "", UI_TONE_MUTED, true);
    lv_obj_set_width(c->bar_state, 1);
    lv_obj_set_flex_grow(c->bar_state, 1);
    lv_label_set_long_mode(c->bar_state, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(c->bar_state, lv_font_get_line_height(FONT_SMALL));
    if (!keyboard_any_present()) ui_chip(b, LV_SYMBOL_KEYBOARD, kbd_cb, c);
    for (size_t i = 0; i < sizeof(k_bar_keys) / sizeof(k_bar_keys[0]); i++)
        ui_chip(b, tr(k_bar_keys[i].label), key_chip_cb, (void *)&k_bar_keys[i]);
    return b;
}

/* A tap in the top 80 px shows the bar again. Touch never sends a byte; the tap that wakes the screen never
 * gets here (power.c swallows it). A drag scrolls the scrollback instead. */
static void term_tap_cb(lv_event_t *e)
{
    term_ctx_t *c = lv_event_get_user_data(e);
    lv_indev_t *in = lv_indev_active();
    if (!in || c->closing || !term_view_long_press_allowed(c->tv)) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (p.y < BAR_TAP_ZONE) bar_show(c, BAR_SHOW_MS);
}

/* The terminal keeps the horizontal axis to itself: a stray swipe must not walk out of a live session, and
 * dragging already scrolls the scrollback. Leaving is "← Leave" or Ctrl+]. */
static void term_gesture_cb(lv_dir_t dir) { (void)dir; }

/* -------------------------------------------------------- periodic pump */
static void pump_cb(lv_timer_t *t)
{
    term_ctx_t *c = lv_timer_get_user_data(t);
    if (!c || c->closing) return;
    xSemaphoreTake(c->lock, portMAX_DELAY);
    size_t n = c->rx_len; uint8_t *buf = NULL;
    if (n) { buf = malloc(n); memcpy(buf, c->rx, n); c->rx_len = 0; }
    bool sd = c->state_dirty; ssh_state_t st = c->pending_state; char msg[160]; strlcpy(msg, c->state_msg, sizeof(msg));
    c->state_dirty = false;
    xSemaphoreGive(c->lock);
    if (buf) {
        if (c->cmd_mode && !c->got_data) {
            c->got_data = true;
            ESP_LOGI(TAG, "command session: first output %lld ms after open",
                     (long long)((esp_timer_get_time() - c->t_open_us) / 1000));
        }
        vt_feed(c->vt, buf, n); free(buf); term_view_refresh(c->tv);
    }
    if (sd && st == SSH_ST_CONNECTED && !c->connected) {
        c->connected = true;
        ssh_active(c, true);
        if (c->cmd_mode)
            ESP_LOGI(TAG, "command session: connected %lld ms after open",
                     (long long)((esp_timer_get_time() - c->t_open_us) / 1000));
    }
    if (sd && c->cmd_mode && (st == SSH_ST_CLOSED || st == SSH_ST_ERROR)) {
        ssh_active(c, false);
        char line[200];
        int code = ssh_client_exit_status(c->ssh);
        if (st == SSH_ST_ERROR) {
            snprintf(line, sizeof(line), "%s %s", tr(STR_SSH_CONN_FAILED), msg);
        } else if (code == 0) {
            strlcpy(line, tr(STR_TERM_SESSION_CLOSED), sizeof(line));
        } else {
            cmd_error_line(c, line, sizeof(line));
            if (!line[0]) snprintf(line, sizeof(line), tr(STR_TERM_SESSION_ENDED), code);
        }
        ESP_LOGI(TAG, "command session ended: state %d, exit %d: %s", (int)st, code, line);
        cmd_close(c, line);
        return;   /* c is gone */
    }
    if (sd && !c->cmd_mode && (st == SSH_ST_CLOSED || st == SSH_ST_ERROR)) {
        /* An SSH session that ends goes back to Terminal. A failure is shown in red on the host's row
           (§5.3: the Host-key refusal and B6's missing key file included); an end after a live session
           also toasts. */
        bool was_live = c->connected;
        ssh_active(c, false);
        if (st == SSH_ST_ERROR) ui_hosts_note_failure(c->server_idx, msg);
        char line[160] = "";
        if (was_live) snprintf(line, sizeof(line), tr(STR_TERM_DISCONNECTED_FROM), c->name);
        ESP_LOGI(TAG, "ssh session ended: state %d%s%s", (int)st, msg[0] ? ": " : "", msg);
        ui_nav_back();
        if (line[0]) ui_toast(line);
        return;   /* c is gone */
    }
    bool lost = c->connected && link_down(c);
    if (lost != c->link_lost) c->link_lost = lost;
    bar_update(c);
}

/* -------------------------------------------------------- lifecycle */
static void scr_delete_cb(lv_event_t *e)
{
    term_ctx_t *c = lv_event_get_user_data(e);
    c->closing = true;
    keyboard_clear_sink(c);
    /* left some other way (e.g. navigation home): still tell the owner */
    if (c->cmd_mode && !c->cmd_done && c->on_close) { c->cmd_done = true; c->on_close(c->close_ctx); }
    if (c->timer) lv_timer_delete(c->timer);
    if (c->bar_timer) lv_timer_delete(c->bar_timer);   /* armed when the screen died */
    if (c->ssh) { ssh_client_stop(c->ssh); c->ssh = NULL; }
    ssh_active(c, false);
    /* term view frees itself with the object; vt after */
    vt_destroy(c->vt);
    free(c->rx);
    free(c->tail);
    vSemaphoreDelete(c->lock);
    vSemaphoreDelete(c->hk_sem);
    if (s_ctx == c) s_ctx = NULL;
    free(c);
}

static void back_async(void *arg) { (void)arg; ui_nav_back(); }

static lv_obj_t *term_create(int idx, const term_cmd_t *cmd)
{
    const server_t *s = server_store_get(idx);
    if (!s) return ui_hosts_create(NULL);
    /* No interactive login shell on the companion's Host: it would start the user's tmux and
       type into live sessions. Only the dashboard's pty exec (`claude attach`) goes there. */
    if ((!cmd || !cmd->cmd[0]) && cc_link_is_companion_server(idx)) {
        ESP_LOGW(TAG, "no login shell on the companion server '%s'", s->name);
        ui_toast(TERM_COMPANION_REFUSED);
        lv_async_call(back_async, NULL);
        return ui_screen_base();
    }
    /* Plan 3 (L25): a take-over of the companion's host connects to the address the link resolved and pinned
       (mDNS, DNS or the fallback), so it meets the link's pin and never asks about a key the link already
       trusts (pins are per address, P76). Review M6: only an address whose key the link checked and
       authenticated on; none yet (a reconnect in progress, a refusal) refuses the take-over. */
    char verified[128] = "";
    if (cmd && cc_link_is_companion_server(idx) &&
        !(cc_link_verified_address(verified, sizeof(verified)) && ts_host_is_ipv4(verified))) {
        ESP_LOGW(TAG, "take-over refused: the link hasn't verified the Host's address yet");
        ui_toast(tr(STR_TERM_NOT_VERIFIED));
        if (cmd->on_close) cmd->on_close(cmd->ctx);
        lv_async_call(back_async, NULL);
        return ui_screen_base();
    }
    term_ctx_t *c = calloc(1, sizeof(*c));
    s_ctx = c;
    c->server_idx = idx;
    strlcpy(c->name, cmd && cmd->title[0] ? cmd->title : s->name, sizeof(c->name));
    c->bar_st = BAR_LIVE;   /* bar_update below sets the first real state (connecting) */
    if (cmd) {
        c->cmd_mode = true;
        c->on_close = cmd->on_close;
        c->close_ctx = cmd->ctx;
        c->tail = malloc(TAIL_CAP);
        if (c->tail) c->tail[0] = 0;
        c->t_open_us = esp_timer_get_time();
    }
    c->lock = xSemaphoreCreateMutex();
    c->hk_sem = xSemaphoreCreateBinary();
    c->rx_cap = 16384; c->rx = malloc(c->rx_cap);
    c->scr = ui_screen_base();   /* bg: UI_TK_TERM_BG */
    lv_obj_add_event_cb(c->scr, scr_delete_cb, LV_EVENT_DELETE, c);

    int W = lv_display_get_horizontal_resolution(NULL), H = lv_display_get_vertical_resolution(NULL);
    fonts_set_term_line_height(UI_TK_TERM_LINE_H);
    int cols = W / 12, rows = H / UI_TK_TERM_LINE_H;   /* 12 px cells */
    c->vt = vt_create(cols, rows, 1500);
    vt_set_write_cb(c->vt, vt_write_cb, c);
    vt_set_bell_cb(c->vt, vt_bell_cb, c);
    vt_set_title_cb(c->vt, vt_title_cb, c);
    c->tv = term_view_create(c->scr, c->vt, &g_term_font);
    /* The token palette (P4-10); the saved terminal colours are no longer read. Claude Code's own colours are
       content, drawn as sent. A steady block cursor: no blink, so no blink timer (B1). */
    vt_set_palette(ui_tk_term_ansi);
    term_view_set_colors(c->tv, lv_color_hex(UI_TK_TERM_FG), lv_color_hex(UI_TK_TERM_BG));
    term_view_set_cursor(c->tv, TERM_CURSOR_BLOCK, false, lv_color_hex(UI_TK_TERM_CURSOR));
    ui_screen_set_gesture(c->scr, term_gesture_cb);
    /* RELEASED, not CLICKED: the terminal is content under the bar, not a control (the touch check's rule). */
    lv_obj_add_event_cb(term_view_obj(c->tv), term_tap_cb, LV_EVENT_RELEASED, c);
    c->bar = session_bar(c, c->name);
    layout(c);
    term_view_fit(c->tv, &cols, &rows);
    vt_resize(c->vt, cols, rows);
    bar_update(c);   /* "Connecting…": the bar stays up until the session is live, then hides after 3 s */

    char banner[320], what[200];
    if (c->cmd_mode) strlcpy(what, tr(STR_TERM_OPENING), sizeof(what));
    else snprintf(what, sizeof(what), tr(STR_TERM_CONNECTING_TO), s->user, s->host, s->port);
    snprintf(banner, sizeof(banner), "\x1b[36m%s\x1b[0m\r\n\x1b[90m%s\x1b[0m\r\n", what,
             tr(c->cmd_mode ? STR_TERM_OPENING_HINT : STR_TERM_SSH_HINT));
    vt_feed(c->vt, (const uint8_t *)banner, strlen(banner));

    key_sink_t sink = { .key = term_key, .user = c };
    keyboard_set_sink(&sink);

    ssh_params_t p = { 0 };
    strlcpy(p.host, s->host, sizeof(p.host)); p.port = s->port;
    if (verified[0]) strlcpy(p.host, verified, sizeof(p.host));   /* the take-over: see above (L25, M6) */
    strlcpy(p.user, s->user, sizeof(p.user)); strlcpy(p.pass, s->pass, sizeof(p.pass));
    strlcpy(p.keypath, s->keypath, sizeof(p.keypath)); p.use_key = s->use_key;
    p.cols = cols; p.rows = rows;
    if (cmd) strlcpy(p.cmd, cmd->cmd, sizeof(p.cmd));
    ESP_LOGI(TAG, "terminal %dx%d%s", cols, rows, cmd ? " (command mode)" : "");
    c->ssh = ssh_client_start(&p, on_data, on_state, on_hostkey, c);
    if (!c->ssh) {
        ESP_LOGE(TAG, "SSH task did not start (internal free %u)", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        on_state(SSH_ST_ERROR, "could not start the SSH task", c);
    }
    c->timer = lv_timer_create(pump_cb, PUMP_MS, c);   /* deleted in scr_delete_cb (B1) */
    return c->scr;
}

lv_obj_t *ui_terminal_create(void *arg)
{
    return term_create((int)(intptr_t)arg, NULL);
}

/* The nav stack can rebuild its top screen (a refresh, the screensaver). A command
 * terminal runs its command once: a rebuild must not attach again without the dashboard's
 * live check, so the pending command is used up here and an empty one goes back. */
static lv_obj_t *ui_terminal_cmd_create(void *arg)
{
    if (!s_pending_cmd.cmd[0]) {
        lv_async_call(back_async, NULL);
        return ui_screen_base();
    }
    lv_obj_t *scr = term_create((int)(intptr_t)arg, &s_pending_cmd);
    s_pending_cmd.cmd[0] = 0;
    s_pending_cmd.title[0] = 0;
    s_pending_cmd.on_close = NULL;
    s_pending_cmd.ctx = NULL;
    return scr;
}

bool ui_terminal_is_top(void)
{
    return ui_nav_is_top(ui_terminal_create) || ui_nav_is_top(ui_terminal_cmd_create);
}

void ui_terminal_open_command(int server_idx, const char *cmd, const char *title, void (*on_close)(void *ctx), void *ctx)
{
    if (!cmd || !cmd[0] || strlen(cmd) >= sizeof(s_pending_cmd.cmd) || !server_store_get(server_idx)) {
        ui_toast(tr(STR_TERM_CANT_OPEN));
        if (on_close) on_close(ctx);
        return;
    }
    strlcpy(s_pending_cmd.cmd, cmd, sizeof(s_pending_cmd.cmd));
    strlcpy(s_pending_cmd.title, title ? title : "", sizeof(s_pending_cmd.title));
    s_pending_cmd.on_close = on_close;
    s_pending_cmd.ctx = ctx;
    ui_nav_push(ui_terminal_cmd_create, (void *)(intptr_t)server_idx);
}
