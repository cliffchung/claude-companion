#pragma once
#include <stdbool.h>
/* Carrying the Mac's host-key pin from its tailnet address to its LAN address (Plan 3 Task 1).
   Pure (no ESP-IDF, no libssh) so the host tests cover it. */
#ifdef __cplusplus
extern "C" {
#endif

/* KHC_PIN_SILENT: pin the LAN address without asking (its key equals the same entry's tailnet pin).
   KHC_UNKNOWN: no pin to carry; the link never trusts it on first use.
   KHC_REFUSE: the key differs from the tailnet pin; refused as a changed key (KH_CHANGED).
   KHC_NORMAL: the LAN address has its own pin; the caller uses the normal check (kh_classify). */
typedef enum { KHC_PIN_SILENT, KHC_UNKNOWN, KHC_REFUSE, KHC_NORMAL } khc_t;

/* lan_pinned: the LAN address has a pin. tailnet_pinned: the entry's tailnet address has one.
   keys_equal: the server's key equals the tailnet pin (ignored without one). */
khc_t kh_carry_decide(bool lan_pinned, bool tailnet_pinned, bool keys_equal);

#ifdef __cplusplus
}
#endif
