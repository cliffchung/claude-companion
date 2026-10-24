/* Terminal: the SSH host list (spec §5.3). The Host's entry is never listed. One column of 72 px host rows (a dot
 * for tailnet hosts while Tailscale is on, the name, user@host, "↵ Connect" on the focused row, and an Edit button),
 * then "+ Add host" and the footnote. A banner offers Turn on while the switch is off and a listed host is on the
 * tailnet.
 *
 * No timer: the list is built when the screen is shown, and the dots and the banner follow Tailscale events
 * (tailscale_set_event_cb, a slot shared with Settings › Network and Peers; B2 keeps one screen alive, and the slot
 * is cleared when this screen is deleted). Rows name their entry by name, host and port, resolved at tap time,
 * so a store that changed under the page never opens another entry. */
#include "ui.h"
#include "ui_comp.h"
#include "ui_host_form.h"
#include "servers/server_store.h"
#include "servers/host_rules.h"
#include "net/tailscale.h"
#include "net/ts_host.h"
#include "net/wifi_mgr.h"
#include "companion/cc_link.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ADD_PEERS_MAX 16

typedef struct { char name[48]; char host[128]; int port; } host_id_t;

static lv_obj_t *s_body, *s_banner;   /* NULL while Terminal isn't shown (a late event or async does nothing) */
static bool s_banner_primary;
static host_id_t *s_ids;              /* the listed entries, in row order */
static lv_obj_t **s_rows;
static int s_n;
static volatile bool s_ts_queued;     /* one ui_async in flight at most, however many events arrive */
static struct { bool set; host_id_t id; char msg[160]; } s_fail;   /* the last connect failure, by entry */
static hf_arg_t s_form_arg;           /* the nav stack holds the pointer */
static ts_peer_t *s_add_peers;        /* the online peers the sheet shows (malloc'd, freed with the screen) */
static const char *s_add_items[ADD_PEERS_MAX];
static int s_add_n;

static void id_of(const server_t *s, host_id_t *id)
{
    memset(id, 0, sizeof(*id));
    if (!s) return;
    strlcpy(id->name, s->name, sizeof(id->name));
    strlcpy(id->host, s->host, sizeof(id->host));
    id->port = s->port;
}

static bool same_id(const host_id_t *a, const server_t *s)
{
    return s && !strcmp(a->name, s->name) && !strcmp(a->host, s->host) && a->port == s->port;
}

static int index_of(const host_id_t *id)
{
    int n = server_store_count();
    for (int i = 0; i < n; i++)
        if (same_id(id, server_store_get(i))) return i;
    return -1;
}

/* -1 no dot (a LAN host, or Tailscale off), 0 offline, 1 online in the peer list */
static int host_dot(const char *host)
{
    if (!hf_tailnet_host(host) || !tailscale_enabled()) return -1;
    char ip[16];
    const char *want = host;
    if (!ts_host_tailnet_ip(host) && tailscale_resolve(host, ip, sizeof(ip))) want = ip;
    int n = tailscale_peer_count();
    ts_peer_t p;
    for (int i = 0; i < n; i++)
        if (tailscale_peer(i, &p) && !strcmp(p.ip, want)) return p.online ? 1 : 0;
    return 0;
}

static void body_place(bool banner)
{
    int32_t y = UI_TK_HEADER_H + (banner ? UI_TK_BANNER_H : 0);
    lv_obj_set_pos(s_body, 0, y);
    lv_obj_set_size(s_body, LV_PCT(100), lv_display_get_vertical_resolution(NULL) - y);
}

static void refresh_ts(void);
static void turn_on_cb(lv_event_t *e)
{
    (void)e;
    tailscale_set_enabled(1);   /* U7's second home for the switch */
    refresh_ts();
}

