// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 2023 by LJ Sonic
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  movie_decode.c
/// \brief Movie decoding using FFmpeg

#include "movie_decode.h"
#include "byteptr.h"
#include "doomdef.h"
#include "doomtype.h"
#include "libavutil/channel_layout.h"
#include "libavutil/imgutils.h"
#include "s_sound.h"
#include "v_video.h"
#include "w_wad.h"

#if defined(__GNUC__) || defined(__clang__)
	// Because of swr_convert not using const on the input buffer's pointer
    #pragma GCC diagnostic ignored "-Wcast-qual"
#endif

#define IO_BUFFER_SIZE (8 * 1024)
#define STREAM_BUFFER_TIME 4000
#define NUM_PACKETS 32
#define SAMPLE_RATE 44100
#define MAX_AUDIO_DESYNC 200
#define MAX_SEEK_DISTANCE 10000

#define POST_MAX_HEIGHT 254
#define POST_BASE_BYTES 4

//
// CIRCULAR BUFFER
//

static void InitialiseBuffer(moviebuffer_t *buffer, INT32 capacity, INT32 slotsize)
{
	buffer->capacity = capacity;
	buffer->slotsize = slotsize;
	buffer->start = 0;
	buffer->size = 0;

	buffer->data = calloc(capacity, slotsize);
	if (!buffer->data)
		I_Error("FFmpeg: cannot allocate buffer");
}

static void UninitialiseBuffer(moviebuffer_t *buffer)
{
	free(buffer->data);
}

static void CloneBuffer(moviebuffer_t *dst, moviebuffer_t *src)
{
	memcpy(dst, src, sizeof(*dst));

	dst->data = calloc(dst->capacity, dst->slotsize);
	if (!dst->data)
		I_Error("FFmpeg: cannot allocate buffer");

	memcpy(dst->data, src->data, dst->capacity * dst->slotsize);
}

static void *GetBufferSlot(const moviebuffer_t *buffer, INT32 index)
{
	if (index < 0)
		return NULL;

	index = (buffer->start + index) % buffer->capacity;
	return &((UINT8*)buffer->data)[index * buffer->slotsize];
}

static void *PeekBuffer(const moviebuffer_t *buffer)
{
	return GetBufferSlot(buffer, 0);
}

static void *EnqueueBuffer(moviebuffer_t *buffer)
{
	buffer->size++;
	return GetBufferSlot(buffer, buffer->size - 1);
}

static void *DequeueBuffer(moviebuffer_t *buffer)
{
	void *slot = PeekBuffer(buffer);
	buffer->start = (buffer->start + 1) % buffer->capacity;
	buffer->size--;
	return slot;
}

static void *DequeueBufferIntoBuffer(moviebuffer_t *dst, moviebuffer_t *src)
{
	void *srcslot = DequeueBuffer(src);
	void *dstslot = EnqueueBuffer(dst);
	memcpy(dstslot, srcslot, src->slotsize);
	return dstslot;
}

static void DequeueWholeBufferIntoBuffer(moviebuffer_t *dst, moviebuffer_t *src)
{
	while (src->size > 0)
		DequeueBufferIntoBuffer(dst, src);
}

//
// TIME CONVERSION
//

static INT64 PTSToSamples(movie_t *movie, INT64 pts)
{
	AVRational oldtb = movie->audiostream.stream->time_base;
	AVRational newtb = { 1, SAMPLE_RATE };

	return av_rescale_q(pts, oldtb, newtb);
}

static INT64 SamplesToPTS(movie_t *movie, INT64 numsamples)
{
	AVRational oldtb = { 1, SAMPLE_RATE };
	AVRational newtb = movie->audiostream.stream->time_base;

	return av_rescale_q(numsamples, oldtb, newtb);
}

static INT64 SamplesToMS(INT64 numsamples)
{
	AVRational oldtb = { 1, SAMPLE_RATE };
	AVRational newtb = { 1, 1000 };

	return av_rescale_q(numsamples, oldtb, newtb);
}

static INT64 MSToSamples(INT64 ms)
{
	AVRational oldtb = { 1, 1000 };
	AVRational newtb = { 1, SAMPLE_RATE };

	return av_rescale_q(ms, oldtb, newtb);
}

static INT64 VideoPTSToMS(movie_t *movie, INT64 pts)
{
	AVRational oldtb = movie->videostream.stream->time_base;
	AVRational newtb = { 1, 1000 };

	return av_rescale_q(pts, oldtb, newtb);
}

static INT64 MSToVideoPTS(movie_t *movie, INT64 ms)
{
	AVRational oldtb = { 1, 1000 };
	AVRational newtb = movie->videostream.stream->time_base;

	return av_rescale_q(ms, oldtb, newtb);
}

static INT64 MSToAudioPTS(movie_t *movie, INT64 ms)
{
	AVRational oldtb = { 1, 1000 };
	AVRational newtb = movie->audiostream.stream->time_base;

	return av_rescale_q(ms, oldtb, newtb);
}

