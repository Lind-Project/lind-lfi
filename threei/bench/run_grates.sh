#!/bin/bash
# run_grates.sh: per-hop grate crossing cost and friends (paper §5.2 Q2b, Table 5).
HERE=$(cd "$(dirname "$0")" && pwd)
RUN=${RUN:-$HERE/../../build/tools/lfi-3i-run/lfi-3i-run}
OUT=${OUT:-$HERE/out}; mkdir -p "$OUT"
INC="-I$HERE/../include -I$HERE/../guest/include"
CC=x86_64_lfi-linux-musl-clang
GR=$HERE/../grates/out
for g in g_fwd g_copy; do $CC -O2 $INC -static-pie -o "$OUT/$g" "$HERE/$g.c" "$HERE/../guest/lfi3i.c" || exit 1; done
$CC -O2 -static-pie -o "$OUT/c_copy" "$HERE/c_copy.c" || exit 1
B=$OUT/b_sys_lfi
r() { taskset -c 2-9 "$@" 2>/dev/null | tail -1; }
echo "== null/geteuid through grates (ns per call)"
printf '%-34s %s\n' "no grate (geteuid)"            "$(r $RUN $B geteuid)"
printf '%-34s %s\n' "1 grate answering"              "$(r $RUN $OUT/g_fwd answer $B geteuid)"
printf '%-34s %s\n' "1 grate forwarding"             "$(r $RUN $OUT/g_fwd fwd $B geteuid)"
printf '%-34s %s\n' "2 grates forwarding (stacked)"  "$(r $RUN $OUT/g_fwd fwd $OUT/g_fwd fwd $B geteuid)"
printf '%-34s %s\n' "3 grates forwarding (stacked)"  "$(r $RUN $OUT/g_fwd fwd $OUT/g_fwd fwd $OUT/g_fwd fwd $B geteuid)"
echo "== policies (getpid, ns per call)"
printf '%-34s %s\n' "no policy"                      "$(r $RUN $B getpid)"
if [ -x "$GR/strace-grate" ]; then
printf '%-34s %s\n' "strace-grate (C, log to /dev/null)" "$(taskset -c 2-9 $RUN $GR/strace-grate $B getpid 2>/dev/null | tail -1)"
printf '%-34s %s\n' "2x strace-grate"                "$(taskset -c 2-9 $RUN $GR/strace-grate $GR/strace-grate $B getpid 2>/dev/null | tail -1)"
printf '%-34s %s\n' "seccomp-grate (allow)"          "$(r $RUN $GR/seccomp-grate $HERE/seccomp_bench.conf $B getpid)"
fi
echo "== copy_data_between_cages"
taskset -c 2-9 $RUN $OUT/g_copy $OUT/c_copy 2>/dev/null
echo "== grate / cage start-up (ms, mean of 30)"
t() { local s=$(date +%s%N); for i in $(seq 30); do "$@" >/dev/null 2>&1; done; echo "$(( ($(date +%s%N) - s) / 30000 )) us"; }
printf '%-34s %s\n' "cage alone (exit-now)"          "$(t taskset -c 2-9 $RUN $B exit-now)"
printf '%-34s %s\n' "grate + cage (exit-now)"        "$(t taskset -c 2-9 $RUN $OUT/g_fwd answer $B exit-now)"
printf '%-34s %s\n' "2 grates + cage"                "$(t taskset -c 2-9 $RUN $OUT/g_fwd answer $OUT/g_fwd answer $B exit-now)"
