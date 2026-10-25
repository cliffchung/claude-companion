/* Full-screen terminal: the take-over of a Claude session on the Host, or an SSH session to another box
 * (spec §5.2, §5.4). One session bar over the top of the terminal; every key goes to the remote side except
 * Ctrl+] / Ctrl+Q / Ctrl+← (Leave), the Shift scroll keys and Ctrl+Space (voice input, follow-up 2026-10-24; Esc
 * too while this screen's dictation is recording or transcribing). */
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
#include "voice/voice.h"
#include "voice/voice_rules.h"
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
#define VOICE_ERROR_TOAST_MS 6000   /* as Sessions: the README pointer must be readable */
#define VOICE_TYPE_CAP (VR_TEXT_MAX + 14)   /* cc_voice_type_build_ex: the two 6-byte markers, a join space, the NUL */
#define VOICE_HOLD_MS 60000         /* a transcript that couldn't be typed waits this long for a Ctrl+Space */

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

/* One host-key question (review Minor 4). ssh_task waits on it inside on_hostkey, the UI shows and answers it, and the
   screen may go first: so it has three references (the waiter, the UI, the screen), changed under s_hk_lock, and the
   last one frees it. The screen's delete cancels it (answer "no") and never deletes the semaphore under a waiter. */
typedef struct {
    SemaphoreHandle_t sem;
    volatile int answer;        /* 0 none yet, 1 trust, 2 no */
    volatile bool cancelled;    /* the screen went */
    char fp[120];
    bool changed;
    int refs;
} hk_req_t;
static SemaphoreHandle_t s_hk_lock;

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
    hk_req_t *hk_req;           /* the host-key question this screen holds a reference to, if any */
    uint32_t gen;               /* this session's number: a confirm raised for it answers only it */
    bool closing;
    /* command mode (the take-over) */
    bool cmd_mode, cmd_done, got_data;
    void (*on_close)(void *ctx); void *close_ctx;
    char *tail; size_t tail_len;    /* newest output, for the error line */
    int64_t t_open_us;
    bool bar_dirty;             /* the voice indicator gave the bar back: bar_update repaints it */
    /* voice input (follow-up 2026-10-24): Ctrl+Space dictates into the session as typed input, never sent */
    vr_keys_t vkeys;            /* the repeat and toggle guard, as Sessions */
    char vkey[24];              /* "term:<gen>": the key this screen records under */
    bool voice_mine;            /* a recording this screen started may still post a result */
    bool voice_was_rec;
    voice_phase_t voice_shown;  /* what the bar shows: VOICE_IDLE is the session's own state */
    int voice_sec_shown;
    voice_result_t *vres;       /* PSRAM, from the start to the end of a run; zeroed after use */
    char *held;                 /* PSRAM: a transcript the screen wasn't ready for; zeroed and freed when typed or old */
    int64_t held_ms;
    bool typed_once;            /* a dictation was the last thing typed: the next one joins it with a space (SSH only) */
} term_ctx_t;

