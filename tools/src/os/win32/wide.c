/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wide.c - the engine's strings as UTF-16 and back, for the Win32 back end
 *          (inc/bgi/os_win32.h): the conversions every other file of the
 *          back end puts around its Windows calls, the path aliases, the
 *          command line, and two helpers built on them (the message box
 *          and the byte limit of an EDIT control)
 *
 * The engine keeps its strings in Shift-JIS, as the original does, and the
 * original hands them to the ANSI entry points of Windows, which read them
 * in the system's code page: correct on a Japanese system only.  This back
 * end calls the wide-character entry points instead and converts with code
 * page 932 itself, so a title, a message or a file name means the same on
 * every system.
 *
 * Engine to Windows (Win32_ToWide) is exact.  The other direction is not
 * always: a directory may carry a name code page 932 cannot spell.  For
 * text the code page's substitute character will do (Win32_FromWide), but
 * a path has to stay usable, so Win32_PathFromWide converts component by
 * component, writes '_' for what is missing and remembers the result as a
 * path alias - the substitute spelling of the directory against its real
 * name.  Win32_ToWide puts the real name back wherever it meets an alias,
 * which is how a game installed under such a directory still finds its
 * files: the engine works with "C:\Users\Bj_rn\Game\" and Windows is asked
 * for "C:\Users\Bjørn\Game\".
 */
#include "bgi/os_win32.h"

#include "bgi/os.h"
#include "bgi/os_common.h"

// ---- engine string -> UTF-16 -------------------------------------------------------------------

/* the bytes of the character at s (`left` bytes remain): a well-formed
 * UTF-8 sequence in UTF-8 text mode (*utf8 = 1), else two for a Shift-JIS
 * lead byte with its second byte, else one */
static int CharBytes(const uint8_t* s, int left, int* utf8)
{
	*utf8 = 0;
	if(gOsTextUtf8)
	{
		int n = OsCommon_Utf8SeqLen(s);
		if(n && n <= left)
		{
			*utf8 = 1;
			return n;
		}
	}
	if(left >= 2 && OsCommon_SjisIsLead(s[0]) && s[1])
		return 2;
	return 1;
}

/* `len` bytes of an engine string as UTF-16 into out (cap units, no
 * terminator); the units written.  Outside UTF-8 text mode the run goes
 * through MultiByteToWideChar in one piece; a run that does not fit, and
 * every run in UTF-8 text mode, is converted character by character, as
 * far as the buffer holds whole characters. */
static int ConvRun(const char* s, int len, WCHAR* out, int cap)
{
	int i = 0, o = 0;
	if(len <= 0 || cap <= 0)
		return 0;
	if(!gOsTextUtf8)
	{
		int n = MultiByteToWideChar(WIN32_CP_SJIS, 0, s, len, out, cap);
		if(n > 0)
			return n;
	}
	while(i < len)
	{
		const uint8_t* u = (const uint8_t*)s + i;
		int utf8;
		int n = CharBytes(u, len - i, &utf8);
		WCHAR w[2];
		int wn = 1;
		if(utf8)
		{
			int used;
			uint32_t cp = OsCommon_Utf8Decode(u, &used);
			if(cp >= 0x10000)
			{
				w[0] = (WCHAR)(0xd800 + ((cp - 0x10000) >> 10));
				w[1] = (WCHAR)(0xdc00 + ((cp - 0x10000) & 0x3ff));
				wn = 2;
			}
			else
				w[0] = (WCHAR)cp;
		}
		else
		{
			wn = MultiByteToWideChar(WIN32_CP_SJIS, 0, (const char*)u, n, w, 2);
			if(wn <= 0)
			{ // no code page 932 on this system: ASCII still means itself
				w[0] = (WCHAR)(u[0] < 0x80 ? u[0] : 0xfffd);
				wn = 1;
			}
		}
		if(o + wn > cap)
			break;
		out[o++] = w[0];
		if(wn == 2)
			out[o++] = w[1];
		i += n;
	}
	return o;
}

// ---- path aliases ------------------------------------------------------------------------------

// a directory whose name code page 932 cannot spell: the spelling the engine was given against the real one (neither ends in a separator)
typedef struct PathAlias
{
	char* alias;  // Shift-JIS, '_' where a character is missing
	int aliasLen; // its bytes
	WCHAR* real;  // what Windows calls the directory
	int realLen;  // its UTF-16 units
} PathAlias_t;

