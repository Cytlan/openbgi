/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * release.h - the OpenBGI release: its name and version as the program
 *             and the tools state them (--version, the launcher's title,
 *             the header of openbgi.json)
 *
 * The version is the one announced publicly, "major.minor" ("1.0").  It
 * names the reimplementation, not the engine builds it runs (those are
 * the profiles, version.h).
 */
#ifndef BGI_RELEASE_H_
#define BGI_RELEASE_H_

#define OPENBGI_NAME    "OpenBGI"
#define OPENBGI_VERSION "1.0"
#define OPENBGI_TITLE   OPENBGI_NAME " v" OPENBGI_VERSION // "OpenBGI v1.0"

#endif // BGI_RELEASE_H_
