#define _GNU_SOURCE
// Calling into grates.
//
// A grate is an ordinary cage that has registered handlers for other cages'
// system calls. When a cage makes such a call, the thread making it is
// switched, in place, into the grate's sandbox ("yield" in LFI terms: save the
// callee-saved registers and jump), runs the handler on a private execution
// context of the grate, and is switched back with the handler's result. There
// is no hand-off to another host thread, no queueing and no locking on the
// fast path: a call costs two sandbox transitions plus copying the request
// (14 words) into the grate's memory.
//
// Each execution context of a grate has its own stack and thread-local storage
// so handlers can run concurrently on different host threads and nest. The
// guest shim library creates them using the ordinary pthread machinery of the
// guest's libc (see guest/lfi3i.c): a thread is created, run until it parks,
// and the runtime captures its context instead of scheduling it.
#include "cage.h"

#include "lock.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(LFI_ARCH_X64)
#error "lfi 3i currently supports x86-64 only"
#endif

#define CACHE_N 8

struct TCache {
    uint32_t gid;
    uint32_t gen;
    struct GCtx *gc;
};

// Per-host-thread cache of the grate context last used for each grate, so
// repeated calls skip the shared pool entirely.
static _Thread_local struct TCache tcache[CACHE_N];

// While this host thread is creating a context for itself (gctx_spawn), the
// new context is handed back through this slot instead of going into the
// shared pool, where another thread could take it first.
static _Thread_local struct GCtx **collect_into;

_Thread_local bool tl_spawn_auth;

// Drops any context-spawn state this host thread still holds, so it cannot
// carry over into whatever guest code runs next on it.
void
grate_spawn_state_reset(void)
{
    tl_spawn_auth = false;
    lfi_capture_clone = false;
    lfi_captured_thread = NULL;
}

long
grate_setentry(struct Cage *c, struct LFILinuxThread *t, lfiptr entry,
    lfiptr spawn)
{
    (void) t;
    struct LFIBox *box = c->proc->box;
    if (!lfi_box_ptrvalid(box, entry) || !lfi_box_ptrvalid(box, spawn))
        return -EFAULT;
    // Indirect branch targets must be 32-byte bundle starts on x86-64.
    if ((entry & 31) || (spawn & 31))
        return -EINVAL;
    // The first installation per program image grants the budget for the
    // shim's initial contexts; the runtime drives any further creation itself.
    if (!c->g_entry && !c->g_spawn)
        atomic_store(&c->g_init_budget, 2);
    c->g_entry = entry;
    c->g_spawn = spawn;
    return 0;
}

// The guest asks that its next clone create a handler context. Granted only
// where the runtime expects one (see tl_spawn_auth); anything else is refused
// and leaves the mediation state untouched.
long
grate_capture(struct Cage *c)
{
    if (!c->g_spawn)
        return -EPERM;
    if (!tl_spawn_auth) {
        int b = atomic_load(&c->g_init_budget);
        do {
            if (b <= 0)
                return -EPERM;
        } while (!atomic_compare_exchange_weak(&c->g_init_budget, &b, b - 1));
    }
    lfi_capture_clone = true;
    return 0;
}

// Called by the guest right after it created a context thread with the
// "capture" flag set: takes ownership of the context that creation produced.
long
grate_collect(struct Cage *c, long spawn_result)
{
    struct LFILinuxThread *nt = lfi_captured_thread;
    lfi_captured_thread = NULL;
    lfi_capture_clone = false;
    if (spawn_result != 0 || !nt) {
        fprintf(stderr, "lfi3i: grate %u: context creation failed (%ld)\n",
                c->id, spawn_result);
        return -EAGAIN;
    }

    struct GCtx *gc = calloc(1, sizeof(*gc));
    if (!gc)
        return -ENOMEM;
    gc->t = nt;
    gc->ctx = nt->ctx;

    // Carve a request buffer out of the parked thread's stack, below its
    // current frames, and start invocations below it.
    struct LFIRegs *regs = lfi_ctx_regs(nt->ctx);
    uint64_t base = (regs->rsp - 1024) & ~63ULL;
    gc->req = base;
    regs->rsp = base - 64;
    gc->idle_rsp = regs->rsp;

    LOCK_WITH_DEFER(&c->g_lk, lk);
    gc->next_all = c->g_all;
    c->g_all = gc;
    if (!c->g_cloner) {
        // The first context is reserved for creating further contexts.
        c->g_cloner = gc;
    } else if (collect_into && !*collect_into) {
        *collect_into = gc;
    } else {
        gc->next_free = c->g_free;
        c->g_free = gc;
    }
    return 0;
}

static struct GCtx *
pool_pop(struct Cage *g)
{
    LOCK_WITH_DEFER(&g->g_lk, lk);
    struct GCtx *gc = g->g_free;
    if (gc) {
        g->g_free = gc->next_free;
        gc->next_free = NULL;
    }
    return gc;
}

static void
pool_push(struct Cage *g, struct GCtx *gc)
{
    LOCK_WITH_DEFER(&g->g_lk, lk);
    gc->next_free = g->g_free;
    g->g_free = gc;
}

