#!/usr/bin/env python3
# This file is part of OpenBGI (https://openbgi.net).
# SPDX-License-Identifier: GPL-2.0-only
"""
checkretro.py - the libretro core and the test frontend (`make check-libretro`)

    python3 tools/checkretro.py --header=src/host/libretro/libretro.h --core=CORE
        [--nm=NM --objdump=OBJDUMP] --work=DIR [--run=PREFIX] FRONTEND

The checks, each printed with its result; the exit status is 1 when one
fails:

  * the core (an ELF one is read with NM, a DLL with OBJDUMP) lets out the
    functions libretro.h declares (RETRO_API) and nothing besides (but the
    toolchain's own marks, whose names begin with an underscore);
  * it needs none of ffmpeg's libraries at run time: they are linked in;
  * FRONTEND (tools/retrofront.c) plays an archive made here with it - a
    boot program that shows a box, waits half a second and ends - twice
    in one process, and once more with the core's library loaded anew:
    the box is drawn in the picture and answered by the Enter key of the
    script, each load must bring pictures, a picture's worth of sound with
    every call and the core's own end, and the folders for what a game
    writes must be made under the save directory.  Then the box is
    answered the other ways there are - a click of the mouse on its
    button (and one beside it, which must leave it), the pad's confirm,
    a touch with the pointer device as a touch screen - and once after
    the frontend's reset, which must bring it a second time.  The places
    are those of the canvas the core hands over: the picture lies in its
    middle;
  * the picture: on the canvas of 1920 x 1080 a frontend that says
    nothing of its display gets, the game's 800 x 600 lie in the middle
    as they are, on black; on a display of 2560 x 1440 twice as large,
    bicubic; the other steps, filters and sizes as the options say, also
    when they change in the middle of a run; and a call in which nothing
    changed brings no new picture;
  * the options: the same ones each time the core tells them, with
    their categories or as plain variables, the patches of the folder
    "patches" among their choices; a button bound by an option does what
    it says, at once; an option that needs a new start says so, and
    holds after the frontend's reset; a patch chosen is laid over the
    game; the pointer drawn for a mouse;
  * a movie in a zip archive (when the unit tests left one to play): its
    pictures, and nothing copied out of the archive.
PREFIX is what runs the programs of another system (wine); without --run
the frontend is only built, not run.
"""
import argparse
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import zipfile

from checklib import exports, make_archive

DECLARED = re.compile(r'^RETRO_API\s[^;(]*?\b(retro_[a-z_]+)\s*\(', re.M)


def needed(core, objdump):
    """The libraries a core needs at run time."""
    out = subprocess.run(shlex.split(objdump) + ["-p", core], capture_output=True, text=True, check=True).stdout
    return re.findall(r"^\s*(?:NEEDED|DLL Name:)\s+(\S+)", out, re.M)


