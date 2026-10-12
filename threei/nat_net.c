#define _GNU_SOURCE
// Sockets and readiness notification (select, poll family, epoll).
//
// Sockets are ordinary entries of the cage's descriptor table, so they are
// inherited by fork, duplicated by dup, closed on exec and usable from a grate
// acting on behalf of the cage exactly like files. Which cages may use the
// network at all is a policy decision: the default is controlled by the
// engine's `net` option, and a grate can interpose on the socket calls to be
// more selective.
#include "cage.h"
#include "natutil.h"

#include "fd.h"
#include "lock.h"
#include "path.h"
#include "sys/sys.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static long
net_allowed(struct Call *c, int domain)
{
    if (domain == AF_UNIX)
        return 0;
    if (!g_eng->opts.net)
        return -EACCES;
    return 0;
}

// ---------------------------------------------------------------------------
// Socket addresses
// ---------------------------------------------------------------------------

// Copies the socket address in argument ai (length in argument li) into
// 'out'. UNIX socket paths are resolved through the cage's file system view,
// so a cage cannot reach host sockets outside its directory mappings.
static long
sockaddr_copy(struct Call *c, int cage_arg, uint64_t ptr, socklen_t len,
    struct sockaddr_storage *out, socklen_t *outlen)
{
    if (len < sizeof(sa_family_t) || len > sizeof(*out))
        return -EINVAL;
    void *g = call_buf_at(c, cage_arg, ptr, len, 1);
    if (!g)
        return -EFAULT;
    memset(out, 0, sizeof(*out));
    memcpy(out, g, len);
    *outlen = len;
    if (out->ss_family == AF_UNIX) {
        struct sockaddr_un *un = (struct sockaddr_un *) out;
        size_t plen = len > offsetof(struct sockaddr_un, sun_path) ?
            len - offsetof(struct sockaddr_un, sun_path) : 0;
        if (plen == 0)
            return 0; // unnamed
        if (un->sun_path[0] == 0)
            return -EACCES; // the abstract namespace is shared with the host
        char path[sizeof(un->sun_path) + 1];
        size_t n = strnlen(un->sun_path, plen);
        memcpy(path, un->sun_path, n);
        path[n] = 0;
        char host[sizeof(un->sun_path)];
        struct LFILinuxProc *pr = c->target->proc;
        bool ok;
        {
            LOCK_WITH_DEFER(&pr->cwd.lk, lk);
            ok = path_resolve(pr, path, host, sizeof(host));
        }
        if (!ok)
            return -ENOENT;
        size_t hl = strlen(host);
        if (hl >= sizeof(un->sun_path))
            return -ENAMETOOLONG; // never bind/connect to a truncated path
        memset(un->sun_path, 0, sizeof(un->sun_path));
        memcpy(un->sun_path, host, hl);
        *outlen = offsetof(struct sockaddr_un, sun_path) + hl + 1;
    }
    return 0;
}

static long
sockaddr_in_(struct Call *c, int ai, socklen_t len, struct sockaddr_storage *out,
    socklen_t *outlen)
{
    return sockaddr_copy(c, ai, c->a[ai], len, out, outlen);
}

// Writes an address returned by the kernel back to the guest: the address
// buffer (argument ai) receives at most the length found in *argument li, and
// that length is updated with the full size.
static long
sockaddr_out(struct Call *c, int ai, int li, const struct sockaddr_storage *ss,
    socklen_t len)
{
    if (c->a[ai] == 0)
        return 0;
    socklen_t *lp = call_buf(c, li, sizeof(socklen_t), alignof(socklen_t));
    if (!lp)
        return -EFAULT;
    socklen_t cap = *lp;
    socklen_t n = len < cap ? len : cap;
    if (n) {
        void *g = call_buf(c, ai, n, 1);
        if (!g)
            return -EFAULT;
        memcpy(g, ss, n);
    }
    *lp = len;
    return 0;
}

// ---------------------------------------------------------------------------
// Socket calls
// ---------------------------------------------------------------------------

#define SOCK_TYPE_MASK 0xf
#define SOCK_FLAGS     (SOCK_NONBLOCK | SOCK_CLOEXEC)

