#include <stdio.h>
#include <unistd.h>
int main(int argc, char **argv) {
    printf("[hello] pid=%d ppid=%d argc=%d\n", getpid(), getppid(), argc);
    return 0;
}
