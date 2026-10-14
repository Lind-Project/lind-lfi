// kill, fatal signals, fault containment and exit statuses.
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void *spin_thread(void *a) { volatile unsigned long n = 0; for (;;) n++; return 0; }

static int run_child(void (*fn)(void)) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) { fn(); _exit(99); }
    int st;
    assert(waitpid(pid, &st, 0) == pid);
    return st;
}

static void exit_7(void) { exit(7); }
static void null_deref(void) { *(volatile int *)0 = 1; }
static void wild_ptr(void) { *(volatile long *)0x123456789abcUL = 1; }
static void do_abort(void) { abort(); }
static void self_kill(void) { kill(getpid(), SIGTERM); }
static void ill(void) { __builtin_trap(); }
static int recurse(volatile int n) {
    volatile char big[4096];
    big[0] = (char)n;
    big[4095] = 1;
    return recurse(n + 1) + big[0] + big[4095];   // not a tail call
}
static void stack_overflow(void) { exit(recurse(0)); }

int main(void) {
    int st;
    fprintf(stderr, "step: exit_7\n"); st = run_child(exit_7);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 7);
    fprintf(stderr, "step: null_deref\n"); st = run_child(null_deref);
    printf("[signals] null deref -> status %d\n", st);
    assert(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
    fprintf(stderr, "step: wild_ptr\n"); st = run_child(wild_ptr);
    assert(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
    fprintf(stderr, "step: stack_overflow\n"); st = run_child(stack_overflow);
    assert(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
    fprintf(stderr, "step: ill\n"); st = run_child(ill);
    assert(WIFSIGNALED(st));
    fprintf(stderr, "step: do_abort\n"); st = run_child(do_abort);
    printf("[signals] abort -> status %d\n", st);
    assert(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT);
    fprintf(stderr, "step: self_kill\n"); st = run_child(self_kill);
    assert(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);

    // Kill a child blocked in read().
    int p[2];
    assert(pipe(p) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        char c;
        read(p[0], &c, 1);          // blocks forever
        _exit(0);
    }
    usleep(100000);
    assert(kill(pid, SIGKILL) == 0);
    assert(waitpid(pid, &st, 0) == pid);
    printf("[signals] killed blocked reader -> status %d\n", st);
    assert(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);

    // A cage spinning in user code (no system calls at all) is still stopped.
    pid = fork();
    if (pid == 0) { volatile unsigned long n = 0; for (;;) n++; }
    usleep(100000);
    assert(kill(pid, SIGKILL) == 0);
    assert(waitpid(pid, &st, 0) == pid);
    printf("[signals] killed spinning cage -> status %d\n", st);
    assert(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    // ... also one whose spinning is done by a second thread.
    pid = fork();
    if (pid == 0) {
        extern void *spin_thread(void *);
        pthread_t th; pthread_create(&th, 0, spin_thread, 0);
        pthread_join(th, 0);
        _exit(0);
    }
    usleep(150000);
    assert(kill(pid, SIGTERM) == 0);
    assert(waitpid(pid, &st, 0) == pid);
    assert(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);

    // Signal 0 probes for existence; unknown pids fail.
    assert(kill(getpid(), 0) == 0);
    assert(kill(4000, 0) == -1 && errno == ESRCH);
    // A child may not signal its parent.
    pid = fork();
    if (pid == 0) {
        int r = kill(getppid(), SIGKILL);
        _exit(r == -1 && errno == EPERM ? 0 : 1);
    }
    waitpid(pid, &st, 0);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    printf("[signals] PASS\n");
    return 0;
}
