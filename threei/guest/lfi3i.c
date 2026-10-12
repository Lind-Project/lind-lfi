// Guest-side runtime support for grates (see lfi_3i_guest.h).
//
// Besides the API wrappers this file provides the machinery the host runtime
// uses to enter this program as a grate: an entry stub that unpacks a request
// and calls the handler, and a way to create the execution contexts that run
// handlers. A context is a thread of this program that is created normally
// with pthread_create (so it gets a stack and thread-local storage from the C
// library) but is never scheduled: the runtime captures its saved state when
// it parks itself, and later runs handlers on it directly from whichever host
// thread is making the call.
#include "lfi_3i_guest.h"

#include <errno.h>
#include <pthread.h>

#define SYS_LFI_PAUSE 1024

static inline long
raw0(long n)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n) : "rcx", "r11", "memory");
    return r;
}

static inline long
raw1(long n, long a)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a) : "rcx", "r11",
        "memory");
    return r;
}

static inline long
raw2(long n, long a, long b)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "rcx",
        "r11", "memory");
    return r;
}

// The calling cage's id. Unlike getpid() this cannot be interposed on, so the
// API wrappers keep working however the cage's own table is configured.
static inline uint64_t
self_id(void)
{
    return (uint64_t) raw0(LFI3I_SYS_SELF);
}

int
make_threei_call(unsigned int callnumber, uint64_t callname,
    uint64_t self_cageid, uint64_t target_cageid,
    uint64_t arg1, uint64_t arg1cageid,
    uint64_t arg2, uint64_t arg2cageid,
    uint64_t arg3, uint64_t arg3cageid,
    uint64_t arg4, uint64_t arg4cageid,
    uint64_t arg5, uint64_t arg5cageid,
    uint64_t arg6, uint64_t arg6cageid,
    int translate_errno)
{
    struct lfi3i_call rq __attribute__((aligned(8))) = {
        .callnum = callnumber,
        .callname = callname,
        .self = self_cageid,
        .target = target_cageid,
        .arg = { arg1, arg2, arg3, arg4, arg5, arg6 },
        .argcage = { arg1cageid, arg2cageid, arg3cageid, arg4cageid,
            arg5cageid, arg6cageid },
    };
    long ret = raw1(LFI3I_SYS_CALL, (long) &rq);
    if (translate_errno && ret < 0 && ret > -4096) {
        errno = (int) -ret;
        return -1;
    }
    return (int) ret;
}

int
register_handler(uint64_t targetcage, uint64_t targetcallnum,
    uint64_t this_grate_id, uint64_t in_grate_fn_ptr_u64)
{
    // Slot layout shared with Lind: arg1 = target cage, arg1cage = call
    // number, arg2cage = grate, arg3 = handler address. The call is made as
    // the calling cage itself (which may be a child registering the grate that
    // created it).
    uint64_t me = self_id();
    return make_threei_call(LFI3I_SYS_REGISTER, UNUSED_NAME, me, me,
        targetcage, targetcallnum, UNUSED_ARG, this_grate_id,
        in_grate_fn_ptr_u64, UNUSED_ID, UNUSED_ARG, UNUSED_ID, UNUSED_ARG,
        UNUSED_ID, UNUSED_ARG, UNUSED_ID, 0);
}

int
copy_data_between_cages(uint64_t thiscage, uint64_t targetcage,
    uint64_t srcaddr, uint64_t srccage, uint64_t destaddr, uint64_t destcage,
    uint64_t len, uint64_t copytype)
{
    (void) targetcage;
    (void) thiscage;
    uint64_t me = self_id();
    return make_threei_call(LFI3I_SYS_COPY_DATA, UNUSED_NAME, me, me, srcaddr, srccage, destaddr, destcage, len, UNUSED_ID,
        copytype, UNUSED_ID, UNUSED_ARG, UNUSED_ID, UNUSED_ARG, UNUSED_ID, 0);
}

int
copy_handler_table_to_cage(uint64_t srccage, uint64_t targetcage)
{
    uint64_t me = self_id();
    return make_threei_call(LFI3I_SYS_COPY_HTAB, UNUSED_NAME, me, me,
        srccage, targetcage, UNUSED_ARG, UNUSED_ID, UNUSED_ARG,
        UNUSED_ID, UNUSED_ARG, UNUSED_ID, UNUSED_ARG, UNUSED_ID, UNUSED_ARG,
        UNUSED_ID, 0);
}

// ---------------------------------------------------------------------------
// Grate entry
// ---------------------------------------------------------------------------

typedef int (*handler_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t);

// Called by the runtime, on one of this program's contexts, with a request
// that holds the handler address and its arguments.
__attribute__((used, noinline)) long
__lfi3i_entry(struct lfi3i_req *r)
{
    handler_t h = (handler_t) (uintptr_t) r->fn;
    return h(r->cageid, r->arg[0], r->argcage[0], r->arg[1], r->argcage[1],
        r->arg[2], r->argcage[2], r->arg[3], r->argcage[3], r->arg[4],
        r->argcage[4], r->arg[5], r->argcage[5]);
}

static void *
ctx_idle(void *arg)
{
    (void) arg;
    // Park: the runtime captures this context and resumes it only to run
    // handler calls.
    raw0(SYS_LFI_PAUSE);
    for (;;)
        __builtin_trap();
}

// A context is a thread to the C library, but it never runs on its own and
// must not be treated as one: functions that broadcast to every thread (musl's
// setuid, setgroups, setrlimit, ...) would signal it and wait for an answer
// that cannot come. So once it exists it is removed from the C library's list
// of threads, keeping the "is this process multi-threaded" locking state (the
// contexts do run concurrently with each other and the program) as it is.
//
// This relies on musl's layout of its thread list; if the symbols are not
// there (a different libc) nothing is done.
struct td_head {
    struct td_head *self;
    void *dtv;
    struct td_head *prev, *next;
};
extern struct {
    char can_do_threads, threaded, secure;
    volatile signed char need_locks;
    int threads_minus_1;
} __libc __attribute__((weak));
extern void __tl_lock(void) __attribute__((weak));
extern void __tl_unlock(void) __attribute__((weak));

static void
hide_from_libc(pthread_t th)
{
    if (!__tl_lock || !__tl_unlock || !&__libc)
        return;
    struct td_head *td = (struct td_head *) th;
    __tl_lock();
    if (td && td->next && td->prev && td->next != td) {
        td->prev->next = td->next;
        td->next->prev = td->prev;
        td->next = td->prev = td;
        __libc.threads_minus_1--;
    }
    __tl_unlock();
}

// Creates one more handler context. Also called by the runtime (on a
// reserved context) when a host thread needs a context and none is free.
__attribute__((used, noinline)) long
__lfi3i_spawn(void)
{
    raw0(LFI3I_SYS_CAPTURE);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 1 << 20);
    pthread_t th;
    int r = pthread_create(&th, &at, ctx_idle, 0);
    pthread_attr_destroy(&at);
    if (r == 0)
        hide_from_libc(th);
    return raw1(LFI3I_SYS_COLLECT, r);
}

__attribute__((constructor)) static void
lfi3i_init(void)
{
    if (raw2(LFI3I_SYS_SETENTRY, (long) __lfi3i_entry, (long) __lfi3i_spawn) != 0)
        return;
    // One context reserved for creating more, one to serve the first calls.
    __lfi3i_spawn();
    __lfi3i_spawn();
}