def play(frontend, run, core, archive, work, reopen):
    """Two loads of the archive in one run of the frontend; the list of what went wrong."""
    name = "reopen" if reopen else "again"
    save, system = os.path.join(work, name + "-save"), os.path.join(work, name + "-system")
    for d in (save, system):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    script = os.path.join(work, "input.txt")
    with open(script, "w") as f:
        f.write("1500 key RETURN\n")
    prefix = os.path.join(work, name + "-")
    for n in os.listdir(work):
        if n.startswith(name + "-") and n.endswith(".ppm"):
            os.remove(os.path.join(work, n))
    cmd = shlex.split(run) + [frontend, "--run=5000", "--every=1000", "--shots=" + prefix, "--input=" + script, "--loads=2",
                              "--save=" + save, "--system=" + system] + (["--reopen"] if reopen else []) + [core, archive]
    p = subprocess.run(cmd, capture_output=True, timeout=300)
    out = p.stdout.decode("latin-1")
    bad = []
    if p.returncode != 0:
        bad.append("exit status %d" % p.returncode)
    loads = re.findall(r"load (\d): OpenBGI \S+, (\d+) calls, (\d+) pictures of (\d+) x (\d+) .*?, (\d+) frames of sound at 44100 Hz, (.*)", out)
    if len(loads) != 2:
        bad.append("%d loads reported, not 2" % len(loads))
    for n, calls, pictures, w, h, sound, end in loads:
        if int(pictures) < 1 or int(w) < 1 or int(h) < 1:
            bad.append("load %s brought no picture" % n)
        if int(sound) != int(calls) * 735:
            bad.append("load %s: %s frames of sound in %s calls" % (n, sound, calls))
        if "asked to be shut down" not in end:
            bad.append("load %s: the game did not end by itself (the box was not answered?)" % n)
        if not 90 < int(calls) < 300:
            bad.append("load %s ended after %s calls, not about 2 s" % (n, calls))
    if not os.path.isdir(os.path.join(save, "OpenBGI", "games")):
        bad.append("no OpenBGI/games/ under the save directory")
    if not os.path.isdir(os.path.join(system, "openbgi", "fonts")):
        bad.append("no openbgi/fonts/ under the system directory")
    pictures = sorted(n for n in os.listdir(work) if n.startswith(name + "-") and n.endswith(".ppm"))
    if not pictures:
        bad.append("no picture was written")
    else:
        with open(os.path.join(work, pictures[0]), "rb") as f:
            data = f.read()
        m = re.match(rb"P6\n(\d+) (\d+)\n255\n", data)
        if not m:
            bad.append("%s is no picture" % pictures[0])
        elif len(set(data[m.end():])) < 3:
            bad.append("%s shows nothing (the box should be in it)" % pictures[0])
    if bad:
        sys.stdout.write(out)
        sys.stdout.write(p.stderr.decode("latin-1"))
    return bad


def answer(frontend, run, core, archive, work, name, script, options, calls):
    """One load in which the box is answered another way; the list of what went wrong."""
    save = os.path.join(work, "input-save")
    shutil.rmtree(save, ignore_errors=True)
    os.makedirs(save)
    path = os.path.join(work, "input-%s.txt" % name)
    with open(path, "w") as f:
        f.write(script)
    cmd = shlex.split(run) + [frontend, "--run=6000", "--input=" + path, "--save=" + save] + ["--option=" + o for o in options] + [core, archive]
    p = subprocess.run(cmd, capture_output=True, timeout=300)
    out = p.stdout.decode("latin-1")
    m = re.search(r"load 1: OpenBGI \S+, (\d+) calls, .*, (.*)", out)
    bad = []
    if p.returncode != 0:
        bad.append("exit status %d" % p.returncode)
    if not m:
        bad.append("no result")
    elif "asked to be shut down" not in m.group(2):
        bad.append("the box was not answered")
    elif not calls[0] <= int(m.group(1)) <= calls[1]:
        bad.append("the game ended after %s calls, not %d to %d" % (m.group(1), calls[0], calls[1]))
    if bad and calls[1]:
        sys.stdout.write(out)
        sys.stdout.write(p.stderr.decode("latin-1"))
    return bad


# The box of the archive's program is in the middle of the game's 800 x 600 pixels, its one button at 400, 331.
# On the canvas of 1920 x 1080 the picture lies at 560, 240; with the option "game" the canvas is the picture.
GAME = "openbgi_canvas=game"
INPUTS = [
    ("the mouse", "1500 click 960 571\n", [], (110, 150)),
    ("the mouse beside the button", "1500 click 760 340\n", [], None),
    ("the mouse on the game's own picture", "1500 click 400 331\n", [GAME], (110, 150)),
    ("the pad's confirm", "1500 pad south\n", [], (110, 150)),
    ("a touch", "1500 tap 960 571\n", ["openbgi_pointer=touch screen"], (110, 150)),
    ("a reset, then the key", "1000 reset\n2500 key RETURN\n", [], (170, 215)),
    # the options of the pad: a button bound, a button unbound, the two that confirm and cancel the other way round, a change on the way
    ("the pad's Y bound to confirm", "1500 pad west\n", ["openbgi_pad_y=confirm"], (110, 150)),
    ("the pad's B bound to nothing", "1500 pad south\n", ["openbgi_pad_b=none"], None),
    ("the pad's A as confirm", "1500 pad east\n", ["openbgi_pad_confirm=right"], (110, 150)),
    ("the pad's Start, where a frontend has the Enter key", "1500 pad start\n", [], (110, 150)),
    ("the pad's Start bound to cancel", "1500 pad start\n", ["openbgi_pad_start=cancel"], (110, 150)),
    ("the pad's Start bound to nothing", "1500 pad start\n", ["openbgi_pad_start=none"], None),
    ("the pad's L2 bound to Enter in the middle of the run", "1000 option openbgi_pad_l2=key:RETURN\n1500 pad l2\n", [], (110, 150)),
]


