// Hostile cage: tries to use the private context-spawn controls to switch off the
// handlers a parent grate installed for it. Exit status is a bitmask of the checks that failed.
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SYS_SETENTRY 1010
#define SYS_CAPTURE  1011
#define SYS_COLLECT  1012

// SETENTRY wants 32-byte-aligned code addresses.
__attribute__((aligned(32), noinline)) static long stub(void) { return 0; }

static int denied(void)
{
    // getppid is denied by the parent (-EPERM); getpid is answered with 777; fork is denied.
    if (getppid() != -EPERM && !(syscall(SYS_getppid) == -EPERM)) return 1;
    if (getpid() != 777) return 1;
    if (fork() >= 0) return 1;
    return 0;
}

// With CAPTURE armed, a direct thread clone is not mediated (context creation is a runtime
// operation) but the "context" it makes may run only its start-up code: this one tries to run
// application code (a write) in the new thread. The runtime must end the cage, and the parent
// must never see an application thread running.
static int abuse_clone(void)
{
    syscall(SYS_SETENTRY, (long) stub, (long) stub);
    if (syscall(SYS_CAPTURE) != 0) return 1;
    char *stk = (char *) malloc(65536);
    static int ptid, ctid;
    long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM;
    long r;
    // The new thread continues right after the syscall instruction on its own stack: it makes a
    // write(1, ..., 1) (application behaviour, not context start-up) and then exits.
    register long r10 __asm__("r10") = (long) &ctid;
    register long r8 __asm__("r8") = 0;
    __asm__ volatile(
        "syscall\n\t"
        "test %%rax, %%rax\n\t"
        "jnz 1f\n\t"
        "mov $1, %%eax\n\t"
        "mov $1, %%edi\n\t"
        "mov %%rsp, %%rsi\n\t"
        "mov $1, %%edx\n\t"
        "syscall\n\t"
        "mov $60, %%eax\n\t"
        "xor %%edi, %%edi\n\t"
        "syscall\n\t"
        "1:"
        : "=a"(r)
        : "0"(56L), "D"(flags), "S"(stk + 65536), "d"(&ptid), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    printf("FAIL: the cage survived a misused capture (r=%ld)\n", r);
    return 0;
}

// With CAPTURE armed, a clone that creates a *process* must still meet the parent's clone
// handler (which denies it here): the exemption is only for thread-creating clones.
static int abuse_procclone(void)
{
    syscall(SYS_SETENTRY, (long) stub, (long) stub);
    if (syscall(SYS_CAPTURE) != 0) return 1;
    errno = 0;
    long r = syscall(SYS_clone, (long) SIGCHLD, 0L, 0L, 0L, 0L);
    if (r == 0) _exit(0);              // we are the unmediated child process
    if (r > 0) {
        printf("FAIL: a process clone bypassed the parent's handler (pid %ld)\n", r);
        return 1;
    }
    if (errno != EPERM) { printf("FAIL: clone returned errno %d, not EPERM\n", errno); return 1; }
    // the request was disarmed, not left pending for a later thread clone
    errno = 0;
    char *stk = (char *) malloc(65536);
    static int ptid, ctid;
    r = syscall(SYS_clone, CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
        CLONE_SYSVSEM, stk + 65536, &ptid, &ctid, 0L);
    if (r != -1 || errno != EPERM) { printf("FAIL: a later thread clone was not mediated (r=%ld)\n", r); return 1; }
    puts("procclone: PASS");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "clone"))
        return abuse_clone();
    if (argc > 1 && !strcmp(argv[1], "procclone"))
        return abuse_procclone();
    int fails = 0;
    if (denied()) fails |= 1;
    // 1. No shim installed: CAPTURE must be refused and change nothing.
    if (syscall(SYS_CAPTURE) != -EPERM) fails |= 2;
    if (denied()) fails |= 4;
    // 2. COLLECT without a capture, repeated and misordered.
    if (syscall(SYS_COLLECT, 0L) >= 0) fails |= 8;
    if (syscall(SYS_COLLECT, 0L) >= 0) fails |= 8;
    if (denied()) fails |= 16;
    // 3. Install entry points (grants the shim's two initial captures), then abuse them:
    //    capture, omit COLLECT, issue the denied calls.
    if (syscall(SYS_SETENTRY, (long) stub, (long) stub) != 0) fails |= 32;
    if (syscall(SYS_CAPTURE) != 0) fails |= 64;
    if (denied()) fails |= 128;
    if (syscall(SYS_CAPTURE) != 0) fails |= 256;
    if (denied()) fails |= 512;
    // 4. The budget is spent: no further captures.
    if (syscall(SYS_CAPTURE) != -EPERM) fails |= 1024;
    if (syscall(SYS_CAPTURE) != -EPERM) fails |= 1024;
    // 5. Re-installing the entry points does not refill it.
    if (syscall(SYS_SETENTRY, (long) stub, (long) stub) != 0) fails |= 2048;
    if (syscall(SYS_CAPTURE) != -EPERM) fails |= 2048;
    if (denied()) fails |= 4096;
    printf("[capture] failed-check mask: %d\n", fails);
    return fails ? 1 : 0;
}
