/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/win32wide.c - unit test of the Win32 back end's string conversions
 *                     (src/os/win32/wide.c, declared in inc/bgi/os_win32.h)
 *                     run on a host; built on its own by the Makefile as
 *                     bin/test_win32wide, against the stub windows.h of
 *                     tools/win32stub/ and with 16-bit wchar_t
 *
 * wide.c needs six functions of Windows: the two code page converters and,
 * for its helpers, a message box and an EDIT control's text.  They are
 * mocked here - the converters with iconv (CP932 and UTF-8 against
 * UTF-16LE) and the calling conventions of the real ones: a length of -1
 * takes the NUL along, a size of 0 asks for the size needed, a buffer too
 * small is a failure, and a character the code page lacks becomes the
 * default character with the "used default" flag set.  The best-fit
 * tables of Windows are not reproduced, so what is tested is wide.c's
 * own logic, not the code page.
 *
 * The groups of tests, in the order they run:
 *
 *   to wide    - Shift-JIS to UTF-16, NULL, a buffer too small, UTF-8 text
 *                mode with its mixed strings and a surrogate pair
 *   from wide  - UTF-16 to Shift-JIS and to the text encoding, cut at a
 *                character boundary; the byte count of a text
 *   paths      - a path code page 932 can spell converts both ways
 *                unchanged; one it cannot gets '_' and an alias, which
 *                Win32_ToWide resolves again - in any case, with either
 *                separator, for the directory and everything below it,
 *                and not for a name that merely starts the same
 *   command    - the command line: the program name dropped, an option's
 *                value and a quoted path converted as paths of their own
 *   helpers    - the message box's strings and the byte limit of an EDIT
 *                control
 *
 * Japanese text is written as escapes (SJ_TAYUTAMA and W_TAYUTAMA below
 * are "タユタマ", Tayutama, in Shift-JIS and in UTF-16); W_BJORN is "Bjørn",
 * a name with a letter code page 932 does not have.
 */
#include "bgi/os_win32.h"

#include "bgi/os.h"
#include "bgi/os_common.h"

#include <errno.h>
#include <iconv.h>

static int gFails; // the number of failed checks so far
// evaluate a condition; print it with its location and count a failure when it is false
#define CHECK(c)                                                  \
	do                                                            \
	{                                                             \
		if(!(c))                                                  \
		{                                                         \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			gFails++;                                             \
		}                                                         \
	} while(0)

// ---- the mock of Windows -----------------------------------------------------------------------

static const char* CodePageName(UINT cp)
{
	return cp == CP_UTF8 ? "UTF-8" : "CP932";
}

int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR src, int cb, LPWSTR dst, int cch)
{
	iconv_t cd = iconv_open("UTF-16LE", CodePageName(cp));
	WCHAR buf[0x2000];
	char* in = (char*)src;
	char* out = (char*)buf;
	size_t inLeft = cb < 0 ? strlen(src) + 1 : (size_t)cb, outLeft = sizeof buf;
	int n;
	BGI_UNUSED(flags);
	while(inLeft)
	{
		if(iconv(cd, &in, &inLeft, &out, &outLeft) != (size_t)-1)
			break;
		if(errno == E2BIG)
			break;
		// an invalid or cut-off character: the code page's default (a middle dot for 932), one byte taken
		*(WCHAR*)out = (WCHAR)(cp == CP_UTF8 ? 0xfffd : 0x30fb);
		out += 2;
		outLeft -= 2;
		in++;
		inLeft--;
	}
	iconv_close(cd);
	n = (int)((out - (char*)buf) / 2);
	if(cch == 0)
		return n;
	if(n > cch)
		return 0;
	memcpy(dst, buf, (size_t)n * 2);
	return n;
}

int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src, int cch, LPSTR dst, int cb, LPCSTR def, BOOL* used)
{
	iconv_t cd = iconv_open(CodePageName(cp), "UTF-16LE");
	char buf[0x4000];
	char* in = (char*)src;
	char* out = buf;
	size_t inLeft, outLeft = sizeof buf;
	int n;
	BGI_UNUSED(flags);
	if(cch < 0)
	{
		cch = 0;
		while(src[cch])
			cch++;
		cch++;
	}
	inLeft = (size_t)cch * 2;
	if(used)
		*used = FALSE;
	while(inLeft)
	{
		if(iconv(cd, &in, &inLeft, &out, &outLeft) != (size_t)-1)
			break;
		if(errno == E2BIG)
			break;
		// a character the code page lacks: the default character, one unit taken
		*out++ = def ? def[0] : '?';
		outLeft--;
		if(used)
			*used = TRUE;
		in += 2;
		inLeft -= 2;
	}
	iconv_close(cd);
	n = (int)(out - buf);
	if(cb == 0)
		return n;
	if(n > cb)
		return 0;
	memcpy(dst, buf, (size_t)n);
	return n;
}

