/* Sessions (spec §5.1, B3, B5): the home screen, master-detail.
 *
 * Data comes from cc_link's poll (a subscriber only bumps a counter; the 300 ms tick copies the
 * snapshot). Everything that talks to the Host -- log tails, answers, the live check before opening,
 * the forget-host-key job -- runs on a small worker task, never on the LVGL task, and every command
 * line comes from cc_cmd_* (cc_link builds the paste and the check itself). Keys reach Sessions
 * through a keyboard sink while it is the active screen.
 *
 * The screen is built once per visit: the header, the banner, a pool of 16 session rows, the detail
 * pane and the footer. A poll only calls setters, and only on a change, so a static screen never
 * redraws and the rows' RAM does not grow with the session count (§11). The selection is a row key
 * (sm_key) found again in every snapshot; drafts are keyed the same way (sessions_model, Review
 * Focus 1). Amber is drawn only by the NEEDS YOU components in ui_comp. */
#include "ui.h"
#include "ui_sessions.h"
#include "ui_comp.h"
#include "ui_softkbd.h"
#include "ui/sessions_model.h"
#include "ui/hotkeys.h"
#include "companion/cc_link.h"
#include "companion/cc_sessions.h"
#include "companion/cc_cmd.h"
#include "net/wifi_mgr.h"
#include "net/tailscale.h"
#include "net/ts_host.h"
#include "settings/settings.h"
#include "sys/power.h"
#include "sys/checkin.h"
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

static const char *TAG = "sessions";

#define HEX(h) lv_color_hex(h)

#define SNAP_CAP         128
#define MAX_ROWS         16
#define TAIL_LINES       40      /* cc_log_tail keeps this many lines; the wrap keeps what fits */
#define LOGS_CAP         (16 * 1024)
#define LOG_TEXT_CAP     4096
#define WRAP_CAP         4096
#define STATUS_CAP       512
#define LOGS_EVERY_MS    5000
#define NOTE_TTL_MS      90000
#define SENT_MS          3000    /* "Sent ✓" shows this long */
#define NOTES_MAX        8
#define CHECK_MAX_AGE_MS 20000   /* an open check answered later than this is not acted on */
#define TICK_MS          300
#define AGE_TICKS        30      /* the rows' ages are refreshed every 9 s */
#define LIST_PAD_L       UI_TK_MARGIN
#define LIST_PAD_R       UI_TK_GAP_S
#define BODY_PAD_V       UI_TK_GAP_M
#define CELL_W           12      /* FONT_MONO's cell */

/* ------------------------------------------------------------------ state */

typedef enum { NOTE_NONE, NOTE_BUSY, NOTE_SENT, NOTE_APPROVE, NOTE_OPEN_TO_ANSWER, NOTE_ERROR } note_kind_t;
typedef struct {
    char key[SM_KEY_MAX];
    note_kind_t kind;
    char text[160];
    cc_state_t state;      /* the session's state when the note was made */
    int64_t at_ms;
} note_t;

typedef enum { JOB_LOGS, JOB_PASTE, JOB_CHECK, JOB_FORGET_KEY } job_type_t;
typedef struct {
    job_type_t type;
    unsigned token;        /* which request this is (actions only) */
    char id[16];
    char text[SM_ANSWER_MAX + 1];
} job_t;

/* Worker results, read by the tick (guarded by res_lock). s_logs_text is PSRAM. */
static struct { unsigned gen; unsigned token; job_type_t type; char id[16]; esp_err_t err; char msg[160]; } s_act;
static struct { unsigned gen; char id[16]; bool ok; } s_logs;
static char *s_logs_text;
static volatile bool s_logs_pending, s_act_pending;
static volatile unsigned s_poll_gen;

/* Model (LVGL task only; the big buffers in PSRAM). */
static cc_session_t *s_snap;               /* the whole poll */
static cc_session_t *s_items;              /* the rows, in order */
static int s_n = -1;                       /* -1: no list yet */
static int s_sel;                          /* selected index */
static char s_sel_key[SM_KEY_MAX];         /* ... and the session it is */
static sm_drafts_t *s_drafts;
static note_t *s_notes;                    /* NOTES_MAX */
static char *s_tail, *s_wrapped;           /* the selected session's tail, and it wrapped to the pane */
static char *s_status;                     /* a status line ("Loading recent output…") wrapped to the pane */
static int s_options;                      /* sm_tail_options of s_tail */
static unsigned s_seen_poll, s_seen_act, s_seen_logs;
static int64_t s_logs_at_ms;
static char s_logs_for[16];
static int64_t s_down_since_ms;            /* link down while the network could reach the Host */
static sm_lowbat_t s_lowbat;
static bool s_lowbat_dismiss;
static uint32_t s_forget_seen;             /* cc_link_forget_done count already toasted (P4-3) */
/* The request in flight: its answer is used only by the visit that asked, and an open check only
   while it is fresh (it can take up to the poll timeout). */
static unsigned s_req_token, s_visit, s_req_visit;
static int64_t s_req_at_ms;

/* Widgets (valid while the screen exists). */
typedef enum { BODY_NONE, BODY_LIST, BODY_LOADING, BODY_NO_HOST, BODY_EMPTY } body_t;
static lv_obj_t *s_scr, *s_needs, *s_banner, *s_list, *s_rows[MAX_ROWS], *s_skel, *s_nohost, *s_empty;
static lv_obj_t *s_pane, *s_title, *s_pill, *s_meta, *s_logbox, *s_log, *s_info;
static lv_obj_t *s_answer_row, *s_ta, *s_action_row, *s_send, *s_note;
static int s_body_top = -1, s_body = -1, s_banner_shown = -1, s_inert = -1;
static int s_cols, s_rows_fit;             /* the log block, measured */
static bool s_dirty_rows, s_dirty_pane, s_dirty_log, s_scroll_sel;
static unsigned s_ticks;
static bool s_kbd_attached;                /* the soft keyboard is attached to this screen's field */

/* LVGL's tick stops in Off (lvgl_port_stop), so TTLs and the open-check age use esp_timer on the
   device. The preview has no esp_timer and drives lv_tick itself. */
#ifdef UI_PREVIEW
static int64_t mono_ms(void) { return (int64_t)lv_tick_get(); }
#else
static int64_t mono_ms(void) { return esp_timer_get_time() / 1000; }
#endif

/* ------------------------------------------------------------------ styles */

/* The screen's own shared styles (built once): text colours and fonts, the log block, the answer
   field's focus border and steady cursor, the mic slot's dashes and the shortcuts sheet. */
static bool s_styles_ready;
static lv_style_t st_fg, st_muted, st_green, st_red, st_body, st_label, st_mono;
static lv_style_t st_list, st_logbox, st_field, st_field_focus, st_cursor, st_mic, st_dash, st_mic_line, st_capsule;

static void styles_init(void)
{
    if (s_styles_ready) return;
    s_styles_ready = true;
    lv_style_t *all[] = { &st_list, &st_fg, &st_muted, &st_green, &st_red, &st_body, &st_label, &st_mono, &st_logbox,
                          &st_field, &st_field_focus, &st_cursor, &st_mic, &st_dash, &st_mic_line, &st_capsule };
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) lv_style_init(all[i]);
    lv_style_set_text_color(&st_fg, HEX(UI_TK_FG));
    lv_style_set_text_color(&st_muted, HEX(UI_TK_MUTED));
    lv_style_set_text_color(&st_green, HEX(UI_TK_GREEN));
    lv_style_set_text_color(&st_red, HEX(UI_TK_RED));
    lv_style_set_text_font(&st_body, FONT_BODY);
    lv_style_set_text_font(&st_label, FONT_SMALL);
    lv_style_set_text_font(&st_mono, FONT_MONO);
    /* the list column: a hairline on its right (Main mockup) */
    lv_style_set_border_color(&st_list, HEX(UI_TK_LINE));
    lv_style_set_border_width(&st_list, 1);
    lv_style_set_border_side(&st_list, LV_BORDER_SIDE_RIGHT);
    /* log block: panel, a line border, radius 12 */
    lv_style_set_bg_color(&st_logbox, HEX(UI_TK_PANEL));
    lv_style_set_bg_opa(&st_logbox, LV_OPA_COVER);
    lv_style_set_border_color(&st_logbox, HEX(UI_TK_LINE));
    lv_style_set_border_width(&st_logbox, 1);
    lv_style_set_radius(&st_logbox, UI_TK_RADIUS);
    lv_style_set_pad_hor(&st_logbox, 20);
    lv_style_set_pad_ver(&st_logbox, UI_TK_GAP_M);
    /* answer field: 64 px, panel-2, radius 12 (the theme layer); a 2 px fg border while focused */
    lv_style_set_pad_hor(&st_field, 18);
    lv_style_set_text_font(&st_field, FONT_BODY);
    lv_style_set_border_color(&st_field_focus, HEX(UI_TK_FG));
    lv_style_set_border_width(&st_field_focus, 2);
    lv_style_set_outline_width(&st_field_focus, 0);
    /* a steady cursor: no blink, so a static screen stays idle (§11, B1) */
    lv_style_set_anim_duration(&st_cursor, 0);
    lv_style_set_border_color(&st_cursor, HEX(UI_TK_FG));
    lv_style_set_border_width(&st_cursor, 2);
    lv_style_set_border_side(&st_cursor, LV_BORDER_SIDE_LEFT);
    /* mic slot: panel-2, no solid border (the dashes are drawn) */
    lv_style_set_bg_color(&st_mic, HEX(UI_TK_PANEL2));
    lv_style_set_bg_opa(&st_mic, LV_OPA_COVER);
    lv_style_set_radius(&st_mic, UI_TK_RADIUS);
    lv_style_set_border_width(&st_mic, 0);
    lv_style_set_line_color(&st_dash, HEX(UI_TK_LINE));
    lv_style_set_line_width(&st_dash, 1);
    lv_style_set_line_dash_width(&st_dash, 6);
    lv_style_set_line_dash_gap(&st_dash, 4);
    lv_style_set_line_color(&st_mic_line, HEX(UI_TK_MUTED));
    lv_style_set_line_width(&st_mic_line, 2);
    lv_style_set_line_rounded(&st_mic_line, true);
    lv_style_set_border_color(&st_capsule, HEX(UI_TK_MUTED));
    lv_style_set_border_width(&st_capsule, 2);
    lv_style_set_radius(&st_capsule, LV_RADIUS_CIRCLE);
}

