#include "ui_vitals.h"
#include "ui.h"
#include "sys/sysinfo.h"
#include "sys/vitals.h"
#include "net/wifi_mgr.h"
#include "net/tailscale.h"
#include "settings/settings.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define C_PANEL 0x161b22
#define C_LINE  0x2a323d
#define C_FG    0xe9e7e1
#define C_MUTED 0x8a94a2
#define C_GREEN 0x54d08a
#define C_RED   0xef6f6f
#define HEX(h)  lv_color_hex(h)

#define BAT_W   28
#define BAT_H   14
#define NUB_W   3
#define NUB_H   6
#define BAR_N   4
#define BAR_W   4
#define BAR_GAP 2

typedef struct {
    lv_obj_t *bat, *fill, *pct, *bolt, *bars[BAR_N], *wifi, *ts, *dot, *host, *clock;
} vitals_t;

static const int k_bar_h[BAR_N] = { 5, 9, 13, 17 };
static const lv_point_precise_t k_bolt_pts[] = { { 6, 0 }, { 0, 9 }, { 5, 9 }, { 3, 17 }, { 10, 6 }, { 5, 6 }, { 6, 0 } };

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *t, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, HEX(color), 0);
    return l;
}

static void vitals_delete_cb(lv_event_t *e)
{
    vitals_t *v = lv_event_get_user_data(e);
    lv_free(v);
}

