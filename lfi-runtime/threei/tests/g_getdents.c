// A grate that handles Lind's getdents (78) and forwards it, while the cage's
// libc (musl) calls getdents64 (217): the runtime routes 217 to the handler and
// hands the cage getdents64 records (the two layouts differ).
#include <assert.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lind_syscall.h"

static int forwarded;

int getdents_grate(uint64_t grate, uint64_t a1, uint64_t c1, uint64_t a2, uint64_t c2,
    uint64_t a3, uint64_t c3, uint64_t a4, uint64_t c4, uint64_t a5, uint64_t c5,
    uint64_t a6, uint64_t c6)
{
    forwarded++;
    return make_threei_call(78, 0, grate, c1, a1, c1, a2, c2, a3, c3, a4, c4, a5, c5,
        a6, c6, 0);
}

int main(int argc, char *argv[])
{
    assert(argc >= 2);
    int grate = getpid();
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        assert(register_handler(getpid(), 78, grate, (uint64_t) (uintptr_t) &getdents_grate) == 0);
        execv(argv[1], &argv[1]);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    if (status != 0) { fprintf(stderr, "[grate] FAIL: cage exited %d\n", status); return 1; }
    printf("[grate] PASS (%d getdents calls forwarded)\n", forwarded);
    return forwarded > 0 ? 0 : 1;
}
