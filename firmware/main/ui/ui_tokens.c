#include "ui_tokens.h"

/* The VS Code palette (the old themes' ANSI_STANDARD). */
const uint32_t ui_tk_term_ansi[16] = {
    0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xE5E5E5,
    0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF,
};

/* The one amber literal in the UI sources: NEEDS YOU, nothing else. */
uint32_t ui_tk_needs_you_hex(void) { return 0xf4b545; }