/* the dots and the banner: on show and on each Tailscale event, in place */
static void refresh_ts(void)
{
    if (!s_body) return;
    bool tailnet = false;
    for (int i = 0; i < s_n; i++) {
        ui_host_row_dot(s_rows[i], host_dot(s_ids[i].host));
        tailnet |= hf_tailnet_host(s_ids[i].host);
    }
    bool show = tailnet && !tailscale_enabled();
    if (show) {
        ui_banner_set(s_banner, UI_BANNER_MUTED, tr(STR_TS_OFF_BANNER), tr(STR_TS_TURN_ON), turn_on_cb, NULL);
        if (!s_banner_primary) { ui_chip_set_primary(lv_obj_get_child(s_banner, 1)); s_banner_primary = true; }
    } else {
        ui_banner_set(s_banner, UI_BANNER_MUTED, NULL, NULL, NULL, NULL);
    }
    body_place(show);
}

static void ts_async(void *arg)
{
    (void)arg;
    s_ts_queued = false;
    refresh_ts();   /* nothing once the screen is gone */
}
static void ts_ev(void *user)
{
    (void)user;
    if (s_ts_queued) return;
    s_ts_queued = true;
    ui_async(ts_async, NULL);
}

/* ------------------------------------------------------------ connect */
static void connect_async(void *arg)
{
    if (!s_body) return;   /* left Terminal before the hand-off */
    ui_nav_push(ui_terminal_create, arg);
}

static void row_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_current_target_obj(e);
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (slot < 0 || slot >= s_n) return;
    const char *hint = tr(STR_HOSTS_CONNECT_HINT);
    int idx = index_of(&s_ids[slot]);
    if (idx < 0) { ui_host_row_note(row, tr(STR_HOSTS_GONE), false, hint); return; }
    s_fail.set = false;
    const server_t *s = server_store_get(idx);
    const char *refused = NULL;
    if (cc_link_is_companion_server(idx)) refused = TERM_COMPANION_REFUSED;   /* never a login shell on the Host */
    else if (!wifi_mgr_is_connected()) refused = tr(STR_HOSTS_NO_WIFI);
    else refused = ts_host_term_refusal(s->host, tailscale_enabled());
    if (refused) { ui_host_row_note(row, refused, false, hint); return; }
    /* the spinner shows until the session screen takes over; a failure comes back to this row */
    ui_host_row_note(row, tr(STR_HOSTS_CONNECTING), true, hint);
    lv_async_call(connect_async, (void *)(intptr_t)idx);
}

static void edit_cb(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (slot < 0 || slot >= s_n) return;
    int idx = index_of(&s_ids[slot]);
    if (idx < 0) { ui_host_row_note(s_rows[slot], tr(STR_HOSTS_GONE), false, tr(STR_HOSTS_CONNECT_HINT)); return; }
    memset(&s_form_arg, 0, sizeof(s_form_arg));
    s_form_arg.index = idx;
    s_form_arg.origin = HF_FROM_TERMINAL;
    ui_nav_push(ui_host_form_create, &s_form_arg);
}

void ui_hosts_note_failure(int idx, const char *msg)
{
    const server_t *s = server_store_get(idx);
    if (!s || !msg) return;
    id_of(s, &s_fail.id);
    strlcpy(s_fail.msg, msg, sizeof(s_fail.msg));
    s_fail.set = true;
}

/* ------------------------------------------------------------ add host */
static void open_form(const char *name, const char *host)
{
    memset(&s_form_arg, 0, sizeof(s_form_arg));
    s_form_arg.index = -1;
    s_form_arg.origin = HF_FROM_TERMINAL;
    if (name) strlcpy(s_form_arg.prefill_name, name, sizeof(s_form_arg.prefill_name));
    if (host) strlcpy(s_form_arg.prefill_host, host, sizeof(s_form_arg.prefill_host));
    ui_nav_push(ui_host_form_create, &s_form_arg);
}

static void peer_pick_cb(int i, void *ud)
{
    (void)ud;
    if (!s_add_peers || i < 0 || i >= s_add_n) return;
    open_form(s_add_peers[i].hostname, s_add_peers[i].ip);
}

