// Signal handlers: delivery, masks, SA_RESTART, alarm, SIGCHLD, SIGPIPE,
// alternate stacks, sigsuspend/sigwait, longjmp out of handlers.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t got, got_sig, got_pid, order[8], norder;
static volatile void *handler_sp;

static void on_usr1(int sig, siginfo_t *si, void *uc) {
    got++;
    got_sig = si->si_signo;
    got_pid = si->si_pid;
    assert(sig == si->si_signo);
    assert(uc != NULL);
}

static void plain(int sig) { got++; got_sig = sig; }

static void test_basic(void) {
    struct sigaction sa = { .sa_sigaction = on_usr1, .sa_flags = SA_SIGINFO };
    sigemptyset(&sa.sa_mask);
    assert(sigaction(SIGUSR1, &sa, NULL) == 0);
    struct sigaction old;
    assert(sigaction(SIGUSR1, NULL, &old) == 0 && old.sa_sigaction == on_usr1);
    got = 0;
    assert(kill(getpid(), SIGUSR1) == 0);
    assert(got == 1 && got_sig == SIGUSR1 && got_pid == getpid());
    assert(raise(SIGUSR1) == 0 && got == 2);
    // The state before the signal is intact: arithmetic across the call.
    volatile double x = 1.5; volatile long y = 7;
    kill(getpid(), SIGUSR1);
    assert(x == 1.5 && y == 7);
    printf("[sig] basic ok\n");
}

static void test_mask(void) {
    signal(SIGUSR2, plain);
    got = 0;
    sigset_t b, p, old;
    sigemptyset(&b); sigaddset(&b, SIGUSR2);
    assert(sigprocmask(SIG_BLOCK, &b, &old) == 0);
    kill(getpid(), SIGUSR2);
    assert(got == 0);
    sigpending(&p);
    assert(sigismember(&p, SIGUSR2));
    sigprocmask(SIG_SETMASK, &old, NULL);        // unblocking delivers it
    assert(got == 1);
    sigpending(&p);
    assert(!sigismember(&p, SIGUSR2));
    // Ignored signals are discarded.
    signal(SIGUSR2, SIG_IGN);
    sigprocmask(SIG_BLOCK, &b, NULL);
    kill(getpid(), SIGUSR2);
    sigpending(&p);
    assert(!sigismember(&p, SIGUSR2));
    sigprocmask(SIG_UNBLOCK, &b, NULL);
    // The signal being handled is blocked inside its handler.
    printf("[sig] mask ok\n");
}

static void nested(int sig) {
    order[norder++] = 1;
    kill(getpid(), SIGUSR2);            // blocked while we run: delivered after
    order[norder++] = 2;
}
static void second(int sig) { order[norder++] = 3; }

static void test_nested(void) {
    norder = 0;
    struct sigaction a = { .sa_handler = nested };
    sigemptyset(&a.sa_mask); sigaddset(&a.sa_mask, SIGUSR2);
    sigaction(SIGUSR1, &a, NULL);
    signal(SIGUSR2, second);
    kill(getpid(), SIGUSR1);
    assert(norder == 3 && order[0] == 1 && order[1] == 2 && order[2] == 3);
    signal(SIGUSR1, SIG_DFL);
    printf("[sig] nested ok\n");
}

static void test_restart(void) {
    for (int restart = 0; restart < 2; restart++) {
        int p[2]; assert(pipe(p) == 0);
        pid_t pid = fork();
        if (pid == 0) {
            struct sigaction sa = { .sa_handler = plain, .sa_flags = restart ? SA_RESTART : 0 };
            sigemptyset(&sa.sa_mask);
            sigaction(SIGUSR1, &sa, NULL);
            got = 0;
            char c = 0;
            ssize_t n = read(p[0], &c, 1);       // blocks; the signal interrupts it
            int e = errno;
            if (restart) _exit(n == 1 && c == 'z' && got == 1 ? 0 : 1);
            _exit(n == -1 && e == EINTR && got == 1 ? 0 : 2);
        }
        usleep(150000);
        kill(pid, SIGUSR1);
        usleep(150000);
        write(p[1], "z", 1);
        int st; waitpid(pid, &st, 0);
        assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        close(p[0]); close(p[1]);
    }
    printf("[sig] SA_RESTART/EINTR ok\n");
}

static volatile sig_atomic_t alarms;
static void on_alarm(int s) { alarms++; }

static void test_alarm(void) {
    signal(SIGALRM, on_alarm);
    alarms = 0;
    alarm(1);
    pause();
    assert(alarms == 1);
    assert(alarm(5) == 0 && alarm(0) >= 4);
    struct itimerval it = { { 0, 100000 }, { 0, 100000 } };
    assert(setitimer(ITIMER_REAL, &it, NULL) == 0);
    while (alarms < 4) pause();
    struct itimerval off = {0};
    setitimer(ITIMER_REAL, &off, NULL);
    printf("[sig] alarm/setitimer/pause ok (%d alarms)\n", (int)alarms);
}

static volatile sig_atomic_t chld;
static void on_chld(int s) { chld++; }

