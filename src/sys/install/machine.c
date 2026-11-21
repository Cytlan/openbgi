/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * machine.c - the machine key and the registration check (src/include/bgi/install.h)
 *
 * The games of the time tied their registration to the machine: a 32-bit
 * key from the CPU signature, the Windows version and the first letters
 * of the user and computer names ("80 EE", "80 EF"), checked against the
 * file "comap.dat" that the game's registration program writes ("80 EC",
 * "80 ED").  The machine's facts are the host's (Host_Version,
 * Host_CpuSignature, Host_UserName, Host_ComputerName).
 */
#include "install_internal.h"
#include "bgi/engine.h" // o->engine.noDiscCheck
#include "bgi/host.h"
#include "bgi/fs.h"
#include "bgi/core.h"

/* A code for the Windows version: 0x1f Win32s, 0x5f / 0x62 / 0x63 Windows
 * 95 / 98 / ME, 3 / 4 NT 3 / 4, 0x7d0 / 0x7d1 2000 / XP, 0x1770 Vista and
 * later, 0 unknown.  Only OsKeyBits reads it. */
static int OsVersionCode(OpenBGI_t* o)
{
	uint32_t ver[4];
	char csd[0x80];
	int platform, major, minor;
	Host_Version(o, ver, csd, sizeof csd);
	major = (int)ver[0];
	minor = (int)ver[1];
	platform = (int)ver[3];
	switch(platform)
	{
		case 0: // Win32s
			return 0x1f;
		case 1: // Windows 9x
			if(major == 4)
			{
				if(minor == 0)
					return 0x5f; // 95
				if(minor <= 10)
					return 0x62; // 98
				return 0x63;     // ME
			}
			return major > 4 ? 0x63 : 0;
		case 2: // NT
			if(major == 3)
				return 3;
			if(major == 4)
				return 4;
			if(major == 5)
				return 0x7d0 + (minor != 0); // 2000 / XP
			if(major >= 6)
				return 0x1770; // Vista and later
			return 0;
		default:
			return 0;
	}
}

// the low 12 bits of CPUID.1:EAX: stepping, model and family
static uint32_t CpuSignature(OpenBGI_t* o)
{
	return Host_CpuSignature(o) & 0xfffu;
}

// the Windows version as bits 12..15 of the key (0xf000 for an unknown version)
static uint32_t OsKeyBits(OpenBGI_t* o)
{
	switch(OsVersionCode(o))
	{
		case 0x5f: return 0x1000;  // 95
		case 0x62: return 0x2000;  // 98
		case 0x63: return 0x3000;  // ME
		case 0x7d0: return 0x4000; // 2000
		case 3: return 0x5000;     // NT 3
		case 4: return 0x6000;     // NT 4
		case 0x1f: return 0x7000;  // Win32s
		case 0x7d1:
		case 0x1770: return 0x8000; // XP, Vista and later
		default: return 0xf000;
	}
}

/* "80 EE": the machine key, (((cpu | os) << 8) | user[0]) << 8 |
 * computer[0]: the CPU signature and the Windows version in the high
 * half, the first byte of the user name and of the computer name in the
 * low one (0 when a name is empty). */
uint32_t GetMachineKey(OpenBGI_t* o)
{
	char user[0x100], computer[0x100];
	uint32_t key;
	memset(user, 0, sizeof user);
	memset(computer, 0, sizeof computer);
	Host_UserName(o, user, sizeof user);
	Host_ComputerName(o, computer, 0x10); // only 16 bytes of the computer name are asked for; the first is all that is used
	key = CpuSignature(o) | OsKeyBits(o);
	key = (key << 8) | (uint8_t)user[0];
	key = (key << 8) | (uint8_t)computer[0];
	return key;
}

// "80 EF": the machine key XOR x
uint32_t GetMachineKeyXor(OpenBGI_t* o, uint32_t x)
{
	return GetMachineKey(o) ^ x;
}

/* "80 ED": the first 0x404 bytes of "comap.dat" in the current directory
 * into buf; 1 when the file could be opened (a shorter file fills less). */
int ReadComapDat(OpenBGI_t* o, void* buf)
{
	FsFile_t* f = Fs_FileOpenRead(o, "comap.dat");
	if(!f)
		return 0;
	Fs_FileRead(f, buf, 0x404);
	Fs_FileClose(f);
	return 1;
}

/* comap.dat is valid when its first dword is the magic 0x33d4f19c and the
 * dword it selects, dword[1 + dword[0x128 / 4]], XOR dword[0x74 / 4]
 * equals the machine key.  1 when it is. */
static int Registration_Verify(OpenBGI_t* o)
{
	uint8_t buf[0x408];
	uint32_t magic, idx, key;
	memset(buf, 0, sizeof buf);
	if(!ReadComapDat(o, buf))
		return 0;
	memcpy(&magic, buf, 4);
	if(magic != 0x33d4f19cu)
		return 0;
	memcpy(&idx, buf + 0x128, 4);
	if(idx >= 0x100) // the original indexes with the file's value unchecked; stay inside the buffer
		return 0;
	memcpy(&key, buf + 4 + idx * 4, 4);
	{
		uint32_t x;
		memcpy(&x, buf + 0x74, 4);
		key ^= x;
	}
	return key == GetMachineKey(o);
}

/* "80 EC": whether the game is registered for this machine: 1 when
 * comap.dat holds its key, 0 otherwise.  The original runs the game's
 * registration program (reg.exe, which wants the disc) when the file is
 * missing or wrong and waits about ten seconds for it to write the file;
 * OpenBGI does not start Windows programs, so a missing registration is
 * simply 0, and the boot program then asks for the disc.
 * --no-disc-check (o->engine.noDiscCheck) counts the game as registered
 * without looking at comap.dat.  Either outcome is said once on stderr. */
int CheckRegistration(OpenBGI_t* o)
{
	int* said = &o->install.registrationSaid;
	if(o->engine.noDiscCheck)
	{
		if(!(*said)++)
			fprintf(stderr, "openbgi: the registration check (comap.dat) is skipped (--no-disc-check)\n");
		return 1;
	}
	if(Registration_Verify(o) == 1)
		return 1;
	if(!(*said)++)
		fprintf(stderr, "openbgi: the game is not registered for this machine (comap.dat), so it asks for its disc; "
						"--no-disc-check skips the check\n");
	return 0;
}