typedef enum { TC_FG, TC_MUTED, TC_GREEN, TC_RED } tc_t;
static lv_style_t *tc_style(tc_t c) { return c == TC_MUTED ? &st_muted : c == TC_GREEN ? &st_green : c == TC_RED ? &st_red : &st_fg; }
static uint32_t tc_hex(tc_t c) { return c == TC_MUTED ? UI_TK_MUTED : c == TC_GREEN ? UI_TK_GREEN : c == TC_RED ? UI_TK_RED : UI_TK_FG; }

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const char *t, lv_style_t *font, tc_t c)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t ? t : "");
    lv_obj_add_style(l, font, 0);
    lv_obj_add_style(l, tc_style(c), 0);
    return l;
}

/* Setters that write only on a change: a write redraws, and a static screen must not (§11). */
static void set_text(lv_obj_t *l, const char *t)
{
    if (l && strcmp(lv_label_get_text(l), t ? t : "")) lv_label_set_text(l, t ? t : "");
}

static void set_colour(lv_obj_t *l, tc_t c)
{
    if (!l || lv_color_eq(lv_obj_get_style_text_color(l, 0), HEX(tc_hex(c)))) return;
    lv_obj_remove_style(l, &st_fg, 0);
    lv_obj_remove_style(l, &st_muted, 0);
    lv_obj_remove_style(l, &st_green, 0);
    lv_obj_remove_style(l, &st_red, 0);
    lv_obj_add_style(l, tc_style(c), 0);
}

static bool set_shown(lv_obj_t *o, bool on)
{
    if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != on) return false;
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return true;
}

/* ------------------------------------------------------------------ worker */

static char *s_logs_buf;   /* worker only */

static void res_lock(void);
static void res_unlock(void);

/* Forget host key (P4-3). The worker's result is shown on the LVGL task. A forget done in time toasts
   from forget_done_cb on whatever screen is up; one that timed out and is finished by a later connect
   is toasted by the Sessions tick. Both go through s_forget_seen, so each forget toasts once. */
static char s_forget_toast[128];

/* The worker's outcome, passed as ui_async's arg; the text is composed here, on the LVGL task, so only
   that task writes s_forget_toast (review M3). */
enum { FORGET_TIMED_OUT = 1, FORGET_NO_HOST };

static void forget_toast_cb(void *arg)
{
    strlcpy(s_forget_toast, (intptr_t)arg == FORGET_TIMED_OUT ? "Old key forgotten; reconnecting\xe2\x80\xa6" : "No host set up",
            sizeof(s_forget_toast));
    ui_toast_ms(s_forget_toast, FORGET_TOAST_MS);
}

static void forget_check(void)
{
    char fp[80];
    uint32_t n = cc_link_forget_done(fp, sizeof(fp));
    if (n == s_forget_seen) return;
    s_forget_seen = n;
    snprintf(s_forget_toast, sizeof(s_forget_toast), "New host key pinned: %s", fp);
    ui_toast_ms(s_forget_toast, FORGET_TOAST_MS);
}

static void forget_done_cb(void *arg) { (void)arg; forget_check(); }

static void run_forget(void)
{
    char fp[80];
    esp_err_t e = cc_link_forget_host_key(fp, sizeof(fp), CC_FORGET_TIMEOUT_MS);
    ESP_LOGI(TAG, "forget host key: %s %s", esp_err_to_name(e), fp);
    if (e == ESP_OK) { ui_async(forget_done_cb, NULL); return; }
    ui_async(forget_toast_cb, (void *)(intptr_t)(e == ESP_ERR_TIMEOUT ? FORGET_TIMED_OUT : FORGET_NO_HOST));
}

static void run_job(const job_t *j)
{
    char claude[64], cmd[192], msg[160] = "";
    if (j->type == JOB_FORGET_KEY) { run_forget(); return; }
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
                cc_log_tail(s_logs_buf, TAIL_LINES);
            }
        }
        /* keep the end of a tail longer than the result buffer, from a line start */
        const char *src = ok ? s_logs_buf : msg;
        size_t len = strlen(src);
        if (len >= LOG_TEXT_CAP) {
            src += len - (LOG_TEXT_CAP - 1);
            const char *nl = strchr(src, '\n');
            if (nl) src = nl + 1;
            while ((*src & 0xC0) == 0x80) src++;
        }
        res_lock();
        s_logs.gen++;
        strlcpy(s_logs.id, j->id, sizeof(s_logs.id));
        s_logs.ok = ok;
        if (s_logs_text) strlcpy(s_logs_text, src, LOG_TEXT_CAP);
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
        xTaskCreatePinnedToCore(worker_task, "sess_work", 6144, NULL, 3, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "worker start failed");
        /* no worker: refuse jobs instead of queueing them forever */
        if (s_jobs) { vQueueDelete(s_jobs); s_jobs = NULL; }
    }
}
#endif

static job_t *job_new(job_type_t type)
{
    job_t *j = heap_caps_calloc(1, sizeof(job_t), MALLOC_CAP_SPIRAM);   /* job_t holds an answer */
    if (j) j->type = type;
    return j;
}

static void poll_cb(const cc_session_t *s, int n, void *ctx)
{
    (void)s; (void)n; (void)ctx;
    s_poll_gen++;   /* link task: only note it; the tick copies the snapshot */
}

static void init_once(void)
{
    static bool done;
    if (done) return;
    done = true;
    s_snap = heap_caps_calloc(SNAP_CAP, sizeof(cc_session_t), MALLOC_CAP_SPIRAM);
    s_items = heap_caps_calloc(MAX_ROWS, sizeof(cc_session_t), MALLOC_CAP_SPIRAM);
    s_drafts = heap_caps_calloc(1, sizeof(sm_drafts_t), MALLOC_CAP_SPIRAM);
    s_notes = heap_caps_calloc(NOTES_MAX, sizeof(note_t), MALLOC_CAP_SPIRAM);
    s_logs_buf = heap_caps_calloc(1, LOGS_CAP, MALLOC_CAP_SPIRAM);
    s_logs_text = heap_caps_calloc(1, LOG_TEXT_CAP, MALLOC_CAP_SPIRAM);
    s_tail = heap_caps_calloc(1, LOG_TEXT_CAP, MALLOC_CAP_SPIRAM);
    s_wrapped = heap_caps_calloc(1, WRAP_CAP, MALLOC_CAP_SPIRAM);
    s_status = heap_caps_calloc(1, STATUS_CAP, MALLOC_CAP_SPIRAM);
    char fp[80];
    s_forget_seen = cc_link_forget_done(fp, sizeof(fp));   /* only forgets from now on are toasted */
    worker_start();
}

/* Sessions can be built before cc_link_start (it is the first screen), and cc_poll_subscribe does
   nothing until then, so subscribe once the link is up. */
