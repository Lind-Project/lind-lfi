#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>
#include "lind_syscall.h"
static int crashing(uint64_t cageid, uint64_t a1, uint64_t c1, uint64_t a2, uint64_t c2,
                    uint64_t a3, uint64_t c3, uint64_t a4, uint64_t c4, uint64_t a5,
                    uint64_t c5, uint64_t a6, uint64_t c6) {
    *(volatile int *)0 = 1;
    return 10;
}
int main(int argc, char **argv) {
    int me = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        assert(register_handler(getpid(), 107, me, (uint64_t)(uintptr_t)&crashing) == 0);
        execv(argv[1], &argv[1]);
        _exit(127);
    }
    int st;
    wait(&st);
    return st;
}