lv_obj_t *ui_vitals_create(lv_obj_t *parent)
{
    vitals_t *v = lv_calloc(1, sizeof(*v));
    lv_obj_t *g = plain(parent);
    lv_obj_set_size(g, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g, 14, 0);
    lv_obj_set_user_data(g, v);
    lv_obj_add_event_cb(g, vitals_delete_cb, LV_EVENT_DELETE, v);

    /* battery: outline, nub, fill, and the percent */
    v->bat = plain(g);
    lv_obj_set_size(v->bat, BAT_W + NUB_W + 2, BAT_H);
    lv_obj_t *body = plain(v->bat);
    lv_obj_set_size(body, BAT_W, BAT_H);
    lv_obj_set_style_border_width(body, 2, 0);
    lv_obj_set_style_border_color(body, HEX(C_MUTED), 0);
    lv_obj_set_style_radius(body, 3, 0);
    lv_obj_t *nub = plain(v->bat);
    lv_obj_set_size(nub, NUB_W, NUB_H);
    lv_obj_set_pos(nub, BAT_W, (BAT_H - NUB_H) / 2);
    lv_obj_set_style_bg_color(nub, HEX(C_MUTED), 0);
    lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
    v->fill = plain(v->bat);
    lv_obj_set_pos(v->fill, 3, 3);
    lv_obj_set_size(v->fill, 1, BAT_H - 6);
    lv_obj_set_style_bg_color(v->fill, HEX(C_FG), 0);
    lv_obj_set_style_bg_opa(v->fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(v->fill, 1, 0);
    v->pct = label(g, "", FONT_SMALL, C_FG);

    /* charging bolt */
    v->bolt = lv_line_create(g);
    lv_line_set_points(v->bolt, k_bolt_pts, sizeof(k_bolt_pts) / sizeof(k_bolt_pts[0]));
    lv_obj_set_style_line_width(v->bolt, 2, 0);
    lv_obj_set_style_line_color(v->bolt, HEX(C_FG), 0);
    lv_obj_set_style_line_rounded(v->bolt, true, 0);
    lv_obj_add_flag(v->bolt, LV_OBJ_FLAG_HIDDEN);

    /* Wi-Fi bars, bottom-aligned */
    v->wifi = plain(g);
    lv_obj_set_size(v->wifi, BAR_N * BAR_W + (BAR_N - 1) * BAR_GAP, 17);
    for (int i = 0; i < BAR_N; i++) {
        v->bars[i] = plain(v->wifi);
        lv_obj_set_size(v->bars[i], BAR_W, k_bar_h[i]);
        lv_obj_set_pos(v->bars[i], i * (BAR_W + BAR_GAP), 17 - k_bar_h[i]);
        lv_obj_set_style_bg_opa(v->bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(v->bars[i], HEX(C_LINE), 0);
        lv_obj_set_style_radius(v->bars[i], 1, 0);
    }

    v->ts = ui_tailscale_icon(g, 4, 2);
    ui_tailscale_icon_set_color(v->ts, HEX(C_MUTED), LV_OPA_COVER);

    /* the link dot and host (the dashboard colours them) */
    lv_obj_t *link = plain(g);
    lv_obj_set_size(link, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(link, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(link, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(link, 8, 0);
    v->dot = plain(link);
    lv_obj_set_size(v->dot, 12, 12);
    lv_obj_set_style_radius(v->dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(v->dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(v->dot, HEX(C_MUTED), 0);
    v->host = label(link, "", FONT_SMALL, C_MUTED);

    v->clock = label(g, "--:--", FONT_BODY, C_FG);
    ui_vitals_update(g);
    return g;
}

lv_obj_t *ui_vitals_link_dot(lv_obj_t *bar) { vitals_t *v = lv_obj_get_user_data(bar); return v ? v->dot : NULL; }
lv_obj_t *ui_vitals_link_host(lv_obj_t *bar) { vitals_t *v = lv_obj_get_user_data(bar); return v ? v->host : NULL; }

static void set_text(lv_obj_t *l, const char *t)
{
    if (strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

void ui_vitals_update(lv_obj_t *bar)
{
    vitals_t *v = bar ? lv_obj_get_user_data(bar) : NULL;
    if (!v) return;
    char b[16];

    /* battery */
    power_info_t pw = sysinfo_power();   /* one copy: percent and source agree */
    int pct = pw.valid ? pw.percent : -1;
    bool charging = pw.valid && vitals_charging((int)lroundf(pw.current_a * 1000.0f));   /* PT23 */
    vitals_bat_level_t lv = vitals_bat_level(pct, charging);
    if (lv == VB_NONE) {
        lv_obj_add_flag(v->bat, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(v->pct, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(v->bat, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(v->pct, LV_OBJ_FLAG_HIDDEN);
        uint32_t col = lv == VB_LOW ? C_RED : C_FG;
        lv_obj_set_style_bg_color(v->fill, HEX(col), 0);
        lv_obj_set_style_text_color(v->pct, HEX(col), 0);
        int w = pct * (BAT_W - 6) / 100;
        lv_obj_set_width(v->fill, w < 1 ? 1 : w);
        snprintf(b, sizeof(b), "%d%%", pct);
        set_text(v->pct, b);
    }
    if (charging) lv_obj_remove_flag(v->bolt, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(v->bolt, LV_OBJ_FLAG_HIDDEN);

    /* Wi-Fi */
    bool up = wifi_mgr_is_connected();
    int bars = vitals_wifi_bars(up, up ? sysinfo_rssi_cached() : 0);
    for (int i = 0; i < BAR_N; i++)
        lv_obj_set_style_bg_color(v->bars[i], HEX(i < bars ? C_FG : C_LINE), 0);

    /* Tailscale */
    ts_state_t ts = tailscale_state();
    if (!g_settings.ts_enable) {
        lv_obj_add_flag(v->ts, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(v->ts, LV_OBJ_FLAG_HIDDEN);
        uint32_t col = ts == TS_CONNECTED ? C_GREEN : ts == TS_ERROR ? C_RED : C_MUTED;
        ui_tailscale_icon_set_color(v->ts, HEX(col), LV_OPA_COVER);
    }

    /* clock */
    time_t t = time(NULL);
    if (t > 1700000000) {
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(b, sizeof(b), "%H:%M", &tm);
    } else {
        strcpy(b, "--:--");
    }
    set_text(v->clock, b);
}
