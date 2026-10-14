# Known issues and limitations (`v0.1.0-experimental`)

This release is for **controlled research workloads**. It is not a hardened sandbox for
untrusted code and does not claim Lind-Wasm's security properties. The items below are
what is known; the absence of an item is not a guarantee. Verification of cage code is on by
default; `--no-verify` and `--permissive` disable protections and are unsafe.

Evidence: **defect** = found by running code or by reading it and reproduced where noted;
**by inspection** = read from the source, not exercised by a test; **risk** = an unresolved
question, no defect shown. Status: **open**, **mitigated** (reduced, not eliminated), **fixed**
(listed only where a limitation was previously documented).

## Isolation and authorization

| Issue | Boundary / workload | Current restriction | Evidence, status |
|---|---|---|---|
| **File-system confinement through symlinks.** Paths are mapped and `/proc`, `/sys` are excluded *lexically*, but the host kernel follows symlinks. A cage that can create a symlink in a writable mapped directory (to `/proc/self/mem`, say) can reach the runtime's own `/proc` entries. | Any cage given a writable `--dir` mapping | Do not run untrusted code with writable mapped directories; run under a user and root with nothing to lose | by inspection, **open** |
| **Argument-owner authorization.** The cage id attached to an argument is not checked against the caller's rights (`call_cage`), so a cage that knows a valid address or descriptor number can name another cage's memory or descriptor table in an ordinary call. Explicit `copy_data_between_cages` is authorized. | Hostile cages in the same runtime | Cages in one runtime must be mutually trusted with respect to guessable addresses | by inspection, **open** |
| **Incomplete per-argument attribution.** Several terminal implementations (`nanosleep`, `clock_gettime`, `getrandom`, `uname`, `futex`, some resource/time calls) resolve pointers against the *target* cage only, ignoring the argument's owner; a `dirfd` owned by another cage is looked up in the target's table; file-backed `mmap` uses the target's descriptor table. A grate that forwards such a call with a buffer in its own memory gets `EFAULT`. | Grates that substitute buffers/descriptors for calls outside the tested set | Tested forwarding cases are listed in `threei/tests` and `threei/grates/tests.list` | defect (by inspection), **open** |
| **Handler identity and lifetime races.** A handler entry stores a cage id and code offset, not a lifetime. A dispatcher that loaded an entry just before the grate exited and its id was reused calls the new cage; table copy/fork inheritance can race a grate's death scrub; table overwrite is a loop of stores, so a concurrent call may see a mixture of old and new rules. Entries for exited or exec'd grates are tombstoned, which covers the sequential cases. | Repeated concurrent grate start/exit/exec while other cages call it | Avoid racing grate teardown with calls to it | by inspection, **open** |
| **Executable backing is not shown immutable.** The ELF text is mapped privately from the file and verified; the verifier checks the mapped bytes, but nothing here proves those pages cannot change through the backing file or another mapping after verification. | Hostile code with write access to its own program file | Do not make cage binaries writable by cages | **risk**, not reproduced |
| **Policy grates can name any handler in their grate's tree.** `seccomp-grate` freezes its subject's table, forbids emptying or replacing protected slots except by a handler inside the subject's subtree, and kills the cage if its own installation fails; the runtime also holds a forwarded `register_handler` to the originating cage's rights. A subject can still name a sibling subtree's cage (when the grate has several children) as a handler for its descendants, and handler offsets are arbitrary 32-byte-aligned code addresses of the named cage. | Multi-child policy chains | Give a policy grate one subject | **mitigated** |
| **CAPTURE.** Context creation is a runtime operation outside the cage's `clone` policy: a parent grate cannot count, trace or refuse it. A cage that installs the shim's entry points obtains two capture requests; the "context" they create may run only its start-up calls and the pause call, otherwise the cage is ended. A policy that denies `clone` does not prevent a cage from becoming a grate. | Resource-limiting policies | Thread limits count application threads, not grate contexts | **mitigated** by design |
| **Faults while the runtime holds a lock.** A fault in the runtime's access to guest memory returns `EFAULT` by jumping out of the call (a recovery point per call that takes pointers). A native implementation that touches guest memory while holding a lock would skip the unlock. `getcwd` did and was fixed; other implementations were read for it, not proven free of it. The recovery pointer is cleared at thread start and exec; paths that discard host frames in other ways were not all audited. | Hostile pointers to unmapped/`PROT_NONE` pages | — | **mitigated** (recovery-pointer lifetime is a **risk**) |
| **Faults during signal delivery.** Faults while the runtime builds a guest signal frame or restores one (`rt_sigreturn`) on a guest stack were not exercised; synchronous faults in guest code always terminate the cage. | Cages that corrupt their signal stack | — | **risk**, not analyzed |
| **Exact-copy semantics.** `copy_data_between_cages` copies the readable prefix of a range that runs into an inaccessible page and zero-fills the rest (Lind grates copy fixed-size buffers from contiguous Wasm memory), instead of failing. A range that starts inaccessible fails. | Grates that rely on exact failure | — | by design, **open** vs Lind |
| **Out-of-memory bookkeeping.** If recording a held cage reference fails to allocate, the reference is kept alive (the cage id leaks) and the matching release pops a different entry. | Memory exhaustion in the runtime | — | defect (by inspection), **open** |
| **Exec of an active grate.** Exec waits up to about 10 s for calls in progress to finish; if they do not, the old contexts are leaked rather than freed. Calls to the old image fail until the new image registers again. | Exec of a grate under load | — | **mitigated** |