static term_ctx_t *s_ctx = NULL;
static uint32_t s_gen;
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
static void hk_put(hk_req_t *r)
{
    if (!r) return;
    xSemaphoreTake(s_hk_lock, portMAX_DELAY);
    int left = --r->refs;
    xSemaphoreGive(s_hk_lock);
    if (left) return;
    vSemaphoreDelete(r->sem);
    free(r);
}
/* Wakes the waiter with an answer; a later answer (the screen went, then the dialog was answered) changes nothing. */
static void hk_answer(hk_req_t *r, int answer)
{
    xSemaphoreTake(s_hk_lock, portMAX_DELAY);
    bool first = r->answer == 0;
    if (first) r->answer = answer;
    xSemaphoreGive(s_hk_lock);
    if (first) xSemaphoreGive(r->sem);
}
static void hk_confirm_cb(bool yes, void *ud) { hk_req_t *r = ud; hk_answer(r, yes ? 1 : 2); hk_put(r); }
static void hk_show(void *arg)
{
    hk_req_t *r = arg;
    if (r->cancelled) { hk_put(r); return; }   /* the screen went before the question showed */
    char txt[320];
    snprintf(txt, sizeof(txt), "%s\n\nSHA256:%s", tr(r->changed ? STR_SSH_HOSTKEY_CHANGED : STR_SSH_HOSTKEY_NEW), r->fp);
    ui_confirm(tr(STR_CONNECT), txt, hk_confirm_cb, r);   /* the UI reference goes with the answer */
}
/* On ssh_task: blocks until the question is answered or the screen goes. After the wait it reads only the request. */
static bool on_hostkey(const char *host, int port, const char *fp, bool changed, void *user)
{
    term_ctx_t *c = user;
    (void)host; (void)port;
    hk_req_t *r = calloc(1, sizeof(*r));
    if (!r) return false;
    r->sem = xSemaphoreCreateBinary();
    if (!r->sem) { free(r); return false; }
    strlcpy(r->fp, fp, sizeof(r->fp)); r->changed = changed;
    r->refs = 3;   /* the waiter, the UI, the screen */
    xSemaphoreTake(c->lock, portMAX_DELAY);
    bool gone = c->closing;
    hk_req_t *old = NULL;
    if (!gone) { old = c->hk_req; c->hk_req = r; }
    xSemaphoreGive(c->lock);
    if (gone) hk_put(r);   /* no screen to hold its reference */
    hk_put(old);
    /* cancelled already (the screen went just now): don't wait on the LVGL lock the closing screen holds */
    if (gone || r->cancelled || !ui_async(hk_show, r)) {   /* never shown: no UI reference, and no wait */
        hk_answer(r, 2);
        hk_put(r);
    }
    xSemaphoreTake(r->sem, portMAX_DELAY);
    bool yes = r->answer == 1;
    hk_put(r);
    return yes;
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
    if (c->voice_shown != VOICE_IDLE) return;   /* the voice indicator keeps the bar up; its end shows it for 3 s */
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
    if (st == c->bar_st && !c->bar_dirty) return;
    c->bar_st = st;
    if (c->voice_shown != VOICE_IDLE) { c->bar_dirty = true; return; }   /* the voice indicator owns the dot and label */
    c->bar_dirty = false;
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
    uint32_t gen = (uint32_t)(uintptr_t)ud;   /* the session it was raised for: a later one is not answered */
    if (yes && s_ctx && !s_ctx->closing && s_ctx->gen == gen) ui_nav_back();
}

/* "← Leave" and Ctrl+]: a take-over closes at once (the session keeps running on the Host); an SSH session
 * asks first. While the confirm is up, keys go to it (ui_keyboard_route), never to the remote side. */
static void leave(term_ctx_t *c)
{
    if (c->cmd_mode) { cmd_close(c, tr(STR_TERM_STILL_RUNNING)); return; }
    char q[160];
    snprintf(q, sizeof(q), tr(STR_TERM_DISCONNECT_Q), c->name);
    ui_confirm_ex(q, NULL, tr(STR_DISCONNECT), false, disconnect_cb, (void *)(uintptr_t)c->gen);
}

/* -------------------------------------------------------- voice input (follow-up 2026-10-24) */
/* Ctrl+Space toggles a recording in a take-over and in an SSH session. While recording, every key but Ctrl+Space, Esc
   (cancel) and the leave keys (which cancel, then leave) is eaten: key clicks stay off the Host and typing doesn't mix
   with the transcript. While transcribing, keys pass. The text is typed into this screen's pty with no Enter
   (cc_voice_type_build_ex), and only when the screen is ready for it (term_voice_blocked); leaving the screen
   cancels the run and drops its result. Nothing is built in internal RAM:
   the indicator is the bar's own dot and state label, and the result and the bytes live in PSRAM. */
/* As Sessions' mono_ms: esp_timer on the device; the preview has none and drives lv_tick itself. */
#ifdef UI_PREVIEW
static int64_t voice_now_ms(void) { return (int64_t)lv_tick_get(); }
#else
static int64_t voice_now_ms(void) { return esp_timer_get_time() / 1000; }
#endif
static bool voice_rec(const voice_status_t *v) { return v->phase == VOICE_STARTING || v->phase == VOICE_RECORDING; }

/* The service's status as this screen acts on it: a run it didn't start (Sessions' transcription, a console
   `voice test`) or one the user cancelled is idle here. A recording that ended without a key (the 30 s limit, a failed
   check) starts the toggle guard, so the stop press meant for it starts nothing (Sessions' review M7). */
