// F08 regression: a grate that forwards exit/exit_group/execve used to leak the references the
// forwarding call held on the cages involved (the host frames that would release them are thrown
// away by the exit). Cage ids are then never recycled, and the table (--max-cages, small here)
// runs out long before the loop ends.
//   g_exitleak <child> [rounds]
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lind_syscall.h"

#define FWD(name, nr)                                                                 \
    static int name(uint64_t g, uint64_t a1, uint64_t c1, uint64_t a2, uint64_t c2,   \
        uint64_t a3, uint64_t c3, uint64_t a4, uint64_t c4, uint64_t a5, uint64_t c5, \
        uint64_t a6, uint64_t c6)                                                     \
    {                                                                                 \
        return make_threei_call(nr, 0, g, c1, a1, c1, a2, c2, a3, c3, a4, c4, a5, c5, \
            a6, c6, 0);                                                               \
    }
FWD(fwd_exit, 60)
FWD(fwd_exit_group, 231)
FWD(fwd_exec, 59)

int main(int argc, char **argv)
{
    assert(argc >= 2);
    int rounds = argc > 2 ? atoi(argv[2]) : 150;
    int grate = getpid();
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        int me = getpid();
        assert(register_handler(me, 60, grate, (uint64_t) (uintptr_t) fwd_exit) == 0);
        assert(register_handler(me, 231, grate, (uint64_t) (uintptr_t) fwd_exit_group) == 0);
        assert(register_handler(me, 59, grate, (uint64_t) (uintptr_t) fwd_exec) == 0);
        char r[16];
        snprintf(r, sizeof r, "%d", rounds);
        execl(argv[1], argv[1], r, (char *) 0);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
    printf("[exitleak] app status %d\n", st);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    printf("[exitleak] PASS\n");
    return 0;
}
