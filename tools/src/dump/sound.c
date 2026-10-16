/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sound.c - the sounds of the dump and pack tools (dump_internal.h): a BW
 *           file (bgi/snd/bw.h, docs/sound.md) becomes an Ogg Vorbis or a
 *           WAV file, and that a BW file again
 *
 * A BW file of codec 3 - every sound of the games examined - is a 0x40-byte
 * header in front of an ordinary Ogg Vorbis stream, so the .ogg of the dump
 * is that stream untouched and nothing is lost either way.  The header's
 * length, sample rate and channel count are the stream's own (read back
 * from its first and last page when packing); what an Ogg file has no place
 * for is whether the sound loops and where the loop restarts, and those two
 * go into the JSON file beside it ("loop", "loop_start" in frames).
 *
 * The other codecs (4-bit ADPCM, PCM, Huffman-coded ADPCM) are decoded
 * with the engine's own streams into a 16-bit WAV.  A WAV that was edited
 * is packed as codec 1, plain PCM, which the sound library plays like the
 * others.
 */
#include "dump_internal.h"
#include "bgi/snd/bw.h"

static uint32_t Le32(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void PutLe16(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void PutLe32(uint8_t* p, uint32_t v)
{
	PutLe16(p, v);
	PutLe16(p + 2, v >> 16);
}

// ---- Ogg Vorbis -------------------------------------------------------------------------------------

typedef struct OggInfo
{
	uint32_t rate;     // Hz
	uint32_t channels; // 1 or 2
	uint32_t frames;   // the stream's length: the granule position of its last page
} OggInfo_t;

/* What the BW header says about a Vorbis stream, read from the stream: the
 * identification packet at the start of the first page (a page is "OggS",
 * 23 more header bytes, a segment table and the segments) gives the rate
 * and the channels, the last page of the same stream its length.  Returns
 * 0 when the data is not an Ogg Vorbis stream. */
static int OggRead(const uint8_t* d, uint32_t n, OggInfo_t* out)
{
	const uint8_t* id;
	uint32_t serial, pos = 0;
	memset(out, 0, sizeof *out);
	if(n < 27 || memcmp(d, "OggS", 4) != 0 || 27u + d[26] + 30u > n)
		return 0;
	id = d + 27 + d[26];
	if(id[0] != 1 || memcmp(id + 1, "vorbis", 6) != 0)
		return 0;
	out->channels = id[11];
	out->rate = Le32(id + 12);
	serial = Le32(d + 14);
	while(pos + 27 <= n && memcmp(d + pos, "OggS", 4) == 0)
	{
		uint32_t segs = d[pos + 26], body = 0, i;
		if(pos + 27 + segs > n)
			break;
		for(i = 0; i < segs; i++)
			body += d[pos + 27 + i];
		// a granule position of -1 marks a page on which no packet ends
		if(Le32(d + pos + 14) == serial && !(Le32(d + pos + 6) == 0xffffffffu && Le32(d + pos + 10) == 0xffffffffu))
			out->frames = Le32(d + pos + 6);
		pos += 27 + segs + body;
	}
	return out->channels != 0 && out->rate != 0;
}

// the BW header of a Vorbis stream of `oggSize` bytes
static void VorbisHeader(BwHeader_t* h, uint32_t oggSize, const OggInfo_t* ogg, uint32_t frames, int loop, uint32_t loopStart)
{
	memset(h, 0, sizeof *h);
	h->headerSize = 0x40;
	memcpy(h->magic, "bw  ", 4);
	h->dataSize = oggSize;
	h->frames = frames;
	h->rate = ogg->rate;
	h->channels = ogg->channels;
	h->loopFlag = loop ? 1 : 0;
	h->loopStart = loopStart;
	h->codec = BW_CODEC_VORBIS;
}

// ---- WAV --------------------------------------------------------------------------------------------

/* decode the whole BW sound with the engine's stream classes into a
 * 16-bit PCM WAV file image (BGI_Free it); NULL when it does not open */
static uint8_t* DecodeToWav(const uint8_t* data, uint32_t n, uint32_t* wavSize)
{
	WmSource_t* src = WmSource_FromMemory(data, n);
	WmStream_t* s;
	int err = 0;
	uint32_t rate, ch, total = 0, cap, bytes;
	int16_t* pcm;
	uint8_t* wav;
	if(!src)
		return NULL;
	s = WmStream_Open(src, 1.0, 0, &err);
	if(!s)
		return NULL;
	rate = WmStream_Rate(s);
	ch = WmStream_Channels(s);
	cap = WmStream_Frames(s);
	pcm = (int16_t*)BGI_Alloc(((size_t)cap + 1) * ch * 2);
	while(total < cap)
	{
		uint32_t got = WmStream_Decode(s, pcm + (size_t)total * ch, cap - total > 4096 ? 4096 : cap - total);
		if(got == 0)
			break;
		total += got;
	}
	WmStream_Close(s);
	bytes = total * ch * 2;
	wav = (uint8_t*)BGI_Alloc(44 + (size_t)bytes);
	// the canonical 44-byte RIFF / WAVE header: format 1 (PCM), 16 bits
	memcpy(wav, "RIFF", 4);
	PutLe32(wav + 4, 36 + bytes);
	memcpy(wav + 8, "WAVEfmt ", 8);
	PutLe32(wav + 16, 16);
	PutLe16(wav + 20, 1);
	PutLe16(wav + 22, ch);
	PutLe32(wav + 24, rate);
	PutLe32(wav + 28, rate * ch * 2);
	PutLe16(wav + 32, ch * 2);
	PutLe16(wav + 34, 16);
	memcpy(wav + 36, "data", 4);
	PutLe32(wav + 40, bytes);
	memcpy(wav + 44, pcm, bytes);
	BGI_Free(pcm);
	*wavSize = 44 + bytes;
	return wav;
}

/* a BW file of codec 1 from a WAV file: PCM of 8 or 16 bits, one or two
 * channels (BGI_Free it); NULL with a message for anything else */
static uint8_t* WavToBw(PackCtx_t* c, const uint8_t* wav, uint32_t n, int loop, uint32_t loopStart, uint32_t* size)
{
	uint32_t pos = 12, rate = 0, ch = 0, bits = 0, dataLen = 0, frames, i;
	const uint8_t* samples = NULL;
	BwHeader_t h;
	uint8_t* out;
	if(n < 12 || memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0)
	{
		Pack_Note(c, "not a WAV file");
		return NULL;
	}
	while(pos + 8 <= n)
	{ // the chunks: a four-letter name, a length, the data padded to an even size
		uint32_t len = Le32(wav + pos + 4);
		if(len > n - pos - 8)
			len = n - pos - 8;
		if(memcmp(wav + pos, "fmt ", 4) == 0 && len >= 16)
		{
			uint32_t format = wav[pos + 8] | ((uint32_t)wav[pos + 9] << 8);
			ch = wav[pos + 10] | ((uint32_t)wav[pos + 11] << 8);
			rate = Le32(wav + pos + 12);
			bits = wav[pos + 22] | ((uint32_t)wav[pos + 23] << 8);
			if(format != 1 && format != 0xfffe) // PCM, or the extensible form of it
				bits = 0;
		}
		else if(memcmp(wav + pos, "data", 4) == 0)
		{
			samples = wav + pos + 8;
			dataLen = len;
		}
		pos += 8 + len + (len & 1);
	}
	if(!samples || (bits != 8 && bits != 16) || (ch != 1 && ch != 2) || !rate)
	{
		Pack_Note(c, "the WAV file has to be PCM of 8 or 16 bits with one or two channels");
		return NULL;
	}
	frames = dataLen / (ch * (bits / 8));
	memset(&h, 0, sizeof h);
	h.headerSize = 0x40;
	memcpy(h.magic, "bw  ", 4);
	h.dataSize = frames * ch * 2;
	h.frames = frames;
	h.rate = rate;
	h.channels = ch;
	h.loopFlag = loop ? 1 : 0;
	h.loopStart = loopStart;
	h.codec = BW_CODEC_PCM;
	*size = 0x40 + h.dataSize;
	out = (uint8_t*)BGI_Alloc(*size);
	memcpy(out, &h, 0x40);
	if(bits == 16)
		memcpy(out + 0x40, samples, h.dataSize);
	else
		for(i = 0; i < frames * ch; i++) // 8-bit WAV samples are unsigned
			PutLe16(out + 0x40 + i * 2, (uint32_t)((samples[i] - 128) * 256) & 0xffffu);
	return out;
}

// ---- the converters ------------------------------------------------------------------------------------

int Dump_Sound(DumpCtx_t* c, const uint8_t* data, uint32_t size)
{
	static const char* const codecs[4] = {"adpcm", "pcm", "huffman", "vorbis"};
	BwHeader_t h, check;
	OggInfo_t ogg;
	char rel[DUMP_NAME_MAX + 16], json[256];
	int n;
	if(size < 0x40 || data[0] != 0x40 || memcmp(data + 4, "bw  ", 4) != 0)
		return 0;
	memcpy(&h, data, sizeof h);
	if(h.codec > 3)
		return 0;
	if(h.codec == BW_CODEC_VORBIS)
	{
		if(h.dataSize != size - 0x40 || !OggRead(data + 0x40, size - 0x40, &ogg))
			return 0;
		/* the header has to come back exactly from the stream and the two loop
		 * values; a length that is not the stream's own is recorded as well */
		VorbisHeader(&check, size - 0x40, &ogg, h.frames, h.loopFlag != 0, h.loopStart);
		if(memcmp(&check, &h, sizeof h) != 0)
		{
			Dump_Note(c, "the sound's header has values an Ogg file cannot carry: kept as it is");
			c->problems++;
			return 0;
		}
		snprintf(rel, sizeof rel, "%s.ogg", c->base);
		if(!Dump_EmitFile(c, "file", rel, data + 0x40, size - 0x40))
			return 1;
		n = snprintf(json, sizeof json, "{\n  \"loop\": %s,\n  \"loop_start\": %u", h.loopFlag ? "true" : "false", (unsigned)h.loopStart);
		if(h.frames != ogg.frames)
			n += snprintf(json + n, sizeof json - (size_t)n, ",\n  \"frames\": %u", (unsigned)h.frames);
		n += snprintf(json + n, sizeof json - (size_t)n, "\n}\n");
	}
	else
	{
		uint32_t wavSize = 0;
		uint8_t* wav = DecodeToWav(data, size, &wavSize);
		int ok;
		if(!wav)
		{
			Dump_Note(c, "the sound does not decode: kept as it is");
			c->problems++;
			return 0;
		}
		snprintf(rel, sizeof rel, "%s.wav", c->base);
		ok = Dump_EmitFile(c, "file", rel, wav, wavSize);
		BGI_Free(wav);
		if(!ok)
			return 1;
		n = snprintf(json, sizeof json, "{\n  \"loop\": %s,\n  \"loop_start\": %u\n}\n", h.loopFlag ? "true" : "false", (unsigned)h.loopStart);
	}
	snprintf(rel, sizeof rel, "%s.json", c->base);
	Dump_EmitFile(c, "info", rel, json, (size_t)n);
	DumpText_Printf(c->meta, ", \"codec\": \"%s\"", codecs[h.codec]);
	return 1;
}

uint8_t* Pack_Sound(PackCtx_t* c, const JsonNode_t* entry, uint32_t* size)
{
	const char* codec = Json_String(entry, "codec", "vorbis");
	uint32_t fileSize = 0, infoSize = 0, loopStart = 0, frames = 0;
	uint8_t *file, *out = NULL;
	int loop = 0, haveFrames = 0;
	if(Json_Get(entry, "info"))
	{
		uint8_t* text = Pack_ReadMember(c, entry, "info", &infoSize);
		JsonNode_t* info = text ? Json_Parse((const char*)text, infoSize, NULL) : NULL;
		if(!info)
		{
			if(text)
				Pack_Note(c, "the sound's .json file is not JSON");
			free(text);
			return NULL;
		}
		loop = Json_Number(info, "loop", 0) != 0;
		loopStart = (uint32_t)Json_Number(info, "loop_start", 0);
		haveFrames = Json_Get(info, "frames") != NULL;
		frames = (uint32_t)Json_Number(info, "frames", 0);
		Json_Free(info);
		free(text);
	}
	file = Pack_ReadMember(c, entry, "file", &fileSize);
	if(!file)
		return NULL;
	if(strcmp(codec, "vorbis") == 0)
	{
		OggInfo_t ogg;
		BwHeader_t h;
		if(!OggRead(file, fileSize, &ogg))
			Pack_Note(c, "not an Ogg Vorbis file");
		else if(ogg.channels > 2)
			Pack_Note(c, "the Ogg file has %u channels: the engine plays one or two", (unsigned)ogg.channels);
		else
		{
			VorbisHeader(&h, fileSize, &ogg, haveFrames ? frames : ogg.frames, loop, loopStart);
			*size = 0x40 + fileSize;
			out = (uint8_t*)BGI_Alloc(*size);
			memcpy(out, &h, 0x40);
			memcpy(out + 0x40, file, fileSize);
		}
	}
	else
		out = WavToBw(c, file, fileSize, loop, loopStart, size);
	free(file);
	return out;
}
