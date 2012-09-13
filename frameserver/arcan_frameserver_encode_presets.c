#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <sys/stat.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#include "../arcan_math.h"
#include "../arcan_general.h"
#include "../arcan_event.h"
#include "arcan_frameserver.h"
#include "arcan_frameserver_encode_presets.h"

static void vcodec_defaults(struct codec_ent* dst, unsigned width, unsigned height, float fps, unsigned vbr)
{
	AVCodecContext* ctx   = dst->storage.video.context;
	size_t base_sz = width * height;

	ctx->width     = width;
	ctx->height    = height;
	ctx->time_base = av_d2q(1.0 / fps, 1000000);
	ctx->bit_rate  = vbr;
	ctx->pix_fmt   = PIX_FMT_YUV420P;
	ctx->gop_size  = 10;

	AVFrame* pframe = avcodec_alloc_frame();
	pframe->data[0] = av_malloc( (base_sz * 3) / 2);
	pframe->data[1] = pframe->data[0] + base_sz;
	pframe->data[2] = pframe->data[1] + base_sz / 4;
	pframe->linesize[0] = width;
	pframe->linesize[1] = width / 2;
	pframe->linesize[2] = width / 2;

	dst->storage.video.pframe = pframe;
}

static bool default_vcodec_setup(struct codec_ent* dst, unsigned width, unsigned height, float fps, unsigned vbr)
{
	AVCodecContext* ctx = dst->storage.video.context;

	assert(width % 2 == 0);
	assert(height % 2 == 0);
	assert(fps > 0 && fps <= 60);
	assert(ctx);

	vcodec_defaults(dst, width, height, fps, vbr);
	if (vbr <= 10){
		vbr = width * height + (width * height) * ( (float) vbr / 10.0f );
	}

	ctx->bit_rate = vbr;

	if (avcodec_open2(dst->storage.video.context, dst->storage.video.codec, NULL) != 0){
		dst->storage.video.codec   = NULL;
		dst->storage.video.context = NULL;
		avcodec_close(dst->storage.video.context);
		return false;
	}

	return true;
}

static bool default_acodec_setup(struct codec_ent* dst, unsigned channels, unsigned samplerate, unsigned abr)
{
	AVCodecContext* ctx = dst->storage.audio.context;
	AVCodec* codec = dst->storage.audio.codec;

	assert(channels == 2);
	assert(samplerate > 0 && samplerate <= 48000);
	assert(codec);

	ctx->channels       = channels;
	ctx->channel_layout = av_get_default_channel_layout(channels);
	ctx->sample_rate    = samplerate;
	ctx->time_base      = av_d2q(1.0 / (double) samplerate, 1000000);

	bool float_found = false, sint16_found = false;

	unsigned i = 0;
/* prefer sint16, but codecs e.g. vorbis requires float */
	while(codec->sample_fmts[i] != AV_SAMPLE_FMT_NONE){
		if (codec->sample_fmts[i] == AV_SAMPLE_FMT_S16)
			sint16_found = true;
		else if (codec->sample_fmts[i] == AV_SAMPLE_FMT_FLT)
			float_found = true;
		i++;
	}

	ctx->sample_fmt  = float_found && !sint16_found ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_S16;

/* rough quality estimate */
	if (abr <= 10)
		abr = 1024 * ( 320 - 240 * ((float)(11.0 - abr) / 11.0) );
	else
		ctx->bit_rate = abr;

	LOG("arcan_frameserver(encode) -- audio setup @ %d hz, %d kbit/s\n", samplerate, abr);
	if (avcodec_open2(dst->storage.audio.context, dst->storage.audio.codec, NULL) != 0){
		avcodec_close(dst->storage.audio.context);
		dst->storage.audio.context = NULL;
		dst->storage.audio.codec   = NULL;
		return false;
	}

	return true;
}

static bool default_format_setup(struct codec_ent* ctx)
{
	avformat_write_header(ctx->storage.container.context, NULL);
	return false;
}

