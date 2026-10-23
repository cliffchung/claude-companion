#pragma once
#include <stdbool.h>
#include <stddef.h>
/* Host-key pin decisions, pure (no ESP-IDF, no libssh) so the host tests cover them. */
#ifdef __cplusplus
extern "C" {
#endif

/* KH_ERROR: the server's key couldn't be read. KH_PIN_ERROR: the pin couldn't be read from
   NVS (any error but "not found"); never treated as unknown, so it is never re-pinned. */
typedef enum { KH_OK, KH_UNKNOWN, KH_CHANGED, KH_ERROR, KH_PIN_ERROR } kh_result_t;
/* What reading the pin gave. */
typedef enum { KH_PIN_FOUND, KH_PIN_NONE, KH_PIN_READ_FAILED } kh_pin_t;

/* Compares the server key's hash with the stored pin. */
kh_result_t kh_classify(kh_pin_t pin, const void *stored, size_t slen, const void *hash, size_t hlen);
/* Whether the terminal connects: OK yes; ERROR and PIN_ERROR never; UNKNOWN and CHANGED only
   when a host-key callback was there to ask and the user said yes (no callback: reject). */
bool kh_accept(kh_result_t r, bool have_cb, bool cb_answer);
/* Whether a result needs the user's answer (UNKNOWN or CHANGED). */
bool kh_needs_answer(kh_result_t r);

#ifdef __cplusplus
}
#endif