// the message box: what it was last asked to show
static WCHAR gBoxText[0x200], gBoxCaption[0x200];
static int gBoxHasCaption;

static void WCopy(WCHAR* dst, const WCHAR* src)
{
	while((*dst++ = *src++) != 0)
	{
	}
}

int MessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT flags)
{
	BGI_UNUSED(owner);
	BGI_UNUSED(flags);
	WCopy(gBoxText, text);
	gBoxHasCaption = caption != NULL;
	if(caption)
		WCopy(gBoxCaption, caption);
	return 1;
}

// the EDIT control: its text and the last selection it was given
static WCHAR gEditText[0x400];
static int gEditSel = -1, gEditSets;

int GetWindowTextW(HWND hwnd, LPWSTR out, int cap)
{
	int n = 0;
	BGI_UNUSED(hwnd);
	while(gEditText[n] && n < cap - 1)
	{
		out[n] = gEditText[n];
		n++;
	}
	out[n] = 0;
	return n;
}

BOOL SetWindowTextW(HWND hwnd, LPCWSTR text)
{
	BGI_UNUSED(hwnd);
	WCopy(gEditText, text);
	gEditSets++;
	return TRUE;
}

LRESULT SendMessageW(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	BGI_UNUSED(hwnd);
	BGI_UNUSED(lp);
	if(msg == EM_SETSEL)
		gEditSel = (int)wp;
	return 0;
}

// ---- helpers -----------------------------------------------------------------------------------

static int WEqual(const WCHAR* a, const WCHAR* b)
{
	while(*a && *a == *b)
	{
		a++;
		b++;
	}
	return *a == *b;
}

// Win32_ToWide of s equals w
static int Wide(const char* s, const WCHAR* w)
{
	WCHAR out[0x400];
	Win32_ToWide(s, out, (int)BGI_COUNTOF(out));
	return WEqual(out, w);
}

// Win32_PathFromWide of w equals s
static int Path(const WCHAR* w, const char* s)
{
	char out[0x400];
	Win32_PathFromWide(w, out, (int)sizeof out);
	return strcmp(out, s) == 0;
}

#define SJ_TAYUTAMA "\x83\x5e\x83\x86\x83\x5e\x83\x7d" // "タユタマ" (Tayutama), four characters of two bytes
#define W_TAYUTAMA  L"\x30bf\x30e6\x30bf\x30de"
#define SJ_SOFT     "\x83\x5c\x83\x74\x83\x67" // "ソフト" (soft): its first character ends in the byte of the backslash
#define W_SOFT      L"\x30bd\x30d5\x30c8"
#define W_BJORN     L"Bj\x00f8rn" // "Bjørn"

// ---- the groups --------------------------------------------------------------------------------

static void TestToWide(void)
{
	WCHAR out[8];
	WCHAR* dup;
	puts("to wide");
	CHECK(Wide("abc", L"abc"));
	CHECK(Wide(SJ_TAYUTAMA, W_TAYUTAMA));
	CHECK(Wide("A" SJ_TAYUTAMA "\\x", L"A" W_TAYUTAMA L"\\x"));
	CHECK(Wide(NULL, L""));
	CHECK(Wide("", L""));
	// a buffer of three units: two whole characters and the NUL
	CHECK(Win32_ToWide(SJ_TAYUTAMA, out, 3) == 2);
	CHECK(out[0] == 0x30bf && out[1] == 0x30e6 && out[2] == 0);
	CHECK(Win32_ToWide("abc", out, 1) == 0 && out[0] == 0);
	dup = Win32_ToWideDup(SJ_TAYUTAMA);
	CHECK(dup && WEqual(dup, W_TAYUTAMA));
	free(dup);
	CHECK(Win32_ToWideDup(NULL) == NULL);

	// UTF-8 text mode: a well-formed sequence is taken as it is, the rest is still Shift-JIS
	OS_SetTextEncoding(1);
	CHECK(Wide("A\xc3\xa9Z", L"A\x00e9Z"));                              // e with acute
	CHECK(Wide("\xe3\x82\xbf\xe3\x83\xa6", L"\x30bf\x30e6"));            // "タユ" (ta-yu)
	CHECK(Wide("\xf0\x9f\x98\x80", L"\xd83d\xde00"));                    // U+1F600 as a surrogate pair
	CHECK(Wide("\xe3\x82\xbf\x83\x5ex", L"\x30bf\x30bfx"));              // the same character in UTF-8, then in Shift-JIS, then an "x"
	CHECK(Win32_ToWide("\xf0\x9f\x98\x80", out, 2) == 0 && out[0] == 0); // half a pair does not fit
	OS_SetTextEncoding(0);
	CHECK(!Wide("\xe3\x82\xbf", L"\x30bf")); // outside UTF-8 mode those bytes are Shift-JIS
}

