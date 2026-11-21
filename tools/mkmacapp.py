#!/usr/bin/env python3
# This file is part of OpenBGI (https://openbgi.net).
# SPDX-License-Identifier: GPL-2.0-only
"""
mkmacapp.py - OpenBGI.app, and the disk image it is handed out in
              (docs/dev/building.md, "macOS")

    python3 tools/mkmacapp.py [--dmg FILE.dmg] BINARY DIR

Run on a Mac, in the tree, after `make PLATFORM=macos`: BINARY is the
program that made (bin/macos/openbgi-arm64), DIR the directory the
application goes into, as DIR/OpenBGI.app:

    Contents/Info.plist
    Contents/MacOS/OpenBGI          the program
    Contents/Resources/OpenBGI.icns the icon, from icons/openbgi.ico and the larger pictures beside it
    Contents/Resources/             README.md, LICENSE, NOTICES, docs/manual/

The application is signed without an identity ("ad hoc"), which is what
an Apple silicon Mac asks of every program.  It is not notarised: a Mac
that got it from elsewhere opens it only after its user has allowed it
(the image's "Read me first.txt" says how).

--dmg writes a compressed disk image as well: the application, a link to
/Applications to drag it onto, the read-me, LICENSE and NOTICES.

The Mac's own tools do the work (sips, iconutil, strip, codesign,
hdiutil); only the standard library is used.
"""
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IDENTIFIER = "net.openbgi.OpenBGI"
COPYRIGHT = "Copyright 2020 - 2026 Cytlan, OpenBGI Team, and Contributors"
# the pictures an .iconset has: (file name, pixels)
ICONSET = [
    ("icon_16x16.png", 16), ("icon_16x16@2x.png", 32), ("icon_32x32.png", 32), ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128), ("icon_128x128@2x.png", 256), ("icon_256x256.png", 256),
    ("icon_256x256@2x.png", 512), ("icon_512x512.png", 512), ("icon_512x512@2x.png", 1024),
]

README = """OpenBGI %(version)s for macOS (Apple silicon)
https://openbgi.net

OpenBGI plays the games of the BGI/Ethornell visual-novel engine from
their own files: a folder, a zip archive or a disc image.

1. Installing

   Drag OpenBGI onto the Applications folder beside it.

2. The first start

   OpenBGI is not signed with a certificate from Apple, so macOS does not
   open it at once:

   - Open OpenBGI.  macOS says that it could not verify it; choose Done.
   - Open System Settings, Privacy & Security, and scroll down to
     Security.  A line there names OpenBGI and has the button "Open
     Anyway".  Click it, choose "Open Anyway" once more and confirm with
     your password or Touch ID.

   After that it opens like any other program.

3. Playing

   Started by itself, OpenBGI shows its launcher: under "Folders", add
   the folder your games are in, and start one - or drag a game's
   folder, zip archive or disc image onto the window.  The documents are
   inside the application (Show Package Contents, Contents/Resources)
   and on openbgi.net.

OpenBGI is free software under the GNU General Public License, version 2
(LICENSE).  NOTICES lists the works of others that are part of it.
"""


def run(*cmd):
    print("  " + " ".join(cmd))
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    if r.returncode != 0:
        sys.exit("%s\nmkmacapp: `%s` failed" % (r.stdout.strip(), cmd[0]))
    return r.stdout


def version():
    with open(os.path.join(ROOT, "src", "include", "bgi", "release.h")) as f:
        m = re.search(r'#define OPENBGI_VERSION\s+"([^"]+)"', f.read())
    if not m:
        sys.exit("mkmacapp: no version in src/include/bgi/release.h")
    return m.group(1)


