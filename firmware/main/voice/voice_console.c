/* The `voice` console command (Plan 5 Tasks 5 and 6). Prints levels, lengths, durations and outcomes only: no
   sample and no transcript text reaches the console (rule 3 as Plan 5 extends it). */
#include "sdkconfig.h"
#include "voice/voice_console.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sys/debug_console.h"
#include "companion/cc_link.h"
#include "voice/mic.h"
#include "voice/voice.h"
#include "voice/voice_debug.h"
#include "voice/voice_rules.h"

#define REC_FRAMES  (VR_RATE * VR_BLOCK_MS / 1000)   /* 1,600 frames: one 100 ms block */
#define POLL_MS     100
#define RESULT_WAIT_MS (CC_TRANSCRIBE_TIMEOUT_MS + 20000)

static int usage(void)
{
#if CONFIG_COMPANION_DEBUG_COMMANDS
    printf("usage: voice rec <1-10 s> [gain_db] | voice status | voice test <1-30 s> | voice test-short | "
           "voice test-cancel <s> | voice test-bg <1-30 s>\n");
#else
    printf("usage: voice status\n");
#endif
    return 1;
}

static int cmd_status(void)
{
    voice_status_t st;
    voice_status(&st);
    printf("voice: %s, mic %s, tasks %d, hold %s, result %s\n", voice_phase_name(st.phase),
           mic_is_open() ? "open" : "closed", voice_task_count(), voice_hold_on() ? "on" : "off",
           voice_result_pending() ? "pending" : "none");
    printf("voice: psram free %u B, internal free %u B\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (voice_stack_free() >= 0) printf("voice: last voice task stack free min %d B\n", voice_stack_free());
    if (st.phase != VOICE_IDLE)
        printf("voice: elapsed %d ms, meter %d, at_limit %d, transcribing %d ms, cancelled %d\n", st.elapsed_ms,
               st.meter, st.at_limit, st.transcribing_ms, st.cancelled);
    return 0;
}

#if CONFIG_COMPANION_DEBUG_COMMANDS
/* The recording tests below are test-only (CONFIG_COMPANION_DEBUG_COMMANDS, off by default). */

static bool parse_int(const char *s, int lo, int hi, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || !*s || *end || v < lo || v > hi) return false;
    *out = (int)v;
    return true;
}

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void zero(void *p, size_t n)
{
    memset(p, 0, n);
    __asm__ __volatile__("" : : "r"(p) : "memory");
}

/* voice rec: the capture spike. One reused PSRAM block buffer (stereo block + one mono block), nothing kept. */
static int cmd_rec(int secs, int gain)
{
    voice_status_t st;
    voice_status(&st);
    if (st.phase != VOICE_IDLE || mic_is_open()) { printf("voice rec: busy\n"); return 1; }
    const size_t bytes = REC_FRAMES * 2 * sizeof(int16_t) + REC_FRAMES * sizeof(int16_t);
    int16_t *blk = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!blk) { printf("voice rec: no memory\n"); return 1; }
    int16_t *mono = blk + REC_FRAMES * 2;
    const mic_cfg_t cfg = { VR_RATE, gain };
    esp_err_t e = mic_open(&cfg);
    if (e != ESP_OK) {
        printf("mic: open failed (%s)\n", esp_err_to_name(e));
        zero(blk, bytes);
        heap_caps_free(blk);
        return 1;
    }
    printf("mic: open, %d Hz, gain %d dB\n", VR_RATE, gain);
    int hl = -96, hr = -96, ha = -96;   /* the loudest block in this half second */
    int peak = 0;                        /* the largest |sample| over the run */
    unsigned clipped = 0;
    int ok = 0;
    for (int i = 0; i < secs * 10; i++) {
        int got = mic_read(blk, REC_FRAMES, 500);
        if (got <= 0) { printf("mic: read failed at block %d\n", i); ok = 1; break; }
        for (int k = 0; k < got * 2; k++) {
            int a = blk[k] < 0 ? -(int)blk[k] : blk[k];
            if (a > peak) peak = a;
            if (a >= 32767) clipped++;
        }
        size_t n = vr_downmix(blk, (size_t)got, 2, 0, mono);
        int l = vr_rms_dbfs(mono, n);
        n = vr_downmix(blk, (size_t)got, 2, 1, mono);
        int r = vr_rms_dbfs(mono, n);
        n = vr_downmix(blk, (size_t)got, 2, -1, mono);
        int a = vr_rms_dbfs(mono, n);
        if (l > hl) hl = l;
        if (r > hr) hr = r;
        if (a > ha) ha = a;
        if ((i + 1) % 5 == 0) {
            int t10 = (i + 1);            /* tenths of a second */
            printf("rec t=%d.%d L=%d R=%d avg=%d\n", t10 / 10, t10 % 10, hl, hr, ha);
            hl = hr = ha = -96;
        }
    }
    mic_close();
    zero(blk, bytes);
    heap_caps_free(blk);
    int16_t pk = (int16_t)(peak > 32767 ? 32767 : peak);
    printf("rec peak=%d dBFS, clipped %u samples\n", peak ? vr_rms_dbfs(&pk, 1) : -96, clipped);
    printf("mic: closed\n");
    return ok;
}

