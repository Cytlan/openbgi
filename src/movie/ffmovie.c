/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ffmovie.c - the movie decoder beside the core: ffmpeg's libraries
 *             behind the calls of a host's movie decoder
 *             (include/openbgi_ffmovie.h)
 *
 * Everything happens on the caller's thread, when it asks: the file is
 * read through the host's two calls, a packet at a time, and decoded
 * only as far as what is asked for needs.  The file's packets come as it
 * interleaves them, so a packet of the stream that was not asked for
 * waits in that stream's queue, still compressed.  Asking for the frames
 * decodes pictures until FF_FRAMES of them wait to be taken; asking for
 * sound decodes it until there is as much as was asked for.
 *
 * The pictures are numbered from the first one that decodes, at the rate
 * the file's time stamps are counted in: a picture's number is its time
 * stamp in frames of that rate.  Where the file holds a picture for
 * longer - or counts in fields, as MPEG does, so that only every other
 * number has a picture - the numbers in between have none of their own:
 * the picture before them stands, and taking one of them gives that
 * picture unless it was taken already.  Every picture the file has gets
 * a number, whatever its time stamp (Picture_Number).  The sound is laid against the
 * same start: silence in front of it when it begins after the first
 * picture, its beginning cut when it begins before.
 */
#include "openbgi_ffmovie.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include <stdlib.h>
#include <string.h>

#define FF_FRAMES    4                        // pictures held for the caller to take
#define FF_IO_BUFFER 0x8000                   // bytes read from the host's file at a time
#define FF_QUEUE_MAX (48u << 20)              // bytes of packets a stream's queue holds; more is not read ahead
#define FF_ALIGN_MAX (OPENBGI_AUDIO_RATE * 2) // the most silence put in front of the sound, or sound cut from its start
#define FF_GAP_MAX   10.0                     // seconds a picture may stand; a longer jump of the time stamps is taken as none
#define FF_THREADS   4                        // decoder threads, where the decoder has any

// a stream's packets read ahead of their turn, oldest first
typedef struct Queue
{
	AVPacket** items;
	int count, cap;
	size_t bytes;
} Queue_t;

// a picture waiting to be taken
typedef struct Picture
{
	uint8_t* pixels; // w * h * 4 bytes
	int32_t index;   // its number
} Picture_t;

typedef struct FfMovie
{
	OpenBGI_MovieFile_t file;
	int64_t pos; // where the host's file stands
	AVIOContext* io;
	AVFormatContext* fmt;
	AVPacket* pkt;  // the packet being read or decoded
	AVFrame* frame; // the frame being decoded
	int eof;        // the file has no more packets

	// the pictures
	int vStream;
	AVCodecContext* vdec;
	Queue_t vq;
	struct SwsContext* sws;
	int w, h;
	double fps;
	Picture_t pic[FF_FRAMES]; // a ring: `held` of them from `oldest` on
	int oldest, held;
	int32_t count;     // the frames decoded so far: the number after the newest picture's
	int32_t step;      // the usual distance of two pictures' numbers: the rate they are numbered at over the frame rate
	int64_t vStart;    // the first picture's time stamp (the stream's time base)
	int haveStart;     // it is known
	int vSent, vEnded; // the decoder was told the stream is over / gave its last picture

	// the sound
	int aStream; // -1 without
	AVCodecContext* adec;
	Queue_t aq;
	SwrContext* swr;
	int swrRate, swrFormat; // what the resampler was made for
	AVChannelLayout swrLayout;
	int mono;      // the stream has one channel, which goes to both sides as it is
	int16_t* fifo; // stereo frames decoded and not yet taken
	int fifoLen, fifoCap;
	int aligned; // the sound's start was laid against the pictures'
	int skip;    // stereo frames still to cut from its start
	int aSent, aEnded;
} FfMovie_t;

// ---- the host's file ----------------------------------------------------------------------------

