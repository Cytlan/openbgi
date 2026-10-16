/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ruby.c - the ruby (furigana) dictionary and the ruby font settings
 *          (inc/bgi/gfx/text.h)
 *
 * The dictionary maps a word to its reading; the layout looks every word
 * up (RubyDict_MatchPrefix) while placing text and sets the reading in
 * small glyphs above (horizontal) or beside (vertical) the word.  Scripts
 * fill the global dictionary gRubyDict with "91 94" one word at a time or
 * with "91 96" from a list string; the immediate layouts ("91 91", "91 9C"
 * and friends) work on a private dictionary built from the "word\reading"
 * lines "91 95" (Text_ParseTags) extracted from the string.
 */
#include "text_internal.h"

// ---- the ruby dictionary -----------------------------------------------------------------------

// store `reading` in the node: the string, its character count and the per-character codes
static void FillReading(RubyNode_t* n, const char* reading)
{
	n->readingLen = (int32_t)strlen(reading) + 1;
	n->readingChars = SjisCodes(NULL, reading);
	n->reading = (char*)BGI_Alloc((size_t)n->readingLen);
	n->readingCodes = (int32_t*)BGI_Alloc((size_t)n->readingChars * 4);
	strcpy(n->reading, reading);
	SjisCodes(n->readingCodes, reading);
}

/* add a word, or give an existing one a new reading.  The list is kept
 * sorted by word length, longest first, so that the prefix match below
 * prefers the longest entry; an equally long word goes in front of the
 * existing ones. */
void RubyDict_Insert(RubyDict_t* d, const char* word, const char* reading)
{
	RubyNode_t* n;
	RubyNode_t* prev;
	int32_t len;

	for(n = d->next; n; n = n->next)
	{
		if(strcmp(word, n->word) == 0)
		{
			BGI_Free(n->reading);
			BGI_Free(n->readingCodes);
			FillReading(n, reading);
			return;
		}
	}
	len = (int32_t)strlen(word) + 1;
	prev = d;
	for(n = d->next; n && len < n->wordLen; n = n->next)
		prev = n;

	n = (RubyNode_t*)BGI_Alloc(sizeof(RubyNode_t));
	n->wordLen = len;
	n->wordChars = SjisCodes(NULL, word);
	n->word = (char*)BGI_Alloc((size_t)len);
	strcpy(n->word, word);
	FillReading(n, reading);
	n->next = prev->next;
	prev->next = n;
}

// remove the entry of `word`; 1 when there was one
int RubyDict_Remove(RubyDict_t* d, const char* word)
{
	RubyNode_t* prev = d;
	RubyNode_t* n;

	for(n = d->next; n; prev = n, n = n->next)
	{
		if(strcmp(word, n->word) == 0)
		{
			prev->next = n->next;
			BGI_Free(n->word);
			BGI_Free(n->reading);
			BGI_Free(n->readingCodes);
			BGI_Free(n);
			return 1;
		}
	}
	return 0;
}

// remove every entry and reset the head node
void RubyDict_Clear(RubyDict_t* d)
{
	while(d->next)
		RubyDict_Remove(d, d->next->word);
	d->wordLen = 0;
	d->word = NULL;
	d->readingLen = 0;
	d->reading = NULL;
	d->next = NULL;
}

// copy the entry of `word` to *out (its next pointer cleared); 1 found
int RubyDict_Find(RubyNode_t* out, const char* word, RubyDict_t* d)
{
	RubyNode_t* n;

	for(n = d->next; n; n = n->next)
	{
		if(strcmp(word, n->word) == 0)
		{
			*out = *n;
			out->next = NULL;
			return 1;
		}
	}
	return 0;
}

// does a dictionary word start at `text`? Copies it to outWord; 1 when one does
int RubyDict_MatchPrefix(char* outWord, const char* text, RubyDict_t* d)
{
	RubyNode_t* n;

	for(n = d->next; n; n = n->next)
	{
		if(strstr(text, n->word) == text)
		{
			strcpy(outWord, n->word);
			return 1;
		}
	}
	return 0;
}

