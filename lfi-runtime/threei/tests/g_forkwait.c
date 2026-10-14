// fork, exit status, wait, pipes between processes, descriptor inheritance.
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lfi_3i_guest.h"

int main(void) {
    int me = getpid();
    printf("[forkwait] cage %d, ppid %d\n", me, getppid());

    int fds[2];
    assert(pipe(fds) == 0);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        const char *msg = "hello from child";
        write(fds[1], msg, strlen(msg));
        close(fds[1]);
        printf("[forkwait child] pid=%d ppid=%d\n", getpid(), getppid());
        fflush(stdout);
        _exit(42);
    }
    close(fds[1]);
    char buf[64] = {0};
    ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
    printf("[forkwait] parent read %zd bytes: '%s'\n", n, buf);
    assert(strcmp(buf, "hello from child") == 0);

    int status = 0;
    pid_t w = waitpid(pid, &status, 0);
    printf("[forkwait] waited for %d, status=%d exited=%d code=%d\n", w, status,
           WIFEXITED(status), WEXITSTATUS(status));
    assert(w == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 42);

    // No children left.
    assert(wait(&status) == -1);

    // Heap and stack state is copied (not shared).
    static int counter = 100;
    int *heap = malloc(sizeof(int));
    *heap = 7;
    pid = fork();
    if (pid == 0) {
        counter += 1;
        *heap += 1;
        _exit(counter == 101 && *heap == 8 ? 0 : 1);
    }
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(counter == 100 && *heap == 7);

    // A file shared across fork keeps one offset.
    int fd = open("/tmp/lfi3i_forkwait.txt", O_RDWR | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    pid = fork();
    if (pid == 0) {
        write(fd, "abc", 3);
        _exit(0);
    }
    waitpid(pid, &status, 0);
    write(fd, "def", 3);
    lseek(fd, 0, SEEK_SET);
    char fb[8] = {0};
    read(fd, fb, 6);
    printf("[forkwait] shared offset file: '%s'\n", fb);
    assert(strcmp(fb, "abcdef") == 0);
    close(fd);
    unlink("/tmp/lfi3i_forkwait.txt");

    printf("[forkwait] PASS\n");
    return 0;
}
