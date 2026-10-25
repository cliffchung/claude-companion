#include "checks.h"
#include "ui/ui.h"
#include "ui/ui_comp.h"
#include "ui/ui_sessions.h"
#include "src/core/lv_obj_private.h"     /* ext_click_pad: LVGL has no public getter */
#include "src/misc/lv_timer_private.h"   /* period: LVGL has no public getter */
#include "src/misc/lv_event_private.h"   /* the event filter */
#include "src/core/lv_obj_draw_private.h" /* lv_obj_get_ext_draw_size */
#include "src/misc/lv_area_private.h"      /* lv_area_intersect */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PARTS 64
#define MAX_CTRLS 512

static void say(char *why, size_t cap, const char *fmt, ...)
{
    if (!why || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, cap, fmt, ap);
    va_end(ap);
}

static bool shown(lv_obj_t *o)
{
    for (; o; o = lv_obj_get_parent(o))
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    return true;
}

/* ------------------------------------------------------------- amber */
static void lab(double r, double g, double b, double out[3])
{
    double c[3] = { r / 255.0, g / 255.0, b / 255.0 };
    for (int i = 0; i < 3; i++) c[i] = c[i] <= 0.04045 ? c[i] / 12.92 : pow((c[i] + 0.055) / 1.055, 2.4);
    double x = (0.4124 * c[0] + 0.3576 * c[1] + 0.1805 * c[2]) / 0.95047;
    double y = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
    double z = (0.0193 * c[0] + 0.1192 * c[1] + 0.9505 * c[2]) / 1.08883;
    double f[3] = { x, y, z };
    for (int i = 0; i < 3; i++) f[i] = f[i] > 0.008856 ? cbrt(f[i]) : 7.787 * f[i] + 16.0 / 116.0;
    out[0] = 116 * f[1] - 16;
    out[1] = 500 * (f[0] - f[1]);
    out[2] = 200 * (f[1] - f[2]);
}

static int collect_parts(lv_obj_t *o, lv_area_t *parts, int n)
{
    if (!shown(o)) return n;
    if (lv_obj_has_flag(o, UI_NEEDS_YOU_PART) && n < MAX_PARTS) lv_obj_get_coords(o, &parts[n++]);
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) n = collect_parts(lv_obj_get_child(o, i), parts, n);
    return n;
}

static bool is_amber(const uint8_t *p, const double target[3])
{
    double l[3];
    lab(p[2], p[1], p[0], l);   /* RGB888 snapshots store B, G, R */
    double d0 = l[0] - target[0], d1 = l[1] - target[1], d2 = l[2] - target[2];
    return d0 * d0 + d1 * d1 + d2 * d2 <= 100.0;
}

static int amber_count(lv_obj_t *scr, const lv_draw_buf_t *snap, bool outside_only)
{
    lv_area_t parts[MAX_PARTS];
    int n = collect_parts(scr, parts, 0);
    n = collect_parts(lv_layer_top(), parts, n);
    uint32_t hex = ui_tk_needs_you_hex();
    double target[3];
    lab((hex >> 16) & 255, (hex >> 8) & 255, hex & 255, target);
    int w = (int)snap->header.w, h = (int)snap->header.h, count = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = snap->data + (size_t)y * snap->header.stride;
        for (int x = 0; x < w; x++) {
            if (!is_amber(row + x * 3, target)) continue;
            bool inside = false;
            for (int i = 0; i < n && !inside; i++)
                inside = x >= parts[i].x1 && x <= parts[i].x2 && y >= parts[i].y1 && y <= parts[i].y2;
            if (!outside_only || !inside) count++;
        }
    }
    return count;
}

int pv_amber_outside(lv_obj_t *scr, const lv_draw_buf_t *snap) { return amber_count(scr, snap, true); }

/* ----------------------------------------------------- touch targets */
static bool has_handler(lv_obj_t *o, lv_event_code_t code)
{
    for (uint32_t i = 0; i < lv_obj_get_event_count(o); i++)
        if (lv_obj_get_event_dsc(o, i)->filter == (uint32_t)code) return true;
    return false;
}

