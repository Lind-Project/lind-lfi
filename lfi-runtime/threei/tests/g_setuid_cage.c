#include <stdio.h>
#include <unistd.h>
#include "lind_syscall.h"
int main(void) {
    int r = setuid(1000);
    printf("setuid -> %d\n", r);
    return r;
}
