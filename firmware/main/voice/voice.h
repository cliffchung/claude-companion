/* The voice service (Plan 5 Task 6, spec 9): one recording at a time into a PSRAM buffer, the parallel Host check,
   the transcription over the link, and one result slot. Nothing exists until voice_start. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "voice/voice_rules.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { VOICE_IDLE, VOICE_STARTING, VOICE_RECORDING, VOICE_TRANSCRIBING } voice_phase_t;
typedef struct {
    voice_phase_t phase;
    int elapsed_ms;              /* recorded so far */
    int meter;                   /* 0..100, the last block */
    bool at_limit;               /* stopped by the 30 s limit */
    int transcribing_ms;         /* time in TRANSCRIBING */
    bool cancelled;              /* voice_cancel was called and the run hasn't ended yet: no result will come, but
                                    voice_start returns VR_OUT_BUSY until it ends (a transcription waits for the Host) */
    char key[VR_KEY_MAX];        /* the session key it was started for */
} voice_status_t;
typedef struct { uint32_t gen; char key[VR_KEY_MAX]; vr_out_t out; bool at_limit; char text[VR_TEXT_MAX + 1]; } voice_result_t;
/* Any task. VR_OUT_STARTED, or VR_OUT_BUSY (not idle), VR_OUT_NO_LINK (cc_link_connected() false),
   VR_OUT_NO_MEMORY (the 960,044 B PSRAM buffer, the result slot or a task didn't allocate). On STARTED: POWER_HOLD_VOICE on, the
   buffer allocated, the voice task created (4 KB), the check helper created (3 KB). */
vr_out_t voice_start(const char *key);
void voice_stop(void);       /* RECORDING/STARTING: stop and transcribe; otherwise nothing */
void voice_cancel(void);     /* RECORDING/STARTING: stop and wipe, no result; TRANSCRIBING: its result is dropped
                                (also one already posted and not yet taken) */
void voice_status(voice_status_t *out);
/* True once per result (cleared and zeroed after the copy); a result not taken within 60 s is zeroed. */
bool voice_take_result(voice_result_t *out);

#ifdef __cplusplus
}
#endif
