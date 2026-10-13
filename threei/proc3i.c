#define _GNU_SOURCE
// Process semantics on top of sandboxes: fork, exec, wait, exit and kill.
//
// A cage is a sandbox (4GiB slot in the shared address space) plus a main
// host thread. fork() clones the sandbox's memory into a fresh sandbox and
// starts a new host thread in the copy; exec() resets the sandbox in place
// and loads a new program into it; exit tears the sandbox down and leaves a
// zombie for the parent to reap.
#include "cage.h"

#include "cwalk.h"
#include "fd.h"
#include "futex.h"
#include "lock.h"
#include "path.h"
#include "sys/sys.h"

#include <elf.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if !defined(LFI_ARCH_X64)
#error "lfi 3i currently supports x86-64 only"
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// The real (sandbox-executing) thread of the call's target cage, when it is
// parked in the current host thread's call chain; NULL if there is none.
static struct LFILinuxThread *
real_thread(struct Call *c)
{
    if (c->t && c->t->ctx && c->caller == c->target &&
        c->t->proc == c->target->proc)
        return c->t;
    struct Frame *f = frame_serving(c->target);
    return f ? f->caller_t : NULL;
}

// ---------------------------------------------------------------------------
// Host thread of a cage
// ---------------------------------------------------------------------------

int
cage_run_main(struct Cage *c, struct LFILinuxThread *t, uintptr_t entry)
{
    pthread_mutex_lock(&g_eng->tree_lk);
    c->host_thread = pthread_self();
    c->host_thread_valid = true;
    pthread_mutex_unlock(&g_eng->tree_lk);

    c->main_t = t;
    grate_spawn_state_reset();
    int code;
    for (;;) {
        code = lfi_ctx_run(t->ctx, entry);
        struct LFILinuxThread *nt = c->next_thread;
        if (!nt)
            break;
        // exec: continue on the new image's main thread.
        c->next_thread = NULL;
        grate_spawn_state_reset();
        t->stack = 0; // the old stack died with the old image
        lfi_thread_free(t);
        t = nt;
        c->main_t = t;
        entry = c->proc->entry;
    }
    if (atomic_load(&c->proc->terminating))
        code = atomic_load(&c->proc->exit_code);
    t->stack = 0;
    lfi_thread_free(t);
    return cage_finish(c, code);
}

struct ChildStart {
    struct Cage *c;
    struct LFILinuxThread *t;
    uintptr_t entry;
};

static void *
child_main(void *arg)
{
    struct ChildStart s = *(struct ChildStart *) arg;
    free(arg);
    cage_run_main(s.c, s.t, s.entry);
    return NULL;
}

// Reaps a zombie child (tree_lk held): removes it from the parent's list and
// drops the reference the list held.
static void
reap_locked(struct Cage *parent, size_t idx)
{
    struct Cage *ch = parent->children[idx];
    parent->children[idx] = parent->children[--parent->nchildren];
    cage_put(ch);
}

// Calls the exit handler a grate registered for 'c' (exit_group, else exit)
// with the status the cage dies with. The grate must not rely on the cage
// answering: a forwarded exit for a cage that is already dying does nothing.
static void
notify_abnormal_exit(struct Cage *c, int code)
{
    static const int calls[] = { 231, 60 };
    for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        uint64_t e = atomic_load_explicit(&c->htab[calls[i]],
            memory_order_acquire);
        if (!e || (uint32_t) (e >> 32) == 0xffffffffu)
            continue;
        uint64_t a[6] = { (uint64_t) code, 0, 0, 0, 0, 0 };
        uint64_t ac[6] = { c->id, c->id, c->id, c->id, c->id, c->id };
        grate_call(c, NULL, c, (uint32_t) (e >> 32), (uint32_t) e, calls[i], a,
            ac);
        return;
    }
}