class Run:
    """One run of the frontend with the archive: its exit status, what it printed, and the pictures it wrote, by their time."""

    def __init__(self, a, archive, name, script="", options=(), extra=(), ms=2500, every=1000):
        work = a.work
        save = os.path.join(work, "run-save")
        shutil.rmtree(save, ignore_errors=True)
        os.makedirs(save)
        path = os.path.join(work, "run-%s.txt" % name)
        with open(path, "w") as f:
            f.write(script)
        prefix = os.path.join(work, "run-%s-" % name)
        for n in os.listdir(work):
            if n.startswith("run-%s-" % name) and n.endswith(".ppm"):
                os.remove(os.path.join(work, n))
        cmd = shlex.split(a.run) + [a.frontend, "--run=%d" % ms, "--every=%d" % every, "--shots=" + prefix, "--input=" + path, "--save=" + save]
        cmd += ["--option=" + o for o in options] + list(extra) + [a.core, archive]
        p = subprocess.run(cmd, capture_output=True, timeout=300)
        self.status = p.returncode
        self.out = p.stdout.decode("latin-1").replace("\r", "")  # (a Windows program's lines)
        self.err = p.stderr.decode("latin-1").replace("\r", "")
        self.prefix = prefix
        m = re.search(r"load 1: OpenBGI \S+, (\d+) calls, (\d+) pictures of (\d+) x (\d+) \(announced (\d+) x (\d+) at 60 Hz; (\d+) calls without a new one\), "
                      r"\d+ frames of sound at 44100 Hz, (.*)", self.out)
        self.calls, self.pictures, self.w, self.h, self.saidW, self.saidH, self.dupes = (int(v) for v in m.groups()[:7]) if m else (0,) * 7
        self.end = m.group(8) if m else "no result"

    def picture(self, ms):
        """The picture written at `ms`: (width, height, its rows); None when there is none."""
        try:
            with open("%s%09d.ppm" % (self.prefix, ms), "rb") as f:
                data = f.read()
        except OSError:
            return None
        m = re.match(rb"P6\n(\d+) (\d+)\n255\n", data)
        if not m:
            return None
        w, h = int(m.group(1)), int(m.group(2))
        body = data[m.end():]
        return w, h, [body[y * w * 3:(y + 1) * w * 3] for y in range(h)]

    def show(self):
        sys.stdout.write(self.out)
        sys.stdout.write(self.err)


def part(pic, x, y, w, h):
    """The rows of a part of a picture."""
    return [row[x * 3:(x + w) * 3] for row in pic[2][y:y + h]]


def black_around(pic, x, y, w, h):
    """Whether a picture is black but for the part at x, y of w x h."""
    pw, ph, rows = pic
    zero = bytes(pw * 3)
    for r in range(ph):
        row = rows[r]
        if r < y or r >= y + h:
            if row != zero:
                return False
        elif row[:x * 3] != bytes(x * 3) or row[(x + w) * 3:] != bytes((pw - x - w) * 3):
            return False
    return True


