/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * image.c - the images of the dump and pack tools (dump_internal.h): a
 *           raw image or a CompressedBG file of either version becomes a
 *           PNG, and a PNG an image file again
 *
 * Every image of the games decodes to the same thing, a 16-byte header
 * (width, height, bit depth, two more words) and rows of 8-bit grey, BGR
 * or BGRA pixels (codec.h), which is what the PNG holds.  The entry's
 * object records how the image was stored ("codec": "raw", "cbg1" or
 * "cbg2"), its depth ("bpp") and, for a version 1 CompressedBG, the key
 * its weight table is encrypted with.  The two header words after the depth
 * are zero in most files; the character pictures of the later games carry
 * a flag and a position there, and those go into a JSON file beside the
 * PNG as "extra".
 *
 * Packing reads the PNG back at the recorded depth.  A "cbg2" image - the
 * lossy codec of 1.573 on, for which there is no encoder here - is written
 * as version 1, which is lossless and which every build reads.
 */
#include "dump_internal.h"
#include "bgi/codec.h"
#include "bgi/png.h"

// a raw image: a plausible header whose pixels fill the entry exactly
static int IsRawImage(const uint8_t* d, uint32_t n)
{
	RawImageHeader_t h;
	if(n < 16)
		return 0;
	memcpy(&h, d, sizeof h);
	return h.width && h.height && (h.bpp == 8 || h.bpp == 24 || h.bpp == 32) && 16u + (uint32_t)h.width * h.height * (h.bpp / 8) == n;
}

int Dump_Image(DumpCtx_t* c, const uint8_t* data, uint32_t size)
{
	const char* codec = "raw";
	uint32_t key = 0;
	uint8_t* decoded = NULL;
	const uint8_t* image = data;
	RawImageHeader_t h;
	uint8_t* png;
	size_t pngSize = 0;
	char rel[DUMP_NAME_MAX + 16];
	uint32_t channels;
	int ok;

	if(size >= 0x30 && BGI_IsCbg(data))
	{
		const CbgHeader_t* ch = (const CbgHeader_t*)data;
		uint32_t bpp = ch->bpp;
		if(!ch->width || !ch->height || (bpp != 8 && bpp != 24 && bpp != 32) || (ch->version != 1 && ch->version != 2))
			return 0;
		decoded = (uint8_t*)BGI_Calloc((size_t)BGI_CbgOutSize(data) + 16);
		if(BGI_CbgDecode(data, decoded) != 0)
		{
			Dump_Note(c, "the CompressedBG image does not decode: kept as it is");
			c->problems++;
			BGI_Free(decoded);
			return 0;
		}
		image = decoded;
		codec = ch->version == 2 ? "cbg2" : "cbg1";
		key = ch->key;
	}
	else if(!IsRawImage(data, size))
		return 0;
	memcpy(&h, image, sizeof h);
	channels = h.bpp / 8;
	png = Png_Encode(image + 16, h.width, h.height, (int)channels, (size_t)h.width * channels, c->pngLevel, &pngSize);
	BGI_Free(decoded);
	if(!png)
		return 0;
	snprintf(rel, sizeof rel, "%s.png", c->base);
	ok = Dump_EmitFile(c, "file", rel, png, pngSize);
	free(png);
	if(!ok)
		return 1; // c->failed is set: the caller stops
	DumpText_Printf(c->meta, ", \"codec\": \"%s\", \"bpp\": %u", codec, (unsigned)h.bpp);
	if(strcmp(codec, "cbg1") == 0) // with its key a version 1 file comes back byte for byte from the same pixels
		DumpText_Printf(c->meta, ", \"key\": %u", (unsigned)key);
	if(h.unused08 || h.unused0c)
	{
		char json[128];
		int n = snprintf(json, sizeof json, "{\n  \"extra\": [%u, %u]\n}\n", (unsigned)h.unused08, (unsigned)h.unused0c);
		snprintf(rel, sizeof rel, "%s.json", c->base);
		Dump_EmitFile(c, "info", rel, json, (size_t)n);
	}
	return 1;
}

uint8_t* Pack_Image(PackCtx_t* c, const JsonNode_t* entry, uint32_t* size)
{
	const char* codec = Json_String(entry, "codec", "raw");
	uint32_t bpp = (uint32_t)Json_Number(entry, "bpp", 0), pngSize = 0, rawSize;
	uint8_t* png = Pack_ReadMember(c, entry, "file", &pngSize);
	PngImage_t img;
	RawImageHeader_t h;
	uint8_t* raw;
	char err[128];
	int lost;

	if(!png)
		return NULL;
	if(!Png_Decode(png, pngSize, &img, err, sizeof err))
	{
		Pack_Note(c, "%s", err);
		free(png);
		return NULL;
	}
	free(png);
	if(bpp != 8 && bpp != 24 && bpp != 32)
		bpp = (uint32_t)img.channels * 8; // an entry added by hand: the PNG decides
	if(img.width > 0xffff || img.height > 0xffff)
	{
		Pack_Note(c, "the picture is %u x %u pixels: an image holds at most 65535 each way", (unsigned)img.width, (unsigned)img.height);
		free(img.pixels);
		return NULL;
	}
	lost = Png_Convert(&img, (int)(bpp / 8));
	if(lost & PNG_LOST_ALPHA)
		Pack_Note(c, "warning: the PNG has transparency, the image is %u-bit: the alpha channel is dropped", (unsigned)bpp);
	if(lost & PNG_LOST_COLOUR)
		Pack_Note(c, "warning: the PNG has colours, the image is 8-bit grey: converted to grey");

	memset(&h, 0, sizeof h);
	h.width = (uint16_t)img.width;
	h.height = (uint16_t)img.height;
	h.bpp = bpp;
	if(Json_Get(entry, "info"))
	{ // the header words the PNG has no place for
		uint32_t n = 0;
		uint8_t* text = Pack_ReadMember(c, entry, "info", &n);
		JsonNode_t* info = text ? Json_Parse((const char*)text, n, NULL) : NULL;
		const JsonNode_t* extra = Json_Get(info, "extra");
		if(!info)
		{
			if(text)
				Pack_Note(c, "the image's .json file is not JSON");
			free(text);
			free(img.pixels);
			return NULL;
		}
		if(extra && extra->type == JSON_ARRAY && extra->child && extra->child->next)
		{
			h.unused08 = (uint32_t)extra->child->num;
			h.unused0c = (uint32_t)extra->child->next->num;
		}
		Json_Free(info);
		free(text);
	}
	rawSize = 16 + img.width * img.height * (bpp / 8);
	raw = (uint8_t*)BGI_Alloc(rawSize);
	memcpy(raw, &h, 16);
	memcpy(raw + 16, img.pixels, rawSize - 16);
	free(img.pixels);
	if(strcmp(codec, "raw") != 0)
	{ // "cbg1", and "cbg2" for want of an encoder of the lossy codec
		uint32_t key = Json_Get(entry, "key") ? (uint32_t)Json_Number(entry, "key", 0) : BGI_Crc32(0, raw, rawSize);
		uint8_t* cbg = BGI_CbgEncode(raw, key, size);
		BGI_Free(raw);
		return cbg;
	}
	*size = rawSize;
	return raw;
}
