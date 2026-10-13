// Hostile cage: tries to use the private context-spawn controls to switch off the
// handlers a parent grate installed for it. Exit status is a bitmask of the checks that failed.
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
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

int main(void)
{
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
