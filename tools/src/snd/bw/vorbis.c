/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * vorbis.c - codec 3 of the BW format: an Ogg Vorbis stream behind the
 *            0x40-byte header (bw_internal.h)
 *
 * Three back ends: libvorbisfile when BGI_HAVE_VORBISFILE is defined (the
 * original links it statically), stb_vorbis when BGI_USE_STB_VORBIS is,
 * and - without either - silence of the right length, so that games whose
 * every sound is Vorbis still run (see docs/building.md).  A pair stream
 * has two decoders, one per part; `which` selects it.  The back ends
 * share Vorbis_Decode, which pulls raw PCM bytes through Vorbis_ReadBytes.
 */
#include "bw_internal.h"

#if defined(BGI_HAVE_VORBISFILE)
// the ov_callbacks: the source minus its 0x40-byte header, positions relative to the Ogg data
static size_t VorbisRead(void* ptr, size_t size, size_t nmemb, void* ds)
{
	WmStream_t* s = (WmStream_t*)ds;
	// bytes rather than items, as the original does (libvorbisfile reads items of size 1)
	return WmSource_Read(s->src, ptr, (uint32_t)(size * nmemb));
}
static int VorbisSeek(void* ds, ogg_int64_t off, int whence)
{
	WmStream_t* s = (WmStream_t*)ds;
	uint32_t target;
	switch(whence)
	{
		case SEEK_SET: target = (uint32_t)off + 0x40; break;
		case SEEK_CUR: target = WmSource_Tell(s->src) + (uint32_t)off; break;
		case SEEK_END: target = WmSource_Size(s->src) + (uint32_t)off; break;
		default: return -1;
	}
	WmSource_Seek(s->src, target);
	return WmSource_Tell(s->src) == target ? 0 : -1;
}
static int VorbisClose(void* ds) // the source is closed by the stream, not by libvorbisfile
{
	return 0;
}
static long VorbisTell(void* ds)
{
	return (long)WmSource_Tell(((WmStream_t*)ds)->src) - 0x40;
}

// open decoder `which` on the stream's current source; 1 ok, 0 when libvorbisfile rejects the data
int Vorbis_Open(WmStream_t* s, int which)
{
	ov_callbacks cb;
	cb.read_func = VorbisRead;
	cb.seek_func = VorbisSeek;
	cb.close_func = VorbisClose;
	cb.tell_func = VorbisTell;
	WmSource_Seek(s->src, 0x40);
	if(ov_open_callbacks(s, &s->vf[which], NULL, 0, cb) < 0)
		return 0;
	s->vfOpen[which] = 1;
	return 1;
}

void Vorbis_Close(WmStream_t* s, int which)
{
	if(s->vfOpen[which])
		ov_clear(&s->vf[which]);
	s->vfOpen[which] = 0;
}

// position decoder `which` at `frame` (PCM frames from the start)
void Vorbis_SeekFrame(WmStream_t* s, int which, uint32_t frame)
{
	ov_pcm_seek(&s->vf[which], (ogg_int64_t)frame);
}

// bytes of 16-bit little-endian PCM into buf (at most n); 0 at the end
uint32_t Vorbis_ReadBytes(WmStream_t* s, int which, uint8_t* buf, uint32_t n)
{
	int bs = 0;
	long r = ov_read(&s->vf[which], (char*)buf, (int)n, 0, 2, 1, &bs);
	return r > 0 ? (uint32_t)r : 0;
}
#elif defined(BGI_USE_STB_VORBIS)
// load the Ogg data behind the header into memory and open decoder `which` on it; 1 ok
int Vorbis_Open(WmStream_t* s, int which)
{
	uint32_t n = WmSource_Size(s->src) - 0x40;
	int err = 0;
	BGI_Free(s->vorbisData[which]);
	s->vorbisData[which] = (uint8_t*)BGI_Alloc(n ? n : 1);
	WmSource_Seek(s->src, 0x40);
	WmSource_Read(s->src, s->vorbisData[which], n);
	s->vorbis[which] = stb_vorbis_open_memory(s->vorbisData[which], (int)n, &err, NULL);
	s->vorbisBlockLen[which] = s->vorbisBlockPos[which] = 0;
	return s->vorbis[which] != NULL;
}

void Vorbis_Close(WmStream_t* s, int which)
{
	if(s->vorbis[which])
		stb_vorbis_close(s->vorbis[which]);
	s->vorbis[which] = NULL;
	BGI_Free(s->vorbisData[which]);
	s->vorbisData[which] = NULL;
	s->vorbisBlockLen[which] = s->vorbisBlockPos[which] = 0;
}

void Vorbis_SeekFrame(WmStream_t* s, int which, uint32_t frame)
{
	stb_vorbis_seek(s->vorbis[which], frame);
	s->vorbisBlockLen[which] = s->vorbisBlockPos[which] = 0;
}

// bytes of 16-bit interleaved PCM into buf (at most n, whole frames); 0 at the end
uint32_t Vorbis_ReadBytes(WmStream_t* s, int which, uint8_t* buf, uint32_t n)
{
	int ch = stb_vorbis_get_info(s->vorbis[which]).channels; // (the decoder's structure is opaque)
	int frames = stb_vorbis_get_samples_short_interleaved(s->vorbis[which], ch, (short*)buf, (int)(n / 2));
	return frames > 0 ? (uint32_t)frames * (uint32_t)ch * 2u : 0;
}
#else
/* no Vorbis decoder in this build: the stream opens and plays as silence
 * of the right length, so that games whose every sound is Vorbis still
 * run (the loader would otherwise report a fatal error for each effect) */
int Vorbis_Open(WmStream_t* s, int which)
{
	static int warned; // the notice goes to stderr once per run
	if(!warned)
	{
		fprintf(stderr, "bgi: built without a Vorbis decoder; BW codec 3 sounds play as silence\n");
		warned = 1;
	}
	return 1;
}
void Vorbis_Close(WmStream_t* s, int which)
{
}
void Vorbis_SeekFrame(WmStream_t* s, int which, uint32_t frame)
{
}
uint32_t Vorbis_ReadBytes(WmStream_t* s, int which, uint8_t* buf, uint32_t n) // `n` bytes of silence, never the end
{
	memset(buf, 0, n);
	return n;
}
#endif

/* Decode up to `frames` frames of codec 3 from the current part's
 * decoder into dst, a scratch buffer at a time; the frames delivered
 * (fewer at the end).  The header's frame count bounds the stream; a
 * decoder that runs dry earlier ends it too. */
uint32_t Vorbis_Decode(WmStream_t* s, int16_t* dst, uint32_t frames)
{
	uint32_t left = s->hdr.frames - s->pos;
	uint32_t bytes = (frames <= left ? frames : left) * s->blockAlign;
	uint32_t chunkBytes = WM_CHUNK_BYTES - WM_CHUNK_BYTES % s->blockAlign; // whole frames per pass
	uint32_t done = 0;
	int which = s->pair ? s->current : 0;
	while(bytes > 0)
	{
		uint32_t want = bytes < chunkBytes ? bytes : chunkBytes, got = 0;
		while(got < want)
		{ // ov_read returns a packet at a time
			uint32_t r = Vorbis_ReadBytes(s, which, (uint8_t*)s->chunk + got, want - got);
			if(r == 0)
				break;
			got += r;
		}
		Wm_ApplyGain(s, s->chunk, got / 2);
		memcpy((uint8_t*)dst + done, s->chunk, got);
		done += got;
		bytes -= got;
		if(got != want)
			break;
	}
	s->pos += done / s->blockAlign;
	return done / s->blockAlign;
}