/* The same notion of a control as ui_keyboard_route's walk. */
static bool is_control(lv_obj_t *o)
{
    return lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) &&
           (has_handler(o, LV_EVENT_CLICKED) || has_handler(o, LV_EVENT_SHORT_CLICKED) ||
            has_handler(o, LV_EVENT_LONG_PRESSED) || lv_obj_check_type(o, &lv_textarea_class) ||
            lv_obj_check_type(o, &lv_slider_class) || lv_obj_has_flag(o, LV_OBJ_FLAG_CHECKABLE));
}

/* Every control, touch-only subtrees (UI_KEYBOARD_SKIP: the soft keyboard, overlays) included. */
static int collect_ctrls(lv_obj_t *o, lv_obj_t **out, int n)
{
    if (!shown(o)) return n;
    if (is_control(o) && n < MAX_CTRLS) out[n++] = o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) n = collect_ctrls(lv_obj_get_child(o, i), out, n);
    return n;
}

/* The area LVGL really delivers to o: its coords grown by ext_click_area, then clipped to every ancestor,
 * because lv_indev_search_obj only looks at a parent's children for a point on the parent's coords (grown
 * by its ext draw size only when the parent is OVERFLOW_VISIBLE; lv_indev.c). */
static lv_area_t hit_area(lv_obj_t *o)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int32_t pad = o->spec_attr ? o->spec_attr->ext_click_pad : 0;
    lv_area_increase(&a, pad, pad);
    for (lv_obj_t *p = lv_obj_get_parent(o); p; p = lv_obj_get_parent(p)) {
        lv_area_t c;
        lv_obj_get_coords(p, &c);
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_OVERFLOW_VISIBLE)) {
            int32_t e = lv_obj_get_ext_draw_size(p);
            lv_area_increase(&c, e, e);
        }
        if (!lv_area_intersect(&a, &a, &c)) { a.x2 = a.x1 - 1; a.y2 = a.y1 - 1; break; }
    }
    return a;
}

/* The bar a bar control sits in: its ancestor that is a direct child of the screen (or layer). */
static lv_obj_t *bar_of(lv_obj_t *o, lv_obj_t *root)
{
    lv_obj_t *p = lv_obj_get_parent(o);
    while (p && lv_obj_get_parent(p) && lv_obj_get_parent(p) != root && lv_obj_get_parent(lv_obj_get_parent(p)))
        p = lv_obj_get_parent(p);
    return p;
}

static bool ancestor(lv_obj_t *a, lv_obj_t *b)
{
    for (lv_obj_t *p = lv_obj_get_parent(b); p; p = lv_obj_get_parent(p))
        if (p == a) return true;
    return false;
}

static const char *ctrl_name(lv_obj_t *o, char *buf, size_t cap)
{
    lv_obj_t *l = lv_obj_check_type(o, &lv_label_class) ? o : NULL;
    for (uint32_t i = 0; !l && i < lv_obj_get_child_count(o); i++)
        if (lv_obj_check_type(lv_obj_get_child(o, i), &lv_label_class)) l = lv_obj_get_child(o, i);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    snprintf(buf, cap, "\"%.24s\" at %d,%d", l ? lv_label_get_text(l) : "?", (int)a.x1, (int)a.y1);
    return buf;
}

/* True when a scrollable ancestor (below the screen) that really overflows or is scrolled shows o only in
   part, or not at all (review M5: a merely scrollable container clips like any other and is checked). "o" here
   is its hit area, and an edge counts only while a scroll toward it is still possible: a row a scroll leaves at
   the viewport's edge is checked once scrolled into view, but a clip no scroll undoes (the sides of a vertical
   list, the top at scroll 0) is still a failure (T7 review I1). */
static bool scrolled_out(lv_obj_t *o)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int32_t pad = o->spec_attr ? o->spec_attr->ext_click_pad : 0;
    lv_area_increase(&a, pad, pad);
    for (lv_obj_t *p = lv_obj_get_parent(o); p && lv_obj_get_parent(p); p = lv_obj_get_parent(p)) {
        if (!lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE)) continue;
        if (!lv_obj_get_scroll_top(p) && !lv_obj_get_scroll_bottom(p) && !lv_obj_get_scroll_left(p) &&
            !lv_obj_get_scroll_right(p)) continue;   /* everything fits: no part is scrolled out */
        lv_area_t c;
        lv_obj_get_coords(p, &c);
        if ((a.y1 < c.y1 && lv_obj_get_scroll_top(p) > 0) || (a.y2 > c.y2 && lv_obj_get_scroll_bottom(p) > 0) ||
            (a.x1 < c.x1 && lv_obj_get_scroll_left(p) > 0) || (a.x2 > c.x2 && lv_obj_get_scroll_right(p) > 0))
            return true;
    }
    return false;
}

