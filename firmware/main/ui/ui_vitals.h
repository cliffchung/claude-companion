#pragma once
#include "lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif

/* The header's right-hand side (spec §5): battery glyph + %, charging bolt, Wi-Fi bars, Tailscale mark,
   the link dot and the word "Host", clock. `parent` is the bar (a flex row); the result is the group
   inside it. Filled at create, then refreshed by one shared 30 s timer that exists while any cluster
   does (B1); each refresh writes only what changed. */
lv_obj_t *ui_vitals_create(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif
