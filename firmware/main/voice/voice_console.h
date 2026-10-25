#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* Registers the `voice` console command: rec | status | test | test-short | test-cancel (levels, lengths and
   outcomes only: never audio or transcript text). */
void voice_console_init(void);
#ifdef __cplusplus
}
#endif
