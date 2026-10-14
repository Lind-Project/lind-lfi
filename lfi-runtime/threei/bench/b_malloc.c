// Allocator churn: many small objects of mixed sizes with a live set, like the
// C++/compiler SPEC programs. Native links glibc's malloc, the LFI build musl's.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
int main(void)
{
    enum { LIVE = 200000, OPS = 20000000 };
    void **p = calloc(LIVE, sizeof(void *));
    uint32_t s = 1;
    double t0 = now();
    for (long i = 0; i < OPS; i++) {
        s = s * 1664525u + 1013904223u;
        uint32_t k = (s >> 8) % LIVE, sz = 16 + ((s >> 3) & 127) * 4;
        free(p[k]);
        p[k] = malloc(sz);
        *(volatile char *) p[k] = 1;
    }
    printf("malloc/free churn: %.0f ms (%.1f ns per malloc+free)\n", now() - t0, (now() - t0) * 1e6 / OPS);
    return 0;
}