static INT64 PTSToMS(INT64 pts)
{
	AVRational newtb = { 1, 1000 };

	return av_rescale_q(pts, AV_TIME_BASE_Q, newtb);
}

//
// MISCELLANEOUS GETTERS
//

static INT64 GetVideoFrameEndPTS(movievideoframe_t *frame)
{
	return frame->pts + frame->duration;
}

static INT64 GetAudioFrameEndPTS(movie_t *movie, movieaudioframe_t *frame)
{
	return frame->pts + SamplesToPTS(movie, frame->numsamples);
}

static INT64 GetAudioFrameEndSample(movieaudioframe_t *frame)
{
	return frame->firstsampleposition + frame->numsamples;
}

static INT32 GetBytesPerPatchColumn(moviedecodeworker_t *worker)
{
	INT32 height = worker->videostream.codeccontext->height;
	INT32 numpostspercolumn = (height + POST_MAX_HEIGHT - 1) / POST_MAX_HEIGHT;
	return height + numpostspercolumn * POST_BASE_BYTES + 1;
}

static INT32 FindVideoBufferIndexForPosition(movie_t *movie, INT64 pts)
{
	INT32 i;

	for (i = movie->videostream.buffer.size - 1; i >= 0; i--)
	{
		movievideoframe_t *frame = GetBufferSlot(&movie->videostream.buffer, i);
		if (frame->pts <= pts)
			return i;
	}

	return -1;
}

static INT32 FindAudioBufferIndexForPosition(movie_t *movie, INT64 sample)
{
	INT32 i;

	for (i = 0; i < movie->audiostream.buffer.size; i++)
	{
		movieaudioframe_t *frame = GetBufferSlot(&movie->audiostream.buffer, i);
		if (frame->firstsampleposition <= sample && sample < GetAudioFrameEndSample(frame))
			return i;
	}

	return -1;
}

static boolean IsPTSInVideoBuffer(movie_t *movie, INT64 pts)
{
	moviebuffer_t *buffer = &movie->videostream.buffer;

	if (buffer->size == 0)
		return false;

	movievideoframe_t *firstframe = GetBufferSlot(buffer, 0);
	movievideoframe_t *lastframe = GetBufferSlot(buffer, buffer->size - 1);

	return (firstframe->pts <= pts && pts < GetVideoFrameEndPTS(lastframe));
}

static INT64 GetSamplesPerFrame(INT64 numsamples, INT64 inputsamplerate)
{
	return numsamples * SAMPLE_RATE / inputsamplerate + 1;
}

//
// DECODING WORKER INITIALISATION
//

static void AllocateAVImage(moviedecodeworker_t *worker, avimage_t *image)
{
	AVCodecContext *context = worker->videostream.codeccontext;

	image->datasize = av_image_alloc(
		image->data, image->linesize,
		context->width, context->height,
		AV_PIX_FMT_RGBA, 1
	);
	if (image->datasize < 0)
		I_Error("FFmpeg: cannot allocate image");
}

static AVCodecContext *InitialiseDecoding(AVStream *stream)
{
	const AVCodec *codec;
	AVCodecContext *codeccontext;

	if (!stream)
		return NULL;

	codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!codec)
		I_Error("FFmpeg: cannot find codec");

	codeccontext = avcodec_alloc_context3(codec);
	if (!codeccontext)
		I_Error("FFmpeg: cannot allocate codec context");

	if (avcodec_parameters_to_context(codeccontext, stream->codecpar) < 0)
		I_Error("FFmpeg: cannot copy parameters to codec context");

	if (avcodec_open2(codeccontext, codec, NULL) < 0)
		I_Error("FFmpeg: cannot open codec");

	return codeccontext;
}

static void InitialiseImages(moviedecodeworker_t *worker)
{
	for (INT32 i = 0; i < worker->videostream.framepool.capacity; i++)
	{
		movievideoframe_t *frame = EnqueueBuffer(&worker->videostream.framepool);

		if (worker->usepatches)
		{
			INT32 size = worker->videostream.codeccontext->width * (sizeof(UINT32) + GetBytesPerPatchColumn(worker));
			frame->image.patch = malloc(size);
			if (!frame->image.patch)
				I_Error("FFmpeg: cannot allocate patch data");
		}
		else
		{
			AllocateAVImage(worker, &frame->image.rgba);
		}
	}

	if (worker->usepatches)
		AllocateAVImage(worker, &worker->tmpimage);
}

static void InitialiseVideoBuffer(movie_t *movie)
{
	movie->videostream.numplanes = 1;

	AVRational *fps = &movie->videostream.stream->avg_frame_rate;
	InitialiseBuffer(
		&movie->videostream.buffer,
		(INT64)STREAM_BUFFER_TIME / 1000 * fps->num / fps->den,
		sizeof(movievideoframe_t)
	);
}