/* P4C-5 (amends spec §8.1): a body control is >= 64 px visible with >= 80 px of hit area each way; a bar
 * control (UI_BAR_CONTROL) is >= 48 px visible, and its hit area is the bar's full height and >= 80 px wide. */
int pv_touch_targets(lv_obj_t *root, char *why, size_t cap)
{
    static lv_obj_t *c[MAX_CTRLS];
    int all = collect_ctrls(root, c, 0), n = 0;
    for (int i = 0; i < all; i++)
        if (!scrolled_out(c[i])) c[n++] = c[i];
    char nm[64], nm2[64];
    for (int i = 0; i < n; i++) {
        lv_area_t a, hit = hit_area(c[i]);
        lv_obj_get_coords(c[i], &a);
        int32_t w = lv_area_get_width(&a), h = lv_area_get_height(&a);
        int32_t hw = lv_area_get_width(&hit), hh = lv_area_get_height(&hit);
        if (hw < 0) hw = 0;
        if (hh < 0) hh = 0;
        bool bar = lv_obj_has_flag(c[i], UI_BAR_CONTROL);
        int32_t min_vis = bar ? UI_TK_BAR_CTRL : UI_TK_TARGET;
        if (w < min_vis || h < min_vis) {
            say(why, cap, "%s is %dx%d visible, under %d", ctrl_name(c[i], nm, sizeof(nm)), (int)w, (int)h, (int)min_vis);
            return 1;
        }
        int32_t need_h = UI_TK_HIT;
        if (bar) {
            lv_obj_t *b = bar_of(c[i], root);
            need_h = b ? lv_obj_get_height(b) : UI_TK_HIT;
        }
        if (hw < UI_TK_HIT || hh < need_h) {
            say(why, cap, "%s hits %dx%d after clipping, needs %dx%d", ctrl_name(c[i], nm, sizeof(nm)), (int)hw,
                (int)hh, UI_TK_HIT, (int)need_h);
            return 1;
        }
    }
    for (int i = 0; i < n; i++) {
        lv_area_t a;
        lv_obj_get_coords(c[i], &a);
        for (int j = i + 1; j < n; j++) {
            if (ancestor(c[i], c[j]) || ancestor(c[j], c[i])) continue;
            lv_area_t b;
            lv_obj_get_coords(c[j], &b);
            int32_t dx = b.x1 > a.x2 ? b.x1 - a.x2 - 1 : a.x1 > b.x2 ? a.x1 - b.x2 - 1 : -1;
            int32_t dy = b.y1 > a.y2 ? b.y1 - a.y2 - 1 : a.y1 > b.y2 ? a.y1 - b.y2 - 1 : -1;
            int32_t gap = dx > dy ? dx : dy;   /* -1: the boxes overlap */
            if (gap < UI_TK_GAP_S) {
                say(why, cap, "%s and %s are %d px apart, under %d", ctrl_name(c[i], nm, sizeof(nm)),
                    ctrl_name(c[j], nm2, sizeof(nm2)), (int)(gap < 0 ? 0 : gap), UI_TK_GAP_S);
                return 1;
            }
        }
    }
    return 0;
}

