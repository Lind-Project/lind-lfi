// Serves write() by copying the cage's buffer into the grate (so the copy can
// race with the cage unmapping it).
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include "lind_syscall.h"

static char local[256 * 1024];

static int h_write(uint64_t cageid, uint64_t a1, uint64_t c1, uint64_t a2, uint64_t c2,
                   uint64_t a3, uint64_t c3, uint64_t a4, uint64_t c4, uint64_t a5,
                   uint64_t c5, uint64_t a6, uint64_t c6) {
    if (a3 > sizeof local) return -EINVAL;
    int r = copy_data_between_cages(getpid(), c2, a2, c2, (uint64_t)local, getpid(), a3, 0);
    if (r == (int)ELINDAPIABORTED || r < 0) return -EFAULT;
    return (int)a3;
}

int main(int argc, char **argv) {
    int me = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        assert(register_handler(getpid(), 1, me, (uint64_t)(uintptr_t)&h_write) == 0);
        execv(argv[1], &argv[1]);
        _exit(127);
    }
    int st;
    wait(&st);
    if (st) { fprintf(stderr, "cage died with status %d\n", st); return 1; }
    printf("[copyrace] PASS\n");
    return 0;
}
