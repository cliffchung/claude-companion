#include "fast_rules.h"
#include <string.h>
#include <stdio.h>
#include "net/ts_host.h"

bool hc_use(const hc_cache_t *c, const char *entry_host, bool ts_on, bool forget_pending)
{
    if (!c || !entry_host || !entry_host[0] || forget_pending) return false;
    if (ts_host_is_ipv4(entry_host)) return false;
    if (strncmp(c->entry, entry_host, sizeof(c->entry)) != 0) return false;
    if (!ts_host_is_ipv4(c->ip)) return false;
    if (ts_host_tailnet_ip(c->ip) && !ts_on) return false;
    return true;
}

bool hc_on_resolved(hc_cache_t *c, const char *entry_host, const char *ip)
{
    if (!c || !entry_host || !entry_host[0] || !ts_host_is_ipv4(ip) || ts_host_is_ipv4(entry_host)) return false;
    if (strlen(entry_host) >= sizeof(c->entry)) return false;
    hc_cache_t n;
    memset(&n, 0, sizeof(n));
    strncpy(n.entry, entry_host, sizeof(n.entry) - 1);
    strncpy(n.ip, ip, sizeof(n.ip) - 1);
    if (memcmp(&n, c, sizeof(n)) == 0) return false;
    *c = n;
    return true;
}

hc_key_t hc_cached_key(kh_result_t kh) { return kh == KH_OK ? HC_ACCEPT : HC_LOOKUP; }

static bool ip_bytes(const char *s, size_t n, uint8_t b[4])
{
    char t[16];
    if (n == 0 || n >= sizeof(t)) return false;
    memcpy(t, s, n);
    t[n] = 0;
    if (!ts_host_is_ipv4(t)) return false;
    int v[4];
    if (sscanf(t, "%d.%d.%d.%d", &v[0], &v[1], &v[2], &v[3]) != 4) return false;
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)v[i];
    return true;
}

static uint32_t be(const uint8_t b[4]) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static uint32_t net(uint32_t h)
{
    uint8_t b[4] = { (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h };
    uint32_t v;
    memcpy(&v, b, 4);
    return v;
}

bool sip_parse(const char *s, sip_t *out)
{
    if (!s || !out) return false;
    const char *tok[4];
    size_t len[4];
    int n = 0;
    for (const char *p = s; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (n == 4) return false;
        tok[n] = p;
        while (*p && *p != ' ') p++;
        len[n] = (size_t)(p - tok[n]);
        n++;
    }
    if (n < 2 || n > 3) return false;
    const char *slash = memchr(tok[0], '/', len[0]);
    if (!slash) return false;
    uint8_t a[4], g[4], d[4];
    if (!ip_bytes(tok[0], (size_t)(slash - tok[0]), a)) return false;
    size_t pl = len[0] - (size_t)(slash - tok[0]) - 1;
    if (pl < 1 || pl > 2) return false;
    int prefix = 0;
    for (size_t i = 0; i < pl; i++) {
        char c = slash[1 + i];
        if (c < '0' || c > '9') return false;
        prefix = prefix * 10 + (c - '0');
    }
    if (prefix < 1 || prefix > 30) return false;
    if (!ip_bytes(tok[1], len[1], g)) return false;
    if (n == 3 && !ip_bytes(tok[2], len[2], d)) return false;
    uint32_t m = 0xffffffffu << (32 - prefix);
    uint32_t ip = be(a), gw = be(g);
    if ((ip & m) != (gw & m) || ip == gw) return false;
    if ((ip & ~m) == 0 || (ip & ~m) == ~m || (gw & ~m) == 0 || (gw & ~m) == ~m) return false;
    out->ip = net(ip);
    out->mask = net(m);
    out->gw = net(gw);
    out->dns = n == 3 ? net(be(d)) : out->gw;
    return true;
}