static int IoRead(void* opaque, uint8_t* buf, int n)
{
	FfMovie_t* m = (FfMovie_t*)opaque;
	int got = m->file.read(m->file.ctx, buf, n);
	if(got < 0)
		return AVERROR(EIO);
	if(got == 0)
		return AVERROR_EOF;
	m->pos += got;
	return got;
}

static int64_t IoSeek(void* opaque, int64_t offset, int whence)
{
	FfMovie_t* m = (FfMovie_t*)opaque;
	int64_t to;
	if(whence & AVSEEK_SIZE)
		return m->file.length >= 0 ? m->file.length : AVERROR(ENOSYS);
	whence &= ~AVSEEK_FORCE;
	if(whence == SEEK_SET)
		to = offset;
	else if(whence == SEEK_CUR)
		to = m->pos + offset;
	else if(whence == SEEK_END && m->file.length >= 0)
		to = m->file.length + offset;
	else
		return AVERROR(EINVAL);
	if(to < 0 || !m->file.seek(m->file.ctx, to))
		return AVERROR(EIO);
	m->pos = to;
	return to;
}

// ---- the packets --------------------------------------------------------------------------------

// the queue takes the packet over (`pkt` is empty afterwards); 0 when memory runs out
static int Queue_Push(Queue_t* q, AVPacket* pkt)
{
	AVPacket* own;
	if(q->count == q->cap)
	{
		int cap = q->cap ? q->cap * 2 : 64;
		AVPacket** items = (AVPacket**)realloc(q->items, (size_t)cap * sizeof *items);
		if(!items)
			return 0;
		q->items = items;
		q->cap = cap;
	}
	own = av_packet_alloc();
	if(!own)
		return 0;
	av_packet_move_ref(own, pkt);
	q->items[q->count++] = own;
	q->bytes += (size_t)own->size;
	return 1;
}

// the oldest packet into `pkt`; 0 when the queue is empty
static int Queue_Pop(Queue_t* q, AVPacket* pkt)
{
	AVPacket* own;
	if(!q->count)
		return 0;
	own = q->items[0];
	memmove(q->items, q->items + 1, (size_t)(q->count - 1) * sizeof *q->items);
	q->count--;
	q->bytes -= (size_t)own->size;
	av_packet_move_ref(pkt, own);
	av_packet_free(&own);
	return 1;
}

static void Queue_Free(Queue_t* q)
{
	int i;
	for(i = 0; i < q->count; i++)
		av_packet_free(&q->items[i]);
	free(q->items);
	memset(q, 0, sizeof *q);
}

/* The next packet of the pictures' stream (`video`) or the sound's into
 * m->pkt: from the stream's queue, else from the file, the other
 * stream's packets met on the way going to its queue.  1 with a packet;
 * 0 without one - at the file's end (m->eof), or for now, when the other
 * queue is full and has to be emptied first. */
static int NextPacket(FfMovie_t* m, int video)
{
	Queue_t* own = video ? &m->vq : &m->aq;
	Queue_t* other = video ? &m->aq : &m->vq;
	int want = video ? m->vStream : m->aStream;
	int rest = video ? m->aStream : m->vStream;
	if(Queue_Pop(own, m->pkt))
		return 1;
	while(!m->eof)
	{
		if(other->bytes > FF_QUEUE_MAX)
			return 0;
		if(av_read_frame(m->fmt, m->pkt) < 0)
		{
			m->eof = 1;
			break;
		}
		if(m->pkt->stream_index == want)
			return 1;
		if(m->pkt->stream_index != rest || rest < 0 || !Queue_Push(other, m->pkt))
			av_packet_unref(m->pkt); // (a stream nobody decodes)
	}
	return 0;
}

// ---- the pictures -------------------------------------------------------------------------------

/* The number of the frame just decoded.  Its time stamp says it, in
 * frames after the first picture, when that lies after the picture
 * before; a picture without a time stamp, or with the one before's,
 * follows at the usual distance; and when the time stamps jump - back,
 * or more than FF_GAP_MAX seconds ahead - they are counted on from this
 * picture at that distance.  No picture is left out. */