// Called by the host thread of a cage after its program has finished.
int
cage_finish(struct Cage *c, int code)
{
    struct LFILinuxProc *p = c->proc;

    // Wait for the cage's other threads (spawned with clone) to finish.
    {
        LOCK_WITH_DEFER(&p->lk_threads, lk);
        while (p->active_threads != 0)
            pthread_cond_wait(&p->cond_threads, &p->lk_threads);
    }

    // Stop accepting grate calls and wait for the ones in progress.
    atomic_store(&c->state, CAGE_EXITING);
    while (atomic_load(&c->inflight) != 0) {
        struct timespec ts = { 0, 20000 };
        nanosleep(&ts, NULL);
    }

    int status = c->killsig ? c->killsig : ((code & 0xff) << 8);

    // A cage that was killed never made its exit call, so the grates that
    // interpose on exit would not learn of its death (Lind's harsh_cage_exit).
    // Call them once, with the cage's sandbox still intact.
    if (c->killsig)
        notify_abnormal_exit(c, code);

    // Handlers that pointed at this cage now fail instead of being left to
    // dangle (the id may be reused by an unrelated cage).
    if (atomic_load(&c->is_grate)) {
        for (size_t i = 1; i < g_eng->table_n; i++) {
            struct Cage *x = cage_get((uint32_t) i);
            if (!x)
                continue;
            for (size_t k = 0, kn = ht_limit(x); k < kn; k++) {
                uint64_t e = atomic_load_explicit(&x->htab[k],
                    memory_order_acquire);
                if (e && (uint32_t) (e >> 32) == c->id)
                    atomic_store_explicit(&x->htab[k], HT_DEAD,
                        memory_order_release);
            }
            cage_put(x);
        }
    }

    // Give the children to the init cage (or orphan them).
    pthread_mutex_lock(&g_eng->tree_lk);
    c->host_thread_valid = false;
    struct Cage *init = atomic_load(&g_eng->table[1]);
    bool init_ok = init && init != c &&
        atomic_load(&init->state) == CAGE_RUNNING;
    for (size_t i = 0; i < c->nchildren; i++) {
        struct Cage *ch = c->children[i];
        bool zombie = atomic_load(&ch->state) == CAGE_ZOMBIE;
        if (init_ok) {
            // Children (finished or not) become init's: it can wait for them.
            ch->ppid = (int) init->id;
            if (init->nchildren == init->cchildren) {
                size_t n = init->cchildren ? init->cchildren * 2 : 8;
                init->children = realloc(init->children, n * sizeof(void *));
                init->cchildren = n;
            }
            init->children[init->nchildren++] = ch;
            if (zombie)
                pthread_cond_broadcast(&init->cv);
        } else {
            ch->ppid = 0;
            if (zombie)
                cage_put(ch); // nobody can reap it
        }
    }
    c->nchildren = 0;
    pthread_mutex_unlock(&g_eng->tree_lk);

    // Release the sandbox and everything attached to it.
    sig_cage_fini(c);
    shm_close_all(p);
    fdfree(&p->fdtable);
    grate_free_all(c);
    atomic_store_explicit(&g_eng->bases[c->id], 0, memory_order_release);
    pthread_rwlock_wrlock(&c->memlk);
    c->proc = NULL;
    struct LFILinuxThread *kt = c->kt;
    c->kt = NULL;
    proc_destroy(p);
    pthread_rwlock_unlock(&c->memlk);
    free(kt);

    // Become a zombie and tell the parent.
    pthread_mutex_lock(&g_eng->tree_lk);
    c->exit_status = status;
    atomic_store(&c->state, CAGE_ZOMBIE);
    cage_unpublish(c);
    struct Cage *parent = c->ppid > 0 ?
        atomic_load(&g_eng->table[c->ppid]) : NULL;
    bool orphan = true;
    uint32_t parent_id = 0, my_id = c->id;
    if (parent && parent != c) {
        for (size_t i = 0; i < parent->nchildren; i++) {
            if (parent->children[i] == c) {
                orphan = false;
                parent_id = parent->id;
                pthread_cond_broadcast(&parent->cv);
                break;
            }
        }
    }
    if (orphan)
        cage_put(c); // nobody will wait for it: drop the owner reference
    pthread_mutex_lock(&g_eng->lk);
    g_eng->live--;
    if (g_eng->live == 0)
        pthread_cond_broadcast(&g_eng->cv);
    pthread_mutex_unlock(&g_eng->lk);
    pthread_mutex_unlock(&g_eng->tree_lk);
    if (parent_id) {
        // Tell the parent (SIGCHLD is ignored unless it installed a handler).
        struct Cage *pp = cage_get(parent_id);
        if (pp) {
            sig_send(pp, NULL, SIGCHLD, (int) my_id);
            cage_put(pp);
        }
    }
    return status;
}

