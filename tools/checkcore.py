#!/usr/bin/env python3
# This file is part of OpenBGI (https://openbgi.net).
# SPDX-License-Identifier: GPL-2.0-only
"""
checkcore.py - the #include lines of the core (`make check-core`)

    python3 tools/checkcore.py FILE...

Every source and header given is the core's: what it includes must be the
core's too or the ISO C library's.  A quoted name is looked for in include/ (the API), src/include/
and beside the file (any other place - src/host/, src/tools/ - is a
host's); a name in angle brackets must be one of the ISO C99 headers, or
FreeType's and Vorbis's (the libraries the core may use; FreeType's own
FT_* header macros included).  One file may name more: src/gfx/blit_simd.h
includes the compiler's own vector headers (emmintrin.h, arm_neon.h),
which belong to no system and are there or not by the processor the
build is for.  The lines inside #if blocks count as well:
every configuration's includes are checked.  Nor may the core's #if
lines ask for a platform (_WIN32, __linux__, BGI_POSIX, ...) or a
feature macro of a system (_POSIX_C_SOURCE, _GNU_SOURCE, ...): the core
is the same everywhere; what differs is the hosts'.

The files of src/base/ are held to more: they are what needs no engine,
so nothing they include - at first hand or through another header - may
be the engine's state (bgi/core.h) or its API (openbgi.h).  (That they
call nothing of the engine either is the link of tests/base.c.)

Each offence is printed as "file:line: ..."; the exit status is 1 when
there is one.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ISO_C99 = {
    "assert.h", "complex.h", "ctype.h", "errno.h", "fenv.h", "float.h", "inttypes.h", "iso646.h", "limits.h",
    "locale.h", "math.h", "setjmp.h", "signal.h", "stdarg.h", "stdbool.h", "stddef.h", "stdint.h", "stdio.h",
    "stdlib.h", "string.h", "tgmath.h", "time.h", "wchar.h", "wctype.h",
}
LIBRARIES = {"ft2build.h", "vorbis/vorbisfile.h", "vorbis/codec.h"}
# the compiler's vector headers, and the one file that may include them
VECTOR = {"emmintrin.h", "arm_neon.h"}
VECTOR_FILE = os.path.join("src", "gfx", "blit_simd.h")

INCLUDE = re.compile(r'^\s*#\s*include\s*(?:"([^"]+)"|<([^>]+)>|([A-Za-z_][A-Za-z0-9_]*))')
CONDITION = re.compile(r'^\s*#\s*(?:if|ifdef|ifndef|elif)\b(.*)')
PLATFORMS = {
    "_WIN32", "_WIN64", "__CYGWIN__", "__MINGW32__", "__MINGW64__", "__linux__", "__linux", "__gnu_linux__", "__APPLE__",
    "__MACH__", "__unix__", "__unix", "__HAIKU__", "__BEOS__", "__EMSCRIPTEN__", "__ANDROID__", "__FreeBSD__",
    "__NetBSD__", "__OpenBSD__", "BGI_WIN32", "BGI_POSIX", "BGI_WASM", "BGI_HAIKU", "BGI_MACOS", "BGI_ANDROID",
    "BGI_HEADLESS", "_POSIX_C_SOURCE", "_POSIX_SOURCE", "_XOPEN_SOURCE", "_GNU_SOURCE", "_DEFAULT_SOURCE",
    "_BSD_SOURCE", "_DARWIN_C_SOURCE", "_FILE_OFFSET_BITS", "_LARGEFILE64_SOURCE",
}
WORD = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')
BASE = os.path.join("src", "base") + os.sep
ENGINE = {"bgi/core.h", "openbgi.h"}  # what src/base/ may not reach


def header(quoted, here):
    """the file a quoted name stands for, None when there is none"""
    for folder in (os.path.join(ROOT, "src", "include"), os.path.join(ROOT, "include"), here):
        path = os.path.join(folder, quoted)
        if os.path.isfile(path):
            return path
    return None


def reaches_engine(path, seen):
    """the chain of quoted names from `path` to one of ENGINE, None when there is none"""
    with open(path, encoding="utf-8", errors="surrogateescape") as f:
        names = [m.group(1) for m in map(INCLUDE.match, f) if m and m.group(1)]
    for quoted in names:
        if quoted in ENGINE:
            return [quoted]
        inner = header(quoted, os.path.dirname(path))
        if inner and inner not in seen:
            seen.add(inner)
            chain = reaches_engine(inner, seen)
            if chain:
                return [quoted] + chain
    return None


def check(path):
    problems = []
    here = os.path.dirname(path)
    base = os.path.relpath(os.path.abspath(path), ROOT).startswith(BASE)
    with open(path, encoding="utf-8", errors="surrogateescape") as f:
        for n, line in enumerate(f, 1):
            c = CONDITION.match(line)
            if c:
                cond = c.group(1).split("//")[0].split("/*")[0]
                for w in sorted(set(WORD.findall(cond)) & PLATFORMS):
                    problems.append("%s:%d: %s: the core asks for no platform" % (path, n, w))
                continue
            m = INCLUDE.match(line)
            if not m:
                continue
            quoted, system, macro = m.groups()
            if quoted and base:
                inner = header(quoted, here)
                chain = [quoted] if quoted in ENGINE else inner and reaches_engine(inner, {inner})
                if chain:
                    if quoted not in ENGINE:
                        chain = [quoted] + chain
                    problems.append('%s:%d: src/base/ needs no engine, and this includes it: %s' % (path, n, " -> ".join(chain)))
            if quoted:
                inc = os.path.join(ROOT, "src", "include", quoted)
                if not os.path.isfile(inc):
                    inc = os.path.join(ROOT, "include", quoted)
                beside = os.path.join(here, quoted)
                if not os.path.isfile(inc) and not os.path.isfile(beside):
                    problems.append('%s:%d: "%s" is not a header of the core (include/, src/include/ or beside the file)' % (path, n, quoted))
                elif os.path.isfile(beside) and not os.path.isfile(inc):
                    rel = os.path.relpath(os.path.realpath(beside), ROOT)
                    if rel.startswith(("src" + os.sep + "host", "src" + os.sep + "tools")):
                        problems.append('%s:%d: "%s" is a host\'s header' % (path, n, quoted))
            elif system:
                vector = system in VECTOR and os.path.relpath(os.path.realpath(path), ROOT) == VECTOR_FILE
                if system not in ISO_C99 and system not in LIBRARIES and not vector:
                    problems.append("%s:%d: <%s> is neither an ISO C header nor one of the core's libraries" % (path, n, system))
            elif macro and not macro.startswith("FT_"):
                problems.append("%s:%d: #include %s: a header named by a macro" % (path, n, macro))
    return problems


def main(files):
    problems = []
    for path in files:
        problems += check(path)
    for p in problems:
        print(p)
    if problems:
        print("checkcore: %d line%s of the core that reach outside it" % (len(problems), "" if len(problems) == 1 else "s"))
        return 1
    print("checkcore: %d files include only the core's headers and ISO C's and ask for no platform" % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
