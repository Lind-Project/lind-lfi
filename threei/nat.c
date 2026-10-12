#define _GNU_SOURCE
// Default ("RawPOSIX") system call implementations of the 3i runtime.
//
// These operate on the host kernel on behalf of the call's *target* cage:
// file descriptors are looked up in the descriptor table of the cage the fd
// argument belongs to, paths are resolved in the target's file system view
// (root mapping and working directory), and pointer arguments are checked
// against the sandbox of the cage that owns them. Together with the argument
// cage ids this is what lets a grate forward a call on behalf of another
// cage with buffers living in a third address space.
#include "cage.h"
#include "natutil.h"

#include "cwalk.h"
#include "fd.h"
#include "futex.h"
#include "host.h"
#include "lock.h"
#include "path.h"
#include "sys/sys.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static inline struct LFILinuxProc *
tproc(struct Call *c)
{
    return c->target->proc;
}

// ---------------------------------------------------------------------------
// File descriptors
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

// Host paths that expose the runtime process itself.
static bool
host_path_forbidden(const char *p)
{
    static const char *const bad[] = { "/proc", "/sys" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        size_t n = strlen(bad[i]);
        if (strncmp(p, bad[i], n) == 0 && (p[n] == 0 || p[n] == '/'))
            return true;
    }
    return false;
}

// Resolves a sandbox path string, relative to the directory descriptor dirfd
// if it is relative and dirfd is not AT_FDCWD, into a host path. If abs_out is
// non-NULL it receives (malloc'd) the normalized absolute sandbox path.
static long
resolve_str(struct Call *c, int dirfd, const char *p, char *host, size_t hsz,
    char **abs_out)
{
    struct LFILinuxProc *pr = tproc(c);
    if (!pr)
        return -ESRCH;
    char joined[FILENAME_MAX];
    const char *use = p;
    if (p[0] == 0)
        return -ENOENT;
    if (p[0] != '/' && dirfd != AT_FDCWD) {
        char base[FILENAME_MAX];
        {
            LOCK_WITH_DEFER(&pr->fdtable.lk, lk_fd);
            if (dirfd < 0 || dirfd >= LINUX_NOFILE ||
                pr->fdtable.fds[dirfd] == -1)
                return -EBADF;
            if (!pr->fdtable.dirs[dirfd])
                return -ENOTDIR;
            strncpy(base, pr->fdtable.dirs[dirfd], sizeof(base) - 1);
            base[sizeof(base) - 1] = 0;
        }
        cwk_path_join(base, p, joined, sizeof(joined));
        use = joined;
    }
    LOCK_WITH_DEFER(&pr->cwd.lk, lk_cwd);
    if (!path_resolve(pr, use, host, hsz))
        return -ENOENT;
    // The host's process and device information describes (and can modify)
    // the runtime itself, e.g. /proc/self/mem: never expose it.
    if (!g_eng->opts.permissive && host_path_forbidden(host))
        return -ENOENT;
    if (abs_out) {
        char abs[FILENAME_MAX];
        if (pr->cwd.path[0] == 0 && use[0] != '/') {
            *abs_out = NULL;
        } else {
            cwk_path_get_absolute(pr->cwd.path, use, abs, sizeof(abs));
            *abs_out = strdup(abs);
        }
    }
    return 0;
}

// Resolves path argument pi (with directory descriptor argument di, or -1).
long
resolve_arg(struct Call *c, int di, int pi, char *host, size_t hsz,
    char **abs_out)
{
    char *p = call_path(c, pi);
    if (!p)
        return -EFAULT;
    int dirfd = di < 0 ? AT_FDCWD : (int) c->a[di];
    long r = resolve_str(c, dirfd, p, host, hsz, abs_out);
    free(p);
    return r;
}

// ---------------------------------------------------------------------------
// read / write family
// ---------------------------------------------------------------------------

#define RW_MAX 0x7ffff000UL

NATIVE(n_read)
{
    size_t n = c->a[2];
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    long r;
    if (f.flags & LINUX_O_PATH) {
        r = -EBADF;
    } else if (f.isdir) {
        r = -EISDIR;
    } else if (n == 0) {
        r = 0;
    } else {
        if (n > RW_MAX)
            n = RW_MAX;
        void *b = call_buf(c, 1, n, 1);
        r = b ? sysret(read(f.hfd, b, n)) : -EFAULT;
    }
    fd_put(&f);
    return r;
}

