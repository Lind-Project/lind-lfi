// A confined cage tries to remove the seccomp grate's denial of chmod by editing Threei's
// routing: deregistering or replacing the slot, removing the guards on the routing calls
// themselves, overwriting its whole table, and doing the same from a forked child or after exec.
// Run under seccomp_whitelist_test.conf (chmod denied). Exit status 0 = every attempt failed
// with EPERM and chmod stayed denied.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "lind_syscall.h"

#define DEREGISTER 500
#define SYS_CHMOD 90
#define SYS_GETPPID 110
#define SYS_REGISTER_HANDLER 1001
#define SYS_COPY_HTAB 1003

static int own_handler(uint64_t g, uint64_t a1, uint64_t c1, uint64_t a2, uint64_t c2,
    uint64_t a3, uint64_t c3, uint64_t a4, uint64_t c4, uint64_t a5, uint64_t c5,
    uint64_t a6, uint64_t c6) { return 4242; }

static int fails;
#define EXPECT(cond, what) do { if (!(cond)) { fails++; fprintf(stderr, "FAIL: %s\n", what); } } while (0)

static void chmod_still_denied(const char *who)
{
    char msg[64];
    int r = chmod("/tmp/seccomp_tamper_probe", 0777);
    snprintf(msg, sizeof msg, "%s: chmod still denied", who);
    EXPECT(r == -1 && errno == EPERM, msg);
}

// For a descendant that legitimately re-pointed the slot at a handler in the subject's subtree:
// the call may now be answered by that handler, but it must not reach the host.
static void chmod_not_performed(const char *who)
{
    char msg[64];
    struct stat st;
    int r = chmod("/tmp/seccomp_tamper_probe", 0777);
    snprintf(msg, sizeof msg, "%s: chmod not performed", who);
    EXPECT(r != 0 && stat("/tmp/seccomp_tamper_probe", &st) == 0 && (st.st_mode & 0777) == 0600, msg);
}

static void tamper(const char *who, int self, int other, int is_subject)
{
    char msg[96];
#define T(expr, what) do { snprintf(msg, sizeof msg, "%s: %s", who, what); EXPECT((expr) == -EPERM, msg); if (is_subject) chmod_still_denied(who); else chmod_not_performed(who); } while (0)
    T(register_handler(self, SYS_CHMOD, DEREGISTER, 0), "deregister chmod slot");
    if (is_subject) {
        T(register_handler(self, SYS_CHMOD, self, (uint64_t) (uintptr_t) own_handler), "replace chmod slot");
    } else {
        // a descendant may point the slot at a handler of its own (inside the subtree), nothing more
        int rr = register_handler(self, SYS_CHMOD, self, (uint64_t) (uintptr_t) own_handler);
        EXPECT(rr == 0 || rr == -EPERM, "child: replace chmod slot is either refused or confined");
        chmod_not_performed("child: after replacing chmod slot");
    }
    T(register_handler(self, SYS_REGISTER_HANDLER, DEREGISTER, 0), "remove register guard");
    T(register_handler(self, SYS_COPY_HTAB, DEREGISTER, 0), "remove copy-table guard");
    if (is_subject) {
        // the confined cage itself may not touch its own table at all ...
        T(register_handler(self, SYS_GETPPID, DEREGISTER, 0), "touch own table at an unprotected slot");
        T(copy_handler_table_to_cage(self, self), "copy own table onto itself");
    }
    // ... and nobody may import a table that lacks the denials (an ancestor's, say)
    T(copy_handler_table_to_cage(other, self), "overwrite own table from an ancestor");
#undef T
}

int main(int argc, char **argv)
{
    int self = getpid();
    if (argc >= 2 && !strcmp(argv[1], "execed")) {   // the policy must survive exec
        chmod_not_performed("after exec");
        tamper("after exec", self, getppid(), 0);
        return fails ? 1 : 0;
    }
    close(open("/tmp/seccomp_tamper_probe", O_CREAT | O_WRONLY, 0600));
    chmod_still_denied("baseline");

    int p[2];
    if (pipe(p)) return 2;
    pid_t child = fork();
    if (child < 0) return 2;
    if (child == 0) {            // a confined descendant: same attempts, on itself
        char x;
        read(p[0], &x, 1);
        int me = getpid();
        tamper("child", me, getppid(), 0);
        // and at its parent's table: not even related-by-descent, so refused
        EXPECT(register_handler(self, SYS_CHMOD, DEREGISTER, 0) == -EPERM, "child: edit parent's chmod slot");
        // exec keeps the table
        if (fails) _exit(1);
        execl(argv[0], argv[0], "execed", (char *) 0);
        _exit(127);
    }

    tamper("subject", self, getppid(), 1);
    // The subject may still configure its descendants for calls the policy allows:
    // route the child's getppid to this cage's handler.
    EXPECT(register_handler(child, SYS_GETPPID, self, (uint64_t) (uintptr_t) own_handler) == 0,
        "subject: register handler for child at an allowed slot");
    write(p[1], "x", 1);
    int st;
    waitpid(child, &st, 0);
    EXPECT(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child checks");
    unlink("/tmp/seccomp_tamper_probe");
    printf(fails ? "seccomp_tamper: %d FAILED\n" : "seccomp_tamper: PASS\n", fails);
    return fails ? 1 : 0;
}
