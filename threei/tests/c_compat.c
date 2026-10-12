#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int bad = 0;
    struct epoll_event ev[4];
    // musl's epoll_wait is epoll_pwait(…, sigmask = NULL)
    int e = epoll_wait(3 /* any */, ev, 4, 0);
    if (e != 4242) { printf("[cage] epoll_wait = %d\n", e); bad |= 1; }
    // musl's stat is newfstatat(AT_FDCWD, path, buf, 0)
    struct stat st;
    int s = stat("/", &st);
    if (s != 4343) { printf("[cage] stat = %d\n", s); bad |= 2; }
    // fchmodat has three arguments in the kernel; a fourth is zero for a handler
    int c = fchmodat(AT_FDCWD, "/nonexistent", 0600, 0);
    if (c != 0) { printf("[cage] fchmodat = %d\n", c); bad |= 4; }
    // A string at the very end of a mapping, for the grate to copy 64 bytes from.
    if (argc >= 3) {
        char *pg = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        munmap(pg + 4096, 4096);
        char *tail = pg + 4096 - 6;
        memcpy(tail, "hello", 6);
        unsigned long long where = (unsigned long long) (unsigned long) tail;
        if (write(atoi(argv[1]), &where, sizeof where) != sizeof where) bad |= 8;
        char x;
        if (read(atoi(argv[2]), &x, 1) != 1) bad |= 16; // the grate has copied
    }
    return bad;
}
