#!/usr/bin/env python3
# This file is part of OpenBGI (https://openbgi.net).
# SPDX-License-Identifier: GPL-2.0-only
"""
mkffmpeg.py - ffmpeg's libraries for the movie decoder, with only what the
              games' movies need (`make ffmpeg`, docs/dev/building.md "Movies")

    python3 tools/mkffmpeg.py --prefix=DIR --target-os=OS --arch=ARCH [options]

fetches the release of ffmpeg named below (once; its SHA-256 is checked
every time), configures it for static libraries without programs,
network, devices, filters or anything found on the build machine, with
the decoders, file formats and parsers listed below and nothing else,
builds it and installs it into DIR: lib/libav*.a, lib/libsw*.a, their
headers and their pkg-config files.  The licence of such a build is the
LGPL, version 2.1 or later; what it was configured with is written to
DIR/openbgi-ffmpeg.txt.

    --prefix=DIR         where the libraries are installed
    --target-os=OS       ffmpeg's name of the system: linux, mingw32, haiku, android, darwin (macOS)
    --arch=ARCH          x86, x86_64, arm or arm64
    --cc=COMMAND         the C compiler (with its flags: "gcc -m32")
    --cross-prefix=P     the prefix of a cross toolchain's tools (P + ar, nm, ranlib, strip; P + gcc without --cc)
    --sysroot=DIR        the cross toolchain's sysroot
    --extra-cflags=F     more flags for the compiler; --extra-ldflags=F for the linker
    --tarball=FILE       the release's source, when it is already there (nothing is fetched)
    --work=DIR           where the source is kept and the build is made (deps/ffmpeg/src beside this tree's tools/)
    --jobs=N             make's -j (the number of processors)
    --keep               leave the build directory behind
    --list               print the configure line and stop

A build for the machine it runs on needs no --cc; one for another system
is a cross build when --cross-prefix or --sysroot is given, or --cc with
--cross.  The x86 builds use nasm for ffmpeg's assembly code and go
without it (slower) when there is none; the 32-bit x86 builds for
systems other than Windows always go without it, since part of it
cannot be loaded at any address.
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

VERSION = "7.1.5"
SHA256 = "de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f"
URL = "https://ffmpeg.org/releases/ffmpeg-%s.tar.xz" % VERSION

# What the games' movies are made of - what a Windows of the original's
# time plays without a codec added: MPEG-1 and MPEG-2 video with MPEG
# audio, Windows Media Video 7, 8 and 9 (and VC-1) with Windows Media
# Audio, the Microsoft MPEG-4 of the first .asf files, H.264 with AAC,
# and plain PCM - in MPEG program streams, ASF, AVI and MP4 / QuickTime
# files.
DECODERS = [
    "mpeg1video", "mpeg2video", "mp1float", "mp2float", "mp3float",
    "wmv1", "wmv2", "wmv3", "vc1", "msmpeg4v1", "msmpeg4v2", "msmpeg4v3", "wmav1", "wmav2", "wmapro",
    "h264", "aac",
    "pcm_u8", "pcm_s16le", "pcm_s16be", "pcm_s24le",
]
DEMUXERS = ["mpegps", "mpegvideo", "asf", "avi", "mov"]
PARSERS = ["mpegvideo", "mpegaudio", "h264", "vc1", "aac"]

FF_ARCH = {"x86": "x86_32", "x86_64": "x86_64", "arm": "arm", "arm64": "aarch64"}


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def source(work, tarball):
    """The release's source tree in `work`, fetched and unpacked when it is not there; the checksum is checked either way."""
    os.makedirs(work, exist_ok=True)
    name = "ffmpeg-%s" % VERSION
    if not tarball:
        tarball = os.path.join(work, name + ".tar.xz")
        if not os.path.isfile(tarball):
            print("mkffmpeg: fetching %s" % URL)
            part = tarball + ".part"
            with urllib.request.urlopen(URL, timeout=120) as r, open(part, "wb") as f:
                shutil.copyfileobj(r, f)
            os.replace(part, tarball)
    got = sha256_of(tarball)
    if got != SHA256:
        sys.exit("mkffmpeg: %s is not ffmpeg %s: its SHA-256 is %s, not %s" % (tarball, VERSION, got, SHA256))
    tree = os.path.join(work, name)
    done = os.path.join(tree, ".unpacked")
    if not os.path.isfile(done):
        print("mkffmpeg: unpacking %s" % tarball)
        shutil.rmtree(tree, ignore_errors=True)
        with tarfile.open(tarball) as t:
            for member in t.getmembers():  # (the archive's own directory, nothing outside it)
                if not (member.name == name or member.name.startswith(name + "/")) or ".." in member.name.split("/"):
                    sys.exit("mkffmpeg: %s holds %s" % (tarball, member.name))
            try:
                t.extractall(work, filter="data")
            except TypeError:  # (a Python from before the filters)
                t.extractall(work)
        open(done, "w").close()
    return tree


