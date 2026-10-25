/* The voice service's console-only view (voice_console.c): names and counters, never audio or text. */
#pragma once
#include <stdbool.h>
#include "voice/voice.h"

#ifdef __cplusplus
extern "C" {
#endif

const char *voice_out_name(vr_out_t o);          /* "TEXT", "NO_SPEECH", ... */
const char *voice_phase_name(voice_phase_t p);   /* "idle", "starting", "recording", "transcribing" */
int voice_task_count(void);                      /* the voice task and check helpers alive */
bool voice_task_alive(void);                     /* the voice task itself */
bool voice_hold_on(void);                        /* POWER_HOLD_VOICE as voice.c last set it */
bool voice_result_pending(void);
int voice_stack_free(void);                      /* the last voice task's least free stack (bytes), -1 before one */

#ifdef __cplusplus
}
#endif
