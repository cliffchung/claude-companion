#include "cc_sessions.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cc_cmd.h"

static void copy_trunc(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--; /* don't cut a UTF-8 char */
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static const char *jstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)o, key);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;
}

static cc_state_t map_state(const char *s)
{
    if (!s) return CC_IDLE;
    if (!strcmp(s, "blocked") || !strcmp(s, "waiting")) return CC_NEEDS_YOU;
    if (!strcmp(s, "working") || !strcmp(s, "busy")) return CC_WORKING;
    if (!strcmp(s, "done")) return CC_DONE;
    if (!strcmp(s, "failed") || !strcmp(s, "stopped")) return CC_ENDED;
    return CC_IDLE;
}

static void basename_of(const char *cwd, char *out, size_t cap)
{
    size_t n = strlen(cwd);
    while (n > 1 && cwd[n - 1] == '/') n--;
    size_t start = n;
    while (start > 0 && cwd[start - 1] != '/') start--;
    char tmp[256];
    size_t len = n - start;
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, cwd + start, len);
    tmp[len] = 0;
    copy_trunc(out, cap, tmp);
}

static int cmp_sessions(const void *a, const void *b)
{
    const cc_session_t *x = a, *y = b;
    if (x->state != y->state) return (int)x->state - (int)y->state;
    if (x->started_ms != y->started_ms) return x->started_ms < y->started_ms ? 1 : -1;
    return 0;
}

int cc_sessions_parse(const char *json, cc_session_t *out, int cap)
{
    if (!json || !out || cap < 0) return -1;
    cJSON *root = cJSON_Parse(json);
    if (!root) return -1;
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return -1;
    }
    int total = cJSON_GetArraySize(root);
    if (total == 0) {
        cJSON_Delete(root);
        return 0;
    }
    cc_session_t *all = calloc((size_t)total, sizeof(*all));
    if (!all) {
        cJSON_Delete(root);
        return -1;
    }
    int n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, root)
    {
        if (!cJSON_IsObject(e)) continue;
        const char *id = jstr(e, "id");
        const char *sid = jstr(e, "sessionId");
        const char *kind = jstr(e, "kind");
        bool bg = kind && !strcmp(kind, "background");
        /* Background entries are actionable and need valid ids. Others (interactive: 2.1.309
           gives them no "id") are listed for display only, with both ids left empty. */
        if (bg && (!cc_id_valid(id) || !cc_session_id_valid(sid))) continue;
        cc_session_t *s = &all[n++];
        if (bg) {
            copy_trunc(s->id, sizeof(s->id), id);
            copy_trunc(s->session_id, sizeof(s->session_id), sid);
        }
        const char *cwd = jstr(e, "cwd");
        basename_of(cwd ? cwd : "", s->repo, sizeof(s->repo));
        const char *name = jstr(e, "name");
        copy_trunc(s->name, sizeof(s->name), (name && name[0]) ? name : s->repo);
        const char *state = jstr(e, "state");
        s->state = map_state(state ? state : jstr(e, "status"));
        s->background = bg;
        const cJSON *st = cJSON_GetObjectItemCaseSensitive((cJSON *)e, "startedAt");
        s->started_ms = cJSON_IsNumber(st) ? (int64_t)st->valuedouble : 0;
        const cJSON *pid = cJSON_GetObjectItemCaseSensitive((cJSON *)e, "pid");
        s->live = cJSON_IsNumber(pid) && pid->valuedouble > 0;
    }
    qsort(all, (size_t)n, sizeof(*all), cmp_sessions);
    if (n > cap) n = cap;
    memcpy(out, all, (size_t)n * sizeof(*all));
    free(all);
    cJSON_Delete(root);
    return n;
}

static bool finished(const cc_session_t *s)
{
    return s->state == CC_DONE || s->state == CC_ENDED;
}

int cc_sessions_visible(cc_session_t *s, int n, int max_finished)
{
    int w = 0;
    for (int i = 0; i < n; i++) {
        if (finished(&s[i])) {
            int newer = 0; /* finished sessions that outrank this one */
            for (int j = 0; j < n; j++) {
                if (j != i && finished(&s[j]) &&
                    (s[j].started_ms > s[i].started_ms || (s[j].started_ms == s[i].started_ms && j < i))) {
                    newer++;
                }
            }
            if (newer >= max_finished) continue;
        }
        if (w != i) s[w] = s[i];
        w++;
    }
    return w;
}

bool cc_session_needs_you(const cc_session_t *s)
{
    return s->background && s->live && s->state == CC_NEEDS_YOU;
}

size_t cc_summary(const cc_session_t *s, int n, char *out, size_t cap)
{
    int need = 0, work = 0;
    for (int i = 0; i < n; i++) {
        if (cc_session_needs_you(&s[i])) need++;
        else if (s[i].state == CC_WORKING) work++;
    }
    int r;
    if (need && work) r = snprintf(out, cap, "%d %s \xc2\xb7 %d working", need, need == 1 ? "needs you" : "need you", work);
    else if (need) r = snprintf(out, cap, "%d %s", need, need == 1 ? "needs you" : "need you");
    else if (work) r = snprintf(out, cap, "%d working", work);
    else r = snprintf(out, cap, "All quiet");
    if (r < 0) return 0;
    return (size_t)r >= cap ? (cap ? cap - 1 : 0) : (size_t)r;
}

size_t cc_age(int64_t now_ms, int64_t then_ms, char *out, size_t cap)
{
    int64_t s = (now_ms - then_ms) / 1000;
    if (s < 0) s = 0;
    int r;
    if (s < 60) r = snprintf(out, cap, "%llds", (long long)s);
    else if (s < 3600) r = snprintf(out, cap, "%lldm", (long long)(s / 60));
    else if (s < 86400) r = snprintf(out, cap, "%lldh", (long long)(s / 3600));
    else r = snprintf(out, cap, "%lldd", (long long)(s / 86400));
    if (r < 0) return 0;
    return (size_t)r >= cap ? (cap ? cap - 1 : 0) : (size_t)r;
}
