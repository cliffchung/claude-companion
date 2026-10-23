/* Home screen: the Claude Code sessions on the companion Mac.
 *
 * Data comes from cc_link's poll (a subscriber only bumps a counter; the LVGL timer copies
 * the snapshot). Everything that talks to the Mac -- logs, quick answers, the live check
 * before opening -- runs on a small worker task, never on the LVGL task. Keys reach the
 * dashboard through a keyboard sink while it is the active screen.
 *
 * Colours are the spec's D9 tokens, independent of the fork's themes. Amber means
 * "needs you" and nothing else. */
#include "ui.h"
#include "ui_dashboard.h"
#include "ui_motion.h"
#include "ui_vitals.h"
#include "companion/cc_link.h"
#include "companion/cc_sessions.h"
#include "companion/cc_cmd.h"
#include "sys/sysinfo.h"
#include "net/wifi_mgr.h"
#include "net/tailscale.h"
#include "settings/settings.h"
#include "sys/power.h"
#include "input/keyboard.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#ifndef UI_PREVIEW
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "dash";

/* spec D9 */
#define C_BG     0x0e1116
#define C_PANEL  0x161b22
#define C_PANEL2 0x1b212b
#define C_LINE   0x2a323d
#define C_FG     0xe9e7e1
#define C_MUTED  0x8a94a2
#define C_AMBER  0xf4b545   /* needs you, nothing else */
#define C_GREEN  0x54d08a
#define C_RED    0xef6f6f
#define HEX(h)   lv_color_hex(h)

#define SNAP_CAP        128
#define MAX_CARDS       16
#define LOG_LINES       6
#define LOG_LINE_CHARS  96
#define LOGS_CAP        (16 * 1024)
#define LOG_TEXT_CAP    1024
#define ANSWER_MAX      480
#define LOGS_EVERY_MS   5000
#define UNREACHABLE_MS  30000
#define NOTE_TTL_MS     90000
#define VITALS_H        56
#define SUMMARY_H       46
#define FOOTER_H        52
#define KEY_MAX         96
#define NOTES_MAX       8
#define CHECK_MAX_AGE_MS 20000   /* an open check answered later than this is not acted on */

/* ------------------------------------------------------------------ state */

typedef enum { NOTE_NONE, NOTE_BUSY, NOTE_SENT, NOTE_APPROVE, NOTE_OPEN_TO_ANSWER, NOTE_ERROR } note_kind_t;
typedef struct {
    char key[KEY_MAX];
    note_kind_t kind;
    char text[160];
    cc_state_t state;      /* the card's state when the note was made */
    int64_t at_ms;
} note_t;

typedef enum { JOB_LOGS, JOB_PASTE, JOB_CHECK } job_type_t;
typedef struct {
    job_type_t type;
    unsigned token;        /* which request this is (actions only) */
    char id[16];
    char text[ANSWER_MAX + 1];
} job_t;

/* Worker results, read by the LVGL timer (guarded by res_lock). */
static struct { unsigned gen; unsigned token; job_type_t type; char id[16]; esp_err_t err; char msg[160]; } s_act;
static struct { unsigned gen; char id[16]; bool ok; char text[LOG_TEXT_CAP]; } s_logs;
static volatile bool s_logs_pending, s_act_pending;
static volatile unsigned s_poll_gen;

/* Model (LVGL task only). */
static cc_session_t *s_snap;               /* PSRAM: the whole poll */
static cc_session_t *s_items;              /* PSRAM: the visible cards */
static int s_n = -1;                       /* -1: no list yet */
static int s_sel;                          /* selected index */
static char s_sel_key[KEY_MAX];
static char s_draft[ANSWER_MAX + 1];
static bool s_msg_mode;                    /* R pressed on a card that isn't waiting */
static note_t s_notes[NOTES_MAX];
static unsigned s_seen_poll, s_seen_act, s_seen_logs;
static int64_t s_logs_at_ms;
static char s_logs_for[16];
static int64_t s_down_since_ms;            /* link down while it could be up */
static bool s_shown_live;                  /* cards were built for a connected link */
/* The request in flight: an answer to it is used only by the dashboard visit that asked,
   and an open check only while it is fresh (it can take up to the poll timeout). */
static unsigned s_req_token, s_visit, s_req_visit;
static int64_t s_req_at_ms;

/* Widgets (valid while the screen exists). */
static lv_obj_t *s_scr, *s_list, *s_summary, *s_vitals, *s_dot, *s_host;
static lv_obj_t *s_ages[MAX_CARDS], *s_cards[MAX_CARDS];
static lv_obj_t *s_logs_lbl, *s_ta;
static unsigned s_ticks;
static int s_view_shown = -1;   /* the pr_cards_t the list is styled for (-1: a new screen) */

/* LVGL's tick stops in Off (lvgl_port_stop), so TTLs and the open-check age use esp_timer on the
   device. The preview has no esp_timer and drives lv_tick itself. */
#ifdef UI_PREVIEW
static int64_t mono_ms(void) { return (int64_t)lv_tick_get(); }
#else
static int64_t mono_ms(void) { return esp_timer_get_time() / 1000; }
#endif

/* ------------------------------------------------------------------ worker */

static char *s_logs_buf;   /* worker only */

static void res_lock(void);
static void res_unlock(void);