def placed(run, ms, size, at, scale, own, exact):
    """What is wrong with the picture of a run at `ms`: it is to be `size`, with the game's picture `own` at `at`, `scale` times as large."""
    pic = run.picture(ms)
    if not pic:
        return ["no picture at %d ms" % ms]
    if (pic[0], pic[1]) != size:
        return ["the picture at %d ms is %d x %d, not %d x %d" % (ms, pic[0], pic[1], size[0], size[1])]
    ow, oh = own[0], own[1]
    w, h = round(ow * scale), round(oh * scale)
    bad = []
    if not black_around(pic, at[0], at[1], w, h):
        bad.append("the canvas is not black around %d x %d at %d, %d" % (w, h, at[0], at[1]))
    got = part(pic, at[0], at[1], w, h)
    if scale == 1:
        if got != own[2]:
            bad.append("the game's picture is not in the canvas as it is")
        return bad
    if len(set(b"".join(got[::7]))) < 3:
        return bad + ["the scaled picture shows nothing"]
    if scale != int(scale):
        return bad
    n = int(scale)
    if exact:  # the nearest pixel: every pixel n times, every row n times
        for y in range(0, oh, 5):
            row = own[2][y]
            want = b"".join(row[i:i + 3] * n for i in range(0, ow * 3, 3))
            if any(got[y * n + k] != want for k in range(n)):
                return bad + ["row %d is not the game's, each pixel %d times" % (y, n)]
        return bad
    # smooth: the mean of each n x n pixels is about the game's pixel, and some pixels are neither of the two it lies between
    total = count = 0
    for y in range(0, oh, 7):
        rows = got[y * n:y * n + n]
        row = own[2][y]
        for i in range(0, ow * 3, 3 * 5):
            for c in range(3):
                mean = sum(r[i * n + c + 3 * k] for r in rows for k in range(n)) / (n * n)
                total += abs(mean - row[i + c])
                count += 1
    if total / count > 4:
        bad.append("the scaled picture is not the game's (mean difference %.1f)" % (total / count))
    y = oh // 2 - 20  # (a row through the box's title)
    if got[y * n] == b"".join(own[2][y][i:i + 3] * n for i in range(0, ow * 3, 3)):
        bad.append("the picture is scaled to the nearest pixel, not smoothly")
    return bad


def pictures(a, archive):
    """The picture's checks; the list of (what, what is wrong)."""
    res = []
    own_run = Run(a, archive, "own", options=[GAME])
    own = own_run.picture(1000)
    bad = []
    if not own or (own[0], own[1]) != (800, 600) or (own_run.saidW, own_run.saidH) != (800, 600):
        own_run.show()
        return [("the game's own picture", ["it is not 800 x 600, announced so"])]
    if len(set(b"".join(own[2][::5]))) < 3:
        bad.append("it shows nothing")
    res.append(("the game's own picture, 800 x 600", bad))

    r = Run(a, archive, "canvas")
    bad = placed(r, 1000, (1920, 1080), (560, 240), 1, own, True)
    if (r.saidW, r.saidH) != (1920, 1080):
        bad.append("announced %d x %d" % (r.saidW, r.saidH))
    if not 1 <= r.pictures <= 5 or r.dupes < r.calls - 5:
        bad.append("%d pictures in %d calls of a game that stands still" % (r.pictures, r.calls))
    res.append(("a canvas of 1920 x 1080, the game's picture in its middle as it is, no picture again while nothing changes", bad))

    cases = [
        ("a display of 2560 x 1440: twice as large, bicubic", [], ["--display=2560x1440"], (2560, 1440), (480, 120), 2, False),
        ("the nearest pixel", ["openbgi_filter=nearest"], ["--display=2560x1440"], (2560, 1440), (480, 120), 2, True),
        ("bilinear", ["openbgi_filter=bilinear"], ["--display=2560x1440"], (2560, 1440), (480, 120), 2, False),
        ("whole numbers on 3840 x 2160: three times", ["openbgi_canvas=3840x2160", "openbgi_scale=whole numbers"], [], (3840, 2160), (720, 180), 3, False),
        ("powers of two on 3840 x 2160: twice", ["openbgi_canvas=3840x2160"], [], (3840, 2160), (1120, 480), 2, False),
        ("filling 2560 x 1440: 2.4 times", ["openbgi_canvas=2560x1440", "openbgi_scale=fill"], [], (2560, 1440), (320, 0), 2.4, False),
        ("no scaling on 2560 x 1440", ["openbgi_scale=none"], ["--display=2560x1440"], (2560, 1440), (880, 420), 1, True),
        ("the option over the display", ["openbgi_canvas=1280x720"], ["--display=2560x1440"], (1280, 720), (240, 60), 1, True),
        ("plain variables only", ["openbgi_canvas=1280x720"], ["--options=0"], (1280, 720), (240, 60), 1, True),
    ]
    for what, options, extra, size, at, scale, exact in cases:
        r = Run(a, archive, "case", options=options, extra=extra, ms=1500)
        bad = placed(r, 1000, size, at, scale, own, exact)
        if (r.saidW, r.saidH) != size:
            bad.append("announced %d x %d" % (r.saidW, r.saidH))
        if r.status:
            bad.append("exit status %d" % r.status)
        if bad:
            r.show()
        res.append((what, bad))

    # the options changed on the way: the filter, a smaller canvas, a larger one than was announced
    r = Run(a, archive, "live", script="1200 option openbgi_filter=nearest\n", extra=["--display=2560x1440"])
    bad = placed(r, 1000, (2560, 1440), (480, 120), 2, own, False) + placed(r, 2000, (2560, 1440), (480, 120), 2, own, True)
    res.append(("the filter changed in the middle of a run", bad))
    r = Run(a, archive, "live", script="1200 option openbgi_canvas=1280x720\n")
    bad = placed(r, 1000, (1920, 1080), (560, 240), 1, own, True) + placed(r, 2000, (1280, 720), (240, 60), 1, own, True)
    if "the core announced 1280 x 720" not in r.out:
        bad.append("the new size was not announced")
    res.append(("a smaller canvas in the middle of a run", bad))
    r = Run(a, archive, "live", script="1200 option openbgi_canvas=3840x2160\n")
    bad = placed(r, 2000, (3840, 2160), (1120, 480), 2, own, False)
    if "the core announced 3840 x 2160" not in r.out or r.status:
        bad.append("the new size was not announced (exit status %d)" % r.status)
    if bad:
        r.show()
    res.append(("a larger canvas in the middle of a run", bad))

    # the pointer for a mouse: not drawn unless the option says so (a frontend shows its own), then also over the box
    for on in (False, True):
        r = Run(a, archive, "arrow", script="500 move 100 100\n700 move 420 300\n", options=[GAME] + (["openbgi_mouse_arrow=on"] if on else []), ms=1500)
        pic = r.picture(1000)
        bad = []
        if not pic:
            bad.append("no picture")
        else:
            white = pic[2][305][422 * 3:423 * 3] == b"\xff\xff\xff" and pic[2][300][419 * 3:420 * 3] == own[2][300][419 * 3:420 * 3]
            if on and not white:
                bad.append("no arrow where the mouse is")
            if not on and part(pic, 0, 0, 800, 600) != own[2]:
                bad.append("the picture is not the game's alone")
        res.append(("the pointer drawn for a mouse, with the option" if on else "no pointer drawn for a mouse", bad))
    return res