NATIVE(n_write)
{
    size_t n = c->a[2];
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    long r;
    if (f.flags & LINUX_O_PATH) {
        r = -EBADF;
    } else if (n == 0) {
        r = 0;
    } else {
        if (n > RW_MAX)
            n = RW_MAX;
        void *b = call_buf(c, 1, n, 1);
        r = b ? pipe_signal(c, sysret(write(f.hfd, b, n))) : -EFAULT;
    }
    fd_put(&f);
    return r;
}

NATIVE(n_pread)
{
    size_t n = c->a[2];
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    long r;
    if (f.flags & LINUX_O_PATH) {
        r = -EBADF;
    } else if (f.isdir) {
        r = -EISDIR;
    } else if (n == 0) {
        r = 0;
    } else {
        if (n > RW_MAX)
            n = RW_MAX;
        void *b = call_buf(c, 1, n, 1);
        r = b ? sysret(pread(f.hfd, b, n, (off_t) c->a[3])) : -EFAULT;
    }
    fd_put(&f);
    return r;
}

NATIVE(n_pwrite)
{
    size_t n = c->a[2];
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    long r;
    if (f.flags & LINUX_O_PATH) {
        r = -EBADF;
    } else if (n == 0) {
        r = 0;
    } else {
        if (n > RW_MAX)
            n = RW_MAX;
        void *b = call_buf(c, 1, n, 1);
        r = b ? pipe_signal(c, sysret(pwrite(f.hfd, b, n, (off_t) c->a[3]))) : -EFAULT;
    }
    fd_put(&f);
    return r;
}

#define IOV_MAX_LFI 1024

struct GIov {
    uint64_t base;
    uint64_t len;
};

// Builds a host iovec array from the guest array in argument 1. The embedded
// pointers belong to the same address space as the array itself.
static long
iov_build(struct Call *c, struct iovec *out, int *cnt_out)
{
    uint64_t cnt = c->a[2];
    if (cnt > IOV_MAX_LFI)
        return -EINVAL;
    *cnt_out = (int) cnt;
    if (cnt == 0)
        return 0;
    struct GIov *g = call_buf(c, 1, cnt * sizeof(struct GIov), 8);
    if (!g)
        return -EFAULT;
    struct LFIBox *box = call_box(c, 1);
    uint64_t total = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        // Snapshot each entry once (the sandbox could modify the array).
        uint64_t base = __atomic_load_n(&g[i].base, __ATOMIC_RELAXED);
        uint64_t len = __atomic_load_n(&g[i].len, __ATOMIC_RELAXED);
        if (len > RW_MAX)
            return -EINVAL;
        total += len;
        if (total > RW_MAX)
            return -EINVAL;
        if (len == 0) {
            out[i].iov_base = NULL;
            out[i].iov_len = 0;
            continue;
        }
        if (!lfi_box_bufvalid(box, base, len))
            return -EFAULT;
        out[i].iov_base = (void *) lfi_box_l2p(box, base);
        out[i].iov_len = len;
    }
    return 0;
}

NATIVE(n_readv)
{
    struct iovec iov[IOV_MAX_LFI];
    int cnt;
    long r = iov_build(c, iov, &cnt);
    if (r)
        return r;
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    if (f.flags & LINUX_O_PATH)
        r = -EBADF;
    else if (f.isdir)
        r = -EISDIR;
    else
        r = cnt == 0 ? 0 : sysret(readv(f.hfd, iov, cnt));
    fd_put(&f);
    return r;
}

NATIVE(n_writev)
{
    struct iovec iov[IOV_MAX_LFI];
    int cnt;
    long r = iov_build(c, iov, &cnt);
    if (r)
        return r;
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    if (f.flags & LINUX_O_PATH)
        r = -EBADF;
    else
        r = cnt == 0 ? 0 : pipe_signal(c, sysret(writev(f.hfd, iov, cnt)));
    fd_put(&f);
    return r;
}

NATIVE(n_lseek)
{
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    long r = (f.flags & LINUX_O_PATH) ? -EBADF :
                                        sysret(lseek(f.hfd, (off_t) c->a[1],
                                            (int) c->a[2]));
    fd_put(&f);
    return r;
}

// ---------------------------------------------------------------------------
// open / close / dup / pipe / fcntl
// ---------------------------------------------------------------------------

#define OPEN_ALLOWED                                                       \
    (O_ACCMODE | O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC | O_APPEND |         \
        O_NONBLOCK | O_DSYNC | O_SYNC | O_DIRECTORY | O_NOFOLLOW |          \
        O_CLOEXEC | O_NOATIME | O_PATH | O_LARGEFILE)

