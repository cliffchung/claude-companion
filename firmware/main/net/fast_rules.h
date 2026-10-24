#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ssh/kh_rules.h"
/* Plan 3 Task 3, the fast check-in's pure rules (no ESP-IDF, host-tested): the Host address cache (Step 4) and
   the static address setting (Step 3). The Wi-Fi join cache was measured and dropped (L30). */
#ifdef __cplusplus
extern "C" {
#endif

/* The Host's last resolved address, with the entry host it came from. NVS "cc"/"hcache". */
typedef struct { char entry[128]; char ip[16]; uint8_t has_key, pad[3]; uint8_t key[32]; } hc_cache_t;
/* Connect to the cached address first (skip mDNS/DNS): only for this entry, a name (not a literal), no
   pending forget, and a tailnet address only with the switch on. */
bool hc_use(const hc_cache_t *c, const char *entry_host, bool ts_on, bool forget_pending);
/* After a lookup gave ip for entry_host and the connect there passed its pin check with the server key
   key_hash (SHA256): stores all three. True when it changed (so it is saved). No key: nothing stored. */
bool hc_on_resolved(hc_cache_t *c, const char *entry_host, const char *ip, const uint8_t key_hash[32]);
typedef enum { HC_ACCEPT, HC_LOOKUP } hc_key_t;
/* The cached address's host-key result: only a matching pin keeps it; anything else is dropped without
   acting on it, and the normal lookup and check decide (the cache never pins and never refuses).
   Fix round 1 (review m2): the key met must also be the one cached with the address, so a pinned machine that took
   over the Host's old address is never taken for the Host. */
hc_key_t hc_cached_key(kh_result_t kh, const hc_cache_t *c, const uint8_t met_hash[32]);

/* The optional static address, typed as "address/prefix gateway [dns]" (Settings > Network > Wi-Fi advanced).
   Addresses in network byte order (lwIP's ip4_addr_t.addr). The DNS defaults to the gateway. False for
   anything that wouldn't work: a prefix outside 1..30, the gateway off the subnet or equal to the address,
   the network or broadcast address. */
typedef struct { uint32_t ip, mask, gw, dns; } sip_t;
bool sip_parse(const char *s, sip_t *out);

#ifdef __cplusplus
}
#endif
