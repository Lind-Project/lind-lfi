#!/bin/bash
# Runs every benchmark (see README.md). Needs the LFI toolchain on PATH and a built runtime.
HERE=$(cd "$(dirname "$0")" && pwd)
status=0
echo "### base system (native vs LFI cage)"; "$HERE/run_base.sh" 2>&1 | grep -v "warning\|\^\|^ *[0-9]* |"; [ ${PIPESTATUS[0]} -eq 0 ] || status=1
echo; echo "### grates"; "$HERE/run_grates.sh" || status=1
echo; echo "### kernel mechanisms (this machine)"; gcc -O2 -o "$HERE/out/b_kernel" "$HERE/b_kernel.c" && taskset -c 2-9 "$HERE/out/b_kernel" || status=1
echo; echo "### compute (same clang: native vs LFI)"; "$HERE/run_compute.sh" || status=1
[ $status -eq 0 ] || echo "SOME BENCHMARKS FAILED; numbers above are not valid for those rows"
exit $status