static long
do_open(struct Call *c, int di, int pi, int flags, mode_t mode)
{
    struct LFILinuxProc *pr = tproc(c);
    char host[FILENAME_MAX];
    char *abs = NULL;
    long r = resolve_arg(c, di, pi, host, sizeof(host), &abs);
    if (r) {
        free(abs);
        return r;
    }
    int hflags = flags & OPEN_ALLOWED;
    mode = mode & 07777 & ~c->target->umask;
    int hfd = open(host, hflags | O_CLOEXEC, mode);
    if (hfd < 0) {
        r = -errno;
        free(abs);
        return r;
    }
    struct stat st;
    bool isdir = fstat(hfd, &st) == 0 && S_ISDIR(st.st_mode);
    char *dir = isdir ? abs : NULL;
    if (!isdir)
        free(abs);
    // On success the descriptor table holds the only reference to hfd.
    int fd = fdalloc(&pr->fdtable, hfd, dir,
        flags & (LINUX_O_CLOEXEC | LINUX_O_PATH));
    if (fd < 0)
        close(hfd);
    return fd;
}

NATIVE(n_open)
{
    return do_open(c, -1, 0, (int) c->a[1], (mode_t) c->a[2]);
}

NATIVE(n_openat)
{
    return do_open(c, 0, 1, (int) c->a[2], (mode_t) c->a[3]);
}

NATIVE(n_creat)
{
    return do_open(c, -1, 0, O_CREAT | O_WRONLY | O_TRUNC, (mode_t) c->a[1]);
}

NATIVE(n_close)
{
    struct FDTable *t = call_fdt(c, 0);
    if (!t || !fdclose(t, (int) c->a[0]))
        return -EBADF;
    return 0;
}

NATIVE(n_dup)
{
    struct FDTable *t = call_fdt(c, 0);
    if (!t)
        return -EBADF;
    return fddup2(t, (int) c->a[0], -1);
}

NATIVE(n_dup2)
{
    struct FDTable *t = call_fdt(c, 0);
    if (!t)
        return -EBADF;
    int oldfd = (int) c->a[0], newfd = (int) c->a[1];
    if (newfd < 0)
        return -EBADF;
    return fddup2(t, oldfd, newfd);
}

NATIVE(n_dup3)
{
    struct FDTable *t = call_fdt(c, 0);
    if (!t)
        return -EBADF;
    int oldfd = (int) c->a[0], newfd = (int) c->a[1];
    int flags = (int) c->a[2];
    if (oldfd == newfd || (flags & ~O_CLOEXEC) != 0 || newfd < 0)
        return -EINVAL;
    int r = fddup2(t, oldfd, newfd);
    if (r >= 0 && (flags & O_CLOEXEC))
        fdsetflags(t, r, fdgetflags(t, r) | LINUX_O_CLOEXEC);
    return r;
}

static long
do_pipe(struct Call *c, int flags)
{
    if (flags & ~(O_CLOEXEC | O_NONBLOCK))
        return -EINVAL;
    int *out = call_buf(c, 0, 2 * sizeof(int), alignof(int));
    if (!out)
        return -EFAULT;
    struct LFILinuxProc *pr = tproc(c);
    int hfds[2];
    if (pipe2(hfds, O_CLOEXEC | (flags & O_NONBLOCK)) < 0)
        return -errno;
    int cl = (flags & O_CLOEXEC) ? LINUX_O_CLOEXEC : 0;
    int r = fdalloc(&pr->fdtable, hfds[0], NULL, cl);
    if (r < 0) {
        close(hfds[0]);
        close(hfds[1]);
        return r;
    }
    int w = fdalloc(&pr->fdtable, hfds[1], NULL, cl);
    if (w < 0) {
        fdclose(&pr->fdtable, r);
        close(hfds[1]);
        return w;
    }
    out[0] = r;
    out[1] = w;
    return 0;
}

NATIVE(n_pipe)
{
    return do_pipe(c, 0);
}

NATIVE(n_pipe2)
{
    return do_pipe(c, (int) c->a[1]);
}

#define F_DUPFD_CLOEXEC_ 1030