static void run_job(const job_t *j)
{
    char claude[64], cmd[192], msg[160] = "";
    cc_settings_claude_path(claude, sizeof(claude));
    if (j->type == JOB_LOGS) {
        bool ok = false;
        int status = -1;
        if (s_logs_buf && cc_cmd_logs(claude, j->id, cmd, sizeof(cmd))) {
            esp_err_t e = cc_link_exec_tail(cmd, s_logs_buf, LOGS_CAP, &status, msg, sizeof(msg), 10000);
            ok = (e == ESP_OK || e == ESP_ERR_INVALID_SIZE) && status == 0;
            if (ok) {
                char *start = s_logs_buf;
                while ((*start & 0xC0) == 0x80) start++;   /* the tail may start mid-character */
                memmove(s_logs_buf, start, strlen(start) + 1);
                cc_log_tail(s_logs_buf, LOG_LINES);
            }
        }
        res_lock();
        s_logs.gen++;
        strlcpy(s_logs.id, j->id, sizeof(s_logs.id));
        s_logs.ok = ok;
        strlcpy(s_logs.text, ok ? s_logs_buf : msg, sizeof(s_logs.text));
        res_unlock();
        s_logs_pending = false;
        return;
    }
    esp_err_t e = j->type == JOB_PASTE ? cc_link_paste(j->id, j->text, msg, sizeof(msg))
                                       : cc_link_check_live(j->id, msg, sizeof(msg));
    ESP_LOGI(TAG, "%s %s: %s %s", j->type == JOB_PASTE ? "answer" : "open check", j->id, esp_err_to_name(e), msg);
    res_lock();
    s_act.gen++;
    s_act.token = j->token;
    s_act.type = j->type;
    strlcpy(s_act.id, j->id, sizeof(s_act.id));
    s_act.err = e;
    strlcpy(s_act.msg, msg, sizeof(s_act.msg));
    res_unlock();
    s_act_pending = false;
}

#ifdef UI_PREVIEW
/* The host preview has no tasks: jobs run at once against its stubs. */
static void res_lock(void) {}
static void res_unlock(void) {}
static bool job_post(const job_t *j) { run_job(j); return true; }
static void worker_start(void) {}
#else
static SemaphoreHandle_t s_res_mutex;
static QueueHandle_t s_jobs;
static void res_lock(void) { xSemaphoreTake(s_res_mutex, portMAX_DELAY); }
static void res_unlock(void) { xSemaphoreGive(s_res_mutex); }

static void worker_task(void *arg)
{
    (void)arg;
    job_t *j = heap_caps_malloc(sizeof(job_t), MALLOC_CAP_SPIRAM);
    for (;;) {
        if (j && xQueueReceive(s_jobs, j, portMAX_DELAY) == pdTRUE) run_job(j);
        else vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static bool job_post(const job_t *j)
{
    return s_jobs && xQueueSend(s_jobs, j, 0) == pdTRUE;
}

static void worker_start(void)
{
    s_res_mutex = xSemaphoreCreateMutex();
    s_jobs = xQueueCreate(4, sizeof(job_t));
    if (!s_res_mutex || !s_jobs ||
        xTaskCreatePinnedToCore(worker_task, "dash_work", 6144, NULL, 3, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "worker start failed");
        /* no worker: refuse jobs instead of queueing them forever */
        if (s_jobs) { vQueueDelete(s_jobs); s_jobs = NULL; }
    }
}
#endif

static void poll_cb(const cc_session_t *s, int n, void *ctx)
{
    (void)s; (void)n; (void)ctx;
    s_poll_gen++;   /* link task: only note it; the LVGL timer copies the snapshot */
}

static void init_once(void)
{
    static bool done;
    if (done) return;
    done = true;
    s_snap = heap_caps_calloc(SNAP_CAP, sizeof(cc_session_t), MALLOC_CAP_SPIRAM);
    s_items = heap_caps_calloc(MAX_CARDS, sizeof(cc_session_t), MALLOC_CAP_SPIRAM);
    s_logs_buf = heap_caps_calloc(1, LOGS_CAP, MALLOC_CAP_SPIRAM);
    worker_start();
}

/* The dashboard can be built before cc_link_start (it is the first screen), and
   cc_poll_subscribe does nothing until then, so subscribe once the link is up. */
static void subscribe_when_ready(void)
{
    static bool subscribed;
    if (subscribed || !cc_link_connected()) return;
    cc_poll_subscribe(poll_cb, NULL);
    subscribed = true;
    s_poll_gen++;
}

/* ------------------------------------------------------------------ helpers */

static const char *key_of(const cc_session_t *s) { return s->background && s->id[0] ? s->id : s->name; }
static const cc_session_t *selected(void) { return (s_n > 0 && s_sel >= 0 && s_sel < s_n) ? &s_items[s_sel] : NULL; }
static bool actionable(const cc_session_t *s)
{
    return s && s->background && s->id[0] && s->live && s->state != CC_ENDED;
}

static note_t *note_find(const char *key)
{
    for (int i = 0; i < NOTES_MAX; i++)
        if (s_notes[i].kind != NOTE_NONE && strcmp(s_notes[i].key, key) == 0) return &s_notes[i];
    return NULL;
}

static void note_set(const cc_session_t *s, note_kind_t kind, const char *text)
{
    note_t *n = note_find(key_of(s));
    if (!n) {
        n = &s_notes[0];
        for (int i = 0; i < NOTES_MAX; i++) {
            if (s_notes[i].kind == NOTE_NONE) { n = &s_notes[i]; break; }
            if (s_notes[i].at_ms < n->at_ms) n = &s_notes[i];
        }
    }
    strlcpy(n->key, key_of(s), sizeof(n->key));
    n->kind = kind;
    strlcpy(n->text, text ? text : "", sizeof(n->text));
    n->state = s->state;
    n->at_ms = mono_ms();
}

static void note_clear(const char *key)
{
    note_t *n = note_find(key);
    if (n) n->kind = NOTE_NONE;
}

/* A refusal note replaces the answer field: Enter then opens the session. */
static bool note_says_open(const cc_session_t *s)
{
    note_t *n = s ? note_find(key_of(s)) : NULL;
    return n && (n->kind == NOTE_APPROVE || n->kind == NOTE_OPEN_TO_ANSWER);
}

static bool field_visible(const cc_session_t *s)
{
    return actionable(s) && !note_says_open(s) && (s->state == CC_NEEDS_YOU || s_msg_mode);
}

static bool same_session(const cc_session_t *a, const cc_session_t *b)
{
    return a->state == b->state && a->live == b->live && a->background == b->background &&
           a->started_ms == b->started_ms && strcmp(a->id, b->id) == 0 &&
           strcmp(a->name, b->name) == 0 && strcmp(a->repo, b->repo) == 0;
}

static int64_t wall_ms(void)
{
    time_t t = time(NULL);
    return t > 1700000000 ? (int64_t)t * 1000 : 0;   /* 0 until the clock is set */
}

/* Cuts every line to LOG_LINE_CHARS characters so the box never wraps. */
static void clip_lines(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    int chars = 0;
    bool cut = false;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 8 < cap; p++) {
        if (*p == '\n') { out[o++] = '\n'; chars = 0; cut = false; continue; }
        if (cut) continue;
        if ((*p & 0xC0) != 0x80) {
            if (chars == LOG_LINE_CHARS) { memcpy(out + o, "...", 3); o += 3; cut = true; continue; }
            chars++;
        }
        out[o++] = (char)*p;
    }
    out[o] = 0;
}

/* A blocked session that is not live (or not a background one) is not amber and not "needs you". */
static const char *pill_text(const cc_session_t *s)
{
    cc_state_t st = s->state;
    if (st == CC_NEEDS_YOU && !cc_session_needs_you(s)) return s->background ? "STOPPED" : "WAITING";
    switch (st) {
    case CC_NEEDS_YOU: return "NEEDS YOU";
    case CC_WORKING:   return "WORKING";
    case CC_IDLE:      return "IDLE";
    case CC_DONE:      return "DONE";
    default:           return "ENDED";
    }
}

/* ------------------------------------------------------------------ widgets */

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const char *t, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t ? t : "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, HEX(color), 0);
    return l;
}