/* Waits for the result (or for the voice task to end without one), prints the outcome and its length, zeroes it. */
static int finish(void)
{
    voice_result_t *r = heap_caps_calloc(1, sizeof(*r), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r) { printf("voice test: no memory for the result\n"); return 1; }
    bool got = false;
    int64_t t0 = now_ms();
    while (now_ms() - t0 < RESULT_WAIT_MS) {
        if (voice_take_result(r)) { got = true; break; }
        if (!voice_task_alive() && !voice_result_pending()) break;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    int rc = 0;
    if (got)
        printf("voice test: %s, %u chars%s\n", voice_out_name(r->out), (unsigned)strnlen(r->text, sizeof(r->text)),
               r->at_limit ? ", at limit" : "");
    else { printf("voice test: no result\n"); rc = 1; }
    zero(r, sizeof(*r));
    heap_caps_free(r);
    return rc;
}

static bool start(void)
{
    vr_out_t o = voice_start("console");
    if (o != VR_OUT_STARTED) { printf("voice test: %s\n", voice_out_name(o)); return false; }
    return true;
}

/* Counts recorded time, not wall time (the mic open and its 250 ms settle come first). At VR_MAX_S it never
   stops the recording itself, so the 30 s limit ends it and the result says at limit. */
static int cmd_test(int secs)
{
    if (!start()) return 1;
    int64_t end = now_ms() + (int64_t)secs * 1000 + 5000;   /* a backstop: a stuck mic still ends the test */
    for (;;) {
        voice_status_t st;
        voice_status(&st);
        if (!(st.phase == VOICE_STARTING || st.phase == VOICE_RECORDING) || now_ms() >= end) break;
        if (secs < VR_MAX_S && st.elapsed_ms >= secs * 1000) break;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    voice_stop();
    return finish();
}

/* voice test-bg: the same as voice test on a short-lived task, so the console stays free for `power go radio-off`,
   `power sim idle`, `power` or `voice status` while it records and transcribes (Task 6 steps 3 and 4). */
#define BG_STACK 4096
static volatile bool s_bg;

static void bg_task(void *arg)
{
    (void)cmd_test((int)(intptr_t)arg);
    s_bg = false;
    vTaskDelete(NULL);
}

static int cmd_test_bg(int secs)
{
    if (s_bg) { printf("voice test: busy\n"); return 1; }
    s_bg = true;
    if (xTaskCreate(bg_task, "vtest", BG_STACK, (void *)(intptr_t)secs, 2, NULL) != pdPASS) {
        s_bg = false;
        printf("voice test: no memory\n");
        return 1;
    }
    printf("voice test-bg: started, %d s\n", secs);
    return 0;
}

static int cmd_test_short(void)
{
    if (!start()) return 1;
    vTaskDelay(pdMS_TO_TICKS(300));
    voice_stop();
    return finish();
}

static int cmd_test_cancel(int secs)
{
    if (!start()) return 1;
    int64_t end = now_ms() + (int64_t)secs * 1000;
    while (now_ms() < end && voice_task_alive()) vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    voice_cancel();
    int64_t t0 = now_ms();
    while (voice_task_alive() && now_ms() - t0 < RESULT_WAIT_MS) vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    if (voice_task_alive()) { printf("voice test: the voice task is still running\n"); return 1; }
    voice_result_t *r = heap_caps_calloc(1, sizeof(*r), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int rc = 0;
    if (r && voice_take_result(r)) {
        printf("voice test: a result was left after the cancel (%s)\n", voice_out_name(r->out));
        rc = 1;
    } else printf("voice test: cancelled\n");
    if (r) { zero(r, sizeof(*r)); heap_caps_free(r); }
    return rc;
}

#endif

static int cmd_voice(int argc, char **argv)
{
    if (argc < 2) return usage();
    const char *v = argv[1];
    if (strcmp(v, "status") == 0 && argc == 2) return cmd_status();
#if CONFIG_COMPANION_DEBUG_COMMANDS
    int n = 0, g = MIC_GAIN_DB;
    if (strcmp(v, "rec") == 0) {
        if (argc < 3 || argc > 4 || !parse_int(argv[2], 1, 10, &n)) return usage();
        if (argc == 4 && !parse_int(argv[3], 0, 37, &g)) return usage();
        return cmd_rec(n, g);
    }
    if (strcmp(v, "test") == 0 && argc == 3 && parse_int(argv[2], 1, VR_MAX_S, &n)) return cmd_test(n);
    if (strcmp(v, "test-bg") == 0 && argc == 3 && parse_int(argv[2], 1, VR_MAX_S, &n)) return cmd_test_bg(n);
    if (strcmp(v, "test-short") == 0 && argc == 2) return cmd_test_short();
    if (strcmp(v, "test-cancel") == 0 && argc == 3 && parse_int(argv[2], 0, VR_MAX_S, &n)) return cmd_test_cancel(n);
#endif
    return usage();
}

void voice_console_init(void)
{
    const esp_console_cmd_t cmd = {
        .command = "voice",
#if CONFIG_COMPANION_DEBUG_COMMANDS
        .help = "voice status | debug: rec <1-10 s> [gain_db] | test <1-30 s> | test-short | test-cancel <s> | test-bg <1-30 s> "
                "(levels, lengths and outcomes only)",
#else
        .help = "voice status: the voice input state",
#endif
        .func = cmd_voice,
    };
    debug_console_register(&cmd);
}
