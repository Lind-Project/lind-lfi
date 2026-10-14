# lind-lfi — Lind's 3i / grate model on LFI (experimental)

**Status: `v0.1.0-experimental`, a research migration.** It runs selected workloads
(listed below) and is meant for **controlled research use**. It is **not** a hardened
sandbox for hostile code and makes no claim of security equivalence with Lind-Wasm; see
[`docs/KNOWN_ISSUES.md`](docs/KNOWN_ISSUES.md) before relying on it for anything.

## What this is

[LFI](https://github.com/lfi-project/lfi) (Lightweight Fault Isolation) sandboxes native
x86-64 code in-process. [Lind](https://github.com/Lind-Project/lind-wasm) provides a
POSIX-like multi-process environment in which a *cage* can act as a *grate* and intercept
the system calls of other cages (the "3i" interposition interface). This repository runs
the Lind programming model on LFI instead of WebAssembly:

* programs run as cages in LFI sandboxes with `fork`/`exec`/`wait`, per-cage descriptor
  tables, pipes, signals, sockets;
* the Lind grate API (`register_handler`, `make_threei_call`, `copy_data_between_cages`,
  `copy_handler_table_to_cage`) is unchanged, so Lind's example grates (strace, seccomp,
  imfs, fs/net routing clamps, ipc, ...) are ported here by recompiling them;
* a call into a grate is an in-place sandbox transition on the calling thread.

How it works and how it differs from Lind: [`docs/SYSTEM.md`](docs/SYSTEM.md).

## Repository map

| Path | What |
|---|---|
| `lfi-runtime/` | The LFI runtime (imported from `lfi-project/lfi-runtime`, history preserved) **plus** the 3i layer in `lfi-runtime/threei/` |
| `lfi-runtime/threei/` | Cage/grate runtime, guest shim (`guest/lfi3i.c`), regression tests (`tests/`), benchmarks (`bench/`) |
| `lfi-runtime/threei/grates/` | Example grates: 5 in C, 18 in Rust, 5 demos, and the harness that runs them |
| `lfi-runtime/tools/lfi-3i-run/` | **The Threei runner** (`lfi-3i-run`) |
| `lfi-runtime/tools/lfi-run/` | Upstream LFI's plain runner (`lfi-run`): no cages, no grates |
| `scripts/` | `bootstrap.sh` (fetch the pinned toolchain), `env.sh` |
| `docs/` | `SYSTEM.md`, `KNOWN_ISSUES.md`, plus upstream LFI's guides (`BENCHMARKING.md`, `DETAILS.md`, `LIBLFI.md`, `LIBRARIES.md`) |
| `specinvoke/`, `examples/` | Upstream LFI |

Related repositories (separate): applications built for this runtime — branch `lfi-threei`
of [`lind-wasm-apps`](https://github.com/Lind-Project/lind-wasm-apps); SPEC CPU2017 comparison
scripts — `lind-lfi-spec`.

## Supported platform

x86-64 Linux only. The runtime and the guest shim assume the x86-64 LFI ABI. Tested on
Ubuntu 22.04 (glibc 2.35; the bootstrap script handles the prebuilt compiler's glibc ≥ 2.38
requirement without touching the system). Needs `gcc`, `python3`, `curl`, `tar`,
`sha256sum`, and `rustup` for the Rust grates.

## Build and test

```
git clone <this repository> lind-lfi && cd lind-lfi
scripts/bootstrap.sh            # LFI clang v0.12 (checksum-pinned), meson/ninja/patchelf, Rust nightly-2026-07-16
source scripts/env.sh           # puts the tools and the LFI compilers on PATH

cd lfi-runtime
meson setup build && ninja -C build               # runtime, lfi-run, lfi-3i-run
(cd threei/tests  && make check)                  # runtime/3i regression tests (49)
(cd threei/grates && make check)                  # example grates, C and Rust (62 + 1 load-only)
meson test -C build                               # upstream runtime tests + the 3i suite (58)
```

Everything is fetched into the checkout (`.toolchain/`, `.tools/`, `.cache/`); nothing is
installed system-wide. The build fetches `lfi-verifier`, `fadec`, `libelf` and `minialloc`
at the commits pinned in `lfi-runtime/subprojects/*.wrap`; `lfi-verifier` is patched
(`subprojects/packagefiles/lfi-verifier-modsp.patch`, explained in `docs/KNOWN_ISSUES.md`).
The Rust grates also download their crates.io dependencies (versions in `Cargo.lock`).

Verification of cage code is on by default in `lfi-3i-run`. `--no-verify` and `--permissive`
exist for experiments and are unsafe.

## A grate in two minutes

```
source scripts/env.sh
cd lfi-runtime/threei/grates && make out/geteuid-grate out/geteuid-test
../../build/tools/lfi-3i-run/lfi-3i-run out/geteuid-grate out/geteuid-test
```

`geteuid-grate` forks a cage, registers a handler that answers `geteuid` with a fixed
value, and runs `geteuid-test` as the cage; the test prints `geteuid ret = 10`. Other grates
and the Rust build recipe are described in `lfi-runtime/threei/grates/Makefile` and
`rust/cargo-lfi.sh`; writing a grate is covered in `docs/SYSTEM.md`.

## Tools: which runner?

* `lfi-run` (upstream) loads and runs one LFI program. No process model, no grates.
* `lfi-3i-run` (this project) runs its program as cage 1 with the full cage/grate model.
  Use `-r --dir=/=<root> --wd=/` for a confined file-system view; the default maps the
  whole host file system and is meant for experiments.

## Performance

Micro-benchmarks and compute kernels are in `lfi-runtime/threei/bench/` (`run_all.sh`;
failed samples are reported and fail the script). A grate call costs roughly 50-100 ns on
top of an in-runtime system call on the development machine (i9-12900K); numbers vary 10-50%
between runs there. The paper's Lind numbers come from different hardware and Wasm, so
cross-system ratios are indicative only. SPEC CPU2017 comparisons live in `lind-lfi-spec`.

## Attribution and license

Built on LFI by Zachary Yedidia and contributors (`lfi-project`, MIT License — see
`LICENSE`) and on the Lind project (`Lind-Project`). `lfi-runtime/` is imported from
`lfi-project/lfi-runtime` with its history and license; the grates under
`lfi-runtime/threei/grates/` are ported from
[`lind-wasm-example-grates`](https://github.com/Lind-Project/lind-wasm-example-grates) and
the `fdtables` crate from [`lind-wasm`](https://github.com/Lind-Project/lind-wasm), with
their original license terms. Upstream LFI's own README and guides are linked under
`docs/` and at <https://github.com/lfi-project/lfi>.
