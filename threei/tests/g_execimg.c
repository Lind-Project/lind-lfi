// F06 regression: a grate that replaces its program image (exec) must not keep serving calls
// through handler offsets and execution contexts of the old image. Image 1 serves getpid for the
// app cage (answer 777), then execs itself; image 2 first checks that the old registration is dead
// (the app's getpid is no longer answered with 777) and then registers its own handler (778).
//   g_execimg <app>                     image 1
//   g_execimg x2 <rfd> <wfd> <appid>    image 2 (internal)
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lind_syscall.h"

#define H(name, ret)                                                                 \
    static int name(uint64_t g, uint64_t a1, uint64_t c1, uint64_t a2, uint64_t c2,  \
        uint64_t a3, uint64_t c3, uint64_t a4, uint64_t c4, uint64_t a5, uint64_t c5, \
        uint64_t a6, uint64_t c6) { return (ret); }
H(h777, 777)
H(h778, 778)

static int ask(int to_app, int from_app)
{
    int v = -12345;
    assert(write(to_app, "g", 1) == 1);
    assert(read(from_app, &v, sizeof v) == sizeof v);
    return v;
}

int main(int argc, char **argv)
{
    if (argc >= 5 && !strcmp(argv[1], "x2")) {
        int to_app = atoi(argv[2]), from_app = atoi(argv[3]);
        uint64_t app = strtoull(argv[4], 0, 10);
        int before = ask(to_app, from_app);
        printf("[execimg] after exec, before re-registering: app getpid = %d\n", before);
        assert(before != 777 && before != 778);   // the old image's registration is dead
        assert(register_handler(app, 39, getpid(), (uint64_t) (uintptr_t) h778) == 0);
        int after = ask(to_app, from_app);
        printf("[execimg] after re-registering: app getpid = %d\n", after);
        assert(after == 778);
        assert(write(to_app, "q", 1) == 1);
        int st;
        assert(waitpid((pid_t) app, &st, 0) == (pid_t) app);
        assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        printf("[execimg] PASS\n");
        return 0;
    }
    assert(argc >= 2);
    int up[2], down[2];                       // app -> grate answers, grate -> app commands
    assert(pipe(up) == 0 && pipe(down) == 0);
    pid_t app = fork();
    assert(app >= 0);
    if (app == 0) {
        char a1[16], a2[16];
        snprintf(a1, sizeof a1, "%d", down[0]);
        snprintf(a2, sizeof a2, "%d", up[1]);
        execl(argv[1], argv[1], a1, a2, (char *) 0);
        _exit(127);
    }
    assert(register_handler(app, 39, getpid(), (uint64_t) (uintptr_t) h777) == 0);
    int first = ask(down[1], up[0]);
    printf("[execimg] image 1: app getpid = %d\n", first);
    assert(first == 777);
    char b1[16], b2[16], b3[16];
    snprintf(b1, sizeof b1, "%d", down[1]);
    snprintf(b2, sizeof b2, "%d", up[0]);
    snprintf(b3, sizeof b3, "%d", (int) app);
    execl(argv[0], argv[0], "x2", b1, b2, b3, (char *) 0);
    perror("exec");
    return 1;
}
