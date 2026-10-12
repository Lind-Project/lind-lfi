#define _GNU_SOURCE
// POSIX signals for cages.
//
// Dispositions (sigaction) are shared by the cage's threads; masks and
// thread-directed signals belong to a thread. A signal that is not ignored
// and not fatal waits in the pending set of the cage (or the thread) until a
// thread that does not block it is about to return to the guest from a
// system call. At that point the handler is entered the way the kernel does
// it: a frame (siginfo, ucontext, return address = the program's restorer) is
// built on the thread's stack or alternate stack, and the thread resumes at the
// handler. The handler returns through the restorer into rt_sigreturn, which
// restores the register state saved by the runtime (the guest cannot alter
// it) and the previous mask, and restarts the interrupted call if the
// handler asked for that (SA_RESTART).
//
// Signals are noticed when a thread makes a system call or when a blocking
// system call is interrupted; a thread spinning in user code is not
// interrupted by a handled signal, but one that is *terminated* by a signal
// is stopped at once (see the SIGUSR1 handler in engine.c).
#include "cage.h"
#include "natutil.h"

#include "lock.h"
#include "sys/sys.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define KSA_NOCLDSTOP 1
#define KSA_NOCLDWAIT 2
#define KSA_SIGINFO   4
#define KSA_RESTORER  0x04000000
#define KSA_ONSTACK   0x08000000
#define KSA_RESTART   0x10000000
#define KSA_NODEFER   0x40000000
#define KSA_RESETHAND 0x80000000u

#define H_DFL 0
#define H_IGN 1

#define UNBLOCKABLE (SIGBIT(SIGKILL) | SIGBIT(SIGSTOP))

enum { A_TERM, A_IGNORE };

static int
default_action(int sig)
{
    switch (sig) {
    case SIGCHLD:
    case SIGURG:
    case SIGWINCH:
    case SIGCONT:
    case SIGSTOP: // no job control
    case SIGTSTP:
    case SIGTTIN:
    case SIGTTOU:
        return A_IGNORE;
    default:
        return A_TERM;
    }
}

// What a handler finds on the stack (x86-64 rt_sigframe, in the kernel's
// layout so handlers that look at ucontext_t work).
struct gframe {
    uint64_t pretcode; // the handler returns here
    struct {
        uint64_t uc_flags, uc_link;
        uint64_t ss_sp;
        int32_t ss_flags, _pad;
        uint64_t ss_size;
        uint64_t gregs[23];
        uint64_t fpregs;
        uint64_t reserved[8];
        uint64_t sigmask;
    } uc;
    uint8_t info[128];
};

// The state to resume at after a handler. Kept by the runtime, not in the
// guest's memory.
struct SigSave {
    struct SigSave *next;
    struct LFIRegs regs;
    uint64_t frame_sp;
    uint64_t old_mask;
    uint32_t flags;
    int sig;
    long nr;
    uint64_t a[6];
    bool restart;
};

// ---------------------------------------------------------------------------
// Cage state
// ---------------------------------------------------------------------------

void
sig_thread_free(struct LFILinuxThread *t)
{
    struct SigSave *ss = t->sigframes;
    t->sigframes = NULL;
    while (ss) {
        struct SigSave *next = ss->next;
        free(ss);
        ss = next;
    }
}

void
sig_cage_init(struct Cage *c)
{
    memset(c->act, 0, sizeof(c->act));
    atomic_store(&c->pending, 0);
    memset(c->sender, 0, sizeof(c->sender));
    c->main_t = NULL;
    c->itimer_on = false;
}

// exec: caught signals revert to the default; ignored ones stay ignored.
void
sig_cage_reset_for_exec(struct Cage *c)
{
    pthread_mutex_lock(&c->siglk);
    for (int s = 1; s <= NSIG3I; s++) {
        if (c->act[s].handler > H_IGN)
            memset(&c->act[s], 0, sizeof(c->act[s]));
    }
    pthread_mutex_unlock(&c->siglk);
}

void
sig_cage_fini(struct Cage *c)
{
    if (c->itimer_on) {
        timer_delete(c->itimer);
        c->itimer_on = false;
    }
}

