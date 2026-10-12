// A tracing grate: forwards the file calls of its child to the default
// implementation on the child's behalf (so the child's descriptors are used),
// counting them.
#include <assert.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include "lind_syscall.h"

static volatile int n_open, n_close, n_read, n_write, n_lseek, n_dup, n_pipe, n_unlink;

#define FWD(NAME, NR, COUNTER)                                                 \
    static int NAME(uint64_t cageid, uint64_t a1, uint64_t c1, uint64_t a2,    \
                    uint64_t c2, uint64_t a3, uint64_t c3, uint64_t a4,        \
                    uint64_t c4, uint64_t a5, uint64_t c5, uint64_t a6,        \
                    uint64_t c6) {                                             \
        __atomic_add_fetch(&COUNTER, 1, __ATOMIC_RELAXED);                     \
        return make_threei_call(NR, 0, getpid(), c1, a1, c1, a2, c2, a3, c3,   \
                                a4, c4, a5, c5, a6, c6, 0);                    \
    }
FWD(h_open, 2, n_open)
FWD(h_close, 3, n_close)
FWD(h_read, 0, n_read)
FWD(h_write, 1, n_write)
FWD(h_lseek, 8, n_lseek)
FWD(h_dup, 32, n_dup)
FWD(h_pipe, 22, n_pipe)
FWD(h_unlink, 87, n_unlink)

int main(int argc, char **argv) {
    assert(argc >= 2);
    int me = getpid();
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        int c = getpid();
        struct { int nr; void *fn; } t[] = {
            {2, h_open}, {3, h_close}, {0, h_read}, {1, h_write},
            {8, h_lseek}, {32, h_dup}, {22, h_pipe}, {87, h_unlink},
        };
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
            assert(register_handler(c, t[i].nr, me, (uint64_t)(uintptr_t)t[i].fn) == 0);
        execv(argv[1], &argv[1]);
        _exit(127);
    }
    int st;
    wait(&st);
    printf("[trace] open=%d close=%d read=%d write=%d lseek=%d dup=%d pipe=%d unlink=%d\n",
           n_open, n_close, n_read, n_write, n_lseek, n_dup, n_pipe, n_unlink);
    // The cage's own printf also goes through write.
    assert(st == 0);
    assert(n_open == 1 && n_unlink == 1 && n_pipe == 1 && n_dup == 1);
    assert(n_write >= 4 && n_read >= 2 && n_lseek == 1 && n_close >= 4);
    printf("[trace] PASS\n");
    return 0;
}