int pv_bar_hits_clear(lv_obj_t *root, lv_obj_t *row, char *why, size_t cap)
{
    static lv_obj_t *c[MAX_CTRLS];
    int n = collect_ctrls(root, c, 0);
    lv_area_t r = hit_area(row), x;
    char nm[64];
    for (int i = 0; i < n; i++) {
        if (!lv_obj_has_flag(c[i], UI_BAR_CONTROL)) continue;
        lv_area_t h = hit_area(c[i]);
        if (lv_area_get_width(&h) <= 0 || lv_area_get_height(&h) <= 0) continue;
        if (lv_area_intersect(&x, &h, &r)) {
            say(why, cap, "%s hits %d,%d-%d,%d, over the row's %d,%d-%d,%d", ctrl_name(c[i], nm, sizeof(nm)), (int)h.x1,
                (int)h.y1, (int)h.x2, (int)h.y2, (int)r.x1, (int)r.y1, (int)r.x2, (int)r.y2);
            return 1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------- fit */
static int fit_walk(lv_obj_t *o, char *why, size_t cap)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return 0;
    lv_obj_t *p = lv_obj_get_parent(o);
    if (p && lv_obj_check_type(o, &lv_label_class) && lv_obj_get_width(o) > 0) {
        lv_area_t a, b;
        lv_obj_get_coords(o, &a);
        lv_obj_get_coords(p, &b);
        bool ver_scroll = lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE) && (lv_obj_get_scroll_dir(p) & LV_DIR_VER);
        const char *t = lv_label_get_text(o);
        if (a.x1 < b.x1 || a.x2 > b.x2 || (!ver_scroll && (a.y1 < b.y1 || a.y2 > b.y2))) {
            say(why, cap, "label \"%.32s\" (%d,%d-%d,%d) extends past its parent (%d,%d-%d,%d)", t, (int)a.x1,
                (int)a.y1, (int)a.x2, (int)a.y2, (int)b.x1, (int)b.y1, (int)b.x2, (int)b.y2);
            return 1;
        }
        if (lv_label_get_long_mode(o) == LV_LABEL_LONG_MODE_WRAP) {
            lv_point_t need;
            const lv_font_t *f = lv_obj_get_style_text_font(o, 0);
            lv_text_get_size(&need, t, f, lv_obj_get_style_text_letter_space(o, 0),
                             lv_obj_get_style_text_line_space(o, 0), lv_obj_get_content_width(o), LV_TEXT_FLAG_NONE);
            if (need.y > lv_obj_get_content_height(o)) {
                say(why, cap, "wrapped label \"%.32s\" needs %d px, has %d", t, (int)need.y,
                    (int)lv_obj_get_content_height(o));
                return 1;
            }
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
        if (fit_walk(lv_obj_get_child(o, i), why, cap)) return 1;
    return 0;
}

int pv_fit(lv_obj_t *root, char *why, size_t cap) { return fit_walk(root, why, cap); }

/* -------------------------------------------------------------- idle */
static uint64_t s_flushed;
static void count_flush(lv_event_t *e)
{
    const lv_area_t *a = lv_event_get_param(e);
    if (!a) return;
    s_flushed += (uint64_t)lv_area_get_width(a) * lv_area_get_height(a);
    if (getenv("PV_DEBUG")) printf("idle flush %d,%d-%d,%d\n", (int)a->x1, (int)a->y1, (int)a->x2, (int)a->y2);
}

unsigned pv_idle_pixels(lv_display_t *d, unsigned ms)
{
    lv_refr_now(d);   /* the first draw */
    s_flushed = 0;
    lv_display_add_event_cb(d, count_flush, LV_EVENT_FLUSH_START, NULL);
    for (unsigned t = 0; t < ms; t += 50) {
        lv_tick_inc(50);
        lv_timer_handler();
        lv_refr_now(d);
    }
    lv_display_remove_event_cb_with_user_data(d, count_flush, NULL);
    return (unsigned)s_flushed;
}

/* ------------------------------------------------------------ timers */
int pv_timers(bool sessions_shown, char *why, size_t cap)
{
    lv_display_t *d = lv_display_get_default();
    for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t)) {
        if (t->paused) continue;
        if (t == lv_display_get_refr_timer(d) || t == lv_anim_get_timer()) continue;   /* LVGL's own */
        bool indev = false;
        for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in))
            if (t == lv_indev_get_read_timer(in)) indev = true;
        if (indev) continue;
        if (t == ui_sessions_voice_meter() && t->period == 100) continue;   /* P5-4: the voice meter, only while recording */
        if (t->period < 300 && !(t->period == 16 && ui_terminal_is_top())) {   /* the pty pump */
            say(why, cap, "a %u ms timer", (unsigned)t->period);
            return 1;
        }
        if (t->period == 300 && !sessions_shown) {
            say(why, cap, "the 300 ms tick without Sessions shown");
            return 1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------- run */
/* The screen with lv_layer_top (dialogs, toasts) blended over it, as RGB888. */
static lv_draw_buf_t *compose(lv_obj_t *scr)
{
    lv_draw_buf_t *base = lv_snapshot_take(scr, LV_COLOR_FORMAT_RGB888);
    if (!base) return NULL;
    if (!lv_obj_get_child_count(lv_layer_top())) return base;
    lv_draw_buf_t *top = lv_snapshot_take(lv_layer_top(), LV_COLOR_FORMAT_ARGB8888);
    if (!top) return base;
    for (uint32_t y = 0; y < base->header.h && y < top->header.h; y++)
        for (uint32_t x = 0; x < base->header.w && x < top->header.w; x++) {
            uint8_t *p = base->data + y * base->header.stride + x * 3;
            const uint8_t *q = top->data + y * top->header.stride + x * 4;
            unsigned a = q[3];
            for (int c = 0; c < 3; c++) p[c] = (uint8_t)((q[c] * a + p[c] * (255 - a)) / 255);
        }
    lv_draw_buf_destroy(top);
    return base;
}

int pv_amber_pixels(lv_obj_t *scr)
{
    lv_obj_update_layout(scr);
    lv_obj_update_layout(lv_layer_top());
    lv_draw_buf_t *snap = compose(scr);
    if (!snap) return -1;
    int n = amber_count(scr, snap, false);
    lv_draw_buf_destroy(snap);
    return n;
}

static int write_ppm(const lv_draw_buf_t *snap, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    fprintf(f, "P6\n%u %u\n255\n", (unsigned)snap->header.w, (unsigned)snap->header.h);
    for (uint32_t y = 0; y < snap->header.h; y++)
        for (uint32_t x = 0; x < snap->header.w; x++) {
            const uint8_t *p = snap->data + y * snap->header.stride + x * 3;
            fputc(p[2], f); fputc(p[1], f); fputc(p[0], f);
        }
    fclose(f);
    return 0;
}

int pv_run_checks(lv_display_t *d, lv_obj_t *scr, const char *name, bool needs_you_expected, unsigned skip)
{
    char why[256];
    int fails = 0;
    lv_obj_update_layout(scr);
    lv_obj_update_layout(lv_layer_top());
    lv_refr_now(d);
    lv_draw_buf_t *snap = compose(scr);
    if (!snap) { printf("check %s: FAIL snapshot\n", name); return 1; }
    if (!(skip & PV_SKIP_AMBER)) {
        int out = pv_amber_outside(scr, snap);
        if (out) { printf("check %s: FAIL amber: %d px outside the NEEDS YOU parts\n", name, out); fails++; }
        if (needs_you_expected && !amber_count(scr, snap, false)) {
            printf("check %s: FAIL amber: NEEDS YOU expected, none drawn\n", name);
            fails++;
        }
    }
    if (!(skip & PV_SKIP_TARGETS)) {
        if (pv_touch_targets(scr, why, sizeof(why)) || pv_touch_targets(lv_layer_top(), why, sizeof(why))) {
            printf("check %s: FAIL targets: %s\n", name, why);
            fails++;
        }
    }
    if (pv_fit(scr, why, sizeof(why)) || pv_fit(lv_layer_top(), why, sizeof(why))) {
        printf("check %s: FAIL fit: %s\n", name, why);
        fails++;
    }
    unsigned idle = pv_idle_pixels(d, 1000);
    if (idle) { printf("check %s: FAIL idle: %u px redrawn on a static screen\n", name, idle); fails++; }
    bool sessions = ui_nav_is_top(ui_sessions_create) && lv_screen_active() == scr;
    if (pv_timers(sessions, why, sizeof(why))) { printf("check %s: FAIL timers: %s\n", name, why); fails++; }
    char path[128];
    snprintf(path, sizeof(path), "%s.ppm", name);
    if (write_ppm(snap, path)) { printf("check %s: FAIL writing %s\n", name, path); fails++; }
    lv_draw_buf_destroy(snap);
    if (!fails) printf("check %s: ok\n", name);
    return fails ? 1 : 0;
}
