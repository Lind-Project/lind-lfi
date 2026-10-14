// A grate for the crossing-cost measurements: for getpid/geteuid/getppid it
// either answers itself ("answer") or forwards to the next layer ("fwd").
//   g_fwd answer|fwd <program> [args...]
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lind_syscall.h"

static int answer(uint64_t g, uint64_t a1, uint64_t c1, uint64_t a2, uint64_t c2,
    uint64_t a3, uint64_t c3, uint64_t a4, uint64_t c4, uint64_t a5, uint64_t c5,
    uint64_t a6, uint64_t c6)
{
    (void) g, (void) a1, (void) c1, (void) a2, (void) c2, (void) a3, (void) c3,
        (void) a4, (void) c4, (void) a5, (void) c5, (void) a6, (void) c6;
    return 1000;
}

// The call number is not passed to handlers, so one forwarding handler per call.
#define FWD(name, nr)                                                          \
    static int name(uint64_t g, uint64_t a1, uint64_t c1, uint64_t a2, uint64_t c2, \
        uint64_t a3, uint64_t c3, uint64_t a4, uint64_t c4, uint64_t a5,       \
        uint64_t c5, uint64_t a6, uint64_t c6)                                 \
    {                                                                          \
        return make_threei_call(nr, 0, g, c1, a1, c1, a2, c2, a3, c3, a4, c4,  \
            a5, c5, a6, c6, 0);                                                \
    }
FWD(fwd_getpid, 39)
FWD(fwd_geteuid, 107)
FWD(fwd_getppid, 110)

int main(int argc, char **argv)
{
    assert(argc >= 3);
    int fwd = !strcmp(argv[1], "fwd");
    int grate = getpid();
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        int me = getpid();
        const int nrs[] = { 39, 107, 110 };
        uint64_t f[] = { (uint64_t) (uintptr_t) (fwd ? (void *) fwd_getpid : (void *) answer),
            (uint64_t) (uintptr_t) (fwd ? (void *) fwd_geteuid : (void *) answer),
            (uint64_t) (uintptr_t) (fwd ? (void *) fwd_getppid : (void *) answer) };
        for (int i = 0; i < 3; i++)
            assert(register_handler(me, nrs[i], grate, f[i]) == 0);
        execv(argv[2], &argv[2]);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
    return st != 0;
}
