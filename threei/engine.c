#define _GNU_SOURCE
// Engine setup and the entry point that runs the initial cage.
#include "cage.h"

#include "lock.h"
#include "sys/sys.h"

#include <execinfo.h>
#include <ucontext.h>

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Size of the sandbox address range and the position of the context pointer
// below the context register block (see core/arch_asm.h).
#define BOX_SPAN        (4ULL * 1024 * 1024 * 1024)
#define CTXREG_CTX_OFFS (-(8 + 32 * 8))

// Returns the context of the sandbox thread that a signal interrupted, or
// NULL if the interrupted code was the runtime's.
static struct LFIContext *
interrupted_ctx(ucontext_t *uc)
{
    uint64_t rip = uc->uc_mcontext.gregs[REG_RIP];
    uint64_t base = uc->uc_mcontext.gregs[REG_R14];
    uint64_t ctxreg = uc->uc_mcontext.gregs[REG_R15];
    // Sandbox code runs with its base in r14 and the context block in r15.
    // Runtime code may have anything in those registers, so r14 must be the
    // base of a sandbox that exists.
    if (base == 0 || rip - base >= BOX_SPAN || ctxreg == 0)
        return NULL;
    for (size_t i = 1; i < g_eng->table_n; i++) {
        if (atomic_load_explicit(&g_eng->bases[i], memory_order_acquire) == base)
            return *(struct LFIContext **) (ctxreg + CTXREG_CTX_OFFS);
    }
    return NULL;
}

// Leaves a sandbox thread that is interrupted in guest code and must die. A
// handler (grate) context returns an error from the call it was serving; every
// other thread exits like exit_group would.
static void
abandon_ctx(struct LFIContext *ctx, int exit_code)
{
    struct LFILinuxThread *t = lfi_ctx_data(ctx);
    if (t->owner == LFI_THREAD_HOST) {
        // lfi_ret_end returns whatever is in rax.
        __asm__ volatile("movq %0, %%rdi\n\t"
                         "movq %1, %%rax\n\t"
                         "jmp lfi_ret_end"
            :
            : "r"(ctx), "i"(-ESRCH)
            : "rdi", "rax");
        __builtin_unreachable();
    }
    thread_exit(t, LFI_EXIT_PROCESS, exit_code);
}

// SIGUSR1 is how the runtime interrupts a host thread: it makes blocking
// system calls return EINTR (so pending signals are noticed), and it stops a
// thread that is running guest code when its cage has been terminated.
static void
sigusr1_handler(int sig, siginfo_t *si, void *vuc)
{
    (void) sig, (void) si;
    struct LFIContext *ctx = interrupted_ctx(vuc);
    if (!ctx)
        return;
    struct LFILinuxThread *t = lfi_ctx_data(ctx);
    if (t && t->proc && atomic_load(&t->proc->terminating))
        abandon_ctx(ctx, atomic_load(&t->proc->exit_code));
}

// Is addr inside the address range of some sandbox?
static bool
fault_in_a_sandbox(void *addr)
{
    uintptr_t a = (uintptr_t) addr;
    for (size_t i = 1; i < g_eng->table_n; i++) {
        uintptr_t base = atomic_load_explicit(&g_eng->bases[i], memory_order_acquire);
        if (base && a >= base && a - base < BOX_SPAN)
            return true;
    }
    return false;
}

// A fault in sandboxed code (wild pointer, stack overflow, bad instruction)
// must only take down the cage that caused it, as SIGSEGV would a process.
static void
fault_handler(int sig, siginfo_t *si, void *vuc)
{
    ucontext_t *uc = vuc;
    // A cross-sandbox copy hit memory that went away underneath it.
    if (tl_copy_jb && (sig == SIGSEGV || sig == SIGBUS))
        siglongjmp(*tl_copy_jb, 1);
    struct LFIContext *ctx = interrupted_ctx(uc);
    // The runtime itself, while carrying out a system call, touched an inaccessible page of a
    // sandbox: the call fails with EFAULT. (A fault anywhere else in the runtime is a bug.)
    if (!ctx && tl_sys_jb && (sig == SIGSEGV || sig == SIGBUS) && fault_in_a_sandbox(si->si_addr))
        siglongjmp(*tl_sys_jb, 1);
    if (ctx) {
        struct LFILinuxThread *t = lfi_ctx_data(ctx);
        struct Cage *c = t->proc->cage;
        if (g_eng->opts.verbose)
            fprintf(stderr, "lfi3i: cage %u killed by signal %d\n", c->id, sig);
        cage_terminate(c, sig);
        abandon_ctx(ctx, 128 + sig);
    }
    // Not a sandbox fault: a bug in the runtime. Say so, then crash normally.
    {
        char msg[160];
        int n = snprintf(msg, sizeof(msg),
            "lfi3i: fatal: signal %d in the runtime (rip=%#llx, address=%p)\n",
            sig, (unsigned long long) uc->uc_mcontext.gregs[REG_RIP],
            si->si_addr);
        if (n > 0)
            (void) !write(2, msg, (size_t) n);
        void *bt[32];
        int depth = backtrace(bt, 32);
        backtrace_symbols_fd(bt, depth, 2);
    }
    signal(sig, SIG_DFL);
}

