// copy_data_between_cages cost: the child reports the address of a 64 KiB
// buffer; the grate copies 1 B, 16 KiB and 32 KiB out of it, many times.
//   g_copy <child>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "lind_syscall.h"

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}

int main(int argc, char **argv)
{
    assert(argc >= 2);
    int up[2], down[2];
    assert(pipe(up) == 0 && pipe(down) == 0);
    int grate = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        char a1[16], a2[16];
        snprintf(a1, sizeof a1, "%d", up[1]);
        snprintf(a2, sizeof a2, "%d", down[0]);
        execl(argv[1], argv[1], a1, a2, (char *) 0);
        _exit(127);
    }
    uint64_t where;
    assert(read(up[0], &where, sizeof where) == sizeof where);
    static char buf[65536];
    const size_t sizes[] = { 1, 4096, 16384, 32768 };
    int bad = 0;
    for (int s = 0; s < 4; s++) {
        size_t n = sizes[s];
        int reps = 200000;
        // c_copy fills its buffer with 1; every copy must report success and deliver exactly that
        memset(buf, 0, sizeof buf);
        long r = copy_data_between_cages(grate, pid, where, pid, (uint64_t) (uintptr_t) buf, grate, n, 0);
        for (size_t k = 0; k < n; k++) if (buf[k] != 1) { bad = 1; break; }
        if (r < 0 || bad) { fprintf(stderr, "g_copy: first %zu B copy failed (ret %ld)\n", n, r); return 1; }
        for (int i = 0; i < 1000; i++)
            if (copy_data_between_cages(grate, pid, where, pid, (uint64_t) (uintptr_t) buf, grate, n, 0) < 0) bad = 1;
        double t0 = now();
        for (int i = 0; i < reps; i++)
            if (copy_data_between_cages(grate, pid, where, pid, (uint64_t) (uintptr_t) buf, grate, n, 0) < 0) bad = 1;
        double el = (now() - t0) / reps;
        if (bad) { fprintf(stderr, "g_copy: a %zu B copy returned an error\n", n); return 1; }
        printf("copy_data_%zuB %.0f ns\n", n, el);
    }
    assert(write(down[1], "x", 1) == 1);
    int st;
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 0) { fprintf(stderr, "g_copy: child failed\n"); return 1; }
    return 0;
}