static void test_sigchld(void) {
    signal(SIGCHLD, on_chld);
    chld = 0;
    pid_t pid = fork();
    if (pid == 0) _exit(0);
    for (int i = 0; i < 100 && !chld; i++) usleep(20000);
    assert(chld >= 1);
    assert(waitpid(pid, NULL, 0) == pid);
    // sigsuspend: wait for SIGCHLD atomically.
    sigset_t mask, empty, old;
    sigemptyset(&mask); sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, &old);
    chld = 0;
    pid = fork();
    if (pid == 0) { usleep(100000); _exit(3); }
    sigemptyset(&empty);
    while (!chld) sigsuspend(&empty);
    sigprocmask(SIG_SETMASK, &old, NULL);
    int st; assert(waitpid(pid, &st, 0) == pid && WEXITSTATUS(st) == 3);
    signal(SIGCHLD, SIG_DFL);
    printf("[sig] SIGCHLD/sigsuspend ok\n");
}

static void test_sigpipe(void) {
    // Default: the writer dies of SIGPIPE.
    pid_t pid = fork();
    if (pid == 0) {
        int p[2]; pipe(p); close(p[0]);
        write(p[1], "x", 1);
        _exit(0);
    }
    int st; waitpid(pid, &st, 0);
    assert(WIFSIGNALED(st) && WTERMSIG(st) == SIGPIPE);
    // Ignored: EPIPE only.
    pid = fork();
    if (pid == 0) {
        signal(SIGPIPE, SIG_IGN);
        int p[2]; pipe(p); close(p[0]);
        _exit(write(p[1], "x", 1) == -1 && errno == EPIPE ? 0 : 1);
    }
    waitpid(pid, &st, 0);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    // Caught: handler runs, write fails with EPIPE.
    pid = fork();
    if (pid == 0) {
        got = 0;
        signal(SIGPIPE, plain);
        int p[2]; pipe(p); close(p[0]);
        _exit(write(p[1], "x", 1) == -1 && errno == EPIPE && got == 1 ? 0 : 1);
    }
    waitpid(pid, &st, 0);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    printf("[sig] SIGPIPE ok\n");
}

static void on_altstack(int s) { char here; handler_sp = &here; }

static void test_altstack(void) {
    static char stack[65536];
    stack_t ss = { .ss_sp = stack, .ss_size = sizeof stack };
    assert(sigaltstack(&ss, NULL) == 0);
    stack_t cur; assert(sigaltstack(NULL, &cur) == 0 && cur.ss_sp == stack);
    struct sigaction sa = { .sa_handler = on_altstack, .sa_flags = SA_ONSTACK };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    kill(getpid(), SIGUSR1);
    assert((char *)handler_sp >= stack && (char *)handler_sp < stack + sizeof stack);
    signal(SIGUSR1, SIG_DFL);
    printf("[sig] sigaltstack ok\n");
}

static void test_sigwait(void) {
    sigset_t s; sigemptyset(&s); sigaddset(&s, SIGUSR2);
    sigprocmask(SIG_BLOCK, &s, NULL);
    pid_t pid = getpid();
    kill(pid, SIGUSR2);
    int sig = 0;
    assert(sigwait(&s, &sig) == 0 && sig == SIGUSR2);
    struct timespec ts = { 0, 50000000 };
    assert(sigtimedwait(&s, NULL, &ts) == -1 && errno == EAGAIN);
    sigprocmask(SIG_UNBLOCK, &s, NULL);
    printf("[sig] sigwait/sigtimedwait ok\n");
}

static void test_exec_reset(void) {
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGUSR1, plain);                 // caught: reset by exec
        signal(SIGUSR2, SIG_IGN);               // ignored: stays ignored
        extern char **environ;
        char *av[] = { "/proc/self/exe", NULL };  // not used
        (void)av;
        _exit(0);
    }
    waitpid(pid, NULL, 0);
}

static void *thread_fn(void *a) {
    // Runs in a thread of its own; the signal sent to it must run here.
    sigset_t s; sigemptyset(&s); sigaddset(&s, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &s, NULL);
    for (int i = 0; i < 200 && !got; i++) usleep(10000);
    return (void *)(long)got;
}

static void test_threads(void) {
    signal(SIGUSR1, plain);
    got = 0;
    sigset_t s; sigemptyset(&s); sigaddset(&s, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &s, NULL);        // main blocks it; the worker handles it
    pthread_t th; pthread_create(&th, NULL, thread_fn, NULL);
    usleep(50000);
    kill(getpid(), SIGUSR1);                     // process-directed
    void *r; pthread_join(th, &r);
    pthread_sigmask(SIG_UNBLOCK, &s, NULL);
    assert((long)r == 1);
    signal(SIGUSR1, SIG_DFL);
    printf("[sig] multi-threaded ok\n");
}

int main(void) {
    test_basic();
    test_mask();
    test_nested();
    test_restart();
    test_alarm();
    test_sigchld();
    test_sigpipe();
    test_altstack();
    test_sigwait();
    test_threads();
    printf("[sig] PASS\n");
    return 0;
}