static void InitialiseAudioBuffer(moviestream_t *stream, moviedecodeworker_t *worker)
{
	moviedecodeworkerstream_t *workerstream = &worker->audiostream;

	if (!stream->stream)
		return;

	stream->numplanes = av_sample_fmt_is_planar(AV_SAMPLE_FMT_S16) ? workerstream->codeccontext->channels : 1;

	INT64 samplesperframe = GetSamplesPerFrame(worker->frame->nb_samples, workerstream->codeccontext->sample_rate);

	InitialiseBuffer(
		&stream->buffer,
		STREAM_BUFFER_TIME / 1000 * workerstream->codeccontext->sample_rate / samplesperframe,
		sizeof(movieaudioframe_t)
	);

	CloneBuffer(&workerstream->framequeue, &stream->buffer);
	CloneBuffer(&workerstream->framepool, &stream->buffer);

	for (INT32 i = 0; i < workerstream->framepool.capacity; i++)
	{
		movieaudioframe_t *frame = EnqueueBuffer(&workerstream->framepool);

		if (!av_samples_alloc(
			frame->samples, NULL,
			worker->frame->channels, samplesperframe,
			AV_SAMPLE_FMT_S16, 1
		))
			I_Error("FFmpeg: cannot allocate samples");
	}
}

static void InitialisePacketQueue(moviedecodeworker_t *worker)
{
	InitialiseBuffer(&worker->packetqueue, NUM_PACKETS, sizeof(AVPacket*));
	CloneBuffer(&worker->packetpool, &worker->packetqueue);
	for (INT32 i = 0; i < worker->packetpool.capacity; i++)
	{
		AVPacket *packet = av_packet_alloc();
		if (!packet)
			I_Error("FFmpeg: cannot allocate packet");

		AVPacket **packetslot = EnqueueBuffer(&worker->packetpool);
		*packetslot = packet;
	}
}

static void InitialiseVideoConversion(moviedecodeworker_t *worker)
{
	worker->frame = av_frame_alloc();
	if (!worker->frame)
		I_Error("FFmpeg: cannot allocate frame");

	int width = worker->videostream.codeccontext->width;
	int height = worker->videostream.codeccontext->height;
	worker->scalingcontext = sws_getContext(
		width, height, worker->videostream.codeccontext->pix_fmt,
		width, height, AV_PIX_FMT_RGBA,
		SWS_BILINEAR,
		NULL,
		NULL,
		NULL
	);
	if (!worker->scalingcontext)
		I_Error("FFmpeg: cannot create scaling context");

	InitColorLUT(&worker->colorlut, pMasterPalette, true);
}

static void InitialiseAudioConversion(moviedecodeworker_t *worker)
{
	AVCodecContext *audiocodeccontext = worker->audiostream.codeccontext;
	worker->resamplingcontext = swr_alloc_set_opts(
		NULL,
		audiocodeccontext->channel_layout, AV_SAMPLE_FMT_S16, SAMPLE_RATE,
		audiocodeccontext->channel_layout, audiocodeccontext->sample_fmt, audiocodeccontext->sample_rate,
		0, NULL
	);
	if (!worker->resamplingcontext)
		I_Error("FFmpeg: cannot allocate resampling context");
	if (swr_init(worker->resamplingcontext))
		I_Error("FFmpeg: cannot initialise resampling context");
}

static void InitialiseDecodeWorker(movie_t *movie)
{
	moviedecodeworker_t *worker = &movie->decodeworker;
	moviestream_t *vstream = &movie->videostream;

	worker->usepatches = movie->usepatches;
	worker->videostream.index = vstream->index;
	worker->audiostream.index = movie->audiostream.index;
	worker->videostream.codeccontext = InitialiseDecoding(vstream->stream);
	worker->audiostream.codeccontext = InitialiseDecoding(movie->audiostream.stream);
	CloneBuffer(&worker->videostream.framequeue, &vstream->buffer);
	CloneBuffer(&worker->videostream.framepool, &vstream->buffer);
	InitialiseImages(worker);
	InitialisePacketQueue(worker);
	InitialiseVideoConversion(worker);
	InitialiseAudioConversion(worker);

	// Hack because we don't know the audio frame size in advance
	// so we initialise the audio buffer in the worker thread
	worker->videostream.stream = vstream;
	worker->audiostream.stream = &movie->audiostream;
}

//
// DECODING WORKER DEINITIALISATION
//

static void FlushFrameBuffers(moviestream_t *stream, moviedecodeworkerstream_t *workerstream)
{
	DequeueWholeBufferIntoBuffer(&workerstream->framepool, &stream->buffer);
	DequeueWholeBufferIntoBuffer(&workerstream->framepool, &workerstream->framequeue);
}

