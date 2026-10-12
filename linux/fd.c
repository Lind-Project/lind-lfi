#include "fd.h"

#include "lock.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

// Sandbox file descriptors map to host file descriptors. A host descriptor can
// be referenced from several tables at once (after fork, or after dup), so
// each host descriptor carries a reference count and is closed when the last
// reference is dropped. Borrowing a descriptor for the duration of one
// operation (fdacquire/fdrelease) is just another reference, which costs one
// atomic operation instead of the dup+close pair this used to take.

#define MAX_TRACKED_FDS (1u << 22)

static _Atomic(int) *fd_refs;
static size_t fd_nrefs;
static pthread_once_t fd_refs_once = PTHREAD_ONCE_INIT;

static void
fd_refs_init(void)
{
    struct rlimit rl;
    size_t n = 1u << 16;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        // Use as many descriptors as the system lets us.
        if (rl.rlim_cur < rl.rlim_max) {
            rl.rlim_cur = rl.rlim_max;
            setrlimit(RLIMIT_NOFILE, &rl);
        }
        n = rl.rlim_max == RLIM_INFINITY ? MAX_TRACKED_FDS : rl.rlim_max;
        if (n > MAX_TRACKED_FDS)
            n = MAX_TRACKED_FDS;
    }
    fd_refs = calloc(n, sizeof(*fd_refs));
    if (!fd_refs) {
        fprintf(stderr, "lfi: out of memory allocating fd refcounts\n");
        abort();
    }
    fd_nrefs = n;
}

static inline void
refs_ensure(void)
{
    pthread_once(&fd_refs_once, fd_refs_init);
}

// Take a reference to an open host descriptor.
static inline void
hostfd_ref(int hostfd)
{
    atomic_fetch_add_explicit(&fd_refs[hostfd], 1, memory_order_relaxed);
}

// Drop a reference to a host descriptor, closing it if it was the last one.
static inline void
hostfd_unref(int hostfd)
{
    if (atomic_fetch_sub_explicit(&fd_refs[hostfd], 1,
            memory_order_acq_rel) == 1)
        close(hostfd);
}

int
fdalloc(struct FDTable *t, int host_fd, char *dir, int flags)
{
    if (t->passthrough) {
        free(dir);
        return host_fd;
    }
    refs_ensure();
    if ((size_t) host_fd >= fd_nrefs) {
        free(dir);
        return -LINUX_EMFILE;
    }
    LOCK_WITH_DEFER(&t->lk, t_lk);
    // Pick the lowest-numbered free slot, independent of the host fd number.
    for (int fd = 0; fd < LINUX_NOFILE; fd++) {
        if (t->fds[fd] == -1) {
            hostfd_ref(host_fd);
            t->fds[fd] = host_fd;
            t->dirs[fd] = dir;
            t->flags[fd] = flags;
            return fd;
        }
    }
    free(dir);
    return -LINUX_EMFILE;
}

int
fdacquire(struct FDTable *t, int fd, int *o_flags, bool *o_isdir)
{
    if (o_flags)
        *o_flags = 0;
    if (o_isdir)
        *o_isdir = false;
    if (t->passthrough)
        return fd;
    if (fd < 0 || fd >= LINUX_NOFILE)
        return -1;
    LOCK_WITH_DEFER(&t->lk, lk);
    int hostfd = t->fds[fd];
    if (hostfd == -1)
        return -1;
    if (o_flags)
        *o_flags = t->flags[fd];
    if (o_isdir)
        *o_isdir = t->dirs[fd] != NULL;
    // The reference keeps the host descriptor open even if another thread
    // closes the sandbox descriptor while the operation is in progress.
    hostfd_ref(hostfd);
    return hostfd;
}

void
fdrelease(struct FDTable *t, int hostfd)
{
    if (t->passthrough)
        return;
    if (hostfd >= 0)
        hostfd_unref(hostfd);
}

int
fddup2(struct FDTable *t, int oldfd, int newfd)
{
    if (oldfd < 0 || oldfd >= LINUX_NOFILE || newfd < -1 || newfd >= LINUX_NOFILE)
        return -LINUX_EBADF;
    LOCK_WITH_DEFER(&t->lk, lk);
    int koldfd = t->fds[oldfd];
    if (koldfd == -1)
        return -LINUX_EBADF;
    if (oldfd == newfd)
        return newfd;
    char *dir = NULL;
    if (t->dirs[oldfd]) {
        dir = strndup(t->dirs[oldfd], FILENAME_MAX - 1);
        if (!dir)
            return -LINUX_ENOMEM;
    }
    if (newfd == -1) {
        // Pick the lowest unoccupied sandbox fd.
        for (int fd = 0; fd < LINUX_NOFILE; fd++) {
            if (t->fds[fd] == -1) {
                newfd = fd;
                break;
            }
        }
        if (newfd == -1) {
            free(dir);
            return -LINUX_EMFILE;
        }
    } else {
        // Replace whatever newfd currently refers to.
        if (t->fds[newfd] != -1)
            hostfd_unref(t->fds[newfd]);
        free(t->dirs[newfd]);
        t->dirs[newfd] = NULL;
    }
    hostfd_ref(koldfd);
    t->fds[newfd] = koldfd;
    t->dirs[newfd] = dir;
    // FD_CLOEXEC is a property of the descriptor slot, not of the open file,
    // so it is not inherited by the duplicate.
    t->flags[newfd] = t->flags[oldfd] & ~LINUX_O_CLOEXEC;
    return newfd;
}

