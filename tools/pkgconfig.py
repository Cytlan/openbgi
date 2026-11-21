#!/usr/bin/env python3
# This file is part of OpenBGI (https://openbgi.net).
# SPDX-License-Identifier: GPL-2.0-only
"""
pkgconfig.py - stands in for pkg-config where there is none

    make PKGCONF="python3 tools/pkgconfig.py" ...

A Mac with the Command Line Tools alone has no pkg-config, and the build
asks it for two things there: the libraries of XQuartz and the tree's own
ffmpeg (docs/dev/building.md, "macOS").  This answers what the Makefile asks:

    pkgconfig.py [--static] [--define-variable=NAME=VALUE] --exists MODULE...
    pkgconfig.py [--static] [--define-variable=NAME=VALUE] [--cflags] [--libs] MODULE...

The .pc files are looked for in the directories of PKG_CONFIG_LIBDIR,
or of PKG_CONFIG_PATH when that is not set; PKG_CONFIG_SYSROOT_DIR goes
before the directories of -I and -L.  A module that is not there ends
the run with status 1 and nothing printed.

It is not pkg-config: a module that another one requires and that has no
.pc file is passed over (pkg-config gives up), versions are not compared,
and there are no directories it looks in by itself.

Only the standard library is used.
"""
import os
import re
import sys


def find(name):
    for d in (os.environ.get("PKG_CONFIG_LIBDIR") or os.environ.get("PKG_CONFIG_PATH") or "").split(os.pathsep):
        path = os.path.join(d, name + ".pc")
        if d and os.path.isfile(path):
            return path
    return None


def read(path, defined):
    """the fields of a .pc file, its variables put in"""
    values = {"pcfiledir": os.path.dirname(os.path.abspath(path))}
    fields = {}

    def put_in(text):
        return re.sub(r"\$\{([^}]*)\}", lambda m: values.get(m.group(1), ""), text)

    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            m = re.match(r"([A-Za-z0-9_.]+)\s*([=:])\s*(.*)", line)
            if not m:
                continue
            key, kind, value = m.groups()
            if kind == "=":
                values[key] = defined[key] if key in defined else put_in(value)
            else:
                fields[key] = put_in(value)
    return fields


def names(requires):
    """the modules of a Requires field: "a >= 1.2, b c" -> a, b, c"""
    out = []
    words = requires.replace(",", " ").split()
    i = 0
    while i < len(words):
        if words[i] in ("=", "<", ">", "<=", ">=", "!="):
            i += 2
            continue
        out.append(words[i])
        i += 1
    return out


def walk(name, private, defined, seen, after):
    """the module and what it requires (`private`: what it requires for itself too), each once and
    after everything it requires; False when the module is not there"""
    if name in seen:
        return True
    path = find(name)
    if not path:
        return False
    seen.add(name)
    fields = read(path, defined)
    for other in reversed(names(fields.get("Requires", "")) + (names(fields.get("Requires.private", "")) if private else [])):
        walk(other, private, defined, seen, after)
    after.append(fields)
    return True


def modules_of(top, private, defined):
    """the modules in the order of a linker's line: each before those it needs"""
    after = []
    seen = set()
    for m in reversed(top):
        if not walk(m, private, defined, seen, after):
            return None
    return list(reversed(after))


def words(text):
    """the switches of a field; "-framework X" stays one"""
    out = []
    parts = text.split()
    i = 0
    while i < len(parts):
        if parts[i] == "-framework" and i + 1 < len(parts):
            out.append(parts[i] + " " + parts[i + 1])
            i += 2
        else:
            out.append(parts[i])
            i += 1
    return out


def main(argv):
    static = exists = cflags = libs = False
    defined = {}
    modules = []
    for a in argv[1:]:
        if a == "--static":
            static = True
        elif a == "--exists":
            exists = True
        elif a == "--cflags":
            cflags = True
        elif a == "--libs":
            libs = True
        elif a.startswith("--define-variable="):
            key, _, value = a[len("--define-variable="):].partition("=")
            defined[key] = value
        elif a.startswith("-"):
            sys.exit("pkgconfig.py: %s is not known here\n%s" % (a, __doc__))
        else:
            modules.append(a)
    if not modules or not (exists or cflags or libs):
        sys.exit(__doc__)
    # (the compiler's switches of a private requirement are needed too: its headers may be included)
    everything = modules_of(modules, True, defined)
    if everything is None:
        return 1
    if exists:
        return 0
    sysroot = os.environ.get("PKG_CONFIG_SYSROOT_DIR") or ""
    out = []
    if cflags:
        for fields in everything:
            for w in words(fields.get("Cflags", "")):
                if w not in out and w != "-I/usr/include":
                    out.append(w)
    if libs:
        found = []
        for fields in everything if static else modules_of(modules, False, defined):
            found += words(fields.get("Libs", ""))
            if static:
                found += words(fields.get("Libs.private", ""))
        # a directory once, where it comes first; a library once, where it comes last (after all that need it)
        dirs = []
        rest = []
        for w in found:
            if w.startswith("-L"):
                if w not in dirs and w != "-L/usr/lib":
                    dirs.append(w)
            else:
                rest.append(w)
        last = []
        for w in reversed(rest):
            if w not in last:
                last.append(w)
        out += dirs + list(reversed(last))
    if sysroot:
        out = [w[:2] + sysroot + w[2:] if w[:2] in ("-I", "-L") and w[2:3] == "/" else w for w in out]
    print(" ".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