## Process model

| Issue | Restriction | Status |
|---|---|---|
| `fork` relocates pointers conservatively: any aligned 64-bit data word whose upper half equals the parent's sandbox base is rewritten, which can change non-pointer data; unaligned/packed pointers and pointer-valued SIMD registers are not relocated; `MAP_SHARED` mappings stay shared and unrelocated. | Programs whose data may contain sandbox-looking words | open |
| `vfork` (`CLONE_VM` + `CLONE_VFORK`) is a normal `fork`: the parent is not suspended and exec failure is not reported through shared memory (`posix_spawn` of a missing program can misreport). Other `CLONE_VM` without `VFORK` is rejected. | Spawn-heavy programs | open |
| A forked cage cannot serve as a grate until it execs: the child has no entry points or contexts. | Fork a cage, then `exec` the grate | open |
| Signals: handlers run at the next system-call return (a spinning thread does not see a *handled* signal; a terminating one stops it). `fork` inside a returning handler leaves the child without its saved signal frame (its `rt_sigreturn` kills it); alternate-stack state is not inherited; `ppoll`/`pselect6`/`epoll_pwait` ignore their temporary masks; `exec` from a multi-threaded cage returns `ENOSYS`; no process groups, job control, `ITIMER_VIRTUAL/PROF`. | Signal-heavy programs (e.g. databases that fork in handlers) | open |
| Handler results are C `int`s: large `lseek`/`pread` results through a grate are truncated; pointer results come back as 32-bit offsets. | As in Lind | by design |
| No `SCM_RIGHTS`; `MAP_SHARED` executable mappings are rejected; 256 cages by default, 8 GiB of address space each. | — | by design |

## Grates and tests

| Issue | Status |
|---|---|
| **mTLS and tee are not validated.** `mtls-grate` is only load-tested (`--help`, reported separately as a load-only check). Its session table lock is held across blocking TLS I/O, so a client and server served concurrently by it can deadlock (inherited from the original grate). There is no tee/redaction grate or test in the ported set. | open |
| The test suites are regression tests for the behaviours and the defects they target; they are not a complete conformance suite. | — |
| `clamp` grates fail closed when their own installation fails (tested by injecting registration failures); the injection tests cover the example chains only. | mitigated |
| Rust grates are built for LFI with a nightly toolchain (`-Zbuild-std`), local-exec TLS, `panic=abort`, no libunwind/AVX; the `fdtables` and `grate-rs` crates are patched copies. | — |
| The test harness propagates the cage's exit status and signals for the C and Rust example grates, but the many Rust grate test *programs* each have their own checks, which were not audited one by one. | risk |

## Toolchain and build

* **Verifier patch.** The prebuilt LFI toolchain (v0.12) restores the stack pointer from memory
  (`alloca`, variable-length arrays, `longjmp`) with `mov esp, gs:[...]; lea rsp, [rsp+r14]`, which
  the pinned verifier rejects. `lfi-runtime/subprojects/packagefiles/lfi-verifier-modsp.patch`
  narrowly allows exactly the `%gs`-relative 32-bit-address source. Drop it when the toolchain
  emits a register source. Still rejected: `rep bsf`/`tzcnt` with a `%gs` memory operand (worked
  around in `gawk`), and a bare unsandboxed `rep movsq` the compiler emits for very large
  by-value struct arguments (worked around in SPEC `gcc_r`).
* The compiler bundle needs glibc ≥ 2.38; `scripts/bootstrap.sh` provides a private copy on older
  hosts by patching the bundle's executables inside the checkout.
* x86-64 Linux only. The aarch64 and RISC-V parts of upstream LFI are not covered by the 3i layer.

## Performance claims

Measured on one machine (Intel i9-12900K) with single-run micro-benchmarks that vary by
10-50% between runs, and SPEC CPU2017 `test`/`train` inputs. Comparisons with the Lind paper mix
different hardware, libc and isolation technology; they are not equivalent-policy comparisons.
Allocation-heavy programs are slower under LFI mostly because of musl's `malloc`, not the sandbox
(measured with a micro-benchmark, not confirmed on the SPEC binaries). `exec` and process start-up
cost about 12 ms per MB of binary for verification; there is no verification cache.
