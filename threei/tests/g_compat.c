// Compatibility behaviours that grates written for Lind rely on:
//  * a handler registered for epoll_wait / stat also serves the calls musl
//    makes instead (epoll_pwait, newfstatat);
//  * the arguments beyond a call's arity reach the handler as zero (imfs-grate
//    reads a fourth argument of fchmodat as its flags);
//  * a handler can be registered under an "alternate" call number (>= 1008) and
//    reached with make_threei_call;
//  * a grate can send a call through the handler table of a cage it launched
//    (self != caller);
//  * copy_data_between_cages copies what is readable of a range that runs past
//    the end of a mapping and zero-fills the rest.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lind_syscall.h"

#define H(name, expr)                                                          \
    int name(uint64_t cageid, uint64_t arg1, uint64_t arg1cage, uint64_t arg2, \
        uint64_t arg2cage, uint64_t arg3, uint64_t arg3cage, uint64_t arg4,    \
        uint64_t arg4cage, uint64_t arg5, uint64_t arg5cage, uint64_t arg6,    \
        uint64_t arg6cage)                                                     \
    {                                                                          \
        (void) cageid, (void) arg1, (void) arg1cage, (void) arg2, (void) arg2cage, \
            (void) arg3, (void) arg3cage, (void) arg4, (void) arg4cage,        \
            (void) arg5, (void) arg5cage, (void) arg6, (void) arg6cage;       \
        return (expr);                                                         \
    }

H(h_epoll_wait, 4242)               // for epoll_wait (232); maxevents is arg3
H(h_stat, arg2 == 0 ? -1 : 4343)    // stat(path, buf): buf must be arg2
H(h_fchmodat, arg4 == 0 ? 0 : -22)  // fchmodat(dirfd, path, mode) + flags = 0
H(h_alt, 7777)

int main(int argc, char *argv[])
{
    assert(argc >= 2);
    int grate = getpid();
    int up[2], down[2]; // child -> grate: address; grate -> child: "done"
    assert(pipe(up) == 0 && pipe(down) == 0);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        int me = getpid();
        assert(register_handler(me, 232, grate, (uint64_t) (uintptr_t) &h_epoll_wait) == 0);
        assert(register_handler(me, 4, grate, (uint64_t) (uintptr_t) &h_stat) == 0);
        assert(register_handler(me, 268, grate, (uint64_t) (uintptr_t) &h_fchmodat) == 0);
        char a1[16], a2[16];
        snprintf(a1, sizeof a1, "%d", up[1]);
        snprintf(a2, sizeof a2, "%d", down[0]);
        char *cargv[] = { argv[1], a1, a2, NULL };
        execv(argv[1], cargv);
        _exit(127);
    }

    // copy_data_between_cages from a cage's string at the end of a mapping:
    // the child reports where the string is; the 64 bytes asked for run past
    // the mapping.
    uint64_t where = 0;
    assert(read(up[0], &where, sizeof where) == sizeof where);
    // (the child has registered its handlers by now)
    // An alternate call number on the grate's own table, reached through
    // make_threei_call; and a call through the child's table (self != caller)
    // although the child is not the caller.
    assert(register_handler(grate, 2000, grate, (uint64_t) (uintptr_t) &h_alt) == 0);
    assert(make_threei_call(2000, 0, grate, grate, 0, grate, 0, grate, 0, grate,
               0, grate, 0, grate, 0, grate, 0) == 7777);
    // (the child's table has the epoll_wait handler: self = child)
    int r = make_threei_call(232, 0, pid, pid, 0, pid, 0, pid, 4, pid, 0, pid,
        0, pid, 0, pid, 0);
    assert(r == 4242);

    char buf[64];
    memset(buf, 0x55, sizeof buf);
    r = copy_data_between_cages(grate, pid, where, pid, (uint64_t) (uintptr_t) buf,
        grate, sizeof buf, 0);
    assert(r != (int) ELINDAPIABORTED);
    assert(memcmp(buf, "hello", 6) == 0);
    for (int i = 6; i < 64; i++)
        assert(buf[i] == 0);
    // ... and a range that starts in unmapped memory is still an error.
    assert(copy_data_between_cages(grate, pid, where + 4096, pid,
               (uint64_t) (uintptr_t) buf, grate, 8, 0) == (int) ELINDAPIABORTED);
    assert(write(down[1], "x", 1) == 1);

    int status;
    waitpid(pid, &status, 0);
    if (status != 0) {
        fprintf(stderr, "[grate] FAIL: cage exited with status %d\n", status);
        return 1;
    }
    printf("[grate] PASS\n");
    return 0;
}
