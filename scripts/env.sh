# source this file: puts the pinned build tools and the LFI compilers on PATH.
_lfi_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export LFI_ROOT=$_lfi_root
export PATH="$_lfi_root/.tools/bin:$_lfi_root/.toolchain/x86_64-lfi-clang/lfi-bin:$PATH"
export LFI_TC="$_lfi_root/.toolchain/x86_64-lfi-clang"
unset _lfi_root
