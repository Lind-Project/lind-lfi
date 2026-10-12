// mknod (FIFO, regular file, socket; not devices) and per-process nice values.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s (errno %d)\n", __LINE__, #c, errno); return 1; } } while (0)

int main(void)
{
    char dir[] = "/tmp/mknod.XXXXXX";
    CHECK(mkdtemp(dir));
    char p[128];
    struct stat st;

    snprintf(p, sizeof p, "%s/fifo", dir);
    CHECK(mkfifo(p, 0600) == 0);
    CHECK(stat(p, &st) == 0 && S_ISFIFO(st.st_mode));
    // the FIFO works: a child writes, we read
    pid_t pid = fork();
    if (pid == 0) { int w = open(p, O_WRONLY); write(w, "hi", 2); _exit(0); }
    int r = open(p, O_RDONLY);
    char b[4] = {0};
    CHECK(read(r, b, 2) == 2 && !memcmp(b, "hi", 2));
    waitpid(pid, NULL, 0);

    snprintf(p, sizeof p, "%s/reg", dir);
    CHECK(mknod(p, S_IFREG | 0640, 0) == 0);
    CHECK(stat(p, &st) == 0 && S_ISREG(st.st_mode));
    CHECK(mknod(p, S_IFREG | 0640, 0) == -1 && errno == EEXIST);

    snprintf(p, sizeof p, "%s/dev", dir);
    CHECK(mknod(p, S_IFCHR | 0600, makedev(1, 3)) == -1 && errno == EPERM);

    // nice: starts at 0, can be raised, not lowered, inherited by a child
    errno = 0;
    CHECK(getpriority(PRIO_PROCESS, 0) == 0 && errno == 0);
    CHECK(setpriority(PRIO_PROCESS, 0, 5) == 0);
    CHECK(getpriority(PRIO_PROCESS, 0) == 5);
    CHECK(setpriority(PRIO_PROCESS, 0, 1) == -1 && errno == EACCES);
    if (fork() == 0) _exit(getpriority(PRIO_PROCESS, 0) == 5 ? 0 : 1);
    int status; wait(&status);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    printf("ok\n");
    return 0;
}
