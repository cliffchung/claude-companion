/* The host form (spec §5.5). One column of 64 px rows: Name, Address, Port, User, Sign in with (Device key or
 * Password; a read-only "Key file on SD card" for an old entry whose key path isn't "device"), Password (only for
 * Password, masked), Host key (a saved host with a pin: the fingerprint in mono and a destructive Forget), and
 * Delete host. Each value row opens a prompt; a change rebuilds the column in place, keeping its scroll.
 *
 * The draft lives in RAM (its password too, as server_store keeps it) and is wiped when the screen goes. The entry
 * is named by the name, host and port it was opened with and resolved again at Save and Delete, so a store that
 * changed under the form never writes another entry. Esc (or ← or a swipe) with unsaved changes asks first. */
#include "ui_host_form.h"
#include "ui.h"
#include "ui_comp.h"
#include "servers/server_store.h"
#include "ssh/ssh_client.h"
#include "companion/cc_link.h"
#include "servers/host_rules.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { F_NAME, F_ADDRESS, F_PORT, F_USER, F_PASSWORD } field_t;

typedef struct {
    hf_origin_t origin;
    bool editing;              /* an existing entry (orig names it) */
    server_t orig;             /* the entry as opened, or the prefilled new one */
    server_t d;                /* the draft */
    char port[12], orig_port[12];
    hf_auth_t auth, orig_auth;
    bool pass_typed;
    bool leaving;              /* Save, Delete or Discard: the back guard lets the back through */
    bool asking;               /* "Discard changes?" is up: a second way out doesn't stack another (M-new-2) */
    lv_obj_t *body;
} form_t;
static form_t *s_f;   /* malloc'd while the form shows (PSRAM-sized, not .bss); NULL otherwise */

static void refill(void);

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    return s;
}

static bool dirty(void)
{
    return strcmp(s_f->d.name, s_f->orig.name) || strcmp(s_f->d.host, s_f->orig.host) || strcmp(s_f->d.user, s_f->orig.user) ||
           strcmp(s_f->port, s_f->orig_port) || s_f->auth != s_f->orig_auth || s_f->pass_typed;
}

/* the entry the form was opened on, now: -1 if it went (or a new host) */
static int entry_index(void)
{
    if (!s_f->editing) return -1;
    int n = server_store_count();
    for (int i = 0; i < n; i++) {
        const server_t *s = server_store_get(i);
        if (s && !strcmp(s->name, s_f->orig.name) && !strcmp(s->host, s_f->orig.host) && s->port == s_f->orig.port) return i;
    }
    return -1;
}

/* the Host's entry: the one Settings › Host names (a new host from the Host picker becomes it) */
static bool host_entry(int idx)
{
    if (s_f->origin != HF_FROM_TERMINAL) return true;
    return idx >= 0 && idx == cc_settings_server();
}

/* ------------------------------------------------------------ prompts */
static void prompt_cb(const char *v, void *ud)
{
    if (!s_f) return;
    if (!v) return;
    char buf[160];
    strlcpy(buf, v, sizeof(buf));
    char *t = trim(buf);
    switch ((field_t)(intptr_t)ud) {
    case F_NAME: strlcpy(s_f->d.name, t, sizeof(s_f->d.name)); break;
    case F_ADDRESS: strlcpy(s_f->d.host, t, sizeof(s_f->d.host)); break;
    case F_PORT: strlcpy(s_f->port, t, sizeof(s_f->port)); break;
    case F_USER: strlcpy(s_f->d.user, t, sizeof(s_f->d.user)); break;
    case F_PASSWORD:
        if (v[0]) {   /* empty keeps the saved one; a password is never trimmed */
            strlcpy(s_f->d.pass, v, sizeof(s_f->d.pass));
            s_f->pass_typed = true;
        }
        break;
    }
    memset(buf, 0, sizeof(buf));
    refill();
}

static void field_cb(lv_event_t *e)
{
    if (!s_f) return;
    field_t f = (field_t)(intptr_t)lv_event_get_user_data(e);
    switch (f) {
    case F_NAME: ui_prompt(tr(STR_HF_NAME), tr(STR_HF_NAME_PH), s_f->d.name, false, prompt_cb, (void *)(intptr_t)f); break;
    case F_ADDRESS: ui_prompt(tr(STR_HF_ADDRESS), tr(STR_HF_ADDRESS_PH), s_f->d.host, false, prompt_cb, (void *)(intptr_t)f); break;
    case F_PORT: ui_prompt(tr(STR_HF_PORT), tr(STR_HF_PORT_PH), s_f->port, false, prompt_cb, (void *)(intptr_t)f); break;
    case F_USER: ui_prompt(tr(STR_HF_USER), "", s_f->d.user, false, prompt_cb, (void *)(intptr_t)f); break;
    case F_PASSWORD: ui_prompt(tr(STR_HF_PASSWORD), "", "", true, prompt_cb, (void *)(intptr_t)f); break;   /* starts empty */
    }
}

