/* The shared components (spec §8.2) and their styles. Every style here is file-static and built
 * once by ui_comp_init(); a component adds them with lv_obj_add_style and keeps only geometry
 * (sizes, positions) as local styles. No style sets a shadow, a gradient or an opacity animation
 * (§11). Amber leaves this file only through the NEEDS YOU components. */
#include "ui_comp.h"
#include "ui_vitals.h"
#include "ui/sessions_model.h"
#include <stdio.h>
#include <string.h>

#define HEX(h) lv_color_hex(h)
#define BAR_HIT_PAD ((UI_TK_HIT - UI_TK_BAR_CTRL) / 2)   /* 16: 48 visible, 80 hit */
#define BODY_HIT_PAD ((UI_TK_HIT - UI_TK_TARGET) / 2)    /* 8: 64 visible, 80 hit */

static bool s_ready;
/* surfaces and text */
static lv_style_t s_bg, s_panel, s_panel2, s_line_border, s_fg_text, s_muted, s_label;
static lv_style_t s_focus;
/* controls */
static lv_style_t s_btn, s_btn_primary, s_btn_secondary, s_btn_destructive, s_pressed, s_chip;
static lv_style_t s_pill, s_pill_working;
static lv_style_t s_row, s_row_sel, s_session_geom, s_srow_geom, s_skeleton, s_set_col, s_srow_line;
/* bars */
static lv_style_t s_dot_on, s_dot_off, s_scrim, s_spinner_main, s_spinner_ind, s_pressed_destructive;
static lv_style_t s_green_text, s_red_text, s_mono, s_rail, s_swatch, s_chip_destructive;
static lv_style_t s_host_geom, s_host_line, s_host_dot, s_dot_green, s_dot_line, s_add_row;
static lv_style_t s_host_col, s_host_hint, s_host_fail, s_srow_wrap, s_wrap_label;
static lv_style_t s_title, s_bar, s_side_bottom, s_side_top, s_banner_muted, s_banner_red, s_section;
/* the LVGL widgets the screens create directly (theme layer) */
static lv_style_t s_w_button, s_w_field, s_w_track, s_w_on, s_w_knob_off, s_w_knob_on, s_w_knob;
/* NEEDS YOU: built on first use by the three components that may draw amber */
static bool s_amber_ready;
static lv_style_t s_amber_text, s_amber_pill, s_amber_edge, s_row_needs, s_row_needs_sel;

static lv_obj_t *plain(lv_obj_t *parent);
static lv_obj_t *text(lv_obj_t *parent, const char *t, lv_style_t *colour, lv_style_t *font);
static void pass_hits(lv_obj_t *o, int32_t pad);
static void tone_apply(lv_obj_t *l, ui_tone_t tone);

static void flex_center(lv_style_t *s)
{
    lv_style_set_layout(s, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(s, LV_FLEX_FLOW_ROW);
    lv_style_set_flex_main_place(s, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_cross_place(s, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(s, LV_FLEX_ALIGN_CENTER);
}

static void flex_row(lv_style_t *s, int32_t gap)
{
    lv_style_set_layout(s, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(s, LV_FLEX_FLOW_ROW);
    lv_style_set_flex_main_place(s, LV_FLEX_ALIGN_START);
    lv_style_set_flex_cross_place(s, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(s, LV_FLEX_ALIGN_CENTER);
    lv_style_set_pad_column(s, gap);
}

void ui_comp_init(void)
{
    if (s_ready) return;
    s_ready = true;
    lv_style_t *all[] = { &s_bg, &s_panel, &s_panel2, &s_line_border, &s_fg_text, &s_muted, &s_label, &s_focus,
                          &s_btn, &s_btn_primary, &s_btn_secondary, &s_btn_destructive, &s_pressed, &s_chip,
                          &s_pill, &s_pill_working, &s_row, &s_row_sel, &s_session_geom, &s_srow_geom, &s_skeleton, &s_set_col, &s_srow_line,
                          &s_dot_on, &s_dot_off, &s_scrim, &s_spinner_main, &s_spinner_ind, &s_pressed_destructive,
                          &s_title, &s_bar, &s_side_bottom, &s_side_top, &s_banner_muted, &s_banner_red, &s_section,
                          &s_w_button, &s_w_field, &s_w_track, &s_w_on, &s_w_knob_off, &s_w_knob_on, &s_w_knob,
                          &s_green_text, &s_red_text, &s_mono, &s_rail, &s_swatch, &s_chip_destructive,
                          &s_host_geom, &s_host_line, &s_host_dot, &s_dot_green, &s_dot_line, &s_add_row,
                          &s_host_col, &s_host_hint, &s_host_fail, &s_srow_wrap, &s_wrap_label };
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) lv_style_init(all[i]);

    lv_style_set_bg_color(&s_bg, HEX(UI_TK_BG));
    lv_style_set_bg_opa(&s_bg, LV_OPA_COVER);
    lv_style_set_bg_color(&s_panel, HEX(UI_TK_PANEL));
    lv_style_set_bg_opa(&s_panel, LV_OPA_COVER);
    lv_style_set_bg_color(&s_panel2, HEX(UI_TK_PANEL2));
    lv_style_set_bg_opa(&s_panel2, LV_OPA_COVER);
    lv_style_set_border_color(&s_line_border, HEX(UI_TK_LINE));
    lv_style_set_border_width(&s_line_border, 1);
    lv_style_set_border_opa(&s_line_border, LV_OPA_COVER);
    lv_style_set_text_color(&s_fg_text, HEX(UI_TK_FG));
    lv_style_set_text_color(&s_muted, HEX(UI_TK_MUTED));
    lv_style_set_text_font(&s_label, FONT_SMALL);
    /* focus: a 2 px fg ring with a 2 px gap, never a hue */
    lv_style_set_outline_width(&s_focus, 2);
    lv_style_set_outline_pad(&s_focus, 2);
    lv_style_set_outline_color(&s_focus, HEX(UI_TK_FG));
    lv_style_set_outline_opa(&s_focus, LV_OPA_COVER);

    /* buttons: 64 px, radius 12 */
    lv_style_set_height(&s_btn, UI_TK_TARGET);
    lv_style_set_min_width(&s_btn, UI_TK_TARGET);
    lv_style_set_radius(&s_btn, UI_TK_RADIUS);
    lv_style_set_pad_hor(&s_btn, UI_TK_GAP_L);
    lv_style_set_text_font(&s_btn, FONT_BODY);
    flex_center(&s_btn);
    lv_style_set_pad_column(&s_btn, UI_TK_GAP_S);
    lv_style_set_bg_color(&s_btn_primary, HEX(UI_TK_FG));
    lv_style_set_bg_opa(&s_btn_primary, LV_OPA_COVER);
    lv_style_set_text_color(&s_btn_primary, HEX(UI_TK_BG));
    lv_style_set_bg_color(&s_btn_secondary, HEX(UI_TK_PANEL2));
    lv_style_set_bg_opa(&s_btn_secondary, LV_OPA_COVER);
    lv_style_set_border_color(&s_btn_secondary, HEX(UI_TK_LINE));
    lv_style_set_border_width(&s_btn_secondary, 1);
    lv_style_set_text_color(&s_btn_secondary, HEX(UI_TK_FG));
    lv_style_set_bg_opa(&s_btn_destructive, LV_OPA_TRANSP);
    lv_style_set_border_color(&s_btn_destructive, HEX(UI_TK_RED));
    lv_style_set_border_width(&s_btn_destructive, 1);
    lv_style_set_text_color(&s_btn_destructive, HEX(UI_TK_RED));
    lv_style_set_bg_color(&s_pressed, HEX(UI_TK_LINE));
    lv_style_set_bg_opa(&s_pressed, LV_OPA_COVER);
    lv_style_set_text_color(&s_pressed, HEX(UI_TK_FG));
    /* chip: 48 px, panel-2, radius 8 */
    lv_style_set_height(&s_chip, UI_TK_BAR_CTRL);
    lv_style_set_min_width(&s_chip, UI_TK_BAR_CTRL);
    lv_style_set_radius(&s_chip, UI_TK_RADIUS_CHIP);
    lv_style_set_pad_hor(&s_chip, UI_TK_GAP_M);
    lv_style_set_bg_color(&s_chip, HEX(UI_TK_PANEL2));
    lv_style_set_bg_opa(&s_chip, LV_OPA_COVER);
    lv_style_set_text_color(&s_chip, HEX(UI_TK_FG));
    lv_style_set_text_font(&s_chip, FONT_SMALL);
    flex_center(&s_chip);
    /* pill: radius 8, the label role, muted on line */
    lv_style_set_radius(&s_pill, UI_TK_RADIUS_CHIP);
    lv_style_set_pad_hor(&s_pill, 10);
    lv_style_set_pad_ver(&s_pill, 2);
    lv_style_set_border_width(&s_pill, 1);
    lv_style_set_border_color(&s_pill, HEX(UI_TK_LINE));
    lv_style_set_text_color(&s_pill, HEX(UI_TK_MUTED));
    lv_style_set_text_font(&s_pill, FONT_SMALL);
    lv_style_set_border_color(&s_pill_working, HEX(UI_TK_GREEN));
    lv_style_set_text_color(&s_pill_working, HEX(UI_TK_GREEN));

    /* rows: panel, radius 12, a line border; selected is panel-2 with a 50 % fg outline */
    lv_style_set_bg_color(&s_row, HEX(UI_TK_PANEL));
    lv_style_set_bg_opa(&s_row, LV_OPA_COVER);
    lv_style_set_radius(&s_row, UI_TK_RADIUS);
    lv_style_set_border_color(&s_row, HEX(UI_TK_LINE));
    lv_style_set_border_width(&s_row, 1);
    lv_style_set_text_color(&s_row, HEX(UI_TK_FG));
    lv_style_set_text_font(&s_row, FONT_BODY);
    lv_style_set_bg_color(&s_row_sel, HEX(UI_TK_PANEL2));
    lv_style_set_outline_width(&s_row_sel, 2);
    lv_style_set_outline_pad(&s_row_sel, 0);
    lv_style_set_outline_color(&s_row_sel, HEX(UI_TK_FG));
    lv_style_set_outline_opa(&s_row_sel, LV_OPA_50);
    lv_style_set_bg_opa(&s_row_sel, LV_OPA_COVER);   /* a rail row is transparent until selected */
    lv_style_set_height(&s_session_geom, UI_TK_SESSION_ROW_H);
    lv_style_set_pad_left(&s_session_geom, 0);
    lv_style_set_pad_right(&s_session_geom, UI_TK_GAP_M);
    flex_row(&s_session_geom, 12);
    lv_style_set_height(&s_srow_geom, UI_TK_ROW_H);
    lv_style_set_border_width(&s_srow_geom, 0);
    lv_style_set_pad_hor(&s_srow_geom, 20);
    flex_row(&s_srow_geom, UI_TK_GAP_M);
    /* a Settings column: the panel and the Network sub-pages */
    lv_style_set_pad_ver(&s_set_col, UI_TK_GAP_L);
    lv_style_set_pad_hor(&s_set_col, 32);
    lv_style_set_pad_row(&s_set_col, UI_TK_GAP_S);
    lv_style_set_layout(&s_set_col, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&s_set_col, LV_FLEX_FLOW_COLUMN);
    /* a row and its trailing button */
    lv_style_set_width(&s_srow_line, LV_PCT(100));
    lv_style_set_height(&s_srow_line, UI_TK_ROW_H);
    flex_row(&s_srow_line, UI_TK_GAP_S);
    /* Terminal's host rows: 72 px, a 12 px dot, and the line that holds a row and its Edit button */
    lv_style_set_height(&s_host_geom, UI_TK_SESSION_ROW_H);
    lv_style_set_pad_hor(&s_host_geom, 20);
    flex_row(&s_host_geom, UI_TK_GAP_M);
    lv_style_set_width(&s_host_line, LV_PCT(100));
    lv_style_set_height(&s_host_line, UI_TK_SESSION_ROW_H);
    flex_row(&s_host_line, UI_TK_GAP_S);
    lv_style_set_width(&s_host_dot, 12);
    lv_style_set_height(&s_host_dot, 12);
    lv_style_set_radius(&s_host_dot, LV_RADIUS_CIRCLE);
    lv_style_set_bg_opa(&s_host_dot, LV_OPA_COVER);
    lv_style_set_bg_color(&s_dot_green, HEX(UI_TK_GREEN));
    lv_style_set_bg_color(&s_dot_line, HEX(UI_TK_LINE));
    lv_style_set_layout(&s_host_col, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&s_host_col, LV_FLEX_FLOW_COLUMN);
    lv_style_set_pad_row(&s_host_col, 4);
    /* the trailing label: the one-line focus hint, or a red failure of two lines at most */
    lv_style_set_width(&s_host_hint, LV_SIZE_CONTENT);
    lv_style_set_max_width(&s_host_hint, LV_PCT(60));
    lv_style_set_height(&s_host_hint, lv_font_get_line_height(FONT_SMALL));
    lv_style_set_width(&s_host_fail, LV_PCT(60));
    lv_style_set_height(&s_host_fail, LV_SIZE_CONTENT);
    lv_style_set_max_height(&s_host_fail, 2 * lv_font_get_line_height(FONT_SMALL));
    lv_style_set_text_align(&s_host_fail, LV_TEXT_ALIGN_RIGHT);
    /* a row whose label wraps (a fingerprint): at least 64 px, 8 px above and below */
    lv_style_set_min_height(&s_srow_wrap, UI_TK_ROW_H);
    lv_style_set_pad_ver(&s_srow_wrap, UI_TK_GAP_S);
    lv_style_set_height(&s_srow_wrap, LV_SIZE_CONTENT);
    lv_style_set_height(&s_wrap_label, LV_SIZE_CONTENT);
    /* + Add host: 72 px, a dashed-look line border (LVGL draws no dashed borders), muted */
    lv_style_set_height(&s_add_row, UI_TK_SESSION_ROW_H);
    lv_style_set_radius(&s_add_row, UI_TK_RADIUS);
    lv_style_set_border_color(&s_add_row, HEX(UI_TK_LINE));
    lv_style_set_border_width(&s_add_row, 1);
    lv_style_set_pad_hor(&s_add_row, 20);
    lv_style_set_text_color(&s_add_row, HEX(UI_TK_MUTED));
    lv_style_set_text_font(&s_add_row, FONT_BODY);
    flex_row(&s_add_row, UI_TK_GAP_M);
    lv_style_set_height(&s_skeleton, UI_TK_SESSION_ROW_H);
    lv_style_set_radius(&s_skeleton, UI_TK_RADIUS);

    lv_style_set_text_font(&s_title, FONT_TITLE);
    lv_style_set_text_color(&s_green_text, HEX(UI_TK_GREEN));
    lv_style_set_text_color(&s_red_text, HEX(UI_TK_RED));
    lv_style_set_text_font(&s_mono, FONT_MONO);
    /* the Settings rail: 72 px rows, radius 12, transparent until selected */
    lv_style_set_height(&s_rail, UI_TK_RAIL_ROW_H);
    lv_style_set_radius(&s_rail, UI_TK_RADIUS);
    lv_style_set_pad_hor(&s_rail, 20);
    lv_style_set_text_color(&s_rail, HEX(UI_TK_FG));
    lv_style_set_text_font(&s_rail, FONT_BODY);
    flex_row(&s_rail, UI_TK_GAP_M);
    lv_style_set_width(&s_swatch, 24);
    lv_style_set_height(&s_swatch, 24);
    lv_style_set_radius(&s_swatch, LV_RADIUS_CIRCLE);
    lv_style_set_bg_opa(&s_swatch, LV_OPA_COVER);
    lv_style_set_bg_opa(&s_chip_destructive, LV_OPA_TRANSP);
    lv_style_set_border_color(&s_chip_destructive, HEX(UI_TK_RED));
    lv_style_set_border_width(&s_chip_destructive, 1);
    lv_style_set_text_color(&s_chip_destructive, HEX(UI_TK_RED));
    /* dialogs: the radio dots, the scrim, the busy spinner; destructive keeps its red when pressed */
    lv_style_set_width(&s_dot_on, 20);
    lv_style_set_height(&s_dot_on, 20);
    lv_style_set_radius(&s_dot_on, LV_RADIUS_CIRCLE);
    lv_style_set_bg_color(&s_dot_on, HEX(UI_TK_FG));
    lv_style_set_bg_opa(&s_dot_on, LV_OPA_COVER);
    lv_style_set_width(&s_dot_off, 20);
    lv_style_set_height(&s_dot_off, 20);
    lv_style_set_radius(&s_dot_off, LV_RADIUS_CIRCLE);
    lv_style_set_border_color(&s_dot_off, HEX(UI_TK_LINE));
    lv_style_set_border_width(&s_dot_off, 2);
    lv_style_set_bg_color(&s_scrim, HEX(UI_TK_BG));
    lv_style_set_bg_opa(&s_scrim, LV_OPA_70);
    lv_style_set_arc_color(&s_spinner_main, HEX(UI_TK_LINE));
    lv_style_set_arc_width(&s_spinner_main, 4);
    lv_style_set_arc_color(&s_spinner_ind, HEX(UI_TK_FG));
    lv_style_set_arc_width(&s_spinner_ind, 4);
    lv_style_set_bg_color(&s_pressed_destructive, HEX(UI_TK_RED));
    lv_style_set_bg_opa(&s_pressed_destructive, UI_TK_RED_BG_OPA);
    lv_style_set_text_color(&s_pressed_destructive, HEX(UI_TK_RED));
    /* bars: panel with a hairline, 24 px margins */
    lv_style_set_bg_color(&s_bar, HEX(UI_TK_PANEL));
    lv_style_set_bg_opa(&s_bar, LV_OPA_COVER);
    lv_style_set_border_color(&s_bar, HEX(UI_TK_LINE));
    lv_style_set_border_width(&s_bar, 1);
    lv_style_set_pad_hor(&s_bar, UI_TK_MARGIN);
    lv_style_set_text_color(&s_bar, HEX(UI_TK_FG));
    flex_row(&s_bar, UI_TK_GAP_M);
    lv_style_set_border_side(&s_side_bottom, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_side(&s_side_top, LV_BORDER_SIDE_TOP);
    lv_style_set_bg_color(&s_banner_muted, HEX(UI_TK_PANEL));
    lv_style_set_bg_opa(&s_banner_muted, LV_OPA_COVER);
    lv_style_set_text_color(&s_banner_muted, HEX(UI_TK_MUTED));
    lv_style_set_bg_color(&s_banner_red, HEX(UI_TK_RED));
    lv_style_set_bg_opa(&s_banner_red, UI_TK_RED_BG_OPA);
    lv_style_set_text_color(&s_banner_red, HEX(UI_TK_RED));
    lv_style_set_text_font(&s_section, FONT_SMALL);
    lv_style_set_text_color(&s_section, HEX(UI_TK_MUTED));
    lv_style_set_pad_top(&s_section, UI_TK_GAP_L);
    lv_style_set_pad_bottom(&s_section, UI_TK_GAP_S);

    /* the theme layer for widgets created directly: no shadow, the tokens (the default theme's
     * transitions are off in sdkconfig: CONFIG_LV_THEME_DEFAULT_TRANSITION_TIME=0) */
    lv_style_set_bg_color(&s_w_button, HEX(UI_TK_PANEL2));
    lv_style_set_bg_opa(&s_w_button, LV_OPA_COVER);
    lv_style_set_border_color(&s_w_button, HEX(UI_TK_LINE));
    lv_style_set_border_width(&s_w_button, 1);
    lv_style_set_radius(&s_w_button, UI_TK_RADIUS);
    lv_style_set_text_color(&s_w_button, HEX(UI_TK_FG));
    lv_style_set_shadow_width(&s_w_button, 0);
    lv_style_set_bg_color(&s_w_field, HEX(UI_TK_PANEL2));
    lv_style_set_bg_opa(&s_w_field, LV_OPA_COVER);
    lv_style_set_border_color(&s_w_field, HEX(UI_TK_LINE));
    lv_style_set_border_width(&s_w_field, 1);
    lv_style_set_radius(&s_w_field, UI_TK_RADIUS);
    lv_style_set_text_color(&s_w_field, HEX(UI_TK_FG));
    lv_style_set_bg_color(&s_w_track, HEX(UI_TK_LINE));
    lv_style_set_bg_opa(&s_w_track, LV_OPA_COVER);
    lv_style_set_radius(&s_w_track, LV_RADIUS_CIRCLE);
    lv_style_set_bg_color(&s_w_on, HEX(UI_TK_FG));
    lv_style_set_bg_opa(&s_w_on, LV_OPA_COVER);
    lv_style_set_radius(&s_w_on, LV_RADIUS_CIRCLE);
    lv_style_set_bg_color(&s_w_knob_off, HEX(UI_TK_MUTED));
    lv_style_set_bg_opa(&s_w_knob_off, LV_OPA_COVER);
    lv_style_set_radius(&s_w_knob_off, LV_RADIUS_CIRCLE);
    lv_style_set_shadow_width(&s_w_knob_off, 0);
    lv_style_set_bg_color(&s_w_knob_on, HEX(UI_TK_BG));
    lv_style_set_bg_color(&s_w_knob, HEX(UI_TK_FG));
    lv_style_set_bg_opa(&s_w_knob, LV_OPA_COVER);
    lv_style_set_radius(&s_w_knob, LV_RADIUS_CIRCLE);
    lv_style_set_shadow_width(&s_w_knob, 0);
}

static void widget_apply(lv_theme_t *theme, lv_obj_t *obj)
{
    (void)theme;
    if (lv_obj_check_type(obj, &lv_button_class)) {
        lv_obj_add_style(obj, &s_w_button, LV_PART_MAIN);
        lv_obj_add_style(obj, &s_pressed, LV_STATE_PRESSED);
    } else if (lv_obj_check_type(obj, &lv_textarea_class)) {
        lv_obj_add_style(obj, &s_w_field, LV_PART_MAIN);
    } else if (lv_obj_check_type(obj, &lv_switch_class)) {
        lv_obj_add_style(obj, &s_w_track, LV_PART_MAIN);
        lv_obj_add_style(obj, &s_w_track, LV_PART_INDICATOR);
        lv_obj_add_style(obj, &s_w_on, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_style(obj, &s_w_knob_off, LV_PART_KNOB);
        lv_obj_add_style(obj, &s_w_knob_on, LV_PART_KNOB | LV_STATE_CHECKED);
    } else if (lv_obj_check_type(obj, &lv_slider_class) || lv_obj_check_type(obj, &lv_bar_class)) {
        lv_obj_add_style(obj, &s_w_track, LV_PART_MAIN);
        lv_obj_add_style(obj, &s_w_on, LV_PART_INDICATOR);
        lv_obj_add_style(obj, &s_w_knob, LV_PART_KNOB);
    }
}

void ui_comp_install_theme(lv_theme_t *base)
{
    static lv_theme_t *s_theme;
    if (!s_theme) s_theme = lv_theme_create();
    if (!s_theme) { lv_display_set_theme(NULL, base); return; }
    lv_theme_copy(s_theme, base);
    lv_theme_set_parent(s_theme, base);
    lv_theme_set_apply_cb(s_theme, widget_apply);
    lv_display_set_theme(NULL, s_theme);
}

void ui_comp_focus_ring(lv_obj_t *obj, bool on)
{
    lv_obj_remove_style(obj, &s_focus, LV_STATE_FOCUS_KEY);
    if (on) lv_obj_add_style(obj, &s_focus, LV_STATE_FOCUS_KEY);
}

void ui_comp_screen(lv_obj_t *scr)
{
    lv_obj_add_style(scr, &s_bg, 0);
    lv_obj_add_style(scr, &s_fg_text, 0);
}

void ui_comp_sheet(lv_obj_t *obj)
{
    lv_obj_add_style(obj, &s_panel, 0);
    lv_obj_add_style(obj, &s_line_border, 0);
    lv_obj_add_style(obj, &s_fg_text, 0);
}

lv_obj_t *ui_comp_radio_dot(lv_obj_t *parent, bool on)
{
    lv_obj_t *d = plain(parent);
    lv_obj_add_style(d, on ? &s_dot_on : &s_dot_off, 0);
    return d;
}

void ui_comp_scrim(lv_obj_t *ov) { lv_obj_add_style(ov, &s_scrim, 0); }

void ui_comp_busy(lv_obj_t *spinner, lv_obj_t *label)
{
    lv_obj_add_style(spinner, &s_spinner_main, LV_PART_MAIN);
    lv_obj_add_style(spinner, &s_spinner_ind, LV_PART_INDICATOR);
    lv_obj_add_style(label, &s_muted, 0);
}

lv_obj_t *ui_comp_body(lv_obj_t *parent, const char *t)
{
    lv_obj_t *l = text(parent, t, &s_muted, NULL);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

void ui_comp_pass_hits(lv_obj_t *o) { pass_hits(o, BODY_HIT_PAD); }

lv_color_t ui_needs_you_color(void) { return HEX(ui_tk_needs_you_hex()); }

/* Built from the colour the caller passes, so amber reaches the styles only from the NEEDS YOU setters. */
static void amber_styles(lv_color_t amber)
{
    if (s_amber_ready) return;
    s_amber_ready = true;
    lv_style_t *all[] = { &s_amber_text, &s_amber_pill, &s_amber_edge, &s_row_needs, &s_row_needs_sel };
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) lv_style_init(all[i]);
    lv_style_set_text_color(&s_amber_text, amber);
    lv_style_set_bg_color(&s_amber_pill, amber);
    lv_style_set_bg_opa(&s_amber_pill, LV_OPA_COVER);
    lv_style_set_border_color(&s_amber_pill, amber);
    lv_style_set_text_color(&s_amber_pill, HEX(UI_TK_BG));
    lv_style_set_bg_color(&s_amber_edge, amber);
    lv_style_set_bg_opa(&s_amber_edge, LV_OPA_COVER);
    lv_style_set_border_color(&s_row_needs, amber);
    lv_style_set_border_opa(&s_row_needs, LV_OPA_60);
    lv_style_set_border_color(&s_row_needs_sel, amber);
    lv_style_set_border_opa(&s_row_needs_sel, LV_OPA_COVER);
}

static void set_flag(lv_obj_t *o, lv_obj_flag_t f, bool on)
{
    if (on) lv_obj_add_flag(o, f);
    else lv_obj_remove_flag(o, f);
}

static void set_text(lv_obj_t *l, const char *t)
{
    if (!t) t = "";
    if (strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

/* A container with no styles, not clickable or scrollable. */
static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const char *t, lv_style_t *colour, lv_style_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t ? t : "");
    if (colour) lv_obj_add_style(l, colour, 0);
    if (font) lv_obj_add_style(l, font, 0);
    return l;
}

/* ------------------------------------------------------------- buttons */
lv_obj_t *ui_btn(lv_obj_t *parent, ui_btn_kind_t kind, const char *t, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, &s_btn, 0);
    lv_obj_add_style(b, kind == UI_BTN_PRIMARY ? &s_btn_primary : kind == UI_BTN_DESTRUCTIVE ? &s_btn_destructive
                                                                                          : &s_btn_secondary, 0);
    lv_obj_add_style(b, kind == UI_BTN_DESTRUCTIVE ? &s_pressed_destructive : &s_pressed, LV_STATE_PRESSED);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(b, BODY_HIT_PAD);
    lv_label_set_text(lv_label_create(b), t ? t : "");
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

lv_obj_t *ui_chip(lv_obj_t *parent, const char *t, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = lv_button_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_add_style(c, &s_chip, 0);
    lv_obj_add_style(c, &s_pressed, LV_STATE_PRESSED);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, UI_BAR_CONTROL);
    lv_obj_set_ext_click_area(c, BAR_HIT_PAD);
    lv_label_set_text(lv_label_create(c), t ? t : "");
    if (cb) lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
    return c;
}

void ui_chip_set_primary(lv_obj_t *chip)
{
    if (!chip) return;
    lv_obj_add_style(chip, &s_btn_primary, 0);
}

/* ---------------------------------------------------------------- bars */
static void back_cb(lv_event_t *e) { (void)e; ui_nav_back(); }

static void hit_pad_cb(lv_event_t *e) { lv_event_set_ext_draw_size(e, (int32_t)(intptr_t)lv_event_get_user_data(e)); }

/* A container must not clip its controls' extended hit area: LVGL only searches a parent's children within
 * the parent's coords, grown by its ext draw size when it is OVERFLOW_VISIBLE. Used on the groups inside a bar
 * (the bar itself still clips, so a chip's hit area is the bar's full height, P4C-5) and on a slider's row. */
static void pass_hits(lv_obj_t *o, int32_t pad)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(o, hit_pad_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, (void *)(intptr_t)pad);
    lv_obj_refresh_ext_draw_size(o);
}

static lv_obj_t *bar(lv_obj_t *scr, int32_t h, lv_style_t *side)
{
    lv_obj_t *b = plain(scr);
    lv_obj_add_style(b, &s_bar, 0);
    lv_obj_add_style(b, side, 0);
    lv_obj_set_size(b, LV_PCT(100), h);
    return b;
}

lv_obj_t *ui_header(lv_obj_t *scr, const char *title, const char *back_label)
{
    lv_obj_t *h = bar(scr, UI_TK_HEADER_H, &s_side_bottom);
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 0);
    if (back_label) {
        char b[64];
        snprintf(b, sizeof(b), "\xe2\x86\x90 %s", back_label);   /* ← */
        ui_chip(h, b, back_cb, NULL);
    }
    text(h, title, NULL, &s_title);
    lv_obj_t *slot = plain(h);
    lv_obj_set_height(slot, LV_PCT(100));
    lv_obj_set_flex_grow(slot, 1);
    lv_obj_set_flex_flow(slot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(slot, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(slot, UI_TK_GAP_S, 0);
    pass_hits(slot, BAR_HIT_PAD);
    lv_obj_set_user_data(h, slot);
    ui_vitals_create(h);   /* filled at create */
    return h;
}

lv_obj_t *ui_header_slot(lv_obj_t *header) { return header ? lv_obj_get_user_data(header) : NULL; }

lv_obj_t *ui_header_needs_you(lv_obj_t *header)
{
    lv_obj_t *l = text(header, "", NULL, &s_label);
    lv_obj_move_to_index(l, (int32_t)lv_obj_get_index(lv_obj_get_user_data(header)));   /* after the title */
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

void ui_header_needs_you_set(lv_obj_t *label, int n)
{
    if (!label) return;
    if (n <= 0) { if (!lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN); return; }
    if (!lv_obj_has_flag(label, UI_NEEDS_YOU_PART)) {
        amber_styles(ui_needs_you_color());
        lv_obj_add_style(label, &s_amber_text, 0);
        lv_obj_add_flag(label, UI_NEEDS_YOU_PART);
    }
    char b[32];
    if (n == 1) snprintf(b, sizeof(b), "\xe2\x97\x8f 1 needs you");   /* ● */
    else snprintf(b, sizeof(b), "\xe2\x97\x8f %d need you", n);
    set_text(label, b);
    if (lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);   /* a flag write redraws */
}

/* banner children: 0 the text, 1 the action chip (made on first use); user data: the kind + 1 */
lv_obj_t *ui_banner(lv_obj_t *scr, int32_t y)
{
    lv_obj_t *b = plain(scr);
    lv_obj_add_style(b, &s_label, 0);
    lv_obj_set_size(b, LV_PCT(100), UI_TK_BANNER_H);
    lv_obj_set_pos(b, 0, y);
    lv_obj_set_style_pad_hor(b, UI_TK_MARGIN, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, UI_TK_GAP_M, 0);
    lv_obj_t *t = text(b, "", NULL, NULL);
    lv_obj_set_width(t, 1);
    lv_obj_set_flex_grow(t, 1);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(t, lv_font_get_line_height(FONT_SMALL));
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    return b;
}

void ui_banner_set(lv_obj_t *banner, ui_banner_kind_t kind, const char *t, const char *action,
                   lv_event_cb_t cb, void *ud)
{
    if (!banner) return;
    if (!t) { if (!lv_obj_has_flag(banner, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN); return; }
    uintptr_t want = (uintptr_t)kind + 1;
    if ((uintptr_t)lv_obj_get_user_data(banner) != want) {
        lv_obj_remove_style(banner, &s_banner_muted, 0);
        lv_obj_remove_style(banner, &s_banner_red, 0);
        lv_obj_add_style(banner, kind == UI_BANNER_RED ? &s_banner_red : &s_banner_muted, 0);
        lv_obj_set_user_data(banner, (void *)want);
    }
    set_text(lv_obj_get_child(banner, 0), t);
    lv_obj_t *chip = lv_obj_get_child(banner, 1);
    if (action) {
        if (!chip) chip = ui_chip(banner, action, NULL, NULL);
        set_text(lv_obj_get_child(chip, 0), action);
        lv_event_dsc_t *d = lv_obj_get_event_count(chip) ? lv_obj_get_event_dsc(chip, 0) : NULL;
        if (!d || lv_event_dsc_get_cb(d) != cb || lv_event_dsc_get_user_data(d) != ud) {
            while (lv_obj_get_event_count(chip)) lv_obj_remove_event(chip, 0);
            if (cb) lv_obj_add_event_cb(chip, cb, LV_EVENT_CLICKED, ud);
        }
        if (lv_obj_has_flag(chip, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(chip, LV_OBJ_FLAG_HIDDEN);
    } else if (chip && !lv_obj_has_flag(chip, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(chip, LV_OBJ_FLAG_HIDDEN);
    }
    if (lv_obj_has_flag(banner, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(banner, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *ui_footer(lv_obj_t *scr)
{
    lv_obj_t *f = bar(scr, UI_TK_FOOTER_H, &s_side_top);
    lv_obj_align(f, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_align(f, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *g = plain(f);
        lv_obj_set_size(g, LV_SIZE_CONTENT, LV_PCT(100));
        lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(g, 12, 0);
        pass_hits(g, BAR_HIT_PAD);
    }
    return f;
}

lv_obj_t *ui_footer_group(lv_obj_t *footer, bool right) { return lv_obj_get_child(footer, right ? 1 : 0); }

/* ------------------------------------------------------------ sessions */
enum { PILL_MUTED = 1, PILL_WORKING, PILL_NEEDS };

lv_obj_t *ui_state_pill(lv_obj_t *parent)
{
    lv_obj_t *p = text(parent, "", NULL, NULL);
    lv_obj_add_style(p, &s_pill, 0);
    lv_obj_set_user_data(p, (void *)(uintptr_t)PILL_MUTED);
    return p;
}

void ui_state_pill_set(lv_obj_t *pill, const cc_session_t *s)
{
    if (!pill || !s) return;
    bool needs = cc_session_needs_you(s);
    uintptr_t want = needs ? PILL_NEEDS : s->state == CC_WORKING ? PILL_WORKING : PILL_MUTED;
    if ((uintptr_t)lv_obj_get_user_data(pill) != want) {
        lv_obj_remove_style(pill, &s_pill_working, 0);
        if (s_amber_ready) lv_obj_remove_style(pill, &s_amber_pill, 0);
        if (want == PILL_WORKING) lv_obj_add_style(pill, &s_pill_working, 0);
        if (want == PILL_NEEDS) {
            amber_styles(ui_needs_you_color());
            lv_obj_add_style(pill, &s_amber_pill, 0);
        }
        set_flag(pill, UI_NEEDS_YOU_PART, needs);
        lv_obj_set_user_data(pill, (void *)want);
    }
    set_text(pill, sm_pill_text(s));
}

/* session row children: 0 the amber edge, 1 the column (name, meta), 2 the pill.
 * Row user data: 1 | needs << 1 | selected << 2. The name label's user data: a hash of the full name,
 * the pill text (the pill's width sets the name's) and the name's laid-out width, so a row that
 * changes width is cut again (T4 M12); 0 until the name has been fitted. */
#define ROW_SET 1u
#define ROW_NEEDS 2u
#define ROW_SEL 4u

static uint32_t name_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h ? h : 1;
}

/* Sets t, cut to the label's width with "…" if it is wider. Returns false if the width isn't known yet. */
static bool set_fitted(lv_obj_t *l, const char *t)
{
    int32_t w = lv_obj_get_content_width(l);
    const lv_font_t *f = lv_obj_get_style_text_font(l, 0);
    lv_point_t sz;
    lv_text_get_size(&sz, t, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x <= w || w <= 0) { set_text(l, t); return w > 0; }
    char buf[sizeof(((cc_session_t *)0)->name) + 4];
    size_t n = strlen(t);
    if (n > sizeof(buf) - 4) n = sizeof(buf) - 4;
    size_t lo = 0, hi = n;   /* the longest prefix (on a UTF-8 boundary) that fits with the ellipsis */
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        while (mid > lo && (t[mid] & 0xC0) == 0x80) mid--;
        if (mid == lo) break;
        memcpy(buf, t, mid);
        memcpy(buf + mid, "\xe2\x80\xa6", 4);
        lv_text_get_size(&sz, buf, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= w) lo = mid;
        else hi = mid - 1;
    }
    while (lo > 0 && (t[lo] & 0xC0) == 0x80) lo--;
    memcpy(buf, t, lo);
    memcpy(buf + lo, "\xe2\x80\xa6", 4);
    set_text(l, buf);
    return true;
}

lv_obj_t *ui_session_row(lv_obj_t *parent, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_add_style(r, &s_row, 0);
    lv_obj_add_style(r, &s_session_geom, 0);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(r, (UI_TK_HIT - UI_TK_SESSION_ROW_H) / 2);
    if (cb) lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *edge = plain(r);
    lv_obj_set_size(edge, 4, UI_TK_SESSION_ROW_H - 2 * UI_TK_RADIUS);
    lv_obj_set_style_radius(edge, 2, 0);
    lv_obj_t *col = plain(r);
    lv_obj_set_size(col, 1, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_t *name = text(col, "", NULL, NULL);
    lv_obj_set_size(name, LV_PCT(100), lv_font_get_line_height(FONT_BODY));
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_t *meta = text(col, "", &s_muted, &s_label);
    lv_obj_set_size(meta, LV_PCT(100), lv_font_get_line_height(FONT_SMALL));
    lv_label_set_long_mode(meta, LV_LABEL_LONG_MODE_DOTS);
    ui_state_pill(r);
    return r;
}

void ui_session_row_set(lv_obj_t *row, const cc_session_t *s, bool selected, const char *meta)
{
    if (!row || !s) return;
    bool needs = cc_session_needs_you(s);
    uintptr_t want = ROW_SET | (needs ? ROW_NEEDS : 0) | (selected ? ROW_SEL : 0);
    lv_obj_t *edge = lv_obj_get_child(row, 0), *col = lv_obj_get_child(row, 1);
    if ((uintptr_t)lv_obj_get_user_data(row) != want) {
        lv_obj_remove_style(row, &s_row_sel, 0);
        if (s_amber_ready) {
            lv_obj_remove_style(row, &s_row_needs, 0);
            lv_obj_remove_style(row, &s_row_needs_sel, 0);
            lv_obj_remove_style(edge, &s_amber_edge, 0);
        }
        if (selected) lv_obj_add_style(row, &s_row_sel, 0);
        if (needs) {
            amber_styles(ui_needs_you_color());
            lv_obj_add_style(row, selected ? &s_row_needs_sel : &s_row_needs, 0);
            lv_obj_add_style(edge, &s_amber_edge, 0);
        }
        set_flag(row, UI_NEEDS_YOU_PART, needs);
        set_flag(edge, UI_NEEDS_YOU_PART, needs);
        lv_obj_set_user_data(row, (void *)want);
    }
    /* the pill first: its width sets the name's */
    lv_obj_t *pill = lv_obj_get_child(row, 2);
    ui_state_pill_set(pill, s);
    set_text(lv_obj_get_child(col, 1), meta);
    lv_obj_t *name = lv_obj_get_child(col, 0);
    lv_obj_update_layout(name);
    uint32_t w = (uint32_t)lv_obj_get_content_width(name);
    uint32_t h = (name_hash(s->name) ^ name_hash(lv_label_get_text(pill))) + w * 2654435761u;
    if (!h) h = 1;
    if ((uint32_t)(uintptr_t)lv_obj_get_user_data(name) != h)
        lv_obj_set_user_data(name, (void *)(uintptr_t)(set_fitted(name, s->name) ? h : 0));
}

/* ------------------------------------------------------------ settings */
static void switch_row_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_user_data(e);
    if (lv_obj_has_state(sw, LV_STATE_CHECKED)) lv_obj_remove_state(sw, LV_STATE_CHECKED);
    else lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, NULL);
}

static lv_obj_t *srow_base(lv_obj_t *parent, const char *label)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_add_style(r, &s_row, 0);
    lv_obj_add_style(r, &s_srow_geom, 0);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = text(r, label, NULL, NULL);
    lv_obj_set_size(l, 1, lv_font_get_line_height(FONT_BODY));
    lv_obj_set_flex_grow(l, 1);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    return r;
}

lv_obj_t *ui_srow(lv_obj_t *parent, ui_row_kind_t kind, const char *label, const char *value,
                  lv_event_cb_t cb, void *ud)
{
    lv_obj_t *r = srow_base(parent, label);
    lv_obj_t *v = NULL;
    if (kind == UI_ROW_SWITCH) {
        v = lv_switch_create(r);
        lv_obj_remove_flag(v, LV_OBJ_FLAG_CLICKABLE);   /* the row is the hit target */
        if (value && *value) lv_obj_add_state(v, LV_STATE_CHECKED);
        if (cb) lv_obj_add_event_cb(v, cb, LV_EVENT_VALUE_CHANGED, ud);
        lv_obj_add_event_cb(r, switch_row_cb, LV_EVENT_CLICKED, v);
    } else if (kind != UI_ROW_ACTION || (value && *value)) {
        v = text(r, value, &s_muted, &s_label);
        if (kind == UI_ROW_INFO) {
            /* a set width, not a max: LVGL sizes a content-width label to one unwrapped line, then clips it */
            lv_obj_set_width(v, LV_PCT(60));
            lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
            lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_style_min_height(r, UI_TK_ROW_H, 0);
            lv_obj_set_style_pad_ver(r, UI_TK_GAP_M, 0);
            lv_obj_set_height(r, LV_SIZE_CONTENT);
        } else {
            lv_obj_set_style_max_width(v, LV_PCT(55), 0);
            lv_obj_set_height(v, lv_font_get_line_height(FONT_SMALL));
            lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
        }
        if (kind == UI_ROW_NAV) text(r, "\xe2\x80\xba", &s_muted, NULL);   /* › */
    }
    if (kind != UI_ROW_INFO) {
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_style(r, &s_pressed, LV_STATE_PRESSED);
        lv_obj_set_ext_click_area(r, BODY_HIT_PAD);
        if (cb && kind != UI_ROW_SWITCH) lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
    }
    lv_obj_set_user_data(r, v);
    return r;
}

lv_obj_t *ui_srow_slider(lv_obj_t *parent, const char *label, int min, int max, int val,
                         lv_event_cb_t on_release, void *ud)
{
    lv_obj_t *r = srow_base(parent, label);
    lv_obj_t *box = plain(r);
    lv_obj_set_size(box, LV_PCT(55), UI_TK_ROW_H);
    lv_obj_set_style_pad_hor(box, 12, 0);
    pass_hits(r, BODY_HIT_PAD);   /* the slider's 80 px hit area crosses its row and box */
    pass_hits(box, BODY_HIT_PAD);
    lv_obj_t *track = plain(box);   /* the slider's main part is transparent so its hit area can be 64 px */
    lv_obj_add_style(track, &s_w_track, 0);
    lv_obj_set_size(track, LV_PCT(100), 8);
    lv_obj_center(track);
    lv_obj_t *s = lv_slider_create(box);
    lv_obj_remove_style_all(s);
    lv_obj_add_style(s, &s_w_on, LV_PART_INDICATOR);
    lv_obj_add_style(s, &s_w_knob, LV_PART_KNOB);
    lv_obj_set_size(s, LV_PCT(100), UI_TK_ROW_H);
    lv_obj_center(s);
    lv_obj_set_style_pad_ver(s, (UI_TK_ROW_H - 8) / 2, 0);
    lv_obj_set_style_pad_all(s, -(UI_TK_ROW_H - 24) / 2, LV_PART_KNOB);   /* LVGL sizes the knob to the slider: 24 px */
    lv_obj_set_ext_click_area(s, BODY_HIT_PAD);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    if (on_release) lv_obj_add_event_cb(s, on_release, LV_EVENT_RELEASED, ud);
    lv_obj_set_user_data(r, s);
    return r;
}

void ui_settings_column(lv_obj_t *o)
{
    lv_obj_add_style(o, &s_set_col, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
}

lv_obj_t *ui_srow_line(lv_obj_t *parent)
{
    lv_obj_t *l = plain(parent);
    lv_obj_add_style(l, &s_srow_line, 0);
    pass_hits(l, BODY_HIT_PAD);   /* the row and the button keep their 80 px hit areas */
    return l;
}

lv_obj_t *ui_srow_value(lv_obj_t *row) { return row ? lv_obj_get_user_data(row) : NULL; }

void ui_srow_set_selected(lv_obj_t *row, bool selected)
{
    lv_obj_remove_style(row, &s_row_sel, 0);
    if (selected) lv_obj_add_style(row, &s_row_sel, 0);
}

lv_obj_t *ui_section_header(lv_obj_t *parent, const char *t)
{
    lv_obj_t *l = text(parent, t, NULL, NULL);
    lv_obj_add_style(l, &s_section, 0);
    return l;
}

lv_obj_t *ui_srow_chip(lv_obj_t *row, const char *t, bool destructive)
{
    lv_obj_t *c = plain(row);
    lv_obj_add_style(c, &s_chip, 0);
    if (destructive) lv_obj_add_style(c, &s_chip_destructive, 0);
    lv_obj_set_width(c, LV_SIZE_CONTENT);
    text(c, t, NULL, NULL);
    return c;
}

/* ------------------------------------------------------------- Terminal */
/* host row children: 0 the dot, 1 the column (name, meta), 2 the trailing label (focus hint, note), 3 the spinner
   (made on first use); user data: HOST_NOTE while a note or the busy state shows (the focus hint stays hidden) */
#define HOST_NOTE ((void *)1)
static void host_focus_cb(lv_event_t *e)
{
    lv_obj_t *r = lv_event_get_current_target_obj(e);
    if (lv_obj_get_user_data(r) == HOST_NOTE) return;
    set_flag(lv_obj_get_child(r, 2), LV_OBJ_FLAG_HIDDEN, lv_event_get_code(e) != LV_EVENT_FOCUSED);
}

lv_obj_t *ui_host_row(lv_obj_t *parent, const char *name, const char *meta, const char *hint,
                      lv_event_cb_t cb, lv_event_cb_t edit_cb, const char *edit, void *ud)
{
    lv_obj_t *line = plain(parent);
    lv_obj_add_style(line, &s_host_line, 0);
    pass_hits(line, BODY_HIT_PAD);   /* the row and the Edit button keep their 80 px hit areas */
    lv_obj_t *r = lv_obj_create(line);
    lv_obj_remove_style_all(r);
    lv_obj_add_style(r, &s_row, 0);
    lv_obj_add_style(r, &s_host_geom, 0);
    lv_obj_add_style(r, &s_pressed, LV_STATE_PRESSED);
    lv_obj_set_width(r, 1);
    lv_obj_set_flex_grow(r, 1);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(r, (UI_TK_HIT - UI_TK_SESSION_ROW_H) / 2);
    if (cb) lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
    lv_obj_add_event_cb(r, host_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(r, host_focus_cb, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_t *dot = plain(r);
    lv_obj_add_style(dot, &s_host_dot, 0);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *col = plain(r);
    lv_obj_add_style(col, &s_host_col, 0);
    lv_obj_set_size(col, 1, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_t *n = text(col, name, NULL, NULL);
    lv_obj_set_size(n, LV_PCT(100), lv_font_get_line_height(FONT_BODY));
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t *m = text(col, meta, &s_muted, &s_label);
    lv_obj_set_size(m, LV_PCT(100), lv_font_get_line_height(FONT_SMALL));
    lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t *t = text(r, hint, &s_muted, &s_label);
    lv_obj_add_style(t, &s_host_hint, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
    if (edit_cb) {
        /* touch only: the keyboard reaches Edit by Ctrl+Enter on the row (its LONG_PRESSED), so Down goes row to row */
        lv_obj_t *b = ui_btn(line, UI_BTN_SECONDARY, edit, edit_cb, ud);
        lv_obj_add_flag(b, UI_KEYBOARD_SKIP);
        lv_obj_add_event_cb(r, edit_cb, LV_EVENT_LONG_PRESSED, ud);
    }
    return r;
}

void ui_host_row_dot(lv_obj_t *row, int dot)
{
    if (!row) return;
    lv_obj_t *d = lv_obj_get_child(row, 0);
    uintptr_t want = (uintptr_t)(dot + 2);   /* 1 none, 2 offline, 3 online */
    if ((uintptr_t)lv_obj_get_user_data(d) == want) return;
    lv_obj_set_user_data(d, (void *)want);
    lv_obj_remove_style(d, &s_dot_green, 0);
    lv_obj_remove_style(d, &s_dot_line, 0);
    if (dot >= 0) lv_obj_add_style(d, dot ? &s_dot_green : &s_dot_line, 0);
    set_flag(d, LV_OBJ_FLAG_HIDDEN, dot < 0);
}

void ui_host_row_note(lv_obj_t *row, const char *note, bool busy, const char *hint)
{
    if (!row) return;
    lv_obj_t *t = lv_obj_get_child(row, 2), *sp = lv_obj_get_child(row, 3);
    if (busy && !sp) {
        sp = lv_spinner_create(row);
        lv_obj_set_size(sp, 24, 24);
        lv_spinner_set_anim_params(sp, 1000, 270);
        lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_style(sp, &s_spinner_main, LV_PART_MAIN);
        lv_obj_add_style(sp, &s_spinner_ind, LV_PART_INDICATOR);
    }
    if (sp) {
        if (busy) lv_obj_remove_flag(sp, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_delete(sp);   /* no animation left running */
    }
    lv_obj_set_user_data(row, (note || busy) ? HOST_NOTE : NULL);
    bool fail = note && !busy;
    tone_apply(t, fail ? UI_TONE_RED : UI_TONE_MUTED);
    /* a failure wraps to two lines at most, so a refusal (TS_HOST_OFF_MESSAGE) reads whole; the hint is one line */
    lv_obj_remove_style(t, &s_host_fail, 0);
    if (fail) lv_obj_add_style(t, &s_host_fail, 0);
    set_text(t, note ? note : hint);
    set_flag(t, LV_OBJ_FLAG_HIDDEN, !note && !busy && !lv_obj_has_state(row, LV_STATE_FOCUS_KEY));
}

lv_obj_t *ui_srow_fingerprint(lv_obj_t *parent, const char *fp, const char *chip, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *r = ui_srow(parent, UI_ROW_ACTION, fp, NULL, cb, ud);
    lv_obj_add_style(r, &s_srow_wrap, 0);
    lv_obj_t *l = lv_obj_get_child(r, 0);
    lv_obj_add_style(l, &s_mono, 0);
    lv_obj_remove_local_style_prop(l, LV_STYLE_HEIGHT, 0);   /* srow_base's one-line height: the shared style wraps */
    lv_obj_add_style(l, &s_wrap_label, 0);
    tone_apply(l, UI_TONE_MUTED);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    if (chip) ui_srow_chip(r, chip, true);
    return r;
}

lv_obj_t *ui_add_row(lv_obj_t *parent, const char *t, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_add_style(r, &s_add_row, 0);
    lv_obj_add_style(r, &s_pressed, LV_STATE_PRESSED);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(r, (UI_TK_HIT - UI_TK_SESSION_ROW_H) / 2);
    lv_obj_t *l = text(r, t, NULL, NULL);
    lv_obj_set_size(l, 1, lv_font_get_line_height(FONT_BODY));
    lv_obj_set_flex_grow(l, 1);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    if (cb) lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
    return r;
}

lv_obj_t *ui_rail_row(lv_obj_t *parent, const char *label, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_add_style(r, &s_rail, 0);
    lv_obj_add_style(r, &s_pressed, LV_STATE_PRESSED);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(r, (UI_TK_HIT - UI_TK_RAIL_ROW_H) / 2);
    lv_obj_t *l = text(r, label, NULL, NULL);
    lv_obj_set_size(l, 1, lv_font_get_line_height(FONT_BODY));
    lv_obj_set_flex_grow(l, 1);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    if (cb) lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
    return r;
}

lv_obj_t *ui_swatch(lv_obj_t *parent, uint32_t rgb)
{
    lv_obj_t *d = plain(parent);
    lv_obj_add_style(d, &s_swatch, 0);
    lv_obj_set_style_bg_color(d, HEX(rgb), 0);   /* one colour per swatch */
    return d;
}

/* ---------------------------------------------------------------- text */
static void tone_apply(lv_obj_t *l, ui_tone_t tone)
{
    lv_obj_remove_style(l, &s_muted, 0);
    lv_obj_remove_style(l, &s_green_text, 0);
    lv_obj_remove_style(l, &s_red_text, 0);
    if (tone == UI_TONE_MUTED) lv_obj_add_style(l, &s_muted, 0);
    else if (tone == UI_TONE_GREEN) lv_obj_add_style(l, &s_green_text, 0);
    else if (tone == UI_TONE_RED) lv_obj_add_style(l, &s_red_text, 0);
}

lv_obj_t *ui_text(lv_obj_t *parent, const char *t, ui_tone_t tone, bool small)
{
    lv_obj_t *l = text(parent, t, NULL, small ? &s_label : NULL);
    tone_apply(l, tone);
    return l;
}

void ui_text_tone(lv_obj_t *label, ui_tone_t tone) { if (label) tone_apply(label, tone); }
void ui_text_mono(lv_obj_t *label) { if (label) lv_obj_add_style(label, &s_mono, 0); }
lv_obj_t *ui_title(lv_obj_t *parent, const char *t) { return text(parent, t, NULL, &s_title); }

lv_obj_t *ui_footnote(lv_obj_t *parent, const char *t)
{
    lv_obj_t *l = text(parent, t, &s_muted, &s_label);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

/* -------------------------------------------------------------- states */
lv_obj_t *ui_empty_state(lv_obj_t *parent, const char *t, const char *action, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = plain(parent);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, UI_TK_GAP_M, 0);
    lv_obj_t *l = text(c, t, &s_muted, NULL);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    if (action) ui_btn(c, UI_BTN_PRIMARY, action, cb, ud);
    return c;
}

lv_obj_t *ui_skeleton_row(lv_obj_t *parent)
{
    lv_obj_t *r = plain(parent);
    lv_obj_add_style(r, &s_panel2, 0);
    lv_obj_add_style(r, &s_skeleton, 0);
    lv_obj_set_width(r, LV_PCT(100));
    return r;
}
