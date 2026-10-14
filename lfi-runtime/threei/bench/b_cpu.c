// Compute kernels for the "compute overhead" question (the paper's Fig. 4 is SPEC
// CPU 2017 under Wasm): float math, integer/array code, indirect calls, hashing.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static double nbody(int steps)
{
    double x[5][3], v[5][3], m[5];
    for (int i = 0; i < 5; i++) { m[i] = 1 + i; for (int k = 0; k < 3; k++) { x[i][k] = i * 1.3 + k; v[i][k] = 0.01 * (i + k); } }
    for (int s = 0; s < steps; s++) {
        for (int i = 0; i < 5; i++) for (int j = i + 1; j < 5; j++) {
            double d[3], d2 = 0.01;
            for (int k = 0; k < 3; k++) { d[k] = x[i][k] - x[j][k]; d2 += d[k] * d[k]; }
            double mag = 0.001 / (d2 * sqrt(d2));
            for (int k = 0; k < 3; k++) { v[i][k] -= d[k] * m[j] * mag; v[j][k] += d[k] * m[i] * mag; }
        }
        for (int i = 0; i < 5; i++) for (int k = 0; k < 3; k++) x[i][k] += 0.001 * v[i][k];
    }
    return x[0][0];
}

static int fannkuch(int n)
{
    int perm[16], perm1[16], count[16], maxflips = 0, r = n;
    for (int i = 0; i < n; i++) perm1[i] = i;
    for (;;) {
        while (r != 1) { count[r - 1] = r; r--; }
        memcpy(perm, perm1, sizeof(int) * n);
        int flips = 0, k;
        while ((k = perm[0]) != 0) { for (int i = 0, j = k; i < j; i++, j--) { int t = perm[i]; perm[i] = perm[j]; perm[j] = t; } flips++; }
        if (flips > maxflips) maxflips = flips;
        for (;;) {
            if (r == n) return maxflips;
            int p0 = perm1[0];
            for (int i = 0; i < r; i++) perm1[i] = perm1[i + 1];
            perm1[r] = p0;
            if (--count[r] > 0) break;
            r++;
        }
    }
}

static int cmp(const void *a, const void *b) { int x = *(const int *) a, y = *(const int *) b; return (x > y) - (x < y); }
// the same code on both sides (libc qsort differs: glibc vs musl algorithms)
static void isort(int *a, long lo, long hi, int (*c)(const void *, const void *))
{
    while (hi - lo > 16) {
        int p = a[(lo + hi) / 2];
        long i = lo, j = hi;
        while (i <= j) {
            while (c(&a[i], &p) < 0) i++;
            while (c(&a[j], &p) > 0) j--;
            if (i <= j) { int t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
        }
        if (j - lo < hi - i) { isort(a, lo, j, c); lo = i; } else { isort(a, i, hi, c); hi = j; }
    }
    for (long i = lo + 1; i <= hi; i++) { int v = a[i]; long j = i - 1; while (j >= lo && c(&a[j], &v) > 0) { a[j + 1] = a[j]; j--; } a[j + 1] = v; }
}

static long qsort_indirect(int n)
{
    int *a = malloc(n * sizeof(int));
    uint32_t s = 12345;
    for (int i = 0; i < n; i++) { s = s * 1664525u + 1013904223u; a[i] = s >> 8; }
    isort(a, 0, n - 1, cmp);   // one indirect call per comparison
    long r = a[n / 2];
    free(a);
    return r;
}

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static uint32_t hash_loop(int blocks)
{
    uint32_t h[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, w[64];
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < 16; i++) w[i] = b * 31 + i;
        for (int i = 16; i < 64; i++) w[i] = rotr(w[i - 2], 17) ^ rotr(w[i - 15], 7) ^ w[i - 7] ^ w[i - 16];
        uint32_t a = h[0], e = h[4];
        for (int i = 0; i < 64; i++) { uint32_t t = a + (rotr(e, 6) ^ rotr(e, 11)) + w[i]; a = e + t; e = rotr(a, 2) ^ t; }
        h[0] ^= a; h[4] ^= e;
    }
    return h[0] ^ h[4];
}

#define TIME(name, expr) do { double t = now(); volatile double sink = (double) (expr); (void) sink; printf("%-14s %8.1f ms\n", name, now() - t); } while (0)
int main(void)
{
    TIME("nbody", nbody(3000000));
    TIME("fannkuch", fannkuch(10));
    TIME("sort(indirect)", qsort_indirect(4000000));
    TIME("hash", hash_loop(1500000));
    return 0;
}
