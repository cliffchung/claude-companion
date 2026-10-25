/* The voice service (Plan 5 Task 6, spec 9.2, 9.4, 9.5; rulings P5-1, P5-3, P5-6).

   voice_start allocates the one PSRAM buffer (P5-1: a 44-byte WAV header slot, then up to 30 s of 16 kHz mono
   samples) and a small PSRAM scratch (one stereo block and the transcript's stdout), turns POWER_HOLD_VOICE on and
   creates two tasks: the check helper (3 KB), which runs cc_link_voice_check once and stores a non-zero exit code,
   and the voice task (4 KB), which records, transcribes and posts one result. Every exit of voice_task passes the
   single wipe_buffer() call, the only free of both allocations, which zeroes them first.

   Privacy (rule 3 as Plan 5 extends it): no sample and no transcript text goes to a log, the console or NVS. The
   one log line per recording holds durations, dBFS, the exit code, a length and the outcome. */
#include "voice/voice.h"
#include "voice/voice_debug.h"
#include "voice/mic.h"
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "companion/cc_link.h"
#include "sys/power.h"

static const char *TAG = "voice";

#define VOICE_BUF_BYTES   (VR_WAV_HDR + VR_PCM_MAX)                 /* 960,044 B */
#define BLOCK_FRAMES      (VR_RATE * VR_BLOCK_MS / 1000)            /* 1,600 */
#define BLOCK_BYTES       (BLOCK_FRAMES * 2 * (int)sizeof(int16_t)) /* one stereo block, 6,400 B */
#define TXT_CAP           2048                                      /* whisper's raw stdout, before cleanup */
#define SCRATCH_BYTES     (BLOCK_BYTES + TXT_CAP)
#define VOICE_STACK       4096
#define CHECK_STACK       3072
#define VOICE_PRIO        5
#define READ_TIMEOUT_MS   500
#define RESULT_TTL_US     (60LL * 1000 * 1000)

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* Guarded by s_mux. */
static voice_phase_t s_phase = VOICE_IDLE;
static bool s_voice_alive;          /* the voice task exists */
static int s_tasks;                 /* the voice task and the check helpers alive */
static bool s_stop_req, s_cancel_req;
static uint32_t s_run;              /* recording generation: a helper from an older run writes nothing */
static int s_check_code;            /* the check's exit code for run s_run; 0 = none (yet) or fine */
static int s_elapsed_ms, s_meter;
static bool s_at_limit;
static int64_t s_trans_start_us;
static bool s_hold;
static int s_stack_free = -1;     /* the last voice task's stack high-water mark (bytes left), -1 before one */

/* The one result slot (P5-3) and the session key, in PSRAM (polish 2: 750 B out of internal .bss). voice_start
   allocates them once, after it has claimed the service, and they are never freed. NULL until then: no run, no result
   and no key. */
typedef struct { voice_result_t result; char key[VR_KEY_MAX]; } voice_slot_t;
static voice_slot_t *s_slot;
static bool s_result_pending;
static uint32_t s_result_gen;
static uint32_t s_result_run;       /* the run that posted the slot's result */
static esp_timer_handle_t s_expire;

/* Owned by the voice task between voice_start and wipe_buffer(). */
static uint8_t *s_buf;
static uint8_t *s_scratch;

static void zero(void *p, size_t n)
{
    if (!p || !n) return;
    memset(p, 0, n);
    __asm__ __volatile__("" : : "r"(p) : "memory");   /* the memset is not a dead store before free() */
}

static int64_t now_us(void) { return esp_timer_get_time(); }

static void set_hold(bool on)
{
    power_hold(POWER_HOLD_VOICE, on);
    taskENTER_CRITICAL(&s_mux);
    s_hold = on;
    taskEXIT_CRITICAL(&s_mux);
}

/* The only free of the recording buffer and the scratch; zeroes both first. Every exit of voice_task passes it. */
static void wipe_buffer(void)
{
    if (s_buf) { zero(s_buf, VOICE_BUF_BYTES); heap_caps_free(s_buf); s_buf = NULL; }
    if (s_scratch) { zero(s_scratch, SCRATCH_BYTES); heap_caps_free(s_scratch); s_scratch = NULL; }
}

/* ---- names ----------------------------------------------------------------- */