static void TestFromWide(void)
{
	char out[0x100];
	puts("from wide");
	CHECK(Win32_FromWide(W_TAYUTAMA, out, sizeof out) == 8 && strcmp(out, SJ_TAYUTAMA) == 0);
	CHECK(Win32_FromWide(L"", out, sizeof out) == 0 && out[0] == 0);
	CHECK(Win32_FromWide(NULL, out, sizeof out) == 0 && out[0] == 0);
	// five bytes hold two characters and the NUL, four only one: a character is never cut in two
	CHECK(Win32_FromWide(W_TAYUTAMA, out, 5) == 4 && strcmp(out, "\x83\x5e\x83\x86") == 0);
	CHECK(Win32_FromWide(W_TAYUTAMA, out, 4) == 2 && strcmp(out, "\x83\x5e") == 0);
	CHECK(Win32_FromWide(W_TAYUTAMA, out, 1) == 0 && out[0] == 0);
	CHECK(Win32_TextFromWide(W_TAYUTAMA, out, sizeof out) == 8);
	CHECK(Win32_TextBytes(W_TAYUTAMA, 4) == 8 && Win32_TextBytes(L"ab", 2) == 2 && Win32_TextBytes(L"ab", 0) == 0);
	OS_SetTextEncoding(1);
	CHECK(Win32_TextFromWide(L"\x30bf\x30e6", out, sizeof out) == 6 && strcmp(out, "\xe3\x82\xbf\xe3\x83\xa6") == 0);
	CHECK(Win32_TextFromWide(L"\x30bf\x30e6", out, 6) == 3); // one character of three bytes
	CHECK(Win32_TextBytes(W_TAYUTAMA, 4) == 12);
	CHECK(Win32_FromWide(L"\x30bf", out, sizeof out) == 2); // this one stays Shift-JIS
	OS_SetTextEncoding(0);
	CHECK(Win32_WideToUtf8(W_BJORN, out, sizeof out) == 6 && strcmp(out, "Bj\xc3\xb8rn") == 0);
	{
		WCHAR w[16];
		CHECK(Win32_Utf8ToWide("Bj\xc3\xb8rn", w, 16) == 5 && WEqual(w, W_BJORN));
		CHECK(Win32_Utf8ToWide(NULL, w, 16) == 0 && w[0] == 0);
	}
}

