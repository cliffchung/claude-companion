#include "ts_host.h"
#include <stdint.h>
#include <string.h>
#include <ctype.h>

static bool parse_ipv4(const char *s, uint32_t *out)
{
    if (!s || !*s) return false;
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        if (part && *s++ != '.') return false;
        if (!isdigit((unsigned char)*s)) return false;
        unsigned n = 0;
        int digits = 0;
        while (isdigit((unsigned char)*s)) {
            n = n * 10 + (unsigned)(*s++ - '0');
            if (++digits > 3 || n > 255) return false;
        }
        v = (v << 8) | n;
    }
    if (*s) return false;
    *out = v;
    return true;
}

/* host ends in suffix (case-insensitive, one trailing dot allowed) with at least one label before it */
static bool ends_with(const char *host, const char *suffix)
{
    if (!host) return false;
    size_t n = strlen(host), m = strlen(suffix);
    if (n && host[n - 1] == '.') n--;
    if (n <= m) return false;
    const char *t = host + n - m;
    if (t[-1] == '.') return false;   /* "x..local" or ".local": no label */
    for (size_t i = 0; i < m; i++)
        if (tolower((unsigned char)t[i]) != tolower((unsigned char)suffix[i])) return false;
    return true;
}

bool ts_host_is_ipv4(const char *s)
{
    uint32_t v;
    return parse_ipv4(s, &v);
}

bool ts_host_tailnet_ip(const char *s)
{
    uint32_t v;
    return parse_ipv4(s, &v) && (v & 0xFFC00000u) == 0x64400000u;
}

bool ts_host_needs_tailscale(const char *host)
{
    return ts_host_tailnet_ip(host) || ends_with(host, ".ts.net");
}

bool ts_host_maybe_tailnet(const char *host)
{
    if (!host || !*host) return false;
    if (ts_host_needs_tailscale(host)) return true;
    if (ts_host_is_ipv4(host)) return false;
    return strchr(host, '.') == NULL;
}

bool ts_host_is_mdns(const char *host)
{
    return ends_with(host, ".local");
}

bool ts_host_lan_fallback_valid(const char *s)
{
    if (!s) return false;
    if (!*s) return true;
    uint32_t v;
    if (!parse_ipv4(s, &v)) return false;
    return (v >> 24) == 10 || (v & 0xFFF00000u) == 0xAC100000u || (v & 0xFFFF0000u) == 0xC0A80000u;
}

const char *ts_host_term_refusal(const char *host, bool ts_on)
{
    return !ts_on && ts_host_needs_tailscale(host) ? TS_HOST_OFF_MESSAGE : NULL;
}