// ---------------------------------------------------------------------------
// Creating cages
// ---------------------------------------------------------------------------

static int
new_kt(struct Cage *c)
{
    struct LFILinuxThread *kt = thread_alloc(c->proc, LFI_THREAD_HOST);
    if (!kt)
        return -1;
    kt->tid = (int) c->id;
    c->kt = kt;
    return 0;
}

struct Cage *
cage_create_root(struct Engine3i *e)
{
    struct LFILinuxProc *p = lfi_proc_new(e->linux_);
    if (!p)
        return NULL;
    struct Cage *c = cage_alloc(0, p);
    if (!c) {
        proc_destroy(p);
        return NULL;
    }
    if (new_kt(c)) {
        atomic_store(&c->state, CAGE_FREE);
        return NULL;
    }
    return c;
}

// ---------------------------------------------------------------------------
// fork
// ---------------------------------------------------------------------------

static inline uint64_t
reloc(uint64_t v, uint64_t src_hi, uint64_t dst_hi)
{
    return (v >> 32) == src_hi ? ((v & 0xffffffffULL) | (dst_hi << 32)) : v;
}

long
cage_fork(struct Call *c, uint64_t flags, uint64_t stack)
{
    struct Cage *parent = c->target;
    struct LFILinuxThread *pt = real_thread(c);
    if (!pt || !pt->ctx)
        return -ESRCH;
    struct LFILinuxProc *pp = parent->proc;

    // Child sandbox, cage and descriptor table.
    struct LFILinuxProc *np = lfi_proc_new(g_eng->linux_);
    if (!np)
        return -EAGAIN;
    struct Cage *child = cage_alloc(parent->id, np);
    if (!child) {
        proc_destroy(np);
        return -EAGAIN;
    }
    long err = -EAGAIN;

    // The child's memory is a copy of ours, with pointers relocated.
    if (lfi_box_clone(np->box, pp->box, true, shm_clone_fd, NULL) != 0)
        goto fail;
    np->has_shared = pp->has_shared;
    np->brkbase = pp->brkbase;
    np->brksize = pp->brksize;
    np->entry = pp->entry;
    np->elfinfo = pp->elfinfo;
    fdfree(&np->fdtable); // drop the default standard streams
    if (fdclone(&np->fdtable, &pp->fdtable) != 0)
        goto fail;
    {
        LOCK_WITH_DEFER(&pp->cwd.lk, lk);
        memcpy(np->cwd.path, pp->cwd.path, sizeof(np->cwd.path));
    }
    child->umask = parent->umask;
    child->nice = parent->nice;
    // The child starts with the parent's interposition rules.
    atomic_store_explicit(&child->alt_hw,
        atomic_load_explicit(&parent->alt_hw, memory_order_acquire),
        memory_order_release);
    for (size_t i = 0, n = ht_limit(parent); i < n; i++)
        atomic_store_explicit(&child->htab[i],
            atomic_load_explicit(&parent->htab[i], memory_order_acquire),
            memory_order_relaxed);
    if (new_kt(child))
        goto fail;

    // The child's main thread resumes right after the fork system call.
    struct LFILinuxThread *nt = thread_alloc(np, LFI_THREAD_MAIN);
    if (!nt)
        goto fail;
    nt->tid = (int) child->id;
    nt->sigmask = pt->sigmask;
    pthread_mutex_lock(&parent->siglk);
    memcpy(child->act, parent->act, sizeof(child->act));
    pthread_mutex_unlock(&parent->siglk);
    nt->ctx = lfi_ctx_new(np->box, nt);
    if (!nt->ctx) {
        free(nt);
        goto fail;
    }
    nt->stack_size = pt->stack_size;

    uint64_t src_hi = (uint64_t) pp->box_info.base >> 32;
    uint64_t dst_hi = (uint64_t) np->box_info.base >> 32;
    struct LFIRegs *pr = lfi_ctx_regs(pt->ctx);
    struct LFIRegs *cr = lfi_ctx_regs(nt->ctx);
#define R(f) cr->f = reloc(pr->f, src_hi, dst_hi)
    R(rbx); R(rcx); R(rdx); R(rbp); R(rsi); R(rdi);
    R(r8); R(r9); R(r10); R(r11); R(r12); R(r13);
#undef R
    cr->rax = 0;
    cr->rsp = reloc(stack ? stack : pr->rsp, src_hi, dst_hi);
    cr->fcw = pr->fcw;
    cr->mxcsr = pr->mxcsr;
    memcpy(cr->xmm, pr->xmm, sizeof(cr->xmm));
    lfi_ctx_set_tp(nt->ctx, reloc(lfi_ctx_get_tp(pt->ctx), src_hi, dst_hi));

    uintptr_t entry = cr->r11;

    // CLONE_PARENT_SETTID / CLONE_CHILD_SETTID.
    if (flags & LINUX_CLONE_PARENT_SETTID) {
        int *p = call_buf(c, 2, sizeof(int), alignof(int));
        if (p)
            *p = (int) child->id;
    }
    if (flags & LINUX_CLONE_CHILD_SETTID) {
        lfiptr cp = reloc(c->a[3], src_hi, dst_hi);
        if (lfi_box_bufvalid(np->box, cp, sizeof(int)) && (cp & 3) == 0)
            *(int *) lfi_box_l2p(np->box, cp) = (int) child->id;
    }

    struct ChildStart *cs = malloc(sizeof(*cs));
    if (!cs) {
        lfi_thread_free(nt);
        goto fail;
    }
    *cs = (struct ChildStart) { child, nt, entry };

    cage_add_child(parent, child);
    cage_publish(child);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    if (pthread_create(&th, &attr, child_main, cs) != 0) {
        pthread_attr_destroy(&attr);
        free(cs);
        lfi_thread_free(nt);
        err = -EAGAIN;
        // Undo the registration: the child never ran.
        pthread_mutex_lock(&g_eng->tree_lk);
        for (size_t i = 0; i < parent->nchildren; i++) {
            if (parent->children[i] == child) {
                parent->children[i] = parent->children[--parent->nchildren];
                break;
            }
        }
        pthread_mutex_unlock(&g_eng->tree_lk);
        cage_unpublish(child);
        goto fail;
    }
    pthread_attr_destroy(&attr);
    return (long) child->id;

fail:
    // Nothing has been published; release what was built. (cage_put drops the
    // owner reference and recycles the cage structure.)
    {
        atomic_store_explicit(&g_eng->bases[child->id], 0, memory_order_release);
        struct LFILinuxProc *dp = child->proc;
        child->proc = NULL;
        if (child->kt) {
            free(child->kt);
            child->kt = NULL;
        }
        if (dp)
            proc_destroy(dp);
        pthread_mutex_lock(&g_eng->lk);
        g_eng->live--;
        if (g_eng->live == 0)
            pthread_cond_broadcast(&g_eng->cv);
        pthread_mutex_unlock(&g_eng->lk);
        atomic_store(&child->state, CAGE_ZOMBIE);
        cage_put(child);
    }
    return err;
}

