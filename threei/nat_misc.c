#define _GNU_SOURCE
// Less common file, time and identity calls.
#include "cage.h"
#include "natutil.h"

#include "sys/sys.h"

#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

NATIVE(n_flock)
{
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    long r = sysret(flock(f.hfd, (int) c->a[1]));
    fd_put(&f);
    return r;
}

NATIVE(n_fallocate)
{
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    long r = sysret(fallocate(f.hfd, (int) c->a[1], (off_t) c->a[2], (off_t) c->a[3]));
    fd_put(&f);
    return r;
}

NATIVE(n_sendfile)
{
    // sendfile(out_fd, in_fd, off_t *offset, count)
    struct FDR o, i;
    if (fd_get(c, 0, &o))
        return -EBADF;
    if (fd_get(c, 1, &i)) {
        fd_put(&o);
        return -EBADF;
    }
    off_t *off = NULL;
    long r = 0;
    if (c->a[2]) {
        off = call_buf(c, 2, sizeof(off_t), alignof(off_t));
        if (!off)
            r = -EFAULT;
    }
    if (!r)
        r = sysret(sendfile(o.hfd, i.hfd, off, c->a[3] > 0x7ffff000 ? 0x7ffff000 : c->a[3]));
    fd_put(&i);
    fd_put(&o);
    return r;
}

NATIVE(n_statfs)
{
    // The kernel's struct statfs on x86-64 is 120 bytes.
    void *buf = call_buf(c, 1, 120, 8);
    if (!buf)
        return -EFAULT;
    char host[FILENAME_MAX];
    long r = resolve_arg(c, -1, 0, host, sizeof(host), NULL);
    if (r)
        return r;
    return sysret(syscall(SYS_statfs, host, buf));
}

NATIVE(n_fstatfs)
{
    void *buf = call_buf(c, 1, 120, 8);
    if (!buf)
        return -EFAULT;
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    long r = sysret(syscall(SYS_fstatfs, f.hfd, buf));
    fd_put(&f);
    return r;
}

NATIVE(n_getrusage)
{
    void *ru = call_buf(c, 1, 144, 8);
    if (!ru)
        return -EFAULT;
    int who = (int) c->a[0];
    if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN && who != RUSAGE_THREAD)
        return -EINVAL;
    return sysret(syscall(SYS_getrusage, who, ru));
}

NATIVE(n_times)
{
    struct tms *t = c->a[0] ? call_buf(c, 0, sizeof(*t), 8) : NULL;
    if (c->a[0] && !t)
        return -EFAULT;
    return sysret((long) times(t));
}

NATIVE(n_clock_getres)
{
    struct timespec *ts = c->a[1] ? call_buf(c, 1, sizeof(*ts), 8) : NULL;
    if (c->a[1] && !ts)
        return -EFAULT;
    return sysret(clock_getres((clockid_t) c->a[0], ts));
}

NATIVE(n_prlimit)
{
    // prlimit64(pid, resource, new, old): limits are reported but not changed.
    if (c->a[0] != 0 && c->a[0] != c->target->id)
        return -EPERM;
    if (c->a[3])
        return sys_getrlimit(c->t, (int) c->a[1], c->a[3]);
    return 0;
}

NATIVE(n_utimensat)
{
    // utimensat(dirfd, path, times[2], flags); a NULL path means "the fd".
    struct timespec *ts = c->a[2] ? call_buf(c, 2, 2 * sizeof(*ts), 8) : NULL;
    if (c->a[2] && !ts)
        return -EFAULT;
    int flags = (int) c->a[3];
    if (flags & ~AT_SYMLINK_NOFOLLOW)
        return -EINVAL;
    if (c->a[1] == 0) {
        struct FDR f;
        if (fd_get(c, 0, &f))
            return -EBADF;
        long r = sysret(futimens(f.hfd, ts));
        fd_put(&f);
        return r;
    }
    char host[FILENAME_MAX];
    long r = resolve_arg(c, 0, 1, host, sizeof(host), NULL);
    if (r)
        return r;
    return sysret(utimensat(AT_FDCWD, host, ts, flags));
}