NATIVE(n_socket)
{
    int domain = (int) c->a[0], type = (int) c->a[1], proto = (int) c->a[2];
    if (domain != AF_UNIX && domain != AF_INET && domain != AF_INET6)
        return -EAFNOSUPPORT;
    long r = net_allowed(c, domain);
    if (r)
        return r;
    int hfd = socket(domain, type | SOCK_CLOEXEC, proto);
    if (hfd < 0)
        return -errno;
    int fd = fdalloc(&c->target->proc->fdtable, hfd, NULL,
        (type & SOCK_CLOEXEC) ? LINUX_O_CLOEXEC : 0);
    if (fd < 0)
        close(hfd);
    return fd;
}

NATIVE(n_socketpair)
{
    int domain = (int) c->a[0], type = (int) c->a[1], proto = (int) c->a[2];
    if (domain != AF_UNIX)
        return -EAFNOSUPPORT;
    int *out = call_buf(c, 3, 2 * sizeof(int), alignof(int));
    if (!out)
        return -EFAULT;
    int sv[2];
    if (socketpair(domain, type | SOCK_CLOEXEC, proto, sv) < 0)
        return -errno;
    struct FDTable *t = &c->target->proc->fdtable;
    int fl = (type & SOCK_CLOEXEC) ? LINUX_O_CLOEXEC : 0;
    int a = fdalloc(t, sv[0], NULL, fl);
    if (a < 0) {
        close(sv[0]);
        close(sv[1]);
        return a;
    }
    int b = fdalloc(t, sv[1], NULL, fl);
    if (b < 0) {
        fdclose(t, a);
        close(sv[1]);
        return b;
    }
    out[0] = a;
    out[1] = b;
    return 0;
}

NATIVE(n_bind)
{
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    struct sockaddr_storage ss;
    socklen_t len;
    long r = sockaddr_in_(c, 1, (socklen_t) c->a[2], &ss, &len);
    if (!r)
        r = sysret(bind(f.hfd, (struct sockaddr *) &ss, len));
    fd_put(&f);
    return r;
}

NATIVE(n_connect)
{
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    struct sockaddr_storage ss;
    socklen_t len;
    long r = sockaddr_in_(c, 1, (socklen_t) c->a[2], &ss, &len);
    if (!r && ss.ss_family != AF_UNIX && !g_eng->opts.net)
        r = -EACCES;
    if (!r)
        r = sysret(connect(f.hfd, (struct sockaddr *) &ss, len));
    fd_put(&f);
    return r;
}

NATIVE(n_listen)
{
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    long r = sysret(listen(f.hfd, (int) c->a[1]));
    fd_put(&f);
    return r;
}

static long
do_accept(struct Call *c, int flags)
{
    if (flags & ~SOCK_FLAGS)
        return -EINVAL;
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    long r = sysret(accept4(f.hfd, (struct sockaddr *) &ss, &len,
        flags | SOCK_CLOEXEC));
    if (r >= 0) {
        int hfd = (int) r;
        long w = sockaddr_out(c, 1, 2, &ss, len);
        r = w ? w : fdalloc(&c->target->proc->fdtable, hfd, NULL,
                        (flags & SOCK_CLOEXEC) ? LINUX_O_CLOEXEC : 0);
        if (r < 0)
            close(hfd);
    }
    fd_put(&f);
    return r;
}

NATIVE(n_accept)
{
    return do_accept(c, 0);
}

NATIVE(n_accept4)
{
    return do_accept(c, (int) c->a[3]);
}

NATIVE(n_getsockname)
{
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    long r = sysret(c->nr == 51 ? getsockname(f.hfd, (struct sockaddr *) &ss, &len) :
                                  getpeername(f.hfd, (struct sockaddr *) &ss, &len));
    if (!r)
        r = sockaddr_out(c, 1, 2, &ss, len);
    fd_put(&f);
    return r;
}

NATIVE(n_shutdown)
{
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    long r = sysret(shutdown(f.hfd, (int) c->a[1]));
    fd_put(&f);
    return r;
}

#define MSG_ALLOWED                                                          \
    (MSG_OOB | MSG_PEEK | MSG_DONTWAIT | MSG_WAITALL | MSG_NOSIGNAL |        \
        MSG_TRUNC | MSG_MORE | MSG_EOR | MSG_CTRUNC)

