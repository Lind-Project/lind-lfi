// File descriptor operations a tracing grate should be able to forward.
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(void) {
    const char *path = "/tmp/lfi3i_fdio.txt";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, "one\n", 4) == 4);
    assert(write(fd, "two\n", 4) == 4);
    assert(write(fd, "three\n", 6) == 6);
    int fd2 = dup(fd);
    assert(fd2 >= 0 && fd2 != fd);
    assert(lseek(fd2, 0, SEEK_SET) == 0);   // shared offset
    char buf[32] = {0};
    assert(read(fd, buf, sizeof buf - 1) == 14);
    assert(strcmp(buf, "one\ntwo\nthree\n") == 0);
    close(fd2);
    close(fd);
    int p[2];
    assert(pipe(p) == 0);
    assert(write(p[1], "xyz", 3) == 3);
    memset(buf, 0, sizeof buf);
    assert(read(p[0], buf, 3) == 3 && strcmp(buf, "xyz") == 0);
    close(p[0]); close(p[1]);
    assert(unlink(path) == 0);
    printf("[cage] fdio done\n");
    return 0;
}
