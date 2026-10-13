#!/bin/bash
# Runs every benchmark (see README.md). Needs the LFI toolchain on PATH and a built runtime.
HERE=$(cd "$(dirname "$0")" && pwd)
status=0
echo "### base system (native vs LFI cage)"; "$HERE/run_base.sh" 2>&1 | grep -v "warning\|\^\|^ *[0-9]* |"; [ ${PIPESTATUS[0]} -eq 0 ] || status=1
echo; echo "### grates"; "$HERE/run_grates.sh" || status=1
echo; echo "### kernel mechanisms (this machine)"; gcc -O2 -o "$HERE/out/b_kernel" "$HERE/b_kernel.c" && taskset -c 2-9 "$HERE/out/b_kernel" || status=1
echo; echo "### compute (same clang: native vs LFI)"
TC=${LFI_TC:-/home/lind/lfi/.toolchain/x86_64-lfi-clang}
"$TC/bin/clang" --target=x86_64-unknown-linux-gnu -O2 -c -o "$HERE/out/b_cpu_clang.o" "$HERE/b_cpu.c" && gcc -o "$HERE/out/b_cpu_native" "$HERE/out/b_cpu_clang.o" -lm
x86_64_lfi-linux-musl-clang -O2 -static-pie -o "$HERE/out/b_cpu_lfi" "$HERE/b_cpu.c" -lm
RUN=${RUN:-$HERE/../../build/tools/lfi-3i-run/lfi-3i-run}
nat=$(taskset -c 2-9 "$HERE/out/b_cpu_native") || status=1
lfi=$(taskset -c 2-9 "$RUN" "$HERE/out/b_cpu_lfi") || status=1
paste <(echo "$nat") <(echo "$lfi")
[ $status -eq 0 ] || echo "SOME BENCHMARKS FAILED; numbers above are not valid for those rows"
exit $status
