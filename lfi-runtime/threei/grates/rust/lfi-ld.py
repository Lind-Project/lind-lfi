#!/usr/bin/env python3
# Linker wrapper: rustc (LLVM 22) emits bitcode (linker-plugin-lto); recompile every
# bitcode member with the LFI clang (so the LFI backend applies), then link with lld.
import sys, os, subprocess, tempfile, glob, hashlib, shutil
from concurrent.futures import ThreadPoolExecutor
HERE = os.path.dirname(os.path.abspath(__file__))
CLANG = os.environ.get("LFICC", "x86_64_lfi-linux-musl-clang")
AR = os.environ.get("LFIAR", "llvm-ar")
STUB = os.environ["LFI_UNWIND_STUB"]   # object defining _Unwind_Backtrace/_GetIP
work = os.environ.get("LFI_RUST_WORK", "/tmp/lfi-rust")
cache = work + "/cache"; os.makedirs(cache, exist_ok=True)
tmp = tempfile.mkdtemp(prefix="ld", dir=work)

def is_bc(p):
    with open(p, "rb") as f: m = f.read(4)
    return m == b"BC\xc0\xde"

def lfi_compile(src):
    # content-addressed cache
    h = hashlib.sha1(open(src, "rb").read()).hexdigest()
    out = f"{cache}/{h}.o"
    if not os.path.exists(out):
        t = out + f".{os.getpid()}"
        subprocess.check_call([CLANG, "-c", "-x", "ir", "-O2", "-ffunction-sections", "-fdata-sections", "-Wno-override-module", src, "-o", t])
        os.rename(t, out)
    return out

jobs = []   # (arg index, kind, payload)
args = sys.argv[1:]
out = ["-fuse-ld=lld", "-static-pie", "--unwindlib=none"]
pending = []
def conv(a):
    return a
i = 0
res = []   # list of ("arg", str) or ("objs", [paths], lazy)
while i < len(args):
    a = args[i]
    if a == "-lunwind": pass
    elif a in ("-static","-pie","-Bstatic","-Bdynamic","--no-dynamic-linker") or a.startswith("-plugin-opt="):
        pass
    elif a in ("-o","-L","-z"):
        res.append(("arg", a)); i += 1; res.append(("arg", args[i]))
    elif os.path.basename(a).startswith("libcompiler_builtins-") and a.endswith(".rlib"):
        # Rust's compiler_builtins is precompiled native code that the LFI
        # backend never sees; the LFI toolchain's own compiler-rt (linked by
        # the driver) provides the same routines.
        pass
    elif a.endswith(".rlib") and os.path.isfile(a):
        d = tempfile.mkdtemp(dir=tmp)
        subprocess.check_call([AR, "x", os.path.abspath(a)], cwd=d)
        mem = [m for m in sorted(glob.glob(d + "/*")) if "rmeta" not in os.path.basename(m)]
        res.append(("objs", mem, True))
    elif a.endswith(".o") and os.path.isfile(a):
        res.append(("objs", [a], False))
    elif a.startswith("--") or a == "-O1" or a.startswith("-Wl,"):
        res.append(("arg", "-Wl," + a))
    else:
        res.append(("arg", a))
    i += 1

allbc = [m for r in res if r[0] == "objs" for m in r[1] if is_bc(m)]
with ThreadPoolExecutor(os.cpu_count()) as ex:
    done = dict(zip(allbc, ex.map(lfi_compile, allbc)))
for r in res:
    if r[0] == "arg": out.append(r[1])
    else:
        objs = [done.get(m, m) for m in r[1]]
        if r[2]: out += ["-Wl,--start-lib"] + objs + ["-Wl,--end-lib"]
        else: out += objs
out.append(STUB)
rc = subprocess.call([CLANG] + out)
shutil.rmtree(tmp, ignore_errors=True)
sys.exit(rc)
