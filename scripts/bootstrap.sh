#!/bin/bash
# Fetches and prepares the external pieces the build needs, all inside this checkout (nothing is
# installed system-wide). Safe to re-run.
#
#   .toolchain/x86_64-lfi-clang   the LFI clang bundle, lfi-project/lfi release v0.12 (checksum-pinned)
#   .tools/venv                   meson and ninja (python venv)
#   Rust nightly-2026-07-16       with rust-src and the x86_64-unknown-linux-musl target, via rustup
#                                 (only needed for the Rust grates; skip with --no-rust)
#
# The bundle's compilers are linked against glibc >= 2.38. On an older host (Ubuntu 22.04 has 2.35)
# the script extracts the matching Ubuntu 24.04 libc6 into .tools/glibc and points the bundle's
# executables at it (patchelf); nothing outside this directory changes.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

TC_VERSION=v0.12
TC_URL=https://github.com/lfi-project/lfi/releases/download/$TC_VERSION/x86_64-lfi-clang.tar.gz
TC_SHA256=4083cdebbf42da3d1b5d5db3320b6699a7dc16e72d358b5ad4e0c4ae72770673
GLIBC_DEB=libc6_2.39-0ubuntu8.9_amd64.deb
GLIBC_URL=http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/$GLIBC_DEB
GLIBC_SHA256=ff5557d99b51f761c4b7c92368b9cc45565eda17df9bf9eb4b134d09825008be
RUST_TOOLCHAIN=${LFI_RUST_TOOLCHAIN:-nightly-2026-07-16}
WANT_RUST=1
[ "${1:-}" = --no-rust ] && WANT_RUST=0

log() { printf '[bootstrap] %s\n' "$*"; }
need() { command -v "$1" >/dev/null 2>&1 || { echo "bootstrap: '$1' is required" >&2; exit 1; }; }
for t in curl tar python3 sha256sum gcc; do need "$t"; done
[ "$(uname -m)" = x86_64 ] && [ "$(uname -s)" = Linux ] || { echo "bootstrap: x86-64 Linux only" >&2; exit 1; }

mkdir -p .cache .tools

fetch() { # url file sha256
    if [ -f "$2" ] && echo "$3  $2" | sha256sum -c --quiet 2>/dev/null; then return; fi
    log "downloading $1"
    curl -fL --retry 3 -o "$2.part" "$1"
    echo "$3  $2.part" | sha256sum -c --quiet || { echo "bootstrap: checksum mismatch for $1" >&2; exit 1; }
    mv "$2.part" "$2"
}

# ---- meson / ninja ---------------------------------------------------------------------------
if [ ! -x .tools/venv/bin/meson ]; then
    log "creating .tools/venv (meson, ninja, patchelf)"
    python3 -m venv .tools/venv
    .tools/venv/bin/pip install -q --upgrade pip
    .tools/venv/bin/pip install -q "meson>=1.4" ninja patchelf
fi

# ---- LFI clang bundle ------------------------------------------------------------------------
if [ ! -x .toolchain/x86_64-lfi-clang/bin/clang ]; then
    fetch "$TC_URL" ".cache/x86_64-lfi-clang-$TC_VERSION.tar.gz" "$TC_SHA256"
    log "extracting the bundle into .toolchain/"
    mkdir -p .toolchain
    tar -xf ".cache/x86_64-lfi-clang-$TC_VERSION.tar.gz" -C .toolchain
    touch .toolchain/x86_64-lfi-clang/.pristine
fi

# ---- glibc workaround for older hosts ----------------------------------------------------------
host_glibc=$(ldd --version | head -1 | grep -o '[0-9]\+\.[0-9]\+$' || echo 0.0)
if [ "$(printf '%s\n2.38\n' "$host_glibc" | sort -V | head -1)" != "2.38" ] && [ ! -f .tools/glibc/.done ]; then
    need dpkg-deb
    log "host glibc $host_glibc < 2.38: using a private glibc 2.39 for the bundle's executables"
    fetch "$GLIBC_URL" ".cache/$GLIBC_DEB" "$GLIBC_SHA256"
    rm -rf .tools/glibc && mkdir -p .tools/glibc
    dpkg-deb -x ".cache/$GLIBC_DEB" .tools/glibc
    G="$ROOT/.tools/glibc"
    n=0
    while IFS= read -r f; do
        if readelf -l "$f" 2>/dev/null | grep -q INTERP; then
            old_rpath=$(.tools/venv/bin/patchelf --print-rpath "$f" 2>/dev/null || true)
            .tools/venv/bin/patchelf --set-interpreter "$G/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" \
                --set-rpath "${old_rpath:+$old_rpath:}$G/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu" "$f"
            n=$((n + 1))
        fi
    done < <(find .toolchain/x86_64-lfi-clang -type f -perm -u+x)
    log "patched $n executables"
    touch .tools/glibc/.done
fi

# ---- Rust ---------------------------------------------------------------------------------------
if [ $WANT_RUST = 1 ]; then
    need rustup
    if ! rustup toolchain list | grep -q "^$RUST_TOOLCHAIN"; then
        log "installing Rust $RUST_TOOLCHAIN"
        rustup toolchain install "$RUST_TOOLCHAIN" --profile minimal -c rust-src -t x86_64-unknown-linux-musl
    fi
fi

log "done. Next:  source scripts/env.sh"
