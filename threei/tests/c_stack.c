#include <stdio.h>
#include <unistd.h>
int main(void) {
    int e = geteuid();
    printf("[cage] geteuid() = %d\n", e);
    return e == 1012 ? 0 : 1;   // 1000 (default) + 5 (A) + 7 (B)
}
