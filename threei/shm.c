#define _GNU_SOURCE
// Shared memory mappings (MAP_SHARED).
//
// LFI refuses shared mappings by default because a page mapped both writable
// and executable through two aliases would defeat verification. Shared memory
// that is never executable is safe, and programs rely on it (a grate and the
// cage it forks synchronise through a process-shared semaphore, for example),
// so those are allowed here: the memory is backed by a memfd (or a duplicate of
// the file's descriptor), mapped at the same offset into every sandbox that
// has it, and survives fork. Executable shared mappings are refused.
//
// The backing descriptor of a region is kept in the sandbox's mapping table;
// descriptors are closed when the region is unmapped, the program is replaced
// or the cage exits.
#include "cage.h"
#include "natutil.h"

#include "align.h"
#include "sys/sys.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAX_FDS 64

struct FdSet {
    int fds[MAX_FDS];
    int n;
};

static void
fdset_add(struct FdSet *s, int fd)
{
    for (int i = 0; i < s->n; i++)
        if (s->fds[i] == fd)
            return;
    if (s->n < MAX_FDS)
        s->fds[s->n++] = fd;
}

struct Scan {
    lfiptr lo, hi;     // restrict to regions overlapping [lo, hi)
    struct FdSet set;  // descriptors of shared regions found
    int match_fd;      // when counting: the descriptor to look for
    int uses;
};

static void
scan_cb(lfiptr start, size_t len, struct LFIMapInfo info, void *u)
{
    struct Scan *sc = u;
    if (!(info.flags & LFI_MAP_SHARED) || info.fd < 0)
        return;
    if (start < sc->hi && start + len > sc->lo)
        fdset_add(&sc->set, info.fd);
}

static void
count_cb(lfiptr start, size_t len, struct LFIMapInfo info, void *u)
{
    struct Scan *sc = u;
    if ((info.flags & LFI_MAP_SHARED) && info.fd == sc->match_fd)
        sc->uses++;
}

// Collects the descriptors of shared regions overlapping [lo, hi).
static void
collect(struct LFIBox *box, lfiptr lo, lfiptr hi, struct Scan *sc)
{
    memset(sc, 0, sizeof(*sc));
    sc->lo = lo;
    sc->hi = hi;
    lfi_box_foreach_map(box, scan_cb, sc);
}

// After part of the address space changed: closes the collected descriptors
// that no remaining region uses.
static void
release_unused(struct LFIBox *box, struct Scan *before)
{
    for (int i = 0; i < before->set.n; i++) {
        struct Scan sc;
        memset(&sc, 0, sizeof(sc));
        sc.lo = 0;
        sc.hi = ~(lfiptr) 0;
        sc.match_fd = before->set.fds[i];
        lfi_box_foreach_map(box, count_cb, &sc);
        if (sc.uses == 0)
            close(before->set.fds[i]);
    }
}

// Closes every descriptor that backs a shared region (program replaced or
// cage exiting).
void
shm_close_all(struct LFILinuxProc *p)
{
    if (!p->has_shared)
        return;
    struct Scan sc;
    collect(p->box, 0, ~(lfiptr) 0, &sc);
    for (int i = 0; i < sc.set.n; i++)
        close(sc.set.fds[i]);
    p->has_shared = false;
}

// lfi_box_clone callback: the child maps the same backing.
int
shm_clone_fd(int fd, void *udata)
{
    (void) udata;
    return fcntl(fd, F_DUPFD_CLOEXEC, 3);
}