static int32_t Picture_Number(FfMovie_t* m, int64_t pts)
{
	const AVStream* st = m->fmt->streams[m->vStream];
	int32_t usual = m->count ? m->count - 1 + m->step : 0;
	double tick = av_q2d(st->time_base) * m->fps; // frames per unit of the time stamps
	double at;
	if(pts == AV_NOPTS_VALUE || tick <= 0.0)
		return usual;
	if(!m->haveStart)
	{
		m->vStart = pts;
		m->haveStart = 1;
	}
	at = (double)(pts - m->vStart) * tick;
	if(at >= (double)m->count - 0.5 && at <= (double)usual + FF_GAP_MAX * m->fps)
		return at < (double)m->count ? m->count : (int32_t)(at + 0.5);
	if(at < (double)m->count - 1.5 || at > (double)usual)
		m->vStart = pts - (int64_t)((double)usual / tick + 0.5); // a jump
	return usual;
}

// the frame just decoded as the newest picture; 0 when it cannot be converted
static int Picture_Put(FfMovie_t* m)
{
	AVFrame* f = m->frame;
	Picture_t* p = &m->pic[(m->oldest + m->held) % FF_FRAMES];
	uint8_t* out[4] = {NULL, NULL, NULL, NULL};
	int pitch[4] = {0, 0, 0, 0};
	m->sws = sws_getCachedContext(m->sws, f->width, f->height, (enum AVPixelFormat)f->format, m->w, m->h, AV_PIX_FMT_BGRA, SWS_BICUBIC, NULL, NULL, NULL);
	if(!m->sws)
		return 0;
	out[0] = p->pixels;
	pitch[0] = m->w * 4;
	sws_scale(m->sws, (const uint8_t* const*)f->data, f->linesize, 0, f->height, out, pitch);
	p->index = Picture_Number(m, f->best_effort_timestamp);
	m->count = p->index + 1;
	m->held++;
	return 1;
}

/* One more picture decoded into the ring, which has room: 1; 0 when
 * there is none - the stream is over (m->vEnded), or its next packet
 * cannot be read for now. */
static int Picture_Decode(FfMovie_t* m)
{
	while(!m->vEnded)
	{
		int r = avcodec_receive_frame(m->vdec, m->frame);
		if(r == 0)
		{
			int put = Picture_Put(m);
			av_frame_unref(m->frame);
			if(put)
				return 1;
			continue;
		}
		if(r != AVERROR(EAGAIN) || m->vSent)
		{ // the last one came, or the decoder gave up
			m->vEnded = 1;
			break;
		}
		if(NextPacket(m, 1))
		{
			avcodec_send_packet(m->vdec, m->pkt); // (a packet it cannot use is skipped)
			av_packet_unref(m->pkt);
		}
		else if(m->eof)
		{
			avcodec_send_packet(m->vdec, NULL);
			m->vSent = 1;
		}
		else
			return 0;
	}
	return 0;
}

// ---- the sound ----------------------------------------------------------------------------------

// room for `frames` more stereo frames in the FIFO; 0 when memory runs out
static int Fifo_Room(FfMovie_t* m, int frames)
{
	if(m->fifoLen + frames > m->fifoCap)
	{
		int cap = (m->fifoLen + frames) * 2;
		int16_t* fifo = (int16_t*)realloc(m->fifo, (size_t)cap * 2 * sizeof *fifo);
		if(!fifo)
			return 0;
		m->fifo = fifo;
		m->fifoCap = cap;
	}
	return 1;
}

/* The sound's start against the pictures', once, with the first sound
 * frame's time stamp: when the sound begins later than the first
 * picture, that much silence goes in front of it; when earlier, that
 * much of it is cut. */