long
nat_clone(struct Call *c)
{
    uint64_t flags = c->a[0];
    if (flags & LINUX_CLONE_THREAD) {
        // A new thread of the target cage, created from the target's own
        // thread (which is the calling thread, or is parked in a grate call).
        struct LFILinuxThread *rt = real_thread(c);
        if (!rt || !rt->ctx)
            return -EPERM;
        return sys_clone(rt, c->a[0], c->a[1], c->a[2], c->a[3], c->a[4],
            c->a[5]);
    }
    // A new process. Shared-memory processes (CLONE_VM without THREAD) other
    // than vfork-style spawns are not supported.
    uint64_t allowed = 0xff | LINUX_CLONE_CHILD_SETTID |
        LINUX_CLONE_CHILD_CLEARTID | LINUX_CLONE_PARENT_SETTID |
        LINUX_CLONE_VM | LINUX_CLONE_VFORK;
    if (flags & ~allowed)
        return -EINVAL;
    if ((flags & LINUX_CLONE_VM) && !(flags & LINUX_CLONE_VFORK))
        return -EINVAL;
    return cage_fork(c, flags, c->a[1]);
}

// ---------------------------------------------------------------------------
// exit / wait / kill
// ---------------------------------------------------------------------------


long
cage_exit(struct Call *c, bool group)
{
    int code = (int) c->a[0];
    struct LFILinuxThread *rt = real_thread(c);
    if (!rt)
        return -ESRCH;
    struct Frame *f = frame_serving(c->target);
    if (f)
        frames_unwind_through(f);
    if (group) {
        // Like Linux's exit_group: the cage's other threads must not stay
        // blocked (in a futex, a read, ...) or spin on. Publish the request,
        // then interrupt them; they see it at their next system call.
        struct LFILinuxProc *p = c->target->proc;
        atomic_store(&p->exit_code, code);
        atomic_store(&p->terminating, true);
        sig_kick(c->target);
    }
    thread_exit(rt, group ? LFI_EXIT_PROCESS : LFI_EXIT_THREAD, code);
}