def configure_line(a, tree):
    line = [
        os.path.join(tree, "configure"),
        "--prefix=" + a.prefix,
        "--target-os=" + a.target_os,
        "--arch=" + FF_ARCH[a.arch],
        "--enable-static", "--disable-shared", "--enable-pic",
        "--disable-programs", "--disable-doc", "--disable-debug",
        "--disable-network", "--disable-autodetect", "--disable-iconv",
        "--disable-avdevice", "--disable-avfilter",
        "--disable-everything",
        "--enable-decoder=" + ",".join(DECODERS),
        "--enable-demuxer=" + ",".join(DEMUXERS),
        "--enable-parser=" + ",".join(PARSERS),
    ]
    if a.cc:
        line.append("--cc=" + a.cc)
    if a.cross_prefix:
        line.append("--cross-prefix=" + a.cross_prefix)
    if a.sysroot:
        line.append("--sysroot=" + a.sysroot)
    if a.cross_prefix or a.sysroot or a.cross:
        line.append("--enable-cross-compile")
    cflags = a.extra_cflags or ""
    if a.target_os != "mingw32":
        # ffmpeg's own names stay inside whatever the libraries are linked into - a program, or a shared
        # object such as a host's plug-in, where its assembly code could not reach them otherwise
        cflags = (cflags + " -fvisibility=hidden").strip()
    if cflags:
        line.append("--extra-cflags=" + cflags)
    if a.extra_ldflags:
        line.append("--extra-ldflags=" + a.extra_ldflags)
    if a.target_os == "mingw32":
        line += ["--disable-pthreads", "--enable-w32threads"]  # (no pthread DLL beside the program)
    if a.arch == "x86" and a.target_os != "mingw32":
        # (32-bit x86 but Windows: not all of its assembly code is position independent, which a
        # program that may be loaded anywhere needs - and Android demands)
        line += ["--disable-x86asm", "--disable-inline-asm"]
    elif a.arch in ("x86", "x86_64") and not shutil.which("nasm"):
        print("mkffmpeg: no nasm: ffmpeg's assembly code is left out (the decoders work, more slowly)")
        line.append("--disable-x86asm")
    if a.arch == "arm":
        line.append("--cpu=armv7-a")
    return line


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description="ffmpeg %s for the movie decoder" % VERSION)
    ap.add_argument("--prefix", required=True)
    ap.add_argument("--target-os", required=True, choices=["linux", "mingw32", "haiku", "android", "darwin"])
    ap.add_argument("--arch", required=True, choices=sorted(FF_ARCH))
    ap.add_argument("--cc")
    ap.add_argument("--cross-prefix")
    ap.add_argument("--sysroot")
    ap.add_argument("--cross", action="store_true")
    ap.add_argument("--extra-cflags")
    ap.add_argument("--extra-ldflags")
    ap.add_argument("--tarball")
    ap.add_argument("--work", default=os.path.join(here, "deps", "ffmpeg", "src"))
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    a.prefix = os.path.abspath(a.prefix)
    a.work = os.path.abspath(a.work)

    if a.list:
        print(" ".join(shlex.quote(x) for x in configure_line(a, os.path.join(a.work, "ffmpeg-%s" % VERSION))))
        return 0
    tree = source(a.work, a.tarball)
    line = configure_line(a, tree)
    build = os.path.join(a.work, "build-%s-%s" % (a.target_os, a.arch))
    shutil.rmtree(build, ignore_errors=True)
    os.makedirs(build)
    print("mkffmpeg: " + " ".join(shlex.quote(x) for x in line))
    log = os.path.join(build, "mkffmpeg.log")
    # (run from a makefile, as `make ffmpeg ARCH=...` does: what that make was told is not for ffmpeg's, whose
    # makefiles have an ARCH of their own that a variable from the command line would override)
    env = {k: v for k, v in os.environ.items() if k not in ("MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES", "MAKELEVEL", "ARCH", "PLATFORM")}
    with open(log, "w") as out:
        for step in (line, ["make", "-j%d" % a.jobs], ["make", "install"]):
            out.write("$ %s\n" % " ".join(shlex.quote(x) for x in step))
            out.flush()
            if subprocess.run(step, cwd=build, env=env, stdout=out, stderr=subprocess.STDOUT).returncode != 0:
                tail = open(log, errors="replace").read().splitlines()[-25:]
                extra = os.path.join(build, "ffbuild", "config.log")
                sys.exit("\n".join(tail) + "\nmkffmpeg: `%s` failed; the whole output is in %s%s" %
                         (step[0] if step is line else " ".join(step), log, " (and %s)" % extra if os.path.isfile(extra) else ""))
    with open(os.path.join(a.prefix, "openbgi-ffmpeg.txt"), "w") as f:
        f.write("ffmpeg %s (%s), SHA-256 %s\n" % (VERSION, URL, SHA256))
        f.write("built by tools/mkffmpeg.py of OpenBGI as static libraries under the LGPL, version 2.1 or later, with:\n\n")
        f.write(" ".join(shlex.quote(x) for x in line[1:]) + "\n")
    if not a.keep:
        shutil.rmtree(build, ignore_errors=True)
    print("mkffmpeg: ffmpeg %s for %s %s is in %s" % (VERSION, a.target_os, a.arch, a.prefix))
    return 0


if __name__ == "__main__":
    sys.exit(main())