bool
fdclose(struct FDTable *t, int fd)
{
    if (t->passthrough)
        return true;
    if (fd < 0 || fd >= LINUX_NOFILE)
        return false;
    int hostfd;
    {
        LOCK_WITH_DEFER(&t->lk, lk);
        hostfd = t->fds[fd];
        if (hostfd == -1)
            return false;
        t->fds[fd] = -1;
        t->flags[fd] = 0;
        free(t->dirs[fd]);
        t->dirs[fd] = NULL;
    }
    hostfd_unref(hostfd);
    return true;
}

void
fdinit(struct LFILinuxEngine *engine, struct FDTable *t)
{
    pthread_mutex_init(&t->lk, NULL);

    for (size_t i = 0; i < LINUX_NOFILE; i++) {
        t->fds[i] = -1;
        t->dirs[i] = NULL;
        t->flags[i] = 0;
    }

    t->passthrough = engine->opts.sys_passthrough;
    if (t->passthrough)
        return;

#ifndef SYS_MINIMAL
    refs_ensure();
    // The sandbox's standard streams are private duplicates of ours, so
    // closing them in the sandbox never closes the host's.
    for (int i = 0; i < 3; i++) {
        int d = dup(i);
        if (d >= 0 && (size_t) d < fd_nrefs) {
            hostfd_ref(d);
            t->fds[i] = d;
        } else if (d >= 0) {
            close(d);
        }
    }
#endif
}

void
fdfree(struct FDTable *t)
{
    if (t->passthrough)
        return;
    for (size_t i = 0; i < LINUX_NOFILE; i++) {
        if (t->fds[i] == -1)
            continue;
        hostfd_unref(t->fds[i]);
        t->fds[i] = -1;
        free(t->dirs[i]);
        t->dirs[i] = NULL;
    }
}

int
fdclone(struct FDTable *dst, struct FDTable *src)
{
    refs_ensure();
    LOCK_WITH_DEFER(&src->lk, lk);
    for (size_t i = 0; i < LINUX_NOFILE; i++) {
        if (src->fds[i] == -1)
            continue;
        char *dir = NULL;
        if (src->dirs[i]) {
            dir = strndup(src->dirs[i], FILENAME_MAX - 1);
            if (!dir)
                return -LINUX_ENOMEM;
        }
        hostfd_ref(src->fds[i]);
        dst->fds[i] = src->fds[i];
        dst->dirs[i] = dir;
        dst->flags[i] = src->flags[i];
    }
    return 0;
}

void
fdcloexec(struct FDTable *t)
{
    for (size_t i = 0; i < LINUX_NOFILE; i++) {
        int hostfd;
        {
            LOCK_WITH_DEFER(&t->lk, lk);
            if (t->fds[i] == -1 || !(t->flags[i] & LINUX_O_CLOEXEC))
                continue;
            hostfd = t->fds[i];
            t->fds[i] = -1;
            t->flags[i] = 0;
            free(t->dirs[i]);
            t->dirs[i] = NULL;
        }
        hostfd_unref(hostfd);
    }
}

int
fdsetflags(struct FDTable *t, int fd, int flags)
{
    if (fd < 0 || fd >= LINUX_NOFILE)
        return -LINUX_EBADF;
    LOCK_WITH_DEFER(&t->lk, lk);
    if (t->fds[fd] == -1)
        return -LINUX_EBADF;
    t->flags[fd] = flags;
    return 0;
}

int
fdgetflags(struct FDTable *t, int fd)
{
    if (fd < 0 || fd >= LINUX_NOFILE)
        return -LINUX_EBADF;
    LOCK_WITH_DEFER(&t->lk, lk);
    if (t->fds[fd] == -1)
        return -LINUX_EBADF;
    return t->flags[fd];
}

int
fdalloc_min(struct FDTable *t, int host_fd, int minfd, int flags)
{
    if (t->passthrough)
        return host_fd;
    refs_ensure();
    if (minfd < 0 || minfd >= LINUX_NOFILE ||
        (size_t) host_fd >= fd_nrefs)
        return -LINUX_EINVAL;
    LOCK_WITH_DEFER(&t->lk, lk);
    for (int fd = minfd; fd < LINUX_NOFILE; fd++) {
        if (t->fds[fd] == -1) {
            hostfd_ref(host_fd);
            t->fds[fd] = host_fd;
            t->dirs[fd] = NULL;
            t->flags[fd] = flags;
            return fd;
        }
    }
    return -LINUX_EMFILE;
}

int
fddup_min(struct FDTable *t, int oldfd, int minfd, int flags)
{
    if (t->passthrough)
        return -LINUX_EINVAL;
    if (oldfd < 0 || oldfd >= LINUX_NOFILE)
        return -LINUX_EBADF;
    if (minfd < 0 || minfd >= LINUX_NOFILE)
        return -LINUX_EINVAL;
    LOCK_WITH_DEFER(&t->lk, lk);
    int host = t->fds[oldfd];
    if (host == -1)
        return -LINUX_EBADF;
    for (int fd = minfd; fd < LINUX_NOFILE; fd++) {
        if (t->fds[fd] != -1)
            continue;
        char *dir = NULL;
        if (t->dirs[oldfd]) {
            dir = strndup(t->dirs[oldfd], FILENAME_MAX - 1);
            if (!dir)
                return -LINUX_ENOMEM;
        }
        hostfd_ref(host);
        t->fds[fd] = host;
        t->dirs[fd] = dir;
        t->flags[fd] = flags;
        return fd;
    }
    return -LINUX_EMFILE;
}
