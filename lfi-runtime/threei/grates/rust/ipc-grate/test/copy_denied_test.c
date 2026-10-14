// Run as: testing-grate -s 1002:-1 ipc-grate copy_denied_test
// The outer grate denies the IPC grate's copy_data_between_cages calls. The IPC grate must
// report that to its caller as a failure (EFAULT), not as a successful getsockopt/read.
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int fails = 0;
    // socket() hands back its fd directly (pipe() and socketpair() would copy the fds out, which
    // is exactly what is denied here).
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { puts("setup failed"); return 2; }

    int type = -1;
    socklen_t len = sizeof type;
    errno = 0;
    int r = getsockopt(s, SOL_SOCKET, SO_TYPE, &type, &len);
    printf("getsockopt with copies denied: r=%d errno=%d type=%d\n", r, errno, type);
    if (!(r == -1 && errno == EFAULT && type == -1)) { puts("FAIL: getsockopt did not report the denied copy"); fails++; }

    puts(fails ? "copy_denied: FAILED" : "copy_denied: PASS");
    return fails ? 1 : 0;
}
