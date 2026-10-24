#include "ui/ui.h"
#include "term/term_view.h"
#include "ui/ui_boot.h"
#include "ui/ui_comp.h"
#include "checks.h"
#include "settings/settings.h"
#include "net/tailscale.h"
#include "input/keyboard.h"
#include "sys/sysinfo.h"
#include "net/wifi_mgr.h"
#include "companion/cc_link.h"
#include "companion/cc_sessions.h"
#include "servers/server_store.h"
#include "sys/power.h"
#include "sys/checkin.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
app_settings_t g_settings;
void settings_save(void) {}
void settings_reset_defaults(void) {}
bool g_ssh_session_active=false;
void softkbd_hide_all(void) {}
int softkbd_height(void) { return 300; }
const char *sysinfo_power_src_text(void) { return "USB-C"; }
const char *wifi_mgr_ssid(void) { return "Clientre Wi-Fi"; }
bool sd_is_mounted(void) { return true; }
static int env_int(const char *n, int d) { const char *e = getenv(n); return e && *e ? atoi(e) : d; }
int sysinfo_battery_percent(void) { return env_int("PREVIEW_BAT", 52); }
int sysinfo_rssi_cached(void) { return env_int("PREVIEW_RSSI", -60); }
const char *sysinfo_app_version(void) { return "preview"; }
void sysinfo_apply_timezone(void) {}
void sysinfo_start_sntp(void) {}
static bool s_rc_preview;   /* dashboard-reconnecting: woken from Radio off, Wi-Fi and the link not back yet */
bool wifi_mgr_is_connected(void) { return !s_rc_preview; }
bool wifi_mgr_is_connecting(void) { return false; }
void wifi_mgr_get_ip4(char *b, size_t n) { snprintf(b,n,"192.168.1.25"); }
void wifi_mgr_disconnect(void) {}
/* Plan 3 surfaces Settings calls (the preview stopped linking at 8b94efb without them). */
bool wifi_mgr_static_ip_get(wifi_static_ip_t *out) { if (out) memset(out, 0, sizeof(*out)); return true; }
bool wifi_mgr_static_ip_set(const wifi_static_ip_t *in) { (void)in; return true; }
void link_down_restart(const char *why) { (void)why; exit(0); }
bool keyboard_present(void) { return true; }
bool keyboard_usb_present(void) { return false; }
static void *focused_textarea;
void keyboard_lvgl_focus(void *p) { focused_textarea = p; }
void *keyboard_lvgl_focused(void) { return focused_textarea; }
void keyboard_set_layout(uint8_t layout) { (void)layout; }
void keyboard_set_led(uint8_t a,uint8_t b,uint8_t c,uint8_t d,uint8_t e) {}
void power_kbd_light_refresh(void) {}
/* power owner: the preview is always Active */
bool power_setting_set(const char *name, int n) { (void)name; (void)n; return true; }
bool hibernate_supported(void) { return false; }
power_boot_t hibernate_boot_kind(void) { return POWER_BOOT_NORMAL; }
void power_hold(power_hold_t h, bool on) { (void)h; (void)on; }
uint32_t power_wake_seq(void) { return s_rc_preview ? 1 : 0; }
uint32_t power_wake_gen(void) { return s_rc_preview ? 1 : 0; }   /* no result newer than the wake: stale */
power_state_t power_woke_from(void) { return s_rc_preview ? PS_RADIO_OFF : PS_ACTIVE; }
bool checkin_woke_parked(void) { return s_rc_preview; }
power_wake_t power_wake_reason(void) { return PW_CONSOLE; }
bool power_shield_up(void) { return false; }
ts_state_t tailscale_state(void)
{
    int m = env_int("PREVIEW_TS", 2);
    return m == 0 ? TS_OFF : m == 1 ? TS_CONNECTING : TS_CONNECTED;
}
bool tailscale_start(void) { return true; }
void tailscale_stop(void) {}
bool tailscale_enabled(void) { return g_settings.ts_enable != 0; }
bool tailscale_stopping(void) { return false; }
void tailscale_set_enabled(bool on) { g_settings.ts_enable = on; }
const char *tailscale_state_text(void) { return tailscale_state() == TS_CONNECTED ? "Connected" : tailscale_state() == TS_OFF ? "Off" : "Connecting"; }
const char *tailscale_vpn_ip(void) { return tailscale_state() == TS_CONNECTED ? "100.101.102.103" : ""; }
void softkbd_attach_textarea(lv_obj_t *a,lv_obj_t *b) {}
void esp_restart(void) {}
void bsp_display_brightness_set(int v) {}
bool server_store_set_on_sd(bool on) { return true; }
void server_store_reload(void) {}
int server_store_count(void) { return 3; }
static server_t s_preview_server = { .name = "Mac", .host = "mac", .port = 22, .user = "me", .use_key = true };
const server_t *server_store_get(int i) { return i >= 0 && i < 3 ? &s_preview_server : NULL; }
power_info_t sysinfo_power(void)
{
    power_info_t p = { .percent = env_int("PREVIEW_BAT", 52), .valid = true };
    p.src = env_int("PREVIEW_CHG", 0) ? PWR_USB_CHARGING : PWR_BATTERY;
    return p;
}
static key_sink_t preview_sink; static bool preview_have_sink;   /* dashboard-keys drives dash_key */
void keyboard_set_sink(const key_sink_t *sink) { if (sink) { preview_sink = *sink; preview_have_sink = true; } else preview_have_sink = false; }
void keyboard_clear_sink(void *user) { if (preview_have_sink && preview_sink.user == user) preview_have_sink = false; }
void ui_terminal_open_command(int i, const char *c, void (*cb)(void *), void *ctx) { (void)i; (void)c; if (cb) cb(ctx); }
bool ui_terminal_is_top(void) { return false; }   /* the preview builds no terminal screen yet */
/* companion link: the dashboard reads the synthetic agents fixture */
static cc_session_t s_preview_sessions[16];
static int s_preview_n = -1;
static void preview_sessions_load(void) {
    FILE *f = fopen(FIXTURE_DIR "/agents_synthetic.json", "rb");
    assert(f);
    static char json[8192]; size_t n = fread(json, 1, sizeof(json) - 1, f); json[n] = 0; fclose(f);
    s_preview_n = cc_sessions_parse(json, s_preview_sessions, 16);
    assert(s_preview_n > 0);
    /* ages: started a few minutes before the preview clock */
    int64_t now = (int64_t)time(NULL) * 1000;
    for (int i = 0; i < s_preview_n; i++) {
        s_preview_sessions[i].started_ms = now - (int64_t)(i + 1) * 7 * 60000;
        /* the fixture has no pids: treat the unfinished ones as running */
        if (s_preview_sessions[i].state != CC_DONE && s_preview_sessions[i].state != CC_ENDED) s_preview_sessions[i].live = true;
    }
}
int cc_poll_snapshot(cc_session_t *out, int cap) {
    int n = s_preview_n < cap ? s_preview_n : cap;
    if (n > 0) memcpy(out, s_preview_sessions, sizeof(cc_session_t) * n);
    return n;
}
void cc_poll_subscribe(cc_sessions_cb_t cb, void *ctx) { (void)cb; (void)ctx; }
void cc_poll_set_rate(int s) { (void)s; }
void cc_poll_set_floor(int s) { (void)s; }
uint32_t cc_poll_generation(void) { return 1; }
int64_t cc_poll_last_ok_ms(void) { return 1; }
bool cc_link_busy(void) { return false; }
void cc_link_watch_enable(bool on) { (void)on; }
void cc_link_watch_test_fallback(void) {}
const char *cc_watch_status_text(char *b, size_t cap) { snprintf(b, cap, "watch: on I=3s, last block 4s ago, heartbeat 12s ago"); return b; }
bool cc_link_connected(void) { return !s_rc_preview; }
const char *cc_link_status_text(char *b, size_t cap) { snprintf(b, cap, "connected Mac"); return b; }
cc_link_fail_t cc_link_failure(void) { return CC_LINK_FAIL_NONE; }
bool cc_link_mac_address(char *o, size_t cap) { snprintf(o, cap, "100.101.102.103"); return true; }
bool cc_link_mac_path(char *o, size_t cap) { snprintf(o, cap, "lan 192.168.1.50"); return true; }
bool cc_link_set_lan_fallback(const char *ip) { (void)ip; return true; }
const char *cc_link_lan_fallback(char *o, size_t cap) { snprintf(o, cap, "192.168.1.50"); return o; }
int cc_poll_failures(bool *too_big) { if (too_big) *too_big = false; return 0; }
const char *cc_settings_claude_path(char *o, size_t cap) { snprintf(o, cap, "~/.local/bin/claude"); return o; }
const char *cc_settings_server_name(char *o, size_t cap) { snprintf(o, cap, "Mac"); return o; }
int cc_settings_server(void) { return 0; }
void cc_link_set_server(const char *n) { (void)n; }
bool cc_link_set_claude_path(const char *p) { (void)p; return true; }
esp_err_t cc_link_exec_tail(const char *cmd, char *out, size_t cap, int *st, char *err, size_t ecap, int ms) {
    (void)cmd; (void)ms; if (ecap) err[0] = 0; if (st) *st = 0;
    snprintf(out, cap, "\x1b[1m\xe2\x8f\xba\x1b[0m Read\x1b[1C3\x1b[1Cfiles\r\n  \xe2\x8e\xbf  src/main.c\r\n"
             "\xe2\x95\xad\xe2\x94\x80\xe2\x94\x80\r\n\xe2\x8f\xba Which colour should the theme use?\r\n"
             "  1. Amber   2. Green\r\n\xe2\x9c\xbb Waiting for your answer\r\n\xe2\x9d\xaf\xc2\xa0\r\n");
    return ESP_OK;
}
esp_err_t cc_link_paste(const char *id, const char *t, char *err, size_t cap) { (void)id; (void)t; if (cap) err[0] = 0; return ESP_OK; }
esp_err_t cc_link_check_live(const char *id, char *err, size_t cap) { (void)id; if (cap) err[0] = 0; return ESP_OK; }
static int s_forget_calls;
#define PREVIEW_FP "SHA256:Xq3v9yPreviewFingerprintOnly0123456789abcd"
esp_err_t cc_link_forget_host_key(char *fp, size_t cap, int ms) {
    (void)ms; s_forget_calls++;
    snprintf(fp, cap, PREVIEW_FP);
    return ESP_OK;
}
uint32_t cc_link_forget_done(char *fp, size_t cap) { snprintf(fp, cap, PREVIEW_FP); return (uint32_t)s_forget_calls; }
bool cc_link_pinned_fingerprint(char *fp, size_t cap) { snprintf(fp, cap, "SHA256:OldPreviewKeyOnly9876543210zyxwvutsrqponm"); return true; }
esp_err_t cc_link_debug_corrupt_host_key(char *h, size_t cap) { if (cap) h[0] = 0; return ESP_OK; }
#define SCREEN(name) lv_obj_t *name(void *p) { return ui_screen_base(); }
SCREEN(ui_hosts_create)
SCREEN(ui_wifi_create) SCREEN(ui_tailscale_create)
SCREEN(ui_server_form_create)
static int snapshot(lv_obj_t *s, const char *path) {
    int w=lv_obj_get_width(s), h=lv_obj_get_height(s);
    lv_draw_buf_t *snap=lv_snapshot_take(s,LV_COLOR_FORMAT_RGB888);
    if(!snap) return 1;
    FILE *f=fopen(path,"wb");
    if (!f) return 1;
    fprintf(f,"P6\n%d %d\n255\n",w,h);
    for(int y=0;y<h;y++) for(int x=0;x<w;x++) {
        uint8_t *p=snap->data+y*snap->header.stride+x*3;
        fputc(p[2],f); fputc(p[1],f); fputc(p[0],f);
    }
    fclose(f); lv_draw_buf_destroy(snap); return 0;
}
/* The screen with lv_layer_top (modals, toasts) blended over it. */
static int snapshot_top(lv_obj_t *s, const char *path) {
    int w=lv_obj_get_width(s), h=lv_obj_get_height(s);
    lv_draw_buf_t *base=lv_snapshot_take(s,LV_COLOR_FORMAT_RGB888);
    lv_draw_buf_t *top=lv_snapshot_take(lv_layer_top(),LV_COLOR_FORMAT_ARGB8888);
    if(!base||!top) return 1;
    FILE *f=fopen(path,"wb");
    if (!f) return 1;
    fprintf(f,"P6\n%d %d\n255\n",w,h);
    for(int y=0;y<h;y++) for(int x=0;x<w;x++) {
        uint8_t *p=base->data+y*base->header.stride+x*3;
        uint8_t *q=top->data+y*top->header.stride+x*4;
        unsigned a=q[3];
        for (int c=2;c>=0;c--) fputc((q[c]*a+p[c]*(255-a))/255,f);
    }
    fclose(f); lv_draw_buf_destroy(base); lv_draw_buf_destroy(top); return 0;
}
/* The label under o whose text is t (or starts with t when prefix is set). */
static lv_obj_t *find_label(lv_obj_t *o, const char *t, bool prefix) {
    if (lv_obj_check_type(o,&lv_label_class)) {
        const char *x=lv_label_get_text(o);
        if (prefix ? !strncmp(x,t,strlen(t)) : !strcmp(x,t)) return o;
    }
    for (uint32_t i=0;i<lv_obj_get_child_count(o);i++) {
        lv_obj_t *r=find_label(lv_obj_get_child(o,i),t,prefix);
        if (r) return r;
    }
    return NULL;
}
/* The first clickable object at or above that label. */
static lv_obj_t *find_clickable(lv_obj_t *o, const char *t) {
    o=find_label(o,t,false);
    while (o && !lv_obj_has_flag(o,LV_OBJ_FLAG_CLICKABLE)) o=lv_obj_get_parent(o);
    return o;
}
static uint64_t flushed_pixels;
static unsigned flushes;
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p) {
    flushed_pixels += (uint64_t)lv_area_get_width(a) * lv_area_get_height(a);
    flushes++;
    lv_display_flush_ready(d);
}
static void step(lv_display_t *d, unsigned ms) {
    lv_tick_inc(ms); lv_timer_handler(); lv_refr_now(d);
}
static int keyboard_clicks, keyboard_menus, keyboard_releases, keyboard_answer;
static void keyboard_test_cb(lv_event_t *e) {
    if (lv_event_get_code(e)==LV_EVENT_CLICKED) keyboard_clicks++;
    if (lv_event_get_code(e)==LV_EVENT_LONG_PRESSED) keyboard_menus++;
    if (lv_event_get_code(e)==LV_EVENT_RELEASED) keyboard_releases++;
}
static void keyboard_confirmed(bool yes, void *ud) { keyboard_answer=yes ? 1 : -1; }
static bool press_key(special_key_t key, uint8_t mods, bool terminal) {
    key_event_t ev={.type=KEY_EV_SPECIAL,.key=key,.mods=mods};
    return ui_keyboard_route(&ev,terminal);
}
static void keyboard_check(void) {
    lv_obj_t *scr=ui_screen_base(); lv_screen_load(scr);
    lv_obj_t *hidden=ui_button(scr,"hidden",keyboard_test_cb,NULL);
    lv_obj_add_flag(hidden,LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *disabled=ui_button(scr,"disabled",keyboard_test_cb,NULL);
    lv_obj_add_state(disabled,LV_STATE_DISABLED);
    lv_obj_t *touch_only=lv_obj_create(scr);
    lv_obj_add_flag(touch_only,UI_KEYBOARD_SKIP);
    ui_button(touch_only,"soft key",keyboard_test_cb,NULL);
    lv_obj_t *button=ui_button(scr,"action",keyboard_test_cb,NULL);
    lv_obj_add_event_cb(button,keyboard_test_cb,LV_EVENT_LONG_PRESSED,NULL);
    lv_obj_t *slider=lv_slider_create(scr); lv_slider_set_range(slider,0,100);
    lv_obj_add_event_cb(slider,keyboard_test_cb,LV_EVENT_RELEASED,NULL);
    lv_obj_t *ta=ui_textarea(scr,"text",false,true);
    press_key(SK_TAB,0,false); assert(lv_obj_has_state(button,LV_STATE_FOCUS_KEY));
    press_key(SK_ENTER,0,false); assert(keyboard_clicks==1);
    press_key(SK_ENTER,MOD_CTRL,false); assert(keyboard_menus==1);
    press_key(SK_TAB,0,false); assert(lv_obj_has_state(slider,LV_STATE_FOCUS_KEY));
    press_key(SK_RIGHT,0,false); assert(lv_slider_get_value(slider)==1 && keyboard_releases==1);
    press_key(SK_TAB,0,false); assert(keyboard_lvgl_focused()==ta);
    assert(!press_key(SK_LEFT,0,false));
    press_key(SK_TAB,MOD_SHIFT,false); assert(keyboard_lvgl_focused()==NULL);
    ui_confirm("test",NULL,keyboard_confirmed,NULL);
    press_key(SK_ESC,0,true); assert(keyboard_answer==-1 && keyboard_clicks==1);
    ui_confirm("test",NULL,keyboard_confirmed,NULL);
    press_key(SK_TAB,0,true); press_key(SK_TAB,0,true); press_key(SK_ENTER,0,true);
    assert(keyboard_answer==1);
    assert(!press_key(SK_TAB,0,true)); /* untouched SSH Tab */
    press_key(SK_TAB,MOD_CTRL,true); assert(ui_keyboard_owns_input());
    press_key(SK_ESC,0,true); assert(!ui_keyboard_owns_input());
    assert(!press_key(SK_ESC,0,true)); /* untouched SSH Esc */
    press_key(SK_TAB,0,false);
    lv_obj_delete(button); /* focused deletion must not leave a dangling target */
    press_key(SK_TAB,0,false); press_key(SK_RIGHT,0,false);
    ui_prompt("test","", "",false,NULL,NULL);
    press_key(SK_TAB,0,true); assert(keyboard_lvgl_focused()!=NULL);
    press_key(SK_ESC,0,true); assert(keyboard_lvgl_focused()==NULL);
    keyboard_lvgl_focus(ta); /* mixed touch/physical input follows the touched field */
    assert(!press_key(SK_RIGHT,0,false)); assert(lv_obj_has_state(ta,LV_STATE_FOCUS_KEY));
    ui_confirm("outer",NULL,keyboard_confirmed,NULL);
    ui_alert("inner","test");
    press_key(SK_ESC,0,true); assert(ui_keyboard_owns_input());
    press_key(SK_ESC,0,true); assert(!ui_keyboard_owns_input());
    lv_obj_t *sw=ui_switch_row(scr,"switch",false,NULL,NULL);
    keyboard_lvgl_focus(ta);
    press_key(SK_RIGHT,0,false); press_key(SK_TAB,0,false);
    lv_obj_t *toggle=lv_obj_get_user_data(sw);
    assert(lv_obj_has_state(toggle,LV_STATE_FOCUS_KEY));
    press_key(SK_ENTER,0,false); assert(lv_obj_has_state(toggle,LV_STATE_CHECKED));
    press_key(SK_ENTER,0,false); assert(!lv_obj_has_state(toggle,LV_STATE_CHECKED));
    puts("Keyboard navigation, slider persistence, modal isolation, mixed input, switches, deletion and SSH routing checks passed.");
}

static void terminal_spacing_check(void) {
    lv_obj_t *scr=ui_screen_base(); lv_screen_load(scr);
    fonts_set_term_line_height(32);
    vt_t *vt=vt_create(80,16,10);
    term_view_t *tv=term_view_create(scr,vt,&g_term_font);
    lv_obj_set_size(term_view_obj(tv),960,512);
    int cols,rows;
    term_view_fit(tv,&cols,&rows);
    assert(cols==80 && rows==16 && term_view_cell_h(tv)==32);
    const char *sample="ABCDEFGHIJKLMNOPQRSTUVWXYZ gjpqy 0123456789 日本語テスト\r\n";
    for(int i=0;i<12;i++) vt_feed(vt,(const uint8_t *)sample,strlen(sample));
    const char *hide="\033[?25l";
    vt_feed(vt,(const uint8_t *)hide,strlen(hide));
    lv_draw_buf_t *snap=lv_snapshot_take(term_view_obj(tv),LV_COLOR_FORMAT_RGB888);
    assert(snap);
    /* At least four wholly blank scanlines between every pair of rendered rows. */
    for(int row=0;row<11;row++) {
        int last=-1,first=32;
        for(int y=0;y<64;y++) {
            bool ink=false;
            for(int x=0;x<960;x++) {
                uint8_t *p=snap->data+(row*32+y)*snap->header.stride+x*3;
                if(p[0]||p[1]||p[2]) { ink=true; break; }
            }
            if(ink && y<32) last=y;
            if(ink && y>=32 && first==32) first=y-32;
        }
        assert(last>=0 && first<32 && 32+first-last-1>=4);
    }
    lv_draw_buf_destroy(snap);
    assert(snapshot(scr,"terminal.ppm")==0);
    fonts_set_term_line_height(38);
    term_view_fit(tv,&cols,&rows);
    assert(term_view_cell_h(tv)==38 && rows==13);
    term_view_destroy(tv); vt_destroy(vt);
    puts("Terminal: 32px rows, visible inter-row whitespace and live 38px resize passed.");
}

static lv_point_t test_pointer;
static bool test_pointer_down;
static unsigned test_swipes;
static void pointer_read(lv_indev_t *in, lv_indev_data_t *data) {
    (void)in; data->point=test_pointer;
    data->state=test_pointer_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static void count_swipe(lv_dir_t dir) { if(dir==LV_DIR_LEFT || dir==LV_DIR_RIGHT) test_swipes++; }
static void pointer_move(lv_indev_t *in,int x,int y,bool down) {
    test_pointer=(lv_point_t){x,y}; test_pointer_down=down;
    lv_tick_inc(20); lv_indev_read(in);
}
static void gesture_check(void) {
    lv_indev_t *in=lv_indev_create(); lv_indev_set_type(in,LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in,pointer_read); ui_init();
    lv_obj_t *scr=ui_screen_base(); lv_screen_load(scr);
    lv_obj_remove_flag(scr,LV_OBJ_FLAG_SCROLLABLE);
    ui_screen_set_gesture(scr,count_swipe);
    lv_obj_t *slider=lv_slider_create(scr); lv_obj_set_pos(slider,50,80); lv_obj_set_size(slider,400,40);
    lv_obj_update_layout(scr);
    pointer_move(in,100,100,true);
    for(int x=130;x<=400;x+=30) pointer_move(in,x,100,true);
    pointer_move(in,400,100,false);
    assert(lv_slider_get_value(slider)>50 && test_swipes==0);
    pointer_move(in,100,300,true);
    for(int x=130;x<=400;x+=30) pointer_move(in,x,300,true);
    pointer_move(in,400,300,false);
    assert(test_swipes==1);
    lv_indev_delete(in);
    puts("Touch slider drag changes volume control without track swipe; background swipe retained.");
}
static void terminal_art_check(void) {
    lv_font_glyph_dsc_t glyph;
    assert(lv_font_get_glyph_dsc(&g_term_font,&glyph,0x23f5,0));
    assert(!glyph.is_placeholder && glyph.adv_w==12);
    lv_obj_t *scr=ui_screen_base(); lv_screen_load(scr);
    fonts_set_term_line_height(32);
    vt_t *vt=vt_create(60,12,0);
    term_view_t *tv=term_view_create(scr,vt,&g_term_font);
    lv_obj_set_size(term_view_obj(tv),720,408);
    const char *art="[⏵⏵] ASCII 日本語\r\n  ▐▛███▜▌\r\n ▝▜█████▛▘\r\n   ▘▘ ▝▝\r\n█████\r\n█████\r\n";
    vt_feed(vt,(const uint8_t *)art,strlen(art));
    const char *hide="\033[?25l"; vt_feed(vt,(const uint8_t *)hide,strlen(hide));
    const vt_cell_t *row=vt_view_row(vt,0);
    assert(row[1].cp==0x23f5 && row[2].cp==0x23f5 && row[3].cp==']' && row[5].cp=='A');
    lv_draw_buf_t *snap=lv_snapshot_take(term_view_obj(tv),LV_COLOR_FORMAT_RGB888); assert(snap);
    for(int y=4*32;y<6*32;y++) for(int x=0;x<5*12;x++) {
        uint8_t *p=snap->data+y*snap->header.stride+x*3;
        assert(p[0] || p[1] || p[2]); /* no seams in solid block artwork */
    }
    lv_draw_buf_destroy(snap);
    assert(snapshot(scr,"art.ppm")==0);
    term_view_destroy(tv); vt_destroy(vt);
    puts("CLI triangles are one cell; full-height block artwork has no gaps across columns or rows.");
}

/* ---------------------------------------------------------------- nav */
/* P4-2: a probe screen's create function runs after the previous screen is already deleted. */
static lv_obj_t *s_probe_prev;
static unsigned s_probe_runs;
static lv_obj_t *probe_settings_create(void *arg) {
    assert(s_probe_prev && !lv_obj_is_valid(s_probe_prev));
    s_probe_runs++;
    return ui_settings_create(arg);
}
static void noop_cb(lv_event_t *e) { (void)e; }
static bool s_boot_deleted;
static void boot_deleted_cb(lv_event_t *e) { (void)e; s_boot_deleted = true; }
/* Every Task 4 component on one screen, with stub sessions: the checks run on it with no skips. */
static lv_obj_t *probe_gallery_create(void *arg) {
    (void)arg;
    assert(s_probe_prev && !lv_obj_is_valid(s_probe_prev));
    s_probe_runs++;
    lv_obj_t *scr = ui_screen_base();
    lv_obj_t *h = ui_header(scr, "Components", "Sessions");
    ui_chip(ui_header_slot(h), "Save", noop_cb, NULL);
    ui_header_needs_you_set(ui_header_needs_you(h), 2);
    lv_obj_t *banner = ui_banner(scr, UI_TK_HEADER_H);
    ui_banner_set(banner, UI_BANNER_MUTED, "Updating\xe2\x80\xa6", NULL, NULL, NULL);
    ui_banner_set(banner, UI_BANNER_RED, "Can't reach the host: connection refused", "Host settings", noop_cb, NULL);
    lv_obj_t *f = ui_footer(scr);
    ui_chip(ui_footer_group(f, false), "\xe2\x86\x91\xe2\x86\x93 Select", noop_cb, NULL);
    ui_chip(ui_footer_group(f, false), "Send", noop_cb, NULL);
    ui_chip(ui_footer_group(f, true), "Settings", noop_cb, NULL);
    int top = UI_TK_HEADER_H + UI_TK_BANNER_H + UI_TK_GAP_M;
    int body_h = 720 - top - UI_TK_FOOTER_H - UI_TK_GAP_M;
    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, UI_TK_MARGIN, top);
    lv_obj_set_size(list, UI_TK_LIST_W, body_h);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, UI_TK_GAP_S, 0);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    static cc_session_t ss[4] = {
        { .id = "a1", .name = "auth-refactor-with-a-name-far-too-long-to-fit-in-the-row", .repo = "api",
          .state = CC_NEEDS_YOU, .background = true, .live = true },
        { .id = "b2", .name = "deps-bump", .repo = "web", .state = CC_NEEDS_YOU, .background = true, .live = true },
        { .id = "c3", .name = "flaky-tests", .repo = "api", .state = CC_WORKING, .background = true, .live = true },
        { .id = "d4", .name = "docs-pass", .repo = "docs", .state = CC_IDLE, .background = true, .live = true },
    };
    const char *meta[4] = { "~/code/api \xc2\xb7 7m", "~/code/web \xc2\xb7 12m", "~/code/api \xc2\xb7 1h", "~/code/docs \xc2\xb7 2d" };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *r = ui_session_row(list, noop_cb, NULL);
        ui_session_row_set(r, &ss[i], i == 0, meta[i]);
        ui_session_row_set(r, &ss[i], i == 0, meta[i]);   /* unchanged: a no-op */
    }
    ui_section_header(list, "Section header");
    ui_srow(list, UI_ROW_ACTION, "Reboot", NULL, noop_cb, NULL);
    ui_skeleton_row(list);
    lv_obj_t *col = lv_obj_create(scr);
    lv_obj_remove_style_all(col);
    int x = UI_TK_MARGIN + UI_TK_LIST_W + UI_TK_GAP_L;
    lv_obj_set_pos(col, x, top);
    lv_obj_set_size(col, 1280 - x - UI_TK_MARGIN, body_h);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, UI_TK_GAP_S, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *buttons = lv_obj_create(col);
    lv_obj_remove_style_all(buttons);
    lv_obj_set_size(buttons, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, UI_TK_GAP_M, 0);
    lv_obj_remove_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);
    ui_btn(buttons, UI_BTN_PRIMARY, "Send", noop_cb, NULL);
    ui_btn(buttons, UI_BTN_SECONDARY, "Open", noop_cb, NULL);
    ui_btn(buttons, UI_BTN_DESTRUCTIVE, "Forget", noop_cb, NULL);
    lv_obj_t *pill = ui_state_pill(buttons);
    ui_state_pill_set(pill, &ss[2]);
    ui_srow(col, UI_ROW_VALUE, "Fallback address", "192.168.1.50", noop_cb, NULL);
    ui_srow(col, UI_ROW_SWITCH, "Tailscale", "on", noop_cb, NULL);
    ui_srow(col, UI_ROW_NAV, "Host", "my-host.local", noop_cb, NULL);
    ui_srow_slider(col, "Brightness", 5, 100, 58, noop_cb, NULL);
    ui_srow(col, UI_ROW_INFO, "Link", "connected over home Wi-Fi", NULL, NULL);
    ui_footnote(col, "A footnote wraps onto as many lines as it needs, in the muted label role.");
    return scr;
}

