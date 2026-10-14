// Internal definitions of the LFI 3i runtime.
#pragma once

#include "lfi_3i.h"

// The public header declares the engine as struct Lfi3iEngine.
#define Engine3i Lfi3iEngine
#include "lfi_arch.h"
#include "lfi_core.h"
#include "linux.h"
#include "proc.h"

#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Handler tables
// ---------------------------------------------------------------------------

// A cage's handler table has one slot per Linux system call number (0-511),
// one per 3i call (1000-1007) and a range of "alternate" call numbers
// (HT_ALT_BASE..HT_ALT_BASE+HT_ALT_N-1; the clamp grates allocate theirs from
// 2000 and 3000 upward) that grates allocate for routing: a grate that wants to send
// a call to another grate's handler registers that handler under an unused
// number in its own table and makes the call with that number. A slot is 0 for
// the default implementation, or (grate cage id << 32) | handler offset inside
// the grate's sandbox.
#define HT_LINUX_N 512
#define HT_3I_BASE 1000
#define HT_3I_N    8
#define HT_FIXED_N (HT_LINUX_N + HT_3I_N)
#define HT_ALT_BASE 1008
#define HT_ALT_N   8192
#define HT_N       (HT_FIXED_N + HT_ALT_N)

// Calls 1000-1007 are the 3i calls; HT_3I_N must cover them.
_Static_assert(HT_3I_N >= 8, "3i call range");

static inline int
ht_index(uint64_t nr)
{
    if (nr < HT_LINUX_N)
        return (int) nr;
    if (nr >= HT_3I_BASE && nr < HT_3I_BASE + HT_3I_N)
        return HT_LINUX_N + (int) (nr - HT_3I_BASE);
    if (nr >= HT_ALT_BASE && nr < HT_ALT_BASE + HT_ALT_N)
        return HT_FIXED_N + (int) (nr - HT_ALT_BASE);
    return -1;
}

// Handler slot value for a grate that has exited: calls fail with ESRCH
// instead of falling back to the default implementation (which would silently
// lift whatever restriction the grate enforced) or reaching a cage that later
// reuses the id.
#define HT_DEAD (((uint64_t) 0xffffffffu) << 32)

static inline uint64_t
ht_make(uint32_t grate, uint32_t off)
{
    return ((uint64_t) grate << 32) | off;
}

// ---------------------------------------------------------------------------
// Cages
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Signals
// ---------------------------------------------------------------------------

#define NSIG3I 64
#define SIGBIT(s) (1ULL << ((s) - 1))

// struct kernel_sigaction as passed to rt_sigaction on x86-64.
struct kact {
    uint64_t handler; // 0 = default, 1 = ignore, otherwise a sandbox address
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

enum CageState {
    CAGE_FREE = 0,
    CAGE_RUNNING,
    CAGE_EXITING, // terminating: no new grate calls are accepted
    CAGE_ZOMBIE,  // finished, waiting to be reaped by the parent
};

// An execution context inside a grate that serves handler calls. Each one has
// its own stack and thread-local storage inside the grate's sandbox, so grate
// calls from different host threads (or nested calls that re-enter the same
// grate) can run concurrently.
struct GCtx {
    struct GCtx *next_all;
    struct GCtx *next_free;
    struct LFILinuxThread *t;
    struct LFIContext *ctx;
    lfiptr req;       // request buffer in the grate's sandbox
    uint64_t idle_rsp; // stack pointer while idle
    bool busy;
};

struct Cage {
    struct Cage *pool_next;
    uint32_t id;
    uint32_t gen;
    int ppid;
    _Atomic int refs;
    _Atomic int state;

    // The program image of the cage. Replaced in place on exec (the sandbox
    // itself is kept and reset).
    struct LFILinuxProc *proc;
    // Surrogate thread used when operations on this cage are executed by
    // another cage's thread (a grate forwarding a system call).
    struct LFILinuxThread *kt;

    // Held for reading by anything that touches this cage's sandbox memory on
    // behalf of another cage; held for writing while the sandbox is torn down.
    pthread_rwlock_t memlk;