static void term_voice_get(term_ctx_t *c, voice_status_t *v)
{
    if (c->voice_mine) voice_status(v);
    else memset(v, 0, sizeof(*v));
    if (!c->voice_mine || v->cancelled || strcmp(v->key, c->vkey)) v->phase = VOICE_IDLE;
    bool rec = voice_rec(v);
    if (c->voice_was_rec && !rec) vr_on_voice_ended(&c->vkeys, voice_now_ms());
    c->voice_was_rec = rec;
}

/* The indicator in the session bar: a red dot and "Recording 0:07 / 0:30" in red (the state slot has no room for the
   Sessions hint, and a toast would be one more internal-RAM allocation while the DMA buffers are up), or
   a muted dot and "Transcribing on the Host…". Red, never amber (amber means needs-you). Only a change writes. */
static void voice_bar(term_ctx_t *c, const voice_status_t *v)
{
    bool rec = voice_rec(v);
    voice_phase_t ph = rec ? VOICE_RECORDING : v->phase;
    int sec = -1;
    if (rec) {
        sec = v->elapsed_ms / 1000;
        sec = sec < 0 ? 0 : sec > VR_MAX_S ? VR_MAX_S : sec;
    }
    if (ph == c->voice_shown && sec == c->voice_sec_shown) return;
    bool was = c->voice_shown != VOICE_IDLE;
    c->voice_shown = ph;
    c->voice_sec_shown = sec;
    if (ph == VOICE_IDLE) {   /* the bar goes back to the session's state, then hides after 3 s */
        c->bar_dirty = true;
        bar_update(c);
        return;
    }
    char t[160];
    if (rec) snprintf(t, sizeof(t), "%s %d:%02d / %d:%02d", tr(STR_VOICE_RECORDING), sec / 60, sec % 60,
                      VR_MAX_S / 60, VR_MAX_S % 60);
    else strlcpy(t, tr(STR_VOICE_TRANSCRIBING), sizeof(t));
    ui_dot_tone(c->bar_dot, rec ? UI_TONE_RED : UI_TONE_MUTED);
    ui_text_tone(c->bar_state, rec ? UI_TONE_RED : UI_TONE_MUTED);
    lv_label_set_text(c->bar_state, t);
    if (!was) bar_show(c, BAR_SHOW_MS);   /* up while voice is active: bar_hide_cb leaves it */
}

static void voice_end_run(term_ctx_t *c)
{
    c->voice_mine = false;
    if (c->vres) { memset(c->vres, 0, sizeof(*c->vres)); heap_caps_free(c->vres); c->vres = NULL; }
}

static void held_wipe(term_ctx_t *c)
{
    if (!c->held) return;
    memset(c->held, 0, VR_TEXT_MAX + 1);
    heap_caps_free(c->held);
    c->held = NULL;
}

/* Why the text can't be typed now, or STR_COUNT when it can (review I1, M3). Fails closed. Any session: the link or the
   session is down, or a modal (the Disconnect confirm, a host-key question) or Ctrl+Tab's terminal UI owns the keys.
   A take-over also needs Claude Code at its empty input line, judged as Sessions' paste gate does
   (cc_screen_input_ready, with the permission-dialog matcher as a second opinion) on the live screen: a selector, a
   dialog, a draft or a screen it doesn't know all say no, because typed letters and digits act there. An SSH shell
   can't be judged; there the user dictates at a prompt (README). */
static str_id_t term_voice_blocked(term_ctx_t *c)
{
    if (c->closing || !c->connected || !c->ssh || c->link_lost || link_down(c) || ui_keyboard_owns_input())
        return STR_VOICE_NOT_READY;
    if (!c->cmd_mode) return STR_COUNT;
    size_t cap = (size_t)vt_rows(c->vt) * ((size_t)vt_cols(c->vt) * 4 + 1) + 1;
    char *t = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!t) return STR_VOICE_NOT_READY;
    vt_screen_text(c->vt, t, cap);
    bool ready = cc_screen_input_ready(t) && !cc_screen_is_permission_prompt(t);
    heap_caps_free(t);
    return ready ? STR_COUNT : STR_VOICE_NOT_AT_PROMPT;
}

/* Types text into the pty when the screen is ready: bracketed always in a take-over (Claude Code takes pastes; a fresh
   vt may not have seen its DECSET 2004), in an SSH session only when the remote turned it on. No Enter. False, with a
   toast, when it isn't ready. */
