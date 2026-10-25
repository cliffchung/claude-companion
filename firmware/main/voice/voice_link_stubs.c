/* Temporary weak stand-ins for Task 3's link calls so this branch builds on its own.
   removed at the Plan 5 merge: the controller deletes this file; cc_link.c's strong definitions take over. */
#include "companion/cc_link.h"

/* removed at the Plan 5 merge */
__attribute__((weak)) esp_err_t cc_link_voice_check(int *exit_status)
{
    if (exit_status) *exit_status = -1;
    return ESP_ERR_NOT_SUPPORTED;
}

/* removed at the Plan 5 merge */
__attribute__((weak)) esp_err_t cc_link_transcribe(const void *wav, size_t len, char *out, size_t cap, int *exit_status,
                                                   int *upload_ms, int timeout_ms)
{
    (void)wav; (void)len; (void)timeout_ms;
    if (out && cap) out[0] = 0;
    if (exit_status) *exit_status = -1;
    if (upload_ms) *upload_ms = 0;
    return ESP_ERR_NOT_SUPPORTED;
}