static void TestPaths(void)
{
	char out[0x100];
	int i;
	puts("paths");
	// what code page 932 can spell goes both ways unchanged and leaves no alias behind
	CHECK(Path(L"C:\\Games\\" W_TAYUTAMA L"\\system.arc", "C:\\Games\\" SJ_TAYUTAMA "\\system.arc"));
	CHECK(Wide("C:\\Games\\" SJ_TAYUTAMA "\\system.arc", L"C:\\Games\\" W_TAYUTAMA L"\\system.arc"));
	CHECK(Path(L"", "") && Path(NULL, "") && Path(L"C:\\", "C:\\") && Path(L"a/b\\c", "a/b\\c"));
	CHECK(Path(L"C:\\" W_SOFT L"\\x", "C:\\" SJ_SOFT "\\x")); // a second byte 0x5c is not a separator

	// a relative name it cannot spell: '_' for the letter, and nothing to remember
	CHECK(Path(W_BJORN L"\\x", "Bj_rn\\x"));
	CHECK(Wide("Bj_rn\\x", L"Bj_rn\\x"));

	// an absolute path: the directory up to the unspellable component becomes an alias
	CHECK(Wide("C:\\Users\\Bj_rn\\Game\\", L"C:\\Users\\Bj_rn\\Game\\")); // not yet
	CHECK(Path(L"C:\\Users\\" W_BJORN L"\\Game\\", "C:\\Users\\Bj_rn\\Game\\"));
	CHECK(Wide("C:\\Users\\Bj_rn\\Game\\", L"C:\\Users\\" W_BJORN L"\\Game\\"));
	CHECK(Wide("C:\\Users\\Bj_rn", L"C:\\Users\\" W_BJORN)); // the directory itself
	CHECK(Wide("C:\\Users\\Bj_rn\\Game\\" SJ_TAYUTAMA ".arc", L"C:\\Users\\" W_BJORN L"\\Game\\" W_TAYUTAMA L".arc"));
	CHECK(Wide("c:\\users\\bj_rn\\game\\system.arc", L"C:\\Users\\" W_BJORN L"\\game\\system.arc")); // the engine lower-cases paths
	CHECK(Wide("C:/Users/Bj_rn/x", L"C:\\Users\\" W_BJORN L"/x"));                                   // either separator
	CHECK(Wide("\"C:\\Users\\Bj_rn\" x", L"\"C:\\Users\\" W_BJORN L"\" x"));                         // inside other text, quoted
	CHECK(Wide("C:\\Users\\Bj_rn2\\x", L"C:\\Users\\Bj_rn2\\x"));                                    // another directory
	CHECK(Wide("C:\\Users\\Bj_r", L"C:\\Users\\Bj_r"));
	CHECK(Wide("ZC:\\Users\\Bj_rn\\x", L"ZC:\\Users\\Bj_rn\\x")); // not the start of a path
	CHECK(Wide("C:\\Users\\Other\\x", L"C:\\Users\\Other\\x"));
	// the way back gives the same spelling again
	CHECK(Path(L"C:\\Users\\" W_BJORN L"\\Game\\save\\a.dat", "C:\\Users\\Bj_rn\\Game\\save\\a.dat"));

	// two such components: each level is known, so a path cut back to the upper one still resolves
	CHECK(Path(L"D:\\" W_BJORN L"\\Spiele\\M\x00fcller\\game", "D:\\Bj_rn\\Spiele\\M_ller\\game"));
	CHECK(Wide("D:\\Bj_rn\\Spiele\\M_ller\\game\\x", L"D:\\" W_BJORN L"\\Spiele\\M\x00fcller\\game\\x"));
	CHECK(Wide("D:\\Bj_rn\\Spiele", L"D:\\" W_BJORN L"\\Spiele"));
	CHECK(Wide("D:\\Bj_rn", L"D:\\" W_BJORN));

	// a spellable component whose second byte is 0x5c in front of the alias, and a network path
	CHECK(Path(L"C:\\" W_SOFT L"\\" W_BJORN L"\\x", "C:\\" SJ_SOFT "\\Bj_rn\\x"));
	CHECK(Wide("C:\\" SJ_SOFT "\\Bj_rn\\x", L"C:\\" W_SOFT L"\\" W_BJORN L"\\x"));
	CHECK(Path(L"\\\\srv\\share\\" W_BJORN L"\\x", "\\\\srv\\share\\Bj_rn\\x"));
	CHECK(Wide("\\\\srv\\share\\Bj_rn\\y", L"\\\\srv\\share\\" W_BJORN L"\\y"));

	// a buffer too small ends the path at a component, never inside a character
	CHECK(Win32_PathFromWide(L"C:\\Games\\" W_TAYUTAMA L"\\x", out, 14) == 9 && strcmp(out, "C:\\Games\\") == 0);
	CHECK(Win32_PathFromWide(L"C:\\Games", out, 1) == 0 && out[0] == 0);

	// more aliases than the table holds: the oldest give way, the newest still resolve
	for(i = 0; i < 80; i++)
	{
		WCHAR w[32] = L"E:\\d00\\" W_BJORN;
		w[4] = (WCHAR)('0' + i / 10);
		w[5] = (WCHAR)('0' + i % 10);
		Win32_PathFromWide(w, out, (int)sizeof out);
	}
	CHECK(Wide("E:\\d79\\Bj_rn\\x", L"E:\\d79\\" W_BJORN L"\\x"));
	CHECK(Wide("E:\\d60\\Bj_rn\\x", L"E:\\d60\\" W_BJORN L"\\x"));
	CHECK(Wide("E:\\d20\\Bj_rn\\x", L"E:\\d20\\" W_BJORN L"\\x"));
	CHECK(Wide("C:\\Users\\Bj_rn\\Game\\", L"C:\\Users\\Bj_rn\\Game\\")); // the first one of all is gone
	CHECK(Path(L"C:\\Users\\" W_BJORN L"\\Game\\", "C:\\Users\\Bj_rn\\Game\\"));
	CHECK(Wide("C:\\Users\\Bj_rn\\Game\\", L"C:\\Users\\" W_BJORN L"\\Game\\")); // and known again once Windows names it again
}

