/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * machine.c - the machine key and the registration check (inc/bgi/install.h)
 *
 * The games of the time tied their registration to the machine: a 32-bit
 * key from the CPU signature, the Windows version and the first letters
 * of the user and computer names ("80 EE", "80 EF"), checked against the
 * file "comap.dat" that the game's registration program writes ("80 EC",
 * "80 ED").  The platform facts come from the OS layer (OS_Version,
 * OS_CpuSignature, OS_UserName, OS_ComputerName).
 */
#include "install_internal.h"
#include "bgi/os.h"

/* A code for the Windows version: 0x1f Win32s, 0x5f / 0x62 / 0x63 Windows
 * 95 / 98 / ME, 3 / 4 NT 3 / 4, 0x7d0 / 0x7d1 2000 / XP, 0x1770 Vista and
 * later, 0 unknown.  Only OsKeyBits reads it. */
static int OsVersionCode(void)
{
	int platform, major, minor;
	if(!OS_Version(&platform, &major, &minor))
		return 0;
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
static uint32_t CpuSignature(void)
{
	return OS_CpuSignature() & 0xfffu;
}

// the Windows version as bits 12..15 of the key (0xf000 for an unknown version)
static uint32_t OsKeyBits(void)
{
	switch(OsVersionCode())
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
uint32_t GetMachineKey(void)
{
	char user[0x100], computer[0x100];
	uint32_t key;
	memset(user, 0, sizeof user);
	memset(computer, 0, sizeof computer);
	OS_UserName(user, sizeof user);
	OS_ComputerName(computer, 0x10); // only 16 bytes of the computer name are asked for; the first is all that is used
	key = CpuSignature() | OsKeyBits();
	key = (key << 8) | (uint8_t)user[0];
	key = (key << 8) | (uint8_t)computer[0];
	return key;
}

// "80 EF": the machine key XOR x
uint32_t GetMachineKeyXor(uint32_t x)
{
	return GetMachineKey() ^ x;
}

/* "80 ED": the first 0x404 bytes of "comap.dat" in the current directory
 * into buf; 1 when the file could be opened (a shorter file fills less). */
int ReadComapDat(void* buf)
{
	OsFile_t* f = OS_FileOpenRead("comap.dat");
	if(!f)
		return 0;
	OS_FileRead(f, buf, 0x404);
	OS_FileClose(f);
	return 1;
}

/* comap.dat is valid when its first dword is the magic 0x33d4f19c and the
 * dword it selects, dword[1 + dword[0x128 / 4]], XOR dword[0x74 / 4]
 * equals the machine key.  1 when it is. */
static int Registration_Verify(void)
{
	uint8_t buf[0x408];
	uint32_t magic, idx, key;
	memset(buf, 0, sizeof buf);
	if(!ReadComapDat(buf))
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
	return key == GetMachineKey();
}

/* "80 EC": verify the registration; when it is missing, run "reg.exe"
 * (the game's registration program, in the current directory) and give it
 * about ten seconds, checking once a second, to write comap.dat.  1 when
 * registered, 0 when it still is not. */
int CheckRegistration(void)
{
	int i;
	if(Registration_Verify() == 1)
		return 1;
	OS_ShellExecute("reg.exe", NULL, NULL);
	OS_SleepMs(1000);
	for(i = 0; i < 10; i++)
	{
		OS_SleepMs(1000);
		if(Registration_Verify() == 1)
			return 1;
	}
	return 0;
}
