#define _GNU_SOURCE
// Access to system call arguments that belong to different cages.
#include "cage.h"

#include <stdlib.h>
#include <string.h>

void
call_init(struct Call *c, struct Cage *caller, struct Cage *target,
    struct LFILinuxThread *t, long nr, const uint64_t a[6],
    const uint64_t ac[6], bool self)
{
    c->caller = caller;
    c->target = target;
    c->t = t;
    c->nr = nr;
    c->self = self;
    for (int i = 0; i < 6; i++) {
        c->a[i] = a[i];
        c->ac[i] = ac[i] & LFI3I_CAGEID_MASK;
        c->xc[i] = NULL;
    }
}

void
call_done(struct Call *c)
{
    if (c->self)
        return;
    for (int i = 0; i < 6; i++) {
        if (c->xc[i])
            cage_put(c->xc[i]);
    }
}

// The cage whose address space / descriptor table argument i refers to.
struct Cage *
call_cage(struct Call *c, int i)
{
    if (c->self)
        return c->target;
    uint32_t id = (uint32_t) c->ac[i];
    if (c->ac[i] > 0xffffffffu)
        return NULL;
    if (id == c->target->id)
        return c->target;
    for (int j = 0; j <= i; j++) {
        if (c->xc[j] && c->xc[j]->id == id)
            return c->xc[j];
    }
    struct Cage *x = cage_get(id);
    c->xc[i] = x;
    return x;
}

static inline struct LFIBox *
cage_box(struct Cage *m)
{
    struct LFILinuxProc *p = m ? m->proc : NULL;
    return p ? p->box : NULL;
}

struct LFIBox *
call_box(struct Call *c, int i)
{
    return cage_box(call_cage(c, i));
}

// Returns a host pointer to [a[i], a[i]+len) in the argument's address space,
// or NULL if the range is not entirely inside the sandbox (or misaligned).
void *
call_buf(struct Call *c, int i, size_t len, size_t align)
{
    struct LFIBox *box = cage_box(call_cage(c, i));
    if (!box)
        return NULL;
    lfiptr p = c->a[i];
    if (!lfi_box_bufvalid(box, p, len) || (align > 1 && p % align != 0))
        return NULL;
    return (void *) lfi_box_l2p(box, p);
}

// Like call_buf, but for a pointer value that is not argument i itself: the
// address space is the one argument i belongs to. (Used for pointers nested in
// a structure, e.g. msghdr.msg_name.)
void *
call_buf_at(struct Call *c, int i, uint64_t ptr, size_t len, size_t align)
{
    struct LFIBox *box = cage_box(call_cage(c, i));
    if (!box || !lfi_box_bufvalid(box, ptr, len) ||
        (align > 1 && ptr % align != 0))
        return NULL;
    return (void *) lfi_box_l2p(box, ptr);
}

void *
call_buf_ro(struct Call *c, int i, size_t len)
{
    return call_buf(c, i, len, 1);
}

// Copies the NUL-terminated string argument i into a fresh host buffer
// (PATH_MAX at most). The caller frees it.
char *
call_path(struct Call *c, int i)
{
    struct LFIBox *box = cage_box(call_cage(c, i));
    if (!box)
        return NULL;
    size_t n = lfi_box_strnlen(box, c->a[i], FILENAME_MAX - 1);
    if (n == SIZE_MAX)
        return NULL;
    char *s = malloc(n + 1);
    if (!s)
        return NULL;
    memcpy(s, (void *) lfi_box_l2p(box, c->a[i]), n);
    s[n] = 0;
    return s;
}

struct FDTable *
call_fdt(struct Call *c, int i)
{
    struct Cage *m = call_cage(c, i);
    if (!m || !m->proc)
        return NULL;
    return &m->proc->fdtable;
}

struct FDTable *
call_target_fdt(struct Call *c)
{
    return c->target && c->target->proc ? &c->target->proc->fdtable : NULL;
}
