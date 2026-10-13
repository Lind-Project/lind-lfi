#!/bin/bash
# run_base.sh: native vs LFI, the "base substrate" rows of the paper's Table 4.
# A sample is only reported if the benchmark process exited 0; otherwise the row says FAILED
# and the script exits non-zero.
HERE=$(cd "$(dirname "$0")" && pwd)
RUN=${RUN:-$HERE/../../build/tools/lfi-3i-run/lfi-3i-run}
OUT=${OUT:-$HERE/out}; mkdir -p "$OUT"
gcc -O2 -o "$OUT/b_sys_native" "$HERE/b_sys.c" || exit 1
x86_64_lfi-linux-musl-clang -O2 -static-pie -o "$OUT/b_sys_lfi" "$HERE/b_sys.c" || exit 1
fail=0
# sample CMD...: last stdout line if CMD exited 0, "FAILED(rc=N)" otherwise
sample() { local o rc; o=$("$@" 2>&1); rc=$?; if [ $rc -ne 0 ]; then fail=1; echo "FAILED(rc=$rc: $(echo "$o" | tail -1))"; else echo "$o" | tail -1; fi; }
for t in null signal fork exec pipe unix readbw; do
    n=$(sample taskset -c 2-9 "$OUT/b_sys_native" $t)
    l=$(sample taskset -c 2-9 "$RUN" "$OUT/b_sys_lfi" $t)
    printf '%-14s native: %-28s lfi: %s\n' "$t" "$n" "$l"
    case "$n$l" in *FAILED*) fail=1;; esac
done
exit $fail
