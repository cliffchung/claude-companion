#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_INTERNAL 1
static inline void *heap_caps_calloc(size_t n, size_t size, unsigned caps) { (void)caps; return calloc(n, size); }
static inline void *heap_caps_malloc(size_t size, unsigned caps) { (void)caps; return malloc(size); }
static inline void heap_caps_free(void *p) { free(p); }
/* fixed numbers, so a Diagnostics refresh redraws nothing */
static inline size_t heap_caps_get_free_size(unsigned caps) { return caps == MALLOC_CAP_INTERNAL ? 151000 : 21000000; }
static inline size_t heap_caps_get_minimum_free_size(unsigned caps) { (void)caps; return 98000; }
static inline size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 61000; }