NATIVE(n_utimes)
{
    // utimes(path, struct timeval times[2])
    struct timeval *tv = c->a[1] ? call_buf(c, 1, 2 * sizeof(*tv), 8) : NULL;
    if (c->a[1] && !tv)
        return -EFAULT;
    char host[FILENAME_MAX];
    long r = resolve_arg(c, -1, 0, host, sizeof(host), NULL);
    if (r)
        return r;
    return sysret(utimes(host, tv));
}

NATIVE(n_fchmodat)
{
    char host[FILENAME_MAX];
    long r = resolve_arg(c, 0, 1, host, sizeof(host), NULL);
    return r ? r : sysret(chmod(host, (mode_t) c->a[2] & 07777));
}

NATIVE(n_chown)
{
    char host[FILENAME_MAX];
    long r = resolve_arg(c, -1, 0, host, sizeof(host), NULL);
    if (r)
        return r;
    return sysret(c->nr == 94 ? lchown(host, (uid_t) c->a[1], (gid_t) c->a[2]) :
                                chown(host, (uid_t) c->a[1], (gid_t) c->a[2]));
}

NATIVE(n_fchownat)
{
    char host[FILENAME_MAX];
    long r = resolve_arg(c, 0, 1, host, sizeof(host), NULL);
    if (r)
        return r;
    return sysret(fchownat(AT_FDCWD, host, (uid_t) c->a[2], (gid_t) c->a[3],
        (int) c->a[4] & AT_SYMLINK_NOFOLLOW));
}

NATIVE(n_linkat)
{
    // linkat(olddirfd, oldpath, newdirfd, newpath, flags)
    char oh[FILENAME_MAX], nh[FILENAME_MAX];
    long r = resolve_arg(c, 0, 1, oh, sizeof(oh), NULL);
    if (r)
        return r;
    r = resolve_arg(c, 2, 3, nh, sizeof(nh), NULL);
    if (r)
        return r;
    return sysret(linkat(AT_FDCWD, oh, AT_FDCWD, nh, (int) c->a[4] & AT_SYMLINK_FOLLOW));
}

NATIVE(n_symlinkat)
{
    // symlinkat(target, newdirfd, linkpath)
    char *target = call_path(c, 0);
    if (!target)
        return -EFAULT;
    char nh[FILENAME_MAX];
    long r = resolve_arg(c, 1, 2, nh, sizeof(nh), NULL);
    if (!r)
        r = sysret(symlink(target, nh));
    free(target);
    return r;
}


// mknod / mknodat: FIFOs, regular files and sockets. Device nodes are refused:
// a sandbox must not create them.
static long
do_mknod(struct Call *c, int di, int pi, mode_t mode)
{
    char host[FILENAME_MAX];
    long r = resolve_arg(c, di, pi, host, sizeof(host), NULL);
    if (r)
        return r;
    mode_t perm = mode & 0777 & ~c->target->umask;
    switch (mode & S_IFMT) {
    case 0:
    case S_IFREG: {
        int fd = open(host, O_CREAT | O_EXCL | O_WRONLY, perm);
        if (fd < 0)
            return -errno;
        close(fd);
        return 0;
    }
    case S_IFIFO:
        return sysret(mkfifo(host, perm));
    case S_IFSOCK:
        return sysret(mknod(host, S_IFSOCK | perm, 0));
    default:
        return -EPERM;
    }
}

NATIVE(n_mknod)
{
    return do_mknod(c, -1, 0, (mode_t) c->a[1]);
}

NATIVE(n_mknodat)
{
    return do_mknod(c, 0, 1, (mode_t) c->a[2]);
}

// getpriority / setpriority: the scheduling priority of a cage is kept by the
// runtime (the host's is not the sandbox's to change). Only "this process".
NATIVE(n_getpriority)
{
    if (c->a[0] != 0 /* PRIO_PROCESS */ ||
        (c->a[1] != 0 && c->a[1] != c->target->id))
        return c->a[0] > 2 ? -EINVAL : -ESRCH;
    return 20 - c->target->nice; // the kernel ABI returns 20 - nice
}

