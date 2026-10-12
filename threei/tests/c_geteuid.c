#include <stdio.h>
#include <unistd.h>
int main(void) {
    int e = geteuid();
    printf("[cage] geteuid() = %d\n", e);
    return e == 10 ? 0 : 1;
}