#define ALIAS_MAX 64
static PathAlias_t gAliases[ALIAS_MAX];
static int gAliasCount; // the entries in use
static int gAliasNext;  // the entry replaced next once the table is full

static int IsSeparator(int c)
{
	return c == '\\' || c == '/';
}

static int AsciiLower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

// the alias spelled at p (ASCII letters in either case, either separator), ending at a component's end; the longest one, NULL for none
static const PathAlias_t* AliasAt(const char* p)
{
	const PathAlias_t* best = NULL;
	int i, k;
	for(i = 0; i < gAliasCount; i++)
	{
		const PathAlias_t* a = &gAliases[i];
		int end;
		if(!a->alias || (best && a->aliasLen <= best->aliasLen))
			continue;
		for(k = 0; k < a->aliasLen; k++)
		{
			int c = (uint8_t)p[k], d = (uint8_t)a->alias[k];
			if(!c)
				break;
			if(AsciiLower(c) != AsciiLower(d) && !(IsSeparator(c) && IsSeparator(d)))
				break;
		}
		if(k < a->aliasLen)
			continue;
		// "C:\Bj_rn" must not match in "C:\Bj_rn2": the component ends here (a quote closes a quoted path)
		end = (uint8_t)p[k];
		if(end && !IsSeparator(end) && end != '"')
			continue;
		best = a;
	}
	return best;
}

// remember that the first aliasLen bytes of `alias` stand for the first realLen units of `real`; a spelling already known keeps its entry
static void AliasAdd(const char* alias, int aliasLen, const WCHAR* real, int realLen)
{
	PathAlias_t* a;
	int i;
	for(i = 0; i < gAliasCount; i++)
		if(gAliases[i].aliasLen == aliasLen && memcmp(gAliases[i].alias, alias, (size_t)aliasLen) == 0)
			break;
	if(i < gAliasCount)
		a = &gAliases[i]; // the same spelling again: the newer directory takes it
	else if(gAliasCount < ALIAS_MAX)
		a = &gAliases[gAliasCount++];
	else
	{
		a = &gAliases[gAliasNext];
		gAliasNext = (gAliasNext + 1) % ALIAS_MAX;
	}
	free(a->alias);
	free(a->real);
	a->alias = (char*)malloc((size_t)aliasLen + 1);
	a->real = (WCHAR*)malloc(((size_t)realLen + 1) * sizeof(WCHAR));
	if(!a->alias || !a->real)
	{ // out of memory: the entry stays empty and matches nothing
		free(a->alias);
		free(a->real);
		a->alias = NULL;
		a->real = NULL;
		a->aliasLen = a->realLen = 0x7fffffff;
		return;
	}
	memcpy(a->alias, alias, (size_t)aliasLen);
	a->alias[aliasLen] = 0;
	memcpy(a->real, real, (size_t)realLen * sizeof(WCHAR));
	a->real[realLen] = 0;
	a->aliasLen = aliasLen;
	a->realLen = realLen;
}

int Win32_ToWide(const char* s, WCHAR* out, int cap)
{
	const char* run;
	const char* p;
	const char* end;
	int o = 0;
	if(cap <= 0)
		return 0;
	if(!s)
		s = "";
	end = s + strlen(s);
	cap--; // the terminator's place
	if(!gAliasCount)
	{
		o = ConvRun(s, (int)(end - s), out, cap);
		out[o] = 0;
		return o;
	}
	/* with aliases: the text between them is converted run by run, each
	 * alias gives way to the real directory.  An alias starts a path, so
	 * it is only looked for where no letter or digit precedes it. */
	run = p = s;
	while(*p)
	{
		const PathAlias_t* a = NULL;
		int utf8;
		if(p == s || !((p[-1] >= '0' && p[-1] <= '9') || (AsciiLower(p[-1]) >= 'a' && AsciiLower(p[-1]) <= 'z')))
			a = AliasAt(p);
		if(a)
		{
			int k;
			o += ConvRun(run, (int)(p - run), out + o, cap - o);
			for(k = 0; k < a->realLen && o < cap; k++)
				out[o++] = a->real[k];
			p += a->aliasLen;
			run = p;
			continue;
		}
		p += CharBytes((const uint8_t*)p, (int)(end - p), &utf8);
	}
	o += ConvRun(run, (int)(p - run), out + o, cap - o);
	out[o] = 0;
	return o;
}