NATIVE(n_fcntl)
{
    struct FDTable *t = call_fdt(c, 0);
    if (!t)
        return -EBADF;
    int fd = (int) c->a[0], cmd = (int) c->a[1];
    uint64_t arg = c->a[2];
    switch (cmd) {
    case F_GETFD: {
        int fl = fdgetflags(t, fd);
        return fl < 0 ? fl : ((fl & LINUX_O_CLOEXEC) ? FD_CLOEXEC : 0);
    }
    case F_SETFD: {
        int fl = fdgetflags(t, fd);
        if (fl < 0)
            return fl;
        fl = (arg & FD_CLOEXEC) ? (fl | LINUX_O_CLOEXEC) :
                                  (fl & ~LINUX_O_CLOEXEC);
        return fdsetflags(t, fd, fl);
    }
    case F_DUPFD:
    case F_DUPFD_CLOEXEC_: {
        return fddup_min(t, fd, (int) arg,
            cmd == F_DUPFD_CLOEXEC_ ? LINUX_O_CLOEXEC : 0);
    }
    case F_GETFL:
    case F_SETFL: {
        struct FDR f;
        if (fd_get(c, 0, &f))
            return -EBADF;
        long r;
        if (cmd == F_GETFL)
            r = sysret(fcntl(f.hfd, F_GETFL));
        else
            r = sysret(fcntl(f.hfd, F_SETFL, (int) arg &
                (O_APPEND | O_NONBLOCK | O_NOATIME)));
        fd_put(&f);
        return r;
    }
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW: {
        struct FDR f;
        if (fd_get(c, 0, &f))
            return -EBADF;
        void *fl = call_buf(c, 2, sizeof(struct flock), 8);
        long r = fl ? sysret(fcntl(f.hfd, cmd, fl)) : -EFAULT;
        fd_put(&f);
        return r;
    }
    default:
        return -EINVAL;
    }
}

// ---------------------------------------------------------------------------
// stat family, directories, file operations
// ---------------------------------------------------------------------------

static long
do_fstatat(struct Call *c, int dirfd_i, int path_i, int buf_i, int flags)
{
    struct stat *st = call_buf(c, buf_i, sizeof(struct stat), 8);
    if (!st)
        return -EFAULT;
    int hflags = flags & (AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH);
    if (flags & AT_EMPTY_PATH) {
        // The path may be empty: operate on the descriptor itself.
        char *p = path_i >= 0 ? call_path(c, path_i) : strdup("");
        if (!p)
            return -EFAULT;
        bool empty = p[0] == 0;
        free(p);
        if (empty) {
            struct FDR f;
            if (fd_get(c, dirfd_i, &f))
                return -EBADF;
            long r = sysret(fstat(f.hfd, st));
            fd_put(&f);
            return r;
        }
    }
    char host[FILENAME_MAX];
    long r = resolve_arg(c, dirfd_i < 0 ? -1 : dirfd_i, path_i, host,
        sizeof(host), NULL);
    if (r)
        return r;
    return sysret(fstatat(AT_FDCWD, host, st, hflags & ~AT_EMPTY_PATH));
}

NATIVE(n_stat)
{
    return do_fstatat(c, -1, 0, 1, 0);
}

NATIVE(n_lstat)
{
    return do_fstatat(c, -1, 0, 1, AT_SYMLINK_NOFOLLOW);
}

NATIVE(n_fstat)
{
    struct stat *st = call_buf(c, 1, sizeof(struct stat), 8);
    if (!st)
        return -EFAULT;
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    long r = sysret(fstat(f.hfd, st));
    fd_put(&f);
    return r;
}

NATIVE(n_newfstatat)
{
    return do_fstatat(c, 0, 1, 2, (int) c->a[3]);
}

// getdents (78, Lind's libc) and getdents64 (217, musl) records have the same
// length but a different layout:
//   getdents64: ino(8) off(8) reclen(2) type(1) name...
//   getdents:   ino(8) off(8) reclen(2) name... [pad] type(1 byte, last of the record)
// so a buffer is converted in place.
static void
dirents_convert(uint8_t *buf, size_t n, bool to_old)
{
    for (size_t o = 0; o + 19 <= n;) {
        uint8_t *r = buf + o;
        size_t reclen = r[16] | (r[17] << 8);
        if (reclen < 20 || o + reclen > n)
            break;
        if (to_old) {
            uint8_t type = r[18];
            size_t nl = strnlen((char *) r + 19, reclen - 19);
            memmove(r + 18, r + 19, nl + 1);
            r[reclen - 1] = type;
        } else {
            uint8_t type = r[reclen - 1];
            size_t nl = strnlen((char *) r + 18, reclen - 19);
            memmove(r + 19, r + 18, nl + 1);
            r[18] = type;
        }
        o += reclen;
    }
}

