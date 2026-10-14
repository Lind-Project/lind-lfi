// Authorization: what a cage may and may not do to others.
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "lind_syscall.h"

static int dummy(uint64_t cageid, uint64_t a1, uint64_t c1, uint64_t a2, uint64_t c2,
                 uint64_t a3, uint64_t c3, uint64_t a4, uint64_t c4, uint64_t a5,
                 uint64_t c5, uint64_t a6, uint64_t c6) { return 1; }

static char secret[32] = "grate secret";

int main(void) {
    int me = getpid();
    int p[2];
    assert(pipe(p) == 0);
    // Sibling B, left alive until the end.
    pid_t b = fork();
    if (b == 0) { char c; read(p[0], &c, 1); _exit(0); }

    pid_t a = fork();
    if (a == 0) {
        int self = getpid();
        int fails = 0;
        // 1. A cage may not redirect its parent's (or a sibling's) calls.
        if (register_handler(me, 107, self, (uint64_t)(uintptr_t)&dummy) != -EPERM) fails |= 1;
        if (register_handler(b, 107, self, (uint64_t)(uintptr_t)&dummy) != -EPERM) fails |= 2;
        // 2. Nor point its own calls at an unrelated cage's code.
        if (register_handler(self, 107, b, (uint64_t)(uintptr_t)&dummy) != -EPERM) fails |= 4;
        // 3. It cannot read its parent's memory.
        char buf[32] = {0};
        int r = copy_data_between_cages(self, me, (uint64_t)secret, me, (uint64_t)buf, self, 13, 0);
        // (every copy_data_between_cages failure is ELINDAPIABORTED, as in Lind)
        if (r != (int) ELINDAPIABORTED || buf[0] != 0) fails |= 8;
        // 4. Or write into a sibling's.
        r = copy_data_between_cages(self, self, (uint64_t)buf, self, (uint64_t)secret, b, 4, 0);
        if (r != (int) ELINDAPIABORTED) fails |= 16;
        // 5. Or signal them.
        if (kill(b, SIGKILL) != -1 || errno != EPERM) fails |= 32;
        // 6. It may register for itself, with its parent as the handler.
        if (register_handler(self, 107, me, (uint64_t)(uintptr_t)&dummy) != 0) fails |= 64;
        // 7. A forged self id is rejected.
        if (make_threei_call(39, 0, me, me, 0, me, 0, me, 0, me, 0, me, 0, me, 0, me, 0) != -EPERM) fails |= 128;
        _exit(fails);
    }
    int st;
    waitpid(a, &st, 0);
    printf("[perm] checks failed mask: %d\n", WEXITSTATUS(st));
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    // The parent may act on its descendants.
    assert(register_handler(a == 0 ? 0 : b, 107, me, (uint64_t)(uintptr_t)&dummy) == 0);
    assert(kill(b, SIGKILL) == 0);
    waitpid(b, &st, 0);
    assert(WIFSIGNALED(st));
    printf("[perm] PASS\n");
    return 0;
}