const char *voice_out_name(vr_out_t o)
{
    static const char *const k[] = {
        "TEXT", "STARTED", "BUSY", "NO_FIELD", "NO_LINK", "NO_MEMORY", "MIC_FAILED", "TOO_SHORT", "NO_SPEECH",
        "NO_WHISPER", "NO_MODEL", "BAD_MODEL", "CANT_RUN", "AUDIO_BAD", "FAILED", "TIMEOUT", "LINK_LOST", "CANCELLED",
    };
    _Static_assert(sizeof(k) / sizeof(k[0]) == VR_OUT_CANCELLED + 1, "one name per vr_out_t");
    return (unsigned)o < sizeof(k) / sizeof(k[0]) ? k[o] : "?";
}

const char *voice_phase_name(voice_phase_t p)
{
    switch (p) {
    case VOICE_IDLE: return "idle";
    case VOICE_STARTING: return "starting";
    case VOICE_RECORDING: return "recording";
    case VOICE_TRANSCRIBING: return "transcribing";
    default: return "?";
    }
}

/* ---- the result slot ------------------------------------------------------- */

static void expire_cb(void *arg)
{
    (void)arg;
    taskENTER_CRITICAL(&s_mux);
    if (s_result_pending) {
        zero(&s_slot->result, sizeof(s_slot->result));
        s_result_pending = false;
    }
    taskEXIT_CRITICAL(&s_mux);
}

