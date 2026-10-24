#include "fonts.h"
#include "settings/settings.h"

/* U+23F5 BLACK MEDIUM RIGHT-POINTING TRIANGLE and U+23FA BLACK CIRCLE FOR RECORD (Claude
 * Code's tool-call bullet), one terminal cell each; neither face has them. */
static const uint8_t term_symbol_bitmap[] = {0x40,0x6,0x0,0x70,0x7,0x80,0x7c,0x7,0xe0,0x7f,0x7,0xf8,0x7f,0x87,0xf0,0x7e,0x7,0xc0,0x78,0x7,0x0,0x60,0x4,0x0,
    0x1e,0xf,0xc7,0xfb,0xff,0xff,0xff,0xff,0xfd,0xfe,0x3f,0x7,0x80};
static const lv_font_fmt_txt_glyph_dsc_t term_symbol_glyphs[] = {
    {0}, {.bitmap_index=0, .adv_w=192, .box_w=12, .box_h=16, .ofs_x=0, .ofs_y=2},
    {.bitmap_index=24, .adv_w=192, .box_w=10, .box_h=10, .ofs_x=1, .ofs_y=5}
};
static const lv_font_fmt_txt_cmap_t term_symbol_cmap[] = {
    {.range_start=0x23F5, .range_length=1, .glyph_id_start=1, .type=LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},
    {.range_start=0x23FA, .range_length=1, .glyph_id_start=2, .type=LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}
};
static const lv_font_fmt_txt_dsc_t term_symbol_dsc = {
    .glyph_bitmap=term_symbol_bitmap, .glyph_dsc=term_symbol_glyphs,
    .cmaps=term_symbol_cmap, .cmap_num=2, .bpp=1
};
static lv_font_t s_term_symbols = {
    .get_glyph_dsc=lv_font_get_glyph_dsc_fmt_txt, .get_glyph_bitmap=lv_font_get_bitmap_fmt_txt,
    .line_height=26, .base_line=5, .dsc=&term_symbol_dsc
};

/* The generated fonts live in flash (const). LVGL stores the fallback pointer
 * inside lv_font_t, so we work on RAM copies.
 * The roles are fixed (spec §8.3): one MaruMinya family, no theme or language switch. */
static lv_font_t s_px20, s_px24, s_px36b, s_cjk, s_icons;
const lv_font_t *FONT_SMALL = &s_px20;   /* label: 20 px role on the 24 px grid */
const lv_font_t *FONT_BODY = &s_px24;
const lv_font_t *FONT_TITLE = &s_px36b;
const lv_font_t *FONT_MONO = &g_term_font;
lv_font_t g_term_font;

void fonts_init(void)
{
    s_px20 = font_px20; s_px24 = font_px24; s_px36b = font_px36b;
    s_cjk = font_cjk; s_icons = font_icons16;
    /* MaruMinya covers Japanese, so the simplified and traditional Chinese
     * of the zh translations falls through to Noto, and the LVGL icon glyphs
     * to the icon font behind it. Noto Sans Mono CJK at 24 px advances 12 px, the
     * same as MaruMinya, so a fallback glyph still fills one terminal cell. */
    /* The generated Noto font has generous source-font vertical metrics.
     * Normalize them for 24 px UI use while retaining its anti-aliased glyphs. */
    s_cjk.line_height = 28;
    s_cjk.base_line = 5;
    s_px20.fallback = &s_cjk;
    s_px24.fallback = &s_cjk;
    s_px36b.fallback = &s_px24;
    s_cjk.fallback = &s_icons;
    s_icons.fallback = NULL;
    fonts_set_term_line_height(g_settings.term_line_height ? g_settings.term_line_height : 32);
}

void fonts_set_term_line_height(int target)
{
    const lv_font_t *base = &s_px24;
    g_term_font = *base;
    if (target < 28) target = 28;
    if (target > 48) target = 48;
    int32_t delta = (int32_t)base->line_height - target;   /* > 0 tightens */
    g_term_font.line_height = target;
    g_term_font.base_line = base->base_line - delta / 2;
    if (g_term_font.base_line < 0) g_term_font.base_line = 0;
    s_term_symbols.fallback = &s_cjk;
    g_term_font.fallback = &s_term_symbols;
}
