#define _GNU_SOURCE
// System call entry and 3i dispatch.
#include "cage.h"
#include "sysarity.h"

#include "sys/sys.h"

#include <signal.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define unlikely(x) __builtin_expect(!!(x), 0)
#define likely(x)   __builtin_expect(!!(x), 1)

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

// The outermost frame in which cage c issued the call being served (i.e. c's
// own thread is parked there), or NULL if c is not blocked in a grate call on
// this host thread.
struct Frame *
frame_serving(struct Cage *c)
{
    struct Frame *found = NULL;
    for (struct Frame *f = tl_frames; f; f = f->prev) {
        if (f->caller == c)
            found = f;
    }
    return found;
}

// True if cage 'other' is being served on this host thread.
static bool
frames_involve(struct Cage *other)
{
    for (struct Frame *f = tl_frames; f; f = f->prev) {
        if (f->caller == other || f->target == other)
            return true;
    }
    return false;
}

// Discards every frame from the innermost down to and including f (the host
// frames belonging to them are about to be thrown away by lfi_ctx_exit).
void
frames_unwind_through(struct Frame *f)
{
    while (tl_frames) {
        struct Frame *x = tl_frames;
        tl_frames = x->prev;
        frame_abort(x);
        if (x == f)
            break;
    }
}

// May 'caller' act on 'other' (read/write its memory, register handlers for
// it, copy its handler table)? A cage may act on itself, on cages it is
// currently serving as a grate, and on its own descendants.
bool
cage_authorized_for(struct Cage *caller, struct Cage *other)
{
    if (g_eng->opts.permissive)
        return true;
    if (caller == other || caller->id == other->id)
        return true;
    if (frames_involve(other))
        return true;
    return cage_is_descendant(other, caller);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

// Lind's C library and this one (musl) sometimes reach the same operation
// through different system calls, and grates register handlers for the Lind
// spelling. When the table has no handler for the call that was made but has
// one for the call Lind would have made, use that one (with its arguments):
//   fork (57) / vfork (58)  -> clone (56)       Lind implements fork as clone
//   epoll_pwait (281)       -> epoll_wait (232) musl's epoll_wait is epoll_pwait
//   newfstatat (262)        -> stat (4) / lstat (6)   musl's stat is fstatat
//   getdents64 (217)        -> getdents (78)  Lind's readdir uses the old call;
//                                             the caller gets getdents64 records
//                                             (converted after the call)
static inline bool
sys_alias(struct Cage *self, long *nr, uint64_t a[6])
{
    long n = *nr;
    if (n != 57 && n != 58 && n != 281 && n != 262 && n != 217)
        return false;
    if (atomic_load_explicit(&self->htab[n], memory_order_acquire) != 0)
        return false;
    switch (n) {
    case 57:
    case 58:
        if (atomic_load_explicit(&self->htab[56], memory_order_acquire) == 0)
            return false;
        a[0] = SIGCHLD;
        if (n == 58)
            a[0] |= LINUX_CLONE_VM | LINUX_CLONE_VFORK;
        for (int i = 1; i < 6; i++)
            a[i] = 0;
        *nr = 56;
        return true;
    case 217:
        if (atomic_load_explicit(&self->htab[78], memory_order_acquire) == 0)
            return false;
        *nr = 78;
        return true;
    case 281:
        // Only the plain form (no signal mask).
        if (a[4] != 0 ||
            atomic_load_explicit(&self->htab[232], memory_order_acquire) == 0)
            return false;
        a[4] = a[5] = 0;
        *nr = 232;
        return true;
    default: { // 262
        long to = (a[3] & 0x100) ? 6 : 4; // AT_SYMLINK_NOFOLLOW
        if ((int32_t) a[0] != -100 /* AT_FDCWD */ || (a[3] & ~0x100ULL) ||
            atomic_load_explicit(&self->htab[to], memory_order_acquire) == 0)
            return false;
        a[0] = a[1];
        a[1] = a[2];
        a[2] = a[3] = 0;
        *nr = to;
        return true;
    }
    }
}

// Routes a call through 'self's handler table.
long
threei_dispatch(struct Cage *self, long nr, struct Cage *target,
    struct LFILinuxThread *t, const uint64_t a[6], const uint64_t ac[6])
{
    uint64_t fa[6];
    bool gd64 = false;
    if (nr == 57 || nr == 58 || nr == 281 || nr == 262 || nr == 217) {
        memcpy(fa, a, sizeof(fa));
        long orig = nr;
        if (sys_alias(self, &nr, fa)) {
            a = fa;
            gd64 = orig == 217;
        }
    }
    int idx = ht_index((uint64_t) nr);
    if (idx < 0)
        return -ENOSYS;
    uint64_t e = atomic_load_explicit(&self->htab[idx], memory_order_acquire);
    if (e) {
        long gr = grate_call(self, t, target, (uint32_t) (e >> 32),
            (uint32_t) e, nr, a, ac);
        if (gd64 && gr > 0)
            dirents_old_to_new((uint32_t) (ac[1] & LFI3I_CAGEID_MASK), a[1], gr);
        return gr;
    }

    // Default implementation. The thread is the caller's own when it acts on
    // itself; otherwise operations on the target use its surrogate thread.
    // Operations that depend on the calling thread (arch_prctl, gettid, ...)
    // must use the target's real thread if it is parked in this call chain;
    // otherwise the target's surrogate thread stands in.
    struct LFILinuxThread *nt = t;
    if (self != target) {
        struct Frame *f = frame_serving(target);
        nt = f && f->caller_t ? f->caller_t : target->kt;
    }
    struct Call c;
    call_init(&c, self, target, nt, nr, a, ac, false);
    long r = threei_native(&c);
    call_done(&c);
    if (gd64 && r > 0)
        dirents_old_to_new((uint32_t) (ac[1] & LFI3I_CAGEID_MASK), a[1], r);
    return r;
}

// make_threei_call: the guest passes a request structure.
long
threei_make_call(struct Cage *caller, struct LFILinuxThread *t, lfiptr reqp)
{
    struct LFIBox *box = caller->proc->box;
    if (!lfi_box_bufvalid(box, reqp, sizeof(struct lfi3i_call)) ||
        (reqp & 7) != 0)
        return -EFAULT;
    // Take a private snapshot: the sandbox must not change it under us.
    struct lfi3i_call rq;
    memcpy(&rq, (void *) lfi_box_l2p(box, reqp), sizeof(rq));

    // A call goes through the handler table of 'self': the caller's own, or
    // that of a cage the caller is entitled to act on (a descendant, say: a
    // clamp grate sends calls through the tables of the grates it launched).
    struct Cage *self = caller;
    bool self_ref = false;
    if (rq.self != caller->id) {
        if (rq.self > 0xffffffffu || !(self = cage_get((uint32_t) rq.self)))
            return -ESRCH;
        self_ref = true;
        if (!cage_authorized_for(caller, self)) {
            cage_put(self);
            return -EPERM;
        }
    }

    struct Cage *target;
    if (rq.target == caller->id) {
        target = caller;
        atomic_fetch_add(&caller->refs, 1); // balanced by the put below
    } else {
        target = rq.target > 0xffffffffu ? NULL : cage_get((uint32_t) rq.target);
        if (!target) {
            if (self_ref)
                cage_put(self);
            return -ESRCH;
        }
        if (!cage_authorized_for(caller, target)) {
            cage_put(target);
            if (self_ref)
                cage_put(self);
            return -EPERM;
        }
        // Calls to a cage that is shutting down fail (except exit itself).
        if (atomic_load(&target->state) != CAGE_RUNNING &&
            rq.callnum != 60 && rq.callnum != 231) {
            cage_put(target);
            if (self_ref)
                cage_put(self);
            return -ESRCH;
        }
    }
    long r = threei_dispatch(self, (long) rq.callnum, target, t, rq.arg,
        rq.argcage);
    cage_put(target);
    if (self_ref)
        cage_put(self);
    return r;
}

// ---------------------------------------------------------------------------
// System call entry from sandboxed code
// ---------------------------------------------------------------------------

// System calls the guest's context start-up (thread entry, then the pause call) may make.
static bool
capture_startup_call(uint64_t nr)
{
    switch (nr) {
    case 14:   // rt_sigprocmask: restore the signal mask
    case 202:  // futex
    case 218:  // set_tid_address
    case 273:  // set_robust_list
    case 186:  // gettid
    case LFI3I_SYS_SELF:
    case 1024: // pause: hands the context back to the runtime
        return true;
    default:
        return false;
    }
}

void
threei_sys_handler(struct LFIContext *ctx)
{
    struct LFILinuxThread *t = lfi_ctx_data(ctx);
    struct LFIRegs *r = lfi_ctx_regs(ctx);
    struct LFILinuxProc *p = t->proc;
    struct Cage *c = p->cage;
    uint64_t nr = r->rax;

    // A context created for a grate may run only its start-up code before it parks itself: a
    // cage that misuses the capture request to get an application thread past its clone policy
    // is ended as soon as that thread tries to do anything else.
    if (unlikely(t->capturing) && !capture_startup_call(nr)) {
        fprintf(stderr, "lfi: captured context made system call %lu before parking; "
                        "terminating the cage\n", (unsigned long) nr);
        thread_exit(t, LFI_EXIT_PROCESS, 137);
    }

    // Leave if another thread asked the cage to terminate.
    if (unlikely(atomic_load_explicit(&p->terminating, memory_order_acquire)))
        thread_exit(t, LFI_EXIT_THREAD,
            atomic_load_explicit(&p->exit_code, memory_order_relaxed));

    struct Call cl;
    cl.a[0] = r->rdi;
    cl.a[1] = r->rsi;
    cl.a[2] = r->rdx;
    cl.a[3] = r->r10;
    cl.a[4] = r->r8;
    cl.a[5] = r->r9;

    long cnr = (long) nr;
    bool gd64 = false;
    if (unlikely(nr == 57 || nr == 58 || nr == 281 || nr == 262 || nr == 217))
        gd64 = sys_alias(c, &cnr, cl.a) && nr == 217;
    int idx = ht_index(cnr);
    if (idx >= 0) {
        uint64_t e = atomic_load_explicit(&c->htab[idx], memory_order_acquire);
        // Only the clone that creates a handler context bypasses the table (nr, not
        // cnr: fork is aliased to clone and must stay mediated).
        if (unlikely(e != 0) && !(lfi_capture_clone && (nr == 56 || nr == 435))) {
            sys_clear_unused_args(cnr, cl.a);
            uint64_t *a = cl.a;
            // Every argument of a call a cage makes itself refers to the cage.
            uint64_t ac[6] = { c->id, c->id, c->id, c->id, c->id, c->id };
            long res = grate_call(c, t, c, (uint32_t) (e >> 32), (uint32_t) e,
                cnr, a, ac);
            // Handlers return a C int, so a call that returns a pointer
            // (mmap, mremap, brk) comes back as the low 32 bits of the
            // address, i.e. an offset in the sandbox (this is also what a
            // grate sees when it forwards such a call). Make it a pointer
            // again for the cage.
            if (unlikely(nr == 9 || nr == 12 || nr == 25)) {
                int32_t r32 = (int32_t) res;
                if (!(r32 >= -4095 && r32 <= 0))
                    res = (long) (p->box_info.base + (uint32_t) r32);
                else
                    res = r32;
            }
            if (unlikely(gd64) && res > 0)
                dirents_old_to_new(c->id, cl.a[1], res);
            if (unlikely(sig_pending_for(c, t) || t->has_saved_sigmask))
                res = sig_deliver(c, t, r, cnr, cl.a, res);
            r->rax = (uint64_t) res;
            return;
        }
    }

    // Default implementation. Arguments of a self call need no cage ids: they
    // all refer to the calling cage (Call.self).
    cl.caller = c;
    cl.target = c;
    cl.t = t;
    cl.nr = cnr;
    cl.self = true;
    if (unlikely(nr >= HT_3I_BASE)) {
        // The 3i calls read their payload slots directly.
        for (int i = 0; i < 6; i++) {
            cl.ac[i] = c->id;
            cl.xc[i] = NULL;
        }
        cl.self = false;
    }
    long res = threei_native(&cl);
    // Run a handler for a pending signal before returning to the guest.
    if (unlikely(sig_pending_for(c, t) || t->has_saved_sigmask))
        res = sig_deliver(c, t, r, cnr, cl.a, res);
    r->rax = (uint64_t) res;
}
