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
	pframe->pts = 0;

	dst->storage.video.pframe = pframe;
}

static bool default_vcodec_setup(struct codec_ent* dst, unsigned width, unsigned height, float fps, unsigned vbr, bool stream)
{
	AVCodecContext* ctx = dst->storage.video.context;

	assert(width % 2 == 0);
	assert(height % 2 == 0);
	assert(fps > 0 && fps <= 60);
	assert(ctx);

	vcodec_defaults(dst, width, height, fps, vbr);
/* terrible */
	if (vbr <= 10){
		int npx = width * height;
		vbr = 150 * 1024;
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

#if LIBAVCODEC_VERSION_MAJOR > 53
	ctx->channel_layout = av_get_default_channel_layout(channels);
#endif

	ctx->sample_rate    = samplerate;
	ctx->time_base      = av_d2q(1.0 / (double) samplerate, 1000000);

	unsigned i = 0;
/* prefer sint16, but codecs e.g. vorbis requires float */
	while(codec->sample_fmts[i] != AV_SAMPLE_FMT_NONE){
		if (codec->sample_fmts[i] == AV_SAMPLE_FMT_S16){
			ctx->sample_fmt = AV_SAMPLE_FMT_S16;
			break;
		}
		else if (codec->sample_fmts[i] == AV_SAMPLE_FMT_FLT){
			ctx->sample_fmt   = AV_SAMPLE_FMT_FLT;
			break;
		}
		else if (codec->sample_fmts[i] == AV_SAMPLE_FMT_FLTP){
			ctx->sample_fmt   = AV_SAMPLE_FMT_FLTP;
			break;
		}
		i++;
	}

/* rough quality estimate */
	if (abr <= 10)
		abr = 1024 * ( 320 - 240 * ((float)(11.0 - abr) / 11.0) );
	else
		ctx->bit_rate = abr;

	LOG("(encode) -- audio setup @ %d hz, %d kbit/s\n", samplerate, abr);
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

static bool setup_cb_x264(struct codec_ent* dst, unsigned width, unsigned height, float fps, unsigned vbr, bool stream)
{
	AVDictionary* opts = NULL;
	const char* const lif = fps > 30.0 ? "25" : "16";
	float vbrf = 1000 * (height >= 720 ? 2.0 : 1.0);

	vcodec_defaults(dst, width, height, fps, vbr);
	if (vbr == 10){
		av_dict_set(&opts, "preset", "medium", 0);
		av_dict_set(&opts, "crf", "4", 0);
		vbr = vbrf * 1200;
	} else if (vbr == 9){
		av_dict_set(&opts, "preset", "medium", 0);
		av_dict_set(&opts, "crf", "8", 0);
		vbr = vbrf * 1100;
	} else if (vbr == 8){
		av_dict_set(&opts, "preset", "medium", 0);
		av_dict_set(&opts, "crf", "14", 0);
		vbr = vbrf * 1000;
	} else if (vbr == 7){
		av_dict_set(&opts, "preset", "medium", 0);
		av_dict_set(&opts, "crf", "18", 0);
		vbr = vbrf * 900;
	} else if (vbr == 6){
		av_dict_set(&opts, "preset", "fast", 0	);
		av_dict_set(&opts, "crf", "22", 0);
		vbr = vbrf * 800;
	} else if (vbr == 5){
		av_dict_set(&opts, "preset", "fast", 0	);
		av_dict_set(&opts, "crf", "24", 0);
		vbr = vbrf * 700;
	} else if (vbr == 4){
		av_dict_set(&opts, "preset", "faster", 0);
		av_dict_set(&opts, "crf", "26", 0);
		vbr = vbrf * 600;
	} else if (vbr == 3){
		av_dict_set(&opts, "preset", "faster", 0	);
		av_dict_set(&opts, "crf", "32", 0);
		vbr = vbrf * 550;
	} else if (vbr == 2){
		av_dict_set(&opts, "preset", "superfast", 0);
		av_dict_set(&opts, "crf", "36", 0);
		vbr = vbrf * 400;
	} else if (vbr == 1){
		av_dict_set(&opts, "preset", "superfast", 0);
		av_dict_set(&opts, "crf", "44", 0);
		vbr = vbrf * 350;
	} else if (vbr == 0){
		av_dict_set(&opts, "preset", "superfast", 0);
		av_dict_set(&opts, "crf", "48", 0);
		vbr = vbrf * 300;
	} else {
		av_dict_set(&opts, "preset", "medium", 0);
		av_dict_set(&opts, "crf", "25", 0);
	}

	if (stream)
		av_dict_set(&opts, "preset", "faster", 0);

	dst->storage.video.context->bit_rate = vbr;

	LOG("(encode) -- video setup @ %d * %d, %f fps, %d kbit / s.\n", width, height, fps, vbr / 1000);
	
	if (avcodec_open2(dst->storage.video.context, dst->storage.video.codec, &opts) != 0){
		avcodec_close(dst->storage.video.context);
		dst->storage.video.context = NULL;
		dst->storage.video.codec   = NULL;
		return false;
	}

	return true;
}

/* would be nice with some decent evaluation of all the parameters and their actual cost / benefit. */
static bool setup_cb_vp8(struct codec_ent* dst, unsigned width, unsigned height, float fps, unsigned vbr, bool stream)
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

	LOG("(encode) -- video setup @ %d * %d, %f fps, %d kbit / s.\n", width, height, fps, vbr / 1024);
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
	{.kind = CODEC_VIDEO, .name = "libx264", .shortname = "H264", .id = 0,             .setup.video = setup_cb_x264},
	{.kind = CODEC_VIDEO, .name = "ffv1",    .shortname = "FFV1", .id = CODEC_ID_FFV1, .setup.video = default_vcodec_setup },
};

static struct codec_ent acodec_tbl[] = {
	{.kind = CODEC_AUDIO, .name = "libvorbis", .shortname = "VORBIS", .id = 0,             .setup.audio = default_acodec_setup},
	{.kind = CODEC_AUDIO, .name = "libmp3lame",.shortname = "MP3",    .id = 0,             .setup.audio = default_acodec_setup},
	{.kind = CODEC_AUDIO, .name = "flac",      .shortname = "FLAC",   .id = CODEC_ID_FLAC, .setup.audio = default_acodec_setup},
	{.kind = CODEC_AUDIO, .name = "pcm_s16le_planar", .shortname = "RAW",    .id = 0,      .setup.audio = default_acodec_setup}
};

static struct codec_ent fcodec_tbl[] = {
	{.kind = CODEC_FORMAT, .name = "matroska", .shortname = "MKV", .id = 0, .setup.muxer = default_format_setup },
	{.kind = CODEC_FORMAT, .name = "mpeg4",    .shortname = "MP4", .id = 0, .setup.muxer = default_format_setup },
	{.kind = CODEC_FORMAT, .name = "avi",      .shortname = "AVI", .id = 0, .setup.muxer = default_format_setup },
	{.kind = CODEC_FORMAT, .name = "flv",      .shortname = "FLV", .id = 0, .setup.muxer = default_format_setup }
};

static struct codec_ent lookup_default(const char* const req, struct codec_ent* tbl, size_t nmemb, bool audio)
{
	struct codec_ent res = {.name = req};
	AVCodec** dst = audio ? &res.storage.audio.codec : &res.storage.video.codec;
	
	if (req){
/* make sure that if the user supplies a name already in the standard table, that we get the same
 * prefix setup function */
		for (int i = 0; i < nmemb; i++)
			if (tbl[i].name != NULL && (strcmp(req, tbl[i].name) == 0 || strcmp(req, tbl[i].shortname) == 0) ){
				memcpy(&res, &tbl[i], sizeof(struct codec_ent));
				*dst = avcodec_find_encoder_by_name(res.name);
			}

/* if the codec specified is unknown (to us) then let avcodec try and sort it up, return default setup */
		if (*dst == NULL){
			*dst = avcodec_find_encoder_by_name(req);
		}
	}

/* if the user didn't supply an explicit codec, or one was not found, search the table for reasonable default */
	for (int i = 0; i < nmemb && *dst == NULL; i++)
		if (tbl[i].name != NULL && tbl[i].id == 0){
			memcpy(&res, &tbl[i], sizeof(struct codec_ent));
			*dst = avcodec_find_encoder_by_name(tbl[i].name);
		}
		else{
			memcpy(&res, &tbl[i], sizeof(struct codec_ent));
			*dst = avcodec_find_encoder(tbl[i].id);
		}

	return res;
}

struct codec_ent encode_getvcodec(const char* const req, int flags)
{
 	struct codec_ent a = lookup_default(req, vcodec_tbl, sizeof(vcodec_tbl) / sizeof(vcodec_tbl[0]), false);
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
	struct codec_ent res = lookup_default(req, acodec_tbl, sizeof(acodec_tbl) / sizeof(acodec_tbl[0]), true);

	if (res.storage.audio.codec && !res.setup.audio)
		res.setup.audio = default_acodec_setup;

	if (!res.storage.audio.codec)
		return res;

	res.storage.audio.context = avcodec_alloc_context3( res.storage.audio.codec );
	if ( (flags & AVFMT_GLOBALHEADER) > 0){
		res.storage.audio.context->flags |= CODEC_FLAG_GLOBAL_HEADER;
	}

	return res;
}

/* slightly difference scanning function here so can't re-use lookup_default */
struct codec_ent encode_getcontainer(const char* const requested, int dst, const char* remote)
{
	char fdbuf[16];
	AVFormatContext* ctx;
	struct codec_ent res = {0};

	if (requested && strcmp(requested, "stream") == 0){
		res.storage.container.format = av_guess_format("flv", NULL, NULL);

		if (!res.storage.container.format)
			LOG("(encode) -- couldn't setup streaming output.\n");
		else {
			ctx = avformat_alloc_context();
			ctx->oformat = res.storage.container.format;
			res.storage.container.context = ctx;
			res.setup.muxer = default_format_setup;
			int rv = avio_open2(&ctx->pb, remote, AVIO_FLAG_WRITE, NULL, NULL);
			LOG("(encode) attempting to open: %s, result: %d\n", remote, rv);
		}
		
		return res;
	}

	if (requested)
		res.storage.container.format = av_guess_format(requested, NULL, NULL);

	if (!res.storage.container.format)
		res.storage.container.format = av_guess_format("matroska", NULL, NULL);

/* no stream, nothing requested that matched and default didn't work. Give up and cascade. */
	if (!res.storage.container.format){
		LOG("(encode) -- couldn't find a suitable container.\n");
		return res;
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