    // Signalled (with the engine's tree_lk held) when a child changes state.
    pthread_cond_t cv;
    struct Cage **children;
    size_t nchildren, cchildren;
    int exit_status; // wait(2) status
    int killsig;     // signal that terminated the cage (0 if none)
    pthread_t host_thread;
    bool host_thread_valid;

    // exec(2) handoff: the new main thread for the host thread to switch to.
    struct LFILinuxThread *next_thread;

    uint32_t umask;

    // Signals. Dispositions are shared by all threads of the cage; a signal
    // sent to the cage waits in 'pending' for a thread that does not block it.
    pthread_mutex_t siglk;
    struct kact act[NSIG3I + 1];
    _Atomic uint64_t pending;
    int sender[NSIG3I + 1]; // pid that last sent each pending signal
    struct LFILinuxThread *main_t; // main thread (for process-directed signals)
    bool itimer_on;
    timer_t itimer;

    // Grate side. Entry points announced by the guest shim library.
    uintptr_t g_entry; // handler dispatch stub
    uintptr_t g_spawn; // creates a new grate context
    _Atomic int g_init_budget; // CAPTURE requests allowed outside a runtime-driven spawn
    pthread_mutex_t g_lk;       // guards the context lists
    pthread_mutex_t g_spawn_lk; // serializes creation of new contexts
    struct GCtx *g_all;
    struct GCtx *g_free;
    struct GCtx *g_cloner;
    _Atomic uint64_t img; // program-image generation of the grate side: bumped when the cage
                          // exec's or exits, which frees its contexts
    _Atomic int inflight; // grate calls currently executing in this cage
    _Atomic bool is_grate; // other cages have handlers in this cage

    int nice;                // scheduling priority (getpriority/setpriority), inherited on fork
    _Atomic uint32_t alt_hw; // 1 + the highest alternate slot ever used
    _Atomic uint64_t htab[HT_N];
};

// Number of table slots that may be non-empty (everything past the fixed part
// that was never registered is known to be empty).
static inline size_t
ht_limit(struct Cage *c)
{
    return HT_FIXED_N + atomic_load_explicit(&c->alt_hw, memory_order_acquire);
}

struct Engine3i {
    struct LFIEngine *core;
    struct LFILinuxEngine *linux_;
    struct Lfi3iOptions opts;

