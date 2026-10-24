#include "server_store.h"
#include <string.h>
#include <stdlib.h>
#include "cJSON.h"
#include "esp_log.h"
#include "settings/settings.h"   /* the NVS helpers */
#include "crypto/secure_store.h"
#include "storage/sd.h"
#include <strings.h>

static const char *TAG = "servers";
static server_t s_list[SERVER_MAX];
static int s_n = 0;

/* NVS ("srv"/"list") is the only store (Plan 4). Builds before it could keep the whole list on the TF card
 * instead ("credentials on the TF card", creds_on_sd), encrypted with the device key, with the NVS copy
 * erased. The CFG 23 migration imports that file once (server_store_import_sd_once) and leaves it as it is:
 * nothing here writes, moves or deletes a file on the card. */
#define SRV_SD_FILE SD_MOUNT "/.tab5/servers.enc"

/* The list as saved: each password encrypted with the device key. Caller frees. */
static char *export_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "servers");
    for (int i = 0; i < s_n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", s_list[i].name);
        cJSON_AddStringToObject(o, "host", s_list[i].host);
        cJSON_AddNumberToObject(o, "port", s_list[i].port);
        cJSON_AddStringToObject(o, "user", s_list[i].user);
        char *enc = secure_encrypt_str(s_list[i].pass);
        cJSON_AddStringToObject(o, "pass", enc ? enc : "");
        free(enc);
        cJSON_AddStringToObject(o, "key", s_list[i].keypath);
        cJSON_AddBoolToObject(o, "use_key", s_list[i].use_key);
        cJSON_AddItemToArray(arr, o);
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

/* Parses a saved list into out (at most max entries; an entry with no host is dropped). -1 when it isn't
   a list. */
static int parse_list(const char *json, server_t *out, int max)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return -1;
    cJSON *arr = cJSON_GetObjectItem(root, "servers");
    if (!cJSON_IsArray(arr)) { cJSON_Delete(root); return -1; }
    int n = 0;
    cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        if (n >= max) break;
        server_t *s = &out[n];
        memset(s, 0, sizeof(*s));
        const char *v;
        if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(o, "name")))) strlcpy(s->name, v, sizeof(s->name));
        if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(o, "host")))) strlcpy(s->host, v, sizeof(s->host));
        cJSON *p = cJSON_GetObjectItem(o, "port"); s->port = cJSON_IsNumber(p) ? p->valueint : 22;
        if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(o, "user")))) strlcpy(s->user, v, sizeof(s->user));
        if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(o, "pass")))) {
            char *dec = secure_decrypt_str(v);
            if (dec) { strlcpy(s->pass, dec, sizeof(s->pass)); memset(dec, 0, strlen(dec)); free(dec); }
        }
        if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(o, "key")))) strlcpy(s->keypath, v, sizeof(s->keypath));
        s->use_key = cJSON_IsTrue(cJSON_GetObjectItem(o, "use_key"));
        if (!s->host[0]) continue;
        n++;
    }
    cJSON_Delete(root);
    return n;
}

static bool save(void)
{
    char *json = export_json();
    if (!json) return false;
    bool ok = nvs_set_str_ns("srv", "list", json);
    free(json);
    return ok;
}

void server_store_init(void)
{
    char *json = NULL;
    s_n = 0;
    if (nvs_get_str_alloc("srv", "list", &json)) {
        int n = parse_list(json, s_list, SERVER_MAX);
        free(json);
        s_n = n > 0 ? n : 0;
        ESP_LOGI(TAG, "%d servers loaded", s_n);
    }
}

/* The same entry: the same name (names identify entries, as companion_server does), or the same user at the
   same host and port. */
static bool have_entry(const server_t *e)
{
    for (int i = 0; i < s_n; i++) {
        if (e->name[0] && !strcmp(s_list[i].name, e->name)) return true;
        if (!strcasecmp(s_list[i].host, e->host) && s_list[i].port == e->port && !strcmp(s_list[i].user, e->user))
            return true;
    }
    return false;
}

int server_store_import_sd_once(void)
{
    char *blob = NULL;
    size_t len = 0;
    if (!sd_is_mounted() || !fs_read_all(SRV_SD_FILE, &blob, &len, 256 * 1024)) {
        ESP_LOGW(TAG, "the TF card's host list can't be read");
        return -1;
    }
    char *json = secure_decrypt_str(blob);
    free(blob);
    if (!json) { ESP_LOGW(TAG, "the TF card's host list doesn't decrypt with this device's key"); return -1; }
    server_t *in = calloc(SERVER_MAX, sizeof(server_t));   /* about 13 KB: PSRAM, and only for this call */
    int n = in ? parse_list(json, in, SERVER_MAX) : -1;
    memset(json, 0, strlen(json));   /* it holds the passwords in the clear */
    free(json);
    if (n < 0) { free(in); ESP_LOGW(TAG, "the TF card's host list isn't a host list"); return -1; }
    int added = 0, skipped = 0;
    for (int i = 0; i < n; i++) {
        if (s_n >= SERVER_MAX || have_entry(&in[i])) { skipped++; continue; }
        s_list[s_n++] = in[i];
        added++;
    }
    memset(in, 0, SERVER_MAX * sizeof(server_t));
    free(in);
    if (added && !save()) {
        s_n -= added;
        memset(&s_list[s_n], 0, (size_t)added * sizeof(server_t));
        ESP_LOGE(TAG, "the imported hosts could not be saved");
        return -1;
    }
    if (skipped) ESP_LOGI(TAG, "%d hosts on the card were already here (or the list is full)", skipped);
    return added;
}

int server_store_count(void) { return s_n; }
const server_t *server_store_get(int i) { return (i >= 0 && i < s_n) ? &s_list[i] : NULL; }

int server_store_add(const server_t *s)
{
    if (s_n >= SERVER_MAX) return -1;
    int index = s_n++;
    s_list[index] = *s;
    if (!save()) { s_n--; memset(&s_list[index], 0, sizeof(server_t)); return -1; }
    return index;
}
bool server_store_update(int i, const server_t *s)
{
    if (i < 0 || i >= s_n) return false;
    server_t previous = s_list[i];
    s_list[i] = *s;
    if (!save()) { s_list[i] = previous; return false; }
    return true;
}
bool server_store_remove(int i)
{
    if (i < 0 || i >= s_n) return false;
    server_t previous = s_list[i];
    memmove(&s_list[i], &s_list[i + 1], (s_n - i - 1) * sizeof(server_t));
    s_n--;
    if (!save()) {
        memmove(&s_list[i + 1], &s_list[i], (s_n - i) * sizeof(server_t));
        s_list[i] = previous; s_n++; return false;
    }
    return true;
}
void server_store_move(int from, int to)
{
    if (from < 0 || from >= s_n || to < 0 || to >= s_n || from == to) return;
    server_t t = s_list[from];
    if (from < to) memmove(&s_list[from], &s_list[from + 1], (to - from) * sizeof(server_t));
    else memmove(&s_list[to + 1], &s_list[to], (from - to) * sizeof(server_t));
    s_list[to] = t; save();
}
