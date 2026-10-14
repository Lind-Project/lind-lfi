// With --no-net only UNIX-domain sockets are available.
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
int main(void) {
    assert(socket(AF_INET, SOCK_STREAM, 0) == -1 && errno == EACCES);
    assert(socket(AF_INET6, SOCK_DGRAM, 0) == -1 && errno == EACCES);
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    int u = socket(AF_UNIX, SOCK_DGRAM, 0);
    assert(u >= 0);
    printf("[nonet] PASS\n");
    return 0;
}
