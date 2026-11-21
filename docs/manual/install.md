# Installing

Downloads: <https://github.com/Cytlan/openbgi/releases>

OpenBGI contains no games. You need the files of a game you own: its
installed folder, a zip archive of that folder, or an image (`.iso`) of
its disc.

| System | File to download |
|---|---|
| Windows | `openbgi-<version>-windows-setup.exe` |
| Debian 12, 13; Ubuntu 22.04, 24.04, 26.04, 26.10 | `openbgi_<version>-1_amd64.deb` or `_arm64.deb` |
| Fedora 43, 44, 45 | `openbgi-<version>-1.x86_64.rpm` or `.aarch64.rpm` |
| Other Linux | `OpenBGI-<version>-x86_64.AppImage` or `-aarch64.AppImage` |
| macOS (Apple silicon) | `openbgi-<version>-macos-arm64.dmg` |
| Haiku R1/beta6 or later | `openbgi-<version>-1-x86_64.hpkg`, or `openbgi_x86-<version>-1-x86_gcc2.hpkg` on 32-bit Haiku |
| RetroArch and other libretro frontends | see [libretro.md](libretro.md) |

`SHA256SUMS` on the download page lists the checksum of every file.

## Windows

1. Run `openbgi-<version>-windows-setup.exe`. It holds the 32-bit and the
   64-bit program and installs the one that fits your system.
2. Windows may show "Windows protected your PC", because the installer
   is not signed. Click **More info**, then **Run anyway**.
3. Choose **Install for anyone using this computer** (needs an
   administrator) or **Install just for me**.
4. Start OpenBGI from the Start menu.

Without installing: unpack `openbgi-<version>-windows-x86_64.zip` (or
`-x86.zip` on 32-bit Windows) anywhere and run `openbgi.exe`.
`openbgi.com` is the same program for a console window: type `openbgi`
there to see its output.

To remove it: Settings, Apps, OpenBGI, Uninstall.

## Linux

Debian and Ubuntu:

    sudo apt install ./openbgi_<version>-1_amd64.deb

Fedora:

    sudo dnf install ./openbgi-<version>-1.x86_64.rpm

Both add OpenBGI to the desktop's menu and the command `openbgi`
(`man openbgi`).

AppImage, on any distribution with X11:

    chmod +x OpenBGI-<version>-x86_64.AppImage
    ./OpenBGI-<version>-x86_64.AppImage

`openbgi-<version>-linux-x86_64.tar.gz` holds the same program as plain
folders; its `INSTALL.txt` says where to copy them.

On Wayland desktops OpenBGI runs through XWayland, which they start by
themselves.

## macOS

OpenBGI needs a Mac with Apple silicon and macOS 11 or later. Nothing
else has to be installed.

1. Open `openbgi-<version>-macos-arm64.dmg` and drag **OpenBGI** onto
   **Applications**.
2. Open OpenBGI. macOS says that it could not verify the application,
   because it is not signed with an Apple certificate. Click **Done**.
3. Open **System Settings**, **Privacy & Security**, and scroll down to
   **Security**. Next to the line about OpenBGI, click **Open Anyway**.
   Confirm with **Open Anyway** and your password or Touch ID.

Steps 2 and 3 are needed once. The first time a game's folder is in
Documents, Downloads or on another drive, macOS asks whether OpenBGI
may read it: allow it.

To remove it: drag OpenBGI from Applications to the Trash.

## Haiku

Open the `.hpkg` file and click **Install**, or in Terminal:

    pkgman install openbgi-<version>-1-x86_64.hpkg

OpenBGI appears under **Applications** in the Deskbar menu, and as the
command `openbgi`. The libraries it needs come from Haiku's own
repositories.

## Removing OpenBGI

Uninstalling removes the program only. Your settings, the launcher's
list and saves that were written outside a game's folder stay where
[files.md](files.md) says; delete those folders to remove them too.
