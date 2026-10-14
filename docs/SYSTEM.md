# System description: cages, grates and 3i on LFI

This describes what the code in this repository does at `v0.1.0-experimental`. Where a
property is not guaranteed it says so; the list of known defects is
[`KNOWN_ISSUES.md`](KNOWN_ISSUES.md). Source references are relative to `lfi-runtime/threei/`.

## 1. Model

**Cage.** A running program with its own LFI sandbox (4 GiB), descriptor table, working
directory, `umask`, signal state, handler table and place in the process tree. The cage id
is the process id (`getpid()`, `fork()`, `wait()`).

**Handler table.** Every cage has a table from a system-call number to either the default
implementation (empty entry) or a *handler*: a function in another cage. `register_handler`
fills entries; `fork` copies the table to the child; `exec` keeps it. The table covers Linux
numbers 0-511, the 3i calls 1000-1007, and *alternate numbers* 1008-9199 that grates
allocate as private routes (a grate registers a handler under an unused number in some table
and later calls that number explicitly).

**Grate.** A cage that other cages' handlers live in. Any program that is named as the
handler owner in `register_handler` is a grate; a typical one forks, registers handlers for
the child, and `exec`s the program to interpose on. Grates stack (the interposed program may
itself be a grate) and a handler may answer, deny, modify or forward a call.

**Argument ownership.** Each system-call argument travels as `(value, cageid)`. For a pointer
the cage id names the address space it points into; for a descriptor it names the descriptor
table it indexes. A handler that forwards a call keeps the pairs, so the default
implementation reads the right memory and descriptors even when the arguments of one call
belong to different cages. This is what lets a grate substitute a buffer in its own memory
while the descriptor belongs to the cage.

**Target versus owner.** `make_threei_call(nr, ..., self, target, args...)` looks `nr` up in
*self*'s table (so the call reaches the layer below `self`, or the default implementation)
and acts on behalf of *target* (its process state, descriptors, address space). The per-argument
cage ids say where each argument lives.

## 2. API

Declared in `threei/guest/include/lfi_3i_guest.h` (also installed as `lind_syscall.h`); link
`threei/guest/lfi3i.c` into any program that uses it. The API matches Lind's.

| Function | Meaning |
|---|---|
| `register_handler(targetcage, callnum, grate, fn)` | `targetcage`'s call `callnum` is handled by `fn` in cage `grate`. `grate == 500` restores the default. 0 on success. |
| `make_threei_call(nr, name, self, target, a1,c1, ..., a6,c6, translate_errno)` | Perform `nr` through `self`'s table on behalf of `target`. Without `translate_errno` the raw (negative errno) result is returned. |
| `copy_data_between_cages(this, target, srcaddr, srccage, destaddr, destcage, len, type)` | Copy `len` bytes (`LFI3I_COPY_MEMCPY`) or a NUL-terminated string of at most `len` bytes (`LFI3I_COPY_STRNCPY`) between two cages. Returns `destaddr` (truncated to `int`) or `ELINDAPIABORTED` for every kind of failure. |
| `copy_handler_table_to_cage(srccage, targetcage)` | Replace `targetcage`'s table with a copy of `srccage`'s. |
| `lfi3i_copy_data_long`, `lfi3i_copy_htab_long` | Full-width results of the two copy calls; used by the Rust library to tell a failure from a destination address whose low 32 bits resemble one. |

`register_handler`, `copy_data_between_cages` and `copy_handler_table_to_cage` are system calls
1001-1003 (`make_threei_call` is 1000 and is not routed through the table), so a grate can
interpose on the routing operations themselves (this is how a policy grate controls what its subject may
register; see `grates/c/seccomp-grate`). 1010-1013 (`SETENTRY`, `CAPTURE`, `COLLECT`,
`SELF`) are private to the guest shim.

A handler has the signature `int h(uint64_t grate_id, uint64_t a1, uint64_t a1cage, ... uint64_t a6, uint64_t a6cage)`
and returns an `int` (a result or a negative errno). Calls that return pointers (`mmap`,
`brk`) come back through a grate as the low 32 bits (a sandbox offset); the runtime turns it
back into a pointer. Arguments beyond a call's arity are zeroed before a handler sees them
(`sysarity.h`), as Lind's library does.

