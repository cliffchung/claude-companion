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

hf_err_t hf_validate(const char *address, const char *port, const char *user, bool is_host_entry,
                     const char *host_address, int *port_out)
{
    if (blank(address)) return HF_NO_ADDRESS;
    int p = 0;
    if (!parse_port(port, &p)) return HF_BAD_PORT;
    if (blank(user)) return HF_NO_USER;
    if (!is_host_entry && host_address && host_address[0] && cc_same_host(address, host_address)) return HF_IS_HOST;
    if (port_out) *port_out = p;
    return HF_OK;
}

static bool device_path(const char *keypath) { return !keypath || !keypath[0] || !strcmp(keypath, "device"); }

hf_auth_t hf_auth_of(bool use_key, const char *keypath)
{
    if (!use_key) return HF_AUTH_PASSWORD;
    return device_path(keypath) ? HF_AUTH_DEVICE : HF_AUTH_SD_FILE;
}

bool hf_tailnet_host(const char *host) { return ts_host_needs_tailscale(host); }

hf_key_src_t hf_key_source(const char *keypath, bool file_exists)
{
    if (device_path(keypath)) return HF_KEY_DEVICE;
    return file_exists ? HF_KEY_FILE : HF_KEY_FILE_MISSING;
}

bool hf_key_on_sd(const char *keypath) { return keypath && !strncmp(keypath, "/sdcard/", 8); }