static long
map_shared(struct Call *c, uint64_t addr, size_t len, int prot, int flags,
    int fd, off_t off)
{
    struct LFILinuxProc *p = c->target->proc;
    struct LFIBox *box = p->box;
    size_t ps = lfi_opts(p->engine->engine).pagesize;

    if (len == 0 || len > SIZE_MAX - ps)
        return -EINVAL;
    len = ceilp(len, ps);
    if (prot & PROT_EXEC)
        return -EACCES; // would alias executable memory
    const int ok = MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE |
        MAP_POPULATE | MAP_DENYWRITE | MAP_STACK;
    if (flags & ~ok)
        return -EINVAL;
    if (off < 0 || (uint64_t) off % ps)
        return -EINVAL;

    // The backing store.
    int sfd;
    if (flags & MAP_ANONYMOUS) {
        sfd = memfd_create("lfi-shm", MFD_CLOEXEC);
        if (sfd < 0)
            return -errno;
        if (ftruncate(sfd, len) < 0) {
            int e = errno;
            close(sfd);
            return -e;
        }
        off = 0;
    } else {
        struct FDTable *t = &p->fdtable;
        int h = fdacquire(t, fd, NULL, NULL);
        if (h < 0)
            return -EBADF;
        sfd = fcntl(h, F_DUPFD_CLOEXEC, 3);
        int e = errno;
        fdrelease(t, h);
        if (sfd < 0)
            return -e;
    }

    // The address range, noting descriptors of any shared regions it replaces.
    struct Scan before;
    memset(&before, 0, sizeof(before));
    if (flags & MAP_FIXED) {
        if (addr % ps || !lfi_box_bufvalid(box, addr, len)) {
            close(sfd);
            return -EINVAL;
        }
        collect(box, addr, addr + len, &before);
    } else {
        addr = lfi_box_mapany(box, len, LFI_PROT_NONE,
            LFI_MAP_PRIVATE | LFI_MAP_ANONYMOUS, -1, 0);
        if (addr == (lfiptr) -1) {
            close(sfd);
            return -ENOMEM;
        }
    }

    // Record the region (non-executable, so nothing to verify), then map the
    // backing over it.
    if (lfi_box_mapat_register(box, addr, len, prot, LFI_MAP_SHARED, sfd,
            off) == (lfiptr) -1) {
        if (!(flags & MAP_FIXED))
            lfi_box_munmap(box, addr, len);
        close(sfd);
        return -ENOMEM;
    }
    void *m = mmap((void *) addr, len, prot, MAP_SHARED | MAP_FIXED, sfd, off);
    if (m == MAP_FAILED) {
        int e = errno;
        lfi_box_munmap(box, addr, len);
        close(sfd);
        release_unused(box, &before);
        return -e;
    }
    p->has_shared = true;
    release_unused(box, &before);
    return (long) addr;
}

long
n_mmap(struct Call *c)
{
    int flags = (int) c->a[3];
    // MAP_STACK and MAP_POPULATE are hints that change nothing here (Rust's
    // and glibc's thread/alt stacks pass MAP_STACK).
    int hints = flags & (MAP_STACK | MAP_POPULATE);
    if ((flags & (MAP_SHARED | MAP_PRIVATE)) != MAP_SHARED)
        flags &= ~hints;
    if ((flags & (MAP_SHARED | MAP_PRIVATE)) != MAP_SHARED) {
        struct LFILinuxProc *p = c->target->proc;
        if (!p->has_shared || !(flags & MAP_FIXED))
            return sys_mmap(c->t, c->a[0], c->a[1], (int) c->a[2], flags,
                (int) c->a[4], (off_t) c->a[5]);
        // A private fixed mapping may replace shared regions.
        struct Scan before;
        size_t ps = lfi_opts(p->engine->engine).pagesize;
        collect(p->box, c->a[0], c->a[0] + ceilp(c->a[1], ps), &before);
        long r = sys_mmap(c->t, c->a[0], c->a[1], (int) c->a[2], flags,
            (int) c->a[4], (off_t) c->a[5]);
        release_unused(p->box, &before);
        return r;
    }
    return map_shared(c, c->a[0], c->a[1], (int) c->a[2], flags, (int) c->a[4],
        (off_t) c->a[5]);
}

long
n_munmap(struct Call *c)
{
    struct LFILinuxProc *p = c->target->proc;
    if (!p->has_shared)
        return sys_munmap(c->t, c->a[0], c->a[1]);
    size_t ps = lfi_opts(p->engine->engine).pagesize;
    struct Scan before;
    collect(p->box, c->a[0], c->a[0] + ceilp(c->a[1], ps), &before);
    long r = sys_munmap(c->t, c->a[0], c->a[1]);
    if (r == 0)
        release_unused(p->box, &before);
    return r;
}

long
n_mprotect(struct Call *c)
{
    struct LFILinuxProc *p = c->target->proc;
    if (p->has_shared && ((int) c->a[2] & PROT_EXEC)) {
        size_t ps = lfi_opts(p->engine->engine).pagesize;
        struct Scan sc;
        collect(p->box, c->a[0], c->a[0] + ceilp(c->a[1], ps), &sc);
        if (sc.set.n)
            return -EACCES; // shared memory is never executable
    }
    return sys_mprotect(c->t, c->a[0], c->a[1], (int) c->a[2]);
}