static const char *s_auth_items[3];
static hf_auth_t s_auth_of_item[3];
static void auth_pick_cb(int i, void *ud)
{
    if (!s_f) return;
    (void)ud;
    if (i < 0 || i > 2 || !s_auth_items[i]) return;
    s_f->auth = s_auth_of_item[i];
    refill();
}
static void auth_cb(lv_event_t *e)
{
    if (!s_f) return;
    (void)e;
    int n = 0, cur = 0;
    memset(s_auth_items, 0, sizeof(s_auth_items));
    s_auth_items[n] = tr(STR_HF_DEVICE_KEY); s_auth_of_item[n++] = HF_AUTH_DEVICE;
    s_auth_items[n] = tr(STR_HF_PASSWORD); s_auth_of_item[n++] = HF_AUTH_PASSWORD;
    /* an old entry's key file stays a choice until another is saved: read-only, its path isn't edited here */
    if (s_f->orig_auth == HF_AUTH_SD_FILE) { s_auth_items[n] = tr(STR_HF_SD_KEY); s_auth_of_item[n++] = HF_AUTH_SD_FILE; }
    for (int i = 0; i < n; i++) if (s_auth_of_item[i] == s_f->auth) cur = i;
    ui_picker(tr(STR_HF_SIGN_IN), s_auth_items, n, cur, auth_pick_cb, NULL);
}

/* ------------------------------------------------------------ host key, delete */
static void forget_yes(bool yes, void *ud)
{
    if (!s_f) return;
    (void)ud;
    if (!yes) return;
    ssh_known_host_forget(s_f->orig.host, s_f->orig.port);
    ui_toast(tr(STR_HF_KEY_FORGOTTEN));
    refill();
}
static void forget_cb(lv_event_t *e)
{
    if (!s_f) return;
    (void)e;
    ui_confirm_ex(tr(STR_SET_FORGET_Q), tr(STR_SET_FORGET_BODY), tr(STR_SET_FORGET), true, forget_yes, NULL);
}

static void delete_yes(bool yes, void *ud)
{
    if (!s_f) return;
    (void)ud;
    if (!yes) return;
    int idx = entry_index();
    if (idx < 0) { ui_toast(tr(STR_HOSTS_GONE)); return; }
    if (!server_store_remove(idx)) { ui_toast(tr(STR_HF_DELETE_FAILED)); return; }
    s_f->leaving = true;
    ui_nav_back();
}
static void delete_cb(lv_event_t *e)
{
    if (!s_f) return;
    (void)e;
    char q[96];
    snprintf(q, sizeof(q), tr(STR_HF_DELETE_Q), s_f->orig.name);
    ui_confirm_ex(q, tr(STR_HF_DELETE_BODY), tr(STR_DELETE), true, delete_yes, NULL);
}

/* ------------------------------------------------------------ save, discard */
/* I1: the name is another entry's (the Host is chosen by name) */
static bool name_taken(const char *name, int self)
{
    int n = server_store_count();
    if (n > SERVER_MAX) n = SERVER_MAX;
    const char *names[SERVER_MAX];
    for (int i = 0; i < n; i++) { const server_t *t = server_store_get(i); names[i] = t ? t->name : NULL; }
    return hf_name_taken(name, names, n, self);
}