NATIVE(n_sendto)
{
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    size_t n = c->a[2];
    long r;
    void *buf = n ? call_buf(c, 1, n, 1) : (void *) &r;
    struct sockaddr_storage ss;
    socklen_t alen = 0;
    bool have_addr = c->a[4] != 0;
    r = 0;
    if (!buf)
        r = -EFAULT;
    else if (have_addr)
        r = sockaddr_in_(c, 4, (socklen_t) c->a[5], &ss, &alen);
    if (!r && have_addr && ss.ss_family != AF_UNIX && !g_eng->opts.net)
        r = -EACCES;
    if (!r) {
        r = sysret(sendto(f.hfd, buf, n, ((int) c->a[3] & MSG_ALLOWED) | MSG_NOSIGNAL,
            have_addr ? (struct sockaddr *) &ss : NULL, alen));
        if (!((int) c->a[3] & MSG_NOSIGNAL))
            r = pipe_signal(c, r);
    }
    fd_put(&f);
    return r;
}

NATIVE(n_recvfrom)
{
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    size_t n = c->a[2];
    long r = 0;
    void *buf = n ? call_buf(c, 1, n, 1) : (void *) &r;
    if (!buf) {
        fd_put(&f);
        return -EFAULT;
    }
    struct sockaddr_storage ss;
    socklen_t alen = sizeof(ss);
    bool want_addr = c->a[4] != 0;
    r = sysret(recvfrom(f.hfd, buf, n, (int) c->a[3] & MSG_ALLOWED,
        want_addr ? (struct sockaddr *) &ss : NULL, want_addr ? &alen : NULL));
    if (r >= 0 && want_addr) {
        long w = sockaddr_out(c, 4, 5, &ss, alen);
        if (w)
            r = w;
    }
    fd_put(&f);
    return r;
}

// struct msghdr as the x86-64 kernel sees it.
struct GMsg {
    uint64_t name;
    uint32_t namelen;
    uint32_t _pad0;
    uint64_t iov;
    uint64_t iovlen;
    uint64_t control;
    uint64_t controllen;
    int32_t flags;
    int32_t _pad1;
};

#define MSG_IOV_MAX 64

static long
do_msg(struct Call *c, bool send)
{
    struct GMsg *gm = call_buf(c, 1, sizeof(*gm), 8);
    if (!gm)
        return -EFAULT;
    struct GMsg m = *gm; // snapshot
    // Ancillary data can carry descriptors, which would have to be translated
    // between the cage's table and the host's; it is not supported.
    if (m.controllen != 0 || m.control != 0)
        return -EOPNOTSUPP;
    if (m.iovlen > MSG_IOV_MAX)
        return -EINVAL;
    struct LFIBox *box = call_box(c, 1);
    struct iovec iov[MSG_IOV_MAX];
    if (m.iovlen) {
        struct { uint64_t base, len; } *g;
        if (!lfi_box_bufvalid(box, m.iov, m.iovlen * 16) || (m.iov & 7))
            return -EFAULT;
        g = (void *) lfi_box_l2p(box, m.iov);
        uint64_t total = 0;
        for (uint64_t i = 0; i < m.iovlen; i++) {
            uint64_t base = g[i].base, len = g[i].len;
            total += len;
            if (len > 0x7ffff000UL || total > 0x7ffff000UL)
                return -EINVAL;
            if (len && !lfi_box_bufvalid(box, base, len))
                return -EFAULT;
            iov[i].iov_base = len ? (void *) lfi_box_l2p(box, base) : NULL;
            iov[i].iov_len = len;
        }
    }
    struct sockaddr_storage ss;
    socklen_t alen = 0;
    struct msghdr hm;
    memset(&hm, 0, sizeof(hm));
    hm.msg_iov = iov;
    hm.msg_iovlen = m.iovlen;
    long r = 0;
    if (send) {
        if (m.name) {
            r = sockaddr_copy(c, 1, m.name, m.namelen, &ss, &alen);
            if (!r && ss.ss_family != AF_UNIX && !g_eng->opts.net)
                r = -EACCES;
            hm.msg_name = &ss;
            hm.msg_namelen = alen;
        }
    } else if (m.name) {
        hm.msg_name = &ss;
        hm.msg_namelen = sizeof(ss);
    }
    if (r)
        return r;
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    int flags = (int) c->a[2] & MSG_ALLOWED;
    if (send)
        r = sysret(sendmsg(f.hfd, &hm, flags | MSG_NOSIGNAL));
    else
        r = sysret(recvmsg(f.hfd, &hm, flags));
    fd_put(&f);
    if (r >= 0 && !send) {
        gm->flags = hm.msg_flags;
        gm->controllen = 0;
        if (m.name) {
            socklen_t cap = m.namelen < hm.msg_namelen ? m.namelen : hm.msg_namelen;
            void *g = cap ? call_buf_at(c, 1, m.name, cap, 1) : NULL;
            if (g)
                memcpy(g, &ss, cap);
            gm->namelen = hm.msg_namelen;
        }
    }
    return r;
}