    _Atomic(struct Cage *) *table;
    size_t table_n;
    // Sandbox base address of each live cage (by id), 0 if none. Lets a
    // signal handler tell sandbox code from runtime code exactly.
    _Atomic uint64_t *bases;
    pthread_mutex_t lk; // id allocation, free list, live count
    pthread_cond_t cv;  // signalled when live reaches zero
    // Protects the process tree (children lists, zombie state, wait()). The
    // per-cage condition variables are all used with this mutex.
    pthread_mutex_t tree_lk;
    uint32_t next_id;
    size_t live;
    uint8_t *id_used;  // ids that are allocated (until the cage is destroyed)
    struct Cage *free_head; // free cage structures (type-stable memory)
    bool shutting_down;
};

extern struct Engine3i *g_eng;

// ---------------------------------------------------------------------------
// Per-host-thread call frames
// ---------------------------------------------------------------------------

// One frame is pushed for every grate call in progress on a host thread. The
// frames let the runtime find the thread of a cage that is blocked inside a
// grate (for fork/exec/exit forwarded on that cage's behalf), check that a
// grate acts only for cages it is serving, and unwind a grate chain.
struct Frame {
    struct Frame *prev;
    struct Cage *caller; // cage that issued the call
    struct LFILinuxThread *caller_t;
    struct Cage *target; // cage the call is on behalf of
    struct Cage *grate;  // grate serving the call
    struct GCtx *gc;
    long nr;             // call number the grate was invoked for
    const uint64_t *a, *ac; // its arguments and their cage ids (valid for the call)
};

extern _Thread_local struct Frame *tl_frames;
// Context-spawn state. Creating a handler context means the guest's spawn function issues a
// clone that the runtime turns into a parked context (lfi_capture_clone). Context creation is a
// runtime operation, not application behaviour: that one clone skips the cage's handler table (a
// parent grate's clone handler would otherwise be asked to serve, and could refuse or count, the
// creation of a context it knows nothing about, and the grates' context pools would depend on
// each other). The exemption is host-owned and narrow:
//  * lfi_capture_clone is only set by grate_capture(), which accepts a request from the guest only
//    inside a spawn the runtime started (tl_spawn_auth) or against the small budget granted when
//    the shim installs its entry points (once per program image);
//  * only a call that arrived as clone/clone3 is exempt (fork, aliased to clone, is not);
//  * the next thread clone consumes the request, a process-creating clone clears it, and it is
//    dropped when the spawn function returns, at thread start and at exec;
//  * the new context may run only its start-up code and the pause call (LFILinuxThread.capturing),
//    so a cage that misuses a request to get an application thread has the cage terminated
//    instead of gaining a thread.
extern _Thread_local bool tl_spawn_auth;
void grate_spawn_state_reset(void);

// Set while this host thread copies between sandboxes; a fault in that window
// (the other sandbox unmapped the range concurrently) returns to the copy
// instead of crashing the runtime.
extern _Thread_local sigjmp_buf *tl_copy_jb;

// Set while this host thread runs a system call implementation that may dereference guest
// pointers (see threei_native_guarded): a fault in the runtime's own access to guest memory (a
// pointer into an unmapped or PROT_NONE page of the sandbox) returns -EFAULT to the guest
// instead of taking the whole runtime, and every other cage, down.
extern _Thread_local sigjmp_buf *tl_sys_jb;

// ---------------------------------------------------------------------------
// System call arguments
// ---------------------------------------------------------------------------

// A system call in flight. Each argument comes with the id of the cage whose
// address space (pointers) or descriptor table (file descriptors) it refers
// to. For calls a cage makes itself, all of them are the cage's own id.
struct Call {
    struct Cage *caller;       // cage that issued the call (owner of the handler table used)
    struct Cage *target;       // cage the call acts on
    struct LFILinuxThread *t;  // thread executing the call (target's or surrogate)
    long nr;
    uint64_t a[6];
    uint64_t ac[6];
    bool self;                 // all args belong to target
    struct Cage *xc[6];        // references taken on foreign cages
};

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------

// cage.c
struct Cage *cage_get(uint32_t id);
void cage_put(struct Cage *c);
struct Cage *cage_alloc(uint32_t ppid, struct LFILinuxProc *proc);
void cage_publish(struct Cage *c);
void cage_unpublish(struct Cage *c);
void cage_add_child(struct Cage *parent, struct Cage *child);
bool cage_is_descendant(struct Cage *c, struct Cage *ancestor);

// dispatch.c
void threei_sys_handler(struct LFIContext *ctx);
long threei_dispatch(struct Cage *self, long nr, struct Cage *target,
    struct LFILinuxThread *t, const uint64_t a[6], const uint64_t ac[6]);
long threei_native(struct Call *c);
long threei_native_guarded(struct Call *c);
long threei_make_call(struct Cage *caller, struct LFILinuxThread *t,
    lfiptr reqp);
void frames_unwind_through(struct Frame *f);
// References a host frame holds across a dispatch (the cages a forwarded call acts for). A
// forwarded exit/exec throws the host stack away, so the frames that would release them never
// run: they are therefore recorded here, tagged with the innermost grate frame at the time, and
// released by frames_unwind_through / call_unwind_for_exit.
void hold_push(struct Cage *c);
void hold_pop_put(void);
void call_unwind_for_exit(struct Call *c, struct Cage *target, struct LFILinuxThread *rt);
void frame_abort(struct Frame *f);
bool cage_authorized_for(struct Cage *caller, struct Cage *other);
struct Frame *frame_serving(struct Cage *c);

// grate.c
long grate_call(struct Cage *caller, struct LFILinuxThread *caller_t,
    struct Cage *target, uint32_t gid, uint32_t fnoff, long nr,
    const uint64_t a[6], const uint64_t ac[6]);
long grate_setentry(struct Cage *c, struct LFILinuxThread *t, lfiptr entry,
    lfiptr spawn);
long grate_collect(struct Cage *c, long spawn_result);
long grate_capture(struct Cage *c);
void grate_free_all(struct Cage *c);

// 3i API calls (nat3i.c)
long n3i_register_handler(struct Call *c);
long n3i_copy_data(struct Call *c);
long n3i_copy_htab(struct Call *c);

// call.c: argument access helpers
void call_init(struct Call *c, struct Cage *caller, struct Cage *target,
    struct LFILinuxThread *t, long nr, const uint64_t a[6], const uint64_t ac[6],
    bool self);
void call_done(struct Call *c);
struct Cage *call_cage(struct Call *c, int i);
struct LFIBox *call_box(struct Call *c, int i);
void *call_buf(struct Call *c, int i, size_t len, size_t align);
void *call_buf_at(struct Call *c, int i, uint64_t ptr, size_t len, size_t align);
void *call_buf_ro(struct Call *c, int i, size_t len);
char *call_path(struct Call *c, int i);
struct FDTable *call_fdt(struct Call *c, int i);
// The descriptor table of the cage the call acts on. For descriptors embedded
// in memory structures (poll's pollfd array, select's sets), whose memory may
// belong to another cage than the descriptors.
struct FDTable *call_target_fdt(struct Call *c);

// Converts dirents in cage 'owner's buffer from the getdents (78) layout to
// getdents64's (217); see sys_alias in dispatch.c.
void dirents_old_to_new(uint32_t owner, uint64_t addr, long n);

// nat.c (file/path/fd natives), proc3i.c (process natives)
long nat_file(struct Call *c, bool *handled);
long nat_proc(struct Call *c, bool *handled);
bool nat_net(struct Call *c, long *ret);
bool nat_misc(struct Call *c, long *ret);

// proc3i.c
int cage_run_main(struct Cage *c, struct LFILinuxThread *t, uintptr_t entry);
struct Cage *cage_create_root(struct Engine3i *e);
long nat_clone(struct Call *c);
long cage_fork(struct Call *c, uint64_t flags, uint64_t stack);
long cage_exec(struct Call *c);
long cage_wait(struct Call *c);
long cage_exit(struct Call *c, bool group);
long cage_kill(struct Call *c);
int cage_finish(struct Cage *c, int code);
void cage_terminate_all(struct Cage *except);
void cage_terminate(struct Cage *v, int sig);
void cage_reap(struct Cage *parent, struct Cage *child);

// shm.c: MAP_SHARED support
long n_mmap(struct Call *c);
long n_munmap(struct Call *c);
long n_mprotect(struct Call *c);
void shm_close_all(struct LFILinuxProc *p);
int shm_clone_fd(int fd, void *udata);

// signal.c
bool nat_signal(struct Call *c, long *ret);
// Delivers a pending signal (building a handler frame on the thread's stack)
// when a system call is about to return to the guest. 'res' is the result of
// the call; the return value is what the guest sees in rax.
long sig_deliver(struct Cage *c, struct LFILinuxThread *t, struct LFIRegs *r,
    long nr, const uint64_t a[6], long res);
// Sends a signal to a cage, or to a thread of it (th != NULL).
void sig_send(struct Cage *v, struct LFILinuxThread *th, int sig, int sender);
// Interrupts the cage's host threads so blocked system calls return EINTR.
void sig_kick(struct Cage *v);
void sig_cage_init(struct Cage *c);
void sig_cage_reset_for_exec(struct Cage *c);
void sig_cage_fini(struct Cage *c);

static inline bool
sig_pending_for(struct Cage *c, struct LFILinuxThread *t)
{
    uint64_t p = atomic_load_explicit(&t->sigpending, memory_order_relaxed) |
        atomic_load_explicit(&c->pending, memory_order_relaxed);
    return (p & ~t->sigmask) != 0;
}

#define LINUX_ERR(e) (-(long) (e))
