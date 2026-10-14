// Kernel-mechanism baselines for the paper's Table 5, on this machine:
// per-syscall cost with no policy, with a seccomp-BPF filter, and under ptrace.
#define _GNU_SOURCE
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }

int main(void)
{
    const int N = 200000;
    double t0 = now();
    for (int i = 0; i < N; i++) syscall(SYS_getppid);
    printf("no policy              %.0f ns\n", (now() - t0) / N);

    pid_t p = fork();
    if (p == 0) {
        struct sock_filter f[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),                                  // nr
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_kill, 0, 1),                    // deny kill
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        struct sock_fprog prog = { .len = 4, .filter = f };
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
        prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog);
        double t = now();
        for (int i = 0; i < N; i++) syscall(SYS_getppid);
        printf("seccomp-BPF allowlist  %.0f ns\n", (now() - t) / N); fflush(stdout);
        _exit(0);
    }
    waitpid(p, NULL, 0);

    p = fork();
    if (p == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        for (int i = 0; i < 20000; i++) syscall(SYS_getppid);
        _exit(0);
    }
    int st;
    waitpid(p, &st, 0);
    ptrace(PTRACE_SYSCALL, p, 0, 0);
    waitpid(p, &st, 0);
    double t = now();
    long stops = 0;
    for (;;) {
        if (ptrace(PTRACE_SYSCALL, p, 0, 0) < 0) break;
        if (waitpid(p, &st, 0) < 0 || WIFEXITED(st)) break;
        stops++;
    }
    printf("ptrace (per syscall)   %.0f ns\n", (now() - t) / (stops / 2.0));
    return 0;
}
