#define _GNU_SOURCE
// The 3i API calls: register_handler, copy_handler_table_to_cage and
// copy_data_between_cages. Payload slots follow Lind's layout so grates that
// interpose on these calls (and forward them) work unchanged:
//
//   register_handler:   arg1 = target cage, arg1cage = call number,
//                       arg2cage = grate cage, arg3 = handler address
//   copy_handler_table: arg1 = source cage, arg1cage = target cage
//   copy_data:          arg1 = src addr, arg1cage = src cage,
//                       arg2 = dest addr, arg2cage = dest cage,
//                       arg3 = length, arg4 = copy type
#include "cage.h"

#include <errno.h>
#include <string.h>

static long
abort_code(void)
{
    return (long) LFI3I_ELINDAPIABORTED;
}

static long
srch_code(void)
{
    return (long) LFI3I_ELINDESRCH;
}

static bool
related(struct Cage *caller, struct Cage *x)
{
    // The caller itself, or a cage the caller created (directly or not).
    return x == caller || x->id == caller->id || cage_is_descendant(x, caller);
}

// When a grate forwards, unchanged, the very call it is serving, the
// forwarded operation must not exceed what the cage that made the call could
// have done itself: otherwise a confined cage could borrow the grate's wider
// authority by routing a routing operation through it. Returns that
// originating cage (the outermost cage of the current run of frames serving
// this same call), or NULL if this is not a pass-through. A grate that makes
// a different registration while it happens to be serving one (a clamp
// allocating routes, say) acts with its own authority.
static bool
same_call(const struct Frame *f, const struct Call *c)
{
    if (f->nr != c->nr)
        return false;
    for (int i = 0; i < 6; i++)
        if (f->a[i] != c->a[i] || (f->ac[i] & LFI3I_CAGEID_MASK) != c->ac[i])
            return false;
    return true;
}

static struct Cage *
forward_origin(const struct Call *c)
{
    struct Frame *f = tl_frames;
    if (!f || f->grate != c->caller)
        return NULL;
    struct Cage *origin = NULL;
    for (; f && same_call(f, c); f = f->prev)
        origin = f->caller;
    return origin != c->caller ? origin : NULL;
}