long
cage_wait(struct Call *c)
{
    struct Cage *me = c->target;
    int pid = (int) c->a[0];
    int options = (int) c->a[2];
    // __WNOTHREAD | __WCLONE | __WALL are accepted and ignored.
    if (options & ~(WNOHANG | WUNTRACED | WCONTINUED | 0x20000000 |
                       0x80000000u | 0x40000000))
        return -EINVAL;
    int *stp = NULL;
    if (c->a[1]) {
        stp = call_buf(c, 1, sizeof(int), alignof(int));
        if (!stp)
            return -EFAULT;
    }
    // Resource usage is not tracked.
    if (c->a[3]) {
        void *ru = call_buf(c, 3, 144, 8);
        if (!ru)
            return -EFAULT;
        memset(ru, 0, 144);
    }

    pthread_mutex_lock(&g_eng->tree_lk);
    for (;;) {
        bool have = false;
        for (size_t i = 0; i < me->nchildren; i++) {
            struct Cage *ch = me->children[i];
            if (pid > 0 && (int) ch->id != pid)
                continue;
            have = true;
            if (atomic_load(&ch->state) == CAGE_ZOMBIE) {
                int id = (int) ch->id;
                int status = ch->exit_status;
                reap_locked(me, i);
                pthread_mutex_unlock(&g_eng->tree_lk);
                if (stp)
                    *stp = status;
                return id;
            }
        }
        if (!have) {
            pthread_mutex_unlock(&g_eng->tree_lk);
            return -ECHILD;
        }
        if (options & WNOHANG) {
            pthread_mutex_unlock(&g_eng->tree_lk);
            return 0;
        }
        if (me->proc && (atomic_load(&me->proc->terminating) ||
                (c->t && c->t->ctx && c->caller == me && sig_pending_for(me, c->t)))) {
            pthread_mutex_unlock(&g_eng->tree_lk);
            return -EINTR;
        }
        pthread_cond_wait(&me->cv, &g_eng->tree_lk);
    }
}

// Asks a cage to terminate, as the default action of a fatal signal would.
// Its threads notice at their next system call; blocking calls are interrupted
// and a thread running guest code is stopped by the SIGUSR1 that sig_kick sends.
void
cage_terminate(struct Cage *v, int sig)
{
    struct LFILinuxProc *p = v->proc;
    if (!p)
        return;
    if (v->killsig == 0)
        v->killsig = sig;
    atomic_store(&p->exit_code, 128 + sig);
    atomic_store(&p->terminating, true);
    sig_kick(v);
}

// Terminates every cage except 'except' (used when the init cage exits).
void
cage_terminate_all(struct Cage *except)
{
    for (size_t i = 1; i < g_eng->table_n; i++) {
        struct Cage *v = cage_get((uint32_t) i);
        if (!v)
            continue;
        if (v != except)
            cage_terminate(v, SIGKILL);
        cage_put(v);
    }
}

// ---------------------------------------------------------------------------
// execve
// ---------------------------------------------------------------------------

#define EXEC_MAX_ARGS 4096
#define EXEC_MAX_BYTES (1u << 20)

static void
strv_free(char **v, int n)
{
    for (int i = 0; i < n; i++)
        free(v[i]);
    free(v);
}

