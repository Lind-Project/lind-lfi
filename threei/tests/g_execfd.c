// exec, close-on-exec, dup2 redirection and descriptor inheritance.
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc, char **argv) {
    assert(argc >= 3);
    const char *out = "/tmp/lfi3i_execfd.txt";
    int f = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(f >= 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        dup2(f, 1);                                         // stdout -> file
        int cloexec = open("/dev/null", O_RDONLY | O_CLOEXEC);   // fd 4 would be this...
        close(cloexec);
        int a = open("/dev/null", O_RDONLY | O_CLOEXEC);    // -> fd 3? (3 is f, so a == 4)
        (void)a;
        execv(argv[1], &argv[1]);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(f);
    char buf[256] = {0};
    int r = open(out, O_RDONLY);
    int n = read(r, buf, sizeof buf - 1);
    printf("[execfd] child output (%d bytes): %s", n, buf);
    assert(strstr(buf, "[hello] pid=") != NULL);
    close(r);

    // Descriptor flags across exec.
    int fd3 = open("/dev/null", O_RDWR | O_CLOEXEC);     // gets fd 3? (3 is free again: f=3 closed)
    int pfd[2];
    assert(pipe(pfd) == 0);
    printf("[execfd] fds: cloexec=%d pipe=%d,%d\n", fd3, pfd[0], pfd[1]);
    pid = fork();
    if (pid == 0) {
        // Arrange fd 3 = cloexec, fd 4 = inheritable pipe write end.
        dup2(fd3, 3);
        fcntl(3, F_SETFD, FD_CLOEXEC);
        dup2(pfd[1], 4);
        execv(argv[2], &argv[2]);
        _exit(127);
    }
    close(pfd[1]);
    char c;
    assert(read(pfd[0], &c, 1) == 1 && c == 'x');
    waitpid(pid, &st, 0);
    printf("[execfd] fdcheck exit %d\n", WEXITSTATUS(st));
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    unlink(out);

    // execv of a missing file fails cleanly and the caller continues.
    pid = fork();
    if (pid == 0) {
        execv("/no/such/program", argv);
        _exit(55);
    }
    waitpid(pid, &st, 0);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 55);
    printf("[execfd] PASS\n");
    return 0;
}
