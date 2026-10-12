#!/bin/bash
# Builds a Rust grate (or any Rust program) for the LFI sandbox.
#
#   cargo-lfi.sh <crate-dir> [<output-file>]
#
# Needs a nightly toolchain with rust-src and the x86_64-unknown-linux-musl
# target (LFI_RUST_TOOLCHAIN, default nightly-2026-07-16) and the LFI clang
# (x86_64_lfi-linux-musl-clang) on PATH. See README.md in this directory for
# how it works.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../../.." && pwd)                      # lfi-runtime
CRATE=$(cd "$1" && pwd)
OUTFILE=${2:+$(mkdir -p "$(dirname "$2")" && cd "$(dirname "$2")" && pwd)/$(basename "$2")}
NAME=$(sed -n 's/^name *= *"\(.*\)"/\1/p' "$CRATE/Cargo.toml" | head -1)
TOOLCHAIN=${LFI_RUST_TOOLCHAIN:-nightly-2026-07-16}
TARGET=x86_64-unknown-linux-musl
export LFICC=${LFICC:-x86_64_lfi-linux-musl-clang}
export LFI_RUST_WORK=${LFI_RUST_WORK:-${TMPDIR:-/tmp}/lfi-rust}
mkdir -p "$LFI_RUST_WORK"
unset MAKEFLAGS MFLAGS MAKELEVEL   # cargo is not a make jobserver client

# The guest shim (register_handler & co.) and the unwinder stub, as LFI objects.
INC="-I$RT/threei/include -I$RT/threei/guest/include"
# (written atomically: several grates may be built in parallel)
$LFICC -O2 -g0 $INC -c "$RT/threei/guest/lfi3i.c" -o "$LFI_RUST_WORK/lfi3i.o.$$"
mv "$LFI_RUST_WORK/lfi3i.o.$$" "$LFI_RUST_WORK/lfi3i.o"
$LFICC -O2 -g0 -c "$HERE/unwind_stub.c" -o "$LFI_RUST_WORK/unwind_stub.o.$$"
mv "$LFI_RUST_WORK/unwind_stub.o.$$" "$LFI_RUST_WORK/unwind_stub.o"
export LFI_UNWIND_STUB="$LFI_RUST_WORK/unwind_stub.o"

# rustc's TLSDESC sequences, AVX and libunwind are not verifiable, so: local-exec
# TLS, a std without backtrace support (and so without memchr/addr2line), and
# panic=abort.
RUSTFLAGS_LIST='"-C","linker-plugin-lto","-C","link-self-contained=no","-C","target-feature=+crt-static","-C","panic=abort","-Z","tls-model=local-exec","-C","link-arg='"$LFI_RUST_WORK"'/lfi3i.o"'
# A crate may need more flags (cfgs that select the portable backend of crates
# that otherwise dispatch to AVX2/AES-NI at run time): lfi.rustflags, one
# argument per line.
if [ -f "$CRATE/lfi.rustflags" ]; then
    while IFS= read -r flag; do
        flag=${flag//\"/\\\"}   # escape quotes for TOML
        [ -n "$flag" ] && RUSTFLAGS_LIST="$RUSTFLAGS_LIST,\"$flag\""
    done < "$CRATE/lfi.rustflags"
fi
export CARGO_PROFILE_RELEASE_PANIC=abort CARGO_PROFILE_RELEASE_LTO=off CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1
# One target directory for all grates: the (rebuilt) std is shared, and cargo's
# own lock serialises concurrent builds.
export CARGO_TARGET_DIR="$LFI_RUST_WORK/target"

cd "$CRATE"
cargo "+$TOOLCHAIN" build --release --target $TARGET \
  -Zbuild-std=std,panic_abort \
  --config 'unstable.build-std-features=[]' \
  --config "target.$TARGET.linker=\"$HERE/lfi-ld.py\"" \
  --config "target.$TARGET.rustflags=[$RUSTFLAGS_LIST]" \
  --config "patch.\"https://github.com/Lind-Project/lind-wasm-example-grates\".grate-rs.path=\"$HERE/../lib/grate-rs\"" \
  --config "patch.\"https://github.com/Lind-Project/lind-wasm\".fdtables.path=\"$HERE/../lib/fdtables\"" \
  ${LFI_CARGO_ARGS:-}

BIN="$CARGO_TARGET_DIR/$TARGET/release/$NAME"
if [ -n "$OUTFILE" ]; then cp "$BIN" "$OUTFILE"; else echo "$BIN"; fi