static int nav_case(lv_display_t *d) {
    preview_sessions_load();
    ui_nav_home(); step(d,400);
    s_probe_prev = lv_screen_active();
    ui_nav_push(probe_settings_create, NULL); step(d,100);
    assert(s_probe_runs == 1 && ui_nav_is_top(probe_settings_create));
    ui_nav_back(); step(d,100);
    assert(ui_nav_is_top(ui_dashboard_create));
    s_probe_prev = lv_screen_active();
    ui_nav_push(probe_gallery_create, NULL); step(d,100);
    assert(s_probe_runs == 2);
    /* the toast: one line at the bottom centre */
    ui_toast("A toast is one line at the bottom centre, cut with dots when it is longer than the screen is wide, "
             "which this one is");
    step(d,100);
    int r = pv_run_checks(d, lv_screen_active(), "nav", true, 0);
    step(d,3100);
    assert(lv_obj_get_child_count(lv_layer_top()) == 0);   /* gone after 3 s */
    /* the picker and the confirm over it */
    const char *items[] = { "Lowest", "Low", "Medium" };
    ui_picker("Needs-you light when the screen is dark", items, 3, 1, NULL, NULL);
    step(d,100);
    r |= pv_run_checks(d, lv_screen_active(), "picker", false, 0);
    lv_obj_clean(lv_layer_top());
    ui_confirm_ex("Forget this host's key? The Tab5 trusts the next key the host offers, so only do this after the key changed.",
                  "Only do this if the host's SSH key changed.", "Forget", true, NULL, NULL);
    step(d,100);
    r |= pv_run_checks(d, lv_screen_active(), "confirm", false, 0);
    lv_obj_clean(lv_layer_top());
    return r;
}

