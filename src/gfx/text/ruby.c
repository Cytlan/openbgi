/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ruby.c - the ruby (furigana) dictionary and the ruby font settings
 *          (src/include/bgi/gfx/text.h)
 *
 * The dictionary maps a word to its reading; the layout looks every word
 * up (RubyDict_MatchPrefix) while placing text and sets the reading in
 * small glyphs above (horizontal) or beside (vertical) the word.  Scripts
 * fill the global dictionary o->text.rubyDict with "91 94" one word at a
 * time or with "91 96" from a list string; the immediate layouts ("91 91",
 * "91 9C" and friends) work on a private dictionary built from the
 * "word\reading" lines "91 95" (Text_ParseTags) extracted from the string.
 */
#include "text_internal.h"
#include "bgi/core.h"

// ---- the ruby dictionary -----------------------------------------------------------------------

// store `reading` in the node: the string, its character count and the per-character codes
static void FillReading(OpenBGI_t* o, RubyNode_t* n, const char* reading)
{
	n->readingLen = (int32_t)strlen(reading) + 1;
	n->readingChars = SjisCodes(o, NULL, reading);
	n->reading = (char*)BGI_Alloc((size_t)n->readingLen);
	n->readingCodes = (int32_t*)BGI_Alloc((size_t)n->readingChars * 4);
	strcpy(n->reading, reading);
	SjisCodes(o, n->readingCodes, reading);
}

/* add a word, or give an existing one a new reading.  The list is kept
 * sorted by word length, longest first, so that the prefix match below
 * prefers the longest entry; an equally long word goes in front of the
 * existing ones. */
void RubyDict_Insert(OpenBGI_t* o, RubyDict_t* d, const char* word, const char* reading)
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
			FillReading(o, n, reading);
			return;
		}
	}
	len = (int32_t)strlen(word) + 1;
	prev = d;
	for(n = d->next; n && len < n->wordLen; n = n->next)
		prev = n;

	n = (RubyNode_t*)BGI_Alloc(sizeof(RubyNode_t));
	n->wordLen = len;
	n->wordChars = SjisCodes(o, NULL, word);
	n->word = (char*)BGI_Alloc((size_t)len);
	strcpy(n->word, word);
	FillReading(o, n, reading);
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
int RubyDict_ParseList(OpenBGI_t* o, RubyDict_t* d, const char* list)
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
		RubyDict_Insert(o, d, word, reading);
	}
	return *p == 0;
}

// the same operations on the script's dictionary o->text.rubyDict ("91 94", "91 96")

void Ruby_Add(OpenBGI_t* o, const char* word, const char* reading)
{
	RubyDict_Insert(o, &o->text.rubyDict, word, reading);
}

int Ruby_AddList(OpenBGI_t* o, const char* list)
{
	return RubyDict_ParseList(o, &o->text.rubyDict, list);
}

int Ruby_Remove(OpenBGI_t* o, const char* word)
{
	return RubyDict_Remove(&o->text.rubyDict, word);
}

void Ruby_Clear(OpenBGI_t* o)
{
	RubyDict_Clear(&o->text.rubyDict);
}

/* "91 95": walk `str`; every position where a word of o->text.rubyDict
 * starts adds a "word\reading\n" line to `tags` (0x400 bytes).  The
 * remaining characters of a matched word are skipped so nested words
 * are not reported.  Returns the number of lines written. */
int Text_ParseTags(OpenBGI_t* o, char* tags, const char* str)
{
	int count = 0, remaining = 0;
	char word[0x400];

	while(*str)
	{
		int len = TextCharLen(o, str);

		if(remaining <= 0)
		{
			if(RubyDict_MatchPrefix(word, str, &o->text.rubyDict))
			{
				RubyNode_t entry;
				count++;
				RubyDict_Find(&entry, word, &o->text.rubyDict);
				sprintf(tags, "%s\\%s\n", entry.word, entry.reading);
				tags += strlen(tags);
				remaining = SjisCodes(o, NULL, word) - 1;
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

void Text_SetRubyFont(OpenBGI_t* o, const char* face, int size, int widthPct, int dx, int dy)
{
	if(face)
		snprintf(o->text.rubyFace, sizeof o->text.rubyFace, "%s", face);
	else
		o->text.rubyFace[0] = 0;
	o->text.rubySizeFixed = size;
	o->text.rubyWidthPct = widthPct;
	o->text.rubyDx = dx;
	o->text.rubyDy = dy;
}

// the ruby size for a base font of `size`: o->text.rubySizePct percent, at least 4 pixels
int Text_RubySize(OpenBGI_t* o, int size)
{
	int r = size * o->text.rubySizePct / 100;
	return r < 4 ? 4 : r;
}