/* feed "word\reading\n..." lines into the dictionary (the buffers hold
 * 0xFF bytes per word and reading; longer ones are not checked).  Returns
 * 1 when the whole list was consumed.  The original loops forever on a
 * trailing fragment without a backslash; here it stops and returns 0. */
int RubyDict_ParseList(RubyDict_t* d, const char* list)
{
	char word[0x100], reading[0x100];
	const char* p = list;

	if(!p)
		return 0;
	while(*p)
	{
		const char* sep = strstr(p, "\\");
		const char* end;
		int len, len2;

		if(!sep || (len = (int)(sep - p)) <= 0)
			break; // original: spins on the same position
		memcpy(word, p, (size_t)len);
		word[len] = 0;
		p = sep + 1;
		end = strstr(p, "\n");
		len2 = end ? (int)(end - p) : (int)strlen(p);
		if(len2 <= 0)
			continue; // "word\" with nothing behind it: the word is dropped
		memcpy(reading, p, (size_t)len2);
		reading[len2] = 0;
		p += len2 + (end ? 1 : 0);
		RubyDict_Insert(d, word, reading);
	}
	return *p == 0;
}

// the same operations on the script's dictionary gRubyDict ("91 94", "91 96")

void Ruby_Add(const char* word, const char* reading)
{
	RubyDict_Insert(&gRubyDict, word, reading);
}

int Ruby_AddList(const char* list)
{
	return RubyDict_ParseList(&gRubyDict, list);
}

int Ruby_Remove(const char* word)
{
	return RubyDict_Remove(&gRubyDict, word);
}

void Ruby_Clear(void)
{
	RubyDict_Clear(&gRubyDict);
}

/* "91 95": walk `str`; every position where a word of gRubyDict starts
 * adds a "word\reading\n" line to `tags` (0x400 bytes).  The remaining
 * characters of a matched word are skipped so nested words are not
 * reported.  Returns the number of lines written. */
int Text_ParseTags(char* tags, const char* str)
{
	int count = 0, remaining = 0;
	char word[0x400];

	while(*str)
	{
		int len = TextCharLen(str);

		if(remaining <= 0)
		{
			if(RubyDict_MatchPrefix(word, str, &gRubyDict))
			{
				RubyNode_t entry;
				count++;
				RubyDict_Find(&entry, word, &gRubyDict);
				sprintf(tags, "%s\\%s\n", entry.word, entry.reading);
				tags += strlen(tags);
				remaining = SjisCodes(NULL, word) - 1;
			}
		}
		else
			remaining--;
		str += len;
	}
	return count;
}

// ---- the ruby font -----------------------------------------------------------------------------

/* "91 97" of 1.529 on: the face, size and width the readings are set in,
 * and an offset of the ruby glyphs.  An empty face, a size of 0 or a width
 * of 0 keep what the base font gives (the base face, Text_RubySize, the
 * base width). */
char gRubyFace[0x100];
int32_t gRubySizeFixed;
int32_t gRubyWidthPct;
int32_t gRubyBold = -1;   // "92 97" (1.616 on): the readings' bold flag, -1 = the base font's
int32_t gRubyShadow = -1; // the readings' shadow colour, -1 = the style's
int32_t gRubyDx, gRubyDy; // offset of the readings in pixels

void Text_SetRubyFont(const char* face, int size, int widthPct, int dx, int dy)
{
	if(face)
		snprintf(gRubyFace, sizeof gRubyFace, "%s", face);
	else
		gRubyFace[0] = 0;
	gRubySizeFixed = size;
	gRubyWidthPct = widthPct;
	gRubyDx = dx;
	gRubyDy = dy;
}

// the ruby size for a base font of `size`: gRubySizePct percent, at least 4 pixels
int Text_RubySize(int size)
{
	int r = size * gRubySizePct / 100;
	return r < 4 ? 4 : r;
}
