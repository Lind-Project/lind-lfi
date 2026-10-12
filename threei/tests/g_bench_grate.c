// Benchmark grates. Usage: g_bench_grate <mode> <cage> [args]
//   mode "answer":  handler answers geteuid itself
//   mode "forward": handler forwards geteuid to the default implementation
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lind_syscall.h"

static int answer(uint64_t cageid, uint64_t a1, uint64_t c1, uint64_t a2,
                  uint64_t c2, uint64_t a3, uint64_t c3, uint64_t a4,
                  uint64_t c4, uint64_t a5, uint64_t c5, uint64_t a6,
                  uint64_t c6) {
    return 10;
}

static int forward(uint64_t cageid, uint64_t a1, uint64_t c1, uint64_t a2,
                   uint64_t c2, uint64_t a3, uint64_t c3, uint64_t a4,
                   uint64_t c4, uint64_t a5, uint64_t c5, uint64_t a6,
                   uint64_t c6) {
    return make_threei_call(107, 0, getpid(), c1, a1, c1, a2, c2, a3, c3, a4,
                            c4, a5, c5, a6, c6, 0);
}

int main(int argc, char **argv) {
    assert(argc >= 3);
    int grate = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        void *fn = strcmp(argv[1], "answer") == 0 ? (void *)answer : (void *)forward;
        assert(register_handler(getpid(), 107, grate, (uint64_t)(uintptr_t)fn) == 0);
        execv(argv[2], &argv[2]);
        _exit(127);
    }
    int st;
    wait(&st);
    return st != 0;
}
