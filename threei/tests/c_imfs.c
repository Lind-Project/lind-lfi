#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(void) {
    int fd = open("/mem/greeting.txt", O_RDWR | O_CREAT, 0600);
    assert(fd >= 2000);                                   // a file-system-grate descriptor
    const char *msg = "hello from the in-memory file system";
    assert(write(fd, msg, strlen(msg)) == (ssize_t)strlen(msg));
    assert(lseek(fd, 0, SEEK_SET) == 0);
    char buf[64] = {0};
    assert(read(fd, buf, sizeof buf) == (ssize_t)strlen(msg));
    printf("[cage] read back: '%s'\n", buf);
    assert(strcmp(buf, msg) == 0);
    assert(close(fd) == 0);
    // Reopen: the data persists in the grate.
    fd = open("/mem/greeting.txt", O_RDONLY);
    assert(fd >= 2000);
    memset(buf, 0, sizeof buf);
    assert(read(fd, buf, 5) == 5 && strcmp(buf, "hello") == 0);
    close(fd);
    // Missing files in the in-memory FS fail; real files still work.
    assert(open("/mem/missing", O_RDONLY) < 0);
    int real = open("/tmp/lfi3i_imfs_real.txt", O_RDWR | O_CREAT | O_TRUNC, 0600);
    assert(real >= 0 && real < 1024);                     // a genuine descriptor
    assert(write(real, "ok", 2) == 2);
    close(real);
    unlink("/tmp/lfi3i_imfs_real.txt");
    printf("[cage] imfs OK\n");
    return 0;
}