static void UninitialiseImages(movie_t *movie)
{
	moviedecodeworker_t *worker = &movie->decodeworker;

	FlushFrameBuffers(&movie->videostream, &worker->videostream);

	while (worker->videostream.framepool.size > 0)
	{
		movievideoframe_t *frame = DequeueBuffer(&worker->videostream.framepool);

		if (movie->usepatches)
			free(frame->image.patch);
		else
			av_freep(&frame->image.rgba.data[0]);
	}

	if (movie->usepatches)
		av_freep(&worker->tmpimage.data[0]);
}

static void UninitialiseDecodeWorkerStream(movie_t *movie, moviestream_t *stream, moviedecodeworkerstream_t *workerstream)
{
	FlushFrameBuffers(stream, workerstream);

	if (stream == &movie->videostream)
	{
		UninitialiseImages(movie);
	}
	else if (stream == &movie->audiostream)
	{
		while (workerstream->framepool.size > 0)
		{
			movieaudioframe_t *frame = DequeueBuffer(&workerstream->framepool);
			av_freep(&frame->samples[0]);
		}
	}

	UninitialiseBuffer(&workerstream->framepool);
	UninitialiseBuffer(&workerstream->framequeue);

	avcodec_free_context(&workerstream->codeccontext);
}

static void UninitialisePacketQueue(moviedecodeworker_t *worker)
{
	DequeueWholeBufferIntoBuffer(&worker->packetpool, &worker->packetqueue);
	while (worker->packetpool.size > 0)
		av_packet_free(DequeueBuffer(&worker->packetpool));
	UninitialiseBuffer(&worker->packetpool);
	UninitialiseBuffer(&worker->packetqueue);
}

static void UninitialiseDecodeWorker(movie_t *movie)
{
	moviedecodeworker_t *worker = &movie->decodeworker;

	UninitialiseDecodeWorkerStream(movie, &movie->videostream, &worker->videostream);
	UninitialiseDecodeWorkerStream(movie, &movie->audiostream, &worker->audiostream);
	UninitialisePacketQueue(worker);
	sws_freeContext(worker->scalingcontext);
	swr_free(&worker->resamplingcontext);
	av_frame_free(&worker->frame);
}

static void StopDecoderThread(moviedecodeworker_t *worker)
{
	I_lock_mutex(&worker->mutex);
	worker->stopping = true;
	I_unlock_mutex(worker->mutex);

	I_wake_one_cond(&worker->cond);

	boolean stopping;
	do
	{
		I_lock_mutex(&worker->mutex);
		stopping = worker->stopping;
		I_unlock_mutex(worker->mutex);
	} while (stopping);
}

//
// DECODING WORKER THREAD
//

static void SendPacket(moviedecodeworker_t *worker)
{
	AVCodecContext *context;
	AVPacket *packet;

	if (worker->packetqueue.size == 0)
		return;

	AVPacket **packetslot = DequeueBufferIntoBuffer(&worker->packetpool, &worker->packetqueue);
	packet = *packetslot;

	if (packet->stream_index == worker->videostream.index)
		context = worker->videostream.codeccontext;
	else if (packet->stream_index == worker->audiostream.index)
		context = worker->audiostream.codeccontext;
	else
		I_Error("FFmpeg: unexpected packet");

	if (avcodec_send_packet(context, packet) < 0)
		I_Error("FFmpeg: cannot send packet to the decoder");

	av_packet_unref(packet);
}

static boolean ReceiveFrame(moviedecodeworker_t *worker, moviedecodeworkerstream_t *stream)
{
	int error = avcodec_receive_frame(stream->codeccontext, worker->frame);

	if (error == 0) // Frame received successfully
		return true;
	else if (error == AVERROR_EOF) // End of movie reached
		return false;
	else if (error == AVERROR(EAGAIN)) // More packets needed
		return false;
	else // Error
		I_Error("FFmpeg: cannot receive frame");
}

static void ConvertRGBAToPatch(moviedecodeworker_t *worker, UINT8 * restrict src, UINT8 * restrict dst)
{
	INT32 width = worker->frame->width;
	INT32 height = worker->frame->height;
	UINT16 *lut = worker->colorlut.table;
	INT32 stride = 4 * width;

	// Write column offsets
	INT32 bytespercolumn = GetBytesPerPatchColumn(worker);
	for (INT32 x = 0; x < width; x++)
		WRITEUINT32(dst, width * sizeof(UINT32) + x * bytespercolumn + (POST_BASE_BYTES - 1));

	for (INT32 x = 0; x < width; x++)
	{
		INT32 y = 0;
		UINT8 *srcptr = &src[4 * x];

		// Write posts
		while (y < height)
		{
			INT32 postend = min(y + POST_MAX_HEIGHT, height);

			// Header
			WRITEUINT8(dst, y ? POST_MAX_HEIGHT : 0); // Top delta
			WRITEUINT8(dst, postend - y); // Length
			WRITEUINT8(dst, 0); // Unused

			// Pixel data
			while (y < postend)
			{
				UINT8 r = srcptr[0];
				UINT8 g = srcptr[1];
				UINT8 b = srcptr[2];
				UINT8 i = lut[CLUTINDEX(r, g, b)];
				WRITEUINT8(dst, i);

				srcptr += stride;
				y++;
			}

			// Unused trail byte
			WRITEUINT8(dst, 0);
		}

		// Terminate column
		WRITEUINT8(dst, 0xFF);
	}
}