// Creates one more execution context by running the guest's spawn function
// on the reserved cloner context.
static struct GCtx *
gctx_spawn(struct Cage *g)
{
    LOCK_WITH_DEFER(&g->g_spawn_lk, lk_spawn);
    // Another thread may have refilled the pool while we waited.
    struct GCtx *gc = pool_pop(g);
    if (gc)
        return gc;
    struct GCtx *cl = g->g_cloner;
    if (!cl || !g->g_spawn)
        return NULL;
    struct GCtx *created = NULL;
    collect_into = &created;
    struct LFIInvokeInfo old = lfi_invoke_info;
    tl_spawn_auth = true;
    (void) LFI_INVOKE(g->proc->box, &cl->ctx, g->g_spawn, long, (void) );
    grate_spawn_state_reset(); // success, failure or abandoned capture alike
    lfi_invoke_info = old;
    collect_into = NULL;
    return created;
}

static struct GCtx *
gctx_acquire(struct Cage *g)
{
    struct TCache *e = &tcache[g->id % CACHE_N];
    if (e->gc && e->gid == g->id && e->gen == g->gen && !e->gc->busy) {
        e->gc->busy = true;
        return e->gc;
    }
    struct GCtx *gc = pool_pop(g);
    if (!gc)
        gc = gctx_spawn(g);
    if (!gc)
        return NULL;
    gc->busy = true;
    // Replace the cache entry; an idle previous occupant goes back to its
    // grate's pool (if that grate still exists).
    if (e->gc && !e->gc->busy) {
        struct Cage *og = cage_get(e->gid);
        if (og && og->gen == e->gen)
            pool_push(og, e->gc);
        if (og)
            cage_put(og);
    }
    e->gid = g->id;
    e->gen = g->gen;
    e->gc = gc;
    return gc;
}

static void
gctx_release(struct Cage *g, struct GCtx *gc)
{
    gc->busy = false;
    struct TCache *e = &tcache[g->id % CACHE_N];
    if (e->gc != gc)
        pool_push(g, gc);
}

void
grate_free_all(struct Cage *c)
{
    struct GCtx *gc = c->g_all;
    while (gc) {
        struct GCtx *next = gc->next_all;
        lfi_thread_free(gc->t);
        free(gc);
        gc = next;
    }
    c->g_all = c->g_free = c->g_cloner = NULL;
    c->g_entry = c->g_spawn = 0;
    atomic_store(&c->g_init_budget, 0);
}

// Undo a grate call whose host frames are about to be discarded (the thread
// is being unwound to a cage's main loop for exit/exec).
void
frame_abort(struct Frame *f)
{
    struct GCtx *gc = f->gc;
    if (gc) {
        lfi_ctx_regs(gc->ctx)->rsp = gc->idle_rsp;
        if (f->grate)
            gctx_release(f->grate, gc);
    }
    if (f->grate) {
        atomic_fetch_sub(&f->grate->inflight, 1);
        cage_put(f->grate);
    }
}

long
grate_call(struct Cage *caller, struct LFILinuxThread *caller_t,
    struct Cage *target, uint32_t gid, uint32_t fnoff, long nr,
    const uint64_t a[6], const uint64_t ac[6])
{
    struct Cage *g = cage_get(gid);
    if (!g)
        return -ESRCH;
    if (atomic_load_explicit(&g->state, memory_order_acquire) != CAGE_RUNNING ||
        !g->g_entry) {
        cage_put(g);
        return -ESRCH;
    }
    // Count the call before re-checking the state so the grate cannot finish
    // exiting underneath us.
    atomic_fetch_add_explicit(&g->inflight, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&g->state, memory_order_acquire) != CAGE_RUNNING) {
        atomic_fetch_sub(&g->inflight, 1);
        cage_put(g);
        return -ESRCH;
    }

    struct GCtx *gc = gctx_acquire(g);
    if (!gc) {
        fprintf(stderr, "lfi3i: grate %u: no execution context available\n", g->id);
        atomic_fetch_sub(&g->inflight, 1);
        cage_put(g);
        return -EAGAIN;
    }

    struct lfi3i_req *rq = (struct lfi3i_req *) gc->req;
    rq->fn = g->proc->box_info.base + fnoff;
    rq->cageid = g->id;
    for (int i = 0; i < 6; i++) {
        rq->arg[i] = a[i];
        rq->argcage[i] = ac[i];
    }

    struct Frame fr = {
        .prev = tl_frames,
        .caller = caller,
        .caller_t = caller_t,
        .target = target,
        .nr = nr,
        .a = a,
        .ac = ac,
        .grate = g,
        .gc = gc,
    };
    tl_frames = &fr;
    struct LFIInvokeInfo old = lfi_invoke_info;
    long r = LFI_INVOKE(g->proc->box, &gc->ctx, g->g_entry, long, (lfiptr),
        gc->req);
    lfi_invoke_info = old;
    tl_frames = fr.prev;

    gctx_release(g, gc);
    atomic_fetch_sub_explicit(&g->inflight, 1, memory_order_acq_rel);
    cage_put(g);
    // Handlers return a C int.
    return (int) r;
}