static void tailnet_sheet(void)
{
    s_add_n = 0;
    if (!s_add_peers) s_add_peers = malloc(sizeof(ts_peer_t) * ADD_PEERS_MAX);
    if (!s_add_peers) return;
    int n = tailscale_peer_count();
    for (int i = 0; i < n && s_add_n < ADD_PEERS_MAX; i++) {
        ts_peer_t *p = &s_add_peers[s_add_n];
        if (tailscale_peer(i, p) && p->online) { s_add_items[s_add_n] = p->hostname; s_add_n++; }
    }
    if (!s_add_n) { ui_toast(tr(STR_HOSTS_NO_PEERS)); return; }
    ui_action_sheet(tr(STR_HOSTS_FROM_TAILNET), s_add_items, s_add_n, 0, peer_pick_cb, NULL);
}

static void add_sheet_cb(int i, void *ud)
{
    (void)ud;
    bool tailnet = tailscale_enabled();
    if (i < 0) return;
    if (tailnet && i == 0) tailnet_sheet();
    else open_form(NULL, NULL);
}

static void add_cb(lv_event_t *e)
{
    (void)e;
    static const char *items[2];
    int n = 0;
    if (tailscale_enabled()) items[n++] = tr(STR_HOSTS_FROM_TAILNET);   /* only with the switch on */
    items[n++] = tr(STR_HOSTS_MANUAL);
    ui_action_sheet(tr(STR_HOSTS_ADD_TITLE), items, n, 0, add_sheet_cb, NULL);
}

/* ------------------------------------------------------------ the screen */
static void fill(void)
{
    int n = server_store_count();
    s_ids = n > 0 ? calloc((size_t)n, sizeof(*s_ids)) : NULL;
    s_rows = n > 0 ? calloc((size_t)n, sizeof(*s_rows)) : NULL;
    s_n = 0;
    const char *hint = tr(STR_HOSTS_CONNECT_HINT);
    for (int i = 0; s_ids && s_rows && i < n; i++) {
        const server_t *s = server_store_get(i);
        if (!s || cc_link_is_companion_server(i)) continue;   /* the Host is never listed */
        char meta[220];
        if (s->port && s->port != 22) snprintf(meta, sizeof(meta), "%s@%s:%d", s->user, s->host, s->port);
        else snprintf(meta, sizeof(meta), "%s@%s", s->user, s->host);
        id_of(s, &s_ids[s_n]);
        lv_obj_t *r = ui_host_row(s_body, s->name[0] ? s->name : s->host, meta, hint, row_cb, edit_cb, tr(STR_EDIT),
                                  (void *)(intptr_t)s_n);
        if (s_fail.set && same_id(&s_fail.id, s)) ui_host_row_note(r, s_fail.msg, false, hint);
        s_rows[s_n++] = r;
    }
    ui_add_row(s_body, tr(STR_HOSTS_ADD), add_cb, NULL);
    char note[200];
    snprintf(note, sizeof(note), "%s %s", tr(STR_HOSTS_NOT_LISTED), tr(STR_HOSTS_LEAVE_NOTE));
    ui_footnote(s_body, note);
    ui_keyboard_hint(ui_footnote(s_body, tr(STR_SET_KEY_HINT)));   /* §5.13: while the keyboard is in use */
}

static void scr_deleted(lv_event_t *e)
{
    (void)e;
    tailscale_set_event_cb(NULL, NULL);   /* the shared slot (Review Focus 5) */
    s_body = s_banner = NULL;
    free(s_ids);
    free(s_rows);
    free(s_add_peers);
    s_add_peers = NULL;
    s_add_n = 0;
    s_ids = NULL;
    s_rows = NULL;
    s_n = 0;
}

lv_obj_t *ui_hosts_create(void *arg)
{
    (void)arg;
    lv_obj_t *scr = ui_screen_base();
    lv_obj_add_event_cb(scr, scr_deleted, LV_EVENT_DELETE, NULL);
    ui_header(scr, tr(STR_HOSTS_TITLE), tr(STR_SET_BACK_SESSIONS));
    s_banner = ui_banner(scr, UI_TK_HEADER_H);
    s_banner_primary = false;
    s_body = lv_obj_create(scr);
    lv_obj_remove_style_all(s_body);
    ui_settings_column(s_body);
    body_place(false);
    fill();
    s_ts_queued = false;
    tailscale_set_event_cb(ts_ev, NULL);
    refresh_ts();
    return scr;
}