/* would be nice with some decent evaluation of all the parameters and their actual cost / benefit. */
static bool setup_cb_vp8(struct codec_ent* dst, unsigned width, unsigned height, float fps, unsigned vbr)
{
	AVDictionary* opts = NULL;
	const char* const lif = fps > 30.0 ? "25" : "16";
	const char* deadline  = "good";

	vcodec_defaults(dst, width, height, fps, vbr);

/* options we want to set irrespective of bitrate */
	if (height > 720){
		av_dict_set(&opts, "slices", "4", 0);
		av_dict_set(&opts, "qmax", "54", 0);
		av_dict_set(&opts, "qmin", "11", 0);
		av_dict_set(&opts, "vprofile", "1", 0);

	} else if (height > 480){
		av_dict_set(&opts, "slices", "4", 0);
		av_dict_set(&opts, "qmax", "54", 0);
		av_dict_set(&opts, "qmin", "11", 0);
		av_dict_set(&opts, "vprofile", "1", 0);

	} else if (height > 360){
		av_dict_set(&opts, "slices", "4", 0);
		av_dict_set(&opts, "qmax", "54", 0);
		av_dict_set(&opts, "qmin", "11", 0);
		av_dict_set(&opts, "vprofile", "1", 0);
	}
	else {
		av_dict_set(&opts, "qmax", "63", 0);
		av_dict_set(&opts, "qmin", "0", 0);
		av_dict_set(&opts, "vprofile", "0", 0);
	}

	av_dict_set(&opts, "lag-in-frames", lif, 0);
	av_dict_set(&opts, "g", "120", 0);


	if (vbr <= 10){
/* "HD" */
		if (height > 360)
			vbr = 1024 + 1024 * ( (float)(vbr+1) /11.0 * 2.0 );
/* "LD" */
		else
			vbr = 365 + 365 * ( (float)(vbr+1) /11.0 * 2.0 );

		vbr *= 1024; /* to bit/s */
	}

	av_dict_set(&opts, "quality", "realtime", 0);
	dst->storage.video.context->bit_rate = vbr;

	LOG("arcan_frameserver(encode) -- video setup @ %d * %d, %f fps, %d kbit / s.\n", width, height, fps, vbr / 1024);
	if (avcodec_open2(dst->storage.video.context, dst->storage.video.codec, &opts) != 0){
		avcodec_close(dst->storage.video.context);
		dst->storage.video.context = NULL;
		dst->storage.video.codec   = NULL;
		return false;
	}

	return true;
}

static struct codec_ent vcodec_tbl[] = {
	{.kind = CODEC_VIDEO, .name = "libvpx",  .shortname = "VP8",  .id = CODEC_ID_VP8,  .setup.video = setup_cb_vp8},
	{.kind = CODEC_VIDEO, .name = "libx264", .shortname = "H264", .id = 0,             .setup.video = default_vcodec_setup },
	{.kind = CODEC_VIDEO, .name = "ffv1",    .shortname = "FFV1", .id = CODEC_ID_FFV1, .setup.video = default_vcodec_setup },
};

static struct codec_ent acodec_tbl[] = {
	{.kind = CODEC_AUDIO, .name = "libvorbis", .shortname = "VORBIS", .id = 0,             .setup.audio = default_acodec_setup },
	{.kind = CODEC_AUDIO, .name = "FLAC",      .shortname = "FLAC",   .id = CODEC_ID_FLAC, .setup.audio = default_acodec_setup },
	{.kind = CODEC_AUDIO, .name = "RAWS16LE",  .shortname = "RAW",    .id = 0,             .setup.audio = default_acodec_setup }
};

static struct codec_ent fcodec_tbl[] = {
	{.kind = CODEC_FORMAT, .name = "matroska", .shortname = "MKV", .id = 0, .setup.muxer = default_format_setup },
	{.kind = CODEC_FORMAT, .name = "mpeg4",    .shortname = "MP4", .id = 0, .setup.muxer = default_format_setup },
	{.kind = CODEC_FORMAT, .name = "avi",      .shortname = "AVI", .id = 0, .setup.muxer = default_format_setup }
};

