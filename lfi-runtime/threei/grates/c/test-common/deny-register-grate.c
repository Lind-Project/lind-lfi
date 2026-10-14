// Test grate for fault injection: refuses register_handler calls for call numbers >= a
// threshold (clamp grates allocate alternate routing numbers from 2000 upward), but only after
// the first <allow-count> registrations have gone through, and forwards the rest. Used to check
// that a clamp whose installation fails at any point fails closed.
//   deny-register-grate <min-call-number> <allow-count> <program> [args...]
#include <errno.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lind_syscall.h"

static uint64_t min_nr, subject, allow_left;

static int guard(uint64_t g, uint64_t a1, uint64_t c1, uint64_t a2, uint64_t c2, uint64_t a3,
    uint64_t c3, uint64_t a4, uint64_t c4, uint64_t a5, uint64_t c5, uint64_t a6, uint64_t c6)
{
    if (c1 >= min_nr) {
        uint64_t left = __atomic_load_n(&allow_left, __ATOMIC_RELAXED);
        for (;;) {
            if (left == 0)
                return -EPERM;
            if (__atomic_compare_exchange_n(&allow_left, &left, left - 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                break;
        }
    }
    return make_threei_call(1001, 0, g, subject, a1, c1, a2, c2, a3, c3, a4, c4, a5, c5, a6, c6, 0);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <min-call-number> <allow-count> <program> [args...]\n", argv[0]);
        return 2;
    }
    min_nr = strtoull(argv[1], 0, 0);
    allow_left = strtoull(argv[2], 0, 0);
    sem_t *sem = mmap(NULL, sizeof(*sem), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    sem_init(sem, 1, 0);
    int grate = getpid();
    pid_t cage = fork();
    if (cage < 0)
        return 1;
    if (cage == 0) {
        sem_wait(sem);
        execv(argv[3], &argv[3]);
        _exit(127);
    }
    subject = (uint64_t) cage;
    if (register_handler(cage, 1001, grate, (uint64_t) (uintptr_t) guard) != 0) {
        kill(cage, SIGKILL);
        return 1;
    }
    sem_post(sem);
    int st;
    if (wait(&st) < 0)
        return 1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1;
}
