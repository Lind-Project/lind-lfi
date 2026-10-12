// Calls geteuid in a loop (each answered by a grate); checks every result.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 100000;
    for (int i = 0; i < n; i++)
        if (geteuid() != 10) { fprintf(stderr, "bad geteuid at %d\n", i); return 1; }
    return 0;
}