static void subscribe_when_ready(void)
{
    static bool subscribed;
    if (subscribed || !cc_link_connected()) return;
    cc_poll_subscribe(poll_cb, NULL);
    subscribed = true;
    s_poll_gen++;
}

bool ui_sessions_forget_host_key(void)
{
    init_once();
    job_t *j = job_new(JOB_FORGET_KEY);
    if (!j) return false;
    bool ok = job_post(j);
    heap_caps_free(j);
    return ok;
}

/* ------------------------------------------------------------------ helpers */

static const cc_session_t *selected(void) { return (s_n > 0 && s_sel >= 0 && s_sel < s_n) ? &s_items[s_sel] : NULL; }
static bool actionable(const cc_session_t *s)
{
    return s && s->background && s->id[0] && s->live && s->state != CC_ENDED;
}

static note_t *note_find(const char *key)
{
    if (!s_notes) return NULL;
    for (int i = 0; i < NOTES_MAX; i++)
        if (s_notes[i].kind != NOTE_NONE && strcmp(s_notes[i].key, key) == 0) return &s_notes[i];
    return NULL;
}

static note_t *note_of(const cc_session_t *s)
{
    char k[SM_KEY_MAX];
    if (!s) return NULL;
    sm_key(s, k, sizeof(k));
    return note_find(k);
}

static void note_set(const cc_session_t *s, note_kind_t kind, const char *t)
{
    if (!s_notes) return;
    char k[SM_KEY_MAX];
    sm_key(s, k, sizeof(k));
    note_t *n = note_find(k);
    if (!n) {
        n = &s_notes[0];
        for (int i = 0; i < NOTES_MAX; i++) {
            if (s_notes[i].kind == NOTE_NONE) { n = &s_notes[i]; break; }
            if (s_notes[i].at_ms < n->at_ms) n = &s_notes[i];
        }
    }
    strlcpy(n->key, k, sizeof(n->key));
    n->kind = kind;
    strlcpy(n->text, t ? t : "", sizeof(n->text));
    n->state = s->state;
    n->at_ms = mono_ms();
    s_dirty_pane = true;
}

static void note_clear(const cc_session_t *s)
{
    note_t *n = note_of(s);
    if (n) { n->kind = NOTE_NONE; s_dirty_pane = true; }
}

/* A refusal note replaces the answer field: Enter then opens the session. */
static bool note_says_open(const cc_session_t *s)
{
    note_t *n = note_of(s);
    return n && (n->kind == NOTE_APPROVE || n->kind == NOTE_OPEN_TO_ANSWER);
}

static bool field_visible(const cc_session_t *s) { return actionable(s) && !note_says_open(s); }

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

/* "repo · age" (the age once the clock is set) */
static void meta_of(const cc_session_t *s, char *out, size_t cap)
{
    char a[16] = "";
    int64_t now = wall_ms();
    if (now && s->started_ms > 0) cc_age(now, s->started_ms, a, sizeof(a));
    if (a[0]) snprintf(out, cap, "%s \xc2\xb7 %s", s->repo, a);
    else snprintf(out, cap, "%s", s->repo);
}

static const char *draft(void) { return s_drafts ? sm_draft_get(s_drafts, s_sel_key) : ""; }

static void field_sync(void)
{
    if (!s_ta) return;
    const char *d = draft();
    if (strcmp(lv_textarea_get_text(s_ta), d)) {
        lv_textarea_set_text(s_ta, d);
        lv_textarea_set_cursor_pos(s_ta, LV_TEXTAREA_CURSOR_LAST);
    }
}

static void draft_put(const char *t)
{
    if (s_drafts && s_sel_key[0]) sm_draft_set(s_drafts, s_sel_key, t);
    field_sync();
}

/* ------------------------------------------------------------------ the log block */

/* The block's capacity in mono cells (cols) and rows (§5.1: about 62 × 10). */
static bool measure_log(void)
{
    if (!s_logbox || lv_obj_has_flag(s_logbox, LV_OBJ_FLAG_HIDDEN)) return false;
    lv_obj_update_layout(s_pane);
    int cols = (int)lv_obj_get_content_width(s_logbox) / CELL_W;
    int rows = (int)lv_obj_get_content_height(s_logbox) / lv_font_get_line_height(FONT_MONO);
    if (cols < 3) cols = 3;
    if (rows < 1) rows = 1;
    if (cols == s_cols && rows == s_rows_fit) return false;
    if (s_cols != cols || s_rows_fit != rows) ESP_LOGI(TAG, "log block: %d cols x %d rows", cols, rows);
    s_cols = cols;
    s_rows_fit = rows;
    return true;
}

/* A character's width in 12 px cells as the log label draws it: FONT_MONO is MaruMinya, which draws
   CJK and several TUI symbols (… ← █ ░ ─ ╭ ● ✽) 24 px wide and ✓ 16 px, unlike the terminal's grid. */
static int label_cells(uint32_t cp, void *ctx)
{
    (void)ctx;
    /* 0, unlike term_cells' 1: a label draws nothing for a control character (CR is already stripped) */
    if (cp < 0x20) return 0;
    return ((int)lv_font_get_glyph_width(FONT_MONO, cp, 0) + CELL_W - 1) / CELL_W;
}

static uint32_t text_hash(const char *s, uint32_t h)
{
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static void log_update(void)
{
    const cc_session_t *s = selected();
    if (!s_log || !s || !s->background || !s_tail) return;
    res_lock();
    bool mine = strcmp(s_logs.id, s->id) == 0;
    bool ok = s_logs.ok;
    if (mine) strlcpy(s_tail, s_logs_text ? s_logs_text : "", LOG_TEXT_CAP);
    res_unlock();
    const char *t;
    tc_t c = TC_FG;
    if (!mine) { s_tail[0] = 0; t = "Loading recent output\xe2\x80\xa6"; c = TC_MUTED; }
    else if (!ok) { t = s_tail[0] ? s_tail : "No output to show"; c = TC_MUTED; }
    else if (!s_tail[0]) { t = "No output yet"; c = TC_MUTED; }
    else {
        /* the wrap is the costly part: only for a new tail or a new block size */
        static uint32_t wrapped_sig;
        uint32_t sig = text_hash(s_tail, 2166136261u) ^ (uint32_t)(s_cols * 131 + s_rows_fit) ^ (uint32_t)(uintptr_t)s_log;
        if (sig != wrapped_sig || !s_wrapped[0]) {
            sm_wrap_tail_fn(s_tail, s_cols ? s_cols : 62, s_rows_fit ? s_rows_fit : 10, s_wrapped, WRAP_CAP, label_cells, NULL);
            wrapped_sig = sig;
        }
        t = s_wrapped;
    }
    int opts = mine && ok ? sm_tail_options(s_tail) : 0;
    if (opts != s_options) { s_options = opts; s_dirty_pane = true; }
    if (!mine || !ok || !s_tail[0]) {   /* a status line: cut to the block */
        if (!s_status) return;   /* not s_wrapped: that keeps the last tail's wrap */
        sm_wrap_tail_fn(t, s_cols ? s_cols : 62, s_rows_fit ? s_rows_fit : 10, s_status, STATUS_CAP, label_cells, NULL);
        t = s_status;
    }
    set_colour(s_log, c);
    set_text(s_log, t);
}

/* ------------------------------------------------------------------ rendering */

static void rows_update(void)
{
    char meta[96];
    for (int i = 0; i < MAX_ROWS; i++) {
        if (!s_rows[i]) continue;
        bool on = i < s_n;
        set_shown(s_rows[i], on);   /* first: the name is cut to the row's laid-out width */
        if (on) {
            meta_of(&s_items[i], meta, sizeof(meta));
            ui_session_row_set(s_rows[i], &s_items[i], i == s_sel, meta);
        }
    }
    int needs = 0;   /* the count goes with the rows on screen (none with no Host set) */
    for (int i = 0; s_body == BODY_LIST && i < s_n; i++) needs += cc_session_needs_you(&s_items[i]);
    ui_header_needs_you_set(s_needs, needs);
}

static void pane_update(void)
{
    const cc_session_t *s = selected();
    if (!s_pane) return;
    set_shown(s_pane, s != NULL);
    if (!s) return;
    char meta[96];
    set_text(s_title, s->name);
    meta_of(s, meta, sizeof(meta));
    set_text(s_meta, meta);
    ui_state_pill_set(s_pill, s);
    bool interactive = !s->background;
    bool act = actionable(s);
    bool field = field_visible(s);
    note_t *n = note_of(s);
    bool changed = set_shown(s_logbox, !interactive);
    changed |= set_shown(s_answer_row, field);
    changed |= set_shown(s_action_row, act);
    set_shown(s_send, field);
    /* the info line: interactive sessions (B3), and background ones that can't be acted on */
    const char *info = NULL;
    tc_t ic = TC_MUTED;
    if (interactive) info = "Interactive \xe2\x80\x94 answer it on the Host";
    else if (!act) {
        if (n && n->kind == NOTE_ERROR) { info = n->text; ic = TC_RED; }
        else info = "This session has stopped.";
    }
    changed |= set_shown(s_info, info != NULL);
    if (info) { set_text(s_info, info); set_colour(s_info, ic); }
    if (field) {
        const char *ph = s->state == CC_NEEDS_YOU ? "Type an answer\xe2\x80\xa6" : "Type a message\xe2\x80\xa6";
        if (strcmp(lv_textarea_get_placeholder_text(s_ta), ph)) lv_textarea_set_placeholder_text(s_ta, ph);
        field_sync();
    }
    /* the action row's note */
    char b[48];
    const char *nt = "";
    tc_t nc = TC_MUTED;
    if (n && n->kind != NOTE_NONE && act) {
        switch (n->kind) {
        case NOTE_BUSY:           nt = n->text; break;
        case NOTE_SENT:           nt = "Sent \xe2\x9c\x93"; nc = TC_GREEN; break;
        case NOTE_APPROVE:        nt = "Open it to approve"; break;
        case NOTE_OPEN_TO_ANSWER: nt = "Open the session to answer"; break;
        case NOTE_ERROR:          nt = n->text; nc = TC_RED; break;
        default: break;
        }
    } else if (act && s_options >= 2) {
        snprintf(b, sizeof(b), "1\xe2\x80\x93%d picks an option", s_options);
        nt = b;
    }
    set_text(s_note, nt);
    set_colour(s_note, nc);
    if (measure_log() || changed) s_dirty_log = true;
}

static void select_index(int i);
static void sessions_open_selected(void);
static void render(void);

/* A tap belongs to the session under the finger when it went down (review I1): a poll can re-sort the
   rows between the press and the release, and the row object then shows another session. */
static char s_press_key[SM_KEY_MAX];

static void row_pressed_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    s_press_key[0] = 0;
    if (i >= 0 && i < s_n) sm_key(&s_items[i], s_press_key, sizeof(s_press_key));
}