// Copies a NULL-terminated array of string pointers out of argument 'ai's
// address space.
static long
strv_collect(struct Call *c, int ai, char ***out, int *n_out)
{
    *out = NULL;
    *n_out = 0;
    char **v = calloc(EXEC_MAX_ARGS + 1, sizeof(char *));
    if (!v)
        return -ENOMEM;
    int n = 0;
    if (c->a[ai] != 0) {
        struct LFIBox *box = call_box(c, ai);
        if (!box) {
            free(v);
            return -EFAULT;
        }
        size_t bytes = 0;
        for (;; n++) {
            if (n >= EXEC_MAX_ARGS) {
                strv_free(v, n);
                return -E2BIG;
            }
            lfiptr ep = c->a[ai] + 8 * (uint64_t) n;
            if (!lfi_box_bufvalid(box, ep, 8) || (ep & 7)) {
                strv_free(v, n);
                return -EFAULT;
            }
            uint64_t sp = *(uint64_t *) lfi_box_l2p(box, ep);
            if (sp == 0)
                break;
            size_t len = lfi_box_strnlen(box, sp, EXEC_MAX_BYTES);
            if (len == SIZE_MAX) {
                strv_free(v, n);
                return -EFAULT;
            }
            bytes += len + 1;
            if (bytes > EXEC_MAX_BYTES) {
                strv_free(v, n);
                return -E2BIG;
            }
            v[n] = strndup((char *) lfi_box_l2p(box, sp), len);
            if (!v[n]) {
                strv_free(v, n);
                return -ENOMEM;
            }
        }
    }
    *out = v;
    *n_out = n;
    return 0;
}

// Reads a whole file; the caller frees *buf.
static long
read_file(const char *path, uint8_t **buf, size_t *size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        int e = S_ISDIR(st.st_mode) ? EACCES : errno ? errno : EACCES;
        close(fd);
        return -e;
    }
    if (st.st_size < 4 || st.st_size > (1L << 30)) {
        close(fd);
        return -ENOEXEC;
    }
    uint8_t *b = malloc(st.st_size);
    if (!b) {
        close(fd);
        return -ENOMEM;
    }
    size_t got = 0;
    while (got < (size_t) st.st_size) {
        ssize_t r = pread(fd, b + got, st.st_size - got, got);
        if (r <= 0) {
            free(b);
            close(fd);
            return -EIO;
        }
        got += r;
    }
    close(fd);
    *buf = b;
    *size = got;
    return 0;
}

// Checks that 'img' is a position-independent x86-64 ELF program whose
// executable segments pass verification, i.e. that loading it will succeed.
static bool
image_acceptable(struct LFILinuxProc *p, uint8_t *img, size_t size)
{
    Elf64_Ehdr eh;
    if (size < sizeof(eh))
        return false;
    memcpy(&eh, img, sizeof(eh));
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_type != ET_DYN ||
        eh.e_machine != EM_X86_64 || eh.e_phentsize != sizeof(Elf64_Phdr) ||
        eh.e_phnum == 0 || eh.e_phnum > 64 ||
        eh.e_phoff > size || size - eh.e_phoff < eh.e_phnum * sizeof(Elf64_Phdr))
        return false;
    for (unsigned i = 0; i < eh.e_phnum; i++) {
        Elf64_Phdr ph;
        memcpy(&ph, img + eh.e_phoff + i * sizeof(ph), sizeof(ph));
        if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_X))
            continue;
        if (ph.p_offset > size || size - ph.p_offset < ph.p_filesz)
            return false;
        if (!lfi_verify_code(p->engine->engine, (char *) img + ph.p_offset,
                ph.p_filesz, ph.p_vaddr))
            return false;
    }
    return true;
}