static struct codec_ent lookup_default(const char* const req, struct codec_ent* tbl, size_t nmemb)
{
	struct codec_ent res = {.name = req};

	if (req){
/* make sure that if the user supplies a name already in the standard table, that we get the same
 * prefix setup function */
		for (int i = 0; i < nmemb; i++)
			if (tbl[i].name != NULL && strcmp(req, tbl[i].name) == 0){
				memcpy(&res, &tbl[i], sizeof(struct codec_ent));
				res.storage.video.codec = avcodec_find_encoder_by_name(req);
			}

/* if the codec specified is unknown (to us) then let avcodec try and sort it up, return default setup */
		if (res.storage.video.codec == NULL){
			res.storage.video.codec = avcodec_find_encoder_by_name(req);
		}
	}

/* if the user didn't supply an explicit codec, or one was not found, search the table for reasonable default */
	for (int i = 0; i < nmemb && res.storage.video.codec == NULL; i++)
		if (tbl[i].name != NULL && tbl[i].id == 0){
			memcpy(&res, &tbl[i], sizeof(struct codec_ent));
			res.storage.video.codec = avcodec_find_encoder_by_name(tbl[i].name);
		}
		else{
			memcpy(&res, &tbl[i], sizeof(struct codec_ent));
			res.storage.video.codec = avcodec_find_encoder(tbl[i].id);
		}

	return res;
}

struct codec_ent encode_getvcodec(const char* const req, int flags)
{
 	struct codec_ent a = lookup_default(req, vcodec_tbl, sizeof(vcodec_tbl) / sizeof(vcodec_tbl[0]));
	LOG("codec setup: %" PRIxPTR "\n", (intptr_t)a.setup.video);
	if (a.storage.video.codec && !a.setup.video)
		a.setup.video = default_vcodec_setup;

	if (!a.storage.video.codec)
		return a;

	a.storage.video.context = avcodec_alloc_context3( a.storage.video.codec );
	if (flags & AVFMT_GLOBALHEADER)
		a.storage.video.context->flags |= CODEC_FLAG_GLOBAL_HEADER;

	return a;
}

struct codec_ent encode_getacodec(const char* const req, int flags)
{
	struct codec_ent res = lookup_default(req, acodec_tbl, sizeof(acodec_tbl) / sizeof(acodec_tbl[0]));

	if (res.storage.audio.codec && !res.setup.audio)
		res.setup.audio = default_acodec_setup;

	if (!res.storage.audio.codec)
		return res;

	res.storage.audio.context = avcodec_alloc_context3( res.storage.audio.codec );
	if ( (flags & AVFMT_GLOBALHEADER) > 0){
		res.storage.audio.context->flags |= CODEC_FLAG_GLOBAL_HEADER;
	}

	LOG("audio setup, %s\n", res.name);
	return res;
}

/* slightly difference scanning function here so can't re-use lookup_default */
struct codec_ent encode_getcontainer(const char* const requested, int dst)
{
	char fdbuf[16];
	AVFormatContext* ctx;

	struct codec_ent res = {0};
	res.storage.container.format = av_guess_format("matroska", NULL, NULL);

	if (!res.storage.container.format){
		LOG("arcan_frameserver(encode) -- couldn't find a suitable container.\n");
	}

	ctx = avformat_alloc_context();
	ctx->oformat = res.storage.container.format;

/* ugly hack around not having a way of mapping filehandle to fd WITHOUT going through open, sic. */
	sprintf(fdbuf, "pipe:%d", dst);
	int rv = avio_open2(&ctx->pb, fdbuf, AVIO_FLAG_WRITE, NULL, NULL);
	ctx->pb->seekable = AVIO_SEEKABLE_NORMAL;

	res.storage.container.context = ctx;
	res.setup.muxer = default_format_setup;

	return res;
}
