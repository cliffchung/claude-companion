#include "host_rules.h"
#include "companion/cc_cmd.h"
#include "net/ts_host.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

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

bool hf_first_label(const char *host, char *out, size_t cap)
{
    if (!host || !out || !cap) return false;
    while (*host == ' ' || *host == '\t') host++;
    if (ts_host_is_ipv4(host)) return false;
    const char *dot = strchr(host, '.');
    if (!dot || dot == host || (size_t)(dot - host) >= cap) return false;
    memcpy(out, host, (size_t)(dot - host));
    out[dot - host] = 0;
    return true;
}

void hf_host_addrs_add_name(hf_host_addrs_t *h, const char *host)
{
    hf_host_addrs_add(h, host);
    char label[64];
    if (!h || h->nl >= HF_HOST_LABELS_MAX || !hf_first_label(host, label, sizeof(label))) return;
    for (int i = 0; i < h->nl; i++) if (!strcmp(h->label[i], label)) return;
    strcpy(h->label[h->nl++], label);
}

static bool zero_key(const uint8_t fp[32])
{
    for (int i = 0; i < 32; i++) if (fp[i]) return false;
    return true;
}

bool hf_hostkey_is_host(const uint8_t fp[32], const uint8_t (*list)[32], int n)
{
    if (!fp || !list || zero_key(fp)) return false;
    for (int i = 0; i < n; i++)
        if (!memcmp(fp, list[i], 32)) return true;
    return false;
}

int hf_hostkeys_add(uint8_t (*list)[32], int n, const uint8_t fp[32])
{
    if (!list || !fp || zero_key(fp)) return n;
    if (n < 0) n = 0;
    if (n > HF_HOSTKEYS_MAX) n = HF_HOSTKEYS_MAX;
    for (int i = 0; i < n; i++)
        if (!memcmp(fp, list[i], 32)) {   /* already there: move it to the newest */
            memmove(list[i], list[i + 1], (size_t)(n - i - 1) * 32);
            n--;
            break;
        }
    if (n == HF_HOSTKEYS_MAX) { memmove(list[0], list[1], (size_t)(n - 1) * 32); n--; }   /* FIFO: the oldest goes */
    memcpy(list[n], fp, 32);
    return n + 1;
}

static bool same_ci_n(const char *a, const char *b, size_t n);
/* label matches addr: addr is that bare name (one trailing dot allowed), or a *.ts.net name with that first label */
static bool label_match(const char *addr, const char *label)
{
    while (*addr == ' ' || *addr == '\t') addr++;
    size_t ll = strlen(label), al = strlen(addr);
    if (al && addr[al - 1] == '.') al--;
    if (al == ll) return same_ci_n(addr, label, ll);
    if (al > ll && addr[ll] == '.' && same_ci_n(addr, label, ll) && ts_host_needs_tailscale(addr) && !ts_host_is_ipv4(addr))
        return true;
    return false;
}

bool hf_is_host(const char *addr, const hf_host_addrs_t *h)
{
    if (!h || blank(addr)) return false;
    for (int i = 0; i < h->n; i++)
        if (cc_same_host(addr, h->a[i])) return true;
    for (int i = 0; i < h->nl; i++)
        if (label_match(addr, h->label[i])) return true;
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

static bool same_ci_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y || !x) return false;
    }
    return true;
}

hf_gate_t hf_shell_gate(bool interactive, hf_deny_read_t rd, const uint8_t key[32], const uint8_t (*list)[32], int n)
{
    if (!interactive) return HF_GATE_ALLOW;
    if (rd == HF_DENY_ERROR || !key) return HF_GATE_REFUSE_UNCHECKED;
    if (rd == HF_DENY_FOUND && hf_hostkey_is_host(key, list, n)) return HF_GATE_REFUSE_HOST;
    return HF_GATE_ALLOW;
}

bool hf_hostkeys_may_write(hf_deny_read_t rd) { return rd != HF_DENY_ERROR; }

int hf_conn_steps(bool interactive, hf_step_t *out, int cap)
{
    int need = interactive ? 3 : 2;
    if (!out || cap < need) return 0;
    int n = 0;
    if (interactive) out[n++] = HF_STEP_DENY_CHECK;
    out[n++] = HF_STEP_VERIFY_HOST;
    out[n++] = HF_STEP_AUTH;
    return n;
}

bool hf_hostkeys_pref(const char *key_type, char *out, size_t cap)
{
    if (!key_type || !out || !cap) return false;
    const char *first;
    if (!strcmp(key_type, "ssh-rsa")) first = "rsa-sha2-512,rsa-sha2-256";
    else if (!strcmp(key_type, "ssh-ed25519") || !strcmp(key_type, "ecdsa-sha2-nistp256") ||
             !strcmp(key_type, "ecdsa-sha2-nistp384") || !strcmp(key_type, "ecdsa-sha2-nistp521")) first = key_type;
    else return false;
    int n = snprintf(out, cap, "^%s", first);
    if (n < 0 || (size_t)n >= cap) { out[0] = 0; return false; }
    return true;
}
