#!/usr/bin/env python3
# This file is part of OpenBGI (https://openbgi.net).
# SPDX-License-Identifier: GPL-2.0-only
"""
mkfreetype.py - FreeType for a system that has none: macOS
                (`make freetype PLATFORM=macos`, docs/dev/building.md "macOS")

    python3 tools/mkfreetype.py --prefix=DIR [options]

fetches the release of FreeType named below (once; its SHA-256 is checked
every time), configures it for a static library that needs nothing else
(no zlib, bzip2, libpng, HarfBuzz or Brotli of the system's), builds it
and installs it into DIR: lib/libfreetype.a, its headers and its
pkg-config file.

    --prefix=DIR         where the library is installed
    --cc=COMMAND         the C compiler
    --host=TRIPLET       the system it is built for, for a cross build (aarch64-apple-darwin)
    --extra-cflags=F     more flags for the compiler
    --tarball=FILE       the release's source, when it is already there (nothing is fetched)
    --work=DIR           where the source is kept and the build is made (deps/freetype/src beside this tree's tools/)
    --jobs=N             make's -j (the number of processors)

Only the standard library is used.
"""
import argparse
import hashlib
import os
import shlex
import shutil
import subprocess
import sys
import tarfile
import urllib.request

VERSION = "2.13.3"
SHA256 = "0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289"
URL = "https://download.savannah.gnu.org/releases/freetype/freetype-%s.tar.xz" % VERSION


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def source(work, tarball):
    """The release's source tree in `work`, fetched and unpacked when it is not there; the checksum is checked either way."""
    os.makedirs(work, exist_ok=True)
    name = "freetype-%s" % VERSION
    if not tarball:
        tarball = os.path.join(work, name + ".tar.xz")
        if not os.path.isfile(tarball):
            print("mkfreetype: fetching %s" % URL)
            part = tarball + ".part"
            with urllib.request.urlopen(URL, timeout=120) as r, open(part, "wb") as f:
                shutil.copyfileobj(r, f)
            os.replace(part, tarball)
    got = sha256_of(tarball)
    if got != SHA256:
        sys.exit("mkfreetype: %s is not FreeType %s: its SHA-256 is %s, not %s" % (tarball, VERSION, got, SHA256))
    tree = os.path.join(work, name)
    done = os.path.join(tree, ".unpacked")
    if not os.path.isfile(done):
        print("mkfreetype: unpacking %s" % tarball)
        shutil.rmtree(tree, ignore_errors=True)
        with tarfile.open(tarball) as t:
            for member in t.getmembers():  # (the archive's own directory, nothing outside it)
                if not (member.name == name or member.name.startswith(name + "/")) or ".." in member.name.split("/"):
                    sys.exit("mkfreetype: %s holds %s" % (tarball, member.name))
            try:
                t.extractall(work, filter="data")
            except TypeError:  # (a Python from before the filters)
                t.extractall(work)
        open(done, "w").close()
    return tree


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description="FreeType %s as a static library" % VERSION)
    ap.add_argument("--prefix", required=True)
    ap.add_argument("--cc")
    ap.add_argument("--host")
    ap.add_argument("--extra-cflags")
    ap.add_argument("--tarball")
    ap.add_argument("--work", default=os.path.join(here, "deps", "freetype", "src"))
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    a = ap.parse_args()
    a.prefix = os.path.abspath(a.prefix)
    a.work = os.path.abspath(a.work)

    tree = source(a.work, a.tarball)
    line = [
        os.path.join(tree, "configure"),
        "--prefix=" + a.prefix,
        "--disable-shared", "--enable-static",
        "--with-zlib=no", "--with-bzip2=no", "--with-png=no", "--with-harfbuzz=no", "--with-brotli=no",
        "CFLAGS=-O2" + (" " + a.extra_cflags if a.extra_cflags else ""),
    ]
    if a.cc:
        line.append("CC=" + a.cc)
    if a.host:
        line.append("--host=" + a.host)
    build = os.path.join(a.work, "build-" + os.path.basename(a.prefix))
    shutil.rmtree(build, ignore_errors=True)
    os.makedirs(build)
    print("mkfreetype: " + " ".join(shlex.quote(x) for x in line))
    log = os.path.join(build, "mkfreetype.log")
    # (run from a makefile, as `make freetype PLATFORM=...` does: what that make was told is not for FreeType's)
    env = {k: v for k, v in os.environ.items() if k not in ("MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES", "MAKELEVEL", "ARCH", "PLATFORM")}
    with open(log, "w") as out:
        for step in (line, ["make", "-j%d" % a.jobs], ["make", "install"]):
            out.write("$ %s\n" % " ".join(shlex.quote(x) for x in step))
            out.flush()
            if subprocess.run(step, cwd=build, env=env, stdout=out, stderr=subprocess.STDOUT).returncode != 0:
                tail = open(log, errors="replace").read().splitlines()[-25:]
                sys.exit("\n".join(tail) + "\nmkfreetype: `%s` failed; the whole output is in %s" %
                         (step[0] if step is line else " ".join(step), log))
    if not os.path.isfile(os.path.join(a.prefix, "lib", "libfreetype.a")):
        sys.exit("mkfreetype: the build left no lib/libfreetype.a in %s (%s)" % (a.prefix, log))
    shutil.rmtree(build, ignore_errors=True)
    print("mkfreetype: FreeType %s is in %s" % (VERSION, a.prefix))
    return 0


if __name__ == "__main__":
    sys.exit(main())