NATIVE(n_sendmsg)
{
    return do_msg(c, true);
}

NATIVE(n_recvmsg)
{
    return do_msg(c, false);
}

NATIVE(n_setsockopt)
{
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    uint64_t len = c->a[4];
    long r;
    void *v = len <= 4096 ? call_buf(c, 3, len ? len : 1, 1) : NULL;
    r = v ? sysret(setsockopt(f.hfd, (int) c->a[1], (int) c->a[2], v, (socklen_t) len)) :
            (len > 4096 ? -EINVAL : -EFAULT);
    fd_put(&f);
    return r;
}

NATIVE(n_getsockopt)
{
    struct FDR f;
    if (fd_get(c, 0, &f))
        return -EBADF;
    socklen_t *lp = call_buf(c, 4, sizeof(socklen_t), alignof(socklen_t));
    long r;
    if (!lp) {
        r = -EFAULT;
    } else if (*lp > 4096) {
        r = -EINVAL;
    } else {
        void *v = call_buf(c, 3, *lp ? *lp : 1, 1);
        r = v ? sysret(getsockopt(f.hfd, (int) c->a[1], (int) c->a[2], v, lp)) :
                -EFAULT;
    }
    fd_put(&f);
    return r;
}

// ---------------------------------------------------------------------------
// select / ppoll / epoll
// ---------------------------------------------------------------------------

#define FD_SETBYTES(n) (((n) + 63) / 64 * 8)

// select(2) implemented on top of poll(2): the sandbox's descriptors are
// translated to host descriptors, which can be numbered above FD_SETSIZE.
static long
do_select(struct Call *c, int timeout_ms)
{
    int64_t nfds = (int64_t) c->a[0];
    if (nfds < 0 || nfds > LINUX_NOFILE)
        return -EINVAL;
    struct FDTable *t = call_target_fdt(c);
    if (!t)
        return -EBADF;
    size_t nb = FD_SETBYTES(nfds);
    uint8_t *sets[3] = { NULL, NULL, NULL };
    for (int k = 0; k < 3; k++) {
        if (c->a[1 + k]) {
            sets[k] = call_buf(c, 1 + k, nb ? nb : 1, 8);
            if (!sets[k])
                return -EFAULT;
        }
    }
    struct pollfd pfd[LINUX_NOFILE];
    int held[LINUX_NOFILE], vfd[LINUX_NOFILE];
    int n = 0;
    long r = 0;
    for (int fd = 0; fd < nfds; fd++) {
        short ev = 0;
        if (sets[0] && (sets[0][fd / 8] & (1 << (fd % 8))))
            ev |= POLLIN;
        if (sets[1] && (sets[1][fd / 8] & (1 << (fd % 8))))
            ev |= POLLOUT;
        if (sets[2] && (sets[2][fd / 8] & (1 << (fd % 8))))
            ev |= POLLPRI;
        if (!ev)
            continue;
        int h = fdacquire(t, fd, NULL, NULL);
        if (h < 0) {
            r = -EBADF;
            goto out;
        }
        held[n] = h;
        vfd[n] = fd;
        pfd[n].fd = h;
        pfd[n].events = ev;
        pfd[n].revents = 0;
        n++;
    }
    {
        long pr = sysret(poll(pfd, n, timeout_ms));
        if (pr < 0) {
            r = pr;
        } else {
            for (int k = 0; k < 3; k++)
                if (sets[k])
                    memset(sets[k], 0, nb);
            long count = 0;
            for (int i = 0; i < n; i++) {
                int fd = vfd[i];
                short re = pfd[i].revents;
                if (sets[0] && (re & (POLLIN | POLLHUP | POLLERR))) {
                    sets[0][fd / 8] |= 1 << (fd % 8);
                    count++;
                }
                if (sets[1] && (re & (POLLOUT | POLLERR))) {
                    sets[1][fd / 8] |= 1 << (fd % 8);
                    count++;
                }
                if (sets[2] && (re & POLLPRI)) {
                    sets[2][fd / 8] |= 1 << (fd % 8);
                    count++;
                }
            }
            r = count;
        }
    }
out:
    for (int i = 0; i < n; i++)
        fdrelease(t, held[i]);
    return r;
}

