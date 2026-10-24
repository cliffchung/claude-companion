#include "host_rules.h"
#include "companion/cc_cmd.h"
#include "net/ts_host.h"
#include <string.h>

static bool blank(const char *s)
{
    if (!s) return true;
    while (*s == ' ' || *s == '\t') s++;
    return !*s;
}

static bool parse_port(const char *s, int *out)
{
    if (!s || !*s) return false;
    long v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (*p - '0');
        if (v > 65535) return false;
    }
    if (v < 1) return false;
    *out = (int)v;
    return true;
}

void hf_host_addrs_init(hf_host_addrs_t *h) { if (h) memset(h, 0, sizeof(*h)); }

void hf_host_addrs_add(hf_host_addrs_t *h, const char *addr)
{
    if (!h || blank(addr) || h->n >= HF_HOST_ADDRS_MAX) return;
    for (int i = 0; i < h->n; i++)
        if (!strcmp(h->a[i], addr)) return;
    strncpy(h->a[h->n], addr, sizeof(h->a[0]) - 1);
    h->a[h->n][sizeof(h->a[0]) - 1] = 0;
    h->n++;
}

bool hf_is_host(const char *addr, const hf_host_addrs_t *h)
{
    if (!h || blank(addr)) return false;
    for (int i = 0; i < h->n; i++)
        if (cc_same_host(addr, h->a[i])) return true;
    return false;
}

hf_err_t hf_validate_addrs(const char *address, const char *port, const char *user, bool is_host_entry,
                           const hf_host_addrs_t *h, int *port_out)
{
    if (blank(address)) return HF_NO_ADDRESS;
    int p = 0;
    if (!parse_port(port, &p)) return HF_BAD_PORT;
    if (blank(user)) return HF_NO_USER;
    if (!is_host_entry && hf_is_host(address, h)) return HF_IS_HOST;
    if (port_out) *port_out = p;
    return HF_OK;
}

hf_err_t hf_validate(const char *address, const char *port, const char *user, bool is_host_entry,
                     const char *host_address, int *port_out)
{
    hf_host_addrs_t h;
    hf_host_addrs_init(&h);
    hf_host_addrs_add(&h, host_address);
    return hf_validate_addrs(address, port, user, is_host_entry, &h, port_out);
}

static bool same_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return false;
    }
    return *a == *b;
}

bool hf_name_taken(const char *name, const char *const *names, int n, int self)
{
    if (blank(name) || !names) return false;
    for (int i = 0; i < n; i++)
        if (i != self && names[i] && same_ci(name, names[i])) return true;
    return false;
}

static bool device_path(const char *keypath) { return !keypath || !keypath[0] || !strcmp(keypath, "device"); }

hf_auth_t hf_auth_of(bool use_key, const char *keypath)
{
    if (!use_key) return HF_AUTH_PASSWORD;
    return device_path(keypath) ? HF_AUTH_DEVICE : HF_AUTH_SD_FILE;
}

bool hf_tailnet_host(const char *host) { return ts_host_needs_tailscale(host); }

bool hf_key_on_sd(const char *keypath) { return keypath && !strncmp(keypath, "/sdcard/", 8); }

/* a path part that is exactly ".." */
static bool dotdot(const char *p)
{
    for (const char *s = p; (s = strstr(s, "..")) != NULL; s += 2)
        if ((s == p || s[-1] == '/') && (s[2] == 0 || s[2] == '/')) return true;
    return false;
}

hf_key_src_t hf_key_source(const char *keypath, bool file_exists)
{
    if (device_path(keypath)) return HF_KEY_DEVICE;
    if (!hf_key_on_sd(keypath) || dotdot(keypath)) return HF_KEY_BAD_PATH;
    return file_exists ? HF_KEY_FILE : HF_KEY_FILE_MISSING;
}

hf_auth_plan_t hf_auth_plan(bool use_key, const char *keypath, bool file_exists)
{
    hf_auth_plan_t p = { HF_KEY_NONE, !use_key };
    if (use_key) p.key = hf_key_source(keypath, file_exists);
    return p;
}