static void row_click_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    char k[SM_KEY_MAX];
    if (s_inert == 1 || i < 0 || i >= s_n) return;
    sm_key(&s_items[i], k, sizeof(k));
    /* no press recorded: a keyboard activation (Ctrl+Tab, Enter), which acts on the row as it is now */
    bool moved = s_press_key[0] && strcmp(k, s_press_key) != 0;
    s_press_key[0] = 0;
    if (moved) return;   /* the row shows another session now: the tap does nothing */
    if (i == s_sel) {
        const cc_session_t *s = selected();
        if (actionable(s)) { sessions_open_selected(); render(); }
        return;
    }
    select_index(i);
    render();
}

/* ------------------------------------------------------------------ actions */

static void request_logs(bool now)
{
    const cc_session_t *s = selected();
    if (!s || !s->background || !s->id[0] || s_logs_pending || !cc_link_connected()) return;
    if (!now && strcmp(s_logs_for, s->id) == 0 && mono_ms() - s_logs_at_ms < LOGS_EVERY_MS) return;
    job_t *j = job_new(JOB_LOGS);
    if (!j) return;
    strlcpy(j->id, s->id, sizeof(j->id));
    strlcpy(s_logs_for, s->id, sizeof(s_logs_for));
    s_logs_at_ms = mono_ms();
    s_logs_pending = true;
    if (!job_post(j)) s_logs_pending = false;
    heap_caps_free(j);
}

static void select_index(int i)
{
    if (s_n <= 0) return;
    if (i < 0) i = 0;
    if (i >= s_n) i = s_n - 1;
    char k[SM_KEY_MAX];
    sm_key(&s_items[i], k, sizeof(k));
    if (i == s_sel && strcmp(s_sel_key, k) == 0) return;
    s_sel = i;
    strlcpy(s_sel_key, k, sizeof(s_sel_key));
    s_options = 0;
    s_dirty_rows = s_dirty_pane = s_dirty_log = true;
    if (s_rows[i]) lv_obj_scroll_to_view(s_rows[i], LV_ANIM_OFF);
    request_logs(true);
}

static void post_action(job_type_t type, const cc_session_t *s, const char *t)
{
    if (s_act_pending) { ui_toast("Still working on the last request"); return; }
    job_t *j = job_new(type);
    if (!j) return;
    j->token = ++s_req_token;
    s_req_visit = s_visit;
    s_req_at_ms = mono_ms();
    strlcpy(j->id, s->id, sizeof(j->id));
    if (t) strlcpy(j->text, t, sizeof(j->text));
    s_act_pending = true;
    note_set(s, NOTE_BUSY, type == JOB_PASTE ? "Sending\xe2\x80\xa6" : "Opening\xe2\x80\xa6");
    if (!job_post(j)) {
        s_act_pending = false;
        note_set(s, NOTE_ERROR, "Busy, try again");
    }
    heap_caps_free(j);
}

static void sessions_open_selected(void)
{
    const cc_session_t *s = selected();
    if (!actionable(s) || s_inert == 1) return;
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
    /* the session as of the latest poll must still be one we may open */
    if (!actionable(s)) { note_set(s, NOTE_ERROR, "Session has stopped"); return; }
    int idx = cc_settings_server();
    if (idx < 0 || !cc_cmd_attach(cc_settings_claude_path(claude, sizeof(claude)), s->id, cmd, sizeof(cmd))) {
        note_set(s, NOTE_ERROR, "Can't open: set up the host in Settings");
        return;
    }
    note_clear(s);
    ESP_LOGI(TAG, "opening %s", s->id);
    power_hold(POWER_HOLD_TAKEOVER, true);   /* a take-over never dims or sleeps */
    cc_poll_set_rate(15);   /* the watch keeps feeding the keyboard light; nothing redraws */
    ui_terminal_open_command(idx, cmd, session_closed_cb, NULL);
}

static void enter_pressed(void)
{
    const cc_session_t *s = selected();
    if (!actionable(s) || s_act_pending || s_inert == 1) return;
    if (field_visible(s) && draft()[0]) { post_action(JOB_PASTE, s, draft()); return; }
    sessions_open_selected();   /* an empty field (or an open-to-answer note) opens it */
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
        if (s_items[i].background && strcmp(s_items[i].id, id) == 0) { s = &s_items[i]; break; }
    if (!s) return;
    if (type == JOB_CHECK) {
        /* Open only for the visit that asked, and only while the check is fresh: the user may have
           left and come back minutes later, and the session may have stopped. */
        if (s_req_visit != s_visit || mono_ms() - s_req_at_ms > CHECK_MAX_AGE_MS) {
            ESP_LOGI(TAG, "dropping a stale open check for %s", id);
            note_clear(s);
            return;
        }
        if (err == ESP_OK) { open_now(s); return; }
    } else if (err == ESP_OK) {
        note_set(s, NOTE_SENT, NULL);
        char k[SM_KEY_MAX];
        sm_key(s, k, sizeof(k));
        if (s_drafts) sm_draft_set(s_drafts, k, "");   /* the draft of the session it was typed for */
        field_sync();
        s_logs_at_ms = 0;
        return;
    } else if (err == CC_PASTE_NEEDS_APPROVAL) {
        note_set(s, NOTE_APPROVE, NULL); return;
    } else if (err == CC_PASTE_NOT_READY) {
        note_set(s, NOTE_OPEN_TO_ANSWER, NULL); return;
    }
    if (err == ESP_ERR_INVALID_STATE) strlcpy(msg, "Not connected to the host", sizeof(msg));
    else if (!msg[0]) strlcpy(msg, esp_err_to_name(err), sizeof(msg));
    if (msg[0] >= 'a' && msg[0] <= 'z') msg[0] = (char)(msg[0] - 'a' + 'A');
    note_set(s, NOTE_ERROR, msg);
    if (s != selected()) ui_toast(msg);   /* the pane shows another session: say it here */
}

/* ------------------------------------------------------------------ Ctrl+K overlay */

/* The shortcuts sheet (P4-11: a two-column table that never wraps) over a scrim on the Sessions
   screen. Any key closes it and is eaten (sessions_key; ui_keyboard_route sends even Ctrl+Tab here
   while it is up), and so does a touch: on the press, and the release is swallowed so it can't land
   on a row underneath. It lives on the screen it was opened on and clears s_keys_ov itself. */
static lv_obj_t *s_keys_ov;

static void keys_ov_deleted(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) == s_keys_ov) s_keys_ov = NULL;
}