static bool term_voice_type(term_ctx_t *c, const char *text)
{
    str_id_t why = term_voice_blocked(c);
    if (why != STR_COUNT) { ui_toast_ms(tr(why), VOICE_ERROR_TOAST_MS); return false; }
    char *buf = heap_caps_malloc(VOICE_TYPE_CAP, MALLOC_CAP_SPIRAM);
    if (!buf) { ui_toast_ms(tr(STR_VOICE_NO_MEMORY), VOICE_ERROR_TOAST_MS); return false; }
    /* re-review N1: join only in an SSH shell, right after a dictation with no key since; a take-over types only at an
       empty prompt, where a leading space is wrong */
    size_t n = cc_voice_type_build_ex(text, c->cmd_mode || vt_bracketed_paste(c->vt), !c->cmd_mode && c->typed_once, buf,
                                      VOICE_TYPE_CAP);
    if (n) { ssh_client_send(c->ssh, (const uint8_t *)buf, n); vt_scroll_view_reset(c->vt); c->typed_once = true; }
    ESP_LOGI(TAG, "voice: typed %u bytes into the session", (unsigned)n);   /* a length only, never the text */
    memset(buf, 0, VOICE_TYPE_CAP);
    heap_caps_free(buf);
    return true;
}

/* A held transcript older than VOICE_HOLD_MS is wiped: it never comes back late. */
static void held_expire(term_ctx_t *c)
{
    if (c->held && voice_now_ms() - c->held_ms >= VOICE_HOLD_MS) held_wipe(c);
}

static void term_voice_cancel(term_ctx_t *c)
{
    voice_cancel();
    voice_status_t v;
    term_voice_get(c, &v);
    voice_bar(c, &v);
}

/* The one path for Ctrl+Space, as Sessions' voice_toggle. */
static void term_voice_toggle(term_ctx_t *c)
{
    voice_status_t v;
    term_voice_get(c, &v);
    vr_key_t a = vr_on_voice_key(&c->vkeys, voice_rec(&v), voice_now_ms());
    if (a == VR_KEY_IGNORE) return;   /* a key repeat, or a press too soon after the last start or stop */
    if (v.phase == VOICE_TRANSCRIBING) { ui_toast(tr(STR_VOICE_BUSY)); return; }
    held_expire(c);
    if (c->held && a == VR_KEY_START) {   /* a held transcript: this press types it if the screen is ready now */
        if (term_voice_type(c, c->held)) held_wipe(c);
        return;
    }
    if (a == VR_KEY_STOP) {
        voice_stop();
        term_voice_get(c, &v);
        voice_bar(c, &v);
        return;
    }
    if (!c->connected || !c->ssh) { ui_toast(tr(STR_HOSTS_CONNECTING)); return; }   /* nothing to type into yet */
    if (!c->vres) c->vres = heap_caps_calloc(1, sizeof(*c->vres), MALLOC_CAP_SPIRAM);
    if (!c->vres) { ui_toast_ms(tr(STR_VOICE_NO_MEMORY), VOICE_ERROR_TOAST_MS); return; }
    vr_out_t o = voice_start(c->vkey);
    if (o != VR_OUT_STARTED) {
        str_id_t id = vr_out_text(o);
        if (id != STR_COUNT) ui_toast_ms(tr(id), o == VR_OUT_BUSY ? 3000 : VOICE_ERROR_TOAST_MS);
        if (!c->voice_mine) voice_end_run(c);
        return;
    }
    c->voice_mine = true;
    term_voice_get(c, &v);
    voice_bar(c, &v);
}

/* A result for this screen's run: the text typed into the pty (no Enter, never sent), or the error as a toast. */
static void term_voice_apply(term_ctx_t *c, const voice_result_t *r)
{
    if (r->out == VR_OUT_CANCELLED || r->out == VR_OUT_STARTED) return;
    if (r->out != VR_OUT_TEXT) {
        str_id_t id = vr_out_text(r->out);
        if (id != STR_COUNT) ui_toast_ms(tr(id), VOICE_ERROR_TOAST_MS);
        return;
    }
    if (term_voice_type(c, r->text)) {
        if (r->at_limit) ui_toast(tr(STR_VOICE_AT_LIMIT));
        return;
    }
    /* not ready (it toasted why): held for the next Ctrl+Space, at most VOICE_HOLD_MS (a newer one replaces it) */
    held_wipe(c);
    c->held = heap_caps_calloc(1, VR_TEXT_MAX + 1, MALLOC_CAP_SPIRAM);
    if (!c->held) return;
    strlcpy(c->held, r->text, VR_TEXT_MAX + 1);
    c->held_ms = voice_now_ms();
}

