#!/usr/bin/env python3
# This file is part of OpenBGI (https://openbgi.net).
# SPDX-License-Identifier: GPL-2.0-only
"""
checklib.py - the library and its example host (`make check-lib`)

    python3 tools/checklib.py --header=include/openbgi.h --example=examples/minihost.c
        [--shared=LIBRARY --nm=NM --objdump=OBJDUMP] [--movie=LIBRARY --movie-header=include/openbgi_ffmovie.h]
        --work=DIR [--run=PREFIX] HOST...

Four checks, each printed with its result; the exit status is 1 when one
fails:

  * the example host includes the library's header and the ISO C
    library's, nothing else - it is what a host needs, and no more;
  * the shared library (--shared; an ELF one is read with NM, a DLL with
    OBJDUMP) lets out the functions the header declares (OPENBGI_API) and
    nothing besides (but the toolchain's own marks, whose names begin
    with an underscore);
  * the movie decoder's library (--movie, a static one read with NM)
    defines the functions its header declares and no other that can be
    seen from outside (but the compiler's own helpers, whose names begin
    with an underscore), and needs nothing of the engine's: what it
    leaves undefined are ffmpeg's functions and the C library's;
  * every HOST - the example built with the static library and with the
    shared one - plays an archive made here: a boot program that shows a
    box, waits half a second and ends.  The box's text must come out, the run must end with
    the exit code 0, and it must have written a picture.  PREFIX is what
    runs the programs of another system (wine); without --run they are
    only built, not run.
"""
import argparse
import os
import re
import shlex
import struct
import subprocess
import sys
import zipfile

ISO_C99 = {
    "assert.h", "complex.h", "ctype.h", "errno.h", "fenv.h", "float.h", "inttypes.h", "iso646.h", "limits.h",
    "locale.h", "math.h", "setjmp.h", "signal.h", "stdarg.h", "stdbool.h", "stddef.h", "stdint.h", "stdio.h",
    "stdlib.h", "string.h", "tgmath.h", "time.h", "wchar.h", "wctype.h",
}
INCLUDE = re.compile(r'^\s*#\s*include\s*(?:"([^"]+)"|<([^>]+)>)')
DECLARED = re.compile(r'^OPENBGI_API\s[^;(]*?\b(OpenBGI_[A-Za-z0-9]+)\s*\(', re.M)
TEXT = "the library runs"


def check_includes(example):
    """The lines of the example that include anything but openbgi.h and ISO C."""
    bad = []
    with open(example, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            m = INCLUDE.match(line)
            if not m:
                continue
            quoted, system = m.groups()
            if quoted and quoted != "openbgi.h":
                bad.append('%s:%d: "%s" is not the library\'s header' % (example, n, quoted))
            if system and system not in ISO_C99:
                bad.append("%s:%d: <%s> is no ISO C header" % (example, n, system))
    return bad


def exports(shared, nm, objdump):
    """The names a shared library lets out."""
    if shared.lower().endswith(".dll"):
        out = subprocess.run(shlex.split(objdump) + ["-p", shared], capture_output=True, text=True, check=True).stdout
        names, table = set(), False
        for line in out.splitlines():
            if "[Ordinal/Name Pointer] Table" in line:
                table = True
            elif table:
                m = re.match(r"\s*\[\s*\d+\]\s+(?:\+base\[\s*\d+\]\s+)?(?:[0-9a-fA-F]+\s+)?(\S+)\s*$", line)
                if m:
                    names.add(m.group(1))
                elif not line.strip():
                    table = False
        return names
    out = subprocess.run(shlex.split(nm) + ["-D", "--defined-only", shared], capture_output=True, text=True, check=True).stdout
    names = {line.split()[-1] for line in out.splitlines() if line.strip()}
    return {n for n in names if not n.startswith("_")}  # (the toolchain's own marks: _init, _end, ...)


def movie_symbols(lib, nm):
    """What a static library defines for others and what it leaves undefined."""
    out = subprocess.run(shlex.split(nm) + ["-g", lib], capture_output=True, text=True, check=True).stdout
    defined, undefined = set(), set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and len(parts[-2]) == 1:
            name = parts[-1][1:] if parts[-1].startswith("_OpenBGI") else parts[-1]  # (32-bit Windows: an underscore in front)
            (undefined if parts[-2] == "U" else defined).add(name)
    return {n for n in defined if not n.startswith("_")}, undefined  # (but the compiler's own helpers: __x86.get_pc_thunk.bx, ...)


def make_archive(path):
    """A game in a zip archive: "ipl._bp", whose one thread shows TEXT in a box, waits half a second and ends."""
    text = TEXT.encode("ascii") + b"\0"
    code = bytes([0x05, 13, 0,       # the address of the text: this instruction's place plus 13
                  0xb0, 0x80,        # an information box with it
                  0x01, 0xf4, 0x01,  # 500
                  0x80, 0x58,        # the thread's timer: so many milliseconds
                  0x80, 0x5a,        # wait for it (the window shows its picture meanwhile)
                  0x17])             # the end of the thread
    size = len(code) + len(text) + 0x40
    program = struct.pack("<IIII", 16, size, 0, 0) + code + text + bytes(0x40)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_STORED) as z:
        z.writestr("ipl._bp", program)


