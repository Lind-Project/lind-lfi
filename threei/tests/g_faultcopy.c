// copy_data_between_cages with memory that is inside the sandbox but not accessible: it must fail
// (or, for a memcpy that got partway, copy what it could) without taking the runtime down.
//   g_faultcopy <child>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lind_syscall.h"

int main(int argc, char **argv)
{
    assert(argc >= 2);
    int up[2], down[2];
    assert(pipe(up) == 0 && pipe(down) == 0);
    int grate = getpid();
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        char a1[16], a2[16];
        snprintf(a1, sizeof a1, "%d", up[1]);
        snprintf(a2, sizeof a2, "%d", down[0]);
        execl(argv[1], argv[1], a1, a2, (char *) 0);
        _exit(127);
    }
    uint64_t where[3];                 // [0] straddles: 16 readable bytes then PROT_NONE; [1] PROT_NONE; [2] writable
    assert(read(up[0], where, sizeof where) == sizeof where);
    static char buf[256];
    int fails = 0;
    int r;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); fails++; } } while (0)

    memset(buf, 0x55, sizeof buf);
    r = copy_data_between_cages(grate, pid, where[1], pid, (uint64_t) (uintptr_t) buf, grate, 16, 0);
    CHECK(r == (int) ELINDAPIABORTED && buf[0] == 0x55, "memcpy from PROT_NONE source fails");
    r = copy_data_between_cages(grate, pid, where[1], pid, (uint64_t) (uintptr_t) buf, grate, 64, 1);
    CHECK(r == (int) ELINDAPIABORTED, "string copy from PROT_NONE source fails (the scan for the terminator faults)");
    r = copy_data_between_cages(grate, grate, (uint64_t) (uintptr_t) buf, grate, where[1], pid, 16, 0);
    CHECK(r == (int) ELINDAPIABORTED, "memcpy into PROT_NONE destination fails");
    // starts on readable bytes, runs into the PROT_NONE page: the readable part is copied, the rest zeroed
    memset(buf, 0x55, sizeof buf);
    r = copy_data_between_cages(grate, pid, where[0], pid, (uint64_t) (uintptr_t) buf, grate, 64, 0);
    CHECK(r != (int) ELINDAPIABORTED && buf[0] == 'A' && buf[15] == 'A' && buf[16] == 0 && buf[63] == 0,
        "memcpy running into an inaccessible page copies the readable bytes and zeroes the rest");
    // the same, but the destination is the inaccessible side after the first page
    r = copy_data_between_cages(grate, grate, (uint64_t) (uintptr_t) buf, grate, where[2], pid, 64, 0);
    CHECK(r != (int) ELINDAPIABORTED, "ordinary copy into a writable region still works");

    assert(write(down[1], "x", 1) == 1);
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "the cage survived and exited cleanly");
    printf(fails ? "faultcopy: %d FAILED\n" : "faultcopy: PASS\n", fails);
    return fails ? 1 : 0;
}
