// A program a clamp is supposed to confine to a directory: it tries to create a file outside it.
// If it can, the clamp let a call through to the host.
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    int fd = open("/tmp/escaped_the_clamp", O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) {
        puts("ESCAPED: created a file outside the clamp");
        close(fd);
        return 0;
    }
    puts("confined: open outside the prefix failed");
    return 0;
}