static void post_result(const voice_result_t *r, uint32_t run)
{
    if (!s_expire) {
        const esp_timer_create_args_t a = { .callback = expire_cb, .name = "voice_res" };
        if (esp_timer_create(&a, &s_expire) != ESP_OK) s_expire = NULL;
    }
    if (s_expire) esp_timer_stop(s_expire);   /* ESP_ERR_INVALID_STATE when not running: fine */
    taskENTER_CRITICAL(&s_mux);
    /* final re-review N1: a cancel that came after the task's last look (it finds nothing posted to drop) still wins,
       decided under the same lock voice_cancel takes, so a cancelled transcript never reaches the slot */
    bool cancelled = s_cancel_req && run == s_run;
    bool older = cancelled && s_result_pending;   /* an older run's untaken result: its expiry timer was stopped above */
    if (!cancelled) {
        s_slot->result = *r;
        s_slot->result.gen = ++s_result_gen;
        s_result_run = run;
        s_result_pending = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (s_expire && (!cancelled || older)) esp_timer_start_once(s_expire, RESULT_TTL_US);
}

bool voice_take_result(voice_result_t *out)
{
    if (!out) return false;
    bool got = false;
    taskENTER_CRITICAL(&s_mux);
    if (s_result_pending) {
        *out = s_slot->result;
        zero(&s_slot->result, sizeof(s_slot->result));
        s_result_pending = false;
        got = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (got && s_expire) esp_timer_stop(s_expire);
    return got;
}

bool voice_result_pending(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool p = s_result_pending;
    taskEXIT_CRITICAL(&s_mux);
    return p;
}

/* ---- status ---------------------------------------------------------------- */

void voice_status(voice_status_t *out)
{
    if (!out) return;
    int64_t now = now_us();
    taskENTER_CRITICAL(&s_mux);
    out->phase = s_phase;
    out->elapsed_ms = s_elapsed_ms;
    out->meter = s_meter;
    out->at_limit = s_at_limit;
    out->transcribing_ms = s_phase == VOICE_TRANSCRIBING ? (int)((now - s_trans_start_us) / 1000) : 0;
    out->cancelled = s_phase != VOICE_IDLE && s_cancel_req;
    if (s_slot) memcpy(out->key, s_slot->key, sizeof(out->key));
    else memset(out->key, 0, sizeof(out->key));
    taskEXIT_CRITICAL(&s_mux);
}

int voice_task_count(void)
{
    taskENTER_CRITICAL(&s_mux);
    int n = s_tasks;
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

bool voice_task_alive(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool a = s_voice_alive;
    taskEXIT_CRITICAL(&s_mux);
    return a;
}

int voice_stack_free(void)
{
    taskENTER_CRITICAL(&s_mux);
    int n = s_stack_free;
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

bool voice_hold_on(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool h = s_hold;
    taskEXIT_CRITICAL(&s_mux);
    return h;
}

/* ---- the check helper (P5-6) ----------------------------------------------- */

static void check_task(void *arg)
{
    uint32_t run = (uint32_t)(uintptr_t)arg;
    int st = 0;
    esp_err_t e = cc_link_voice_check(&st);
    taskENTER_CRITICAL(&s_mux);
    /* A link error, a timeout, an exit status the link couldn't read (-1) or a check still pending at stop is
       ignored: the transcribe command runs the same pre-checks. Only a real shell exit (120, 121, ...) stops it. */
    if (e == ESP_OK && st > 0 && run == s_run) s_check_code = st;
    s_tasks--;
    taskEXIT_CRITICAL(&s_mux);
    vTaskDelete(NULL);
}

/* ---- the voice task -------------------------------------------------------- */

/* dst gets src cut to VR_TEXT_MAX bytes at the last space that fits, else at a UTF-8 boundary. */
static size_t fit_text(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n > VR_TEXT_MAX) {
        size_t cut = VR_TEXT_MAX;
        size_t sp = cut;
        while (sp > 0 && src[sp] != ' ') sp--;
        if (sp > 0) cut = sp;
        else while (cut > 0 && ((unsigned char)src[cut] & 0xC0) == 0x80) cut--;
        n = cut;
        while (n > 0 && src[n - 1] == ' ') n--;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
    return n;
}

static vr_out_t map_link(esp_err_t e, int rc)
{
    if (e == ESP_ERR_INVALID_STATE || e == ESP_FAIL) return VR_OUT_LINK_LOST;
    if (e == ESP_ERR_TIMEOUT) return VR_OUT_TIMEOUT;
    if (e != ESP_OK) return VR_OUT_FAILED;   /* ESP_ERR_INVALID_SIZE, ESP_ERR_NO_MEM, ... */
    if (rc != 0) return vr_host_result(rc);
    return VR_OUT_TEXT;
}

static void voice_task(void *arg)
{
    const uint32_t run = (uint32_t)(uintptr_t)arg;
    voice_result_t res;
    memset(&res, 0, sizeof(res));
    taskENTER_CRITICAL(&s_mux);
    memcpy(res.key, s_slot->key, sizeof(res.key));
    taskEXIT_CRITICAL(&s_mux);

    vr_out_t out = VR_OUT_FAILED;
    bool go = false;                 /* record ended in a state worth uploading */
    bool uploaded = false;
    int loudest = -96, rc = -1, upload_ms = -1, host_ms = -1;
    size_t pcm = 0;                  /* sample bytes after the header slot */
    int16_t *blk = (int16_t *)s_scratch;
    char *txt = (char *)s_scratch + BLOCK_BYTES;

    /* ---- record ---- */
    const mic_cfg_t cfg = { VR_RATE, MIC_GAIN_DB };
    bool mic_ok = mic_open(&cfg) == ESP_OK;
    bool mic_failed = !mic_ok, cancelled = false, at_limit = false;
    vr_out_t host_stop = VR_OUT_TEXT;
    if (mic_ok) {
        taskENTER_CRITICAL(&s_mux);
        if (s_phase == VOICE_STARTING) s_phase = VOICE_RECORDING;
        taskEXIT_CRITICAL(&s_mux);
        for (;;) {
            bool stop, cancel;
            int code;
            taskENTER_CRITICAL(&s_mux);
            stop = s_stop_req;
            cancel = s_cancel_req;
            code = s_check_code;
            taskEXIT_CRITICAL(&s_mux);
            if (cancel) { cancelled = true; break; }
            if (code != 0) { host_stop = vr_host_result(code); break; }
            if (stop) break;
            if (pcm >= VR_PCM_MAX) { at_limit = true; break; }
            size_t want = BLOCK_FRAMES;
            if (want > (VR_PCM_MAX - pcm) / 2) want = (VR_PCM_MAX - pcm) / 2;
            int got = mic_read(blk, want, READ_TIMEOUT_MS);
            if (got <= 0) { mic_failed = true; break; }
            int16_t *mono = (int16_t *)(s_buf + VR_WAV_HDR + pcm);
            size_t n = vr_downmix(blk, (size_t)got, 2, MIC_PICK, mono);
            int db = vr_rms_dbfs(mono, n);
            if (db > loudest) loudest = db;
            pcm += n * sizeof(int16_t);
            taskENTER_CRITICAL(&s_mux);
            s_elapsed_ms = (int)(pcm * 1000 / (VR_RATE * sizeof(int16_t)));
            s_meter = vr_meter(db);
            taskEXIT_CRITICAL(&s_mux);
        }
        zero(blk, BLOCK_BYTES);
    }
    mic_close();
    const int rec_ms = (int)(pcm * 1000 / (VR_RATE * sizeof(int16_t)));
    taskENTER_CRITICAL(&s_mux);
    s_at_limit = at_limit;
    if (s_cancel_req) cancelled = true;
    taskEXIT_CRITICAL(&s_mux);
    res.at_limit = at_limit;

    if (cancelled) out = VR_OUT_CANCELLED;
    else if (mic_failed) out = VR_OUT_MIC_FAILED;
    else if (host_stop != VR_OUT_TEXT) out = host_stop;
    else if (rec_ms < VR_MIN_MS) out = VR_OUT_TOO_SHORT;
    else if (!vr_heard(loudest)) out = VR_OUT_NO_SPEECH;
    else go = true;

    /* ---- transcribe ---- */
    if (go) {
        taskENTER_CRITICAL(&s_mux);
        s_phase = VOICE_TRANSCRIBING;
        s_trans_start_us = now_us();
        taskEXIT_CRITICAL(&s_mux);
        vr_wav_header(s_buf, (uint32_t)pcm, VR_RATE, 1, 16);
        txt[0] = 0;
        int64_t t0 = now_us();
        esp_err_t e = cc_link_transcribe(s_buf, VR_WAV_HDR + pcm, txt, TXT_CAP, &rc, &upload_ms,
                                         CC_TRANSCRIBE_TIMEOUT_MS);
        int total_ms = (int)((now_us() - t0) / 1000);
        uploaded = true;
        if (upload_ms < 0 || upload_ms > total_ms) upload_ms = total_ms;
        host_ms = total_ms - upload_ms;
        if (e != ESP_OK) rc = -1;
        out = map_link(e, rc);
        if (out == VR_OUT_TEXT) {
            txt[TXT_CAP - 1] = 0;
            vr_clean_transcript(txt);
            if (fit_text(res.text, txt) == 0) out = VR_OUT_NO_SPEECH;
        }
    }

    /* ---- the one exit: every path above lands here ---- */
    wipe_buffer();
    /* Only now: cc_link_transcribe held link-busy until it returned, so the screen never had a moment with neither
       hold (at a 30 s auto-stop the user is already past the dim timeout). The result lands now: it restarts the idle
       clock first, so the ladder doesn't dim the screen as the text arrives and swallow the next key as a wake (a
       screen that isn't Active is left alone). */
    power_note_activity();
    set_hold(false);

    taskENTER_CRITICAL(&s_mux);
    if (s_cancel_req) out = VR_OUT_CANCELLED;   /* a cancel while transcribing drops the result */
    taskEXIT_CRITICAL(&s_mux);
    res.out = out;
    size_t len = out == VR_OUT_TEXT ? strlen(res.text) : 0;
    if (out != VR_OUT_TEXT) zero(res.text, sizeof(res.text));

    if (uploaded)
        ESP_LOGI(TAG, "rec %d.%d s, loudest %d dBFS, upload %d.%02d s, host %d.%02d s, rc %d, %u chars, %s",
                 rec_ms / 1000, (rec_ms % 1000) / 100, loudest, upload_ms / 1000, (upload_ms % 1000) / 10,
                 host_ms / 1000, (host_ms % 1000) / 10, rc, (unsigned)len, voice_out_name(out));
    else
        ESP_LOGI(TAG, "rec %d.%d s, loudest %d dBFS, no upload, %u chars, %s", rec_ms / 1000, (rec_ms % 1000) / 100,
                 loudest, (unsigned)len, voice_out_name(out));

    if (out != VR_OUT_CANCELLED) post_result(&res, run);
    zero(&res, sizeof(res));

    int stack_free = (int)uxTaskGetStackHighWaterMark(NULL);
    taskENTER_CRITICAL(&s_mux);
    s_stack_free = stack_free;
    if (run == s_run) {
        s_phase = VOICE_IDLE;
        s_stop_req = s_cancel_req = false;
        s_meter = 0;
    }
    s_voice_alive = false;
    s_tasks--;
    taskEXIT_CRITICAL(&s_mux);
    vTaskDelete(NULL);
}

/* ---- the API ----------------------------------------------------------------- */

vr_out_t voice_start(const char *key)
{
    /* Claim the service first (STARTING), so two callers can't both pass the idle check. */
    taskENTER_CRITICAL(&s_mux);
    bool busy = s_phase != VOICE_IDLE || s_voice_alive;
    if (!busy) s_phase = VOICE_STARTING;
    taskEXIT_CRITICAL(&s_mux);
    if (busy) return VR_OUT_BUSY;
    if (!s_slot) {
        voice_slot_t *slot = heap_caps_calloc(1, sizeof(*slot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        taskENTER_CRITICAL(&s_mux);
        s_slot = slot;
        taskEXIT_CRITICAL(&s_mux);
    }
    vr_out_t refuse = VR_OUT_STARTED;
    if (mic_is_open()) refuse = VR_OUT_BUSY;              /* `voice rec` has the mic */
    else if (!cc_link_connected()) refuse = VR_OUT_NO_LINK;
    else if (!s_slot) {
        ESP_LOGW(TAG, "start: no memory for the result slot");
        refuse = VR_OUT_NO_MEMORY;
    } else {
        s_buf = heap_caps_malloc(VOICE_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_scratch = heap_caps_calloc(1, SCRATCH_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_buf || !s_scratch) {
            wipe_buffer();
            ESP_LOGW(TAG, "start: no memory for the buffer");
            refuse = VR_OUT_NO_MEMORY;
        }
    }
    if (refuse != VR_OUT_STARTED) {
        taskENTER_CRITICAL(&s_mux);
        s_phase = VOICE_IDLE;
        taskEXIT_CRITICAL(&s_mux);
        return refuse;
    }

    uint32_t run;
    taskENTER_CRITICAL(&s_mux);
    run = ++s_run;
    s_stop_req = s_cancel_req = false;
    s_check_code = 0;
    s_elapsed_ms = 0;
    s_meter = 0;
    s_at_limit = false;
    s_trans_start_us = 0;
    memset(s_slot->key, 0, sizeof(s_slot->key));
    if (key) strlcpy(s_slot->key, key, sizeof(s_slot->key));
    s_tasks++;                   /* the helper, counted before it can end */
    taskEXIT_CRITICAL(&s_mux);
    set_hold(true);

    if (xTaskCreate(check_task, "voice_chk", CHECK_STACK, (void *)(uintptr_t)run, VOICE_PRIO, NULL) != pdPASS) {
        taskENTER_CRITICAL(&s_mux);
        s_tasks--;
        s_phase = VOICE_IDLE;
        taskEXIT_CRITICAL(&s_mux);
        set_hold(false);
        wipe_buffer();
        ESP_LOGW(TAG, "start: no memory for the check task");
        return VR_OUT_NO_MEMORY;
    }
    taskENTER_CRITICAL(&s_mux);
    s_tasks++;
    s_voice_alive = true;
    taskEXIT_CRITICAL(&s_mux);
    if (xTaskCreate(voice_task, "voice", VOICE_STACK, (void *)(uintptr_t)run, VOICE_PRIO, NULL) != pdPASS) {
        taskENTER_CRITICAL(&s_mux);
        s_tasks--;
        s_voice_alive = false;
        s_run++;                 /* the helper, still running, now writes nothing */
        s_phase = VOICE_IDLE;
        taskEXIT_CRITICAL(&s_mux);
        set_hold(false);
        wipe_buffer();
        ESP_LOGW(TAG, "start: no memory for the voice task");
        return VR_OUT_NO_MEMORY;
    }
    return VR_OUT_STARTED;
}

void voice_stop(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (s_phase == VOICE_STARTING || s_phase == VOICE_RECORDING) s_stop_req = true;
    taskEXIT_CRITICAL(&s_mux);
}

void voice_cancel(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (s_phase != VOICE_IDLE) {
        s_cancel_req = true;
        /* The task posts just before it goes idle: a result this run already posted is dropped too. */
        if (s_result_pending && s_result_run == s_run) {
            zero(&s_slot->result, sizeof(s_slot->result));
            s_result_pending = false;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
}