static void
install_signals(void)
{
    struct sigaction fa;
    memset(&fa, 0, sizeof(fa));
    fa.sa_sigaction = fault_handler;
    fa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&fa.sa_mask);
    sigaction(SIGSEGV, &fa, NULL);
    sigaction(SIGBUS, &fa, NULL);
    sigaction(SIGILL, &fa, NULL);
    sigaction(SIGFPE, &fa, NULL);
    sigaction(SIGTRAP, &fa, NULL);

    // A write to a pipe or socket with no reader must fail with EPIPE instead
    // of killing the whole runtime.
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigusr1_handler;
    // SA_ONSTACK: the signal may arrive while sandbox code is running, whose
    // stack pointer is not ours to use. No SA_RESTART: we want EINTR.
    sa.sa_flags = SA_ONSTACK | SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
}

static size_t
gb(size_t x)
{
    return x * 1024 * 1024 * 1024;
}

struct Lfi3iEngine *
lfi3i_new(struct Lfi3iOptions opts)
{
    if (g_eng)
        return NULL;
    if (opts.max_cages == 0)
        opts.max_cages = 256;
    if (opts.stacksize == 0)
        opts.stacksize = 2 * 1024 * 1024;

    struct Engine3i *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->opts = opts;
    pthread_mutex_init(&e->lk, NULL);
    pthread_mutex_init(&e->tree_lk, NULL);
    pthread_cond_init(&e->cv, NULL);
    e->table_n = opts.max_cages + 1;
    e->table = calloc(e->table_n, sizeof(*e->table));
    e->id_used = calloc(e->table_n, 1);
    e->bases = calloc(e->table_n, sizeof(*e->bases));
    e->next_id = 1;
    if (!e->table || !e->id_used || !e->bases)
        return NULL;

    e->core = lfi_new(
        (struct LFIOptions) {
            .boxsize = gb(4),
            .pagesize = getpagesize(),
            .no_verify = !opts.verify,
            .no_aslr = opts.no_aslr,
            .verbose = opts.verbose,
        },
        opts.max_cages);
    if (!e->core) {
        fprintf(stderr, "lfi3i: cannot create LFI engine: %s\n", lfi_errmsg());
        return NULL;
    }
    e->linux_ = lfi_linux_new(e->core,
        (struct LFILinuxOptions) {
            .stacksize = opts.stacksize,
            .verbose = opts.verbose,
            .dir_maps = opts.dir_maps,
            .wd = opts.wd,
        });
    if (!e->linux_)
        return NULL;
    lfi_sys_handler(e->core, threei_sys_handler);
    install_signals();
    g_eng = e;
    return e;
}

int
lfi3i_run(struct Lfi3iEngine *e, const char *path, int argc, const char **argv,
    const char **envp)
{
    struct Cage *root = cage_create_root(e);
    if (!root) {
        fprintf(stderr, "lfi3i: cannot create the initial cage\n");
        return 127 << 8;
    }
    if (root->id != 1)
        fprintf(stderr, "lfi3i: warning: initial cage has id %u\n", root->id);
    if (!lfi_proc_load_file(root->proc, path)) {
        fprintf(stderr, "lfi3i: cannot load %s\n", path);
        return 127 << 8;
    }
    lfi_box_init_ret(root->proc->box);
    struct LFILinuxThread *t = lfi_thread_new(root->proc, argc, argv, envp);
    if (!t) {
        fprintf(stderr, "lfi3i: cannot create the main thread\n");
        return 127 << 8;
    }
    t->tid = (int) root->id;
    cage_publish(root);
    pthread_mutex_lock(&e->lk);
    // live was incremented by cage_alloc.
    pthread_mutex_unlock(&e->lk);

    uintptr_t entry = root->proc->entry;
    int status = cage_run_main(root, t, entry);

    // The init cage is gone: nothing else may keep running.
    cage_terminate_all(NULL);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    pthread_mutex_lock(&e->lk);
    while (e->live > 0) {
        if (pthread_cond_timedwait(&e->cv, &e->lk, &deadline) == ETIMEDOUT)
            break;
    }
    pthread_mutex_unlock(&e->lk);
    return status;
}

void
lfi3i_free(struct Lfi3iEngine *e)
{
    (void) e;
    // The process is about to exit; sandboxes are released with it.
}