int main(int argc,char **argv) {
    lv_init();
    int w=1280, h=720;
    lv_display_t *d=lv_display_create(w,h);
    void *buf=malloc(w*50*4);
    lv_display_set_buffers(d,buf,NULL,w*50*4,LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush);
    g_settings.brightness=58;
    g_settings.term_line_height=32;
    g_settings.kbd_dark_light=5;   /* PT24 default: "Low" */
    const char *name = argc>1 ? argv[1] : "";
    /* Exercise splash before font runtime copies or the application UI exist. */
    if (!strcmp(name,"boot")) {
        lv_font_glyph_dsc_t g;
        assert(lv_font_get_glyph_dsc(&font_px20,&g,0x2026,0) && !g.is_placeholder);   /* "Starting…" */
        ui_boot_show();
        lv_obj_t *boot=lv_screen_active();
        assert(lv_obj_get_child_count(boot)==1);
        flushed_pixels=0; step(d,1000);
        assert(flushed_pixels==0); /* static logo must not schedule redraws */
        int result=pv_run_checks(d,boot,"boot",false,0);
        lv_obj_add_event_cb(boot,boot_deleted_cb,LV_EVENT_DELETE,NULL);
        fonts_init(); ui_init(); ui_nav_home(); step(d,400);
        assert(s_boot_deleted && ui_nav_is_top(ui_dashboard_create));   /* P4-2: the splash is deleted */
        puts("Early boot logo and handoff checks passed.");
        return result;
    }
    fonts_init(); ui_init();
    g_settings.ts_enable = env_int("PREVIEW_TS", 2) != 0;
    if (!strcmp(name,"sleep")) {
        /* the pre-Hibernate notice: full screen, over whatever is shown */
        lv_obj_t *s=ui_screen_base(); lv_screen_load(s);
        ui_sleep_notice(true); step(d,400);
        return pv_run_checks(d,s,"sleep",false,0);
    }
    if (!strcmp(name,"nav")) return nav_case(d);
    if (!strcmp(name,"gesture")) { gesture_check(); return 0; }
    if (!strcmp(name,"art")) { terminal_art_check(); return 0; }
    if (!strcmp(name,"terminal")) { terminal_spacing_check(); return 0; }
    if (!strcmp(name,"keyboard")) { keyboard_check(); return 0; }
    if (!strcmp(name,"glyphs")) {
        /* Which of Claude Code's interface symbols the terminal font draws. */
        static const struct { uint32_t cp; const char *name; } g[] = {
            {0x2500,"box \xe2\x94\x80"},{0x256D,"box \xe2\x95\xad"},{0x273B,"\xe2\x9c\xbb"},{0x23FA,"\xe2\x8f\xba"},
            {0x23BF,"\xe2\x8e\xbf"},{0x2713,"\xe2\x9c\x93"},{0x25CF,"\xe2\x97\x8f"},{0x276F,"\xe2\x9d\xaf"},
            {0x2026,"\xe2\x80\xa6"},{0x00B7,"\xc2\xb7"},{0x2014,"\xe2\x80\x94"},{0x2192,"\xe2\x86\x92"},
            {0x2190,"\xe2\x86\x90"},{0x2191,"\xe2\x86\x91"},{0x2193,"\xe2\x86\x93"},{0x21B5,"\xe2\x86\xb5"},
            {0x2303,"\xe2\x8c\x83"},{0x203A,"\xe2\x80\xba"},
            {0x2722,"\xe2\x9c\xa2"},{0x2733,"\xe2\x9c\xb3"},{0x2736,"\xe2\x9c\xb6"},{0x273D,"\xe2\x9c\xbd"},{0x00B7,"spinner dot"} };
        const lv_font_t *fonts[] = { &g_term_font, FONT_BODY, FONT_SMALL, FONT_TITLE };
        const char *fn[] = { "terminal", "body", "small", "title" };
        for (unsigned f=0; f<4; f++) for (unsigned i=0;i<sizeof(g)/sizeof(g[0]);i++) {
            lv_font_glyph_dsc_t dsc;
            bool ok = lv_font_get_glyph_dsc(fonts[f],&dsc,g[i].cp,0) && !dsc.is_placeholder;
            printf("%-8s U+%04X %-12s %s\n", fn[f], (unsigned)g[i].cp, g[i].name, ok ? "yes" : "MISSING");
        }
        return 0;
    }
    if (!strcmp(name,"dashboard")) {
        /* The live dashboard against the synthetic fixture; the first card is selected.
           Not rebuilt until Task 6, so the amber scan and the target sizes are skipped. */
        preview_sessions_load();
        ui_nav_home(); step(d,400);
        assert(ui_nav_is_top(ui_dashboard_create));
        step(d,400);
        return pv_run_checks(d,lv_screen_active(),"dashboard",true,PV_SKIP_AMBER|PV_SKIP_TARGETS);
    }
    if (!strcmp(name,"dashboard-reconnecting")) {
        /* A key woke it from Radio off: "Reconnecting to Mac…" (muted) over the stale cards, not "No Wi-Fi". */
        preview_sessions_load();
        s_rc_preview = true;
        ui_nav_home(); step(d,400);
        assert(ui_nav_is_top(ui_dashboard_create));
        step(d,400);
        lv_obj_t *s=lv_screen_active();
        lv_obj_update_layout(s); lv_refr_now(d);
        assert(find_label(s,"Reconnecting to Mac\xe2\x80\xa6",false));
        assert(!find_label(s,"No Wi-Fi",true));
        return pv_run_checks(d,s,"dashboard-reconnecting",true,PV_SKIP_AMBER|PV_SKIP_TARGETS);
    }
    if (!strcmp(name,"settings-forget")) {
        /* Settings -> Companion -> Forget Mac host key: the confirm, then the toast after OK. */
        preview_sessions_load();
        ui_nav_home(); step(d,400);
        ui_nav_push(ui_settings_create,NULL); step(d,400);
        lv_obj_t *s=lv_screen_active();
        lv_obj_t *row=find_clickable(s,"Forget Mac host key");
        assert(row);
        lv_obj_send_event(row,LV_EVENT_CLICKED,NULL); step(d,400);
        lv_obj_update_layout(lv_layer_top()); lv_refr_now(d);
        assert(find_label(s,"Pinned key: SHA256:OldPreviewKey",true));   /* the row shows the pinned key */
        assert(find_label(lv_layer_top(),"Forget Mac host key?",false));
        assert(find_label(lv_layer_top(),"Only do this if your Mac's SSH host key changed. The Tab5 will trust the next key it sees.",false));
        int r = pv_run_checks(d,s,"settings-forget",false,PV_SKIP_AMBER|PV_SKIP_TARGETS);
        assert(s_forget_calls==0);   /* nothing happens before Yes */
        lv_obj_t *ok=find_clickable(lv_layer_top(),tr(STR_OK));
        assert(ok);
        lv_obj_send_event(ok,LV_EVENT_CLICKED,NULL); step(d,100); step(d,600);
        assert(s_forget_calls==1);
        assert(find_label(lv_layer_top(),"New Mac key pinned: " PREVIEW_FP,false));   /* from the poll */
        lv_obj_update_layout(lv_layer_top()); lv_refr_now(d);
        r |= snapshot_top(lv_screen_active(),"settings-forget-toast.ppm");
        puts("Settings forget host key: confirm, one request after OK, toast passed.");
        return r;
    }
    if (!strcmp(name,"dashboard-keys")) {
        /* The Ctrl+K shortcuts overlay over the dashboard, opened through the dashboard's own sink. */
        preview_sessions_load();
        ui_nav_home(); step(d,400);
        assert(ui_nav_is_top(ui_dashboard_create) && preview_have_sink);
        key_event_t k = { .type = KEY_EV_CHAR, .cp = 'k', .mods = MOD_CTRL };
        assert(preview_sink.key(&k, preview_sink.user));
        assert(ui_dashboard_keys_overlay_shown());
        step(d,400);
        int r = pv_run_checks(d,lv_screen_active(),"dashboard-keys",true,PV_SKIP_AMBER|PV_SKIP_TARGETS);
        /* Ctrl+Tab is not taken by buttons mode while the overlay is up; the sink closes it */
        key_event_t ct = { .type = KEY_EV_SPECIAL, .key = SK_TAB, .mods = MOD_CTRL };
        assert(!ui_keyboard_route(&ct, true) && !ui_keyboard_owns_input());
        key_event_t z = { .type = KEY_EV_CHAR, .cp = 'z' };
        assert(preview_sink.key(&z, preview_sink.user));
        assert(!ui_dashboard_keys_overlay_shown());
        step(d,100);
        assert(ui_nav_is_top(ui_dashboard_create));
        /* Ctrl+P and Ctrl+M navigate (Settings, Terminal); Esc comes back */
        key_event_t p = { .type = KEY_EV_CHAR, .cp = 'p', .mods = MOD_CTRL };
        assert(preview_sink.key(&p, preview_sink.user)); step(d,400);
        assert(ui_nav_is_top(ui_settings_create));
        ui_nav_back(); step(d,400);
        key_event_t m = { .type = KEY_EV_CHAR, .cp = 'm', .mods = MOD_CTRL };
        assert(preview_have_sink && preview_sink.key(&m, preview_sink.user)); step(d,400);
        assert(ui_nav_is_top(ui_hosts_create));
        puts("Dashboard hotkeys: Ctrl+K overlay, any key closes it, Ctrl+P settings, Ctrl+M terminal passed.");
        return r;
    }
    if (!strcmp(name,"settings")) {
        unsigned category = argc>2 ? (unsigned)atoi(argv[2]) : 0;
        assert(category < 7);
        preview_sessions_load();
        ui_nav_home(); step(d,200);
        ui_nav_push(ui_settings_create, NULL); step(d,200);
        lv_obj_t *s = lv_screen_active();
        /* Real click events through every rail entry: one visible panel, the same screen object. */
        for (unsigned i = 0; i < 7; i++) {
            lv_obj_t *before = s;
            lv_obj_t *nav = lv_obj_get_child(lv_obj_get_child(s, 1), 0);
            lv_obj_send_event(lv_obj_get_child(nav, i), LV_EVENT_CLICKED, NULL);
            step(d,200);
            s = lv_screen_active();
            assert(s == before); /* a rail click must not rebuild the screen */
            lv_obj_t *deck = lv_obj_get_child(lv_obj_get_child(s, 1), 1);
            for (unsigned j = 0; j < 7; j++)
                assert(lv_obj_has_flag(lv_obj_get_child(deck, j), LV_OBJ_FLAG_HIDDEN) == (j != i));
        }
        lv_obj_t *nav = lv_obj_get_child(lv_obj_get_child(s, 1), 0);
        lv_obj_send_event(lv_obj_get_child(nav, category), LV_EVENT_CLICKED, NULL);
        step(d,200);
        assert(lv_screen_active() == s);
        char n[16]; snprintf(n, sizeof(n), "settings-%u", category);
        return pv_run_checks(d, s, n, false, PV_SKIP_AMBER|PV_SKIP_TARGETS);
    }
    fprintf(stderr, "preview: unknown case %s\n", argc>1 ? argv[1] : "(none)");
    return 2;
}
