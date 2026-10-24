#pragma once
#include <stdint.h>
typedef struct { int model; uint32_t features; uint16_t revision; uint8_t cores; } esp_chip_info_t;
static inline void esp_chip_info(esp_chip_info_t *o) { o->model = 18; o->features = 0; o->revision = 100; o->cores = 2; }
