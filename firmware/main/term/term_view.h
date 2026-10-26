#pragma once
#include "lvgl.h"
#include "vt.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct term_view term_view_t;
term_view_t *term_view_create(lv_obj_t *parent, vt_t *vt, const lv_font_t *font);
void term_view_destroy(term_view_t *tv);
lv_obj_t *term_view_obj(term_view_t *tv);
void term_view_set_vt(term_view_t *tv, vt_t *vt);
/* Recompute cols/rows from the widget size; returns true if size changed. */
bool term_view_fit(term_view_t *tv, int *cols, int *rows);
void term_view_refresh(term_view_t *tv);      /* invalidate dirty rows */
void term_view_set_colors(term_view_t *tv, lv_color_t fg, lv_color_t bg);
typedef enum { TERM_CURSOR_BLOCK = 0, TERM_CURSOR_UNDERLINE = 1, TERM_CURSOR_BAR = 2 } term_cursor_t;
/* A blinking cursor runs a 550 ms timer; blink false (the default, B1) runs none. */
void term_view_set_cursor(term_view_t *tv, term_cursor_t style, bool blink, lv_color_t color);
int  term_view_cell_w(term_view_t *tv);
int  term_view_cell_h(term_view_t *tv);
/* A drag's rows, for an app that scrolls itself (a take-over of Claude Code: its alternate screen leaves the local
   scrollback empty). While set, each text row a drag moves calls cb with the rows since the last call (+ = finger down,
   i.e. back to older text) and the cell the press began on (1-based). cb returns how many of those rows it took: once it
   has taken any, the drag leaves the local view alone (the rest are offered again on the next move); while it takes
   none (0: say the app hasn't turned mouse reporting on), the drag scrolls the local view as without a cb. NULL clears it. */
typedef int (*term_view_drag_cb_t)(int rows, int col, int row, void *user);
void term_view_set_drag_cb(term_view_t *tv, term_view_drag_cb_t cb, void *user);
/* True only while the current press has stayed within the tap slop. */
bool term_view_long_press_allowed(term_view_t *tv);
#ifdef __cplusplus
}
#endif
