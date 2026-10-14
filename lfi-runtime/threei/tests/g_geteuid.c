// The simplest grate: makes geteuid() in the cage return 10.
#include <assert.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lind_syscall.h"

int geteuid_grate(uint64_t cageid, uint64_t arg1, uint64_t arg1cage,
                  uint64_t arg2, uint64_t arg2cage, uint64_t arg3,
                  uint64_t arg3cage, uint64_t arg4, uint64_t arg4cage,
                  uint64_t arg5, uint64_t arg5cage, uint64_t arg6,
                  uint64_t arg6cage) {
    printf("[grate] geteuid handler running in grate %d (cageid=%llu), call from cage %llu\n",
           getpid(), (unsigned long long)cageid, (unsigned long long)arg1cage);
    return 10;
}

int main(int argc, char *argv[]) {
    assert(argc >= 2);
    int grateid = getpid();

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        int cageid = getpid();
        int ret = register_handler(cageid, 107 /* geteuid */, grateid,
                                   (uint64_t)(uintptr_t)&geteuid_grate);
        if (ret != 0) fprintf(stderr, "register_handler -> %d (fn=%llx)\n", ret, (unsigned long long)(uintptr_t)&geteuid_grate);
        assert(ret == 0);
        if (execv(argv[1], &argv[1]) == -1) {
            perror("execv");
            _exit(127);
        }
    }

    int status;
    while (wait(&status) > 0) {
        if (status != 0) {
            fprintf(stderr, "[grate] FAIL: cage exited with status %d\n", status);
            return 1;
        }
    }
    printf("[grate] PASS\n");
    return 0;
}