long
n3i_register_handler(struct Call *c)
{
    struct Cage *caller = c->caller;
    struct Cage *origin = forward_origin(c);
    uint64_t target_id = c->a[0];
    uint64_t callnum = c->ac[0];
    uint64_t grate_id = c->ac[1];
    uint64_t fn = c->a[2];

    if (target_id == 0 || target_id > 0xffffffffu)
        return abort_code();
    int idx = ht_index(callnum);
    if (idx < 0)
        return abort_code();

    struct Cage *target = cage_get((uint32_t) target_id);
    if (!target)
        return srch_code();

    long ret;
    if (atomic_load(&target->state) != CAGE_RUNNING) {
        ret = srch_code();
        goto out_target;
    }

    // A cage may redirect its own calls, or those of cages it created.
    if (!g_eng->opts.permissive &&
        (!related(caller, target) || (origin && !related(origin, target)))) {
        ret = -EPERM;
        goto out_target;
    }

    if (grate_id == LFI3I_DEREGISTER) {
        atomic_store_explicit(&target->htab[idx], 0, memory_order_release);
        ret = 0;
        goto out_target;
    }

    struct Cage *grate = grate_id <= 0xffffffffu ? cage_get((uint32_t) grate_id) :
                                                   NULL;
    if (!grate) {
        ret = srch_code();
        goto out_target;
    }
    if (atomic_load(&grate->state) != CAGE_RUNNING) {
        ret = srch_code();
        goto out_grate;
    }
    // The handler must live in a cage that agreed to serve: the registrar
    // itself, one of its ancestors (a child registering the grate that
    // created it) or one of its descendants. Otherwise a cage could invoke
    // arbitrary entry points of unrelated cages.
    if (!g_eng->opts.permissive &&
        ((!related(caller, grate) && !cage_is_descendant(caller, grate)) ||
            (origin && !related(origin, grate) &&
                !cage_is_descendant(origin, grate)))) {
        ret = -EPERM;
        goto out_grate;
    }

    // The handler address is a pointer into the grate's sandbox; only its
    // offset is kept (the registrar may be a fork of the grate whose copy of
    // the address space has a different base).
    struct LFIBox *gbox = grate->proc ? grate->proc->box : NULL;
    uint32_t off = (uint32_t) fn;
    if (!gbox) {
        ret = srch_code();
        goto out_grate;
    }
    struct LFIBoxInfo bi = lfi_box_info(gbox);
    if (off == 0 || bi.base + off < bi.min || bi.base + off >= bi.max_exec ||
        (off & 31) != 0) {
        ret = -EINVAL;
        goto out_grate;
    }
    atomic_store_explicit(&grate->is_grate, true, memory_order_release);
    if (idx >= HT_FIXED_N) {
        // Make the slot part of the table's used range before it is set.
        uint32_t need = (uint32_t) (idx - HT_FIXED_N + 1);
        uint32_t hw = atomic_load_explicit(&target->alt_hw, memory_order_relaxed);
        while (hw < need && !atomic_compare_exchange_weak_explicit(
                   &target->alt_hw, &hw, need, memory_order_acq_rel,
                   memory_order_relaxed))
            ;
    }
    atomic_store_explicit(&target->htab[idx], ht_make(grate->id, off),
        memory_order_release);
    // The grate may have started exiting (and scrubbing handler tables)
    // before the entry above became visible: don't leave it dangling.
    if (atomic_load_explicit(&grate->state, memory_order_acquire) !=
        CAGE_RUNNING) {
        atomic_store_explicit(&target->htab[idx], HT_DEAD,
            memory_order_release);
        ret = srch_code();
        goto out_grate;
    }
    ret = 0;
out_grate:
    cage_put(grate);
out_target:
    cage_put(target);
    return ret;
}

long
n3i_copy_htab(struct Call *c)
{
    struct Cage *caller = c->caller;
    struct Cage *origin = forward_origin(c);
    uint64_t src_id = c->a[0];
    uint64_t dst_id = c->ac[0];
    if (src_id > 0xffffffffu || dst_id > 0xffffffffu)
        return abort_code();
    struct Cage *src = cage_get((uint32_t) src_id);
    struct Cage *dst = cage_get((uint32_t) dst_id);
    long ret = 0;
    if (!src || !dst) {
        ret = srch_code();
    } else if (atomic_load(&src->state) != CAGE_RUNNING ||
        atomic_load(&dst->state) != CAGE_RUNNING) {
        ret = srch_code();
    } else if (!g_eng->opts.permissive &&
        (!related(caller, dst) ||
            !(related(caller, src) || cage_is_descendant(caller, src)) ||
            (origin && (!related(origin, dst) ||
                           !(related(origin, src) ||
                               cage_is_descendant(origin, src)))))) {
        ret = -EPERM;
    } else if (src != dst) {
        size_t n = ht_limit(src), dn = ht_limit(dst);
        if (src->alt_hw > dst->alt_hw)
            atomic_store_explicit(&dst->alt_hw, src->alt_hw, memory_order_release);
        for (size_t i = 0; i < (n > dn ? n : dn); i++)
            atomic_store_explicit(&dst->htab[i],
                i < n ? atomic_load_explicit(&src->htab[i], memory_order_acquire) : 0,
                memory_order_release);
    }
    if (src)
        cage_put(src);
    if (dst)
        cage_put(dst);
    return ret;
}