def run_host(host, run, archive, work):
    """Play the archive with one build of the example; the list of what went wrong."""
    prefix = os.path.join(work, os.path.basename(host) + "-")
    for name in os.listdir(work):
        if name.startswith(os.path.basename(host) + "-"):
            os.remove(os.path.join(work, name))
    cmd = shlex.split(run) + [host, "--run=2000", "--every=100", archive, prefix]
    p = subprocess.run(cmd, capture_output=True, timeout=120)
    out = p.stdout.decode("latin-1")
    bad = []
    if p.returncode != 0:
        bad.append("exit status %d" % p.returncode)
    if TEXT not in out:
        bad.append("the box's text did not come out")
    if "exit code 0" not in out:
        bad.append("the run did not end with the exit code 0")
    pictures = [n for n in os.listdir(work) if n.startswith(os.path.basename(host) + "-") and n.endswith(".ppm")]
    if not pictures:
        bad.append("no picture was written")
    else:
        with open(os.path.join(work, pictures[0]), "rb") as f:
            if not re.match(rb"P6\n\d+ \d+\n255\n", f.read(32)):
                bad.append("%s is no picture" % pictures[0])
    if bad:
        sys.stdout.write(out)
        sys.stdout.write(p.stderr.decode("latin-1"))
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--header", required=True)
    ap.add_argument("--example", required=True)
    ap.add_argument("--shared")
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--objdump", default="objdump")
    ap.add_argument("--movie")
    ap.add_argument("--movie-header")
    ap.add_argument("--work", required=True)
    ap.add_argument("--run")
    ap.add_argument("hosts", nargs="*")
    a = ap.parse_args()
    failed = 0

    bad = check_includes(a.example)
    for b in bad:
        print(b)
    print("checklib: %s includes %s" % (a.example, "more than the library's header and ISO C" if bad else "openbgi.h and ISO C alone"))
    failed += bool(bad)

    if a.shared:
        with open(a.header, encoding="utf-8") as f:
            declared = set(DECLARED.findall(f.read()))
        out = exports(a.shared, a.nm, a.objdump)
        for name in sorted(declared - out):
            print("%s: %s is declared and not let out" % (a.shared, name))
        for name in sorted(out - declared):
            print("%s: %s is let out and not declared" % (a.shared, name))
        same = declared == out and declared
        print("checklib: %s lets out %s" % (a.shared, "the %d functions of %s and nothing else" % (len(out), a.header) if same else "something else than the header declares"))
        failed += not same

    if a.movie:
        with open(a.movie_header, encoding="utf-8") as f:
            declared = set(DECLARED.findall(f.read()))
        defined, undefined = movie_symbols(a.movie, a.nm)
        ours = {n for n in undefined if n.lstrip("_").startswith(("OpenBGI_", "BGI_", "Host_", "OS_"))}
        for name in sorted(declared - defined):
            print("%s: %s is declared and not defined" % (a.movie, name))
        for name in sorted(defined - declared):
            print("%s: %s can be seen from outside and is not declared" % (a.movie, name))
        for name in sorted(ours):
            print("%s: needs %s of the engine's" % (a.movie, name))
        same = declared == defined and declared and not ours
        print("checklib: %s %s" % (a.movie, "defines the %d functions of %s, and needs nothing of the engine's" % (len(defined), a.movie_header)
                                    if same else "is not what its header declares"))
        failed += not same

    if a.run is None:
        print("checklib: the example is built; programs of this platform do not run here")
    else:
        os.makedirs(a.work, exist_ok=True)
        archive = os.path.join(a.work, "boxgame.zip")
        make_archive(archive)
        for host in a.hosts:
            bad = run_host(host, a.run, archive, a.work)
            print("checklib: %s %s" % (host, "; ".join(bad) if bad else "plays an archive: its box, a picture, the exit code 0"))
            failed += bool(bad)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
