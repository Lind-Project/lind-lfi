// Base-system micro-benchmarks mirroring Table 4 of the Lind/GrateOS paper
// (null syscall, signal catch, fork, exec, pipe/unix-socket round trip, file read
// bandwidth). The same source runs natively and as an LFI cage.
//   b_sys <name>   prints "<name> <value> <unit>"
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}

// A sample is only valid if every operation it timed succeeded.
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "b_sys: check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)
static int child_ok(pid_t p) { int st; return waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 0; }

static volatile int got;
static void h(int s) { (void) s; got++; }

#define REPS(n, body)                                                          \
    do {                                                                       \
        for (int w = 0; w < (n) / 10 + 1; w++) { body; }                       \
        double t0 = now();                                                     \
        for (int i = 0; i < (n); i++) { body; }                                \
        elapsed = (now() - t0) / (n);                                          \
    } while (0)

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    const char *m = argv[1];
    double elapsed = 0;
    if (!strcmp(m, "exit-now")) return 0;
    if (!strcmp(m, "null")) {
        REPS(2000000, getppid());
        printf("null_syscall %.1f ns\n", elapsed);
    } else if (!strcmp(m, "getpid")) {
        REPS(1000000, getpid());
        printf("getpid %.1f ns\n", elapsed);
    } else if (!strcmp(m, "geteuid")) {
        REPS(1000000, geteuid());
        printf("geteuid %.1f ns\n", elapsed);
    } else if (!strcmp(m, "signal")) {
        struct sigaction sa = { .sa_handler = h };
        sigaction(SIGUSR1, &sa, NULL);
        pid_t me = getpid();
        int sent = 0;
        REPS(200000, { CHECK(kill(me, SIGUSR1) == 0); sent++; });
        CHECK(got == sent);   // every signal must have been delivered to the handler
        printf("signal_catch %.1f ns\n", elapsed);
    } else if (!strcmp(m, "fork")) {
        REPS(500, { pid_t p = fork(); CHECK(p >= 0); if (p == 0) _exit(0); CHECK(child_ok(p)); });
        printf("fork_exit_wait %.1f us\n", elapsed / 1000);
    } else if (!strcmp(m, "exec")) {
        REPS(300, { pid_t p = fork(); CHECK(p >= 0); if (p == 0) { execl(argv[0], argv[0], "exit-now", (char *) 0); _exit(127); } CHECK(child_ok(p)); });
        printf("fork_exec_wait %.1f us\n", elapsed / 1000);
    } else if (!strcmp(m, "pipe") || !strcmp(m, "unix")) {
        int a[2], b[2];
        if (m[0] == 'p') { if (pipe(a) || pipe(b)) return 1; }
        else { int s[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, s)) return 1; a[0] = s[0]; a[1] = s[0]; b[0] = s[1]; b[1] = s[1]; }
        pid_t p = fork();
        char c = 'x';
        CHECK(p >= 0);
        if (p == 0) { for (;;) { if (read(a[0], &c, 1) != 1) _exit(0); if (write(b[1], &c, 1) != 1) _exit(1); } }
        c = 'x';
        REPS(100000, { CHECK(write(a[1], &c, 1) == 1); c = 0; CHECK(read(b[0], &c, 1) == 1); CHECK(c == 'x'); });
        printf("%s_roundtrip %.2f us\n", m, elapsed / 1000);
        kill(p, SIGKILL);
    } else if (!strcmp(m, "readbw")) {
        size_t sz = 64u << 20, chunk = 1u << 20;
        const char *path = "/tmp/b_sys.dat";
        int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
        char *buf = malloc(chunk);
        memset(buf, 7, chunk);
        CHECK(fd >= 0 && buf);
        for (size_t o = 0; o < sz; o += chunk) CHECK(write(fd, buf, chunk) == (ssize_t) chunk);
        for (int w = 0; w < 2; w++) { CHECK(lseek(fd, 0, SEEK_SET) == 0); for (size_t o = 0; o < sz; o += chunk) CHECK(read(fd, buf, chunk) == (ssize_t) chunk); }
        double t0 = now();
        int reps = 8;
        for (int r = 0; r < reps; r++) { CHECK(lseek(fd, 0, SEEK_SET) == 0); for (size_t o = 0; o < sz; o += chunk) CHECK(read(fd, buf, chunk) == (ssize_t) chunk); }
        CHECK(buf[0] == 7 && buf[chunk - 1] == 7);
        double s = (now() - t0) / 1e9;
        printf("file_read_bw %.0f MB/s\n", reps * (sz / 1048576.0) / s);
        unlink(path);
    } else return 2;
    return 0;
}