def icon(ico, out):
    """the .ico file's pictures as an .icns file; False when the Mac's tools do not make one"""
    with tempfile.TemporaryDirectory() as tmp:
        pictures = os.path.join(tmp, "png")
        run(sys.executable, os.path.join(ROOT, "tools", "mkicon.py"), "--png", ico, pictures)
        have = {}
        for name in os.listdir(pictures):
            m = re.match(r"openbgi-(\d+)\.png$", name)
            if m:
                have[int(m.group(1))] = os.path.join(pictures, name)
        if not have:
            return False
        for size in (512, 1024):  # (an .ico file ends at 256 pixels: the larger pictures lie beside it)
            beside = os.path.join(os.path.dirname(ico), "openbgi-%d.png" % size)
            if os.path.isfile(beside):
                have[size] = beside
        largest = max(have)
        iconset = os.path.join(tmp, "OpenBGI.iconset")
        os.makedirs(iconset)
        for name, size in ICONSET:
            if size in have:
                shutil.copyfile(have[size], os.path.join(iconset, name))
            elif size <= largest:  # (a picture is made smaller, never larger)
                run("sips", "-z", str(size), str(size), have[largest], "--out", os.path.join(iconset, name))
        r = subprocess.run(["iconutil", "-c", "icns", "-o", out, iconset], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return r.returncode == 0 and os.path.isfile(out)


def application(binary, directory, ver):
    app = os.path.join(directory, "OpenBGI.app")
    shutil.rmtree(app, ignore_errors=True)
    macos = os.path.join(app, "Contents", "MacOS")
    resources = os.path.join(app, "Contents", "Resources")
    os.makedirs(macos)
    os.makedirs(os.path.join(resources, "docs", "manual"))

    shutil.copyfile(binary, os.path.join(macos, "OpenBGI"))
    os.chmod(os.path.join(macos, "OpenBGI"), 0o755)
    run("strip", os.path.join(macos, "OpenBGI"))

    for name in ("README.md", "LICENSE", "NOTICES"):
        shutil.copyfile(os.path.join(ROOT, name), os.path.join(resources, name))
    for name in sorted(os.listdir(os.path.join(ROOT, "docs", "manual"))):
        if name.endswith(".md"):
            shutil.copyfile(os.path.join(ROOT, "docs", "manual", name), os.path.join(resources, "docs", "manual", name))

    info = {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleName": "OpenBGI",
        "CFBundleDisplayName": "OpenBGI",
        "CFBundleIdentifier": IDENTIFIER,
        "CFBundleExecutable": "OpenBGI",
        "CFBundlePackageType": "APPL",
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleShortVersionString": ver,
        "CFBundleVersion": ver,
        "LSMinimumSystemVersion": "11.0",
        "LSArchitecturePriority": ["arm64"],
        "LSApplicationCategoryType": "public.app-category.games",
        "NSPrincipalClass": "NSApplication",
        # the picture is drawn at the display's own resolution (two pixels a point where it has them)
        "NSHighResolutionCapable": True,
        # a game's folder, zip archive or disc image may be dropped on the icon; OpenBGI is nobody's usual program for them
        "CFBundleDocumentTypes": [{
            "CFBundleTypeName": "Game folder, archive or disc image",
            "CFBundleTypeRole": "Viewer",
            "LSHandlerRank": "Alternate",
            "LSItemContentTypes": ["public.folder", "public.zip-archive", "public.iso-image"],
        }],
        "NSHumanReadableCopyright": COPYRIGHT,
    }
    if icon(os.path.join(ROOT, "icons", "openbgi.ico"), os.path.join(resources, "OpenBGI.icns")):
        info["CFBundleIconFile"] = "OpenBGI"
    else:
        print("mkmacapp: no icon (sips / iconutil did not make one); the application has the system's")
    with open(os.path.join(app, "Contents", "Info.plist"), "wb") as f:
        plistlib.dump(info, f)

    run("codesign", "--force", "--sign", "-", app)
    run("codesign", "--verify", "--strict", app)
    return app


def image(app, dmg, ver):
    with tempfile.TemporaryDirectory() as tmp:
        top = os.path.join(tmp, "OpenBGI")
        os.makedirs(top)
        # (ditto keeps what the signature of a script is stored in, which a plain copy may not)
        run("ditto", app, os.path.join(top, "OpenBGI.app"))
        os.symlink("/Applications", os.path.join(top, "Applications"))
        with open(os.path.join(top, "Read me first.txt"), "w") as f:
            f.write(README % {"version": ver})
        for name in ("LICENSE", "NOTICES"):
            shutil.copyfile(os.path.join(ROOT, name), os.path.join(top, name + ".txt"))
        if os.path.exists(dmg):
            os.remove(dmg)
        run("hdiutil", "create", "-volname", "OpenBGI " + ver, "-srcfolder", top, "-fs", "HFS+", "-format", "UDZO", "-ov", dmg)


def main(argv):
    args = argv[1:]
    dmg = None
    if len(args) >= 2 and args[0] == "--dmg":
        dmg = args[1]
        args = args[2:]
    if len(args) != 2 or args[0].startswith("-"):
        sys.exit(__doc__)
    binary, directory = args
    if sys.platform != "darwin":
        sys.exit("mkmacapp: this is for a Mac (it runs sips, iconutil, codesign and hdiutil)")
    if not os.path.isfile(binary):
        sys.exit("mkmacapp: no %s (make PLATFORM=macos first)" % binary)
    os.makedirs(directory, exist_ok=True)
    ver = version()
    app = application(binary, directory, ver)
    print("mkmacapp: %s" % app)
    if dmg:
        image(app, dmg, ver)
        print("mkmacapp: %s" % dmg)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