// Converts the dirents a call left in cage 'owner's buffer at 'addr' from the
// getdents layout to getdents64's (see sys_alias in dispatch.c).
void
dirents_old_to_new(uint32_t owner, uint64_t addr, long n)
{
    struct Cage *m = cage_get(owner);
    if (!m)
        return;
    struct LFIBox *box = m->proc ? m->proc->box : NULL;
    if (box && lfi_box_bufvalid(box, addr, (size_t) n))
        dirents_convert((uint8_t *) lfi_box_l2p(box, addr), (size_t) n, false);
    cage_put(m);
}

NATIVE(n_getdents64);

NATIVE(n_getdents)
{
    long r = n_getdents64(c);
    if (r > 0) {
        void *b = call_buf(c, 1, (size_t) r, 8);
        if (b)
            dirents_convert(b, (size_t) r, true);
    }
    return r;
}

NATIVE(n_getdents64)
{
    size_t n = c->a[2];
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    long r;
    void *b = call_buf(c, 1, n, 8);
    if (!b)
        r = -EFAULT;
    else
        r = sysret(syscall(SYS_getdents64, f.hfd, b, n));
    fd_put(&f);
    return r;
}

static long
do_access(struct Call *c, int di, int pi, int mode)
{
    char host[FILENAME_MAX];
    long r = resolve_arg(c, di, pi, host, sizeof(host), NULL);
    if (r)
        return r;
    return sysret(access(host, mode & (R_OK | W_OK | X_OK)));
}

NATIVE(n_access)
{
    return do_access(c, -1, 0, (int) c->a[1]);
}

NATIVE(n_faccessat)
{
    return do_access(c, 0, 1, (int) c->a[2]);
}

static long
do_mkdir(struct Call *c, int di, int pi, mode_t mode)
{
    char host[FILENAME_MAX];
    long r = resolve_arg(c, di, pi, host, sizeof(host), NULL);
    if (r)
        return r;
    return sysret(mkdir(host, mode & 0777 & ~c->target->umask));
}

NATIVE(n_mkdir)
{
    return do_mkdir(c, -1, 0, (mode_t) c->a[1]);
}

NATIVE(n_mkdirat)
{
    return do_mkdir(c, 0, 1, (mode_t) c->a[2]);
}

static long
do_unlink(struct Call *c, int di, int pi, int flags)
{
    if (flags & ~AT_REMOVEDIR)
        return -EINVAL;
    char host[FILENAME_MAX];
    long r = resolve_arg(c, di, pi, host, sizeof(host), NULL);
    if (r)
        return r;
    return sysret((flags & AT_REMOVEDIR) ? rmdir(host) : unlink(host));
}

NATIVE(n_unlink)
{
    return do_unlink(c, -1, 0, 0);
}

NATIVE(n_rmdir)
{
    return do_unlink(c, -1, 0, AT_REMOVEDIR);
}

NATIVE(n_unlinkat)
{
    return do_unlink(c, 0, 1, (int) c->a[2]);
}

static long
do_rename(struct Call *c, int od, int op, int nd, int np, unsigned flags)
{
    char oh[FILENAME_MAX], nh[FILENAME_MAX];
    long r = resolve_arg(c, od, op, oh, sizeof(oh), NULL);
    if (r)
        return r;
    r = resolve_arg(c, nd, np, nh, sizeof(nh), NULL);
    if (r)
        return r;
    if (flags)
        return sysret(syscall(SYS_renameat2, AT_FDCWD, oh, AT_FDCWD, nh,
            flags));
    return sysret(rename(oh, nh));
}

NATIVE(n_rename)
{
    return do_rename(c, -1, 0, -1, 1, 0);
}

NATIVE(n_renameat)
{
    return do_rename(c, 0, 1, 2, 3, 0);
}

NATIVE(n_renameat2)
{
    return do_rename(c, 0, 1, 2, 3, (unsigned) c->a[4]);
}

NATIVE(n_link)
{
    char oh[FILENAME_MAX], nh[FILENAME_MAX];
    long r = resolve_arg(c, -1, 0, oh, sizeof(oh), NULL);
    if (r)
        return r;
    r = resolve_arg(c, -1, 1, nh, sizeof(nh), NULL);
    if (r)
        return r;
    return sysret(link(oh, nh));
}