static void save_cb(lv_event_t *e)
{
    if (!s_f) return;
    (void)e;
    if (!dirty()) { s_f->leaving = true; ui_nav_back(); return; }
    int idx = entry_index();
    if (s_f->editing && idx < 0) { ui_toast(tr(STR_HOSTS_GONE)); return; }
    static const str_id_t msg[] = { STR_HF_SAVED, STR_HF_ERR_NO_ADDRESS, STR_HF_ERR_NO_USER, STR_HF_ERR_BAD_PORT,
                                    STR_HF_ERR_IS_HOST, STR_HF_ERR_NAME_TAKEN };
    /* C1: the Host by any address the link knows (its LAN fallback, tailnet IP, verified address, ...) */
    hf_host_addrs_t *h = malloc(sizeof(*h));
    if (!h) { ui_toast(tr(STR_HF_SAVE_FAILED)); return; }
    cc_link_host_addresses(h);
    int port = 0;
    hf_err_t err = hf_validate_addrs(s_f->d.host, s_f->port, s_f->d.user, host_entry(idx), h, &port);
    free(h);
    server_t out = s_f->d;
    out.port = port;
    if (!out.name[0]) snprintf(out.name, sizeof(out.name), "%s@%s", out.user, out.host);   /* §5.5: user@host */
    if (err == HF_OK && name_taken(out.name, idx)) err = HF_NAME_TAKEN;
    if (err != HF_OK) { memset(&out, 0, sizeof(out)); ui_toast(tr(msg[err])); return; }
    if (idx < 0 && server_store_count() >= SERVER_MAX) { memset(&out, 0, sizeof(out)); ui_toast(tr(STR_HF_LIST_FULL)); return; }
    out.use_key = s_f->auth != HF_AUTH_PASSWORD;
    if (s_f->auth == HF_AUTH_DEVICE) strlcpy(out.keypath, "device", sizeof(out.keypath));
    else if (s_f->auth == HF_AUTH_SD_FILE) strlcpy(out.keypath, s_f->orig.keypath, sizeof(out.keypath));
    bool ok = idx >= 0 ? server_store_update(idx, &out) : server_store_add(&out) >= 0;
    if (ok && s_f->origin == HF_FROM_SETTINGS_HOST) {
        /* the Host's own entry: a rename follows it and keeps the pin carry-over (M1); then the link reconfigures
           under the same name (cc_link_set_server with an unchanged name keeps the carry too) */
        if (strcmp(out.name, s_f->orig.name)) cc_link_rename_server(s_f->orig.name, out.name);
        cc_link_set_server(out.name);
    } else if (ok && s_f->origin == HF_FROM_HOST_PICKER) {
        cc_link_set_server(out.name);   /* P4-6: a new Host */
    }
    memset(&out, 0, sizeof(out));
    if (!ok) { ui_toast(tr(STR_HF_SAVE_FAILED)); return; }
    ui_toast(tr(STR_HF_SAVED));
    s_f->leaving = true;
    ui_nav_back();
}

static void discard_yes(bool yes, void *ud)
{
    if (!s_f) return;
    (void)ud;
    s_f->asking = false;
    if (!yes) { ui_nav_cancel_pending(); return; }
    s_f->leaving = true;
    ui_nav_proceed();   /* whatever was leaving: Back, Esc, a swipe, home, a push (M7) */
}
static bool back_guard(void)
{
    if (!s_f) return false;
    if (s_f->leaving || !dirty()) return false;
    if (s_f->asking) return true;   /* already asking: this way out waits for the same answer */
    s_f->asking = true;
    ui_confirm_ex(tr(STR_HF_DISCARD_Q), NULL, tr(STR_HF_DISCARD), true, discard_yes, NULL);
    return true;
}

/* ------------------------------------------------------------ rows */
static lv_obj_t *value_row(const char *label, const char *value, field_t f)
{
    return ui_srow(s_f->body, UI_ROW_VALUE, label, value, field_cb, (void *)(intptr_t)f);
}

static void fill(lv_obj_t *p)
{
    char v[200];
    if (s_f->d.name[0]) value_row(tr(STR_HF_NAME), s_f->d.name, F_NAME);
    else {
        snprintf(v, sizeof(v), "%s@%s", s_f->d.user[0] ? s_f->d.user : tr(STR_HF_USER_PH), s_f->d.host[0] ? s_f->d.host : tr(STR_HF_HOST_PH));
        value_row(tr(STR_HF_NAME), v, F_NAME);   /* the default it saves as */
    }
    value_row(tr(STR_HF_ADDRESS), s_f->d.host[0] ? s_f->d.host : tr(STR_SET_NOT_SET), F_ADDRESS);
    value_row(tr(STR_HF_PORT), s_f->port, F_PORT);
    value_row(tr(STR_HF_USER), s_f->d.user[0] ? s_f->d.user : tr(STR_SET_NOT_SET), F_USER);
    static const str_id_t auth_name[] = { STR_HF_DEVICE_KEY, STR_HF_PASSWORD, STR_HF_SD_KEY };
    ui_srow(p, UI_ROW_VALUE, tr(STR_HF_SIGN_IN), tr(auth_name[s_f->auth]), auth_cb, NULL);
    if (s_f->auth == HF_AUTH_PASSWORD)
        value_row(tr(STR_HF_PASSWORD), s_f->d.pass[0] ? tr(STR_HF_PW_SET) : tr(STR_SET_NOT_SET), F_PASSWORD);

    /* the Host's own pin is Settings › Host's (it goes through the link), so not here */
    char fp[80];
    if (s_f->editing && s_f->origin == HF_FROM_TERMINAL && ssh_known_host_fingerprint(s_f->orig.host, s_f->orig.port, fp, sizeof(fp))) {
        ui_section_header(p, tr(STR_HF_HOST_KEY));
        ui_srow_fingerprint(p, fp, tr(STR_SET_FORGET), forget_cb, NULL);
    }
    if (s_f->editing && s_f->origin == HF_FROM_TERMINAL) {   /* never the Host's entry from Settings */
        lv_obj_t *r = ui_srow(p, UI_ROW_ACTION, tr(STR_HF_DELETE), NULL, delete_cb, NULL);
        ui_text_tone(lv_obj_get_child(r, 0), UI_TONE_RED);
    }
}

