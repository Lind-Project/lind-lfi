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
#include <time.h>

#include "lock.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(LFI_ARCH_X64)
#error "lfi 3i currently supports x86-64 only"
#endif

#define CACHE_N 8

// An entry is only dereferenced after the grate cage it names is looked up again and still has
// the same id generation (gen) and program image (img): a cage that exited, whose id was reused,
// or that exec'd has freed its contexts, so the cached pointer is dead.
struct TCache {
    uint32_t gid;
    uint32_t gen;
    uint64_t img;
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
    tl_sys_jb = NULL; // may point into a host stack that a nonlocal exit threw away
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
    // This can run nested inside another spawn on the same host thread (a parent grate's clone
    // handler needing a context while the outer capture is armed): keep the outer state intact.
    bool outer_cap = lfi_capture_clone;
    struct LFILinuxThread *outer_thread = lfi_captured_thread;
    bool outer_auth = tl_spawn_auth;
    grate_spawn_state_reset();
    tl_spawn_auth = true;
    (void) LFI_INVOKE(g->proc->box, &cl->ctx, g->g_spawn, long, (void) );
    grate_spawn_state_reset(); // success, failure or abandoned capture alike
    lfi_capture_clone = outer_cap;
    lfi_captured_thread = outer_thread;
    tl_spawn_auth = outer_auth;
    lfi_invoke_info = old;
    collect_into = NULL;
    return created;
}

static struct GCtx *
gctx_acquire(struct Cage *g)
{
    struct TCache *e = &tcache[g->id % CACHE_N];
    // The caller has counted this call in g->inflight, and teardown (grate_free_all) waits for
    // inflight to reach zero after bumping img, so an entry whose img still matches is alive.
    if (e->gc && e->gid == g->id && e->gen == g->gen &&
        e->img == atomic_load(&g->img) && !e->gc->busy) {
        e->gc->busy = true;
        return e->gc;
    }
    struct GCtx *gc = pool_pop(g);
    if (!gc)
        gc = gctx_spawn(g);
    if (!gc)
        return NULL;
    gc->busy = true;
    // Replace the cache entry; an idle previous occupant goes back to its grate's pool if that
    // grate still exists with the same contexts. Nothing of the old entry (not even its busy
    // flag) is touched before the old cage has been looked up and its generation checked, and
    // the check and the access happen under that cage's context lock, which teardown also takes
    // while freeing.
    if (e->gc && e->gid != 0) {
        struct Cage *og = cage_get(e->gid);
        if (og) {
            pthread_mutex_lock(&og->g_lk);
            if (og->gen == e->gen && e->img == atomic_load(&og->img) && !e->gc->busy) {
                e->gc->next_free = og->g_free;
                og->g_free = e->gc;
            }
            pthread_mutex_unlock(&og->g_lk);
            cage_put(og);
        }
    }
    e->gid = g->id;
    e->gen = g->gen;
    e->img = atomic_load(&g->img);
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

// Frees the cage's execution contexts (it is exiting, or its program image is being replaced).
// Every host thread may hold a cached pointer to them: the image generation is bumped first so
// the caches stop matching, new calls are refused, and the calls already in progress are waited
// for before anything is freed. If they do not finish, the contexts are leaked instead of freed:
// a leak is harmless, a use-after-free is not.
void
grate_free_all(struct Cage *c)
{
    __atomic_store_n(&c->g_entry, 0, __ATOMIC_SEQ_CST); // new calls are refused from here on
    __atomic_store_n(&c->g_spawn, 0, __ATOMIC_SEQ_CST);
    atomic_store(&c->g_init_budget, 0);
    atomic_fetch_add(&c->img, 1); // caches stop matching
    bool quiet = true;
    for (int i = 0; atomic_load(&c->inflight) != 0; i++) {
        if (i > 500) { // ~10 s
            quiet = false;
            break;
        }
        struct timespec ts = { 0, 20000000 };
        nanosleep(&ts, NULL);
    }
    pthread_mutex_lock(&c->g_lk);
    struct GCtx *gc = c->g_all;
    c->g_all = c->g_free = c->g_cloner = NULL;
    pthread_mutex_unlock(&c->g_lk);
    while (gc) {
        struct GCtx *next = gc->next_all;
        if (quiet) {
            lfi_thread_free(gc->t);
            free(gc);
        }
        gc = next;
    }
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
        !__atomic_load_n(&g->g_entry, __ATOMIC_SEQ_CST)) {
        cage_put(g);
        return -ESRCH;
    }
    // Count the call before re-checking the state and the entry point, so the grate cannot
    // finish exiting or replace its program image underneath us: teardown clears the entry
    // point, then waits for the count to reach zero before freeing anything (all seq_cst).
    atomic_fetch_add(&g->inflight, 1);
    if (atomic_load_explicit(&g->state, memory_order_acquire) != CAGE_RUNNING ||
        !__atomic_load_n(&g->g_entry, __ATOMIC_SEQ_CST)) {
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

    // The request block lives in the grate's own memory, which the grate (or a bug in it) may
    // have made inaccessible: that fails this call, not the runtime. This region has its own
    // recovery point so that the cleanup below still runs.
    {
        struct lfi3i_req *rq = (struct lfi3i_req *) gc->req;
        sigjmp_buf jb;
        sigjmp_buf *saved = tl_sys_jb;
        if (sigsetjmp(jb, 0) != 0) {
            tl_sys_jb = saved;
            gctx_release(g, gc);
            atomic_fetch_sub_explicit(&g->inflight, 1, memory_order_acq_rel);
            cage_put(g);
            return -EFAULT;
        }
        tl_sys_jb = &jb;
        rq->fn = g->proc->box_info.base + fnoff;
        rq->cageid = g->id;
        for (int i = 0; i < 6; i++) {
            rq->arg[i] = a[i];
            rq->argcage[i] = ac[i];
        }
        tl_sys_jb = saved;
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