long
n3i_copy_data(struct Call *c)
{
    struct Cage *caller = c->caller;
    uint64_t srcaddr = c->a[0], srccage = c->ac[0];
    uint64_t dstaddr = c->a[1], dstcage = c->ac[1];
    uint64_t len = c->a[2];
    uint64_t type = c->a[3];

    if (srccage == dstcage)
        return abort_code(); // cross-cage transfers only
    if (len > 0xffffffffu || dstaddr == 0 || srccage > 0xffffffffu ||
        dstcage > 0xffffffffu)
        return abort_code();

    struct Cage *src = cage_get((uint32_t) srccage);
    struct Cage *dst = cage_get((uint32_t) dstcage);
    long ret = abort_code();
    if (!src || !dst) {
        goto out;
    }
    if (!g_eng->opts.permissive &&
        (!cage_authorized_for(caller, src) || !cage_authorized_for(caller, dst))) {
        goto out;
    }

    // Lock the address spaces against teardown for the duration of the copy,
    // always in cage id order.
    struct Cage *first = src->id < dst->id ? src : dst;
    struct Cage *second = src->id < dst->id ? dst : src;
    pthread_rwlock_rdlock(&first->memlk);
    pthread_rwlock_rdlock(&second->memlk);

    struct LFIBox *sbox = src->proc ? src->proc->box : NULL;
    struct LFIBox *dbox = dst->proc ? dst->proc->box : NULL;
    if (!sbox || !dbox ||
        atomic_load(&src->state) == CAGE_ZOMBIE ||
        atomic_load(&dst->state) == CAGE_ZOMBIE) {
        goto unlock;
    }

    size_t n;
    if (type == LFI3I_COPY_MEMCPY) {
        n = len;
    } else if (type == LFI3I_COPY_STRNCPY) {
        if (srcaddr == 0)
            goto unlock;
        size_t sl = lfi_box_strnlen(sbox, srcaddr, len);
        if (sl == SIZE_MAX)
            goto unlock; // no terminator within the limit
        n = sl + 1;
    } else {
        goto unlock;
    }
    if (n == 0) {
        ret = (long) dstaddr;
        goto unlock;
    }
    if (srcaddr == 0 || !lfi_box_bufvalid(sbox, srcaddr, n) ||
        !lfi_box_bufvalid(dbox, dstaddr, n))
        goto unlock;
    // The boxes are distinct, so the ranges cannot overlap. A cage whose
    // other threads unmap the range mid-copy must not take the runtime down:
    // a fault returns here.
    //
    // Grates written for Lind copy a fixed-size buffer (a path buffer, say)
    // from a pointer into a cage's memory, which there is always readable:
    // WebAssembly memory is contiguous. Here the bytes after a string may be
    // on an unmapped page. So a memcpy that runs into unreadable memory after
    // at least one byte is not an error: what could be read is copied and the
    // rest of the destination is zeroed.
    {
        sigjmp_buf jb;
        volatile size_t done = 0;
        char *d = (char *) lfi_box_l2p(dbox, dstaddr);
        char *sp = (char *) lfi_box_l2p(sbox, srcaddr);
        if (sigsetjmp(jb, 1) == 0) {
            tl_copy_jb = &jb;
            while (done < n) {
                // Copy to the end of the source page, then page by page.
                size_t chunk = 4096 - ((srcaddr + done) & 4095);
                if (chunk > n - done)
                    chunk = n - done;
                memcpy(d + done, sp + done, chunk);
                done += chunk;
            }
            tl_copy_jb = NULL;
            ret = (long) dstaddr;
        } else {
            tl_copy_jb = NULL;
            if (type == LFI3I_COPY_MEMCPY && done > 0) {
                memset(d + done, 0, n - done);
                ret = (long) dstaddr;
            } else {
                ret = abort_code();
            }
        }
    }
unlock:
    pthread_rwlock_unlock(&second->memlk);
    pthread_rwlock_unlock(&first->memlk);
out:
    if (src)
        cage_put(src);
    if (dst)
        cage_put(dst);
    return ret;
}
