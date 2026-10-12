#!/bin/bash
# usage: run_tests.sh <lfi-3i-run> <outdir> [name-filter]
RUN=$1; OUT=$2; FILTER=${3:-}
pass=0; fail=0; failed=()

run() { # name expected-exit-code cmd...
    local name=$1 want=$2; shift 2
    if [ -n "$FILTER" ] && [[ "$name" != *$FILTER* ]]; then return; fi
    local log; log=$(mktemp)
    timeout -s KILL 120 "$@" >"$log" 2>&1
    local rc=$?
    if [ "$rc" = "$want" ]; then
        pass=$((pass+1)); printf 'PASS  %s\n' "$name"
    else
        fail=$((fail+1)); failed+=("$name")
        printf 'FAIL  %s (exit %s, wanted %s)\n' "$name" "$rc" "$want"
        sed 's/^/      | /' "$log" | tail -25
    fi
    rm -f "$log"
}

# Fixtures used by some of the Lind-derived tests.
echo "helloworld file contents" > /tmp/redirected.txt

source ./tests.list
rm -f /tmp/redirected.txt
echo; echo "passed: $pass  failed: $fail"
[ ${#failed[@]} -eq 0 ] || { echo "failed: ${failed[*]}"; exit 1; }
