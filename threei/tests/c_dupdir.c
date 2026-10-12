// fcntl(F_DUPFD) of a directory descriptor must keep it a directory: openat
// relative to the duplicate works (gnulib's fts, so `grep -r`, relies on it).
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    int d = open("/", O_RDONLY | O_DIRECTORY);
    int d2 = fcntl(d, F_DUPFD_CLOEXEC, 10);
    if (d < 0 || d2 < 10) { printf("dup failed: %d %d\n", d, d2); return 1; }
    int f = openat(d2, "dev/null", O_RDONLY);
    if (f < 0) { perror("openat(dup)"); return 1; }
    int g = openat(d, "dev/null", O_RDONLY);
    return g < 0;
}
