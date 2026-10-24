#include "settings_migrate.h"
#include <string.h>

uint32_t settings_blob_load(app_settings_t *s, const void *blob, size_t len)
{
    if (!s || !blob || len < 2 * sizeof(uint32_t)) return 0;
    const cfg_blob_t *b = blob;
    uint32_t magic, version;
    memcpy(&magic, &b->magic, sizeof(magic));
    memcpy(&version, &b->version, sizeof(version));
    if (magic != CFG_MAGIC) return 0;
    size_t n = len - 2 * sizeof(uint32_t);
    if (version == CFG_VERSION && n >= sizeof(app_settings_t)) {
        memcpy(s, &b->s, sizeof(app_settings_t));
    } else {
        /* older layout: copy what fits, keep defaults for the rest */
        if (n > sizeof(app_settings_t)) n = sizeof(app_settings_t);
        memcpy(s, &b->s, n);
        if (version <= 20) {
            /* v21: Tailscale off once (user, 2026-10-23); the appended addresses start empty */
            s->ts_enable = 0;
            memset(s->mac_lan_ip, 0, sizeof(s->mac_lan_ip));
            memset(s->mac_ts_ip, 0, sizeof(s->mac_ts_ip));
        }
    }
    s->mac_lan_ip[sizeof(s->mac_lan_ip) - 1] = 0;
    s->mac_ts_ip[sizeof(s->mac_ts_ip) - 1] = 0;
    return version;
}

cfg_blob_t settings_blob_make(const app_settings_t *s)
{
    cfg_blob_t b = { .magic = CFG_MAGIC, .version = CFG_VERSION, .s = *s };
    return b;
}
