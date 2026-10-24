#pragma once
#include <stdbool.h>
#include "ssh/kh_rules.h"
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

/* The whole host-key decision of the companion link's connect (fix round 1, M10). */
/* The carry source (the key the link last met on the entry's tailnet address, saved with the entry's
   host) against the key the server presents: none (no source, or the entry's host was edited since),
   equal, different, or unreadable. */
typedef enum { KHS_NONE, KHS_EQUAL, KHS_DIFFERENT, KHS_ERROR } khs_t;
/* KHA_NORMAL: act on lan as kh_classify gave it (OK connects; CHANGED, ERROR, PIN_ERROR refuse).
   KHA_PIN_NEW: pin the key as new (first contact on a tailnet address, as in Plan 2, or the first
   connect after the user's Forget Mac host key). KHA_PIN_SILENT: pin it, it equals the carry source.
   KHA_REFUSE_CHANGED: refused as a changed key. KHA_REFUSE_UNKNOWN: refused, nothing to carry (the
   user's Forget is the way to trust it). KHA_PIN_ERROR: the carry source can't be read: refused. */
typedef enum { KHA_NORMAL, KHA_PIN_NEW, KHA_PIN_SILENT, KHA_REFUSE_CHANGED, KHA_REFUSE_UNKNOWN, KHA_PIN_ERROR } kha_t;
/* lan: kh_classify of the address being connected. tailnet_addr: that address is in 100.64.0.0/10.
   forget_pending: the user's forget is waiting for this connect. */
kha_t kh_carry_action(kh_result_t lan, bool tailnet_addr, bool forget_pending, khs_t src);

#ifdef __cplusplus
}
#endif
