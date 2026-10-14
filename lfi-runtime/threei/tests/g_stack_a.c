// Outer grate of a two-grate stack: cage -> B -> A -> default implementation.
// A adds 5 to whatever geteuid returns.
#include <assert.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include "lind_syscall.h"

static int geteuid_a(uint64_t cageid, uint64_t a1, uint64_t c1, uint64_t a2,
                     uint64_t c2, uint64_t a3, uint64_t c3, uint64_t a4,
                     uint64_t c4, uint64_t a5, uint64_t c5, uint64_t a6,
                     uint64_t c6) {
    int r = make_threei_call(107, 0, getpid(), c1, a1, c1, a2, c2, a3, c3, a4,
                             c4, a5, c5, a6, c6, 0);
    printf("[A] geteuid below me = %d, returning %d\n", r, r + 5);
    return r + 5;
}

int main(int argc, char **argv) {
    assert(argc >= 2);
    int me = getpid();
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        // The next grate (B) runs on top of A: its calls go through A.
        assert(register_handler(getpid(), 107, me, (uint64_t)(uintptr_t)&geteuid_a) == 0);
        execv(argv[1], &argv[1]);
        _exit(127);
    }
    int st;
    wait(&st);
    printf("[A] %s\n", st == 0 ? "PASS" : "FAIL");
    return st != 0;
}