static int
ms_of(uint64_t sec, uint64_t nsec)
{
    if (sec > 1000000)
        return -1; // effectively forever
    return (int) (sec * 1000 + (nsec + 999999) / 1000000);
}

NATIVE(n_select)
{
    int ms = -1;
    if (c->a[4]) {
        struct { int64_t sec, usec; } *tv = call_buf(c, 4, 16, 8);
        if (!tv)
            return -EFAULT;
        if (tv->sec < 0 || tv->usec < 0)
            return -EINVAL;
        ms = ms_of(tv->sec, tv->usec * 1000);
    }
    return do_select(c, ms);
}

NATIVE(n_pselect6)
{
    int ms = -1;
    if (c->a[4]) {
        struct { int64_t sec, nsec; } *ts = call_buf(c, 4, 16, 8);
        if (!ts)
            return -EFAULT;
        if (ts->sec < 0 || ts->nsec < 0)
            return -EINVAL;
        ms = ms_of(ts->sec, ts->nsec);
    }
    return do_select(c, ms);
}

NATIVE(n_ppoll)
{
    // ppoll(fds, nfds, timespec*, sigmask*, sigsetsize): the mask is ignored.
    uint64_t nfds = c->a[1];
    if (nfds > LINUX_NOFILE)
        return -EINVAL;
    int ms = -1;
    if (c->a[2]) {
        struct { int64_t sec, nsec; } *ts = call_buf(c, 2, 16, 8);
        if (!ts)
            return -EFAULT;
        if (ts->sec < 0 || ts->nsec < 0)
            return -EINVAL;
        ms = ms_of(ts->sec, ts->nsec);
    }
    struct pollfd *gp = nfds ? call_buf(c, 0, nfds * sizeof(struct pollfd), 4) :
                               NULL;
    if (nfds && !gp)
        return -EFAULT;
    struct FDTable *t = call_target_fdt(c);
    if (!t)
        return -EBADF;
    struct pollfd hp[LINUX_NOFILE];
    int held[LINUX_NOFILE];
    for (uint64_t i = 0; i < nfds; i++) {
        hp[i].events = gp[i].events;
        hp[i].revents = 0;
        held[i] = gp[i].fd >= 0 ? fdacquire(t, gp[i].fd, NULL, NULL) : -1;
        hp[i].fd = held[i] >= 0 ? held[i] : -1;
    }
    long r = sysret(poll(hp, nfds, ms));
    if (r >= 0) {
        long ready = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            gp[i].revents = (gp[i].fd >= 0 && held[i] < 0) ? POLLNVAL :
                                                              hp[i].revents;
            if (gp[i].revents)
                ready++;
        }
        r = ready;
    }
    for (uint64_t i = 0; i < nfds; i++)
        if (held[i] >= 0)
            fdrelease(t, held[i]);
    return r;
}

NATIVE(n_epoll_create)
{
    int flags = c->nr == 291 ? (int) c->a[0] : 0;
    if (c->nr == 213 && (int) c->a[0] <= 0)
        return -EINVAL;
    if (flags & ~EPOLL_CLOEXEC)
        return -EINVAL;
    int hfd = epoll_create1(EPOLL_CLOEXEC);
    if (hfd < 0)
        return -errno;
    int fd = fdalloc(&c->target->proc->fdtable, hfd, NULL,
        (flags & EPOLL_CLOEXEC) ? LINUX_O_CLOEXEC : 0);
    if (fd < 0)
        close(hfd);
    return fd;
}

