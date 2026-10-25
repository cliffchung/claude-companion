/* Pure voice-input logic (Plan 5 Task 2): no ESP-IDF or LVGL headers, host-tested. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "i18n/i18n.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VR_RATE          16000
#define VR_MAX_S         30
#define VR_MIN_MS        500
#define VR_BLOCK_MS      100
#define VR_WAV_HDR       44
#define VR_PCM_MAX       (VR_RATE * 2 * VR_MAX_S)          /* 960,000 B */
#define VR_REPEAT_GAP_MS 150
#define VR_TOGGLE_GUARD_MS 700
#define VR_SILENCE_DBFS  (-50)    /* device pass A (O2): kept. At 30 dB the quiet floor is -60..-70 and a noisy room
                                     -46..-55; desk speech from a laptop at low volume -43..-48 (a lower bound for a
                                     voice). A -55 gate let a -55 dBFS silence through to a 3-char hallucination (O5) */
#define VR_METER_FLOOR_DBFS (-60)
#define VR_KEY_MAX       128      /* == SM_KEY_MAX */
#define VR_TEXT_MAX      480      /* == SM_ANSWER_MAX */
/* Voice in a session (follow-up 2026-10-24): a terminal screen records under "term:<its generation>", a key no
   session has (sm_key gives 8 hex digits or "i:<name>:<ms>"), so Sessions never puts that text in a draft. */
#define VR_TERM_KEY_PREFIX "term:"
bool vr_key_is_term(const char *key);

/* 44-byte PCM WAV header; 0 for rate 0, channels 0 or bits not 16. */
size_t vr_wav_header(uint8_t out[VR_WAV_HDR], uint32_t data_bytes, uint32_t rate, uint16_t channels, uint16_t bits);
/* Interleaved -> mono. pick < 0: the average of all channels (int32 sum, saturating); else that channel. Returns frames. */
size_t vr_downmix(const int16_t *in, size_t frames, int channels, int pick, int16_t *out);
int vr_rms_dbfs(const int16_t *pcm, size_t n);   /* rounded; -96 for n == 0 or all zero */
int vr_meter(int dbfs);                          /* VR_METER_FLOOR_DBFS..0 -> 0..100, clamped */
bool vr_heard(int loudest_block_dbfs);           /* loudest >= VR_SILENCE_DBFS */

typedef enum { VR_KEY_START, VR_KEY_STOP, VR_KEY_IGNORE } vr_key_t;
typedef struct { int64_t last_ev_ms, last_toggle_ms; bool seen; } vr_keys_t;   /* zero-init */
vr_key_t vr_on_voice_key(vr_keys_t *k, bool recording, int64_t now_ms);
/* A recording ended without a key: the 30 s limit, a failed check, a cancel. Starts the toggle guard there, so the
   stop press the user meant for it, arriving just after, is ignored instead of starting a new recording. */
void vr_on_voice_ended(vr_keys_t *k, int64_t now_ms);

size_t vr_clean_transcript(char *s);             /* in place, spec 9.3; returns the new length */

typedef enum { VR_MERGE_OK, VR_MERGE_CUT, VR_MERGE_EMPTY } vr_merge_t;
/* out = draft + (space) + text, at most max_bytes bytes plus NUL, cut at the last space (or UTF-8 boundary) that fits. */
vr_merge_t vr_merge_draft(const char *draft, const char *text, char *out, size_t max_bytes);

typedef enum { VR_OUT_TEXT, VR_OUT_STARTED, VR_OUT_BUSY, VR_OUT_NO_FIELD, VR_OUT_NO_LINK, VR_OUT_NO_MEMORY,
               VR_OUT_MIC_FAILED, VR_OUT_TOO_SHORT, VR_OUT_NO_SPEECH, VR_OUT_NO_WHISPER, VR_OUT_NO_MODEL,
               VR_OUT_BAD_MODEL, VR_OUT_CANT_RUN, VR_OUT_AUDIO_BAD, VR_OUT_FAILED, VR_OUT_TIMEOUT,
               VR_OUT_LINK_LOST, VR_OUT_CANCELLED } vr_out_t;
vr_out_t vr_host_result(int exit_status);        /* spec 7.1 table; 0 -> VR_OUT_TEXT */
str_id_t vr_out_text(vr_out_t o);                /* the STR_ id; STR_COUNT for TEXT, STARTED, CANCELLED */

#ifdef __cplusplus
}
#endif