WCHAR* Win32_ToWideDup(const char* s)
{
	// a UTF-16 unit never comes from less than one byte, and an alias is never shorter than the name it stands for
	int cap;
	WCHAR* w;
	if(!s)
		return NULL;
	cap = (int)strlen(s) + 1;
	w = (WCHAR*)malloc((size_t)cap * sizeof(WCHAR));
	if(w)
		Win32_ToWide(s, w, cap);
	return w;
}

// ---- UTF-16 -> engine string -------------------------------------------------------------------

// 1 when w[0], w[1] are a surrogate pair (one character in two units)
static int IsPair(const WCHAR* w)
{
	return w[0] >= 0xd800 && w[0] < 0xdc00 && w[1] >= 0xdc00 && w[1] < 0xe000;
}

/* UTF-16 in code page `cp` into out (n bytes with the NUL): in one piece
 * when it fits, else character by character up to the last one that
 * does, so that a cut never splits a character */
static int FromWideCp(UINT cp, const WCHAR* w, char* out, int n)
{
	int o = 0;
	int len;
	if(n <= 0)
		return 0;
	if(!w)
		w = L"";
	len = WideCharToMultiByte(cp, 0, w, -1, out, n, NULL, NULL);
	if(len > 0)
		return len - 1;
	while(*w)
	{
		char mb[8];
		int units = IsPair(w) ? 2 : 1;
		len = WideCharToMultiByte(cp, 0, w, units, mb, (int)sizeof mb, NULL, NULL);
		if(len <= 0)
		{
			mb[0] = '?';
			len = 1;
		}
		if(o + len >= n)
			break;
		memcpy(out + o, mb, (size_t)len);
		o += len;
		w += units;
	}
	out[o] = 0;
	return o;
}

int Win32_FromWide(const WCHAR* w, char* out, int n)
{
	return FromWideCp(WIN32_CP_SJIS, w, out, n);
}

int Win32_TextFromWide(const WCHAR* w, char* out, int n)
{
	return FromWideCp(gOsTextUtf8 ? CP_UTF8 : WIN32_CP_SJIS, w, out, n);
}

int Win32_TextBytes(const WCHAR* w, int units)
{
	if(units <= 0)
		return 0;
	return WideCharToMultiByte(gOsTextUtf8 ? CP_UTF8 : WIN32_CP_SJIS, 0, w, units, NULL, 0, NULL, NULL);
}

int Win32_PathFromWide(const WCHAR* w, char* out, int n)
{
	int absolute, i = 0, o = 0;
	if(n <= 0)
		return 0;
	if(!w)
		w = L"";
	// "X:..." or "\\server\..": only a full path can be told apart from other text later on
	absolute = (((w[0] >= 'A' && w[0] <= 'Z') || (w[0] >= 'a' && w[0] <= 'z')) && w[1] == ':') ||
		(IsSeparator(w[0]) && IsSeparator(w[1]));
	while(w[i])
	{
		int j = i;
		while(w[j] && !IsSeparator(w[j]))
			j++;
		if(j > i)
		{
			// one component, exactly: no best-fit letters, '_' for a character the code page lacks
			BOOL lossy = FALSE;
			int len;
			if(o >= n - 1)
				break;
			len = WideCharToMultiByte(WIN32_CP_SJIS, WC_NO_BEST_FIT_CHARS, w + i, j - i, out + o, n - 1 - o, "_", &lossy);
			if(len <= 0)
				break; // it does not fit (or there is no code page 932): the path ends here
			o += len;
			if(lossy && absolute)
				AliasAdd(out, o, w, j);
		}
		if(!w[j])
			break;
		if(o >= n - 1)
			break;
		out[o++] = (char)w[j]; // the separator
		i = j + 1;
	}
	out[o] = 0;
	return o;
}

// ---- UTF-8 -------------------------------------------------------------------------------------

