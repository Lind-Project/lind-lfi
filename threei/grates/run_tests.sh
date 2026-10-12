#!/bin/bash
# Runs the example grates against their tests. Env: RUN (lfi-3i-run), OUT.
# usage: run_tests.sh [name-filter]
RUN=$(realpath "${RUN:-../../build/tools/lfi-3i-run/lfi-3i-run}")
OUT=${OUT:-out}
ABS=$(realpath "$OUT")                 # test programs by absolute path
HERE=$(pwd)
FIX=$(mktemp -d)                       # scratch working directory for fixtures
LFS=$(mktemp -d)                       # a lindfs-style root for the Rust grates
trap 'rm -rf "$FIX" "$LFS"' EXIT
export RUN ABS HERE FIX LFS

# The Rust grates' tests (as in the example grates' harness) run inside a root
# directory that holds the grates, the test programs and their fixtures.
mkdir -p "$LFS/grates" "$LFS/tests" "$LFS/tmp"
cp "$OUT"/*-grate "$LFS/grates/" 2>/dev/null
if [ -d "$OUT/rust" ]; then
    cp "$OUT"/rust/* "$LFS/grates/"
    for d in "$OUT"/rust-tests/*/; do
        g=$(basename "$d"); mkdir -p "$LFS/tests/$g"; cp "$d"* "$LFS/tests/$g/"
        # fixtures
        cp rust/$g/test/*.cfg rust/$g/test/*.txt "$LFS/tests/$g/" 2>/dev/null
    done
    # ipc_test execs its helpers by their Lind (.cwasm) names
    for h in pipe_stdin_reader file_collision_reader; do
        cp "$OUT/rust-tests/ipc-grate/$h" "$LFS/tests/ipc-grate/$h.cwasm" 2>/dev/null
    done
    cp "$OUT/strace-test" "$OUT/geteuid-test" "$LFS/tests/" 2>/dev/null
    for d in "$OUT"/demo-tests/*/; do
        g=$(basename "$d"); mkdir -p "$LFS/demos/$g"; cp "$d"* "$LFS/demos/$g/"
        cp demos/$g/*.conf demos/$g/*.cfg "$LFS/demos/$g/" 2>/dev/null
    done
fi
LRUN="$HERE/lrun.sh"
export LRUN
# witness-grate signs with a per-cage Ed25519 seed read from ./witness.seed
head -c 32 /dev/urandom > "$FIX/witness.seed"
FILTER=${1:-}
pass=0; fail=0; failed=()

# run <name> <expected-exit> <required-output-regex|-> cmd...
run() {
    local name=$1 want=$2 pat=$3; shift 3
    if [ -n "$FILTER" ] && [[ "$name" != *$FILTER* ]]; then return; fi
    local log; log=$(mktemp)
    timeout -s KILL 120 "$@" >"$log" 2>&1
    local rc=$?
    local ok=1
    [ "$rc" = "$want" ] || ok=0
    if [ "$pat" != "-" ] && ! grep -Eq "$pat" "$log"; then ok=0; fi
    if [ $ok = 1 ]; then
        pass=$((pass+1)); printf 'PASS  %s\n' "$name"
    else
        fail=$((fail+1)); failed+=("$name")
        printf 'FAIL  %s (exit %s, wanted %s; output pattern: %s)\n' "$name" "$rc" "$want" "$pat"
        sed 's/^/      | /' "$log" | tail -25
    fi
    rm -f "$log"
}

source ./tests.list
echo; echo "passed: $pass  failed: $fail"
[ ${#failed[@]} -eq 0 ] || { echo "failed: ${failed[*]}"; exit 1; }
