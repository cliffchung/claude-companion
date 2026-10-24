#include "kbd_colour.h"
#include <stddef.h>

const kbd_swatch_t kbd_swatches[] = {
    { "Blue", 0x0080FF }, { "White", 0xFFFFFF }, { "Red", 0xFF0000 },
    { "Green", 0x00FF00 }, { "Magenta", 0xFF00FF }, { "Cyan", 0x00FFFF },
    { NULL, 0 },
};

int kbd_colour_index(uint8_t mode, const uint8_t rgb[3])
{
    if (mode == 0) return 0;
    if (mode != 1 || !rgb) return -1;
    uint32_t c = ((uint32_t)rgb[0] << 16) | ((uint32_t)rgb[1] << 8) | rgb[2];
    for (int i = 0; kbd_swatches[i].label; i++)
        if (kbd_swatches[i].rgb == c) return 1 + i;
    return -1;
}

void kbd_colour_apply(int index, uint8_t *mode, uint8_t rgb[3])
{
    if (!mode || !rgb || index < 0 || index > KBD_SWATCH_COUNT) return;
    if (index == 0) { *mode = 0; return; }
    uint32_t c = kbd_swatches[index - 1].rgb;
    *mode = 1;
    rgb[0] = (uint8_t)(c >> 16);
    rgb[1] = (uint8_t)(c >> 8);
    rgb[2] = (uint8_t)c;
}