NATIVE(n_epoll_ctl)
{
    struct FDR ep, f;
    if (fd_get(c, 0, &ep))
        return -EBADF;
    if (fd_get(c, 2, &f)) {
        fd_put(&ep);
        return -EBADF;
    }
    int op = (int) c->a[1];
    struct epoll_event ev;
    long r = 0;
    if (op != EPOLL_CTL_DEL) {
        void *g = call_buf(c, 3, sizeof(ev), 1);
        if (!g)
            r = -EFAULT;
        else
            memcpy(&ev, g, sizeof(ev));
    }
    if (!r)
        r = sysret(epoll_ctl(ep.hfd, op, f.hfd, op == EPOLL_CTL_DEL ? NULL : &ev));
    fd_put(&f);
    fd_put(&ep);
    return r;
}

NATIVE(n_epoll_wait)
{
    struct FDR ep;
    if (fd_get(c, 0, &ep))
        return -EBADF;
    int max = (int) c->a[2];
    long r;
    if (max <= 0 || max > 4096) {
        r = -EINVAL;
    } else {
        void *ev = call_buf(c, 1, (size_t) max * sizeof(struct epoll_event), 1);
        r = ev ? sysret(epoll_wait(ep.hfd, ev, max, (int) c->a[3])) : -EFAULT;
    }
    fd_put(&ep);
    return r;
}

NATIVE(n_eventfd)
{
    int flags = c->nr == 290 ? (int) c->a[1] : 0;
    if (flags & ~(EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE))
        return -EINVAL;
    int hfd = eventfd((unsigned) c->a[0], (flags & ~EFD_CLOEXEC) | EFD_CLOEXEC);
    if (hfd < 0)
        return -errno;
    int fd = fdalloc(&c->target->proc->fdtable, hfd, NULL,
        (flags & EFD_CLOEXEC) ? LINUX_O_CLOEXEC : 0);
    if (fd < 0)
        close(hfd);
    return fd;
}

NATIVE(n_memfd_create)
{
    char *name = call_path(c, 0);
    if (!name)
        return -EFAULT;
    unsigned flags = (unsigned) c->a[1];
    if (flags & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING))
        return -EINVAL;
    int hfd = memfd_create(name, flags | MFD_CLOEXEC);
    free(name);
    if (hfd < 0)
        return -errno;
    int fd = fdalloc(&c->target->proc->fdtable, hfd, NULL,
        (flags & MFD_CLOEXEC) ? LINUX_O_CLOEXEC : 0);
    if (fd < 0)
        close(hfd);
    return fd;
}

// Returns true if this file handles the call.
bool
nat_net(struct Call *c, long *ret)
{
    switch (c->nr) {
    case 41:  *ret = n_socket(c); return true;
    case 42:  *ret = n_connect(c); return true;
    case 43:  *ret = n_accept(c); return true;
    case 44:  *ret = n_sendto(c); return true;
    case 45:  *ret = n_recvfrom(c); return true;
    case 46:  *ret = n_sendmsg(c); return true;
    case 47:  *ret = n_recvmsg(c); return true;
    case 48:  *ret = n_shutdown(c); return true;
    case 49:  *ret = n_bind(c); return true;
    case 50:  *ret = n_listen(c); return true;
    case 51:  /* getsockname */
    case 52:  *ret = n_getsockname(c); return true;
    case 53:  *ret = n_socketpair(c); return true;
    case 54:  *ret = n_setsockopt(c); return true;
    case 55:  *ret = n_getsockopt(c); return true;
    case 288: *ret = n_accept4(c); return true;
    case 23:  *ret = n_select(c); return true;
    case 270: *ret = n_pselect6(c); return true;
    case 271: *ret = n_ppoll(c); return true;
    case 213: /* epoll_create */
    case 291: *ret = n_epoll_create(c); return true;
    case 233: *ret = n_epoll_ctl(c); return true;
    case 232: /* epoll_wait */
    case 281: /* epoll_pwait */
        *ret = n_epoll_wait(c); return true;
    case 284: /* eventfd */
    case 290: *ret = n_eventfd(c); return true;
    case 319: *ret = n_memfd_create(c); return true;
    default:  return false;
    }
}
