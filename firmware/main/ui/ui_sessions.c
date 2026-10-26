/* Sessions (spec §5.1, B3, B5): the home screen, master-detail.
 *
 * Data comes from cc_link's poll (a subscriber only bumps a counter; the 300 ms tick copies the
 * snapshot). Everything that talks to the Host -- log tails, answers, the live check before opening,
 * a stop, the forget-host-key job -- runs on a small worker task, never on the LVGL task, and every command
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
#include "ui_settings.h"
#include "ui_comp.h"
#include "ui_softkbd.h"
#include "ui/sessions_model.h"
#include "ui/ui_settings_fw.h"
#include "ui/hotkeys.h"
#include "voice/voice_rules.h"
#include "voice/voice.h"
#include "companion/cc_link.h"
#include "companion/cc_sessions.h"
#include "companion/cc_cmd.h"
#include "companion/cc_logview.h"
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
#define TAIL_LINES       40      /* cc_logview_tail (or cc_log_tail) keeps this many lines; the wrap keeps what fits */
#define LOGS_CAP         CC_LOGS_TAIL_BYTES   /* 512 KB: `claude logs` since its latest clear; PSRAM, only during a log job */
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
#define VOICE_NOTE_MS    5000    /* "Check the text, then press Enter to send" shows this long */
#define VOICE_METER_MS   100     /* P5-4: the meter's timer, only while recording */
#define VOICE_SECS_AFTER_MS 3000 /* the transcribing note adds its seconds after this */
#define VOICE_RED_LAST_S 5       /* the timer turns red for the last 5 s */
#define STOP_TIMEOUT_MS  30000   /* `claude stop` waits for the session's process to end; a timeout keeps the link */
#define STOP_POLL_MS     3000    /* the refresh after a stop (the watch, every 3 s, catches what this misses) */
#define STOP_TOAST_MS    4000

/* The transcript goes into the draft of the session key the recording was started for (spec 5.3). */
_Static_assert(VR_KEY_MAX == SM_KEY_MAX && VR_TEXT_MAX == SM_ANSWER_MAX, "voice keys and text must fit the drafts");

/* ------------------------------------------------------------------ state */

typedef enum { NOTE_NONE, NOTE_BUSY, NOTE_SENT, NOTE_APPROVE, NOTE_OPEN_TO_ANSWER, NOTE_ERROR, NOTE_VOICE } note_kind_t;
typedef struct {
    char key[SM_KEY_MAX];
    note_kind_t kind;
    char text[160];
    cc_state_t state;      /* the session's state when the note was made */
    int64_t at_ms;
} note_t;

typedef enum { JOB_LOGS, JOB_PASTE, JOB_CHECK, JOB_FORGET_KEY, JOB_TRUST_KEY, JOB_VOICE_CHECK, JOB_STOP, JOB_FW_CHECK, JOB_FW_START } job_type_t;
typedef struct {
    job_type_t type;
    unsigned token;        /* which request this is (actions only) */
    char id[16];
    char text[SM_ANSWER_MAX + 1];
} job_t;

/* Worker results, read by the tick (guarded by res_lock). s_logs_text is PSRAM. */
static struct { unsigned gen; unsigned token; job_type_t type; char id[16]; esp_err_t err; int rc; char msg[160]; } s_act;
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
static lv_obj_t *s_answer_row, *s_ta, *s_action_row, *s_send, *s_open, *s_stop, *s_note;
static int s_body_top = -1, s_body = -1, s_banner_shown = -1, s_inert = -1;
static int s_cols, s_rows_fit;             /* the log block, measured */
static bool s_dirty_rows, s_dirty_pane, s_dirty_log, s_scroll_sel;
static unsigned s_ticks;
static bool s_kbd_attached;                /* the soft keyboard is attached to this screen's field */
/* Voice input (Plan 5, spec 5-6): the mic button and its glyph, the voice strip in the action row's note slot */
static lv_obj_t *s_mic, *s_mic_glyph[3], *s_mic_dot;
static lv_obj_t *s_vstrip, *s_vtimer, *s_vmeter;
static lv_obj_t *s_vcancel;                /* the strip's Cancel button: shown while recording or transcribing (touch Esc) */
static lv_obj_t *s_vhint_keys, *s_vhint_touch;   /* the strip's hint, both wordings built once (no realloc mid-recording) */
static bool s_kbd_up;                      /* the soft keyboard is up: the pane ends above it (touch alone) */
static lv_obj_t *s_footer;                 /* hidden under the soft keyboard: its chips would sit under the keys */
static lv_timer_t *s_meter_timer;          /* P5-4: exists only while recording */
static vr_keys_t s_vkeys;                  /* Ctrl+Space and the mic tap: the repeat and toggle guard */
static bool s_voice_mine;                  /* a recording Sessions started may still post a result */
static char s_vnote[64];                   /* "Transcribing on the Host… 5 s" while transcribing, else "" */
static bool s_vrec;                        /* recording (the strip is up): the action row stays shown (M3) */
static voice_result_t *s_vres;              /* PSRAM: the transcript is a secret like typed text; zeroed after use */
typedef enum { MIC_IDLE, MIC_STARTING, MIC_RECORDING, MIC_TRANSCRIBING } mic_look_t;
static int s_mic_look = -1;

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
static lv_style_t st_list, st_logbox, st_field, st_field_focus, st_cursor, st_mic, st_mic_rec, st_mic_line, st_capsule;
static lv_style_t st_glyph_fg, st_glyph_muted, st_glyph_red, st_dot, st_meter, st_meter_ind;