NATIVE(n_setpriority)
{
    if (c->a[0] != 0 || (c->a[1] != 0 && c->a[1] != c->target->id))
        return c->a[0] > 2 ? -EINVAL : -ESRCH;
    int v = (int) c->a[2];
    if (v > 19)
        v = 19;
    if (v < -20)
        v = -20;
    if (v < c->target->nice)
        return -EACCES; // unprivileged: may only lower the priority
    c->target->nice = v;
    return 0;
}

// preadv / pwritev (and the offset-less forms handled in nat.c).
#define IOVN 1024
NATIVE(n_preadv)
{
    uint64_t cnt = c->a[2];
    if (cnt > IOVN)
        return -EINVAL;
    struct { uint64_t base, len; } *g = cnt ? call_buf(c, 1, cnt * 16, 8) : NULL;
    if (cnt && !g)
        return -EFAULT;
    struct LFIBox *box = call_box(c, 1);
    struct iovec iov[IOVN];
    uint64_t total = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        uint64_t base = g[i].base, len = g[i].len;
        total += len;
        if (len > 0x7ffff000UL || total > 0x7ffff000UL)
            return -EINVAL;
        if (len && !lfi_box_bufvalid(box, base, len))
            return -EFAULT;
        iov[i].iov_base = len ? (void *) lfi_box_l2p(box, base) : NULL;
        iov[i].iov_len = len;
    }
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    long r = cnt == 0 ? 0 :
        sysret(c->nr == 295 ? preadv(f.hfd, iov, (int) cnt, (off_t) c->a[3]) :
                              pwritev(f.hfd, iov, (int) cnt, (off_t) c->a[3]));
    fd_put(&f);
    return r;
}

NATIVE(n_getpgid)
{
    int pid = (int) c->a[0];
    if (pid == 0)
        return c->target->id;
    struct Cage *x = cage_get((uint32_t) pid);
    if (!x)
        return -ESRCH;
    cage_put(x);
    return pid; // every cage leads its own process group and session
}

NATIVE(n_ok)
{
    return 0;
}

bool
nat_misc(struct Call *c, long *ret)
{
    switch (c->nr) {
    case 26:  /* msync */
    case 105: /* setuid */
    case 106: /* setgid */
    case 109: /* setpgid */
    case 113: /* setreuid */
    case 114: /* setregid */
    case 115: /* getgroups: no supplementary groups */
    case 116: /* setgroups */
    case 117: /* setresuid */
    case 119: /* setresgid */
    case 149: /* mlock */
    case 150: /* munlock */
    case 151: /* mlockall */
    case 152: /* munlockall */
    case 160: /* setrlimit: limits are not enforced */
        *ret = n_ok(c); return true;
    case 40:  *ret = n_sendfile(c); return true;
    case 73:  *ret = n_flock(c); return true;
    case 98:  *ret = n_getrusage(c); return true;
    case 100: *ret = n_times(c); return true;
    case 112: /* setsid */
        *ret = c->target->id; return true;
    case 121: /* getpgid */
    case 124: /* getsid */
        *ret = n_getpgid(c); return true;
    case 137: *ret = n_statfs(c); return true;
    case 138: *ret = n_fstatfs(c); return true;
    case 162: sync(); *ret = 0; return true;
    case 229: *ret = n_clock_getres(c); return true;
    case 235: *ret = n_utimes(c); return true;
    case 92:  /* chown */
    case 94:  /* lchown */
        *ret = n_chown(c); return true;
    case 260: *ret = n_fchownat(c); return true;
    case 265: *ret = n_linkat(c); return true;
    case 266: *ret = n_symlinkat(c); return true;
    case 133: *ret = n_mknod(c); return true;
    case 259: *ret = n_mknodat(c); return true;
    case 140: *ret = n_getpriority(c); return true;
    case 141: *ret = n_setpriority(c); return true;
    case 268: *ret = n_fchmodat(c); return true;
    case 280: *ret = n_utimensat(c); return true;
    case 285: *ret = n_fallocate(c); return true;
    case 295: /* preadv */
    case 296: /* pwritev */
        *ret = n_preadv(c); return true;
    case 302: *ret = n_prlimit(c); return true;
    default:  return false;
    }
}