static void ParseVideoFrame(moviedecodeworker_t *worker)
{
	movievideoframe_t *frame = PeekBuffer(&worker->videostream.framepool);

	frame->id = worker->nextframeid;
	worker->nextframeid++;

	frame->pts = worker->frame->pts;
	frame->duration = worker->frame->pkt_duration;

	avimage_t *image = worker->usepatches ? &worker->tmpimage : &frame->image.rgba;
	sws_scale(
		worker->scalingcontext,
		(UINT8 const * const *)worker->frame->data,
		worker->frame->linesize,
		0,
		worker->frame->height,
		image->data,
		image->linesize
	);

	if (worker->usepatches)
		ConvertRGBAToPatch(worker, worker->tmpimage.data[0], frame->image.patch);

	I_lock_mutex(&worker->mutex);
	DequeueBufferIntoBuffer(&worker->videostream.framequeue, &worker->videostream.framepool);
	I_unlock_mutex(worker->mutex);
}

static void ParseAudioFrame(moviedecodeworker_t *worker)
{
	movieaudioframe_t *frame;

	if (!worker->audiostream.framequeue.data)
		InitialiseAudioBuffer(worker->audiostream.stream, worker);

	frame = PeekBuffer(&worker->audiostream.framepool);

	INT64 maxsamples = GetSamplesPerFrame(worker->frame->nb_samples, worker->audiostream.codeccontext->sample_rate);

	int numoutputsamples = swr_convert(
		worker->resamplingcontext,
		frame->samples,
		maxsamples,
		(UINT8 const **)worker->frame->data,
		worker->frame->nb_samples
	);
	if (numoutputsamples < 0)
		I_Error("FFmpeg: cannot convert audio frame");

	frame->pts = worker->frame->pts;
	frame->numsamples = numoutputsamples;

	I_lock_mutex(&worker->mutex);
	DequeueBufferIntoBuffer(&worker->audiostream.framequeue, &worker->audiostream.framepool);
	I_unlock_mutex(worker->mutex);
}

static void FlushDecodeWorker(moviedecodeworker_t *worker)
{
	worker->flushing = true;
	DequeueWholeBufferIntoBuffer(&worker->packetpool, &worker->packetqueue);
	I_wake_one_cond(&worker->cond);
}

static void FlushStream(moviedecodeworker_t *worker, moviedecodeworkerstream_t *stream)
{
	// Flush the decoder
	if (avcodec_send_packet(stream->codeccontext, NULL) < 0)
		I_Error("FFmpeg: cannot flush decoder");

	while (true) {
		int error = avcodec_receive_frame(stream->codeccontext, worker->frame);
		if (error == 0)
			continue;
		else if (error == AVERROR_EOF)
			break;
		else
			I_Error("FFmpeg: cannot receive frame");
	}

	avcodec_flush_buffers(stream->codeccontext);

	I_lock_mutex(&worker->mutex);
	DequeueWholeBufferIntoBuffer(&stream->framepool, &stream->framequeue);
	I_unlock_mutex(worker->mutex);
}

static void FlushDecoding(moviedecodeworker_t *worker)
{
	FlushStream(worker, &worker->videostream);
	FlushStream(worker, &worker->audiostream);

	I_lock_mutex(&worker->mutex);
	worker->flushing = false;
	I_unlock_mutex(worker->mutex);
}

static void DecoderThread(moviedecodeworker_t *worker)
{
	I_lock_mutex(&worker->condmutex);

	while (true)
	{
		boolean stopping;
		INT64 flushing;
		boolean queuesfull;

		I_lock_mutex(&worker->mutex);
		{
			stopping = worker->stopping;
			flushing = worker->flushing;

			INT32 vsize = worker->videostream.framepool.size;
			INT32 asize = worker->audiostream.framepool.size;
			queuesfull = (vsize == 0 || (worker->audiostream.framequeue.data && asize == 0));
		}
		I_unlock_mutex(worker->mutex);

		if (stopping)
			break;
		if (flushing)
			FlushDecoding(worker);
		if (queuesfull)
		{
			I_hold_cond(&worker->cond, worker->condmutex);
			continue;
		}

		if (ReceiveFrame(worker, &worker->videostream))
		{
			ParseVideoFrame(worker);
		}
		else if (ReceiveFrame(worker, &worker->audiostream))
		{
			ParseAudioFrame(worker);
		}
		else
		{
			boolean sent = false;

			I_lock_mutex(&worker->mutex);
			{
				if (worker->packetqueue.size > 0)
				{
					SendPacket(worker);
					sent = true;
				}
			}
			I_unlock_mutex(worker->mutex);

			if (!sent)
				I_hold_cond(&worker->cond, worker->condmutex);
		}
	}

	I_lock_mutex(&worker->mutex);
	worker->stopping = false;
	I_unlock_mutex(worker->mutex);

	I_unlock_mutex(worker->condmutex);
}