static void TestCommandLine(void)
{
	char out[0x400];
	puts("command");
	CHECK(Win32_CmdLineFromWide(L"bgi.exe", out, sizeof out) == 0 && out[0] == 0);
	CHECK(Win32_CmdLineFromWide(NULL, out, sizeof out) == 0);
	Win32_CmdLineFromWide(L"bgi.exe  --list-engines", out, sizeof out);
	CHECK(strcmp(out, "--list-engines") == 0);
	Win32_CmdLineFromWide(L"\"C:\\Program Files\\OpenBGI\\bgi.exe\" --engine=1.69/444  \"Do not use mutex.\" boot.arc", out, sizeof out);
	CHECK(strcmp(out, "--engine=1.69/444  \"Do not use mutex.\" boot.arc") == 0);
	Win32_CmdLineFromWide(L"bgi.exe " W_TAYUTAMA L".arc", out, sizeof out);
	CHECK(strcmp(out, SJ_TAYUTAMA ".arc") == 0);
	// an option's value and a quoted path with a blank are paths of their own: both get an alias
	Win32_CmdLineFromWide(L"\"F:\\x\\bgi.exe\" --games=F:\\" W_BJORN L"\\g.json \"G:\\My " W_BJORN L"\\boot dir\\b.arc\" --raw", out, sizeof out);
	CHECK(strcmp(out, "--games=F:\\Bj_rn\\g.json \"G:\\My Bj_rn\\boot dir\\b.arc\" --raw") == 0);
	CHECK(Wide("F:\\Bj_rn\\g.json", L"F:\\" W_BJORN L"\\g.json"));
	CHECK(Wide("G:\\My Bj_rn\\boot dir\\b.arc", L"G:\\My " W_BJORN L"\\boot dir\\b.arc"));
	// an argument that does not fit the buffer is left out whole
	CHECK(Win32_CmdLineFromWide(L"bgi.exe ab cdefgh", out, 5) == 3 && strcmp(out, "ab ") == 0);
}

static void TestHelpers(void)
{
	HWND edit = (HWND)gEditText; // any non-NULL handle
	puts("helpers");
	Win32_MessageBox(NULL, SJ_TAYUTAMA, "Error!!", 0);
	CHECK(WEqual(gBoxText, W_TAYUTAMA) && gBoxHasCaption && WEqual(gBoxCaption, L"Error!!"));
	Win32_MessageBox(NULL, NULL, NULL, 0);
	CHECK(gBoxText[0] == 0 && !gBoxHasCaption);

	// four characters of two bytes each against a limit of six bytes: three stay, the caret goes behind them
	WCopy(gEditText, W_TAYUTAMA);
	gEditSets = 0;
	Win32_EditClamp(edit, 6);
	CHECK(WEqual(gEditText, L"\x30bf\x30e6\x30bf") && gEditSel == 3 && gEditSets == 1);
	Win32_EditClamp(edit, 6); // within the limit: left alone
	CHECK(gEditSets == 1);
	Win32_EditClamp(edit, 5); // an odd limit holds two characters
	CHECK(WEqual(gEditText, L"\x30bf\x30e6") && gEditSel == 2);
	Win32_EditClamp(edit, 0); // no limit
	CHECK(WEqual(gEditText, L"\x30bf\x30e6"));
	WCopy(gEditText, L"ab\xd83d\xde00"); // a surrogate pair goes as a whole
	OS_SetTextEncoding(1);
	Win32_EditClamp(edit, 5);
	OS_SetTextEncoding(0);
	CHECK(WEqual(gEditText, L"ab") && gEditSel == 2);
}

// run every group; exit status 1 when any check failed
int main(void)
{
	TestToWide();
	TestFromWide();
	TestPaths();
	TestCommandLine();
	TestHelpers();
	puts(gFails ? "FAILED" : "all passed");
	return gFails ? 1 : 0;
}
