// A grate learns that a cage it interposes on died abnormally.
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include "lind_syscall.h"

static volatile int notified;
static volatile int last_code;
static volatile int forwarded_result;

static int on_exit(uint64_t cageid, uint64_t code, uint64_t c1, uint64_t a2, uint64_t c2,
                   uint64_t a3, uint64_t c3, uint64_t a4, uint64_t c4, uint64_t a5,
                   uint64_t c5, uint64_t a6, uint64_t c6) {
    notified++;
    last_code = (int)code;
    // Forwarding the exit of a cage that is already dying is harmless.
    forwarded_result = make_threei_call(231, 0, getpid(), c1, code, c1, 0, c1, 0, c1,
                                        0, c1, 0, c1, 0, c1, 0);
    return 0;
}

static pid_t launch(char **argv, int *me) {
    pid_t pid = fork();
    if (pid == 0) {
        assert(register_handler(getpid(), 231, *me, (uint64_t)(uintptr_t)&on_exit) == 0);
        execv(argv[0], argv);
        _exit(127);
    }
    return pid;
}

int main(int argc, char **argv) {
    assert(argc >= 3);
    int me = getpid();
    // 1. Killed: the handler is told.
    pid_t pid = launch(&argv[1], &me);
    usleep(150000);
    kill(pid, SIGKILL);
    int st; assert(waitpid(pid, &st, 0) == pid);
    assert(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    printf("[harsh] killed: notified=%d code=%d forwarded=%d\n", notified, last_code, forwarded_result);
    assert(notified == 1 && last_code == 128 + SIGKILL);
    // 2. A normal exit goes through the exit call itself, once.
    notified = 0;
    pid = launch(&argv[2], &me);
    assert(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    printf("[harsh] normal exit: notified=%d\n", notified);
    assert(notified == 1);
    printf("[harsh] PASS\n");
    return 0;
}
