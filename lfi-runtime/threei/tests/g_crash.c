// Runs g_crash_grate (which crashes while serving its cage) and checks that
// only the grate died: the runtime survives and the cage finishes normally.
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc, char **argv) {
    assert(argc >= 3);
    unlink("/tmp/lfi3i_crash_ok");
    pid_t pid = fork();
    if (pid == 0) { execv(argv[1], &argv[1]); _exit(127); }
    int st, grate_signaled = 0, n = 0;
    pid_t w;
    while ((w = wait(&st)) > 0) {
        n++;
        printf("[crash] cage %d finished with status %d\n", w, st);
        if (w == pid) grate_signaled = WIFSIGNALED(st) && WTERMSIG(st) == 11;
        else assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    // The cage may have been reaped by the grate before the grate died.
    assert(n >= 1 && grate_signaled);
    assert(access("/tmp/lfi3i_crash_ok", F_OK) == 0);
    unlink("/tmp/lfi3i_crash_ok");
    printf("[crash] PASS\n");
    return 0;
}
