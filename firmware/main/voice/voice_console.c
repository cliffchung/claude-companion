/* The `voice` console command (Plan 5 Task 5; Task 6 adds verbs). Prints levels, lengths, durations and outcomes only: no
   sample and no transcript text reaches the console (rule 3 as Plan 5 extends it). */
#include "voice/voice_console.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sys/debug_console.h"
#include "voice/mic.h"
#include "voice/voice_rules.h"

#define REC_FRAMES  (VR_RATE * VR_BLOCK_MS / 1000)   /* 1,600 frames: one 100 ms block */

static int usage(void)
{
    printf("usage: voice rec <1-10 s> [gain_db]\n");
    return 1;
}

static bool parse_int(const char *s, int lo, int hi, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || !*s || *end || v < lo || v > hi) return false;
    *out = (int)v;
    return true;
}

static void zero(void *p, size_t n)
{
    memset(p, 0, n);
    __asm__ __volatile__("" : : "r"(p) : "memory");
}

/* voice rec: the capture spike. One reused PSRAM block buffer (stereo block + one mono block), nothing kept. */
static int cmd_rec(int secs, int gain)
{
    if (mic_is_open()) { printf("voice rec: busy\n"); return 1; }
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

static int cmd_voice(int argc, char **argv)
{
    if (argc < 2) return usage();
    const char *v = argv[1];
    int n = 0, g = MIC_GAIN_DB;
    if (strcmp(v, "rec") == 0) {
        if (argc < 3 || argc > 4 || !parse_int(argv[2], 1, 10, &n)) return usage();
        if (argc == 4 && !parse_int(argv[3], 0, 37, &g)) return usage();
        return cmd_rec(n, g);
    }
    return usage();
}

void voice_console_init(void)
{
    const esp_console_cmd_t cmd = {
        .command = "voice",
        .help = "voice rec <1-10 s> [gain_db] (levels only)",
        .func = cmd_voice,
    };
    debug_console_register(&cmd);
}
