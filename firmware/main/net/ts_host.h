#pragma once
#include <stdbool.h>
/* What kind of host a server entry names, for the Tailscale switch (Plan 3 Task 1).
   Pure (no ESP-IDF) so the host tests cover it. All take NULL as "not". */
#ifdef __cplusplus
extern "C" {
#endif

/* A dotted-quad IPv4 address, each part 0-255. */
bool ts_host_is_ipv4(const char *s);
/* An IPv4 address in 100.64.0.0/10. */
bool ts_host_tailnet_ip(const char *s);
/* Needs Tailscale for sure: a tailnet address or a *.ts.net name. */
bool ts_host_needs_tailscale(const char *host);
/* Might be on the tailnet: needs_tailscale, or a bare name with no dot (a MagicDNS short name). */
bool ts_host_maybe_tailnet(const char *host);
/* A *.local name (mDNS). */
bool ts_host_is_mdns(const char *host);
/* The Mac's fallback address: "" (none) or a private IPv4 address (10/8, 172.16/12, 192.168/16). */
bool ts_host_lan_fallback_valid(const char *s);

#ifdef __cplusplus
}
#endif
