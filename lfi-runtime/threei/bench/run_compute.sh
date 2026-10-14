#!/bin/bash
# run_compute.sh: the same clang builds the kernels for native and for an LFI cage.
HERE=$(cd "$(dirname "$0")" && pwd)
status=0
mkdir -p "$HERE/out"
TC=${LFI_TC:-/home/lind/lfi/.toolchain/x86_64-lfi-clang}
RUN=${RUN:-$HERE/../../build/tools/lfi-3i-run/lfi-3i-run}
# Fresh binaries only: a failed build must not leave an old binary to be measured as if current.
rm -f "$HERE/out/b_cpu_clang.o" "$HERE/out/b_cpu_native" "$HERE/out/b_cpu_lfi"
nat=; lfi=
if "$TC/bin/clang" --target=x86_64-unknown-linux-gnu -O2 -c -o "$HERE/out/b_cpu_clang.o" "$HERE/b_cpu.c" &&
   gcc -o "$HERE/out/b_cpu_native" "$HERE/out/b_cpu_clang.o" -lm; then
    nat=$(taskset -c 2-9 "$HERE/out/b_cpu_native") || { status=1; nat="FAILED(run)"; }
else
    status=1; nat="FAILED(native build)"
fi
if x86_64_lfi-linux-musl-clang -O2 -static-pie -o "$HERE/out/b_cpu_lfi" "$HERE/b_cpu.c" -lm; then
    lfi=$(taskset -c 2-9 "$RUN" "$HERE/out/b_cpu_lfi") || { status=1; lfi="FAILED(run)"; }
else
    status=1; lfi="FAILED(LFI build)"
fi
paste <(echo "$nat") <(echo "$lfi")
[ $status -eq 0 ] || echo "COMPUTE BENCHMARKS FAILED; numbers above are not valid for those rows"
exit $status