static void styles_init(void)
{
    if (s_styles_ready) return;
    s_styles_ready = true;
    lv_style_t *all[] = { &st_list, &st_fg, &st_muted, &st_green, &st_red, &st_body, &st_label, &st_mono, &st_logbox,
                          &st_field, &st_field_focus, &st_cursor, &st_mic, &st_mic_rec, &st_mic_line, &st_capsule,
                          &st_glyph_fg, &st_glyph_muted, &st_glyph_red, &st_dot, &st_meter, &st_meter_ind };
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
    /* the mic button (spec 6.1): panel-2, a solid 1 px line border; 2 px red while recording */
    lv_style_set_bg_color(&st_mic, HEX(UI_TK_PANEL2));
    lv_style_set_bg_opa(&st_mic, LV_OPA_COVER);
    lv_style_set_radius(&st_mic, UI_TK_RADIUS);
    lv_style_set_border_color(&st_mic, HEX(UI_TK_LINE));
    lv_style_set_border_width(&st_mic, 1);
    lv_style_set_border_color(&st_mic_rec, HEX(UI_TK_RED));
    lv_style_set_border_width(&st_mic_rec, 2);
    lv_style_set_line_width(&st_mic_line, 2);
    lv_style_set_line_rounded(&st_mic_line, true);
    lv_style_set_border_width(&st_capsule, 2);
    lv_style_set_radius(&st_capsule, LV_RADIUS_CIRCLE);
    /* the glyph's colour: lines take line_color, the capsule border_color */
    lv_style_t *gl[] = { &st_glyph_fg, &st_glyph_muted, &st_glyph_red };
    const uint32_t gc[] = { UI_TK_FG, UI_TK_MUTED, UI_TK_RED };
    for (int i = 0; i < 3; i++) { lv_style_set_line_color(gl[i], HEX(gc[i])); lv_style_set_border_color(gl[i], HEX(gc[i])); }
    /* the recording dot (12 px, red) and the level meter (160 x 8: fg on line, radius 4) */
    lv_style_set_bg_color(&st_dot, HEX(UI_TK_RED));
    lv_style_set_bg_opa(&st_dot, LV_OPA_COVER);
    lv_style_set_radius(&st_dot, LV_RADIUS_CIRCLE);
    lv_style_set_bg_color(&st_meter, HEX(UI_TK_LINE));
    lv_style_set_bg_opa(&st_meter, LV_OPA_COVER);
    lv_style_set_radius(&st_meter, 4);
    lv_style_set_bg_color(&st_meter_ind, HEX(UI_TK_FG));
    lv_style_set_bg_opa(&st_meter_ind, LV_OPA_COVER);
    lv_style_set_radius(&st_meter_ind, 4);
    lv_style_set_anim_duration(&st_meter_ind, 0);
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



static void res_lock(void);
static void res_unlock(void);

/* Forget host key (P4-3). The worker's result is shown on the LVGL task. A forget done in time toasts
   from forget_done_cb on whatever screen is up; one that timed out and is finished by a later connect
   is toasted by the Sessions tick. Both go through s_forget_seen, so each forget toasts once. */
static char s_forget_toast[128];

/* The worker's outcome, passed as ui_async's arg; the text is composed here, on the LVGL task, so only
   that task writes s_forget_toast (review M3). */
enum { FORGET_TIMED_OUT = 1, FORGET_NO_HOST, FORGET_ENDED, TRUST_TIMED_OUT, TRUST_REFUSED, TRUST_ENDED };

static void forget_toast_cb(void *arg)
{
    intptr_t why = (intptr_t)arg;
    str_id_t t = why == FORGET_TIMED_OUT ? STR_SES_FORGET_RECONNECTING : why == FORGET_ENDED ? STR_SES_FORGET_ENDED
               : why == TRUST_TIMED_OUT ? STR_SES_TRUST_WAITING : why == TRUST_REFUSED ? STR_SES_TRUST_REFUSED
               : why == TRUST_ENDED ? STR_SES_TRUST_ENDED : STR_LINK_NO_HOST;
    strlcpy(s_forget_toast, tr(t), sizeof(s_forget_toast));
    ui_toast_ms(s_forget_toast, FORGET_TOAST_MS);
    ui_settings_refill();   /* Settings › Host's key rows follow (nothing when Settings isn't shown) */
}

static void forget_check(void)
{
    char fp[80];
    uint32_t n = cc_link_forget_done(fp, sizeof(fp));
    if (n == s_forget_seen) return;
    s_forget_seen = n;
    snprintf(s_forget_toast, sizeof(s_forget_toast), "%s %s", tr(STR_SES_NEW_KEY_PINNED), fp);
    ui_toast_ms(s_forget_toast, FORGET_TOAST_MS);
    ui_settings_key_pinned();   /* the pinned fingerprint replaces the Trust or Forget row; the focus isn't on Forget */
}

static void forget_done_cb(void *arg) { (void)arg; forget_check(); }

static void run_forget(void)
{
    char fp[80];
    esp_err_t e = cc_link_forget_host_key(fp, sizeof(fp), CC_FORGET_TIMEOUT_MS);
    ESP_LOGI(TAG, "forget host key: %s %s", esp_err_to_name(e), fp);
    if (e == ESP_OK) { ui_async(forget_done_cb, NULL); return; }
    ui_async(forget_toast_cb, (void *)(intptr_t)(e == ESP_ERR_TIMEOUT ? FORGET_TIMED_OUT
                                                 : e == ESP_ERR_INVALID_RESPONSE ? FORGET_ENDED : FORGET_NO_HOST));
}

/* Trust this key (polish 1): a forget that pins only the key the user confirmed. NOT_ALLOWED: a pin or a carry source
   is there after all; INVALID_RESPONSE: it ended without a pin (expired, another Host, a pin appeared); INVALID_STATE:
   no Host. Nothing is called trusted before a key is pinned (fix round 1, M2). */
static void run_trust(const char *want)
{
    char fp[80];
    esp_err_t e = cc_link_trust_host_key(want, fp, sizeof(fp), CC_FORGET_TIMEOUT_MS);
    ESP_LOGI(TAG, "trust host key %s: %s %s", want, esp_err_to_name(e), fp);
    if (e == ESP_OK) { ui_async(forget_done_cb, NULL); return; }
    ui_async(forget_toast_cb, (void *)(intptr_t)(e == ESP_ERR_TIMEOUT ? TRUST_TIMED_OUT : e == ESP_ERR_NOT_ALLOWED ? TRUST_REFUSED
                                                 : e == ESP_ERR_INVALID_RESPONSE ? TRUST_ENDED : FORGET_NO_HOST));
}

/* Settings › Host › Voice input · Check (P5-5). The worker passes the string id to the LVGL task. */
static volatile bool s_voice_check_pending;

static void voice_check_toast_cb(void *arg) { ui_toast_ms(tr((str_id_t)(intptr_t)arg), 5000); }

static void run_voice_check(void)
{
    int rc = -1;
    esp_err_t e = cc_link_voice_check(&rc);
    ESP_LOGI(TAG, "voice check: %s rc %d", esp_err_to_name(e), rc);
    str_id_t id;
    if (e == ESP_ERR_INVALID_STATE) id = STR_SES_NOT_CONNECTED;
    else if (e == ESP_ERR_TIMEOUT) id = STR_VOICE_TIMEOUT;
    else if (e != ESP_OK) id = vr_out_text(VR_OUT_FAILED);
    else if (rc == 0) id = STR_SET_VOICE_READY;
    else id = vr_out_text(vr_host_result(rc));
    s_voice_check_pending = false;
    ui_async(voice_check_toast_cb, (void *)(intptr_t)id);
}

/* Whether the link's newest list still has id as the same conversation (session UUID sid) with a live process: the
   stop is tied to the conversation, not only the 8-hex id (stop review M2). */
static bool stop_still_same(const char *id, const char *sid)
{
    cc_session_t *all = heap_caps_calloc(SNAP_CAP, sizeof(cc_session_t), MALLOC_CAP_SPIRAM);
    if (!all) return false;
    int n = cc_poll_snapshot(all, SNAP_CAP);
    bool ok = false;
    for (int i = 0; i < n; i++)
        if (all[i].background && !strcmp(all[i].id, id)) { ok = all[i].live && !strcmp(all[i].session_id, sid); break; }
    heap_caps_free(all);
    return ok;
}

/* Ctrl+X or the Stop button, after the confirm: `claude stop <id>` (cc_cmd_stop), for the session the confirm named
   (j->text holds its session UUID). Its output is not kept or shown: a failure shows the exit status only. It runs as
   a slow exec: keepalives go on and a timeout doesn't drop the link (stop review M4). A stop that worked refreshes the
   list, so the row turns ENDED without waiting for the watch. */
static void run_stop(const job_t *j, const char *claude)
{
    char cmd[192], out[64], err[64];
    int status = -1;
    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (!stop_still_same(j->id, j->text)) e = ESP_ERR_NOT_FOUND;
    else if (cc_cmd_stop(claude, j->id, cmd, sizeof(cmd)))
        e = cc_link_exec_slow(cmd, out, sizeof(out), &status, err, sizeof(err), STOP_TIMEOUT_MS);
    if (e == ESP_ERR_INVALID_SIZE) e = ESP_OK;   /* more output than out holds: it is dropped anyway */
    ESP_LOGI(TAG, "stop %s: %s, exit %d", j->id, esp_err_to_name(e), status);
    res_lock();
    s_act.gen++;
    s_act.token = j->token;
    s_act.type = JOB_STOP;
    strlcpy(s_act.id, j->id, sizeof(s_act.id));
    s_act.err = e;
    s_act.rc = status;
    s_act.msg[0] = 0;
    res_unlock();
    s_act_pending = false;
    if (e == ESP_OK && status == 0) cc_link_poll_now(STOP_POLL_MS);
}

static void run_job(const job_t *j)
{
    char claude[64], cmd[192], msg[160] = "";
    if (j->type == JOB_FORGET_KEY) { run_forget(); return; }
    if (j->type == JOB_TRUST_KEY) { run_trust(j->text); return; }
    if (j->type == JOB_VOICE_CHECK) { run_voice_check(); return; }
    if (j->type == JOB_FW_CHECK) { ui_fw_job_check(); return; }   /* Plan 6 (P6-16) */
    if (j->type == JOB_FW_START) { ui_fw_job_start(); return; }
    cc_settings_claude_path(claude, sizeof(claude));
    if (j->type == JOB_STOP) { run_stop(j, claude); return; }
    if (j->type == JOB_LOGS) {
        bool ok = false;
        int status = -1;
        /* PSRAM, and only for this job: the raw output, and the conversation read off its rendered screen */
        char *raw = heap_caps_malloc(LOGS_CAP, MALLOC_CAP_SPIRAM), *view = heap_caps_malloc(LOG_TEXT_CAP, MALLOC_CAP_SPIRAM);
        const char *src = msg;
        static int last_view = 1;   /* the last cc_logview_tail result logged: one line per change, never text */
        static bool no_psram;        /* the same for the buffers: logged when they start failing, not every job */
        if ((!raw || !view) != no_psram) {
            no_psram = !raw || !view;
            if (no_psram) ESP_LOGW(TAG, "logs: no PSRAM for %u + %u bytes", (unsigned)LOGS_CAP, (unsigned)LOG_TEXT_CAP);
        }
        if (raw && view && cc_cmd_logs(claude, j->id, cmd, sizeof(cmd))) {
            /* the bytes since the stream's latest full clear: what its screen needs (cc_logview.h) */
            esp_err_t e = cc_link_exec_screen(cmd, raw, LOGS_CAP, &status, msg, sizeof(msg), 10000);
            ok = (e == ESP_OK || e == ESP_ERR_INVALID_SIZE) && status == 0;
            if (ok) {
                size_t rl = strlen(raw);
                int v = cc_logview_tail(raw, rl, e == ESP_OK, TAIL_LINES, view, LOG_TEXT_CAP);
                if (v <= 0 && rl && v != last_view)
                    ESP_LOGW(TAG, "logs: screen view %d (%u bytes%s)", v, (unsigned)rl, e == ESP_OK ? "" : ", front cut");
                last_view = v;
                if (v > 0) {
                    src = view;
                } else {   /* no screen to read (a cut stream, no memory, an unknown screen): the escape-stripping heuristic */
                    char *start = raw;
                    while ((*start & 0xC0) == 0x80) start++;   /* the tail may start mid-character */
                    cc_logview_fallback(start, TAIL_LINES);   /* without the TUI's chrome */
                    src = start;
                }
            }
        }
        /* keep the end of a tail longer than the result buffer, from a line start */
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
        heap_caps_free(raw);
        heap_caps_free(view);
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
    s_logs_text = heap_caps_calloc(1, LOG_TEXT_CAP, MALLOC_CAP_SPIRAM);
    s_tail = heap_caps_calloc(1, LOG_TEXT_CAP, MALLOC_CAP_SPIRAM);
    s_wrapped = heap_caps_calloc(1, WRAP_CAP, MALLOC_CAP_SPIRAM);
    s_status = heap_caps_calloc(1, STATUS_CAP, MALLOC_CAP_SPIRAM);
    s_vres = heap_caps_calloc(1, sizeof(voice_result_t), MALLOC_CAP_SPIRAM);
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

bool ui_sessions_trust_host_key(const char *fp)
{
    if (!fp || !fp[0]) return false;
    init_once();
    job_t *j = job_new(JOB_TRUST_KEY);
    if (!j) return false;
    strlcpy(j->text, fp, sizeof(j->text));
    bool ok = job_post(j);
    heap_caps_free(j);
    return ok;
}

/* Plan 6 (P6-16): the firmware check and Install jobs; ui_settings_fw.c keeps their pending flags */
static bool post_fw_job(job_type_t type)
{
    init_once();
    job_t *j = job_new(type);
    if (!j) return false;
    bool ok = job_post(j);
    heap_caps_free(j);
    return ok;
}
bool ui_sessions_fw_check(void) { return post_fw_job(JOB_FW_CHECK); }
bool ui_sessions_fw_start(void) { return post_fw_job(JOB_FW_START); }

bool ui_sessions_voice_check(void)
{
    init_once();
    if (s_voice_check_pending) return false;
    job_t *j = job_new(JOB_VOICE_CHECK);
    if (!j) return false;
    s_voice_check_pending = true;
    bool ok = job_post(j);
    if (!ok) s_voice_check_pending = false;
    heap_caps_free(j);
    return ok;
}

/* ------------------------------------------------------------------ helpers */

static const cc_session_t *selected(void) { return (s_n > 0 && s_sel >= 0 && s_sel < s_n) ? &s_items[s_sel] : NULL; }
/* Quick answers need a running session; Open takes any background session, because `claude attach`
   starts a stopped or finished one again (as agent view does). */
static bool actionable(const cc_session_t *s)
{
    return s && s->background && s->id[0] && s->live && s->state != CC_ENDED;
}
static bool openable(const cc_session_t *s) { return s && s->background && s->id[0]; }
/* Ctrl+X and Stop: a background session whose process runs (any state: a DONE one may still be live). A stopped one
   has nothing to stop, and an interactive one is never acted on. */
static bool stoppable(const cc_session_t *s) { return s && s->background && s->id[0] && s->live; }

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

/* "repo · age" (the age once the clock is set); "on Host · repo · age" for an interactive session */
static void meta_of(const cc_session_t *s, char *out, size_t cap)
{
    char a[16] = "";
    int64_t now = wall_ms();
    if (now && s->started_ms > 0) cc_age(now, s->started_ms, a, sizeof(a));
    sm_meta(s, a, out, cap);   /* an interactive session's leads with "on Host" (P4C-11) */
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
    if (!mine) { s_tail[0] = 0; t = tr(STR_SES_LOADING_OUTPUT); c = TC_MUTED; }
    else if (!ok) { t = s_tail[0] ? s_tail : tr(STR_SES_NO_OUTPUT); c = TC_MUTED; }
    else if (!s_tail[0]) { t = tr(STR_SES_NO_OUTPUT_YET); c = TC_MUTED; }
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

/* A button's width as its text sets it (the action row's buttons are content-sized), laid out or not. */
static int32_t btn_width(lv_obj_t *b)
{
    lv_obj_t *l = lv_obj_get_child(b, 0);
    lv_point_t sz;
    lv_text_get_size(&sz, lv_label_get_text(l), lv_obj_get_style_text_font(l, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t w = sz.x + lv_obj_get_style_pad_left(b, 0) + lv_obj_get_style_pad_right(b, 0) + 2 * lv_obj_get_style_border_width(b, 0);
    int32_t mw = lv_obj_get_style_min_width(b, 0);
    return w > mw ? w : mw;
}

/* Whether the action row has room for Stop beside Send and Open (as shown now) and the whole note: Stop gives way to a
   note that would be cut, so a voice or error note is never shortened by it. */
static bool stop_fits(const char *note)
{
    int32_t gap = lv_obj_get_style_pad_column(s_action_row, 0);
    int32_t avail = lv_obj_get_style_width(s_pane, 0) - lv_obj_get_style_pad_left(s_pane, 0) - lv_obj_get_style_pad_right(s_pane, 0);
    if (!lv_obj_has_flag(s_send, LV_OBJ_FLAG_HIDDEN)) avail -= btn_width(s_send) + gap;
    if (!lv_obj_has_flag(s_open, LV_OBJ_FLAG_HIDDEN)) avail -= btn_width(s_open) + gap;
    avail -= btn_width(s_stop) + gap;
    if (!note || !note[0]) return avail >= 0;
    lv_point_t sz;
    lv_text_get_size(&sz, note, FONT_SMALL, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x <= avail;
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
    bool act = actionable(s), open = openable(s);
    bool field = field_visible(s);
    note_t *n = note_of(s);
    bool changed = set_shown(s_logbox, !interactive);
    changed |= set_shown(s_answer_row, field);
    /* M3: while recording or transcribing, the action row stays for the strip or the note whatever is selected (a row
       with no answer field and no Open too): the recording goes on, so the screen must say so */
    changed |= set_shown(s_action_row, open || s_vrec || s_vnote[0]);
    set_shown(s_send, field);
    /* recording or transcribing, Open is inert (review M1): Cancel takes its room, so the strip fits the row */
    set_shown(s_open, open && !s_vrec && !s_vnote[0]);
    /* the info line: interactive sessions (B3), and background ones that can't be acted on */
    const char *info = NULL;
    tc_t ic = TC_MUTED;
    if (interactive) info = tr(STR_SES_INTERACTIVE);
    else if (!act) {
        if (n && n->kind == NOTE_ERROR) { info = n->text; ic = TC_RED; }
        else info = tr(STR_SES_STOPPED_OPEN);
    }
    changed |= set_shown(s_info, info != NULL);
    if (info) { set_text(s_info, info); set_colour(s_info, ic); }
    if (field) {
        const char *ph = s->state == CC_NEEDS_YOU ? tr(STR_SES_TYPE_ANSWER) : tr(STR_SES_TYPE_MESSAGE);
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
        case NOTE_SENT:           nt = tr(STR_SES_SENT); nc = TC_GREEN; break;
        case NOTE_APPROVE:        nt = tr(STR_SES_OPEN_TO_APPROVE); break;
        case NOTE_OPEN_TO_ANSWER: nt = tr(STR_SES_OPEN_TO_ANSWER); break;
        case NOTE_ERROR:          nt = n->text; nc = TC_RED; break;
        case NOTE_VOICE:          nt = n->text; break;
        default: break;
        }
    } else if (act && s_options >= 2) {
        snprintf(b, sizeof(b), "1\xe2\x80\x93%d %s", s_options, tr(STR_SES_PICKS_OPTION));
        nt = b;
    }
    if (s_vnote[0]) { nt = s_vnote; nc = TC_MUTED; }   /* transcribing: the one thing going on */
    /* Stop (Ctrl+X by touch) only for a running session, never while recording or transcribing (the strip and Cancel
       need the room, and the keys are eaten then too), and only while the note still fits beside it */
    changed |= set_shown(s_stop, stoppable(s) && !s_vrec && !s_vnote[0] && stop_fits(nt));
    set_text(s_note, nt);
    set_colour(s_note, nc);
    if (measure_log() || changed) s_dirty_log = true;
}

static void select_index(int i);
static void sessions_open_selected(void);
static void render(void);
static bool voice_busy(void);
static void pane_fit_kbd(void);

/* A tap belongs to the session under the finger when it went down (review I1): a poll can re-sort the
   rows between the press and the release, and the row object then shows another session. */
static char s_press_key[SM_KEY_MAX];

static void row_pressed_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    s_press_key[0] = 0;
    if (i >= 0 && i < s_n) sm_key(&s_items[i], s_press_key, sizeof(s_press_key));
}

/* a press that slid off the row (a scroll) never ends in a click: forget it (T6 N1) */
static void row_press_lost_cb(lv_event_t *e) { (void)e; s_press_key[0] = 0; }

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
        if (openable(s) && !voice_busy()) { sessions_open_selected(); render(); }   /* like Enter (spec 5.2) */
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
    if (s_act_pending) { ui_toast(tr(STR_SES_STILL_WORKING)); return; }
    job_t *j = job_new(type);
    if (!j) return;
    j->token = ++s_req_token;
    s_req_visit = s_visit;
    s_req_at_ms = mono_ms();
    strlcpy(j->id, s->id, sizeof(j->id));
    if (t) strlcpy(j->text, t, sizeof(j->text));
    s_act_pending = true;
    note_set(s, NOTE_BUSY, tr(type == JOB_PASTE ? STR_SES_SENDING : type == JOB_STOP ? STR_SES_STOPPING : STR_SES_OPENING));
    if (!job_post(j)) {
        s_act_pending = false;
        note_set(s, NOTE_ERROR, tr(STR_SET_BUSY));
    }
    heap_caps_free(j);
}

static void sessions_open_selected(void)
{
    const cc_session_t *s = selected();
    if (!openable(s) || s_inert == 1) return;
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
    if (!openable(s)) { note_set(s, NOTE_ERROR, tr(STR_SES_CANT_OPEN)); return; }
    int idx = cc_settings_server();
    if (idx < 0 || !cc_cmd_attach(cc_settings_claude_path(claude, sizeof(claude)), s->id, cmd, sizeof(cmd))) {
        note_set(s, NOTE_ERROR, tr(STR_SES_CANT_OPEN));
        return;
    }
    note_clear(s);
    ESP_LOGI(TAG, "opening %s", s->id);
    power_hold(POWER_HOLD_TAKEOVER, true);   /* a take-over never dims or sleeps */
    cc_poll_set_rate(15);   /* the watch keeps feeding the keyboard light; nothing redraws */
    ui_terminal_open_command(idx, cmd, s->name, session_closed_cb, NULL);
}

static void enter_pressed(void)
{
    const cc_session_t *s = selected();
    if (!openable(s) || s_act_pending || s_inert == 1) return;
    if (field_visible(s) && draft()[0]) { post_action(JOB_PASTE, s, draft()); return; }
    sessions_open_selected();   /* an empty field (or an open-to-answer note) opens it */
}

static void stop_result(const char *id, esp_err_t err, int rc);

static void apply_action_result(void)
{
    res_lock();
    unsigned gen = s_act.gen, token = s_act.token;
    job_type_t type = s_act.type;
    char id[16], msg[160];
    strlcpy(id, s_act.id, sizeof(id));
    strlcpy(msg, s_act.msg, sizeof(msg));
    esp_err_t err = s_act.err;
    int rc = s_act.rc;
    res_unlock();
    if (gen == s_seen_act) return;
    s_seen_act = gen;
    if (token != s_req_token) return;   /* an answer to an older request */
    if (type == JOB_STOP) { stop_result(id, err, rc); return; }   /* toasted even when the row is gone */
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
    if (err == ESP_ERR_INVALID_STATE) strlcpy(msg, tr(STR_SES_NOT_CONNECTED), sizeof(msg));
    else if (!msg[0]) strlcpy(msg, esp_err_to_name(err), sizeof(msg));
    if (msg[0] >= 'a' && msg[0] <= 'z') msg[0] = (char)(msg[0] - 'a' + 'A');
    note_set(s, NOTE_ERROR, msg);
    if (s != selected()) ui_toast(msg);   /* the pane shows another session: say it here */
}

/* ------------------------------------------------------------------ stop (Ctrl+X) */

/* The confirm is tied to the session selected when it opened, by key (and id), never to what is selected when it is
   answered: a poll may re-sort the rows, move the selection or end the session meanwhile. The answer then stops that
   same session if the newest list still has it running, and is refused otherwise. gen ties an answer to the latest
   confirm. */
static struct { char key[SM_KEY_MAX]; char id[16]; char sid[40]; unsigned gen; } s_stop_ask;
/* Sessions stopped here this boot: listed past the finished limit (sm_visible), RAM only (stop review I2). */
static sm_pins_t s_pins;

static const cc_session_t *item_by_key(const char *key);
static void load_snapshot(void);

static void stop_result(const char *id, esp_err_t err, int rc)
{
    const cc_session_t *s = NULL;
    for (int i = 0; i < s_n; i++)
        if (s_items[i].background && strcmp(s_items[i].id, id) == 0) { s = &s_items[i]; break; }
    if (err == ESP_OK && rc == 0) {
        sm_pin_add(&s_pins, id);   /* a background session's key is its id */
        s_seen_poll = s_poll_gen - 1;   /* re-filter now: a list loaded before the pin must not drop it */
        if (s) note_clear(s);
        ui_toast(tr(STR_SES_STOPPED));
        s_logs_at_ms = 0;
        return;
    }
    char m[96];
    if (err == ESP_ERR_NOT_FOUND) strlcpy(m, tr(STR_SES_STOP_GONE), sizeof(m));
    else if (err == ESP_ERR_INVALID_STATE) strlcpy(m, tr(STR_SES_NOT_CONNECTED), sizeof(m));
    else if (err == ESP_ERR_TIMEOUT) strlcpy(m, tr(STR_SES_STOP_TIMEOUT), sizeof(m));
    else if (err == ESP_OK) snprintf(m, sizeof(m), tr(STR_SES_STOP_FAILED), rc);   /* the exit status, never the output */
    else strlcpy(m, tr(STR_SES_STOP_ERROR), sizeof(m));
    if (s) note_set(s, NOTE_ERROR, m);
    ui_toast_ms(m, STOP_TOAST_MS);
}

static void stop_yes(bool yes, void *ud)
{
    unsigned gen = (unsigned)(uintptr_t)ud;
    if (!yes || gen != s_stop_ask.gen) return;   /* Cancel, Esc, or an older confirm */
    s_stop_ask.gen = 0;
    if (!s_scr) return;   /* Sessions is gone: nothing would show the result */
    load_snapshot();      /* judge it against the newest list */
    const cc_session_t *s = item_by_key(s_stop_ask.key);
    if (!s || strcmp(s->id, s_stop_ask.id) != 0 || strcmp(s->session_id, s_stop_ask.sid) != 0) {
        ui_toast(tr(STR_SES_STOP_GONE)); render(); return;
    }
    if (!stoppable(s)) { ui_toast(tr(STR_SES_STOP_NOT_RUNNING)); render(); return; }
    if (voice_busy()) return;
    post_action(JOB_STOP, s, s_stop_ask.sid);   /* s, not selected(): the session the confirm named */
    render();
}

/* Ctrl+X and the Stop button: asks first, Cancel first (Enter on the dialog cancels). */
static void stop_ask(void)
{
    const cc_session_t *s = selected();
    if (!s || s_inert == 1) return;
    if (!stoppable(s)) { ui_toast(tr(STR_SES_STOP_NOT_RUNNING)); return; }
    if (s_act_pending) { ui_toast(tr(STR_SES_STILL_WORKING)); return; }
    sm_key(s, s_stop_ask.key, sizeof(s_stop_ask.key));
    strlcpy(s_stop_ask.id, s->id, sizeof(s_stop_ask.id));
    strlcpy(s_stop_ask.sid, s->session_id, sizeof(s_stop_ask.sid));
    static unsigned gens;
    if (++gens == 0) gens = 1;   /* 0 means no confirm is open */
    s_stop_ask.gen = gens;
    char title[160];
    snprintf(title, sizeof(title), tr(STR_SES_STOP_Q), s->name);
    ui_confirm_ex(title, tr(STR_SES_STOP_BODY), tr(STR_SES_STOP_ACTION), true, stop_yes, (void *)(uintptr_t)gens);
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

    lv_obj_t *t = text(p, tr(STR_SES_KEYS_TITLE), &st_body, TC_FG);
    lv_obj_set_style_pad_bottom(t, UI_TK_GAP_S, 0);
    /* the keys column is as wide as its widest entry, so neither column wraps */
    int32_t kw = 0;
    for (int i = 0; i < hk_overlay_n; i++) {
        lv_point_t sz;
        lv_text_get_size(&sz, tr(hk_overlay[i].keys), FONT_SMALL, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x > kw) kw = sz.x;
    }
    for (int i = 0; i < hk_overlay_n; i++) {
        lv_obj_t *r = box(p);
        lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(r, UI_TK_GAP_L, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_t *k = text(r, tr(hk_overlay[i].keys), &st_label, TC_MUTED);
        lv_obj_set_width(k, kw);
        text(r, tr(hk_overlay[i].what), &st_label, TC_FG);
    }
    lv_obj_t *h = text(p, tr(STR_SES_KEYS_CLOSE), &st_label, TC_MUTED);
    lv_obj_set_style_pad_top(h, UI_TK_GAP_S, 0);
    s_keys_ov = o;
}

/* ------------------------------------------------------------------ voice input (Plan 5) */

/* The voice service owns the recording; Sessions shows it and routes the keys (spec 5.2). A recording is tied to
   the session key selected at its start: the transcript goes into that session's draft, whatever is selected
   when it arrives, and is never sent (Review Focus 1). */
static bool voice_recording(const voice_status_t *v) { return v->phase == VOICE_STARTING || v->phase == VOICE_RECORDING; }

/* The service's status as Sessions acts on it. A run the user cancelled is idle here (review I1): its result is
   dropped, so the keys pass, the mic is idle and there is no transcribing note, even while a cancelled transcription
   still waits for the Host. A Ctrl+Space then gets VR_OUT_BUSY from voice_start ("Still transcribing…"). */
static void voice_get(voice_status_t *v)
{
    static bool was_rec;
    voice_status(v);
    if (v->cancelled) v->phase = VOICE_IDLE;
    /* Review M7: a recording that ended without the user's key (the 30 s limit, a failed check) starts the toggle
       guard, so the stop press they meant for it starts nothing. A user's stop or Esc passes here too: the guard
       then runs a moment longer, which is harmless. */
    bool rec = voice_recording(v);
    if (was_rec && !rec) vr_on_voice_ended(&s_vkeys, mono_ms());
    was_rec = rec;
}

/* Recording or transcribing (not cancelled): Enter, Send, Open and a tap on the selected row do nothing (review M1).
   A send would change the draft under the recording; an open deletes Sessions and cancels it. */
static bool voice_busy(void)
{
    voice_status_t v;
    voice_get(&v);
    return v.phase != VOICE_IDLE;
}

static const cc_session_t *item_by_key(const char *key)
{
    char k[SM_KEY_MAX];
    for (int i = 0; i < s_n; i++) {
        sm_key(&s_items[i], k, sizeof(k));
        if (strcmp(k, key) == 0) return &s_items[i];
    }
    return NULL;
}

/* The mic button (spec 6.1): idle a fg glyph; starting a red ring and a red glyph; recording a red ring and a red
   dot; transcribing a muted glyph. Only on a change: a static screen must not redraw. */
static void mic_look(mic_look_t look)
{
    if (!s_mic || (int)look == s_mic_look) return;
    s_mic_look = look;
    bool ring = look == MIC_STARTING || look == MIC_RECORDING;
    if (ring) lv_obj_add_style(s_mic, &st_mic_rec, 0);
    else lv_obj_remove_style(s_mic, &st_mic_rec, 0);
    lv_style_t *c = look == MIC_STARTING ? &st_glyph_red : look == MIC_TRANSCRIBING ? &st_glyph_muted : &st_glyph_fg;
    for (int i = 0; i < 3; i++) {
        lv_obj_remove_style(s_mic_glyph[i], &st_glyph_fg, 0);
        lv_obj_remove_style(s_mic_glyph[i], &st_glyph_muted, 0);
        lv_obj_remove_style(s_mic_glyph[i], &st_glyph_red, 0);
        lv_obj_add_style(s_mic_glyph[i], c, 0);
        set_shown(s_mic_glyph[i], look != MIC_RECORDING);
    }
    set_shown(s_mic_dot, look == MIC_RECORDING);
}

/* The mic, the voice strip (while recording) and the transcribing note, from the service's status. */
static void voice_view(const voice_status_t *v)
{
    bool rec = voice_recording(v);
    mic_look(v->phase == VOICE_STARTING ? MIC_STARTING : v->phase == VOICE_RECORDING ? MIC_RECORDING
             : v->phase == VOICE_TRANSCRIBING ? MIC_TRANSCRIBING : MIC_IDLE);
    set_shown(s_vstrip, rec);
    set_shown(s_note, !rec);   /* the strip takes the note's slot */
    set_shown(s_vcancel, v->phase != VOICE_IDLE);   /* Esc by touch: after the strip, or after the transcribing note */
    if (rec) {   /* a keyboard may come or go: both wordings exist, so this only flips HIDDEN (no allocation) */
        bool touch = ui_touch_text(STR_VOICE_HINT) != STR_VOICE_HINT;
        set_shown(s_vhint_keys, !touch);
        set_shown(s_vhint_touch, touch);
    }
    if (rec != s_vrec) { s_vrec = rec; s_dirty_pane = true; }
    if (rec && s_vtimer && s_vmeter) {
        int sec = v->elapsed_ms / 1000;
        if (sec < 0) sec = 0;
        if (sec > VR_MAX_S) sec = VR_MAX_S;
        char t[24];
        snprintf(t, sizeof(t), "%d:%02d / %d:%02d", sec / 60, sec % 60, VR_MAX_S / 60, VR_MAX_S % 60);
        set_text(s_vtimer, t);
        set_colour(s_vtimer, sec >= VR_MAX_S - VOICE_RED_LAST_S ? TC_RED : TC_FG);
        int m = v->phase == VOICE_RECORDING ? v->meter : 0;
        m = m < 0 ? 0 : m > 100 ? 100 : m;
        if (lv_bar_get_value(s_vmeter) != m) lv_bar_set_value(s_vmeter, m, LV_ANIM_OFF);
    }
    char n[sizeof(s_vnote)] = "";
    if (v->phase == VOICE_TRANSCRIBING) {
        if (v->transcribing_ms >= VOICE_SECS_AFTER_MS)
            snprintf(n, sizeof(n), "%s %d %s", tr(STR_VOICE_TRANSCRIBING), v->transcribing_ms / 1000, tr(STR_UNIT_S));
        else strlcpy(n, tr(STR_VOICE_TRANSCRIBING), sizeof(n));
    }
    if (strcmp(n, s_vnote)) { strlcpy(s_vnote, n, sizeof(s_vnote)); s_dirty_pane = true; }
}

/* P5-4: the 100 ms timer updates the timer label and the meter while recording, and deletes itself after. */
static void meter_cb(lv_timer_t *t)
{
    voice_status_t v;
    voice_get(&v);
    if (!voice_recording(&v)) { lv_timer_delete(t); s_meter_timer = NULL; }
    voice_view(&v);
    if (!s_meter_timer) render();
}

static void meter_ensure(void)
{
    if (!s_meter_timer && s_scr) s_meter_timer = lv_timer_create(meter_cb, VOICE_METER_MS, NULL);
}

lv_timer_t *ui_sessions_voice_meter(void) { return s_meter_timer; }

/* A voice error: red in the session's note slot, which cuts the longer copy short, and whole in a toast (the README
   pointer must be readable). The toast stays 6 s, long enough to read the longest copy, so it sits above the answer
   field rather than at the bottom: the field, the mic, Send and Open stay in view (polish 2). */
#define VOICE_ERROR_TOAST_MS 6000
static void voice_toast(str_id_t id)
{
    lv_obj_t *above = s_answer_row && lv_obj_is_visible(s_answer_row) ? s_answer_row : s_action_row;
    ui_toast_above_ms(tr(ui_touch_text(id)), VOICE_ERROR_TOAST_MS, above);
}

static void voice_error(const cc_session_t *s, str_id_t id)
{
    note_set(s, NOTE_ERROR, tr(ui_touch_text(id)));
    voice_toast(id);
}

/* The one path for Ctrl+Space and the mic tap. */
static void voice_toggle(void)
{
    voice_status_t v;
    voice_get(&v);
    vr_key_t a = vr_on_voice_key(&s_vkeys, voice_recording(&v), mono_ms());
    if (a == VR_KEY_IGNORE) return;   /* a key repeat, or a press too soon after the last start or stop */
    if (v.phase == VOICE_TRANSCRIBING) { ui_toast(tr(STR_VOICE_BUSY)); return; }
    if (a == VR_KEY_STOP) {
        voice_stop();
        voice_get(&v);
        voice_view(&v);
        return;
    }
    const cc_session_t *s = selected();
    if (!field_visible(s)) { ui_toast(tr(STR_VOICE_NEEDS_FIELD)); return; }
    vr_out_t o = voice_start(s_sel_key);
    if (o != VR_OUT_STARTED) {
        str_id_t id = vr_out_text(o);
        if (o == VR_OUT_BUSY) ui_toast(tr(id));
        else if (id != STR_COUNT) voice_error(s, id);
        return;
    }
    s_voice_mine = true;
    note_clear(s);   /* an older note must not come back when the strip goes */
    meter_ensure();
    voice_get(&v);
    voice_view(&v);
}

/* Esc and the Cancel button: the recording or the transcription is dropped (nothing is applied), the draft stays. */
static void voice_cancel_view(void)
{
    voice_status_t v;
    voice_cancel();
    voice_get(&v);
    voice_view(&v);
}

/* A result from the voice service: into the draft of the session it was recorded for, never sent. */
static void apply_voice_result(const voice_result_t *r)
{
    if (r->out == VR_OUT_CANCELLED || r->out == VR_OUT_STARTED) return;
    if (vr_key_is_term(r->key)) return;   /* a terminal screen's recording: its text never goes into a draft */
    power_note_activity();   /* the text or the error is new on screen: the idle clock restarts (never a wake) */
    const cc_session_t *s = item_by_key(r->key);
    str_id_t id = vr_out_text(r->out);
    if (!s) {
        if (r->out == VR_OUT_TEXT) ui_toast(tr(STR_VOICE_SESSION_GONE));   /* the text is dropped */
        else if (id != STR_COUNT) voice_toast(id);
        return;
    }
    if (r->out != VR_OUT_TEXT) {
        if (id != STR_COUNT) voice_error(s, id);
        return;
    }
    if (!s_drafts) return;
    char *buf = heap_caps_malloc(SM_ANSWER_MAX + 1, MALLOC_CAP_SPIRAM);
    if (!buf) return;
    vr_merge_t m = vr_merge_draft(sm_draft_get(s_drafts, r->key), r->text, buf, SM_ANSWER_MAX);
    if (m != VR_MERGE_EMPTY) sm_draft_set(s_drafts, r->key, buf);
    memset(buf, 0, SM_ANSWER_MAX + 1);
    heap_caps_free(buf);
    const char *what = tr(m == VR_MERGE_CUT ? STR_VOICE_CUT : ui_touch_text(STR_VOICE_REVIEW));
    char n[160];
    if (r->at_limit) snprintf(n, sizeof(n), "%s. %s", tr(STR_VOICE_AT_LIMIT), what);
    else strlcpy(n, what, sizeof(n));
    note_set(s, NOTE_VOICE, n);
    field_sync();
    if (s_ta) lv_obj_add_state(s_ta, LV_STATE_FOCUSED);
}

/* The 300 ms tick's part: the strip and the notes, then a waiting result. A result is taken only while a recording
   Sessions started may still post one, so a console `voice test` keeps its own. */
static void voice_tick(void)
{
    voice_status_t v;
    voice_get(&v);
    if (voice_recording(&v)) meter_ensure();   /* a recording started before this screen was built */
    voice_view(&v);
    if (!s_voice_mine || !s_vres) return;
    if (voice_take_result(s_vres)) {
        apply_voice_result(s_vres);
        memset(s_vres, 0, sizeof(*s_vres));
    }
    if (v.phase == VOICE_IDLE) s_voice_mine = false;   /* the service posts before it goes idle: nothing more comes */
}

/* ------------------------------------------------------------------ keys */

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
    /* Voice (spec 5.2): Ctrl+Space toggles in every state. While recording or transcribing every other key is eaten
       (key clicks would be recorded, and the draft must not change under the recording) except Esc, which cancels. */
    if (hk_dashboard(ev) == HK_D_VOICE) { voice_toggle(); render(); return true; }
    voice_status_t vs;
    voice_get(&vs);
    if (vs.phase != VOICE_IDLE) {
        if (ev->type == KEY_EV_SPECIAL && ev->key == SK_ESC) {
            voice_cancel_view();
            render();
        }
        return true;
    }
    /* Local shortcuts first: they work with no rows on screen and never reach the draft. */
    switch (hk_dashboard(ev)) {
    case HK_D_OPEN:     sessions_open_selected(); render(); return true;
    case HK_D_SETTINGS: ui_nav_push(ui_settings_create, NULL); return true;
    case HK_D_TERMINAL: ui_nav_push(ui_hosts_create, NULL); return true;
    case HK_D_KEYS:     ui_sessions_keys_overlay(true); return true;
    case HK_D_VOICE:    return true;   /* taken above */
    case HK_D_STOP:     if (s_inert != 1 && s_n > 0) { stop_ask(); render(); } return true;   /* dimmed or no rows: inert */
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
    /* P4C-11: finished companion-test-* sessions aren't shown; the 4 newest finished ones are, and (stop review I2) every
       session stopped here this boot */
    n = sm_visible(s_snap, n, 4, &s_pins);
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
        if (nt->kind == NOTE_ERROR || nt->kind == NOTE_SENT || nt->kind == NOTE_VOICE) keep = true;   /* these outlive the state change they explain */
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

static void wifi_settings_cb(lv_event_t *e) { (void)e; ui_settings_open(UI_SET_NETWORK); }
static void host_settings_cb(lv_event_t *e) { (void)e; ui_settings_open(UI_SET_HOST); }
static void lowbat_dismiss_cb(lv_event_t *e) { (void)e; s_lowbat_dismiss = true; render(); }

/* Why the Host link is down, for the Sessions banner and Settings › Host › Link: no host, "Can't reach <name>.
   Is it awake?" when nothing failed, the pointer to Settings › Host for a first key to trust, or cc_link's status text up to its parenthetical ("Host not found at
   my-host.local (set its fallback address …) (retry in 12s)"), capitalised. */
void ui_sessions_link_down_text(char *out, size_t cap)
{
    char name[48];
    cc_settings_server_name(name, sizeof(name));
    if (!name[0]) { strlcpy(out, tr(STR_LINK_NO_HOST), cap); return; }
    if (cc_link_failure() == CC_LINK_FAIL_NONE) { snprintf(out, cap, "%s %s. %s", tr(STR_LINK_CANT_REACH), name, tr(STR_LINK_IS_IT_AWAKE)); return; }
    /* first run: the fingerprint is too long for the banner's one line; Settings › Host shows it, to trust */
    if (cc_link_unknown_host_key(NULL, 0)) { strlcpy(out, tr(STR_SES_NEW_HOST_KEY), cap); return; }
    char st[CC_STATUS_MAX];
    cc_link_status_text(st, sizeof(st));
    char *p = strstr(st, " (");
    if (p) *p = 0;
    if (st[0] >= 'a' && st[0] <= 'z') st[0] = (char)(st[0] - 'a' + 'A');
    strlcpy(out, st, cap);
}

static void unreachable_text(char *out, size_t cap, bool too_big, int fails)
{
    if (cc_link_connected() && too_big) { strlcpy(out, tr(STR_SES_LIST_TOO_BIG), cap); return; }
    if (cc_link_connected() && fails >= CC_POLL_FAILS_SHOWN) { strlcpy(out, tr(STR_SES_CANT_LOAD), cap); return; }
    ui_sessions_link_down_text(out, cap);
}

/* The pane's and the list's height, the one place it is worked out: from under the header (and the banner, when
   shown) to the footer, or to the soft keyboard's top while it is up (review I1), so no row or control sits under
   the keys. Only a change resizes. */
static void pane_size(void)
{
    if (!s_pane || s_body_top < 0) return;
    int h = lv_display_get_vertical_resolution(NULL) - s_body_top - (s_kbd_up ? softkbd_height() : UI_TK_FOOTER_H);
    if (s_list && lv_obj_get_style_height(s_list, 0) != h) lv_obj_set_height(s_list, h);
    if (lv_obj_get_style_height(s_pane, 0) == h) return;
    lv_obj_set_height(s_pane, h);
    s_dirty_pane = true;   /* the log block changed height */
}

static void place_body(int top)
{
    if (top == s_body_top) return;
    s_body_top = top;
    int h = lv_display_get_vertical_resolution(NULL) - UI_TK_FOOTER_H - top;
    lv_obj_t *parts[] = { s_list, s_skel, s_pane, s_nohost, s_empty };
    for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        lv_obj_set_y(parts[i], top);
        if (parts[i] != s_pane && parts[i] != s_list) lv_obj_set_height(parts[i], h);
    }
    pane_size();
    s_dirty_pane = true;
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
        ui_banner_set(s_banner, UI_BANNER_RED, tr(STR_HOSTS_NO_WIFI), tr(STR_SES_WIFI_SETTINGS), wifi_settings_cb, NULL); break;
    case SM_B_UNREACHABLE:
        unreachable_text(t, sizeof(t), too_big, fails);
        ui_banner_set(s_banner, UI_BANNER_RED, t, tr(STR_SES_HOST_SETTINGS), host_settings_cb, NULL); break;
    case SM_B_LOW_BATTERY:
        snprintf(t, sizeof(t), "%s %d %%. %s", tr(STR_SES_BATTERY), pct, tr(STR_SES_SLEEPS_AT));
        ui_banner_set(s_banner, UI_BANNER_RED, t, tr(STR_SES_DISMISS), lowbat_dismiss_cb, NULL); break;
    case SM_B_RECONNECTING:
        ui_banner_set(s_banner, UI_BANNER_MUTED, tr(STR_SES_RECONNECTING), NULL, NULL, NULL); break;
    case SM_B_LOADING:
        ui_banner_set(s_banner, UI_BANNER_MUTED, tr(STR_SES_LOADING), NULL, NULL, NULL); break;
    case SM_B_STALE:
        ui_banner_set(s_banner, UI_BANNER_MUTED, tr(STR_SES_UPDATING), NULL, NULL, NULL); break;
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
        if (n->kind == NOTE_VOICE && now - n->at_ms >= VOICE_NOTE_MS) { n->kind = NOTE_NONE; s_dirty_pane = true; }
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
    ui_fw_note_tick();    /* Plan 6: Updated / rolled back / boot loop, once */
    forget_check();       /* P4-3: a forget finished by a later connect */
    voice_tick();
    pane_fit_kbd();
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
static void chip_send_cb(lv_event_t *e) { (void)e; if (voice_busy()) return; enter_pressed(); render(); }
static void chip_open_cb(lv_event_t *e) { (void)e; if (voice_busy()) return; sessions_open_selected(); render(); }
static void chip_stop_cb(lv_event_t *e) { (void)e; if (voice_busy() || s_inert == 1) return; stop_ask(); render(); }
static void chip_terminal_cb(lv_event_t *e) { (void)e; ui_nav_push(ui_hosts_create, NULL); }
static void chip_settings_cb(lv_event_t *e) { (void)e; ui_nav_push(ui_settings_create, NULL); }
static void chip_keys_cb(lv_event_t *e) { (void)e; ui_sessions_keys_overlay(true); }
static void set_up_host_cb(lv_event_t *e) { (void)e; ui_settings_open(UI_SET_HOST); }
static void mic_cb(lv_event_t *e) { (void)e; voice_toggle(); render(); }
static void vcancel_cb(lv_event_t *e) { (void)e; if (voice_busy()) voice_cancel_view(); render(); }

/* With the soft keyboard up (no physical keyboard), the pane ends above it, so the field, the mic, Send, Open and
   the voice strip's Cancel stay in view; the log block gives up the height. Down on the keyboard (or its Enter in
   a prompt) hides it: the 300 ms tick then gives the pane its height back. Only a change resizes. */
static void pane_fit_kbd(void)
{
    bool up = softkbd_visible();
    if (!s_pane || up == s_kbd_up) return;
    s_kbd_up = up;
    pane_size();   /* with the banner's height, whether it is up or not */
    set_shown(s_footer, !up);
}

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
    pane_fit_kbd();
    render();
}

/* The mic button (spec 6.1): 64 x 64 with an 80 px hit area, a solid line border and a mic glyph; mic_look
   restyles it by the voice state. */
static lv_obj_t *mic_button(lv_obj_t *parent)
{
    static const lv_point_precise_t cup[] = { { 0, 0 }, { 0, 4 }, { 3, 9 }, { 9, 11 }, { 15, 9 }, { 18, 4 }, { 18, 0 } };
    static const lv_point_precise_t stem[] = { { 9, 0 }, { 9, 5 } };
    lv_obj_t *m = lv_button_create(parent);
    lv_obj_remove_style_all(m);
    lv_obj_add_style(m, &st_mic, 0);
    lv_obj_set_size(m, UI_TK_TARGET, UI_TK_TARGET);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(m, (UI_TK_HIT - UI_TK_TARGET) / 2);
    lv_obj_add_event_cb(m, mic_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cap = box(m);
    lv_obj_add_style(cap, &st_capsule, 0);
    lv_obj_set_size(cap, 12, 20);
    lv_obj_set_pos(cap, 26, 14);
    lv_obj_t *c = lv_line_create(m);
    lv_obj_add_style(c, &st_mic_line, 0);
    lv_line_set_points(c, cup, sizeof(cup) / sizeof(cup[0]));
    lv_obj_set_pos(c, 23, 28);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *st = lv_line_create(m);
    lv_obj_add_style(st, &st_mic_line, 0);
    lv_line_set_points(st, stem, 2);
    lv_obj_set_pos(st, 23, 39);
    lv_obj_remove_flag(st, LV_OBJ_FLAG_CLICKABLE);
    s_mic_glyph[0] = cap;
    s_mic_glyph[1] = c;
    s_mic_glyph[2] = st;
    for (int i = 0; i < 3; i++) lv_obj_add_style(s_mic_glyph[i], &st_glyph_fg, 0);
    s_mic_dot = box(m);
    lv_obj_add_style(s_mic_dot, &st_dot, 0);
    lv_obj_set_size(s_mic_dot, 12, 12);
    lv_obj_center(s_mic_dot);
    lv_obj_add_flag(s_mic_dot, LV_OBJ_FLAG_HIDDEN);
    s_mic_look = MIC_IDLE;
    return m;
}

/* The voice strip (spec 6.2), built once with the pane and hidden: shown only while recording, in the note's slot.
   Two lines, right-aligned: the dot, "Recording", the timer and the meter; the hint under them ("Tap the mic to stop"
   with no physical keyboard). Then the Cancel button. */
static void build_voice_strip(lv_obj_t *row)
{
    s_vstrip = box(row);
    lv_obj_set_width(s_vstrip, 1);
    lv_obj_set_flex_grow(s_vstrip, 1);
    lv_obj_set_height(s_vstrip, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_vstrip, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_vstrip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(s_vstrip, 2, 0);
    lv_obj_t *top = box(s_vstrip);
    lv_obj_set_size(top, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 12, 0);
    lv_obj_t *dot = box(top);
    lv_obj_add_style(dot, &st_dot, 0);
    lv_obj_set_size(dot, 12, 12);
    text(top, tr(STR_VOICE_RECORDING), &st_label, TC_RED);
    s_vtimer = text(top, "", &st_label, TC_FG);
    s_vmeter = lv_bar_create(top);
    lv_obj_remove_style_all(s_vmeter);
    lv_obj_add_style(s_vmeter, &st_meter, LV_PART_MAIN);
    lv_obj_add_style(s_vmeter, &st_meter_ind, LV_PART_INDICATOR);
    lv_obj_set_size(s_vmeter, 160, 8);
    lv_bar_set_range(s_vmeter, 0, 100);
    lv_obj_remove_flag(s_vmeter, LV_OBJ_FLAG_CLICKABLE);
    s_vhint_keys = text(s_vstrip, tr(STR_VOICE_HINT), &st_label, TC_MUTED);
    s_vhint_touch = text(s_vstrip, tr(STR_VOICE_HINT_TOUCH), &st_label, TC_MUTED);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *h = i ? s_vhint_touch : s_vhint_keys;
        lv_label_set_long_mode(h, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_max_width(h, LV_PCT(100), 0);
    }
    set_shown(ui_touch_text(STR_VOICE_HINT) == STR_VOICE_HINT ? s_vhint_touch : s_vhint_keys, false);
    lv_obj_add_flag(s_vstrip, LV_OBJ_FLAG_HIDDEN);
    /* Cancel (touch Esc): a 64 px button at the row's end, also shown while transcribing (the strip then gives its
       slot back to the transcribing note). Secondary: red stays the recording's, amber is never voice's. */
    s_vcancel = ui_btn(row, UI_BTN_SECONDARY, tr(STR_CANCEL), vcancel_cb, NULL);
    lv_obj_add_flag(s_vcancel, LV_OBJ_FLAG_HIDDEN);
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
    s_mic = mic_button(s_answer_row);

    /* 4. the action row: Send, Open and Stop, and a note on the right */
    s_action_row = box(p);
    lv_obj_set_size(s_action_row, LV_PCT(100), UI_TK_TARGET);
    lv_obj_set_flex_flow(s_action_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_action_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_action_row, 12, 0);
    ui_comp_pass_hits(s_action_row);
    s_send = ui_btn(s_action_row, UI_BTN_PRIMARY, tr(STR_SES_SEND), chip_send_cb, NULL);
    s_open = ui_btn(s_action_row, UI_BTN_SECONDARY, tr(STR_SES_OPEN), chip_open_cb, NULL);
    /* Stop (Ctrl+X): secondary like Open; the confirm's button is the red one */
    s_stop = ui_btn(s_action_row, UI_BTN_SECONDARY, tr(STR_SES_STOP), chip_stop_cb, NULL);
    lv_obj_add_flag(s_stop, LV_OBJ_FLAG_HIDDEN);
    s_note = text(s_action_row, "", &st_label, TC_MUTED);
    lv_obj_set_width(s_note, 1);
    lv_obj_set_flex_grow(s_note, 1);
    lv_obj_set_style_text_align(s_note, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_note, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_note, lv_font_get_line_height(FONT_SMALL));
    build_voice_strip(s_action_row);
    return p;
}

/* Rebuilding over itself (ui_nav_refresh, ui_nav_home from Sessions) deletes the old screen first
   (P4-2); the checks still keep a stale delete from touching a newer screen's widgets and sink. */
static void scr_delete_cb(lv_event_t *e)
{
    lv_obj_t *scr = lv_event_get_target_obj(e);
    keyboard_clear_sink(scr);   /* only if it is still this screen's sink */
    if (scr != s_scr) return;
    /* leaving Sessions while recording cancels it (spec 5.2); a transcription goes on, and its result waits 60 s */
    voice_status_t vs;
    voice_get(&vs);
    if (voice_recording(&vs)) voice_cancel();
    if (s_meter_timer) { lv_timer_delete(s_meter_timer); s_meter_timer = NULL; }
    s_mic = s_mic_dot = s_vstrip = s_vtimer = s_vmeter = s_vhint_keys = s_vhint_touch = s_vcancel = NULL;
    memset(s_mic_glyph, 0, sizeof(s_mic_glyph));
    s_mic_look = -1;
    s_vnote[0] = 0;
    s_vrec = false;
    s_scr = s_needs = s_banner = s_list = s_skel = s_nohost = s_empty = NULL;
    s_pane = s_title = s_pill = s_meta = s_logbox = s_log = s_info = NULL;
    s_answer_row = s_ta = s_action_row = s_send = s_open = s_stop = s_note = NULL;
    s_keys_ov = s_footer = NULL;
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
        lv_obj_add_event_cb(s_rows[i], row_press_lost_cb, LV_EVENT_PRESS_LOST, NULL);
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
    ui_comp_pass_hits(ui_empty_state(s_nohost, tr(STR_SES_NO_HOST_BODY), tr(STR_SES_SET_UP_HOST), set_up_host_cb, NULL));
    ui_empty_state(s_empty, tr(STR_SES_EMPTY), NULL, NULL, NULL);
    s_pane = build_pane(scr, W, top, h);

    /* the bars last, over the body */
    lv_obj_t *hd = ui_header(scr, tr(STR_SES_TITLE), NULL);
    s_needs = ui_header_needs_you(hd);
    s_banner = ui_banner(scr, UI_TK_HEADER_H);
    lv_obj_t *f = ui_footer(scr);
    s_footer = f;
    lv_obj_t *l = ui_footer_group(f, false), *r = ui_footer_group(f, true);
    ui_chip(l, tr(STR_SES_SELECT), chip_select_cb, NULL);
    ui_chip(l, tr(STR_SES_SEND), chip_send_cb, NULL);
    ui_chip(l, tr(STR_SES_OPEN), chip_open_cb, NULL);
    ui_chip(r, tr(STR_SES_TERMINAL), chip_terminal_cb, NULL);   /* Ctrl+M: the two-thumb alias */
    ui_chip(r, tr(STR_SES_SETTINGS), chip_settings_cb, NULL);
    ui_chip(r, tr(STR_SES_KEYS), chip_keys_cb, NULL);

    key_sink_t sink = { .key = sessions_key, .user = scr };   /* per screen */
    keyboard_set_sink(&sink);
    cc_poll_set_rate(3);

    s_visit++;   /* answers to requests from an earlier visit are not acted on */
    s_kbd_attached = false;
    s_kbd_up = false;
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