NATIVE(n_symlink)
{
    char *target = call_path(c, 0);
    if (!target)
        return -EFAULT;
    char nh[FILENAME_MAX];
    long r = resolve_arg(c, -1, 1, nh, sizeof(nh), NULL);
    if (r) {
        free(target);
        return r;
    }
    r = sysret(symlink(target, nh));
    free(target);
    return r;
}

static long
do_readlink(struct Call *c, int di, int pi, int bi, size_t size)
{
    char host[FILENAME_MAX];
    long r = resolve_arg(c, di, pi, host, sizeof(host), NULL);
    if (r)
        return r;
    if (size == 0)
        return -EINVAL;
    char *buf = call_buf(c, bi, size, 1);
    if (!buf)
        return -EFAULT;
    return sysret(readlink(host, buf, size));
}

NATIVE(n_readlink)
{
    return do_readlink(c, -1, 0, 1, c->a[2]);
}

NATIVE(n_readlinkat)
{
    return do_readlink(c, 0, 1, 2, c->a[3]);
}

NATIVE(n_chmod)
{
    char host[FILENAME_MAX];
    long r = resolve_arg(c, -1, 0, host, sizeof(host), NULL);
    return r ? r : sysret(chmod(host, (mode_t) c->a[1] & 07777));
}

NATIVE(n_fchmod)
{
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    long r = sysret(fchmod(f.hfd, (mode_t) c->a[1] & 07777));
    fd_put(&f);
    return r;
}

NATIVE(n_fchown)
{
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    long r = sysret(fchown(f.hfd, (uid_t) c->a[1], (gid_t) c->a[2]));
    fd_put(&f);
    return r;
}

NATIVE(n_truncate)
{
    char host[FILENAME_MAX];
    long r = resolve_arg(c, -1, 0, host, sizeof(host), NULL);
    return r ? r : sysret(truncate(host, (off_t) c->a[1]));
}

NATIVE(n_ftruncate)
{
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    long r = sysret(ftruncate(f.hfd, (off_t) c->a[1]));
    fd_put(&f);
    return r;
}

NATIVE(n_fsync)
{
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    long r = sysret(c->nr == 75 ? fdatasync(f.hfd) : fsync(f.hfd));
    fd_put(&f);
    return r;
}

NATIVE(n_chdir)
{
    char *p = call_path(c, 0);
    if (!p)
        return -EFAULT;
    long r = proc_chdir(tproc(c), p);
    free(p);
    return r;
}

