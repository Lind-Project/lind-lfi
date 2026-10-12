#!/bin/bash
# run_base.sh: native vs LFI, the "base substrate" rows of the paper's Table 4.
HERE=$(cd "$(dirname "$0")" && pwd)
RUN=${RUN:-$HERE/../../build/tools/lfi-3i-run/lfi-3i-run}
OUT=${OUT:-$HERE/out}; mkdir -p "$OUT"
gcc -O2 -o "$OUT/b_sys_native" "$HERE/b_sys.c" || exit 1
x86_64_lfi-linux-musl-clang -O2 -static-pie -o "$OUT/b_sys_lfi" "$HERE/b_sys.c" || exit 1
for t in null signal fork exec pipe unix readbw; do
    n=$(taskset -c 2-9 "$OUT/b_sys_native" $t 2>&1 | tail -1)
    l=$(taskset -c 2-9 "$RUN" "$OUT/b_sys_lfi" $t 2>&1 | tail -1)
    printf '%-14s native: %-28s lfi: %s\n' "$t" "$n" "$l"
done
