// Sockets, select/poll/epoll, message passing.
#include <arpa/inet.h>
#include <assert.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static void tcp_echo(void) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    assert(srv >= 0);
    int one = 1;
    assert(setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    assert(bind(srv, (struct sockaddr *)&a, sizeof a) == 0);
    socklen_t al = sizeof a;
    assert(getsockname(srv, (struct sockaddr *)&a, &al) == 0 && al == sizeof a);
    assert(a.sin_port != 0);
    assert(listen(srv, 4) == 0);
    int v = 0; socklen_t vl = sizeof v;
    assert(getsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &v, &vl) == 0 && v);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(srv);
        int c = socket(AF_INET, SOCK_STREAM, 0);
        assert(connect(c, (struct sockaddr *)&a, sizeof a) == 0);
        assert(send(c, "ping", 4, 0) == 4);
        char b[8] = {0};
        assert(recv(c, b, sizeof b, 0) == 4 && !memcmp(b, "PONG", 4));
        _exit(0);
    }
    struct sockaddr_in peer; socklen_t pl = sizeof peer;
    int s = accept(srv, (struct sockaddr *)&peer, &pl);
    assert(s >= 0 && peer.sin_family == AF_INET && pl == sizeof peer);
    char b[8] = {0};
    assert(read(s, b, sizeof b) == 4 && !memcmp(b, "ping", 4));
    assert(write(s, "PONG", 4) == 4);
    close(s);
    int st; waitpid(pid, &st, 0);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(srv);
    printf("[net] tcp echo ok (port %d)\n", ntohs(a.sin_port));
}

static void unix_socket(void) {
    const char *path = "/tmp/lfi3i_net.sock";
    unlink(path);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strcpy(a.sun_path, path);
    assert(bind(s, (struct sockaddr *)&a, sizeof a) == 0);
    assert(listen(s, 1) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        assert(connect(c, (struct sockaddr *)&a, sizeof a) == 0);
        assert(write(c, "unix", 4) == 4);
        _exit(0);
    }
    int c = accept4(s, NULL, NULL, SOCK_CLOEXEC);
    assert(c >= 0);
    char b[8] = {0};
    assert(read(c, b, 4) == 4 && !memcmp(b, "unix", 4));
    waitpid(pid, NULL, 0);
    unlink(path);
    // The abstract namespace is shared with the host and not available.
    struct sockaddr_un ab = { .sun_family = AF_UNIX };
    memcpy(ab.sun_path + 1, "lfi3i", 5);
    int t = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(bind(t, (struct sockaddr *)&ab, offsetof(struct sockaddr_un, sun_path) + 6) == -1 && errno == EACCES);
    printf("[net] unix socket ok\n");
}

static void msgs_and_pairs(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    char a1[] = "hello ", a2[] = "scatter";
    struct iovec iov[2] = { { a1, 6 }, { a2, 7 } };
    struct msghdr m = { .msg_iov = iov, .msg_iovlen = 2 };
    assert(sendmsg(sv[0], &m, 0) == 13);
    char r1[4] = {0}, r2[16] = {0};
    struct iovec riov[2] = { { r1, 3 }, { r2, 10 } };
    struct msghdr rm = { .msg_iov = riov, .msg_iovlen = 2 };
    assert(recvmsg(sv[1], &rm, 0) == 13);
    assert(!memcmp(r1, "hel", 3) && !memcmp(r2, "lo scatter", 10));
    // Passing descriptors is not supported.
    char cbuf[CMSG_SPACE(sizeof(int))] = {0};
    struct msghdr cm = { .msg_iov = iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
    assert(sendmsg(sv[0], &cm, 0) == -1 && errno == EOPNOTSUPP);
    shutdown(sv[0], SHUT_WR);
    char z;
    assert(read(sv[1], &z, 1) == 0);
    printf("[net] sendmsg/recvmsg/socketpair ok\n");
}

static void readiness(void) {
    int p[2];
    assert(pipe(p) == 0);
    // select
    fd_set rf; FD_ZERO(&rf); FD_SET(p[0], &rf);
    struct timeval tv = { 0, 50000 };
    assert(select(p[0] + 1, &rf, NULL, NULL, &tv) == 0);
    assert(write(p[1], "x", 1) == 1);
    FD_ZERO(&rf); FD_SET(p[0], &rf);
    assert(select(p[0] + 1, &rf, NULL, NULL, &tv) == 1 && FD_ISSET(p[0], &rf));
    // poll
    struct pollfd pf[2] = { { p[0], POLLIN, 0 }, { 999, POLLIN, 0 } };
    assert(poll(pf, 2, 100) == 2);
    assert((pf[0].revents & POLLIN) && pf[1].revents == POLLNVAL);
    // epoll
    int ep = epoll_create1(0);
    assert(ep >= 0);
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0x1234 };
    assert(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
    struct epoll_event out[4];
    assert(epoll_wait(ep, out, 4, 100) == 1 && out[0].data.u64 == 0x1234);
    char b; read(p[0], &b, 1);
    assert(epoll_wait(ep, out, 4, 20) == 0);
    assert(epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL) == 0);
    // A process blocked in poll is woken by data written by another process.
    pid_t pid = fork();
    if (pid == 0) { usleep(50000); write(p[1], "y", 1); _exit(0); }
    struct pollfd w = { p[0], POLLIN, 0 };
    assert(poll(&w, 1, 5000) == 1);
    waitpid(pid, NULL, 0);
    printf("[net] select/poll/epoll ok\n");
}

int main(void) {
    tcp_echo();
    unix_socket();
    msgs_and_pairs();
    readiness();
    printf("[net] PASS\n");
    return 0;
}