NATIVE(n_fchdir)
{
    struct FDTable *t = call_fdt(c, 0);
    int fd = (int) c->a[0];
    if (!t || fd < 0 || fd >= LINUX_NOFILE)
        return -EBADF;
    char dir[FILENAME_MAX];
    {
        LOCK_WITH_DEFER(&t->lk, lk);
        if (t->fds[fd] == -1)
            return -EBADF;
        if (!t->dirs[fd])
            return -ENOTDIR;
        strncpy(dir, t->dirs[fd], sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = 0;
    }
    return proc_chdir(tproc(c), dir);
}

NATIVE(n_getcwd)
{
    size_t size = c->a[1];
    if (size == 0)
        return -EINVAL;
    char *buf = call_buf(c, 0, size, 1);
    if (!buf)
        return -EFAULT;
    struct LFILinuxProc *pr = tproc(c);
    LOCK_WITH_DEFER(&pr->cwd.lk, lk);
    size_t len = strnlen(pr->cwd.path, sizeof(pr->cwd.path) - 1) + 1;
    if (len > size)
        return -ERANGE;
    memcpy(buf, pr->cwd.path, len);
    return (long) len;
}

NATIVE(n_umask)
{
    uint32_t old = c->target->umask;
    c->target->umask = (uint32_t) c->a[0] & 0777;
    return old;
}

#define LINUX_TIOCGWINSZ 0x5413
#define LINUX_TCGETS     0x5401
#define LINUX_FIONREAD   0x541B

NATIVE(n_ioctl)
{
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    long r;
    switch (c->a[1]) {
    case LINUX_TIOCGWINSZ: {
        void *b = call_buf(c, 2, sizeof(struct winsize), 2);
        r = b ? sysret(ioctl(f.hfd, TIOCGWINSZ, b)) : -EFAULT;
        break;
    }
    case LINUX_TCGETS: {
        // The kernel's struct termios is 36 bytes.
        void *b = call_buf(c, 2, 36, 4);
        r = b ? sysret(ioctl(f.hfd, TCGETS, b)) : -EFAULT;
        break;
    }
    case LINUX_FIONREAD: {
        void *b = call_buf(c, 2, sizeof(int), 4);
        r = b ? sysret(ioctl(f.hfd, FIONREAD, b)) : -EFAULT;
        break;
    }
    default:
        r = -ENOTTY;
    }
    fd_put(&f);
    return r;
}

NATIVE(n_poll)
{
    uint64_t nfds = c->a[1];
    if (nfds > LINUX_NOFILE)
        return -EINVAL;
    struct pollfd *gp = nfds ? call_buf(c, 0, nfds * sizeof(struct pollfd), 4) :
                               NULL;
    if (nfds && !gp)
        return -EFAULT;
    struct pollfd hp[LINUX_NOFILE];
    struct FDTable *t = call_target_fdt(c);
    if (!t)
        return -EBADF;
    int held[LINUX_NOFILE];
    for (uint64_t i = 0; i < nfds; i++) {
        hp[i].events = gp[i].events;
        hp[i].revents = 0;
        int fd = gp[i].fd;
        held[i] = fd >= 0 ? fdacquire(t, fd, NULL, NULL) : -1;
        // Negative descriptors are ignored by poll; unknown ones report
        // POLLNVAL, which we emulate after the call.
        hp[i].fd = held[i] >= 0 ? held[i] : -1;
    }
    int timeout = (int) c->a[2];
    long r = sysret(poll(hp, nfds, timeout));
    long ready = 0;
    if (r >= 0) {
        for (uint64_t i = 0; i < nfds; i++) {
            if (gp[i].fd >= 0 && held[i] < 0)
                gp[i].revents = POLLNVAL;
            else
                gp[i].revents = hp[i].revents;
            if (gp[i].revents)
                ready++;
        }
        r = ready;
    }
    for (uint64_t i = 0; i < nfds; i++) {
        if (held[i] >= 0)
            fdrelease(t, held[i]);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Time, identity, misc
// ---------------------------------------------------------------------------

NATIVE(n_gettimeofday)
{
    struct timeval *tv = c->a[0] ? call_buf(c, 0, sizeof(*tv), 8) : NULL;
    if (c->a[0] && !tv)
        return -EFAULT;
    if (tv)
        return sysret(gettimeofday(tv, NULL));
    return 0;
}

NATIVE(n_clock_nanosleep)
{
    // clock_nanosleep(clockid, flags, req, rem)
    struct timespec *req = call_buf(c, 2, sizeof(*req), 8);
    if (!req)
        return -EFAULT;
    struct timespec *rem = c->a[3] ? call_buf(c, 3, sizeof(*rem), 8) : NULL;
    if (c->a[3] && !rem)
        return -EFAULT;
    int r = clock_nanosleep((clockid_t) c->a[0], (int) c->a[1], req, rem);
    return -r;
}

NATIVE(n_getuid)
{
    return 1000;
}

NATIVE(n_getppid)
{
    return c->target->ppid;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

long
threei_native(struct Call *c)
{
    uint64_t *a = c->a;
    switch (c->nr) {
    case 0:   return n_read(c);
    case 1:   return n_write(c);
    case 2:   return n_open(c);
    case 3:   return n_close(c);
    case 4:   return n_stat(c);
    case 5:   return n_fstat(c);
    case 6:   return n_lstat(c);
    case 7:   return n_poll(c);
    case 8:   return n_lseek(c);
    case 9:   return n_mmap(c);
    case 10:  return n_mprotect(c);
    case 11:  return n_munmap(c);
    case 12:  return sys_brk(c->t, a[0]);
    case 16:  return n_ioctl(c);
    case 17:  return n_pread(c);
    case 18:  return n_pwrite(c);
    case 19:  return n_readv(c);
    case 20:  return n_writev(c);
    case 21:  return n_access(c);
    case 22:  return n_pipe(c);
    case 24:  return sys_sched_yield(c->t);
    case 28:  return 0; /* madvise */
    case 32:  return n_dup(c);
    case 33:  return n_dup2(c);
    case 35:  return sys_nanosleep(c->t, a[0], a[1]);
    case 39:  return c->target->id; /* getpid */
    case 56:  return nat_clone(c);
    case 57:  return cage_fork(c, SIGCHLD, 0);
    case 58:  return cage_fork(c, SIGCHLD, 0); /* vfork */
    case 59:  return cage_exec(c);
    case 60:  return cage_exit(c, false);
    case 61:  return cage_wait(c);
    case 62:  return cage_kill(c);
    case 63:  return sys_uname(c->t, a[0]);
    case 72:  return n_fcntl(c);
    case 74:  /* fsync */
    case 75:  /* fdatasync */
        return n_fsync(c);
    case 76:  return n_truncate(c);
    case 77:  return n_ftruncate(c);
    case 79:  return n_getcwd(c);
    case 80:  return n_chdir(c);
    case 81:  return n_fchdir(c);
    case 82:  return n_rename(c);
    case 83:  return n_mkdir(c);
    case 84:  return n_rmdir(c);
    case 85:  return n_creat(c);
    case 86:  return n_link(c);
    case 87:  return n_unlink(c);
    case 88:  return n_symlink(c);
    case 89:  return n_readlink(c);
    case 90:  return n_chmod(c);
    case 91:  return n_fchmod(c);
    case 93:  return n_fchown(c);
    case 95:  return n_umask(c);
    case 96:  return n_gettimeofday(c);
    case 97:  return sys_getrlimit(c->t, (int) a[0], a[1]);
    case 99:  return sys_sysinfo(c->t, a[0]);
    case 102: /* getuid */
    case 104: /* getgid */
    case 107: /* geteuid */
    case 108: /* getegid */
        return n_getuid(c);
    case 110: return n_getppid(c);
    case 111: /* getpgrp */
        return c->target->id;
    case 157: return sys_prctl(c->t, (int) a[0], a[1], a[2], a[3], a[4]);
    case 158: return sys_arch_prctl(c->t, (int) a[0], a[1]);
    case 186: return c->t ? c->t->tid : -ESRCH; /* gettid */
    case 200: /* tkill */
    case 234: /* tgkill */
        return cage_kill(c);
    case 201: return sys_time(c->t, a[0]);
    case 202: return sys_futex(c->t, a[0], (int) a[1], (uint32_t) a[2], a[3],
                  a[4], (uint32_t) a[5]);
    case 203: return 0; /* sched_setaffinity */
    case 204: return sys_sched_getaffinity(c->t, (int32_t) a[0], a[1], a[2]);
    case 78:  return n_getdents(c);
    case 217: return n_getdents64(c);
    case 218: return sys_set_tid_address(c->t, a[0]);
    case 228: return sys_clock_gettime(c->t, (int) a[0], a[1]);
    case 230: return n_clock_nanosleep(c);
    case 231: return cage_exit(c, true);
    case 257: return n_openat(c);
    case 258: return n_mkdirat(c);
    case 262: return n_newfstatat(c);
    case 263: return n_unlinkat(c);
    case 264: return n_renameat(c);
    case 267: return n_readlinkat(c);
    case 269: return n_faccessat(c);
    case 273: return 0; /* set_robust_list */
    case 292: return n_dup3(c);
    case 293: return n_pipe2(c);
    case 316: return n_renameat2(c);
    case 318: return sys_getrandom(c->t, a[0], a[1], (unsigned) a[2]);
    case 324: return 0; /* membarrier */
    case 332: return -ENOSYS; /* statx */
    case 334: return -ENOSYS; /* rseq */

    // 3i calls.
    case LFI3I_SYS_CALL:
        // Only the guest itself can ask the host to unpack a request.
        if (!c->t)
            return -EPERM;
        return threei_make_call(c->caller, c->t, a[0]);
    case LFI3I_SYS_REGISTER:
        return n3i_register_handler(c);
    case LFI3I_SYS_COPY_DATA:
        return n3i_copy_data(c);
    case LFI3I_SYS_COPY_HTAB:
        return n3i_copy_htab(c);
    case LFI3I_SYS_SETENTRY:
        return grate_setentry(c->caller, c->t, a[0], a[1]);
    case LFI3I_SYS_CAPTURE:
        lfi_capture_clone = true;
        tl_spawning = true;
        return 0;
    case LFI3I_SYS_SELF:
        return c->caller->id;
    case LFI3I_SYS_COLLECT:
        return grate_collect(c->caller, (long) a[0]);
    case 1024: /* lfi pause */
        return sys_lfi_pause(c->t);
    default: {
        long nr_ret;
        if (nat_signal(c, &nr_ret) || nat_misc(c, &nr_ret) ||
            nat_net(c, &nr_ret))
            return nr_ret;
        return -ENOSYS;
    }
    }
}
