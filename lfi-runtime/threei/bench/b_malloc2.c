// Allocator churn: many small objects of mixed sizes with a live set, like the
// C++/compiler SPEC programs. Native links glibc's malloc, the LFI build musl's.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
// the same allocator code on both sides: size classes of 16 bytes, LIFO free lists,
// memory carved from big blocks and never returned
static void *fl[256];
static char *cur, *end;
static void *my_malloc(size_t n)
{
    size_t c = (n + 15) >> 4;
    void *r = fl[c];
    if (r) { fl[c] = *(void **) r; return r; }
    size_t sz = c << 4;
    if (cur + sz > end) { cur = malloc(1 << 20); end = cur + (1 << 20); }
    r = cur; cur += sz;
    return r;
}
static void my_free(void *p, size_t n) { if (!p) return; size_t c = (n + 15) >> 4; *(void **) p = fl[c]; fl[c] = p; }

int main(void)
{
    enum { LIVE = 200000, OPS = 20000000 };
    void **p = calloc(LIVE, sizeof(void *)); uint32_t *sizes = calloc(LIVE, 4);
    uint32_t s = 1;
    double t0 = now();
    for (long i = 0; i < OPS; i++) {
        s = s * 1664525u + 1013904223u;
        uint32_t k = (s >> 8) % LIVE, sz = 16 + ((s >> 3) & 127) * 4;
        my_free(p[k], sizes[k]);
        p[k] = my_malloc(sz); sizes[k] = sz;
        *(volatile char *) p[k] = 1;
    }
    printf("free-list allocator churn: %.0f ms (%.1f ns per malloc+free)\n", now() - t0, (now() - t0) * 1e6 / OPS);
    return 0;
}