Authorization (without `--permissive`): a cage may register handlers for itself and its
descendants; the handler's grate must be the caller, an ancestor of the caller, or a
descendant. A cage may act on (copy memory of, signal) itself, its descendants, and cages its
grate calls are currently serving. A grate that forwards, unchanged, the very
`register_handler`/`copy_handler_table_to_cage` call it is serving is additionally held to the
rights of the cage that made it. *Argument* cage ids are not authorized (see known issues).

## 3. How it works

### 3.1 Dispatch and entering a grate

A cage's system call enters `threei_sys_handler` (`dispatch.c`): it checks the cage is not
being terminated, indexes the handler table, and either runs the default implementation
(`nat*.c`) or enters the grate (`grate.c`). A grate is entered **in place** on the calling
thread with LFI's `LFI_INVOKE` trampoline: the grate's stack and sandbox base are loaded and
the handler runs on one of the grate's *execution contexts*; when it returns, the same thread
continues in the cage's return path. No other thread is woken and the fast path takes no lock.

### 3.2 Execution contexts

A grate needs one context per concurrently running handler call. Contexts are created lazily by
the guest shim: a context is a thread of the grate created with the C library's
`pthread_create`, but the runtime *captures* it instead of scheduling it (`lfi_capture_clone`
in `linux/sys/sys_clone.c`): the thread runs its start-up code up to LFI's pause call, and its
saved registers become the context. The capture request is host-owned: it is accepted only
inside a spawn the runtime started, or against a budget of two requests granted when the shim
installs its entry points (once per program image); it applies only to a thread-creating
`clone` and is otherwise disarmed; and a captured context may run only its start-up system
calls and the pause call, otherwise the cage is terminated. Context creation is a runtime
operation and is **not** subject to the cage's `clone` handler. Host threads cache the last
context used per grate; entries carry the grate's image generation, which exec and exit bump.
Teardown refuses new calls, waits (up to ~10 s) for calls in progress, and leaks rather than
frees the contexts if they do not finish.

### 3.3 Processes

* `fork` allocates a sandbox and copies the parent's mappings into it (`lfi_box_clone`); pointers
  are absolute addresses, so every aligned 64-bit word whose upper half equals the parent's
  sandbox base is rewritten to point into the child. The descriptor table is duplicated by
  reference counting host descriptors. Only the calling thread is forked.
* `exec` resets the sandbox in place, verifies and loads the new program, closes close-on-exec
  descriptors, keeps the handler table and the cage id, and switches to the new main thread.
  Handlers other cages registered into the old image of an exec'ing grate are tombstoned (calls
  to them fail with `ESRCH`) until the new image registers again.