MOVIE = os.path.join("bin", "test_ffmovie", "red.mpg")  # a second of red, left by the unit tests where the ffmpeg program is


def movie(a):
    """A movie in an archive: played from where it is; the list of (what, what is wrong)."""
    if not os.path.isfile(MOVIE):
        return [("a movie in an archive: not tried (no %s: `make test` makes it where there is an ffmpeg program)" % MOVIE, [])]
    name = b"op.mpg\0"
    code = bytes([0x05, 25, 0,        # the address of the name: this instruction's place plus 25
                  0x01, 0, 0,         # x
                  0x01, 0, 0,         # y
                  0x01, 0x20, 0x03,   # 800 wide
                  0x01, 0x58, 0x02,   # 600 high
                  0x90, 0xf0,         # play the movie there
                  0x01, 0xb8, 0x0b,   # 3000
                  0x80, 0x58,         # the thread's timer: so many milliseconds
                  0x80, 0x5a,         # wait for it
                  0x17])              # the end of the thread
    program = struct.pack("<IIII", 16, len(code) + len(name) + 0x40, 0, 0) + code + name + bytes(0x40)
    archive = os.path.join(a.work, "movie.zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_STORED) as z:
        z.writestr("ipl._bp", program)
        z.write(MOVIE, "op.mpg")
    r = Run(a, archive, "movie", options=[GAME], ms=2500, every=500)
    bad = []
    for ms, red in ((500, True), (2000, False)):
        pic = r.picture(ms)
        if not pic:
            bad.append("no picture at %d ms" % ms)
            continue
        px = pic[2][300][400 * 3:401 * 3]
        if red != (px[0] > 200 and px[1] < 60 and px[2] < 60):
            bad.append("at %d ms the picture is %s" % (ms, "not the movie's" if red else "still the movie's"))
    if "extracting" in r.err or os.path.exists(os.path.join(a.work, "run-save", "OpenBGI", "games", "movie", ".openbgi")):
        bad.append("the movie was copied out of the archive")
    if r.status:
        bad.append("exit status %d" % r.status)
    if bad:
        r.show()
    return [("a movie in an archive: played from where it is, nothing copied out", bad)]


def make_patch(path, top=""):
    """A patch: a boot program that ends at once, in a zip archive (in the folder `top` of it)."""
    code = bytes([0x17])  # the end of the thread
    program = struct.pack("<IIII", 16, len(code) + 0x40, 0, 0) + code + bytes(0x40)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(top + "IPL._bp", program)
        z.writestr(top + "Patch/readme.txt", "a patch for the test")


def options(a, archive):
    """The checks of the options and the patches; the list of (what, what is wrong)."""
    res = []
    # the patches: for an archive, the zip archives of "patches/<its name>/" beside it when that is there, else of "patches/"
    folder = os.path.join(os.path.dirname(archive), "patches")
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    make_patch(os.path.join(folder, "ends at once.zip"))
    make_patch(os.path.join(folder, "Another.ZIP"), "Another/")
    with open(os.path.join(folder, "notes.txt"), "w") as f:
        f.write("no patch")
    r = Run(a, archive, "list", extra=["--list"], ms=500, every=0)
    told = dict(re.findall(r"^option (\S+) \(.*?\): (.*)$", r.out, re.M))
    m = re.search(r"(\d+) options in (\d+) categories, told (\d+) times", r.out)
    bad = []
    if not m or int(m.group(1)) != len(told) or len(told) < 30 or int(m.group(2)) < 5 or int(m.group(3)) != 2 or r.status:
        bad.append("the options were not told twice, the same ones, in categories (%s; exit status %d)" % (m.group(0) if m else "nothing", r.status))
    for key in ("openbgi_patch_1", "openbgi_patch_4"):
        if told.get(key) != "[none] [Another.ZIP] [ends at once.zip]":
            bad.append("%s offers %s" % (key, told.get(key)))
    if not told.get("openbgi_engine", "").startswith("[automatic] [1.58] ") or "[1.669]" not in told.get("openbgi_engine", ""):
        bad.append("the engine's profiles are not offered")
    if "control 1 0 0: Confirm" not in r.out or "control 1 0 1:" in r.out:
        bad.append("the controls are not told as they are")
    if bad:
        r.show()
    res.append(("%d options in %s categories, the patches beside the game among them" % (len(told), m.group(2) if m else "no"), bad))

    r0 = Run(a, archive, "list", extra=["--list", "--options=0"], ms=500, every=0)
    told0 = dict(re.findall(r"^option (\S+) \(.*?\): (.*)$", r0.out, re.M))
    bad = [] if told0 == told and "in 0 categories" in r0.out and not r0.status else ["they are not the same as with categories"]
    res.append(("the same options as plain variables", bad))

    own = os.path.join(folder, os.path.splitext(os.path.basename(archive))[0])
    os.makedirs(own)
    make_patch(os.path.join(own, "its own.zip"))
    r = Run(a, archive, "list", extra=["--list"], options=["openbgi_pad_y=confirm", "openbgi_pad_b=none"], ms=500, every=0)
    bad = []
    if "option openbgi_patch_2 (Patch 2): [none] [its own.zip]\n" not in r.out:
        bad.append("the archive's own folder of patches is not the one offered")
    if "control 1 0 1: Confirm" not in r.out or "control 1 0 0:" in r.out:
        bad.append("the controls do not follow the options")
    res.append(("an archive's own folder of patches; the controls as the options bind them", bad))
    shutil.rmtree(own)

    # a patch laid over the game: its boot program ends at once, so no box comes
    r = Run(a, archive, "patch", options=["openbgi_patch_1=ends at once.zip"], ms=3000, every=0)
    bad = []
    if "asked to be shut down" not in r.end or r.calls > 60:
        bad.append("the game did not end at once (%d calls, %s)" % (r.calls, r.end))
    if "2 files over the game's" not in r.err:
        bad.append("the patch was not laid over the game")
    if bad:
        r.show()
    res.append(("a patch chosen: its files in place of the game's", bad))
    r = Run(a, archive, "patch", options=["openbgi_patch_1=ends at once.zip", "openbgi_patch_2=Another.ZIP"], ms=3000, every=0)
    bad = [] if "asked to be shut down" in r.end and r.calls <= 60 and r.err.count("2 files over the game's") == 2 else ["the two were not laid over the game"]
    res.append(("two patches, one of them zipped as a folder", bad))
    r = Run(a, archive, "patch", options=["openbgi_patch_1=no such.zip"], ms=1500, every=0)
    bad = [] if "stopped" in r.end and "over the game's" not in r.err and "is no choice" in r.err else ["%s" % r.end]
    res.append(("a patch that is not there is not chosen", bad))

    # an option of the start changed while the game runs: said, and in force after the frontend's reset
    r = Run(a, archive, "restart", script="500 option openbgi_patch_1=ends at once.zip\n1500 reset\n", ms=4000, every=0)
    bad = []
    if "next start" not in r.err:
        bad.append("the player was not told that it needs a new start")
    if "asked to be shut down" not in r.end or not 85 <= r.calls <= 130:
        bad.append("the reset did not start the game with it (%d calls, %s)" % (r.calls, r.end))
    if bad:
        r.show()
    res.append(("an option of the start changed on the way: said, and in force after a reset", bad))
    r = Run(a, archive, "restart", script="500 option openbgi_scale=fill\n800 option openbgi_pad_y=skip\n", ms=1500, every=0)
    bad = ["the player was told of a new start"] if "next start" in r.err else []
    res.append(("the picture's and the pad's options need none", bad))
    shutil.rmtree(folder)
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--header", required=True)
    ap.add_argument("--core", required=True)
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--objdump", default="objdump")
    ap.add_argument("--work", required=True)
    ap.add_argument("--run")
    ap.add_argument("frontend")
    a = ap.parse_args()
    failed = 0

    with open(a.header, encoding="utf-8") as f:
        declared = set(DECLARED.findall(f.read()))
    out = exports(a.core, a.nm, a.objdump)
    missing, extra = sorted(declared - out), sorted(out - declared)
    if missing or extra or not declared:
        failed = 1
        print("checkretro: %s: FAILED: missing %s; not in libretro.h: %s" % (a.core, ", ".join(missing) or "none", ", ".join(extra) or "none"))
    else:
        print("checkretro: %s lets out the %d functions of libretro.h and nothing else" % (a.core, len(declared)))

    libs = needed(a.core, a.objdump)
    ffmpeg = [n for n in libs if re.match(r"(lib)?(av|sw)[a-z]+[-.]", n)]
    if ffmpeg:
        failed = 1
        print("checkretro: %s: FAILED: needs %s at run time" % (a.core, ", ".join(ffmpeg)))
    else:
        print("checkretro: %s needs %s" % (a.core, ", ".join(libs) or "no library"))

    if a.run is None:
        print("checkretro: %s built; programs of this platform do not run here" % a.frontend)
        return failed
    os.makedirs(a.work, exist_ok=True)
    archive = os.path.join(a.work, "retro.zip")
    make_archive(archive)
    for reopen in (False, True):
        bad = play(a.frontend, a.run, a.core, archive, a.work, reopen)
        how = "the library loaded anew for each" if reopen else "in one loaded library"
        if bad:
            failed = 1
            print("checkretro: two loads, %s: FAILED: %s" % (how, "; ".join(bad)))
        else:
            print("checkretro: two loads, %s: the box drawn and answered, pictures and sound, the core's own end" % how)
    for n, (name, script, opts, calls) in enumerate(INPUTS):
        bad = answer(a.frontend, a.run, a.core, archive, a.work, str(n), script, opts, calls or (0, 0))
        if calls is None:  # (nothing answers the box: the run must go on to its end)
            ok = bad == ["the box was not answered"]
            bad = [] if ok else ["the box went away"]
        if bad:
            failed = 1
            print("checkretro: the box answered by %s: FAILED: %s" % (name, "; ".join(bad)))
        else:
            print("checkretro: %s: %s" % (name, "the box stays" if calls is None else "the box answered, the game's end"))
    for what, bad in pictures(a, archive) + options(a, archive) + movie(a):
        if bad:
            failed = 1
            print("checkretro: %s: FAILED: %s" % (what, "; ".join(bad)))
        else:
            print("checkretro: %s" % what)
    return failed


if __name__ == "__main__":
    sys.exit(main())