/* From the pump: the indicator, then a waiting result. The service posts before it goes idle, so idle after the take
   means nothing more comes. */
static void term_voice_tick(term_ctx_t *c)
{
    held_expire(c);
    if (!c->voice_mine) return;
    voice_status_t v;
    term_voice_get(c, &v);
    voice_bar(c, &v);
    if (c->vres && voice_take_result_for(c->vkey, c->vres)) {
        term_voice_apply(c, c->vres);
        memset(c->vres, 0, sizeof(*c->vres));
    }
    if (v.phase == VOICE_IDLE) voice_end_run(c);
}

/* Leaving: a run of this screen's is cancelled (recording or transcribing), and a result it posted and nobody took is
   dropped now rather than held 60 s. */
static void term_voice_leave(term_ctx_t *c)
{
    held_wipe(c);
    if (!c->voice_mine) return;
    voice_status_t v;
    voice_status(&v);
    if (v.phase != VOICE_IDLE && !strcmp(v.key, c->vkey)) voice_cancel();
    if (c->vres && voice_take_result_for(c->vkey, c->vres)) memset(c->vres, 0, sizeof(*c->vres));
    voice_end_run(c);
}

/* While recording: Esc cancels, the leave keys cancel and leave, everything else is eaten. While this screen's
   transcription is pending, Esc cancels it and isn't sent (review M1: the habit Sessions teaches; a second Esc, with
   nothing pending, goes to the Host as usual); other keys pass. True when it took ev (or c may be gone: a leave). */
static bool voice_gate(term_ctx_t *c, const key_event_t *ev, hk_term_t hk)
{
    if (!c->voice_mine) return false;
    voice_status_t v;
    term_voice_get(c, &v);
    bool esc = ev->type == KEY_EV_SPECIAL && ev->key == SK_ESC;
    if (v.phase == VOICE_TRANSCRIBING && esc) { term_voice_cancel(c); return true; }
    if (!voice_rec(&v)) return false;
    if (hk == HK_T_LEAVE) { term_voice_cancel(c); leave(c); return true; }
    if (esc) term_voice_cancel(c);
    return true;
}

/* -------------------------------------------------------- key sink */
static void send_key(term_ctx_t *c, const key_event_t *e)
{
    char buf[32];
    size_t n = term_keys_encode(e, c->vt, buf, sizeof(buf));
    if (n && c->ssh) { ssh_client_send(c->ssh, (const uint8_t *)buf, n); vt_scroll_view_reset(c->vt); }
    if (n) c->typed_once = false;   /* a key since the dictation (Enter, an edit): the next one doesn't join */
}

static bool term_key(const key_event_t *ev, void *user)
{
    term_ctx_t *c = user;
    hk_term_t hk = hk_takeover(ev, c->cmd_mode);
    if (hk == HK_T_VOICE) { term_voice_toggle(c); return true; }   /* never sent as NUL (Ctrl+@ still is) */
    if (voice_gate(c, ev, hk)) return true;   /* recording: eaten; c may be gone */
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
        if (vt_resize(c->vt, cols, rows) && c->ssh) ssh_client_resize(c->ssh, cols, rows);   /* no memory: the old size stays */
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
    bar_show(c, BAR_SHOW_MS);   /* in use: keep it up */
    if (voice_gate(c, &k->ev, HK_T_NONE)) return;   /* recording: the Esc chip cancels, the others send nothing */
    send_key(c, &k->ev);
}

static void leave_cb(lv_event_t *e)
{
    (void)e;
    if (!s_ctx || s_ctx->closing) return;
    static const key_event_t none = { .type = KEY_EV_SPECIAL, .key = SK_NONE };
    if (!voice_gate(s_ctx, &none, HK_T_LEAVE)) leave(s_ctx);   /* recording: cancelled, then left */
}

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
    ui_session_bar_name(b, name);
    c->bar_dot = ui_dot(b);
    c->bar_state = ui_session_bar_state(b);
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
            /* a live session the network cut keeps running on the host; one that never connected didn't open */
            if (c->connected) snprintf(line, sizeof(line), "%s %s", msg, tr(STR_TERM_KEEPS_RUNNING));
            else snprintf(line, sizeof(line), "%s %s", tr(STR_SSH_CONN_FAILED), msg);
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
    c->link_lost = c->connected && link_down(c);
    bar_update(c);
    term_voice_tick(c);
}

