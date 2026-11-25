#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_DMA 0
#define MALLOC_CAP_INTERNAL 0
static inline void* heap_caps_calloc(size_t n, size_t s, int) { return calloc(n, s); }
static inline void* heap_caps_malloc(size_t s, int) { return malloc(s); }
