// F01 regression: a cage confined by a parent grate cannot suspend that grate's handlers by
// abusing the private context-spawn controls (CAPTURE/COLLECT/SETENTRY).
//   g_capture <child>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include "lind_syscall.h"

#define H(name, ret)                                                                 \
    static int name(uint64_t g, uint64_t a1, uint64_t c1, uint64_t a2, uint64_t c2,  \
        uint64_t a3, uint64_t c3, uint64_t a4, uint64_t c4, uint64_t a5, uint64_t c5, \
        uint64_t a6, uint64_t c6) { return (ret); }
H(answer777, 777)
H(deny, -EPERM)

int main(int argc, char **argv)
{
    assert(argc >= 2);
    int grate = getpid();
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        int me = getpid();
        assert(register_handler(me, 39, grate, (uint64_t) (uintptr_t) answer777) == 0); // getpid
        assert(register_handler(me, 110, grate, (uint64_t) (uintptr_t) deny) == 0);      // getppid
        assert(register_handler(me, 57, grate, (uint64_t) (uintptr_t) deny) == 0);       // fork
        execv(argv[1], &argv[1]);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
    printf("[capture] child status %d\n", st);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    printf("[capture] PASS\n");
    return 0;
}
