// Forks many children that exit, and that exec and then exit, under a grate that forwards
// exit/exec. With references leaked on every forwarded exit, fork starts failing after about
// --max-cages iterations.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[2], "child"))
        return 0;                                  // the exec'd image: exit through the grate
    int rounds = argc > 1 ? atoi(argv[1]) : 150;
    for (int i = 0; i < rounds; i++) {
        pid_t p = fork();
        if (p < 0) {
            printf("FAIL: fork failed at round %d (errno %d): cage ids were not recycled\n", i, errno);
            return 1;
        }
        if (p == 0) {
            if (i % 2) _exit(0);                   // exit (forwarded)
            execl(argv[0], argv[0], "x", "child", (char *) 0);   // exec (forwarded), then exit
            _exit(127);
        }
        int st;
        if (waitpid(p, &st, 0) != p || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            printf("FAIL: child %d: wait/status %d\n", i, st);
            return 1;
        }
    }
    printf("exitleak: %d rounds ok\n", rounds);
    return 0;
}