static lv_obj_t *row(lv_obj_t *parent, int gap)
{
    lv_obj_t *r = box(parent);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, gap, 0);
    return r;
}

static void pill(lv_obj_t *parent, const cc_session_t *sess)
{
    cc_state_t st = sess->state;
    bool amber = cc_session_needs_you(sess);
    lv_obj_t *p = box(parent);
    lv_obj_set_size(p, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(p, 12, 0);
    lv_obj_set_style_pad_ver(p, 3, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    uint32_t fg, bg, border;
    lv_opa_t bg_opa = LV_OPA_20;
    switch (st) {
    case CC_NEEDS_YOU:
        if (amber) { fg = C_BG; bg = C_AMBER; border = C_AMBER; bg_opa = LV_OPA_COVER; }
        else { fg = C_MUTED; bg = C_LINE; border = C_LINE; bg_opa = LV_OPA_40; }
        break;
    case CC_WORKING:   fg = C_GREEN; bg = C_GREEN; border = C_GREEN; break;
    case CC_ENDED:     fg = C_RED;   bg = C_RED;   border = C_RED;   break;
    default:           fg = C_MUTED; bg = C_LINE;  border = C_LINE;  bg_opa = LV_OPA_40; break;
    }
    lv_obj_set_style_bg_color(p, HEX(bg), 0);
    lv_obj_set_style_bg_opa(p, bg_opa, 0);
    lv_obj_set_style_border_color(p, HEX(border), 0);
    lv_obj_set_style_border_opa(p, amber ? LV_OPA_COVER : LV_OPA_50, 0);
    text(p, pill_text(sess), FONT_SMALL, fg);
}

static void select_index(int i);

static void card_click_cb(lv_event_t *e)
{
    select_index((int)(intptr_t)lv_event_get_user_data(e));
}

static void set_age(lv_obj_t *l, const cc_session_t *s)
{
    char a[16] = "";
    int64_t now = wall_ms();
    if (now && s->started_ms > 0) cc_age(now, s->started_ms, a, sizeof(a));
    if (strcmp(lv_label_get_text(l), a)) lv_label_set_text(l, a);
}

static void set_logs_text(void)
{
    const cc_session_t *s = selected();
    if (!s_logs_lbl || !s) return;
    static char clipped[LOG_TEXT_CAP + 64];
    res_lock();
    bool mine = strcmp(s_logs.id, s->id) == 0;
    bool ok = s_logs.ok;
    if (mine) clip_lines(s_logs.text, clipped, sizeof(clipped));
    res_unlock();
    if (!mine) lv_label_set_text(s_logs_lbl, "Loading recent output...");
    else if (!ok) lv_label_set_text(s_logs_lbl, clipped[0] ? clipped : "No output to show");
    else lv_label_set_text(s_logs_lbl, clipped[0] ? clipped : "No output yet");
}

static void build_expanded(lv_obj_t *card, const cc_session_t *s)
{
    if (s->background && s->id[0]) {
        lv_obj_t *lb = box(card);
        lv_obj_set_size(lb, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(lb, HEX(C_BG), 0);
        lv_obj_set_style_bg_opa(lb, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(lb, 8, 0);
        lv_obj_set_style_pad_hor(lb, 12, 0);
        lv_obj_set_style_pad_ver(lb, 8, 0);
        lv_obj_set_style_border_width(lb, 1, 0);
        lv_obj_set_style_border_color(lb, HEX(C_LINE), 0);
        s_logs_lbl = text(lb, "", FONT_MONO, C_MUTED);
        lv_obj_set_width(s_logs_lbl, LV_PCT(100));
        lv_label_set_long_mode(s_logs_lbl, LV_LABEL_LONG_CLIP);
        set_logs_text();
    }
    if (!s->background) {
        text(card, "On your Mac \xe2\x80\x94 run /bg there to take it over here.", FONT_SMALL, C_MUTED);
        return;
    }
    note_t *n = note_find(key_of(s));
    if (!actionable(s)) {
        /* an action that just failed says why (e.g. "Session has stopped") */
        if (n && n->kind == NOTE_ERROR) text(card, n->text, FONT_SMALL, C_RED);
        else text(card, "Stopped", FONT_SMALL, C_MUTED);
        return;
    }
    if (field_visible(s)) {
        s_ta = lv_textarea_create(card);
        lv_obj_set_width(s_ta, LV_PCT(100));
        lv_textarea_set_one_line(s_ta, true);
        lv_textarea_set_placeholder_text(s_ta, s->state == CC_NEEDS_YOU ? "Type an answer\xe2\x80\xa6" : "Type a message\xe2\x80\xa6");
        lv_textarea_set_text(s_ta, s_draft);
        lv_obj_set_style_text_font(s_ta, FONT_BODY, 0);
        lv_obj_set_style_bg_color(s_ta, HEX(C_BG), 0);
        lv_obj_set_style_bg_opa(s_ta, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_ta, HEX(C_FG), 0);
        lv_obj_set_style_text_color(s_ta, HEX(C_MUTED), LV_PART_TEXTAREA_PLACEHOLDER);
        /* the theme's focus colours (its accent) must not leak in: amber is for needs-you */
        lv_obj_set_style_border_color(s_ta, HEX(C_MUTED), LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(s_ta, HEX(C_MUTED), 0);
        lv_obj_set_style_border_width(s_ta, 1, LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(s_ta, 1, 0);
        lv_obj_set_style_outline_width(s_ta, 0, LV_STATE_FOCUSED);
        lv_obj_set_style_shadow_width(s_ta, 0, LV_STATE_FOCUSED);
        lv_obj_set_style_shadow_width(s_ta, 0, 0);
        lv_obj_set_style_radius(s_ta, 9, 0);
        lv_obj_set_style_bg_color(s_ta, HEX(C_FG), LV_PART_CURSOR);
        lv_obj_set_style_border_color(s_ta, HEX(C_FG), LV_PART_CURSOR);
        lv_obj_remove_flag(s_ta, LV_OBJ_FLAG_CLICKABLE);   /* keys come from the sink */
        lv_obj_add_state(s_ta, LV_STATE_FOCUSED);          /* shows the cursor */
    }
    lv_obj_t *r = row(card, 18);
    if (n && n->kind != NOTE_NONE) {
        const char *t = n->kind == NOTE_SENT ? "Sent" :
                        n->kind == NOTE_APPROVE ? "Open to approve" :
                        n->kind == NOTE_OPEN_TO_ANSWER ? "Open the session to answer" : n->text;
        text(r, t, FONT_SMALL, n->kind == NOTE_ERROR ? C_RED : C_MUTED);
    }
    const char *hint = field_visible(s) ? "Enter sends  \xc2\xb7  Ctrl+O opens the session" :
                       "Enter opens the session  \xc2\xb7  R sends a message";
    if (note_says_open(s)) hint = "Enter opens the session";
    text(r, hint, FONT_SMALL, C_MUTED);
}

static void build_card(int i)
{
    const cc_session_t *s = &s_items[i];
    bool sel = i == s_sel;
    lv_obj_t *card = lv_obj_create(s_list);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, HEX(C_PANEL2), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, HEX(cc_session_needs_you(s) ? C_AMBER : C_LINE), 0);
    lv_obj_set_style_border_opa(card, cc_session_needs_you(s) ? LV_OPA_60 : LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(card, 16, 0);
    lv_obj_set_style_pad_ver(card, 10, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    /* Ctrl+Tab focus: neutral, never the theme's orange accent (amber means "needs you") */
    lv_obj_set_style_outline_color(card, HEX(C_FG), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_opa(card, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
    if (sel) {
        lv_obj_set_style_outline_width(card, 2, 0);
        lv_obj_set_style_outline_color(card, HEX(C_FG), 0);
        lv_obj_set_style_outline_opa(card, LV_OPA_50, 0);
        lv_obj_set_style_outline_pad(card, 2, 0);
    }
    s_cards[i] = card;
    lv_obj_t *h = row(card, 16);
    lv_obj_t *name = text(h, s->name, FONT_BODY, C_FG);
    lv_obj_set_width(name, 1);
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_height(name, lv_font_get_line_height(FONT_BODY));
    lv_obj_t *repo = text(h, s->repo, FONT_SMALL, C_MUTED);
    lv_label_set_long_mode(repo, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(repo, 320, 0);
    lv_obj_set_height(repo, lv_font_get_line_height(FONT_SMALL));
    s_ages[i] = text(h, "", FONT_SMALL, C_MUTED);
    lv_obj_set_style_min_width(s_ages[i], 48, 0);
    lv_obj_set_style_text_align(s_ages[i], LV_TEXT_ALIGN_RIGHT, 0);
    set_age(s_ages[i], s);
    pill(h, s);
    if (sel) build_expanded(card, s);
}

static void rebuild(void)
{
    if (!s_list) return;
    lv_obj_clean(s_list);
    s_logs_lbl = s_ta = NULL;
    memset(s_cards, 0, sizeof(s_cards));
    memset(s_ages, 0, sizeof(s_ages));
    if (!s_shown_live || s_n <= 0) return;
    for (int i = 0; i < s_n; i++) build_card(i);
    if (s_cards[s_sel]) {
        lv_obj_update_layout(s_list);
        lv_obj_scroll_to_view(s_cards[s_sel], LV_ANIM_OFF);
    }
}

/* ------------------------------------------------------------------ actions */

static void request_logs(bool now)
{
    const cc_session_t *s = selected();
    if (!s || !s->background || !s->id[0] || s_logs_pending || !cc_link_connected()) return;
    if (!now && strcmp(s_logs_for, s->id) == 0 && mono_ms() - s_logs_at_ms < LOGS_EVERY_MS) return;
    job_t *j = calloc(1, sizeof(*j));
    if (!j) return;
    j->type = JOB_LOGS;
    strlcpy(j->id, s->id, sizeof(j->id));
    strlcpy(s_logs_for, s->id, sizeof(s_logs_for));
    s_logs_at_ms = mono_ms();
    s_logs_pending = true;
    if (!job_post(j)) s_logs_pending = false;
    free(j);
}

static void select_index(int i)
{
    if (s_n <= 0) return;
    if (i < 0) i = 0;
    if (i >= s_n) i = s_n - 1;
    if (i == s_sel && strcmp(s_sel_key, key_of(&s_items[i])) == 0) return;
    s_sel = i;
    strlcpy(s_sel_key, key_of(&s_items[i]), sizeof(s_sel_key));
    s_draft[0] = 0;
    s_msg_mode = false;
    rebuild();
    request_logs(true);
}

static void post_action(job_type_t type, const cc_session_t *s, const char *t)
{
    if (s_act_pending) { ui_toast("Still working on the last request"); return; }
    job_t *j = calloc(1, sizeof(*j));
    if (!j) return;
    j->type = type;
    j->token = ++s_req_token;
    s_req_visit = s_visit;
    s_req_at_ms = mono_ms();
    strlcpy(j->id, s->id, sizeof(j->id));
    if (t) strlcpy(j->text, t, sizeof(j->text));
    s_act_pending = true;
    note_set(s, NOTE_BUSY, type == JOB_PASTE ? "Sending\xe2\x80\xa6" : "Opening\xe2\x80\xa6");
    rebuild();
    if (!job_post(j)) {
        s_act_pending = false;
        note_set(s, NOTE_ERROR, "Busy, try again");
        rebuild();
    }
    free(j);
}

static void open_selected(void)
{
    const cc_session_t *s = selected();
    if (!actionable(s)) return;
    post_action(JOB_CHECK, s, NULL);   /* the terminal opens when the check passes */
}

static void session_closed_cb(void *ctx)
{
    (void)ctx;
    power_hold(POWER_HOLD_TAKEOVER, false);
    cc_poll_set_rate(3);
    s_logs_at_ms = 0;
}

static void open_now(const cc_session_t *s)
{
    char claude[64], cmd[192];
    /* the card as of the latest poll must still be one we may open */
    if (!actionable(s)) {
        note_set(s, NOTE_ERROR, "Session has stopped");
        rebuild();
        return;
    }
    int idx = cc_settings_server();
    if (idx < 0 || !cc_cmd_attach(cc_settings_claude_path(claude, sizeof(claude)), s->id, cmd, sizeof(cmd))) {
        note_set(s, NOTE_ERROR, "Can't open: set up your Mac in Settings");
        rebuild();
        return;
    }
    note_clear(key_of(s));
    ESP_LOGI(TAG, "opening %s", s->id);
    power_hold(POWER_HOLD_TAKEOVER, true);   /* a take-over never dims or sleeps */
    cc_poll_set_rate(15);   /* the watch keeps feeding the keyboard light; nothing redraws */
    ui_terminal_open_command(idx, cmd, session_closed_cb, NULL);
}

static void enter_pressed(void)
{
    const cc_session_t *s = selected();
    if (!actionable(s) || s_act_pending) return;
    if (field_visible(s)) {
        if (!s_draft[0]) {
            if (s->state == CC_NEEDS_YOU) ui_toast("Type an answer, or Ctrl+O to open the session");
            return;
        }
        post_action(JOB_PASTE, s, s_draft);
        return;
    }
    open_selected();
}

static void apply_action_result(void)
{
    res_lock();
    unsigned gen = s_act.gen, token = s_act.token;
    job_type_t type = s_act.type;
    char id[16], msg[160];
    strlcpy(id, s_act.id, sizeof(id));
    strlcpy(msg, s_act.msg, sizeof(msg));
    esp_err_t err = s_act.err;
    res_unlock();
    if (gen == s_seen_act) return;
    s_seen_act = gen;
    if (token != s_req_token) return;   /* an answer to an older request */
    const cc_session_t *s = NULL;
    for (int i = 0; i < s_n; i++)
        if (strcmp(s_items[i].id, id) == 0) { s = &s_items[i]; break; }
    if (!s) return;
    if (type == JOB_CHECK) {
        /* Open only for the visit that asked, and only while the check is fresh: the user
           may have left and come back minutes later, and the session may have stopped. */
        if (s_req_visit != s_visit || mono_ms() - s_req_at_ms > CHECK_MAX_AGE_MS) {
            ESP_LOGI(TAG, "dropping a stale open check for %s", id);
            note_clear(key_of(s));
            rebuild();
            return;
        }
        if (err == ESP_OK) { open_now(s); return; }
    } else if (err == ESP_OK) {
        note_set(s, NOTE_SENT, NULL);
        if (s == selected()) { s_draft[0] = 0; s_msg_mode = false; }
        s_logs_at_ms = 0;
        rebuild();
        return;
    } else if (err == CC_PASTE_NEEDS_APPROVAL) {
        note_set(s, NOTE_APPROVE, NULL); rebuild(); return;
    } else if (err == CC_PASTE_NOT_READY) {
        note_set(s, NOTE_OPEN_TO_ANSWER, NULL); rebuild(); return;
    }
    if (err == ESP_ERR_INVALID_STATE) strlcpy(msg, "Not connected to your Mac", sizeof(msg));
    else if (!msg[0]) strlcpy(msg, esp_err_to_name(err), sizeof(msg));
    if (msg[0] >= 'a' && msg[0] <= 'z') msg[0] = (char)(msg[0] - 'a' + 'A');
    note_set(s, NOTE_ERROR, msg);
    rebuild();
}

/* ------------------------------------------------------------------ keys */

static void draft_changed(void)
{
    if (s_ta) {
        lv_textarea_set_text(s_ta, s_draft);
        lv_textarea_set_cursor_pos(s_ta, LV_TEXTAREA_CURSOR_LAST);
    }
}

static void draft_add(const char *utf8)
{
    if (strlen(s_draft) + strlen(utf8) > ANSWER_MAX) return;
    strlcat(s_draft, utf8, sizeof(s_draft));
    draft_changed();
}

static bool dash_key(const key_event_t *ev, void *user)
{
    (void)user;
    if (!s_shown_live) return true;   /* no cards on screen: nothing to act on */
    const cc_session_t *s = selected();
    if (ev->type == KEY_EV_SPECIAL) {
        switch (ev->key) {
        case SK_UP:   select_index(s_sel - 1); break;
        case SK_DOWN: select_index(s_sel + 1); break;
        case SK_PGUP: select_index(s_sel - 4); break;
        case SK_PGDN: select_index(s_sel + 4); break;
        case SK_HOME: select_index(0); break;
        case SK_END:  select_index(s_n - 1); break;
        case SK_ENTER: enter_pressed(); break;
        case SK_BACKSPACE:
            if (field_visible(s) && s_draft[0]) {
                size_t n = strlen(s_draft);
                do n--; while (n && (s_draft[n] & 0xC0) == 0x80);
                s_draft[n] = 0;
                draft_changed();
            }
            break;
        case SK_ESC:
            if (s_msg_mode) { s_msg_mode = false; s_draft[0] = 0; rebuild(); }
            else if (s_draft[0]) { s_draft[0] = 0; draft_changed(); }
            break;
        default: break;
        }
        return true;
    }
    if (ev->mods & (MOD_CTRL | MOD_META)) {
        if (ev->cp == 'o' || ev->cp == 'O') open_selected();
        return true;
    }
    if (field_visible(s)) {
        char b[5] = {0};
        uint32_t c = ev->cp;
        if (c < 0x20) return true;
        if (c < 0x80) b[0] = (char)c;
        else if (c < 0x800) { b[0] = 0xC0 | (c >> 6); b[1] = 0x80 | (c & 0x3F); }
        else if (c < 0x10000) { b[0] = 0xE0 | (c >> 12); b[1] = 0x80 | ((c >> 6) & 0x3F); b[2] = 0x80 | (c & 0x3F); }
        else { b[0] = 0xF0 | (c >> 18); b[1] = 0x80 | ((c >> 12) & 0x3F); b[2] = 0x80 | ((c >> 6) & 0x3F); b[3] = 0x80 | (c & 0x3F); }
        draft_add(b);
    } else if ((ev->cp == 'r' || ev->cp == 'R') && actionable(s) && !note_says_open(s)) {
        s_msg_mode = true;
        s_draft[0] = 0;
        rebuild();
    }
    return true;
}

/* ------------------------------------------------------------------ refresh */

static void load_snapshot(void)
{
    if (s_poll_gen == s_seen_poll && s_n >= 0) return;
    s_seen_poll = s_poll_gen;
    int n = cc_poll_snapshot(s_snap, SNAP_CAP);
    if (n < 0) return;
    n = cc_sessions_visible(s_snap, n, 4);
    if (n > MAX_CARDS) n = MAX_CARDS;
    bool changed = n != s_n;
    for (int i = 0; !changed && i < n; i++) changed = !same_session(&s_snap[i], &s_items[i]);
    if (!changed) return;
    memcpy(s_items, s_snap, sizeof(cc_session_t) * n);
    s_n = n;
    /* keep the selection on the same session */
    int sel = -1;
    for (int i = 0; i < n; i++)
        if (strcmp(key_of(&s_items[i]), s_sel_key) == 0) { sel = i; break; }
    if (sel < 0) {
        sel = s_sel < n ? s_sel : n - 1;
        if (sel < 0) sel = 0;
        s_draft[0] = 0;
        s_msg_mode = false;
        if (n > 0) strlcpy(s_sel_key, key_of(&s_items[sel]), sizeof(s_sel_key));
    }
    s_sel = sel;
    /* a note belongs to the state it was made in */
    int64_t now = mono_ms();
    for (int k = 0; k < NOTES_MAX; k++) {
        note_t *nt = &s_notes[k];
        if (nt->kind == NOTE_NONE) continue;
        if (nt->kind == NOTE_BUSY) {
            if (!s_act_pending) nt->kind = NOTE_NONE;   /* its result found no card */
            continue;
        }
        bool keep = false;
        for (int i = 0; i < n; i++)
            if (strcmp(key_of(&s_items[i]), nt->key) == 0) { keep = s_items[i].state == nt->state; break; }
        if (nt->kind == NOTE_ERROR) keep = true;   /* an error outlives the state change it explains */
        if (!keep || now - nt->at_ms > NOTE_TTL_MS) nt->kind = NOTE_NONE;
    }
    rebuild();
}

/* Stale cards after a wake from Off or deeper (I was 15 s): dimmed with "Updating…" until a
   result newer than the wake arrives. The power owner records the generation at the wake, so a
   dashboard built later (it went home, or Settings was open) judges the same wake the same way.
   A wake from Dim never marks them: the link ran at I = 3 s throughout. */
static pr_cards_t cards_view(void)
{
    bool hib = hibernate_boot_kind() == POWER_BOOT_HIB_WAKE;
    bool dark_wake = power_wake_seq() > 0 && power_woke_from() >= PS_OFF;
    if (!hib && !dark_wake) return PR_CARDS_LIVE;
    return pr_cards_view(hib, cc_poll_generation(), dark_wake ? power_wake_gen() : 0,
                         power_wake_reason() == PW_NEEDS_YOU);
}

static void update_status(void)
{
    char name[48], line[224];
    cc_settings_server_name(name, sizeof(name));
    bool up = cc_link_connected();
    int64_t now = mono_ms();
    uint32_t dot = C_MUTED, color = C_FG;
    pr_cards_t view = cards_view();
    if (!name[0]) {
        strlcpy(line, "Set up your Mac in Settings \xe2\x86\x92 Companion.", sizeof(line));
        s_down_since_ms = 0;
    } else if (!up && cc_link_failure() != CC_LINK_FAIL_NONE) {
        /* Not a reachability problem (a changed host key, a missing server entry, a refused
           key): waking the Mac won't help, so say what cc_link knows. */
        char st[CC_STATUS_MAX];
        cc_link_status_text(st, sizeof(st));
        if (st[0] >= 'a' && st[0] <= 'z') st[0] = (char)(st[0] - 'a' + 'A');
        snprintf(line, sizeof(line), "%s.", st);
        color = dot = C_RED;
        s_down_since_ms = 0;
    } else if (!up) {
        /* The clock starts once the network could reach the Mac: Tailscale takes about a
           minute to register after boot, and that isn't the Mac's fault. */
        bool wifi = wifi_mgr_is_connected();
        bool net = wifi && (!g_settings.ts_enable || tailscale_state() == TS_CONNECTED);
        if (!net) s_down_since_ms = 0;
        else if (!s_down_since_ms) s_down_since_ms = now;
        if (!wifi && !wifi_mgr_is_connecting()) {
            snprintf(line, sizeof(line), "No Wi-Fi, so the Tab5 can't reach %s.", name);
            color = dot = C_RED;
        } else if (s_down_since_ms && now - s_down_since_ms >= UNREACHABLE_MS) {
            snprintf(line, sizeof(line), "Can't reach %s. Is your Mac awake?", name);
            color = dot = C_RED;
        } else if (view == PR_CARDS_WAKING) {
            strlcpy(line, "Waking: reconnecting to Mac", sizeof(line));
            color = C_MUTED;
        } else {
            snprintf(line, sizeof(line), "Connecting to %s\xe2\x80\xa6", name);
            color = C_MUTED;
        }
    } else {
        s_down_since_ms = 0;
        dot = C_GREEN;
        bool too_big = false;
        int fails = cc_poll_failures(&too_big);
        if (too_big) {
            /* the cards below are from the last poll that fit: not current */
            strlcpy(line, "Session list too large to load, so these cards aren't updating.", sizeof(line));
            color = dot = C_RED;
        } else if (fails >= CC_POLL_FAILS_SHOWN) {
            snprintf(line, sizeof(line), "Can't load sessions from %s, so these cards aren't updating.", name);
            color = dot = C_RED;
        } else if (view == PR_CARDS_WAKING) { strlcpy(line, "Waking: reconnecting to Mac", sizeof(line)); color = C_MUTED; }
        else if (view == PR_CARDS_STALE && s_n >= 0) { strlcpy(line, "Updating\xe2\x80\xa6", sizeof(line)); color = C_MUTED; }
        else if (s_n < 0) { strlcpy(line, "Loading sessions\xe2\x80\xa6", sizeof(line)); color = C_MUTED; }
        else if (s_n == 0) snprintf(line, sizeof(line), "No sessions on %s. Start one on your Mac with claude --bg.", name);
        else cc_summary(s_items, s_n, line, sizeof(line));
    }
    if (s_summary) {
        if (strcmp(lv_label_get_text(s_summary), line)) lv_label_set_text(s_summary, line);
        lv_obj_set_style_text_color(s_summary, HEX(color), 0);
    }
    if (s_dot) lv_obj_set_style_bg_color(s_dot, HEX(dot), 0);
    if (s_host && strcmp(lv_label_get_text(s_host), name[0] ? name : "No Mac"))
        lv_label_set_text(s_host, name[0] ? name : "No Mac");
    if (s_list && (int)view != s_view_shown) {   /* a style write redraws the list: only on a change */
        static const char *names[] = { "live", "stale", "waking" };
        ESP_LOGI(TAG, "cards: %s", names[view]);
        s_view_shown = (int)view;
        lv_obj_set_style_opa(s_list, view == PR_CARDS_STALE ? LV_OPA_50 : LV_OPA_COVER, 0);
        if (view == PR_CARDS_WAKING) lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    }
    bool live = up && name[0];
    if (live != s_shown_live) { s_shown_live = live; rebuild(); }
}

static void update_vitals(void)
{
    ui_vitals_update(s_vitals);
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    subscribe_when_ready();
    load_snapshot();
    apply_action_result();
    if (!s_scr) return;   /* opening a session replaced the screen */
    /* a request whose result matched no card leaves a BUSY note behind */
    for (int k = 0; !s_act_pending && k < NOTES_MAX; k++)
        if (s_notes[k].kind == NOTE_BUSY) { s_notes[k].kind = NOTE_NONE; rebuild(); }
    res_lock();
    unsigned lg = s_logs.gen;
    res_unlock();
    if (lg != s_seen_logs) { s_seen_logs = lg; set_logs_text(); }
    update_status();
    request_logs(false);
    if (s_ticks++ % 3 == 0) update_vitals();
    if (s_ticks % 30 == 0)
        for (int i = 0; i < s_n && i < MAX_CARDS; i++) if (s_ages[i]) set_age(s_ages[i], &s_items[i]);
}

bool ui_dashboard_selected_center(int32_t *x, int32_t *y)
{
    if (!s_scr || s_scr != lv_screen_active() || s_n <= 0 || s_sel < 0 || s_sel >= MAX_CARDS || !s_cards[s_sel])
        return false;
    lv_area_t a;
    lv_obj_get_coords(s_cards[s_sel], &a);
    *x = (a.x1 + a.x2) / 2;
    *y = (a.y1 + a.y2) / 2;
    return true;
}

/* ------------------------------------------------------------------ screen */

/* Home has nothing to go back to: a stray swipe must not rebuild it. */
static void dash_gesture_cb(lv_dir_t dir) { (void)dir; }

static void more_cb(lv_event_t *e) { (void)e; ui_nav_push(ui_home_create, NULL); }
static void settings_cb(lv_event_t *e) { (void)e; ui_nav_push(ui_settings_create, NULL); }

static lv_obj_t *footer_button(lv_obj_t *parent, const char *label, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 38);
    lv_obj_set_style_pad_hor(b, 18, 0);
    lv_obj_set_style_radius(b, 9, 0);
    lv_obj_set_style_bg_color(b, HEX(C_PANEL2), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, HEX(C_LINE), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, HEX(C_LINE), 0);
    lv_obj_set_style_outline_color(b, HEX(C_FG), LV_STATE_FOCUS_KEY);
    lv_obj_set_ext_click_area(b, 6);
    lv_obj_t *l = text(b, label, FONT_SMALL, C_FG);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

/* The dashboard can be rebuilt over itself (ui_nav_refresh, ui_nav_home from the
   dashboard): the new screen is created before the old one is deleted, so the old one's
   delete must leave the new one's widgets, key sink and poll rate alone. */
static void scr_delete_cb(lv_event_t *e)
{
    lv_obj_t *scr = lv_event_get_target_obj(e);
    keyboard_clear_sink(scr);   /* only if it is still this screen's sink */
    if (scr != s_scr) return;
    s_scr = s_list = s_summary = s_vitals = s_dot = s_host = NULL;
    s_logs_lbl = s_ta = NULL;
    memset(s_cards, 0, sizeof(s_cards));
    memset(s_ages, 0, sizeof(s_ages));
    /* PT11: the rate stays 3 s on every other screen (Active/Dim); Off sets its floor */
}

lv_obj_t *ui_dashboard_create(void *arg)
{
    (void)arg;
    init_once();
    int W = lv_display_get_horizontal_resolution(NULL), H = lv_display_get_vertical_resolution(NULL);
    lv_obj_t *scr = ui_screen_base();
    ui_motion_plain(scr);
    s_scr = scr;
    lv_obj_set_style_bg_color(scr, HEX(C_BG), 0);
    lv_obj_set_style_text_color(scr, HEX(C_FG), 0);
    lv_obj_add_event_cb(scr, scr_delete_cb, LV_EVENT_DELETE, NULL);
    ui_screen_set_gesture(scr, dash_gesture_cb);

    /* vitals */
    lv_obj_t *v = box(scr);
    lv_obj_set_size(v, W, VITALS_H);
    lv_obj_set_style_bg_color(v, HEX(C_PANEL), 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(v, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(v, 1, 0);
    lv_obj_set_style_border_color(v, HEX(C_LINE), 0);
    lv_obj_set_style_pad_hor(v, 18, 0);
    lv_obj_set_flex_flow(v, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(v, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(v, 18, 0);
    lv_obj_t *brand = text(v, "Claude Companion", FONT_BODY, C_FG);
    lv_obj_set_flex_grow(brand, 1);
    s_vitals = ui_vitals_create(v);
    s_dot = ui_vitals_link_dot(s_vitals);
    s_host = ui_vitals_link_host(s_vitals);

    /* summary or status line */
    s_summary = text(scr, "", FONT_BODY, C_FG);
    lv_obj_set_pos(s_summary, 20, VITALS_H + 10);
    lv_obj_set_width(s_summary, W - 40);
    lv_label_set_long_mode(s_summary, LV_LABEL_LONG_DOT);

    /* cards */
    s_list = box(scr);
    lv_obj_set_pos(s_list, 0, VITALS_H + SUMMARY_H);
    lv_obj_set_size(s_list, W, H - VITALS_H - SUMMARY_H - FOOTER_H);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_hor(s_list, 18, 0);
    lv_obj_set_style_pad_ver(s_list, 6, 0);
    lv_obj_set_style_pad_row(s_list, 10, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);

    /* footer: keys, and the way to the fork's tools (Ctrl+Tab: Tab walks the cards and the
       Settings / More buttons, Enter presses one, Esc goes back to the session keys) */
    lv_obj_t *f = box(scr);
    lv_obj_set_size(f, W, FOOTER_H);
    lv_obj_set_pos(f, 0, H - FOOTER_H);
    lv_obj_set_style_bg_color(f, HEX(C_PANEL), 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(f, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(f, 1, 0);
    lv_obj_set_style_border_color(f, HEX(C_LINE), 0);
    lv_obj_set_style_pad_hor(f, 18, 0);
    lv_obj_set_flex_flow(f, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(f, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(f, 12, 0);
    lv_obj_t *keys = text(f, "Up/Down select \xc2\xb7 Enter send / open \xc2\xb7 Ctrl+O open \xc2\xb7 Ctrl+Q leave \xc2\xb7 Ctrl+Tab buttons",
                          FONT_SMALL, C_MUTED);
    lv_obj_set_flex_grow(keys, 1);
    lv_label_set_long_mode(keys, LV_LABEL_LONG_DOT);
    lv_obj_set_height(keys, lv_font_get_line_height(FONT_SMALL));
    footer_button(f, "Settings", settings_cb);
    footer_button(f, "More", more_cb);

    key_sink_t sink = { .key = dash_key, .user = scr };   /* per screen */
    keyboard_set_sink(&sink);
    cc_poll_set_rate(3);

    s_visit++;   /* answers to requests from an earlier visit are not acted on */
    s_shown_live = false;
    s_view_shown = -1;
    s_seen_poll = s_poll_gen - 1;   /* load the snapshot now */
    lv_timer_t *t = lv_timer_create(tick_cb, 300, NULL);
    ui_timer_bind(t, scr);
    update_vitals();
    update_status();
    rebuild();
    tick_cb(t);
    return scr;
}
