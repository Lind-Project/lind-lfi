// Helpers shared by the default system call implementations.
#pragma once

#include "cage.h"
#include "fd.h"

#include <errno.h>
#include <signal.h>

#define NATIVE(name) static long name(struct Call *c)

// Converts the result of a libc call that returns -1 and sets errno into a
// system call result.
static inline long
sysret(long r)
{
    return r < 0 ? -(long) errno : r;
}

// A file descriptor argument borrowed for the duration of one operation.
struct FDR {
    struct FDTable *t;
    int hfd;
    int flags;
    bool isdir;
};

// Borrows the host descriptor for the fd in argument i. Returns 0 or -EBADF.
static inline int
fd_get(struct Call *c, int i, struct FDR *f)
{
    f->t = call_fdt(c, i);
    f->hfd = -1;
    if (!f->t || (int64_t) c->a[i] != (int32_t) c->a[i])
        return -EBADF;
    f->hfd = fdacquire(f->t, (int) c->a[i], &f->flags, &f->isdir);
    return f->hfd < 0 ? -EBADF : 0;
}

static inline void
fd_put(struct FDR *f)
{
    if (f->hfd >= 0)
        fdrelease(f->t, f->hfd);
}

// Resolves path argument pi (relative to the directory descriptor argument di,
// or the working directory if di < 0) to a host path in the target's file
// system view. If abs_out is non-NULL it receives the normalized sandbox path.
long resolve_arg(struct Call *c, int di, int pi, char *host, size_t hsz,
    char **abs_out);

// A write to a pipe or socket nobody reads from fails with EPIPE and also
// raises SIGPIPE on the writing thread (default action: terminate).
static inline long
pipe_signal(struct Call *c, long r)
{
    if (r == -EPIPE) {
        struct LFILinuxThread *t =
            (c->t && c->t->ctx && c->caller == c->target) ? c->t : NULL;
        sig_send(c->target, t, SIGPIPE, (int) c->target->id);
    }
    return r;
}
