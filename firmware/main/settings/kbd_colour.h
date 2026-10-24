#pragma once
/* Settings › Keyboard light › Colour (spec §5.10): one picker over kbd_led_mode and kbd_led_rgb. Index 0 is
 * "Keyboard's own" (mode 0, the keyboard's bound colours); 1 + i is swatch i in mode 1. Pure C. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { const char *label; uint32_t rgb; } kbd_swatch_t;
/* Blue (the default), White, Red, Green, Magenta, Cyan, then {NULL, 0}. None is amber-like: amber on the
 * keyboard means a session needs you (pr_rgb_amber_like). */
extern const kbd_swatch_t kbd_swatches[];
#define KBD_SWATCH_COUNT 6

/* 0 for mode 0; 1 + i when mode 1 holds swatch i's colour; -1 otherwise ("Custom", set from the console). */
int kbd_colour_index(uint8_t mode, const uint8_t rgb[3]);
/* 0: mode 0, rgb kept. 1..KBD_SWATCH_COUNT: mode 1 and that swatch's rgb. Anything else changes nothing. */
void kbd_colour_apply(int index, uint8_t *mode, uint8_t rgb[3]);

#ifdef __cplusplus
}
#endif
