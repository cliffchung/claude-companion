#pragma once
#include "lvgl.h"
#include "i18n/i18n.h"
#ifdef __cplusplus
extern "C" {
#endif
/* MaruMinya from the repository TTF, rasterized on exact 12px multiples.
 * Legacy C names are retained: font_px20 now uses the same 24px/2x grid
 * for readable secondary text. Missing CJK glyphs fall back to Noto. */
LV_FONT_DECLARE(font_px20);
LV_FONT_DECLARE(font_px24);
LV_FONT_DECLARE(font_px36b);
LV_FONT_DECLARE(font_cjk);
LV_FONT_DECLARE(font_icons16);

/* Fixed roles (spec §8.3). Font selection lives here and in ui_comp only. */
extern const lv_font_t *FONT_SMALL;   /* label role: font_px20 */
extern const lv_font_t *FONT_BODY;    /* font_px24 */
extern const lv_font_t *FONT_TITLE;   /* font_px36b, bold */
extern const lv_font_t *FONT_MONO;    /* g_term_font: 24 px, 12 px cells (terminal) */
extern lv_font_t g_term_font;         /* FONT_MONO with an adjustable line height */

void fonts_init(void);
void fonts_set_term_line_height(int px);
#ifdef __cplusplus
}
#endif
