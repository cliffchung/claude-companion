#pragma once
/* The design tokens (spec §8.1): one set of values, no runtime themes. Pure C, no LVGL, so host
 * tests and the settings migration can use them. Colours are 0xRRGGBB.
 *
 * Amber is not here: it means NEEDS YOU only, and leaves this module through
 * ui_tk_needs_you_hex() alone, whose one caller is ui_comp.c (firmware/tools/ui_lint.py). */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- colours ---- */
#define UI_TK_BG          0x0e1116   /* screens; the terminal background */
#define UI_TK_PANEL       0x161b22   /* header, footer, banner, sheets, idle rows */
#define UI_TK_PANEL2      0x1b212b   /* selected rows, fields, secondary buttons, chips, skeletons */
#define UI_TK_LINE        0x2a323d   /* hairlines, idle borders, the off switch track */
#define UI_TK_FG          0xe9e7e1   /* text; primary-button fill; the focus ring; the on switch */
#define UI_TK_MUTED       0x8a94a2   /* secondary text, hints, placeholders, footnotes */
#define UI_TK_GREEN       0x54d08a   /* WORKING, link up, "Sent" */
#define UI_TK_RED         0xef6f6f   /* errors, unreachable, low battery, destructive buttons */
#define UI_TK_RED_BG_OPA  31         /* red at 12 % behind red banners */

/* ---- sizes (px) ---- */
#define UI_TK_MARGIN         24
#define UI_TK_GAP_S          8
#define UI_TK_GAP_M          16
#define UI_TK_GAP_L          24
#define UI_TK_ROW_H          64
#define UI_TK_SESSION_ROW_H  72
#define UI_TK_RAIL_ROW_H     72
#define UI_TK_HEADER_H       56
#define UI_TK_SESSION_BAR_H  56
#define UI_TK_FOOTER_H       64
#define UI_TK_BANNER_H       48
#define UI_TK_RADIUS         12      /* rows, cards, fields, buttons */
#define UI_TK_RADIUS_CHIP    8       /* chips, pills */
#define UI_TK_TARGET         64      /* a body control's visible size */
#define UI_TK_HIT            80      /* every control's hit area */
#define UI_TK_BAR_CTRL       48      /* a control inside a 56/64 px bar */
#define UI_TK_LIST_W         440     /* the Sessions list */
#define UI_TK_RAIL_W         320     /* the Settings rail */

/* ---- glyphs (font_icons16, behind every UI face; tools/gen_ui_icons.py EXTRA_CPS) ---- */
#define UI_GLYPH_LOCK      "\xef\x80\xa3"   /* U+F023: a secured Wi-Fi network */

/* ---- the terminal ---- */
#define UI_TK_TERM_FG      UI_TK_FG
#define UI_TK_TERM_BG      UI_TK_BG
#define UI_TK_TERM_CURSOR  UI_TK_FG
#define UI_TK_TERM_LINE_H  32
extern const uint32_t ui_tk_term_ansi[16];   /* the VS Code palette */

/* NEEDS YOU amber. Only ui_comp.c calls it (ui_lint `amber-accessor`). */
uint32_t ui_tk_needs_you_hex(void);

#ifdef __cplusplus
}
#endif
