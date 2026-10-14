// Run after exec: fd 3 was close-on-exec, fd 4 was not.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
int main(void) {
    int f3 = fcntl(3, F_GETFD), e3 = errno;
    int f4 = fcntl(4, F_GETFD);
    printf("[fdcheck] fd3: %d (errno %d), fd4: %d\n", f3, e3, f4);
    char c;
    int w = write(4, "x", 1);
    return (f3 == -1 && e3 == EBADF && f4 >= 0 && w == 1 && read(0, &c, 0) >= 0) ? 0 : 1;
}
