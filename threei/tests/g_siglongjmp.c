// siglongjmp out of signal handlers: the handler never returns through the
// restorer, so the runtime must discard the abandoned frames. (Needs
// --no-verify with the v0.12 prebuilt toolchain: its sigsetjmp/longjmp do not
// pass the current verifier.)
#define _GNU_SOURCE
#include <assert.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static volatile int got;
static void plain(int sig) { got++; }

static sigjmp_buf jb;
static void jumper(int s) { siglongjmp(jb, 1); }

int main(void) {
    struct sigaction sa = { .sa_handler = jumper };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    for (int i = 0; i < 2000; i++) {
        if (sigsetjmp(jb, 1) == 0) {
            kill(getpid(), SIGUSR1);
            assert(0);
        }
    }
    // A normal signal still works afterwards (the abandoned frames are gone).
    signal(SIGUSR1, plain);
    got = 0; kill(getpid(), SIGUSR1); assert(got == 1);
    signal(SIGUSR1, SIG_DFL);
    printf("[siglongjmp] PASS\n");
    return 0;
}