//
// FRAME CLEARING
//

static void ClearOldestFrame(movie_t *movie, moviestream_t *stream, moviedecodeworkerstream_t *workerstream)
{
	DequeueBufferIntoBuffer(&workerstream->framepool, &stream->buffer);
	I_wake_one_cond(&movie->decodeworker.cond);
}

static void ClearOldVideoFrames(movie_t *movie)
{
	moviebuffer_t *buffer = &movie->videostream.buffer;
	INT64 limit = MSToVideoPTS(movie, movie->position - STREAM_BUFFER_TIME / 2);

	while (buffer->size > 0 && ((movievideoframe_t*)PeekBuffer(buffer))->pts < limit)
		ClearOldestFrame(movie, &movie->videostream, &movie->decodeworker.videostream);
}

static void ClearOldAudioFrames(movie_t *movie)
{
	moviebuffer_t *buffer = &movie->audiostream.buffer;
	INT64 limit = max(MSToAudioPTS(movie, movie->position - STREAM_BUFFER_TIME / 2), 0);

	while (buffer->size > 0 && GetAudioFrameEndPTS(movie, PeekBuffer(buffer)) < limit)
		ClearOldestFrame(movie, &movie->audiostream, &movie->decodeworker.audiostream);
}

static void ClearAllFrames(movie_t *movie)
{
	while (movie->videostream.buffer.size != 0)
		ClearOldestFrame(movie, &movie->videostream, &movie->decodeworker.videostream);

	while (movie->audiostream.buffer.size != 0)
		ClearOldestFrame(movie, &movie->audiostream, &movie->decodeworker.audiostream);
}

//
// DEMUXING AND I/O
//

static lumpnum_t FindMovieLumpNum(const char *name)
{
	INT32 wadnum;

	for (wadnum = numwadfiles - 1; wadnum >= 0; wadnum--)
	{
		char fullname[256];
		UINT16 lumpnum;

		snprintf(fullname, sizeof(fullname), "Movies/%s", name);
		lumpnum = W_CheckNumForFullNamePK3(fullname, wadnum, 0);
		if (lumpnum != INT16_MAX)
			return (wadnum << 16) + lumpnum;
	}

	return LUMPERROR;
}

static void CacheMovieLump(movie_t *movie, const char *name)
{
	lumpnum_t lumpnum;

	lumpnum = FindMovieLumpNum(name);
	if (lumpnum == LUMPERROR)
		I_Error("FFmpeg: cannot find movie lump");

	movie->lumpsize = W_LumpLength(lumpnum);
	movie->lumpdata = malloc(movie->lumpsize);
	if (!movie->lumpdata)
		I_Error("FFmpeg: cannot allocate lump data");
	W_ReadLump(lumpnum, movie->lumpdata);
}

static int ReadStream(void *owner, uint8_t *buffer, int buffersize)
{
	movie_t *movie = owner;
	size_t bs = buffersize;
	buffersize = min(bs, movie->lumpsize - movie->lumpposition);
	memcpy(buffer, &movie->lumpdata[movie->lumpposition], buffersize);
	movie->lumpposition += buffersize;
	return buffersize;
}

static INT64 SeekStream(void *owner, int64_t offset, int whence)
{
	movie_t *movie = owner;

	if (whence == SEEK_CUR)
		offset += movie->lumpposition;
	else if (whence == SEEK_END)
		offset += movie->lumpsize;
	else if (whence == AVSEEK_SIZE)
		return movie->lumpsize;

	movie->lumpposition = offset;

	return 0;
}

