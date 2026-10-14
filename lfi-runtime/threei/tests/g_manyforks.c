// Many processes: sequential fork/wait far beyond the number of slots (ids and
// sandboxes are recycled), and many live children at once.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(void) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int N = 1000;
    for (int i = 0; i < N; i++) {
        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) _exit(i % 200);
        int st;
        assert(waitpid(pid, &st, 0) == pid);
        assert(WIFEXITED(st) && WEXITSTATUS(st) == i % 200);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double us = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e3 / N;
    printf("[manyforks] %d sequential fork+exit+wait: %.1f us each\n", N, us);

    // 120 children alive at the same time, released together.
    int p[2];
    assert(pipe(p) == 0);
    int live = 120;
    pid_t pids[120];
    for (int i = 0; i < live; i++) {
        pids[i] = fork();
        assert(pids[i] >= 0);
        if (pids[i] == 0) {
            char c;
            read(p[0], &c, 1);
            _exit(i);
        }
    }
    close(p[0]);
    for (int i = 0; i < live; i++) assert(write(p[1], "x", 1) == 1);
    long sum = 0;
    for (int i = 0; i < live; i++) {
        int st;
        pid_t w = wait(&st);
        assert(w > 0 && WIFEXITED(st));
        sum += WEXITSTATUS(st);
    }
    assert(sum == (long)live * (live - 1) / 2);
    printf("[manyforks] PASS\n");
    return 0;
}