static void keys_ov_pressed(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_wait_release(in);
    ui_sessions_keys_overlay(false);
}

bool ui_sessions_keys_overlay_shown(void)
{
    return s_keys_ov != NULL && s_scr != NULL && lv_obj_get_screen(s_keys_ov) == s_scr;
}

void ui_sessions_keys_overlay(bool show)
{
    if (!show) {
        if (s_keys_ov) {
            lv_obj_t *o = s_keys_ov;
            s_keys_ov = NULL;
            lv_obj_delete(o);   /* LVGL 9 allows a target to be deleted in its own event */
        }
        return;
    }
    if (s_keys_ov || !s_scr) return;
    lv_obj_t *o = box(s_scr);
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    ui_comp_scrim(o);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE | UI_KEYBOARD_SKIP);
    lv_obj_add_event_cb(o, keys_ov_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(o, keys_ov_deleted, LV_EVENT_DELETE, NULL);

    lv_obj_t *p = box(o);
    ui_comp_sheet(p);
    lv_obj_set_size(p, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(p);
    lv_obj_set_style_radius(p, UI_TK_RADIUS, 0);
    lv_obj_set_style_pad_hor(p, 32, 0);
    lv_obj_set_style_pad_ver(p, UI_TK_GAP_L, 0);
    lv_obj_set_style_pad_row(p, UI_TK_GAP_S, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(p, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *t = text(p, "Keyboard shortcuts", &st_body, TC_FG);
    lv_obj_set_style_pad_bottom(t, UI_TK_GAP_S, 0);
    /* the keys column is as wide as its widest entry, so neither column wraps */
    int32_t kw = 0;
    for (int i = 0; hk_overlay[i].keys; i++) {
        lv_point_t sz;
        lv_text_get_size(&sz, hk_overlay[i].keys, FONT_SMALL, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x > kw) kw = sz.x;
    }
    for (int i = 0; hk_overlay[i].keys; i++) {
        lv_obj_t *r = box(p);
        lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(r, UI_TK_GAP_L, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_t *k = text(r, hk_overlay[i].keys, &st_label, TC_MUTED);
        lv_obj_set_width(k, kw);
        text(r, hk_overlay[i].what, &st_label, TC_FG);
    }
    lv_obj_t *h = text(p, "Any key or a touch closes this", &st_label, TC_MUTED);
    lv_obj_set_style_pad_top(h, UI_TK_GAP_S, 0);
    s_keys_ov = o;
}

/* ------------------------------------------------------------------ keys */

static void voice_toast(void) { ui_toast_ms(HK_VOICE_TOAST, 3000); }

static void type_cp(uint32_t c)
{
    char b[5] = { 0 };
    if (c < 0x20) return;
    if (c < 0x80) b[0] = (char)c;
    else if (c < 0x800) { b[0] = 0xC0 | (c >> 6); b[1] = 0x80 | (c & 0x3F); }
    else if (c < 0x10000) { b[0] = 0xE0 | (c >> 12); b[1] = 0x80 | ((c >> 6) & 0x3F); b[2] = 0x80 | (c & 0x3F); }
    else { b[0] = 0xF0 | (c >> 18); b[1] = 0x80 | ((c >> 12) & 0x3F); b[2] = 0x80 | ((c >> 6) & 0x3F); b[3] = 0x80 | (c & 0x3F); }
    const char *d = draft();
    if (strlen(d) + strlen(b) > SM_ANSWER_MAX) return;
    char buf[SM_ANSWER_MAX + 1];
    snprintf(buf, sizeof(buf), "%s%s", d, b);
    draft_put(buf);
}

static void backspace(void)
{
    const char *d = draft();
    size_t n = strlen(d);
    if (!n) return;
    char buf[SM_ANSWER_MAX + 1];
    strlcpy(buf, d, sizeof(buf));
    do n--; while (n && (buf[n] & 0xC0) == 0x80);
    buf[n] = 0;
    draft_put(buf);
}

static bool sessions_key(const key_event_t *ev, void *user)
{
    (void)user;
    if (s_keys_ov) { ui_sessions_keys_overlay(false); return true; }   /* any key closes it, eaten */
    /* Local shortcuts first: they work with no rows on screen and never reach the draft. */
    switch (hk_dashboard(ev)) {
    case HK_D_OPEN:     sessions_open_selected(); render(); return true;
    case HK_D_SETTINGS: ui_nav_push(ui_settings_create, NULL); return true;
    case HK_D_TERMINAL: ui_nav_push(ui_hosts_create, NULL); return true;
    case HK_D_KEYS:     ui_sessions_keys_overlay(true); return true;
    case HK_D_VOICE:    voice_toast(); return true;   /* reserved for voice: no space is typed */
    case HK_D_LOCAL:    return true;
    case HK_D_NONE:     break;
    }
    if (s_inert == 1 || s_n <= 0) return true;   /* dimmed or no rows: nothing to act on */
    const cc_session_t *s = selected();
    if (ev->type == KEY_EV_SPECIAL) {
        switch (ev->key) {
        case SK_UP:    select_index(s_sel - 1); break;
        case SK_DOWN:  select_index(s_sel + 1); break;
        case SK_PGUP:  select_index(s_sel - 4); break;
        case SK_PGDN:  select_index(s_sel + 4); break;
        case SK_HOME:  select_index(0); break;
        case SK_END:   select_index(s_n - 1); break;
        case SK_ENTER: enter_pressed(); break;
        case SK_BACKSPACE: if (field_visible(s)) backspace(); break;
        case SK_ESC:   draft_put(""); break;   /* C24: Esc clears the draft and never leaves Sessions */
        default: break;
        }
        render();
        return true;
    }
    if (ev->mods & (MOD_CTRL | MOD_META)) return true;   /* hk_dashboard took every such character */
    if (field_visible(s)) type_cp(ev->cp);   /* digits too: a digit is typed, never a single-key pick */
    return true;
}

/* ------------------------------------------------------------------ refresh */

static void load_snapshot(void)
{
    if (s_poll_gen == s_seen_poll && s_n >= 0) return;
    s_seen_poll = s_poll_gen;
    if (!s_snap || !s_items) return;
    int n = cc_poll_snapshot(s_snap, SNAP_CAP);
    if (n < 0) return;
    n = cc_sessions_visible(s_snap, n, 4);
    if (n > MAX_ROWS) n = MAX_ROWS;
    bool changed = n != s_n;
    for (int i = 0; !changed && i < n; i++) changed = !same_session(&s_snap[i], &s_items[i]);
    if (!changed) return;
    memcpy(s_items, s_snap, sizeof(cc_session_t) * n);
    s_n = n;
    /* the selection follows the session; a session that went away leaves the row now at its place */
    int sel = sm_select(s_items, n, s_sel_key, s_sel);
    if ((sel < 0 ? 0 : sel) != s_sel) s_scroll_sel = true;   /* the selected session moved: keep it in view */
    s_sel = sel < 0 ? 0 : sel;
    char k[SM_KEY_MAX] = "";
    if (n > 0) sm_key(&s_items[s_sel], k, sizeof(k));
    if (strcmp(k, s_sel_key)) { strlcpy(s_sel_key, k, sizeof(s_sel_key)); s_options = 0; s_dirty_log = true; request_logs(true); }
    if (s_drafts) sm_drafts_prune(s_drafts, s_items, n);   /* a draft goes with its session */
    /* a note belongs to the state it was made in */
    int64_t now = mono_ms();
    for (int j = 0; s_notes && j < NOTES_MAX; j++) {
        note_t *nt = &s_notes[j];
        if (nt->kind == NOTE_NONE) continue;
        if (nt->kind == NOTE_BUSY) {
            if (!s_act_pending) nt->kind = NOTE_NONE;   /* its result found no session */
            continue;
        }
        bool keep = false;
        for (int i = 0; i < n; i++) {
            sm_key(&s_items[i], k, sizeof(k));
            if (strcmp(k, nt->key) == 0) { keep = s_items[i].state == nt->state; break; }
        }
        if (nt->kind == NOTE_ERROR || nt->kind == NOTE_SENT) keep = true;   /* these outlive the state change they explain */
        if (!keep || now - nt->at_ms > NOTE_TTL_MS) nt->kind = NOTE_NONE;
    }
    s_dirty_rows = s_dirty_pane = true;
}

/* Stale rows after a wake from Off or deeper (I was 15 s), until a result newer than the wake arrives.
   The power owner records the generation at the wake, so a Sessions screen built later judges the same
   wake the same way. A wake from Dim never marks them: the link ran at I = 3 s throughout. */
static pr_cards_t cards_view(void)
{
    /* a key during a Hibernate check-in boot is an ordinary wake: the same Waking line */
    bool hib = hibernate_boot_kind() == POWER_BOOT_HIB_WAKE || hibernate_boot_kind() == POWER_BOOT_HIB_CHECKIN;
    bool dark_wake = power_wake_seq() > 0 && power_woke_from() >= PS_OFF;
    if (!hib && !dark_wake) return PR_CARDS_LIVE;
    return pr_cards_view(hib, cc_poll_generation(), dark_wake ? power_wake_gen() : 0,
                         power_wake_reason() == PW_NEEDS_YOU);
}

static void wifi_settings_cb(lv_event_t *e) { (void)e; ui_nav_push(ui_settings_create, NULL); }   /* Task 7: › Network */
static void host_settings_cb(lv_event_t *e) { (void)e; ui_nav_push(ui_settings_create, NULL); }   /* Task 7: › Host */
static void lowbat_dismiss_cb(lv_event_t *e) { (void)e; s_lowbat_dismiss = true; render(); }

/* The banner's reason for an unreachable Host: cc_link's status text up to its parenthetical
   ("Host not found at my-host.local (set its address …) (retry in 12s)"), capitalised. */
static void unreachable_text(char *out, size_t cap, const char *name, bool too_big, int fails)
{
    if (cc_link_connected() && too_big) { strlcpy(out, "Session list too large to load", cap); return; }
    if (cc_link_connected() && fails >= CC_POLL_FAILS_SHOWN) { strlcpy(out, "Can't load sessions from the host\xe2\x80\xa6", cap); return; }
    if (cc_link_failure() == CC_LINK_FAIL_NONE) { snprintf(out, cap, "Can't reach %s. Is it awake?", name); return; }
    char st[CC_STATUS_MAX];
    cc_link_status_text(st, sizeof(st));
    char *p = strstr(st, " (");
    if (p) *p = 0;
    if (st[0] >= 'a' && st[0] <= 'z') st[0] = (char)(st[0] - 'a' + 'A');
    strlcpy(out, st, cap);
}

static void place_body(int top)
{
    if (top == s_body_top) return;
    s_body_top = top;
    int h = lv_display_get_vertical_resolution(NULL) - UI_TK_FOOTER_H - top;
    lv_obj_t *parts[] = { s_list, s_skel, s_pane, s_nohost, s_empty };
    for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        lv_obj_set_y(parts[i], top);
        lv_obj_set_height(parts[i], h);
    }
    s_dirty_pane = true;   /* the log block changed height */
}

static void update_state(void)
{
    char name[48];
    cc_settings_server_name(name, sizeof(name));
    bool up = cc_link_connected();
    bool wifi = wifi_mgr_is_connected();
    int64_t now = mono_ms();
    pr_cards_t view = cards_view();
    bool too_big = false;
    int fails = up ? cc_poll_failures(&too_big) : 0;
    /* the clock starts once the network could reach the Host: Tailscale takes about a minute to
       register after boot, and that isn't the Host's fault; a LAN Host needs Wi-Fi only (M9) */
    if (!name[0] || up || cc_link_failure() != CC_LINK_FAIL_NONE) s_down_since_ms = 0;
    else {
        char mac[128];
        bool lan_host = cc_link_mac_address(mac, sizeof(mac)) && !ts_host_maybe_tailnet(mac);
        bool net = wifi && (!g_settings.ts_enable || lan_host || tailscale_state() == TS_CONNECTED);
        if (!net) s_down_since_ms = 0;
        else if (!s_down_since_ms) s_down_since_ms = now;
    }
    /* the battery as the power rules see it (their 10 % trigger; `power sim battery` counts): USB re-arms */
    int pct = -1;
    bool charging = false;
    pr_src_t src = power_source(&pct, &charging);
    if (src == PR_SRC_UNKNOWN) pct = -1;
    if (src == PR_SRC_USB) charging = true;
    bool parked = name[0] && checkin_woke_parked() && view == PR_CARDS_STALE;   /* woken from Radio off */
    sm_in_t in = {
        .host_set = name[0] != 0,
        .wifi_up = wifi,
        .wifi_connecting = wifi_mgr_is_connecting() || parked,   /* the radio is coming back */
        .link_up = up,
        .link_failed = !up && cc_link_failure() != CC_LINK_FAIL_NONE,
        .poll_failing = up && (too_big || fails >= CC_POLL_FAILS_SHOWN),
        .reconnecting = parked,
        .waking = view == PR_CARDS_WAKING,
        .stale = view == PR_CARDS_STALE,
        .have_list = s_n >= 0,
        .low_battery_shown = sm_lowbat_step(&s_lowbat, pct, charging, s_lowbat_dismiss),
        .down_ms = s_down_since_ms ? now - s_down_since_ms : 0,
        .n = s_n < 0 ? 0 : s_n,
    };
    s_lowbat_dismiss = false;
    sm_banner_t b = sm_banner(&in);
    char t[CC_STATUS_MAX + 16];
    switch (b) {
    case SM_B_NO_WIFI:
        ui_banner_set(s_banner, UI_BANNER_RED, "No Wi-Fi", "Wi-Fi settings", wifi_settings_cb, NULL); break;
    case SM_B_UNREACHABLE:
        unreachable_text(t, sizeof(t), name, too_big, fails);
        ui_banner_set(s_banner, UI_BANNER_RED, t, "Host settings", host_settings_cb, NULL); break;
    case SM_B_LOW_BATTERY:
        snprintf(t, sizeof(t), "Battery %d %%. The Tab5 sleeps at 10 %%.", pct);
        ui_banner_set(s_banner, UI_BANNER_RED, t, "Dismiss", lowbat_dismiss_cb, NULL); break;
    case SM_B_RECONNECTING:
        ui_banner_set(s_banner, UI_BANNER_MUTED, "Reconnecting to host\xe2\x80\xa6", NULL, NULL, NULL); break;
    case SM_B_LOADING:
        ui_banner_set(s_banner, UI_BANNER_MUTED, "Loading sessions\xe2\x80\xa6", NULL, NULL, NULL); break;
    case SM_B_STALE:
        ui_banner_set(s_banner, UI_BANNER_MUTED, "Updating\xe2\x80\xa6", NULL, NULL, NULL); break;
    default:
        ui_banner_set(s_banner, UI_BANNER_MUTED, NULL, NULL, NULL, NULL); break;
    }
    bool banner = !lv_obj_has_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    if ((int)banner != s_banner_shown) {
        s_banner_shown = banner;
        place_body(UI_TK_HEADER_H + (banner ? UI_TK_BANNER_H : 0));
    }
    /* the body */
    bool offline = b == SM_B_NO_WIFI || b == SM_B_UNREACHABLE;
    body_t body;
    if (!in.host_set) body = BODY_NO_HOST;
    else if (s_n < 0) body = offline ? BODY_NONE : BODY_LOADING;
    else if (s_n == 0) body = offline ? BODY_NONE : BODY_EMPTY;
    else body = BODY_LIST;
    if ((int)body != s_body) {
        s_body = body;
        set_shown(s_list, body == BODY_LIST);
        set_shown(s_skel, body == BODY_LOADING);
        set_shown(s_nohost, body == BODY_NO_HOST);
        set_shown(s_empty, body == BODY_EMPTY);
        s_dirty_pane = s_dirty_rows = true;
    }
    if (body != BODY_LIST) set_shown(s_pane, false);
    /* no Wi-Fi, unreachable or reconnecting: the last rows stay, at 50 % and inert */
    bool inert = offline || b == SM_B_RECONNECTING || !up;
    if ((int)inert != s_inert) {
        static const char *names[] = { "live", "stale", "waking" };
        ESP_LOGI(TAG, "rows: %s (%s)", inert ? "inert" : "live", names[view]);
        s_inert = inert;
        lv_obj_set_style_opa(s_list, inert ? LV_OPA_50 : LV_OPA_COVER, 0);
        lv_obj_set_style_opa(s_pane, inert ? LV_OPA_50 : LV_OPA_COVER, 0);
    }
}

static void render(void)
{
    if (!s_scr) return;
    if (s_dirty_rows) { s_dirty_rows = false; rows_update(); }
    if (s_scroll_sel) {
        s_scroll_sel = false;
        if (s_sel >= 0 && s_sel < MAX_ROWS && s_rows[s_sel] && s_body == BODY_LIST) {
            lv_obj_update_layout(s_list);
            lv_obj_scroll_to_view(s_rows[s_sel], LV_ANIM_OFF);   /* a no-op when it is in view */
        }
    }
    if (s_dirty_pane && s_body == BODY_LIST) { s_dirty_pane = false; pane_update(); }
    if (s_dirty_log) { s_dirty_log = false; log_update(); }
    if (s_dirty_pane && s_body == BODY_LIST) { s_dirty_pane = false; pane_update(); }   /* the options note */
}

static void notes_expire(void)
{
    int64_t now = mono_ms();
    for (int k = 0; s_notes && k < NOTES_MAX; k++) {
        note_t *n = &s_notes[k];
        if (n->kind == NOTE_SENT && now - n->at_ms >= SENT_MS) { n->kind = NOTE_NONE; s_dirty_pane = true; }
        /* a request whose result matched no session leaves a BUSY note behind */
        if (n->kind == NOTE_BUSY && !s_act_pending) { n->kind = NOTE_NONE; s_dirty_pane = true; }
    }
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    subscribe_when_ready();
    load_snapshot();
    apply_action_result();
    if (!s_scr) return;   /* opening a session replaced the screen */
    forget_check();       /* P4-3: a forget finished by a later connect */
    notes_expire();
    res_lock();
    unsigned lg = s_logs.gen;
    res_unlock();
    if (lg != s_seen_logs) { s_seen_logs = lg; s_dirty_log = true; }
    update_state();
    if (s_inert != 1) request_logs(false);
    if (++s_ticks % AGE_TICKS == 0) s_dirty_rows = s_dirty_pane = true;   /* ages */
    render();
}

bool ui_sessions_selected_center(int32_t *x, int32_t *y)
{
    if (!s_scr || s_scr != lv_screen_active() || s_body != BODY_LIST || s_n <= 0 || s_sel < 0 ||
        s_sel >= MAX_ROWS || !s_rows[s_sel] || lv_obj_has_flag(s_rows[s_sel], LV_OBJ_FLAG_HIDDEN))
        return false;
    lv_area_t a, l;
    lv_obj_get_coords(s_rows[s_sel], &a);
    lv_obj_get_coords(s_list, &l);
    *x = (a.x1 + a.x2) / 2;
    *y = (a.y1 + a.y2) / 2;
    return *y >= l.y1 && *y <= l.y2;   /* a scrolled-out row has no point to touch */
}

int ui_sessions_open_test_session(const char *id)
{
    /* the same gate as Ctrl+O plus the screen: with Sessions gone, nothing collects the open check's result */
    if (!s_scr || s_scr != lv_screen_active() || s_inert != 0 || !cc_link_connected()) return UI_SESS_TEST_NOT_SHOWING;
    for (int i = 0; i < s_n; i++) {
        const cc_session_t *s = &s_items[i];
        if (!s->background || strcmp(s->id, id) != 0) continue;
        if (strncmp(s->name, "companion-test-", 15) != 0) return UI_SESS_TEST_NO_CARD;   /* only the agent's own test sessions */
        select_index(i);
        ESP_LOGI(TAG, "debug: take-over of %s requested", id);
        sessions_open_selected();
        render();
        return UI_SESS_TEST_OK;
    }
    return UI_SESS_TEST_NO_CARD;
}

/* ------------------------------------------------------------------ screen */

/* Home has nothing to go back to, and a swipe does nothing. */
static void sessions_gesture_cb(lv_dir_t dir) { (void)dir; }

static void chip_select_cb(lv_event_t *e)
{
    (void)e;
    if (s_inert == 1 || s_n <= 0) return;
    select_index(sm_next_needs_you(s_items, s_n, s_sel));
    render();
}
static void chip_send_cb(lv_event_t *e) { (void)e; enter_pressed(); render(); }
static void chip_open_cb(lv_event_t *e) { (void)e; sessions_open_selected(); render(); }
static void chip_terminal_cb(lv_event_t *e) { (void)e; ui_nav_push(ui_hosts_create, NULL); }
static void chip_settings_cb(lv_event_t *e) { (void)e; ui_nav_push(ui_settings_create, NULL); }
static void chip_keys_cb(lv_event_t *e) { (void)e; ui_sessions_keys_overlay(true); }
static void set_up_host_cb(lv_event_t *e) { (void)e; ui_nav_push(ui_settings_create, NULL); }   /* Task 7: › Host */
static void mic_cb(lv_event_t *e) { (void)e; voice_toast(); }

/* A tap focuses the field; the soft keyboard is only a fallback with no physical keyboard. Its keys
   come back through keyboard_inject to the sink, like the physical ones. */
static void field_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (!s_ta) return;
    lv_obj_add_state(s_ta, LV_STATE_FOCUSED);
    if (!keyboard_any_present() && !s_kbd_attached) {
        softkbd_attach_textarea(s_ta, s_scr);
        s_kbd_attached = true;
        lv_obj_send_event(s_ta, LV_EVENT_CLICKED, NULL);   /* the attach missed this tap */
    }
}

/* The mic slot (reserved for voice): 64 × 64, a dashed line border and a muted mic, drawn disabled. */
static lv_obj_t *mic_slot(lv_obj_t *parent)
{
    static const lv_point_precise_t top[] = { { 6, 0 }, { 58, 0 } }, bottom[] = { { 6, 63 }, { 58, 63 } };
    static const lv_point_precise_t left[] = { { 0, 6 }, { 0, 58 } }, right[] = { { 63, 6 }, { 63, 58 } };
    static const lv_point_precise_t cup[] = { { 0, 0 }, { 0, 4 }, { 3, 9 }, { 9, 11 }, { 15, 9 }, { 18, 4 }, { 18, 0 } };
    static const lv_point_precise_t stem[] = { { 9, 0 }, { 9, 5 } };
    lv_obj_t *m = lv_button_create(parent);
    lv_obj_remove_style_all(m);
    lv_obj_add_style(m, &st_mic, 0);
    lv_obj_set_size(m, UI_TK_TARGET, UI_TK_TARGET);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(m, (UI_TK_HIT - UI_TK_TARGET) / 2);
    lv_obj_add_event_cb(m, mic_cb, LV_EVENT_CLICKED, NULL);
    const lv_point_precise_t *sides[] = { top, bottom, left, right };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *l = lv_line_create(m);
        lv_obj_add_style(l, &st_dash, 0);
        lv_line_set_points(l, sides[i], 2);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t *cap = box(m);
    lv_obj_add_style(cap, &st_capsule, 0);
    lv_obj_set_size(cap, 12, 20);
    lv_obj_set_pos(cap, 26, 14);
    lv_obj_t *c = lv_line_create(m);
    lv_obj_add_style(c, &st_mic_line, 0);
    lv_line_set_points(c, cup, sizeof(cup) / sizeof(cup[0]));
    lv_obj_set_pos(c, 23, 28);
    lv_obj_t *s = lv_line_create(m);
    lv_obj_add_style(s, &st_mic_line, 0);
    lv_line_set_points(s, stem, 2);
    lv_obj_set_pos(s, 23, 39);
    return m;
}

static lv_obj_t *build_pane(lv_obj_t *scr, int W, int top, int h)
{
    lv_obj_t *p = box(scr);
    lv_obj_set_pos(p, UI_TK_LIST_W, top);
    lv_obj_set_size(p, W - UI_TK_LIST_W, h);
    lv_obj_set_style_pad_hor(p, UI_TK_MARGIN, 0);
    lv_obj_set_style_pad_ver(p, BODY_PAD_V, 0);
    lv_obj_set_style_pad_row(p, UI_TK_GAP_M, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);

    /* 1. the title: the full name (wrapped, never cut) and the pill; the repo and age under it */
    lv_obj_t *head = box(p);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(head, 12, 0);
    s_title = text(head, "", &st_body, TC_FG);
    lv_obj_set_width(s_title, 1);
    lv_obj_set_flex_grow(s_title, 1);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_WRAP);
    s_pill = ui_state_pill(head);
    s_meta = text(p, "", &st_label, TC_MUTED);
    lv_obj_set_width(s_meta, LV_PCT(100));
    lv_label_set_long_mode(s_meta, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_margin_top(s_meta, -12, 0);   /* it belongs to the title */

    /* 2. the log block */
    s_logbox = box(p);
    lv_obj_add_style(s_logbox, &st_logbox, 0);
    lv_obj_set_width(s_logbox, LV_PCT(100));
    lv_obj_set_flex_grow(s_logbox, 1);
    /* pre-wrapped to the block (sm_wrap_tail_fn): the label never wraps again on its own, and a row
       wider than the block is clipped by it rather than pushing the rows below out (the preview's fit
       check fails such a row) */
    s_log = text(s_logbox, "", &st_mono, TC_FG);

    /* interactive sessions (B3) and stopped ones: one muted line instead of the answer rows */
    s_info = text(p, "", &st_label, TC_MUTED);
    lv_obj_set_width(s_info, LV_PCT(100));
    lv_label_set_long_mode(s_info, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_add_flag(s_info, LV_OBJ_FLAG_HIDDEN);

    /* 3. the answer row: the field and the mic slot */
    s_answer_row = box(p);
    lv_obj_set_size(s_answer_row, LV_PCT(100), UI_TK_TARGET);
    lv_obj_set_flex_flow(s_answer_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_answer_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_answer_row, 12, 0);
    ui_comp_pass_hits(s_answer_row);
    s_ta = lv_textarea_create(s_answer_row);
    lv_obj_add_style(s_ta, &st_field, 0);
    lv_obj_add_style(s_ta, &st_field_focus, LV_STATE_FOCUSED);
    lv_obj_add_style(s_ta, &st_cursor, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_add_style(s_ta, &st_muted, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_width(s_ta, 1);
    lv_obj_set_flex_grow(s_ta, 1);
    lv_textarea_set_one_line(s_ta, true);   /* sets a content height: the 64 px comes after it */
    lv_obj_set_height(s_ta, UI_TK_TARGET);
    lv_obj_set_style_pad_ver(s_ta, (UI_TK_TARGET - 4 - lv_font_get_line_height(FONT_BODY)) / 2, 0);
    lv_textarea_set_max_length(s_ta, SM_ANSWER_MAX);
    lv_obj_set_scrollbar_mode(s_ta, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_ext_click_area(s_ta, (UI_TK_HIT - UI_TK_TARGET) / 2);
    lv_obj_add_event_cb(s_ta, field_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(s_ta, LV_STATE_FOCUSED);   /* keys come from the sink; the field is their target */
    mic_slot(s_answer_row);

    /* 4. the action row: Send and Open, and a note on the right */
    s_action_row = box(p);
    lv_obj_set_size(s_action_row, LV_PCT(100), UI_TK_TARGET);
    lv_obj_set_flex_flow(s_action_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_action_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_action_row, 12, 0);
    ui_comp_pass_hits(s_action_row);
    s_send = ui_btn(s_action_row, UI_BTN_PRIMARY, "\xe2\x86\xb5 Send", chip_send_cb, NULL);
    ui_btn(s_action_row, UI_BTN_SECONDARY, "\xe2\x8c\x83O Open", chip_open_cb, NULL);
    s_note = text(s_action_row, "", &st_label, TC_MUTED);
    lv_obj_set_width(s_note, 1);
    lv_obj_set_flex_grow(s_note, 1);
    lv_obj_set_style_text_align(s_note, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_note, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_note, lv_font_get_line_height(FONT_SMALL));
    return p;
}

/* Rebuilding over itself (ui_nav_refresh, ui_nav_home from Sessions) deletes the old screen first
   (P4-2); the checks still keep a stale delete from touching a newer screen's widgets and sink. */
static void scr_delete_cb(lv_event_t *e)
{
    lv_obj_t *scr = lv_event_get_target_obj(e);
    keyboard_clear_sink(scr);   /* only if it is still this screen's sink */
    if (scr != s_scr) return;
    s_scr = s_needs = s_banner = s_list = s_skel = s_nohost = s_empty = NULL;
    s_pane = s_title = s_pill = s_meta = s_logbox = s_log = s_info = NULL;
    s_answer_row = s_ta = s_action_row = s_send = s_note = NULL;
    s_keys_ov = NULL;
    memset(s_rows, 0, sizeof(s_rows));
    /* PT11: the rate stays 3 s on every other screen (Active/Dim); Off sets its floor */
}

lv_obj_t *ui_sessions_create(void *arg)
{
    (void)arg;
    init_once();
    styles_init();
    int W = lv_display_get_horizontal_resolution(NULL), H = lv_display_get_vertical_resolution(NULL);
    lv_obj_t *scr = ui_screen_base();
    s_scr = scr;
    lv_obj_add_event_cb(scr, scr_delete_cb, LV_EVENT_DELETE, NULL);
    ui_screen_set_gesture(scr, sessions_gesture_cb);
    int top = UI_TK_HEADER_H, h = H - UI_TK_FOOTER_H - top;

    /* the list: 440 px, 72 px rows 8 apart, a pool of 16 (hidden until used) */
    s_list = box(scr);
    lv_obj_add_style(s_list, &st_list, 0);
    lv_obj_set_pos(s_list, 0, top);
    lv_obj_set_size(s_list, UI_TK_LIST_W, h);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_left(s_list, LIST_PAD_L, 0);
    lv_obj_set_style_pad_right(s_list, LIST_PAD_R, 0);
    lv_obj_set_style_pad_ver(s_list, BODY_PAD_V, 0);
    lv_obj_set_style_pad_row(s_list, UI_TK_GAP_S, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < MAX_ROWS; i++) {
        s_rows[i] = ui_session_row(s_list, row_click_cb, (void *)(intptr_t)i);
        lv_obj_add_event_cb(s_rows[i], row_pressed_cb, LV_EVENT_PRESSED, (void *)(intptr_t)i);
        lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
    }
    /* loading: 3 skeleton rows where the list goes */
    s_skel = box(scr);
    lv_obj_add_style(s_skel, &st_list, 0);
    lv_obj_set_pos(s_skel, 0, top);
    lv_obj_set_size(s_skel, UI_TK_LIST_W, h);
    lv_obj_set_style_pad_left(s_skel, LIST_PAD_L, 0);
    lv_obj_set_style_pad_right(s_skel, LIST_PAD_R, 0);
    lv_obj_set_style_pad_ver(s_skel, BODY_PAD_V, 0);
    lv_obj_set_style_pad_row(s_skel, UI_TK_GAP_S, 0);
    lv_obj_set_flex_flow(s_skel, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < 3; i++) ui_skeleton_row(s_skel);
    lv_obj_add_flag(s_skel, LV_OBJ_FLAG_HIDDEN);
    /* no Host, and an empty list: a centred line (and a button) across the body */
    lv_obj_t *centred[2];
    for (int i = 0; i < 2; i++) {
        lv_obj_t *c = box(scr);
        lv_obj_set_pos(c, 0, top);
        lv_obj_set_size(c, W, h);
        lv_obj_set_style_pad_hor(c, 160, 0);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        ui_comp_pass_hits(c);
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        centred[i] = c;
    }
    s_nohost = centred[0];
    s_empty = centred[1];
    ui_comp_pass_hits(ui_empty_state(s_nohost, "Set up your host to see its Claude sessions.", "Set up host", set_up_host_cb, NULL));
    ui_empty_state(s_empty, "No sessions on this host. Start one with `claude --bg`.", NULL, NULL, NULL);
    s_pane = build_pane(scr, W, top, h);

    /* the bars last, over the body */
    lv_obj_t *hd = ui_header(scr, "Sessions", NULL);
    s_needs = ui_header_needs_you(hd);
    s_banner = ui_banner(scr, UI_TK_HEADER_H);
    lv_obj_t *f = ui_footer(scr);
    lv_obj_t *l = ui_footer_group(f, false), *r = ui_footer_group(f, true);
    ui_chip(l, "\xe2\x86\x91\xe2\x86\x93 Select", chip_select_cb, NULL);
    ui_chip(l, "\xe2\x86\xb5 Send", chip_send_cb, NULL);
    ui_chip(l, "\xe2\x8c\x83O Open", chip_open_cb, NULL);
    ui_chip(r, "\xe2\x8c\x83T/\xe2\x8c\x83M Terminal", chip_terminal_cb, NULL);   /* Ctrl+M: the two-thumb alias */
    ui_chip(r, "\xe2\x8c\x83P Settings", chip_settings_cb, NULL);
    ui_chip(r, "\xe2\x8c\x83K Keys", chip_keys_cb, NULL);

    key_sink_t sink = { .key = sessions_key, .user = scr };   /* per screen */
    keyboard_set_sink(&sink);
    cc_poll_set_rate(3);

    s_visit++;   /* answers to requests from an earlier visit are not acted on */
    s_kbd_attached = false;
    s_body_top = s_body = s_banner_shown = s_inert = -1;
    s_cols = s_rows_fit = 0;
    s_dirty_rows = s_dirty_pane = s_dirty_log = true;
    s_seen_poll = s_poll_gen - 1;   /* load the snapshot now */
    lv_timer_t *t = lv_timer_create(tick_cb, TICK_MS, NULL);
    ui_timer_bind(t, scr);
    tick_cb(t);
    if (s_rows[s_sel] && s_body == BODY_LIST) lv_obj_scroll_to_view(s_rows[s_sel], LV_ANIM_OFF);
    return scr;
}