static void Sound_Align(FfMovie_t* m, int64_t pts)
{
	double lead;
	m->aligned = 1;
	if(pts == AV_NOPTS_VALUE || !m->haveStart)
		return;
	lead = (double)pts * av_q2d(m->fmt->streams[m->aStream]->time_base) - (double)m->vStart * av_q2d(m->fmt->streams[m->vStream]->time_base);
	if(lead > 0.005)
	{
		int n = lead * OPENBGI_AUDIO_RATE < FF_ALIGN_MAX ? (int)(lead * OPENBGI_AUDIO_RATE) : FF_ALIGN_MAX;
		if(Fifo_Room(m, n))
		{
			memset(m->fifo + (size_t)m->fifoLen * 2, 0, (size_t)n * 2 * sizeof *m->fifo);
			m->fifoLen += n;
		}
	}
	else if(lead < -0.005)
		m->skip = -lead * OPENBGI_AUDIO_RATE < FF_ALIGN_MAX ? (int)(-lead * OPENBGI_AUDIO_RATE) : FF_ALIGN_MAX;
}

// the frame just decoded (NULL: what the resampler still holds), resampled into the FIFO
static void Sound_Put(FfMovie_t* m, const AVFrame* f)
{
	uint8_t* out[1];
	int room, got;
	if(f)
	{
		if(!m->aligned)
			Sound_Align(m, f->best_effort_timestamp);
		if(!m->swr || m->swrRate != f->sample_rate || m->swrFormat != f->format || av_channel_layout_compare(&m->swrLayout, &f->ch_layout) != 0)
		{ // the first frame, or the stream changed its format
			AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
			AVChannelLayout one = AV_CHANNEL_LAYOUT_MONO;
			AVChannelLayout in;
			memset(&in, 0, sizeof in);
			if(f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
				av_channel_layout_default(&in, f->ch_layout.nb_channels); // (a file that only counts its channels)
			else
				av_channel_layout_copy(&in, &f->ch_layout);
			swr_free(&m->swr);
			av_channel_layout_uninit(&m->swrLayout);
			/* one channel is resampled as one and then put on both sides at
			 * its own level, as the original's player does (a mix to
			 * stereo would make it 3 dB quieter) */
			m->mono = in.nb_channels == 1;
			if(swr_alloc_set_opts2(&m->swr, m->mono ? &one : &stereo, AV_SAMPLE_FMT_S16, OPENBGI_AUDIO_RATE, &in, (enum AVSampleFormat)f->format, f->sample_rate, 0, NULL) < 0 || swr_init(m->swr) < 0)
				swr_free(&m->swr);
			av_channel_layout_uninit(&in);
			if(!m->swr)
				return;
			m->swrRate = f->sample_rate;
			m->swrFormat = f->format;
			av_channel_layout_copy(&m->swrLayout, &f->ch_layout);
		}
	}
	if(!m->swr)
		return;
	room = swr_get_out_samples(m->swr, f ? f->nb_samples : 0);
	if(room <= 0 || !Fifo_Room(m, room))
		return;
	out[0] = (uint8_t*)(m->fifo + (size_t)m->fifoLen * 2);
	got = swr_convert(m->swr, out, room, f ? (const uint8_t**)f->extended_data : NULL, f ? f->nb_samples : 0);
	if(got <= 0)
		return;
	if(m->mono)
	{ // (from the end, in place)
		int16_t* s16 = (int16_t*)(void*)out[0];
		int i;
		for(i = got - 1; i >= 0; i--)
			s16[i * 2] = s16[i * 2 + 1] = s16[i];
	}
	if(m->skip)
	{ // the cut at the start
		int cut = m->skip < got ? m->skip : got;
		memmove(out[0], out[0] + (size_t)cut * 4, (size_t)(got - cut) * 4);
		m->skip -= cut;
		got -= cut;
	}
	m->fifoLen += got;
}

// some more sound decoded into the FIFO: 1; 0 when there is none - the stream is over (m->aEnded), or not for now
static int Sound_Decode(FfMovie_t* m)
{
	while(!m->aEnded)
	{
		int r = avcodec_receive_frame(m->adec, m->frame);
		if(r == 0)
		{
			Sound_Put(m, m->frame);
			av_frame_unref(m->frame);
			return 1;
		}
		if(r != AVERROR(EAGAIN) || m->aSent)
		{
			Sound_Put(m, NULL);
			m->aEnded = 1;
			break;
		}
		if(NextPacket(m, 0))
		{
			avcodec_send_packet(m->adec, m->pkt);
			av_packet_unref(m->pkt);
		}
		else if(m->eof)
		{
			avcodec_send_packet(m->adec, NULL);
			m->aSent = 1;
		}
		else
			return 0;
	}
	return 0;
}

// ---- the calls ----------------------------------------------------------------------------------

// a decoder for one of the file's streams, opened; NULL when this build has none for it
static AVCodecContext* OpenDecoder(const AVStream* st)
{
	const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
	AVCodecContext* c;
	if(!codec || !(c = avcodec_alloc_context3(codec)))
		return NULL;
	c->thread_count = FF_THREADS;
	c->pkt_timebase = st->time_base;
	if(avcodec_parameters_to_context(c, st->codecpar) < 0 || avcodec_open2(c, codec, NULL) < 0)
		avcodec_free_context(&c);
	return c;
}

// a rate as frames a second; 0 when it is not one of 1 to 240
static double RateOf(AVRational r)
{
	double v = r.num > 0 && r.den > 0 ? av_q2d(r) : 0.0;
	return v >= 1.0 && v <= 240.0 ? v : 0.0;
}

void OpenBGI_FfMovieClose(void* movie)
{
	FfMovie_t* m = (FfMovie_t*)movie;
	int i;
	if(!m)
		return;
	avcodec_free_context(&m->vdec);
	avcodec_free_context(&m->adec);
	avformat_close_input(&m->fmt); // (the file's reader below is this library's to free)
	if(m->io)
	{
		av_freep(&m->io->buffer);
		avio_context_free(&m->io);
	}
	Queue_Free(&m->vq);
	Queue_Free(&m->aq);
	sws_freeContext(m->sws);
	swr_free(&m->swr);
	av_channel_layout_uninit(&m->swrLayout);
	av_packet_free(&m->pkt);
	av_frame_free(&m->frame);
	for(i = 0; i < FF_FRAMES; i++)
		free(m->pic[i].pixels);
	free(m->fifo);
	free(m);
}

void* OpenBGI_FfMovieOpen(const OpenBGI_MovieFile_t* file, int w, int h, int audio, OpenBGI_MovieInfo_t* info)
{
	FfMovie_t* m;
	uint8_t* buffer;
	const AVStream* st;
	int i;
	if(!file || file->size < offsetof(OpenBGI_MovieFile_t, length) + sizeof file->length || !file->read || w <= 0 || h <= 0 ||
		(size_t)w * (size_t)h > 0x1000000u)
		return NULL;
	m = (FfMovie_t*)calloc(1, sizeof *m);
	if(!m)
		return NULL;
	av_log_set_level(AV_LOG_QUIET); // (the engine says what it has to say itself)
	m->file = *file;
	m->w = w;
	m->h = h;
	m->aStream = -1;
	m->pkt = av_packet_alloc();
	m->frame = av_frame_alloc();
	m->fmt = avformat_alloc_context();
	buffer = (uint8_t*)av_malloc(FF_IO_BUFFER);
	if(!m->pkt || !m->frame || !m->fmt || !buffer)
	{
		av_free(buffer);
		OpenBGI_FfMovieClose(m);
		return NULL;
	}
	m->io = avio_alloc_context(buffer, FF_IO_BUFFER, 0, m, IoRead, NULL, file->seek ? IoSeek : NULL);
	if(!m->io)
	{
		av_free(buffer);
		OpenBGI_FfMovieClose(m);
		return NULL;
	}
	m->fmt->pb = m->io;
	m->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
	if(avformat_open_input(&m->fmt, NULL, NULL, NULL) < 0 || avformat_find_stream_info(m->fmt, NULL) < 0)
	{
		OpenBGI_FfMovieClose(m);
		return NULL;
	}
	m->vStream = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if(m->vStream < 0 || !(m->vdec = OpenDecoder(m->fmt->streams[m->vStream])))
	{
		OpenBGI_FfMovieClose(m);
		return NULL;
	}
	for(i = 0; i < FF_FRAMES; i++)
	{
		m->pic[i].pixels = (uint8_t*)malloc((size_t)w * (size_t)h * 4);
		if(!m->pic[i].pixels)
		{
			OpenBGI_FfMovieClose(m);
			return NULL;
		}
	}
	if(audio)
	{
		m->aStream = av_find_best_stream(m->fmt, AVMEDIA_TYPE_AUDIO, -1, m->vStream, NULL, 0);
		if(m->aStream >= 0 && !(m->adec = OpenDecoder(m->fmt->streams[m->aStream])))
			m->aStream = -1; // a sound this build does not decode: the pictures alone
	}
	if(m->aStream < 0)
		m->aEnded = 1;
	/* the rate the pictures are numbered at: the one every time stamp of
	 * the stream is a multiple of (for an MPEG stream twice its frame
	 * rate), so that no two pictures share a number; where the file has
	 * no such rate (time stamps in milliseconds), its average rate; 30
	 * when it says neither */
	st = m->fmt->streams[m->vStream];
	m->fps = RateOf(st->r_frame_rate);
	if(m->fps == 0.0)
		m->fps = RateOf(st->avg_frame_rate);
	if(m->fps == 0.0)
		m->fps = 30.0;
	m->step = RateOf(st->avg_frame_rate) > 0.0 ? (int32_t)(m->fps / RateOf(st->avg_frame_rate) + 0.5) : 1;
	if(m->step < 1)
		m->step = 1;
	info->fps = m->fps;
	info->lengthMs = m->fmt->duration != AV_NOPTS_VALUE && m->fmt->duration > 0 ? (int32_t)((m->fmt->duration + 500) / 1000) : 0;
	info->audio = m->aStream >= 0;
	return m;
}

int32_t OpenBGI_FfMovieFrames(void* movie, int* ended)
{
	FfMovie_t* m = (FfMovie_t*)movie;
	while(m->held < FF_FRAMES && Picture_Decode(m))
		;
	*ended = m->vEnded;
	return m->count;
}

int OpenBGI_FfMovieTake(void* movie, int32_t index, uint8_t* out)
{
	FfMovie_t* m = (FfMovie_t*)movie;
	int found = 0;
	if(index >= m->count)
		return 0; // not yet there
	while(m->held && m->pic[m->oldest].index <= index)
	{ // the pictures up to that number are let go; the newest of them is the one that stands there
		if(m->held == 1 || m->pic[(m->oldest + 1) % FF_FRAMES].index > index)
		{
			memcpy(out, m->pic[m->oldest].pixels, (size_t)m->w * (size_t)m->h * 4);
			found = 1;
		}
		m->oldest = (m->oldest + 1) % FF_FRAMES;
		m->held--;
	}
	return found;
}

int OpenBGI_FfMovieSamples(void* movie, int16_t* out, int frames, int* ended)
{
	FfMovie_t* m = (FfMovie_t*)movie;
	int n;
	while(m->fifoLen < frames && Sound_Decode(m))
		;
	n = m->fifoLen < frames ? m->fifoLen : frames;
	if(n < 0)
		n = 0;
	if(n)
	{
		if(out)
			memcpy(out, m->fifo, (size_t)n * 2 * sizeof *out);
		memmove(m->fifo, m->fifo + (size_t)n * 2, (size_t)(m->fifoLen - n) * 2 * sizeof *m->fifo);
		m->fifoLen -= n;
	}
	*ended = m->aEnded && m->fifoLen == 0;
	return n;
}

const char* OpenBGI_FfMovieVersion(void)
{
	return LIBAVCODEC_IDENT;
}

int OpenBGI_FfMovieHas(const char* name)
{
	return avcodec_find_decoder_by_name(name) != NULL || av_find_input_format(name) != NULL;
}
