// Pointers that are inside the sandbox but not accessible (PROT_NONE page, unmapped hole) must
// make the call fail with EFAULT. Used to take the whole runtime down. A bystander cage keeps
// running throughout and must be unaffected.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails, idx_, only_ = -1;
#define EXPECT_EFAULT(what, expr) do {                                               \
    if (only_ >= 0 && ++idx_ != only_) break;                                        \
    errno = 0; long r_ = (long) (expr);                                              \
    if (getenv("BADPTR_TRACE")) { printf("  %s -> r=%ld errno=%d\n", what, r_, errno); fcntl(1, F_GETFD); printf("    (fd table ok)\n"); } \
    if (!(r_ == -1 && errno == EFAULT)) {                                            \
        printf("FAIL: %s: r=%ld errno=%d (wanted -1/EFAULT)\n", what, r_, errno);   \
        fails++;                                                                     \
    }                                                                                \
} while (0)

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getenv("BADPTR_ONLY")) only_ = atoi(getenv("BADPTR_ONLY"));
    int p[2];
    if (pipe(p)) return 2;
    pid_t by = fork();                      // the bystander: counts to 200 and reports
    if (by == 0) {
        for (int i = 0; i < 200; i++) { usleep(2000); if (getppid() < 0) _exit(3); }
        _exit(0);
    }

    char *none = mmap(NULL, 8192, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *hole = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (none == MAP_FAILED || hole == MAP_FAILED) return 2;
    munmap(hole, 8192);                     // an unmapped hole inside the sandbox
    char okbuf[256];

    EXPECT_EFAULT("open(PROT_NONE path)", open(none, O_RDONLY));
    EXPECT_EFAULT("open(unmapped path)", open(hole, O_RDONLY));
    EXPECT_EFAULT("stat(PROT_NONE path)", syscall(SYS_stat, none, okbuf));
    // (raw system calls: musl's stat() wrapper writes the result in user code, which faults the cage itself)
    EXPECT_EFAULT("stat(.., PROT_NONE buf)", syscall(SYS_stat, "/", none));
    EXPECT_EFAULT("access(unmapped path)", access(hole, F_OK));
    EXPECT_EFAULT("execve(PROT_NONE path)", execve(none, (char *[]) { none, NULL }, NULL));
    EXPECT_EFAULT("getcwd(PROT_NONE)", (long) (getcwd(none, 100) ? 0 : -1));
    EXPECT_EFAULT("uname(PROT_NONE)", uname((struct utsname *) none));
    EXPECT_EFAULT("clock_gettime(PROT_NONE)", clock_gettime(CLOCK_REALTIME, (struct timespec *) none));
    EXPECT_EFAULT("pipe(PROT_NONE)", pipe((int *) none));
    EXPECT_EFAULT("writev(PROT_NONE iov array)", writev(1, (struct iovec *) none, 1));
    EXPECT_EFAULT("readv(unmapped iov array)", readv(p[0], (struct iovec *) hole, 1));
    EXPECT_EFAULT("socketpair(unmapped)", socketpair(AF_UNIX, SOCK_STREAM, 0, (int *) hole));
    EXPECT_EFAULT("nanosleep(PROT_NONE)", nanosleep((struct timespec *) none, NULL));
    EXPECT_EFAULT("mkdir(PROT_NONE path)", mkdir(none, 0700));
    EXPECT_EFAULT("rename(PROT_NONE, ok)", rename(none, "/tmp/x"));
    EXPECT_EFAULT("readlink(PROT_NONE path)", readlink(none, okbuf, sizeof okbuf));

    // the cage is still fully functional afterwards
    if (getenv("BADPTR_TRACE")) puts("  -- after the faults");
    if (write(p[1], "ok", 2) != 2) { puts("FAIL: write after faults"); fails++; }
    if (getenv("BADPTR_TRACE")) puts("  -- write ok");
    if (open("/", O_RDONLY) < 0) { puts("FAIL: open after faults"); fails++; }
    if (getenv("BADPTR_TRACE")) puts("  -- open ok");

    if (getenv("BADPTR_TRACE")) puts("  -- waiting for the bystander");
    int st;
    if (waitpid(by, &st, 0) != by || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("FAIL: the bystander cage did not finish cleanly (status %d)\n", st);
        fails++;
    }
    printf(fails ? "badptr: %d FAILED\n" : "badptr: PASS\n", fails);
    return fails ? 1 : 0;
}
