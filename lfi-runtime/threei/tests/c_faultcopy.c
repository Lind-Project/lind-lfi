// Child of g_faultcopy: publishes addresses of interesting regions, then waits.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    long pg = 4096;
    char *s = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *n = mmap(NULL, pg, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *w = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (s == MAP_FAILED || n == MAP_FAILED || w == MAP_FAILED) return 2;
    memset(s, 'A', pg);
    if (mprotect(s + pg, pg, PROT_NONE)) return 2;       // second page of s is inaccessible
    uint64_t where[3] = { (uint64_t) (uintptr_t) (s + pg - 16), (uint64_t) (uintptr_t) n, (uint64_t) (uintptr_t) w };
    if (write(atoi(argv[1]), where, sizeof where) != sizeof where) return 3;
    char c;
    return read(atoi(argv[2]), &c, 1) != 1;
}