/* -------------------------------------------------------- lifecycle */
static void scr_delete_cb(lv_event_t *e)
{
    term_ctx_t *c = lv_event_get_user_data(e);
    /* A host-key question still open: answer "no" first, so ssh_task wakes before ssh_client_stop waits for its
       callback (I1). closing is set in the same locked step on_hostkey reads, so a question that comes later sees
       it and doesn't wait. The semaphore is freed by the last reference, never here under a waiter. */
    xSemaphoreTake(c->lock, portMAX_DELAY);
    c->closing = true;
    hk_req_t *r = c->hk_req;
    c->hk_req = NULL;
    xSemaphoreGive(c->lock);
    if (r) { r->cancelled = true; hk_answer(r, 2); hk_put(r); }
    keyboard_clear_sink(c);
    term_voice_leave(c);   /* leaving cancels a recording, and a transcription's result is dropped */
    /* left some other way (e.g. navigation home): still tell the owner */
    if (c->cmd_mode && !c->cmd_done && c->on_close) { c->cmd_done = true; c->on_close(c->close_ctx); }
    if (c->timer) lv_timer_delete(c->timer);
    if (c->bar_timer) lv_timer_delete(c->bar_timer);   /* armed when the screen died */
    /* Waits for a callback in flight on ssh_task (I1); none touches the LVGL lock unbounded, so this can't deadlock. */
    bool idle = !c->ssh || ssh_client_stop(c->ssh);
    c->ssh = NULL;
    ssh_active(c, false);
    /* term view frees itself with the object; vt after (the callbacks never touch either) */
    vt_destroy(c->vt);
    if (s_ctx == c) s_ctx = NULL;
    if (!idle) {   /* a callback is stuck in it: leak the hand-off state rather than free it under the callback */
        ESP_LOGE(TAG, "an SSH callback is still running: its buffers stay allocated");
        return;
    }
    free(c->rx);
    free(c->tail);
    vSemaphoreDelete(c->lock);
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
    int W = lv_display_get_horizontal_resolution(NULL), H = lv_display_get_vertical_resolution(NULL);
    fonts_set_term_line_height(UI_TK_TERM_LINE_H);
    int cols = W / 12, rows = H / UI_TK_TERM_LINE_H;   /* 12 px cells */
    vt_t *vt = vt_create(cols, rows, 1500);
    if (!vt) {   /* out of memory (M1): nothing is set up yet, so leave as a refusal does */
        ESP_LOGW(TAG, "terminal: no memory for a %dx%d screen", cols, rows);
        ui_toast(tr(STR_TERM_NO_MEMORY));
        if (cmd && cmd->on_close) cmd->on_close(cmd->ctx);
        lv_async_call(back_async, NULL);
        return ui_screen_base();
    }
    term_ctx_t *c = calloc(1, sizeof(*c));
    s_ctx = c;
    c->server_idx = idx;
    strlcpy(c->name, cmd && cmd->title[0] ? cmd->title : s->name, sizeof(c->name));
    c->voice_sec_shown = -1;
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
    if (!s_hk_lock) s_hk_lock = xSemaphoreCreateMutex();   /* the UI task, before any ssh_task */
    c->gen = ++s_gen;
    snprintf(c->vkey, sizeof(c->vkey), VR_TERM_KEY_PREFIX "%lu", (unsigned long)c->gen);
    c->rx_cap = 16384; c->rx = malloc(c->rx_cap);
    c->scr = ui_screen_base();   /* bg: UI_TK_TERM_BG */
    lv_obj_add_event_cb(c->scr, scr_delete_cb, LV_EVENT_DELETE, c);

    c->vt = vt;
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
    if (!vt_resize(c->vt, cols, rows)) { cols = vt_cols(c->vt); rows = vt_rows(c->vt); }   /* no memory: the old size */
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
        on_state(SSH_ST_ERROR, tr(STR_TERM_NO_SSH_TASK), c);
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