* `exit`, `wait`, zombies and reparenting to the init cage follow POSIX. A cage that dies
  without making an exit call (signal, fault) causes its grate's `exit_group` handler to be
  called once with the death status while its memory still exists (Lind's `harsh_cage_exit`).
* The references a forwarded `exit`/`exec` holds on the cages involved are recorded per host
  thread and released when the host frames are discarded, so cage ids are recycled.

### 3.4 Descriptors

A per-cage table maps small integers to reference-counted host descriptors, so `fork`, `dup`,
`pipe`, close-on-exec and use from several threads or from a grate on the cage's behalf behave
like POSIX. Handlers may hand out their own descriptor numbers above the real range (1024+)
and intercept `read`/`write`/`close` on them (the in-memory file system grate does).

### 3.5 Signals

Dispositions are per cage, masks per thread. A signal is delivered when a thread returns to
the guest from a system call: the runtime builds the kernel's frame on the thread's stack or
alternate stack and resumes at the handler. Signals whose action terminates the cage stop a
thread that is running guest code immediately (`SIGUSR1` to the host thread). Faults in guest
code terminate the cage with `SIGSEGV`/`SIGILL`; they cannot be caught.

### 3.6 Faults in the runtime's own accesses to guest memory

Terminal implementations that take pointers run under a recovery point: if the runtime touches
a page of the sandbox that is not accessible (a `PROT_NONE` page or an unmapped hole), the call
returns `EFAULT` and other cages are unaffected. `copy_data_between_cages` keeps all of its guest
accesses inside its own recovery region.

### 3.7 System-call coverage

Files, directories, links, permissions, timestamps (including `*at` forms), descriptors,
`mmap`/`brk`, `fork`/`vfork`/`clone` (threads and spawns)/`execve`/`wait4`/`exit`/`kill`, signals,
timers, futexes, sockets (without ancillary data, so no `SCM_RIGHTS`), `poll`/`select`/`epoll`,
`eventfd`, `memfd_create`, identity and limits. Anything else returns `ENOSYS`.
`MAP_SHARED` is supported if never executable.

## 4. Writing a grate

```c
#include "lind_syscall.h"
int geteuid_handler(uint64_t grate, uint64_t a1, uint64_t c1, uint64_t a2, uint64_t c2,
                    uint64_t a3, uint64_t c3, uint64_t a4, uint64_t c4,
                    uint64_t a5, uint64_t c5, uint64_t a6, uint64_t c6) { return 10; }

int main(int argc, char **argv) {
    int grate = getpid();
    if (fork() == 0) {                       // the cage
        register_handler(getpid(), 107 /* geteuid */, grate, (uint64_t) &geteuid_handler);
        execv(argv[1], &argv[1]);
        _exit(127);
    }
    int st; wait(&st);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);   // report the cage's status
}
```

Build with `x86_64_lfi-linux-musl-clang -O2 -static-pie -I threei/include -I threei/guest/include
prog.c threei/guest/lfi3i.c`; run `lfi-3i-run ./grate ./cage`. To forward a call,
`make_threei_call(nr, 0, getpid(), <cage the call came from>, a1, c1, ..., 0)` with the incoming
pairs. A grate that launches its cage should return the cage's exit status, as above; the
C example grates do. Rust grates use `grates/lib/grate-rs` and are built by
`grates/rust/cargo-lfi.sh` (a linker wrapper that recompiles LLVM bitcode with the LFI clang).

Composition patterns in `threei/grates/`: *policy* grates (`seccomp-grate`, `strace-grate`),
a *file-system* grate (`imfs-grate`), *clamps* (`fs-routing-clamp`, `net-routing-clamp`: calls
for a path or port prefix are routed to the grates inside a `%{ ... %}` block, the rest to the
layer below), and *stacking* (`demos/`). A policy grate must itself intercept
`register_handler` and `copy_handler_table_to_cage`, otherwise its subject can remove the policy:
`seccomp-grate` does (the subject's own table is frozen, descendants may re-point a protected slot
only inside the subtree, installation failure kills the cage).

## 5. Differences from Lind-Wasm

| | Lind-Wasm | This system |
|---|---|---|
| Isolation | WebAssembly (Wasmtime) | native x86-64 code under LFI SFI; verified before it runs |
| Grate call | re-enter a Wasmtime instance on a pooled worker | in-place sandbox transition on the calling thread |
| `fork` | Asyncify unwind/rewind | copy of the sandbox with conservative pointer relocation |
| Pointers in handlers | 32-bit offsets | 64-bit sandbox addresses; `int` results of pointer-returning calls are offsets |
| libc | patched glibc | stock LFI musl |
| `fork()` as syscall | `clone` (56) | 57; a handler for `clone` also receives `fork`/`vfork` (aliased), `epoll_wait` serves `epoll_pwait`, `stat`/`lstat` serve `newfstatat(AT_FDCWD, ...)` |
| `clone` flags to a handler | pointer to `struct clone_args` | the native flags value itself in `arg1` (`grate_rs::clone_flags`) |
| Call numbers in a table | Linux, 1001-1003 | Linux, 1000-1007, alternate 1008-9199 |
| Signals | epoch checks in the program | at syscall return; terminating signals stop spinning threads |
| Registration | any cage can name any cage | restricted (section 2) |
| Process/descriptor limits | per Wasmtime | 256 live cages by default (`--max-cages`), each reserving 8 GiB of address space |

## 6. Testing

`threei/tests` (49 cases: process and descriptor semantics, signals, authorization, grates,
CAPTURE misuse, lifetime and fault containment, Lind's ported grate tests), `threei/grates`
(62 cases plus one load-only check), and the runtime's own meson suite (58, including the 3i
suite). These establish that the covered behaviours work and that the listed defects they target
are fixed; they do not establish complete compatibility with Lind or hostile-code safety.
Exit status and signals of the cage are propagated by the example grates and checked by tests.
