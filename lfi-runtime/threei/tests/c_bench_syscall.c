// Measures the cost of a trivial system call (geteuid) from a cage.
#ifndef CALL
#define CALL geteuid
#endif
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 2000000;
    volatile int sink = 0;
    for (long i = 0; i < 10000; i++) sink = CALL();
    double t0 = now();
    for (long i = 0; i < n; i++) sink = CALL();
    double t1 = now();
    printf("call: %.1f ns/call (n=%ld, last=%d)\n", (t1 - t0) / n, n, sink);
    return 0;
}