int Win32_Utf8ToWide(const char* utf8, WCHAR* out, int cap)
{
	int n;
	if(cap <= 0)
		return 0;
	n = MultiByteToWideChar(CP_UTF8, 0, utf8 ? utf8 : "", -1, out, cap);
	if(n <= 0)
	{
		out[0] = 0;
		return 0;
	}
	return n - 1;
}

int Win32_WideToUtf8(const WCHAR* w, char* out, int n)
{
	return FromWideCp(CP_UTF8, w, out, n);
}

// ---- the command line --------------------------------------------------------------------------

// `units` units at w as a path into out + *o (n bytes in all); a pair of double quotes around them stays outside the conversion
static void CmdPiece(const WCHAR* w, int units, char* out, int n, int* o)
{
	WCHAR piece[WIN32_WPATH];
	int quoted = units >= 1 && w[0] == '"';
	int closed = quoted && units >= 2 && w[units - 1] == '"';
	int k, len = units - quoted - closed;
	if(len > (int)BGI_COUNTOF(piece) - 1)
		len = (int)BGI_COUNTOF(piece) - 1;
	for(k = 0; k < len; k++)
		piece[k] = w[quoted + k];
	piece[len] = 0;
	if(quoted && *o < n - 1)
		out[(*o)++] = '"';
	*o += Win32_PathFromWide(piece, out + *o, n - *o);
	if(closed && *o < n - 1)
		out[(*o)++] = '"';
	out[*o] = 0;
}

int Win32_CmdLineFromWide(const WCHAR* cmd, char* out, int n)
{
	const WCHAR* p = cmd ? cmd : L"";
	int o = 0;
	if(n <= 0)
		return 0;
	out[0] = 0;
	// the program name: up to the closing quote when it starts with one, else up to the first blank
	if(*p == '"')
	{
		p++;
		while(*p && *p != '"')
			p++;
		if(*p)
			p++;
	}
	else
		while(*p && *p != ' ' && *p != '\t')
			p++;
	while(*p == ' ' || *p == '\t')
		p++;
	while(*p)
	{
		const WCHAR* start = p;
		int inQuotes = 0, eq = -1, len;
		if(*p == ' ' || *p == '\t')
		{ // the blanks between the arguments stay as they are
			if(o < n - 1)
				out[o++] = (char)*p;
			out[o] = 0;
			p++;
			continue;
		}
		while(*p && (inQuotes || (*p != ' ' && *p != '\t')))
		{
			if(*p == '"')
				inQuotes = !inQuotes;
			else if(*p == '=' && eq < 0 && !inQuotes)
				eq = (int)(p - start);
			p++;
		}
		len = (int)(p - start);
		if(len >= 2 && start[0] == '-' && start[1] == '-' && eq > 0)
		{
			// "--option=value": the name and the '=' are ASCII, the value is a string of its own
			int k;
			for(k = 0; k <= eq && o < n - 1; k++)
				out[o++] = (char)start[k];
			out[o] = 0;
			CmdPiece(start + eq + 1, len - eq - 1, out, n, &o);
		}
		else
			CmdPiece(start, len, out, n, &o);
	}
	return o;
}

// ---- helpers on top of the conversions ---------------------------------------------------------

int Win32_MessageBox(HWND owner, const char* text, const char* caption, UINT flags)
{
	WCHAR* wtext = Win32_ToWideDup(text ? text : "");
	WCHAR* wcaption = Win32_ToWideDup(caption); // NULL stays NULL: the system's default caption ("Error")
	int r = MessageBoxW(owner, wtext ? wtext : L"", wcaption, flags);
	free(wtext);
	free(wcaption);
	return r;
}

void Win32_EditClamp(HWND edit, int maxBytes)
{
	WCHAR w[WIN32_WPATH];
	int len, cut;
	if(maxBytes <= 0 || !edit)
		return;
	len = GetWindowTextW(edit, w, (int)BGI_COUNTOF(w));
	cut = len;
	while(cut > 0 && Win32_TextBytes(w, cut) > maxBytes)
	{
		cut--;
		if(cut > 0 && IsPair(w + cut - 1))
			cut--; // both halves of a pair go together
	}
	if(cut == len)
		return;
	w[cut] = 0;
	SetWindowTextW(edit, w);
	SendMessageW(edit, EM_SETSEL, (WPARAM)cut, (LPARAM)cut);
}
