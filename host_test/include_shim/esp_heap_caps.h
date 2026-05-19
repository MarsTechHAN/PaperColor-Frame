#pragma once
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_SPIRAM   0
#define MALLOC_CAP_8BIT     0

static inline void *heap_caps_calloc(size_t n, size_t sz, int caps)     { (void)caps; return calloc(n, sz); }
static inline void *heap_caps_malloc(size_t sz, int caps)               { (void)caps; return malloc(sz); }
static inline void *heap_caps_aligned_alloc(size_t a, size_t sz, int c) { (void)a;(void)c; return malloc(sz); }
static inline void  heap_caps_free(void *p)                             { free(p); }
static inline size_t heap_caps_get_free_size(int caps)                  { (void)caps; return 0; }