static bool
code_addr_ok(struct Cage *c, uint64_t addr)
{
    struct LFIBoxInfo bi = c->proc->box_info;
    uint32_t off = (uint32_t) addr;
    return (off & 31) == 0 && bi.base + off >= bi.min &&
        bi.base + off < bi.max_exec;
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

void
sig_kick(struct Cage *v)
{
    pthread_mutex_lock(&g_eng->tree_lk);
    pthread_cond_broadcast(&v->cv);
    if (v->host_thread_valid)
        pthread_kill(v->host_thread, SIGUSR1);
    pthread_mutex_unlock(&g_eng->tree_lk);

    // Other threads of the cage (created by the guest) run on host threads of
    // their own.
    pthread_rwlock_rdlock(&v->memlk);
    struct LFILinuxProc *p = v->proc;
    if (p) {
        LOCK_WITH_DEFER(&p->lk_threads, lk);
        for (struct List *e = list_first(p->threads); e;
             e = list_next(p->threads, e)) {
            struct LFILinuxThread *t =
                LIST_CONTAINER(struct LFILinuxThread, threads_elem, e);
            if (t->pthread)
                pthread_kill(*t->pthread, SIGUSR1);
        }
    }
    pthread_rwlock_unlock(&v->memlk);
}

// Makes 'sig' pending for thread 'th' (non-NULL, which the caller guarantees
// stays valid) or for the cage.
static void
post(struct Cage *v, struct LFILinuxThread *th, int sig, int sender)
{
    struct kact ka;
    pthread_mutex_lock(&v->siglk);
    ka = v->act[sig];
    pthread_mutex_unlock(&v->siglk);

    // Signals that are ignored are discarded when generated.
    if (ka.handler == H_IGN ||
        (ka.handler == H_DFL && default_action(sig) == A_IGNORE))
        return;

    if (ka.handler == H_DFL) {
        // Fatal by default. SIGKILL cannot be blocked; for the others the
        // death waits until the target unblocks the signal.
        struct LFILinuxThread *target = th ? th : v->main_t;
        bool blocked = target && sig != SIGKILL &&
            (target->sigmask & SIGBIT(sig));
        if (!blocked) {
            cage_terminate(v, sig);
            return;
        }
    }
    if (th) {
        atomic_fetch_or(&th->sigpending, SIGBIT(sig));
    } else {
        v->sender[sig] = sender;
        atomic_fetch_or(&v->pending, SIGBIT(sig));
    }
    sig_kick(v);
}

// Sends 'sig' to the cage as a whole.
void
sig_send(struct Cage *v, struct LFILinuxThread *th, int sig, int sender)
{
    if (sig < 1 || sig > NSIG3I)
        return;
    post(v, th, sig, sender);
}

// Sends 'sig' to the thread of cage 'v' whose tid is 'tid'. Returns -ESRCH if
// there is no such thread.
static long
send_tid(struct Cage *v, struct LFILinuxThread *self, int tid, int sig,
    int sender)
{
    if (self && self->tid == tid && self->proc == v->proc) {
        post(v, self, sig, sender);
        return 0;
    }
    if (tid == (int) v->id) {
        // The main thread; it lives as long as the cage does.
        post(v, v->main_t, sig, sender);
        return 0;
    }
    long r = -ESRCH;
    struct LFILinuxProc *p = v->proc;
    if (!p)
        return r;
    LOCK_WITH_DEFER(&p->lk_threads, lk);
    for (struct List *e = list_first(p->threads); e;
         e = list_next(p->threads, e)) {
        struct LFILinuxThread *t =
            LIST_CONTAINER(struct LFILinuxThread, threads_elem, e);
        if (t->tid == tid) {
            post(v, t, sig, sender);
            r = 0;
            break;
        }
    }
    return r;
}

long
cage_kill(struct Call *c)
{
    int pid, sig;
    int tid = 0;
    switch (c->nr) {
    case 62: // kill(pid, sig)
        pid = (int) c->a[0];
        sig = (int) c->a[1];
        break;
    case 200: // tkill(tid, sig): a thread of the target cage
        tid = (int) c->a[0];
        pid = (int) c->target->id;
        sig = (int) c->a[1];
        break;
    default: // tgkill(tgid, tid, sig)
        pid = (int) c->a[0];
        tid = (int) c->a[1];
        sig = (int) c->a[2];
        break;
    }
    if (sig < 0 || sig > NSIG3I)
        return -EINVAL;
    if (pid == 0)
        pid = (int) c->target->id;
    if (pid < 0)
        return -ESRCH; // process groups are not supported

    struct Cage *v = (uint32_t) pid == c->target->id ? c->target :
                                                      cage_get((uint32_t) pid);
    if (!v)
        return -ESRCH;
    bool own = v == c->target;
    long ret = 0;
    if (!own && !cage_authorized_for(c->caller, v)) {
        ret = -EPERM;
    } else if (atomic_load(&v->state) != CAGE_RUNNING) {
        ret = -ESRCH;
    } else if (sig != 0) {
        int sender = (int) c->target->id;
        if (tid != 0)
            ret = send_tid(v, own ? c->t : NULL, tid, sig, sender);
        else
            sig_send(v, NULL, sig, sender);
    }
    if (!own)
        cage_put(v);
    return ret;
}

// ---------------------------------------------------------------------------
// Delivery
// ---------------------------------------------------------------------------

static void
fill_info(uint8_t *info, int sig, int sender)
{
    int32_t *w = (int32_t *) info;
    memset(info, 0, 128);
    w[0] = sig;
    if (sig == SIGALRM && sender == 0)
        w[2] = -2; // SI_TIMER
    else
        w[2] = 0; // SI_USER
    w[4] = sender; // si_pid
    w[5] = 1000;   // si_uid
}

// Enters the handler: builds the frame and redirects the thread. Returns 0
// on success, -1 if the frame does not fit in the guest's stack.
static int
enter_handler(struct Cage *c, struct LFILinuxThread *t, struct LFIRegs *r,
    int sig, const struct kact *ka, long nr, const uint64_t a[6], long res,
    bool restart)
{
    struct LFIBox *box = c->proc->box;
    uint64_t rsp0 = r->rsp;
    if ((ka->flags & KSA_ONSTACK) && t->altstack_size &&
        !(t->altstack_flags & SS_DISABLE)) {
        uint64_t lo = t->altstack_sp, hi = lo + t->altstack_size;
        rsp0 = (rsp0 >= lo && rsp0 < hi) ? rsp0 - 128 : hi;
    } else {
        rsp0 -= 128; // the red zone
    }
    if (rsp0 < sizeof(struct gframe) + 64)
        return -1;
    uint64_t sp = ((rsp0 - sizeof(struct gframe)) & ~15ULL) - 8;
    if (!lfi_box_bufvalid(box, sp, sizeof(struct gframe)))
        return -1;

    struct gframe *f = (struct gframe *) lfi_box_l2p(box, sp);
    uint64_t base = c->proc->box_info.base;
    memset(f, 0, sizeof(*f));
    f->pretcode = base + (uint32_t) ka->restorer;
    fill_info(f->info, sig, c->sender[sig]);
    uint64_t old_mask = t->has_saved_sigmask ? t->saved_sigmask : t->sigmask;
    f->uc.ss_sp = t->altstack_sp;
    f->uc.ss_size = t->altstack_size;
    f->uc.ss_flags = t->altstack_flags;
    f->uc.sigmask = old_mask;
    uint64_t *g = f->uc.gregs;
    g[0] = r->r8; g[1] = r->r9; g[2] = r->r10; g[3] = r->r11;
    g[4] = r->r12; g[5] = r->r13; g[6] = r->r14; g[7] = r->r15;
    g[8] = r->rdi; g[9] = r->rsi; g[10] = r->rbp; g[11] = r->rbx;
    g[12] = r->rdx; g[13] = (uint64_t) res; g[14] = r->rcx; g[15] = r->rsp;
    g[16] = r->r11; // the resume address
    g[17] = 0x202;

    struct SigSave *ss = calloc(1, sizeof(*ss));
    if (!ss)
        return -1;
    ss->regs = *r;
    ss->regs.rax = (uint64_t) res;
    ss->frame_sp = sp;
    ss->old_mask = old_mask;
    ss->flags = (uint32_t) ka->flags;
    ss->sig = sig;
    ss->nr = nr;
    memcpy(ss->a, a, sizeof(ss->a));
    ss->restart = restart;
    ss->next = t->sigframes;
    t->sigframes = ss;
    t->has_saved_sigmask = false;

    // The handler runs with the handler's mask, plus the signal itself.
    t->sigmask |= ka->mask;
    if (!(ka->flags & KSA_NODEFER))
        t->sigmask |= SIGBIT(sig);
    t->sigmask &= ~UNBLOCKABLE;

    r->rsp = sp;
    r->rdi = (uint64_t) sig;
    r->rsi = sp + offsetof(struct gframe, info);
    r->rdx = sp + offsetof(struct gframe, uc);
    r->rax = 0;
    r->r11 = base + (uint32_t) ka->handler;
    return 0;
}

static bool
restartable(long nr)
{
    switch (nr) {
    case 0: case 1: case 2: case 17: case 18: case 19: case 20: case 40:
    case 42: case 43: case 44: case 45: case 46: case 47: case 61: case 73:
    case 257: case 288: case 295: case 296: case 72:
        return true;
    default:
        return false; // nanosleep, poll, select, epoll_wait, pause, ...
    }
}

long
sig_deliver(struct Cage *c, struct LFILinuxThread *t, struct LFIRegs *r,
    long nr, const uint64_t a[6], long res)
{
    for (;;) {
        uint64_t avail = (atomic_load(&t->sigpending) |
                             atomic_load(&c->pending)) &
            ~t->sigmask;
        if (!avail) {
            // sigsuspend() ended without a handler running.
            if (t->has_saved_sigmask) {
                t->sigmask = t->saved_sigmask;
                t->has_saved_sigmask = false;
            }
            return res;
        }
        int sig = __builtin_ctzll(avail) + 1;
        uint64_t bit = SIGBIT(sig);
        // Claim it; another thread may have taken a cage-wide signal.
        bool had = (atomic_fetch_and(&t->sigpending, ~bit) & bit) != 0;
        if (!had)
            had = (atomic_fetch_and(&c->pending, ~bit) & bit) != 0;
        if (!had)
            continue;

        struct kact ka;
        pthread_mutex_lock(&c->siglk);
        ka = c->act[sig];
        if (ka.handler > H_IGN && (ka.flags & KSA_RESETHAND))
            memset(&c->act[sig], 0, sizeof(c->act[sig]));
        pthread_mutex_unlock(&c->siglk);

        if (ka.handler == H_IGN)
            continue;
        if (ka.handler == H_DFL) {
            if (default_action(sig) == A_TERM) {
                cage_terminate(c, sig);
                thread_exit(t, LFI_EXIT_PROCESS, 128 + sig);
            }
            continue;
        }
        bool restart = res == -EINTR && (ka.flags & KSA_RESTART) &&
            restartable(nr);
        if (enter_handler(c, t, r, sig, &ka, nr, a, res, restart) != 0) {
            // No room for the frame: like the kernel, die of SIGSEGV.
            cage_terminate(c, SIGSEGV);
            thread_exit(t, LFI_EXIT_PROCESS, 128 + SIGSEGV);
        }
        return 0;
    }
}

// rt_sigreturn: the handler is done.
static long
n_sigreturn(struct Call *c)
{
    struct LFILinuxThread *t = c->t;
    if (!t || !t->ctx || t->proc != c->target->proc)
        return -EPERM;
    struct LFIRegs *r = lfi_ctx_regs(t->ctx);
    // The frame the handler is returning from: the return address has been
    // popped, so rsp is just above the frame base. Frames abandoned by a
    // longjmp out of a handler are dropped.
    struct SigSave *ss = t->sigframes;
    while (ss && ss->frame_sp + 8 != r->rsp) {
        t->sigframes = ss->next;
        free(ss);
        ss = t->sigframes;
    }
    if (!ss) {
        cage_terminate(c->target, SIGSEGV);
        thread_exit(t, LFI_EXIT_PROCESS, 128 + SIGSEGV);
    }
    t->sigframes = ss->next;
    t->sigmask = ss->old_mask & ~UNBLOCKABLE;

    // Resume exactly where the system call was about to return.
    struct LFIRegs *s = &ss->regs;
    r->rsp = s->rsp; r->rcx = s->rcx; r->rdx = s->rdx; r->rbx = s->rbx;
    r->rbp = s->rbp; r->rsi = s->rsi; r->rdi = s->rdi; r->r8 = s->r8;
    r->r9 = s->r9; r->r10 = s->r10; r->r11 = s->r11; r->r12 = s->r12;
    r->r13 = s->r13;
    r->fcw = s->fcw;
    r->mxcsr = s->mxcsr;
    memcpy(r->xmm, s->xmm, sizeof(r->xmm));
    long res = (long) s->rax;

    if (ss->restart) {
        // SA_RESTART: run the interrupted call again.
        uint64_t ac[6];
        for (int i = 0; i < 6; i++)
            ac[i] = c->target->id;
        res = threei_dispatch(c->target, ss->nr, c->target, t, ss->a, ac);
    }
    free(ss);
    return res;
}

// ---------------------------------------------------------------------------
// System calls
// ---------------------------------------------------------------------------

// The target cage's thread making the call: the calling thread itself, or,
// when a grate forwards the call, the thread of the target parked in the grate
// (the dispatcher passes it in c->t).
static struct LFILinuxThread *
cur_thread(struct Call *c)
{
    return (c->t && c->t->ctx && c->t->proc == c->target->proc) ? c->t : NULL;
}

NATIVE(n_rt_sigaction)
{
    int sig = (int) c->a[0];
    if (c->a[3] != 8 || sig < 1 || sig > NSIG3I)
        return -EINVAL;
    struct kact *oact = NULL, nact;
    bool have_new = c->a[1] != 0;
    if (c->a[2]) {
        oact = call_buf(c, 2, sizeof(struct kact), 8);
        if (!oact)
            return -EFAULT;
    }
    if (have_new) {
        struct kact *g = call_buf(c, 1, sizeof(struct kact), 8);
        if (!g)
            return -EFAULT;
        nact = *g;
        if (sig == SIGKILL || sig == SIGSTOP)
            return -EINVAL;
        if (nact.handler > H_IGN) {
            // Handlers are entered by jumping to them: they must be
            // verified-code entry points.
            if (!code_addr_ok(c->target, nact.handler) ||
                !(nact.flags & KSA_RESTORER) ||
                !code_addr_ok(c->target, nact.restorer))
                return -EINVAL;
        }
        nact.mask &= ~UNBLOCKABLE;
    }
    pthread_mutex_lock(&c->target->siglk);
    struct kact old = c->target->act[sig];
    if (have_new)
        c->target->act[sig] = nact;
    pthread_mutex_unlock(&c->target->siglk);
    if (have_new && (nact.handler == H_IGN ||
                        (nact.handler == H_DFL &&
                            default_action(sig) == A_IGNORE)))
        atomic_fetch_and(&c->target->pending, ~SIGBIT(sig));
    if (oact)
        *oact = old;
    return 0;
}

NATIVE(n_rt_sigprocmask)
{
    struct LFILinuxThread *t = cur_thread(c);
    if (!t)
        return -EPERM;
    if (c->a[3] != 8)
        return -EINVAL;
    uint64_t *oset = NULL;
    if (c->a[2]) {
        oset = call_buf(c, 2, 8, 8);
        if (!oset)
            return -EFAULT;
    }
    uint64_t old = t->sigmask;
    if (c->a[1]) {
        uint64_t *set = call_buf(c, 1, 8, 8);
        if (!set)
            return -EFAULT;
        uint64_t m = *set;
        switch ((int) c->a[0]) {
        case SIG_BLOCK:
            t->sigmask |= m;
            break;
        case SIG_UNBLOCK:
            t->sigmask &= ~m;
            break;
        case SIG_SETMASK:
            t->sigmask = m;
            break;
        default:
            return -EINVAL;
        }
        t->sigmask &= ~UNBLOCKABLE;
    }
    if (oset)
        *oset = old;
    return 0;
}

NATIVE(n_rt_sigpending)
{
    struct LFILinuxThread *t = cur_thread(c);
    uint64_t *set = call_buf(c, 0, 8, 8);
    if (!t || c->a[1] != 8)
        return -EINVAL;
    if (!set)
        return -EFAULT;
    *set = (atomic_load(&t->sigpending) | atomic_load(&c->target->pending)) &
        t->sigmask;
    return 0;
}

// Waits (tree_lk held by the caller) until 'ready' or the deadline or the
// cage is terminating. Returns false on timeout.
static bool
wait_cv(struct Cage *v, const struct timespec *deadline)
{
    if (!deadline) {
        pthread_cond_wait(&v->cv, &g_eng->tree_lk);
        return true;
    }
    return pthread_cond_timedwait(&v->cv, &g_eng->tree_lk, deadline) != ETIMEDOUT;
}

NATIVE(n_pause_or_suspend)
{
    // pause(): wait for a signal. rt_sigsuspend(mask, size): the same with a
    // temporary mask.
    struct LFILinuxThread *t = cur_thread(c);
    if (!t)
        return -EPERM;
    if (c->nr == 130) {
        if (c->a[1] != 8)
            return -EINVAL;
        uint64_t *m = call_buf(c, 0, 8, 8);
        if (!m)
            return -EFAULT;
        t->saved_sigmask = t->sigmask;
        t->has_saved_sigmask = true;
        t->sigmask = *m & ~UNBLOCKABLE;
    }
    struct Cage *v = c->target;
    pthread_mutex_lock(&g_eng->tree_lk);
    while (!sig_pending_for(v, t) && !atomic_load(&v->proc->terminating))
        wait_cv(v, NULL);
    pthread_mutex_unlock(&g_eng->tree_lk);
    return -EINTR;
}

NATIVE(n_rt_sigtimedwait)
{
    struct LFILinuxThread *t = cur_thread(c);
    if (!t || c->a[3] != 8)
        return -EINVAL;
    uint64_t *set = call_buf(c, 0, 8, 8);
    if (!set)
        return -EFAULT;
    uint64_t want = *set & ~UNBLOCKABLE;
    struct timespec deadline, *dl = NULL;
    if (c->a[2]) {
        struct { int64_t sec, nsec; } *ts = call_buf(c, 2, 16, 8);
        if (!ts)
            return -EFAULT;
        if (ts->sec < 0 || ts->nsec < 0 || ts->nsec >= 1000000000)
            return -EINVAL;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += ts->sec;
        deadline.tv_nsec += ts->nsec;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
        dl = &deadline;
    }
    struct Cage *v = c->target;
    int got = 0;
    pthread_mutex_lock(&g_eng->tree_lk);
    for (;;) {
        uint64_t avail = (atomic_load(&t->sigpending) | atomic_load(&v->pending)) & want;
        if (avail) {
            got = __builtin_ctzll(avail) + 1;
            uint64_t bit = SIGBIT(got);
            if (!(atomic_fetch_and(&t->sigpending, ~bit) & bit) &&
                !(atomic_fetch_and(&v->pending, ~bit) & bit))
                continue;
            break;
        }
        if (sig_pending_for(v, t) || atomic_load(&v->proc->terminating)) {
            pthread_mutex_unlock(&g_eng->tree_lk);
            return -EINTR;
        }
        if (!wait_cv(v, dl)) {
            pthread_mutex_unlock(&g_eng->tree_lk);
            return -EAGAIN;
        }
    }
    pthread_mutex_unlock(&g_eng->tree_lk);
    if (c->a[1]) {
        uint8_t *info = call_buf(c, 1, 128, 8);
        if (info)
            fill_info(info, got, v->sender[got]);
    }
    return got;
}

NATIVE(n_sigaltstack)
{
    struct LFILinuxThread *t = cur_thread(c);
    if (!t)
        return -EPERM;
    struct { uint64_t sp; int32_t flags, pad; uint64_t size; } *old = NULL, *ss = NULL;
    if (c->a[1]) {
        old = call_buf(c, 1, 24, 8);
        if (!old)
            return -EFAULT;
    }
    if (c->a[0]) {
        ss = call_buf(c, 0, 24, 8);
        if (!ss)
            return -EFAULT;
    }
    uint64_t rsp = lfi_ctx_regs(t->ctx)->rsp;
    bool on = t->altstack_size && rsp >= t->altstack_sp &&
        rsp < t->altstack_sp + t->altstack_size;
    if (old) {
        old->sp = t->altstack_sp;
        old->size = t->altstack_size;
        old->flags = (t->altstack_size ? 0 : SS_DISABLE) | (on ? SS_ONSTACK : 0);
        old->pad = 0;
    }
    if (ss) {
        if (on)
            return -EPERM;
        if (ss->flags & SS_DISABLE) {
            t->altstack_sp = t->altstack_size = 0;
            t->altstack_flags = SS_DISABLE;
        } else {
            if (ss->size < 2048)
                return -ENOMEM;
            if (!lfi_box_bufvalid(c->target->proc->box, ss->sp, ss->size))
                return -EFAULT;
            t->altstack_sp = ss->sp;
            t->altstack_size = ss->size;
            t->altstack_flags = 0;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Interval timers (alarm, setitimer)
// ---------------------------------------------------------------------------

static void
itimer_fire(union sigval sv)
{
    uint64_t key = (uint64_t) (uintptr_t) sv.sival_ptr;
    struct Cage *v = cage_get((uint32_t) key);
    if (!v)
        return;
    if (v->gen == (uint32_t) (key >> 32) &&
        atomic_load(&v->state) == CAGE_RUNNING)
        sig_send(v, NULL, SIGALRM, 0);
    cage_put(v);
}

static int
itimer_ensure(struct Cage *v)
{
    if (v->itimer_on)
        return 0;
    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = itimer_fire;
    sev.sigev_value.sival_ptr =
        (void *) (uintptr_t) (((uint64_t) v->gen << 32) | v->id);
    if (timer_create(CLOCK_MONOTONIC, &sev, &v->itimer) != 0)
        return -errno;
    v->itimer_on = true;
    return 0;
}

static void
tv_from(struct timeval *tv, const struct timespec *ts)
{
    tv->tv_sec = ts->tv_sec;
    tv->tv_usec = ts->tv_nsec / 1000;
}

// setitimer(ITIMER_REAL) with the timeval values already parsed.
static long
set_real_timer(struct Cage *v, const struct timeval *val,
    const struct timeval *interval, struct itimerval *old)
{
    long r = itimer_ensure(v);
    if (r)
        return r;
    struct itimerspec its = {
        .it_value = { val->tv_sec, val->tv_usec * 1000 },
        .it_interval = { interval->tv_sec, interval->tv_usec * 1000 },
    };
    struct itimerspec prev;
    if (timer_settime(v->itimer, 0, &its, &prev) != 0)
        return -errno;
    if (old) {
        tv_from(&old->it_value, &prev.it_value);
        tv_from(&old->it_interval, &prev.it_interval);
    }
    return 0;
}

NATIVE(n_alarm)
{
    struct itimerval old = { 0 };
    struct timeval val = { (time_t) (uint32_t) c->a[0], 0 }, zero = { 0, 0 };
    long r = set_real_timer(c->target, &val, &zero, &old);
    if (r)
        return r;
    long rem = old.it_value.tv_sec + (old.it_value.tv_usec ? 1 : 0);
    return rem;
}

NATIVE(n_setitimer)
{
    if ((int) c->a[0] != ITIMER_REAL)
        return -EINVAL;
    struct itimerval *nv = call_buf(c, 1, sizeof(*nv), 8);
    if (!nv)
        return -EFAULT;
    struct itimerval *ov = NULL;
    if (c->a[2]) {
        ov = call_buf(c, 2, sizeof(*ov), 8);
        if (!ov)
            return -EFAULT;
    }
    struct itimerval old = { 0 };
    long r = set_real_timer(c->target, &nv->it_value, &nv->it_interval,
        ov ? &old : NULL);
    if (!r && ov)
        *ov = old;
    return r;
}

NATIVE(n_getitimer)
{
    if ((int) c->a[0] != ITIMER_REAL)
        return -EINVAL;
    struct itimerval *cur = call_buf(c, 1, sizeof(*cur), 8);
    if (!cur)
        return -EFAULT;
    memset(cur, 0, sizeof(*cur));
    if (c->target->itimer_on) {
        struct itimerspec its;
        if (timer_gettime(c->target->itimer, &its) == 0) {
            tv_from(&cur->it_value, &its.it_value);
            tv_from(&cur->it_interval, &its.it_interval);
        }
    }
    return 0;
}

bool
nat_signal(struct Call *c, long *ret)
{
    switch (c->nr) {
    case 13:  *ret = n_rt_sigaction(c); return true;
    case 14:  *ret = n_rt_sigprocmask(c); return true;
    case 15:  *ret = n_sigreturn(c); return true;
    case 34:  /* pause */
    case 130: /* rt_sigsuspend */
        *ret = n_pause_or_suspend(c); return true;
    case 36:  *ret = n_getitimer(c); return true;
    case 37:  *ret = n_alarm(c); return true;
    case 38:  *ret = n_setitimer(c); return true;
    case 127: *ret = n_rt_sigpending(c); return true;
    case 128: *ret = n_rt_sigtimedwait(c); return true;
    case 131: *ret = n_sigaltstack(c); return true;
    default:  return false;
    }
}
