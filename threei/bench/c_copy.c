#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static char big[65536];
int main(int argc, char **argv)
{
    memset(big, 1, sizeof big);
    unsigned long long w = (unsigned long long) (unsigned long) big;
    if (argc < 3 || write(atoi(argv[1]), &w, 8) != 8) return 1;
    char x;
    return read(atoi(argv[2]), &x, 1) != 1;
}