static void InitialiseDemuxing(movie_t *movie)
{
	movie->formatcontext = avformat_alloc_context();
	if (!movie->formatcontext)
		I_Error("FFmpeg: cannot allocate format context");

	UINT8 *streambuffer = av_malloc(IO_BUFFER_SIZE);
	if (!streambuffer)
		I_Error("FFmpeg: cannot allocate stream buffer");
	movie->lumpposition = 0;

	movie->formatcontext->pb = avio_alloc_context(streambuffer, IO_BUFFER_SIZE, 0, movie, ReadStream, NULL, SeekStream);
	if (!movie->formatcontext->pb)
		I_Error("FFmpeg: cannot allocate I/O context");

	if (avformat_open_input(&movie->formatcontext, NULL, NULL, NULL) != 0)
		I_Error("FFmpeg: cannot open format context");

	if (avformat_find_stream_info(movie->formatcontext, NULL) < 0)
		I_Error("FFmpeg: cannot find stream information");

	movie->videostream.index = av_find_best_stream(movie->formatcontext, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (movie->videostream.index < 0)
		I_Error("FFmpeg: cannot find video stream");
	movie->videostream.stream = movie->formatcontext->streams[movie->videostream.index];

	movie->audiostream.index = av_find_best_stream(movie->formatcontext, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (movie->audiostream.index >= 0)
		movie->audiostream.stream = movie->formatcontext->streams[movie->audiostream.index];
}

static void UninitialiseDemuxing(movie_t *movie)
{
	av_freep(&movie->formatcontext->pb->buffer);
	avio_context_free(&movie->formatcontext->pb);
	free(movie->lumpdata);

	avformat_close_input(&movie->formatcontext);
}

//
// MAIN THREAD
//

static boolean ReadPacket(movie_t *movie)
{
	moviedecodeworker_t *worker = &movie->decodeworker;
	AVPacket **packetslot = PeekBuffer(&worker->packetpool);
	AVPacket *packet = *packetslot;

	int error = av_read_frame(movie->formatcontext, packet);

	if (error == AVERROR_EOF)
		return false;
	else if (error < 0)
		I_Error("FFmpeg: cannot read packet");
	else if (packet->stream_index == movie->videostream.index
		|| packet->stream_index == movie->audiostream.index)
	{
		DequeueBufferIntoBuffer(&worker->packetqueue, &worker->packetpool);
		I_wake_one_cond(&worker->cond);
	}
	else
		av_packet_unref(packet);

	return true;
}

static void PollVideoFrameQueue(movie_t *movie)
{
	moviedecodeworker_t *worker = &movie->decodeworker;

	if (worker->videostream.framequeue.size != 0)
	{
		DequeueWholeBufferIntoBuffer(&movie->videostream.buffer, &worker->videostream.framequeue);
		I_wake_one_cond(&worker->cond);
	}
}

static void PollAudioFrameQueue(movie_t *movie)
{
	moviedecodeworker_t *worker = &movie->decodeworker;
	moviebuffer_t *buffer = &movie->audiostream.buffer;

	if (worker->audiostream.framequeue.size != 0)
	{
		while (worker->audiostream.framequeue.size > 0)
		{
			movieaudioframe_t *frame = DequeueBufferIntoBuffer(buffer, &worker->audiostream.framequeue);

			if (buffer->size > 1)
			{
				movieaudioframe_t *prevframe = GetBufferSlot(buffer, buffer->size - 2);
				frame->firstsampleposition = prevframe->firstsampleposition + prevframe->numsamples;
			}
			else
			{
				frame->firstsampleposition = PTSToSamples(movie, frame->pts);
			}
		}

		I_wake_one_cond(&worker->cond);
	}
}

static void Seek(movie_t *movie)
{
	movie->seeking = true;

	ClearAllFrames(movie);

	if (avformat_seek_file(
		movie->formatcontext,
		movie->videostream.index,
		MSToVideoPTS(movie, max(movie->position - 5000, 0)),
		MSToVideoPTS(movie, movie->position),
		MSToVideoPTS(movie, movie->position),
		0
	) < 0)
		I_Error("FFmpeg: cannot seek");

	FlushDecodeWorker(&movie->decodeworker);
}

static void UpdateSeeking(movie_t *movie)
{
	moviebuffer_t *buffer = &movie->videostream.buffer;

	if (movie->seeking && buffer->size > 0)
	{
		movievideoframe_t *lastframe = GetBufferSlot(buffer, buffer->size - 1);
		INT64 target = movie->position + 250;
		INT64 targetdist = target - VideoPTSToMS(movie, GetVideoFrameEndPTS(lastframe));

		if (targetdist <= 0 || targetdist > MAX_SEEK_DISTANCE)
			movie->seeking = false;
	}

	boolean inbuffer = IsPTSInVideoBuffer(movie, MSToVideoPTS(movie, movie->position));
	if (!(inbuffer || movie->seeking || movie->decodeworker.flushing || buffer->size == 0))
		Seek(movie);

	if (movie->audioposition != -1)
	{
		INT64 desync = llabs(SamplesToMS(movie->audioposition) - movie->position);
		if (desync > MAX_AUDIO_DESYNC)
			movie->audioposition = -1;
	}
}

//
// API
//

movie_t *MovieDecode_Play(const char *name, boolean usepatches)
{
	movie_t *movie;

	movie = calloc(1, sizeof(*movie));
	if (!movie)
		I_Error("FFmpeg: cannot allocate movie object");

	movie->lastvideoframeusedid = 0;
	movie->position = 0;
	movie->audioposition = 0;
	movie->usepatches = usepatches;

	CacheMovieLump(movie, name);
	InitialiseDemuxing(movie);
	InitialiseVideoBuffer(movie);
	InitialiseDecodeWorker(movie);

	I_spawn_thread("decode-movie", (I_thread_fn)DecoderThread, &movie->decodeworker);

	return movie;
}

void MovieDecode_Stop(movie_t **movieptr)
{
	movie_t *movie = *movieptr;

	if (!movie)
		return;

	StopDecoderThread(&movie->decodeworker);

	if (S_MusicType() == MU_MOVIE)
		S_StopMusic();

	UninitialiseDecodeWorker(movie);
	UninitialiseBuffer(&movie->videostream.buffer);
	UninitialiseBuffer(&movie->audiostream.buffer);
	UninitialiseDemuxing(movie);

	free(movie);
	*movieptr = NULL;
}

void MovieDecode_SetPosition(movie_t *movie, INT64 ms)
{
	movie->position = ms;

	if (movie->audioposition == -1)
		movie->audioposition = MSToSamples(movie->position);
}

void MovieDecode_Seek(movie_t *movie, INT64 ms)
{
	movie->position = ms;

	if (movie->audioposition == -1)
		movie->audioposition = MSToSamples(movie->position);
}

void MovieDecode_Update(movie_t *movie)
{
	moviedecodeworker_t *worker = &movie->decodeworker;

	I_lock_mutex(&worker->mutex);
	{
		while (worker->packetpool.size > 0 && ReadPacket(movie))
			;

		if (!worker->flushing)
		{
			PollVideoFrameQueue(movie);
			PollAudioFrameQueue(movie);
		}

		UpdateSeeking(movie);

		if (movie->videostream.buffer.size > 0)
		{
			ClearOldVideoFrames(movie);
			ClearOldAudioFrames(movie);
		}
	}
	I_unlock_mutex(worker->mutex);
}

void MovieDecode_SetImageFormat(movie_t *movie, boolean usepatches)
{
	moviedecodeworker_t *worker = &movie->decodeworker;

	if (usepatches == movie->usepatches)
		return;

	StopDecoderThread(worker);
	UninitialiseImages(movie);
	ClearAllFrames(movie);

	movie->usepatches = usepatches;
	worker->usepatches = usepatches;

	InitialiseImages(worker);
	I_spawn_thread("decode-movie", (I_thread_fn)DecoderThread, worker);
}

INT64 MovieDecode_GetDuration(movie_t *movie)
{
	return PTSToMS(movie->formatcontext->duration);
}

void MovieDecode_GetDimensions(movie_t *movie, INT32 *width, INT32 *height)
{
	AVCodecContext *context = movie->decodeworker.videostream.codeccontext;
	*width = context->width;
	*height = context->height;
}

UINT8 *MovieDecode_GetImage(movie_t *movie)
{
	INT32 bufferindex = FindVideoBufferIndexForPosition(movie, MSToVideoPTS(movie, movie->position));
	movievideoframe_t *frame = GetBufferSlot(&movie->videostream.buffer, bufferindex);

	if (frame && movie->lastvideoframeusedid != frame->id)
	{
		movie->lastvideoframeusedid = frame->id;
		return movie->usepatches ? frame->image.patch : frame->image.rgba.data[0];
	}
	else
	{
		return NULL;
	}
}

INT32 MovieDecode_GetPatchBytes(movie_t *movie)
{
	AVCodecContext *context = movie->decodeworker.videostream.codeccontext;
	return context->width * (4 + GetBytesPerPatchColumn(&movie->decodeworker));
}

void MovieDecode_CopyAudioSamples(movie_t *movie, void *mem, size_t size)
{
	moviestream_t *stream = &movie->audiostream;
	moviebuffer_t *buffer = &stream->buffer;
	AVCodecContext *codeccontext = movie->decodeworker.audiostream.codeccontext;
	UINT8 *membytes = mem;

	if (movie->audioposition == -1)
		return;

	// Here, if using packed audio, the sample size includes both channels
	INT32 samplesize = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
	if (!av_sample_fmt_is_planar(AV_SAMPLE_FMT_S16))
		samplesize *= codeccontext->channels;
	INT64 numsamples = size / samplesize;

	INT32 startbufferindex = FindAudioBufferIndexForPosition(movie, movie->audioposition);
	INT32 endbufferindex = FindAudioBufferIndexForPosition(movie, movie->audioposition + numsamples);
	size_t mempos = 0;

	if (startbufferindex != -1 && endbufferindex != -1)
	{
		INT32 i;
		for (i = startbufferindex; i <= endbufferindex; i++)
		{
			movieaudioframe_t *frame = GetBufferSlot(buffer, i);
			INT32 startsample = max(movie->audioposition - frame->firstsampleposition, 0);
			INT32 sizeforframe = min(size, (frame->numsamples - startsample) * samplesize);
			void *ptr = &frame->samples[0][startsample * samplesize];

			memcpy(&membytes[mempos], ptr, sizeforframe);
			mempos += sizeforframe;
			size -= sizeforframe;
		}
	}

	movie->audioposition += numsamples;
}