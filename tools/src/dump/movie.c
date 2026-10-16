/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * movie.c - the frame sequences of the dump and pack tools
 *           (dump_internal.h): a BF_Movie (bfmovie.h, docs/bfmovie.md)
 *           becomes a directory of PNG files, one per frame, and those a
 *           BF_Movie again
 *
 * The frames are written as the engine shows them - each one decoded on
 * top of the one before - into "<entry>/0000.png", "0001.png", ...: RGBA
 * for a 32-bit movie, RGB for a 24-bit one.  The frame rate, which no PNG
 * carries, goes into "<entry>.json" with the picture's size and depth.
 * Packing encodes the frames in the order the entry's object lists them
 * (bfmovieenc.c); they all have to be of one size.  Only the movies with
 * their frames in the file (type 0) are converted; a streamed one, should
 * a game have it, stays as it is.
 */
#include "dump_internal.h"
#include "bgi/bfmovie.h"
#include "bgi/png.h"

int Dump_Movie(DumpCtx_t* c, const uint8_t* data, uint32_t size)
{
	const BfMovieHeader_t* h = (const BfMovieHeader_t*)data;
	uint32_t w, hgt, i, count, done = 0;
	uint8_t *slot, *rgb = NULL;
	uint8_t** pngs;
	size_t* sizes;
	char rel[DUMP_NAME_MAX + 32], json[256];
	int n, ok = 1;
	if(size < 0x40 || !BfMovie_Check(data, size))
		return 0;
	w = h->width;
	hgt = h->height;
	count = h->frames;
	if(!BfMovie_IsSimple(data) || !w || !hgt || !count || w > 0xffff || hgt > 0xffff || (h->bpp != 24 && h->bpp != 32) ||
		h->mode != (h->bpp == 32 ? 2u : 1u) || h->zero[0] || h->zero[1] || h->zero[2] || h->zero[3] || h->zero[4])
	{
		Dump_Note(c, "a BF_Movie of a form that is not converted (type 0x%x): kept as it is", (unsigned)h->type);
		c->problems++;
		return 0;
	}
	// every frame is decoded before anything is written: a movie that fails half way leaves no files
	slot = (uint8_t*)BGI_Calloc((size_t)w * hgt * 4);
	if(h->bpp == 24)
		rgb = (uint8_t*)BGI_Alloc((size_t)w * hgt * 3);
	pngs = (uint8_t**)BGI_Calloc(sizeof(uint8_t*) * count);
	sizes = (size_t*)BGI_Calloc(sizeof(size_t) * count);
	for(i = 0; i < count && ok; i++)
	{
		if(!BfMovie_DecodeFrame(data, size, i, slot, (int)(w * 4)))
			ok = 0;
		else if(rgb)
		{ // a 24-bit movie's slot has an alpha of 0 throughout: the PNG is RGB
			size_t k, pixels = (size_t)w * hgt;
			for(k = 0; k < pixels; k++)
				memcpy(rgb + k * 3, slot + k * 4, 3);
			pngs[i] = Png_Encode(rgb, w, hgt, 3, (size_t)w * 3, c->pngLevel, &sizes[i]);
		}
		else
			pngs[i] = Png_Encode(slot, w, hgt, 4, (size_t)w * 4, c->pngLevel, &sizes[i]);
		if(ok && !pngs[i])
			ok = 0;
		done += ok;
	}
	BGI_Free(slot);
	BGI_Free(rgb);
	if(!ok)
	{
		Dump_Note(c, "frame %u of the BF_Movie does not decode: kept as it is", (unsigned)done);
		c->problems++;
	}
	else if(Dump_MakeDir(c, c->base))
	{
		n = snprintf(json, sizeof json, "{\n  \"width\": %u,\n  \"height\": %u,\n  \"bpp\": %u,\n  \"fps\": %u\n}\n", (unsigned)w, (unsigned)hgt,
			(unsigned)h->bpp, (unsigned)h->fps);
		snprintf(rel, sizeof rel, "%s.json", c->base);
		Dump_EmitFile(c, "info", rel, json, (size_t)n);
		DumpText_Add(c->meta, ", \"frames\": [");
		for(i = 0; i < count && !c->failed; i++)
		{
			snprintf(rel, sizeof rel, "%s/%04u.png", c->base, (unsigned)i);
			if(i)
				DumpText_Add(c->meta, ", ");
			Dump_EmitFile(c, NULL, rel, pngs[i], sizes[i]);
		}
		DumpText_Add(c->meta, "]");
	}
	for(i = 0; i < count; i++)
		free(pngs[i]);
	BGI_Free(pngs);
	BGI_Free(sizes);
	return ok;
}

uint8_t* Pack_Movie(PackCtx_t* c, const JsonNode_t* entry, uint32_t* size)
{
	const JsonNode_t* frames = Json_Get(entry, "frames");
	const JsonNode_t* f;
	uint32_t infoSize = 0, bpp, fps, w = 0, hgt = 0, index = 0;
	uint8_t* text = Pack_ReadMember(c, entry, "info", &infoSize);
	JsonNode_t* info = text ? Json_Parse((const char*)text, infoSize, NULL) : NULL;
	BfMovieEnc_t* enc = NULL;
	int ok = 1;
	if(!info)
	{
		if(text)
			Pack_Note(c, "the movie's .json file is not JSON");
		free(text);
		return NULL;
	}
	bpp = (uint32_t)Json_Number(info, "bpp", 32);
	fps = (uint32_t)Json_Number(info, "fps", 30);
	Json_Free(info);
	free(text);
	if(!frames || frames->type != JSON_ARRAY || !frames->child || (bpp != 24 && bpp != 32))
	{
		Pack_Note(c, "the movie has no frames, or a depth other than 24 or 32 bits");
		return NULL;
	}
	for(f = frames->child; f && ok; f = f->next, index++)
	{
		uint32_t pngSize = 0;
		uint8_t* png = Pack_ReadFileObject(c, f, &pngSize);
		PngImage_t img;
		char err[128];
		if(!png)
			ok = 0;
		else if(!Png_Decode(png, pngSize, &img, err, sizeof err))
		{
			Pack_Note(c, "frame %u: %s", (unsigned)index, err);
			ok = 0;
		}
		else
		{
			if(!enc)
			{
				w = img.width;
				hgt = img.height;
				enc = BfMovieEnc_Begin(w, hgt, bpp, fps);
			}
			if(img.width != w || img.height != hgt)
			{
				Pack_Note(c, "frame %u is %u x %u pixels, the first one %u x %u: the frames of a movie are of one size", (unsigned)index,
					(unsigned)img.width, (unsigned)img.height, (unsigned)w, (unsigned)hgt);
				ok = 0;
			}
			else
			{
				Png_Convert(&img, 4); // the encoder takes four bytes per pixel and drops the alpha of a 24-bit movie itself
				BfMovieEnc_AddFrame(enc, img.pixels, (size_t)w * 4);
			}
			free(img.pixels);
		}
		free(png);
	}
	if(!enc)
		return NULL;
	{
		uint8_t* out = BfMovieEnc_Finish(enc, size);
		if(!ok)
		{
			BGI_Free(out);
			return NULL;
		}
		return out;
	}
}