static void refill(void)
{
    if (!s_f || !s_f->body) return;
    int32_t y = lv_obj_get_scroll_y(s_f->body);
    lv_obj_clean(s_f->body);
    fill(s_f->body);
    lv_obj_update_layout(s_f->body);
    lv_obj_scroll_to_y(s_f->body, y, LV_ANIM_OFF);
}

static void scr_deleted(lv_event_t *e)
{
    (void)e;
    if (!s_f) return;
    memset(s_f, 0, sizeof(*s_f));   /* the draft's password goes with the form */
    free(s_f);
    s_f = NULL;
}

lv_obj_t *ui_host_form_create(void *arg)
{
    const hf_arg_t *a = arg;
    if (s_f) { memset(s_f, 0, sizeof(*s_f)); free(s_f); }   /* B2 deleted the last form first; never two */
    s_f = calloc(1, sizeof(*s_f));
    if (!s_f) return NULL;   /* nav falls back a level */
    s_f->origin = a ? a->origin : HF_FROM_TERMINAL;
    hf_arg_t *w = (hf_arg_t *)a;   /* the caller's static arg: the form records what it opened */
    const server_t *s = NULL;
    if (w && w->index >= 0) {
        if (w->opened) {   /* a re-create (ui_nav_refresh): the same entry by identity, never a stale index */
            int n = server_store_count();
            for (int i = 0; i < n && !s; i++) {
                const server_t *t = server_store_get(i);
                if (t && !strcmp(t->name, w->opened_name) && !strcmp(t->host, w->opened_host) && t->port == w->opened_port) s = t;
            }
            if (!s) { ui_toast(tr(STR_HOSTS_GONE)); free(s_f); s_f = NULL; return NULL; }
        } else if ((s = server_store_get(w->index)) != NULL) {
            w->opened = true;
            strlcpy(w->opened_name, s->name, sizeof(w->opened_name));
            strlcpy(w->opened_host, s->host, sizeof(w->opened_host));
            w->opened_port = s->port;
        }
    }
    if (s) {
        s_f->editing = true;
        s_f->orig = *s;
    } else {
        s_f->orig.port = 22;
        s_f->orig.use_key = true;
        strlcpy(s_f->orig.keypath, "device", sizeof(s_f->orig.keypath));
        if (a) {
            strlcpy(s_f->orig.name, a->prefill_name, sizeof(s_f->orig.name));
            strlcpy(s_f->orig.host, a->prefill_host, sizeof(s_f->orig.host));
        }
    }
    s_f->d = s_f->orig;
    snprintf(s_f->orig_port, sizeof(s_f->orig_port), "%d", s_f->orig.port > 0 ? s_f->orig.port : 22);
    strlcpy(s_f->port, s_f->orig_port, sizeof(s_f->port));
    s_f->orig_auth = s_f->auth = hf_auth_of(s_f->orig.use_key, s_f->orig.keypath);

    lv_obj_t *scr = ui_screen_base();
    lv_obj_add_event_cb(scr, scr_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_t *h = ui_header(scr, tr(s_f->editing ? STR_HF_TITLE_EDIT : STR_HF_TITLE_ADD),
                            tr(s_f->origin == HF_FROM_TERMINAL ? STR_HF_BACK_TERMINAL : STR_NET_BACK_SETTINGS));
    lv_obj_t *slot = ui_header_slot(h);
    lv_obj_set_flex_align(slot, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_chip_set_primary(ui_chip(slot, tr(STR_HF_SAVE), save_cb, NULL));
    s_f->body = lv_obj_create(scr);
    lv_obj_remove_style_all(s_f->body);
    ui_settings_column(s_f->body);
    lv_obj_set_pos(s_f->body, 0, UI_TK_HEADER_H);
    lv_obj_set_size(s_f->body, LV_PCT(100), lv_display_get_vertical_resolution(NULL) - UI_TK_HEADER_H);
    fill(s_f->body);
    ui_nav_set_back_guard(scr, back_guard);
    return scr;
}