long
cage_exec(struct Call *c)
{
    struct Cage *T = c->target;
    struct LFILinuxThread *pt = real_thread(c);
    if (!pt || !pt->ctx)
        return -ESRCH;
    struct LFILinuxProc *p = T->proc;
    if (p->active_threads != 0)
        return -ENOSYS; // exec from a multi-threaded cage is not supported

    char *path = call_path(c, 0);
    if (!path)
        return -EFAULT;
    char **argv = NULL, **envp = NULL;
    int argc = 0, envc = 0;
    long r = strv_collect(c, 1, &argv, &argc);
    if (!r)
        r = strv_collect(c, 2, &envp, &envc);
    if (r) {
        free(path);
        if (argv)
            strv_free(argv, argc);
        return r;
    }

    // Locate the program, following #! interpreters.
    uint8_t *img = NULL;
    size_t imgsize = 0;
    char host[FILENAME_MAX];
    for (int depth = 0;; depth++) {
        {
            LOCK_WITH_DEFER(&p->cwd.lk, lk);
            if (!path_resolve(p, path, host, sizeof(host))) {
                r = -ENOENT;
                goto fail;
            }
        }
        if (access(host, X_OK) != 0) {
            r = -errno;
            goto fail;
        }
        free(img);
        img = NULL;
        r = read_file(host, &img, &imgsize);
        if (r)
            goto fail;
        if (imgsize >= 4 && memcmp(img, "\177ELF", 4) == 0)
            break;
        if (imgsize < 2 || img[0] != '#' || img[1] != '!' || depth >= 4) {
            r = -ENOEXEC;
            goto fail;
        }
        // #!interpreter [argument]
        size_t eol = 2;
        while (eol < imgsize && eol < 256 && img[eol] != '\n')
            eol++;
        char line[258];
        memcpy(line, img + 2, eol - 2);
        line[eol - 2] = 0;
        char *interp = line;
        while (*interp == ' ' || *interp == '\t')
            interp++;
        char *arg = interp;
        while (*arg && *arg != ' ' && *arg != '\t')
            arg++;
        if (*arg) {
            *arg++ = 0;
            while (*arg == ' ' || *arg == '\t')
                arg++;
        }
        if (!*interp) {
            r = -ENOEXEC;
            goto fail;
        }
        // New argv: interpreter, [arg], script path, original args[1..].
        int extra = *arg ? 2 : 1;
        char **nv = calloc(argc + extra + 1, sizeof(char *));
        if (!nv) {
            r = -ENOMEM;
            goto fail;
        }
        int k = 0;
        nv[k++] = strdup(interp);
        if (*arg)
            nv[k++] = strdup(arg);
        nv[k++] = strdup(path);
        for (int i = 1; i < argc; i++)
            nv[k++] = argv[i];
        free(argv[0]);
        free(argv);
        argv = nv;
        argc = k;
        free(path);
        path = strdup(interp);
    }
    free(path);
    path = NULL;

    // Reject a program that would not load before the old one is destroyed.
    if (!image_acceptable(p, img, imgsize)) {
        r = -ENOEXEC;
        goto fail;
    }

    // ---- Point of no return: the old program image is replaced. ----
    fdcloexec(&p->fdtable);
    grate_free_all(T);
    free(p->dynsym.data);
    free(p->dynstr.data);
    p->dynsym = (struct ElfSection) { 0 };
    p->dynstr = (struct ElfSection) { 0 };
    p->libsyms = (struct LibSymbols) { 0 };
    free(p->interp_path);
    p->interp_path = NULL;
    free(p->prog_path);
    p->prog_path = NULL;
    p->brksize = 0;
    shm_close_all(p);
    lfi_box_reset(p->box);

    bool ok = lfi_proc_load(p, img, imgsize, NULL);
    free(img);
    img = NULL;
    struct LFILinuxThread *nt = NULL;
    if (ok) {
        lfi_box_init_ret(p->box);
        nt = lfi_thread_new(p, argc, (const char **) argv,
            (const char **) envp);
    }
    strv_free(argv, argc);
    strv_free(envp, envc);
    if (!nt) {
        // The old image is gone and the new one failed to load: the cage
        // cannot continue.
        T->killsig = SIGKILL;
        pt->ctidp = 0; // it pointed into the image that is gone
        atomic_store(&p->exit_code, 127);
        atomic_store(&p->terminating, true);
        struct Frame *f = frame_serving(T);
        if (f)
            frames_unwind_through(f);
        thread_exit(pt, LFI_EXIT_PROCESS, 127);
    }
    nt->tid = (int) T->id;
    nt->sigmask = pt->sigmask;
    sig_cage_reset_for_exec(T);
    T->next_thread = nt;
    pt->stack = 0;

    // Discard any grate frames this call came through, then leave the old
    // image: control returns to the cage's main loop, which switches over.
    struct Frame *f = frame_serving(T);
    if (f)
        frames_unwind_through(f);
    lfi_ctx_exit(pt->ctx, 0);
    __builtin_unreachable();

fail:
    free(path);
    free(img);
    strv_free(argv, argc);
    strv_free(envp, envc);
    return r;
}
