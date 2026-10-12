// Several cages calling into one grate at the same time.
#include <assert.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include "lind_syscall.h"

static volatile long calls;

static int answer(uint64_t cageid, uint64_t a1, uint64_t c1, uint64_t a2, uint64_t c2,
                  uint64_t a3, uint64_t c3, uint64_t a4, uint64_t c4, uint64_t a5,
                  uint64_t c5, uint64_t a6, uint64_t c6) {
    __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    return 10;
}

int main(int argc, char **argv) {
    assert(argc >= 2);
    int me = getpid();
    int ncages = 12;
    for (int i = 0; i < ncages; i++) {
        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            assert(register_handler(getpid(), 107, me, (uint64_t)(uintptr_t)&answer) == 0);
            execv(argv[1], &argv[1]);
            _exit(127);
        }
    }
    int st, bad = 0;
    while (wait(&st) > 0) bad += st != 0;
    printf("[conc] %d cages, grate served %ld calls\n", ncages, calls);
    assert(!bad);
    printf("[conc] PASS\n");
    return 0;
}
