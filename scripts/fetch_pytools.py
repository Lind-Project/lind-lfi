#!/usr/bin/env python3
"""Installs meson, ninja and patchelf into .tools/ without pip or venv (neither is guaranteed to
exist). Downloads the pinned wheels from PyPI, checks their sha256 against what PyPI publishes
for that exact file, unpacks them, and writes small wrappers into .tools/bin/.

usage: fetch_pytools.py <tools-dir>
"""
import hashlib, json, os, stat, sys, urllib.request, zipfile

PINS = {
    # name: (version, wheel-filename substring that selects the file)
    "meson": ("1.12.1", "py3-none-any"),
    "ninja": ("1.13.0", "manylinux2014_x86_64"),
    "patchelf": ("0.19.1.0", "manylinux1_x86_64"),
}


def fetch(name, version, pick, dest):
    meta = json.load(urllib.request.urlopen(f"https://pypi.org/pypi/{name}/{version}/json"))
    cands = [u for u in meta["urls"] if u["packagetype"] == "bdist_wheel" and pick in u["filename"]]
    if not cands:
        sys.exit(f"fetch_pytools: no {pick} wheel for {name} {version}")
    u = cands[0]
    data = urllib.request.urlopen(u["url"]).read()
    if hashlib.sha256(data).hexdigest() != u["digests"]["sha256"]:
        sys.exit(f"fetch_pytools: checksum mismatch for {u['filename']}")
    path = os.path.join(dest, u["filename"])
    with open(path, "wb") as f:
        f.write(data)
    return path


def main():
    tools = os.path.abspath(sys.argv[1])
    py, bin_, whl = (os.path.join(tools, d) for d in ("py", "bin", "wheels"))
    for d in (py, bin_, whl):
        os.makedirs(d, exist_ok=True)
    for name, (ver, pick) in PINS.items():
        path = fetch(name, ver, pick, whl)
        with zipfile.ZipFile(path) as z:
            z.extractall(py)
    # ninja and patchelf wheels carry a prebuilt executable
    def find(sub):
        for root, _, files in os.walk(py):
            if os.path.basename(root) == sub[0] and sub[1] in files:
                return os.path.join(root, sub[1])
    for exe, where in (("ninja", ("scripts", "ninja")), ("patchelf", ("scripts", "patchelf"))):
        src = find(where)
        if not src:
            sys.exit(f"fetch_pytools: {exe} binary not found in its wheel")
        os.chmod(src, os.stat(src).st_mode | stat.S_IXUSR)
        dst = os.path.join(bin_, exe)
        if os.path.lexists(dst):
            os.remove(dst)
        os.symlink(src, dst)
    wrapper = os.path.join(bin_, "meson")
    with open(wrapper, "w") as f:
        f.write('#!/bin/sh\nD="$(cd "$(dirname "$0")/.." && pwd)"\n'
                'exec env PYTHONPATH="$D/py" python3 -m mesonbuild.mesonmain "$@"\n')
    os.chmod(wrapper, 0o755)


main()
