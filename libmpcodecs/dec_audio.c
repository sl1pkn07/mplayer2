/*
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#include "config.h"
#include "mp_msg.h"
#include "bstr.h"

#include "stream/stream.h"
#include "libmpdemux/demuxer.h"

#include "codec-cfg.h"
#include "libmpdemux/stheader.h"

#include "dec_audio.h"
#include "ad.h"
#include "libaf/af_format.h"

#include "libaf/af.h"

int fakemono = 0;

af_cfg_t af_cfg = { 1, NULL };	// Configuration for audio filters

void afm_help(void)
{
    int i;
    mp_tmsg(MSGT_DECAUDIO, MSGL_INFO, "Available (compiled-in) audio codec families/drivers:\n");
    mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_AUDIO_DRIVERS\n");
    mp_msg(MSGT_DECAUDIO, MSGL_INFO, "    afm:    info:  (comment)\n");
    for (i = 0; mpcodecs_ad_drivers[i] != NULL; i++)
	if (mpcodecs_ad_drivers[i]->info->comment
	    && mpcodecs_ad_drivers[i]->info->comment[0])
	    mp_msg(MSGT_DECAUDIO, MSGL_INFO, "%9s  %s (%s)\n",
		   mpcodecs_ad_drivers[i]->info->short_name,
		   mpcodecs_ad_drivers[i]->info->name,
		   mpcodecs_ad_drivers[i]->info->comment);
	else
	    mp_msg(MSGT_DECAUDIO, MSGL_INFO, "%9s  %s\n",
		   mpcodecs_ad_drivers[i]->info->short_name,
		   mpcodecs_ad_drivers[i]->info->name);
}

static int init_audio_codec(sh_audio_t *sh_audio)
{
    assert(!sh_audio->initialized);
    resync_audio_stream(sh_audio);
    sh_audio->samplesize = 2;
    sh_audio->sample_format = AF_FORMAT_S16_NE;
    if ((af_cfg.force & AF_INIT_FORMAT_MASK) == AF_INIT_FLOAT) {
	int fmt = AF_FORMAT_FLOAT_NE;
	if (sh_audio->ad_driver->control(sh_audio, ADCTRL_QUERY_FORMAT,
					 &fmt) == CONTROL_TRUE) {
	    sh_audio->sample_format = fmt;
	    sh_audio->samplesize = 4;
	}
    }
    sh_audio->audio_out_minsize = 8192; // default, preinit() may change it
    if (!sh_audio->ad_driver->preinit(sh_audio)) {
	mp_tmsg(MSGT_DECAUDIO, MSGL_ERR, "ADecoder preinit failed :(\n");
	return 0;
    }

    /* allocate audio in buffer: */
    if (sh_audio->audio_in_minsize > 0) {
	sh_audio->a_in_buffer_size = sh_audio->audio_in_minsize;
	mp_tmsg(MSGT_DECAUDIO, MSGL_V, "dec_audio: Allocating %d bytes for input buffer.\n",
	       sh_audio->a_in_buffer_size);
	sh_audio->a_in_buffer = av_mallocz(sh_audio->a_in_buffer_size);
    }

    const int base_size = 65536;
    // At least 64 KiB plus rounding up to next decodable unit size
    sh_audio->a_buffer_size = base_size + sh_audio->audio_out_minsize;

    mp_tmsg(MSGT_DECAUDIO, MSGL_V, "dec_audio: Allocating %d + %d = %d bytes for output buffer.\n",
	   sh_audio->audio_out_minsize, base_size, sh_audio->a_buffer_size);

    sh_audio->a_buffer = av_mallocz(sh_audio->a_buffer_size);
    if (!sh_audio->a_buffer)
        abort();
    sh_audio->a_buffer_len = 0;

    if (!sh_audio->ad_driver->init(sh_audio)) {
	mp_tmsg(MSGT_DECAUDIO, MSGL_V, "ADecoder init failed :(\n");
	uninit_audio(sh_audio);	// free buffers
	return 0;
    }

    sh_audio->initialized = 1;

    if (!sh_audio->channels || !sh_audio->samplerate) {
	mp_tmsg(MSGT_DECAUDIO, MSGL_ERR, "Audio decoder did not specify "
                "audio format!\n");
	uninit_audio(sh_audio);	// free buffers
	return 0;
    }

    if (!sh_audio->o_bps)
	sh_audio->o_bps = sh_audio->channels * sh_audio->samplerate
	                  * sh_audio->samplesize;
    return 1;
}

static int init_audio(sh_audio_t *sh_audio, char *codecname, char *afm,
		      int status, stringset_t *selected)
{
    unsigned int orig_fourcc = sh_audio->wf ? sh_audio->wf->wFormatTag : 0;
    int force = 0;
    if (codecname && codecname[0] == '+') {
	codecname = &codecname[1];
	force = 1;
    }
    sh_audio->codec = NULL;
    while (1) {
	const ad_functions_t *mpadec;
	int i;
	sh_audio->ad_driver = 0;
	// restore original fourcc:
	if (sh_audio->wf)
	    sh_audio->wf->wFormatTag = i = orig_fourcc;
	if (!(sh_audio->codec = find_audio_codec(sh_audio->format,
						 sh_audio->wf ? (&i) : NULL,
						 sh_audio->codec, force)))
	    break;
	if (sh_audio->wf)
	    sh_audio->wf->wFormatTag = i;
	// ok we found one codec
	if (stringset_test(selected, sh_audio->codec->name))
	    continue;	// already tried & failed
	if (codecname && strcmp(sh_audio->codec->name, codecname))
	    continue;	// -ac
	if (afm && strcmp(sh_audio->codec->drv, afm))
	    continue;	// afm doesn't match
	if (!force && sh_audio->codec->status < status)
	    continue;	// too unstable
	stringset_add(selected, sh_audio->codec->name);	// tagging it
	// ok, it matches all rules, let's find the driver!
	for (i = 0; mpcodecs_ad_drivers[i] != NULL; i++)
	    if (!strcmp(mpcodecs_ad_drivers[i]->info->short_name,
		 sh_audio->codec->drv))
		break;
	mpadec = mpcodecs_ad_drivers[i];
	if (!mpadec) {		// driver not available (==compiled in)
	    mp_tmsg(MSGT_DECAUDIO, MSGL_ERR,
		   "Requested audio codec family [%s] (afm=%s) not available.\nEnable it at compilation.\n",
		   sh_audio->codec->name, sh_audio->codec->drv);
	    continue;
	}
	// it's available, let's try to init!
	// init()
	mp_tmsg(MSGT_DECAUDIO, MSGL_V, "Opening audio decoder: [%s] %s\n",
	       mpadec->info->short_name, mpadec->info->name);
	sh_audio->ad_driver = mpadec;
	if (!init_audio_codec(sh_audio)) {
            mp_tmsg(MSGT_DECAUDIO, MSGL_WARN, "Audio decoder init failed for "
                    "codecs.conf entry \"%s\".\n", sh_audio->codec->name);
	    continue;		// try next...
	}
	// Yeah! We got it!
	return 1;
    }
    return 0;
}

int init_best_audio_codec(sh_audio_t *sh_audio, char **audio_codec_list,
			  char **audio_fm_list)
{
    stringset_t selected;
    char *ac_l_default[2] = { "", (char *) NULL };
    // hack:
    if (!audio_codec_list)
	audio_codec_list = ac_l_default;
    // Go through the codec.conf and find the best codec...
    sh_audio->initialized = 0;
    stringset_init(&selected);
    while (!sh_audio->initialized && *audio_codec_list) {
	char *audio_codec = *(audio_codec_list++);
	if (audio_codec[0]) {
	    if (audio_codec[0] == '-') {
		// disable this codec:
		stringset_add(&selected, audio_codec + 1);
	    } else {
		// forced codec by name:
		mp_tmsg(MSGT_DECAUDIO, MSGL_INFO, "Forced audio codec: %s\n",
		       audio_codec);
		init_audio(sh_audio, audio_codec, NULL, -1, &selected);
	    }
	} else {
	    int status;
	    // try in stability order: UNTESTED, WORKING, BUGGY.
	    // never try CRASHING.
	    if (audio_fm_list) {
		char **fmlist = audio_fm_list;
		// try first the preferred codec families:
		while (!sh_audio->initialized && *fmlist) {
		    char *audio_fm = *(fmlist++);
		    mp_tmsg(MSGT_DECAUDIO, MSGL_INFO, "Trying to force audio codec driver family %s...\n",
			   audio_fm);
		    for (status = CODECS_STATUS__MAX;
			 status >= CODECS_STATUS__MIN; --status)
			if (init_audio(sh_audio, NULL, audio_fm, status, &selected))
			    break;
		}
	    }
	    if (!sh_audio->initialized)
		for (status = CODECS_STATUS__MAX; status >= CODECS_STATUS__MIN;
		     --status)
		    if (init_audio(sh_audio, NULL, NULL, status, &selected))
			break;
	}
    }
    stringset_free(&selected);

    if (!sh_audio->initialized) {
	mp_tmsg(MSGT_DECAUDIO, MSGL_ERR, "Cannot find codec for audio format 0x%X.\n",
	       sh_audio->format);
	return 0;   // failed
    }

    mp_tmsg(MSGT_DECAUDIO, MSGL_INFO, "Selected audio codec: %s [%s]\n",
            sh_audio->codecname ? sh_audio->codecname : sh_audio->codec->info,
            sh_audio->ad_driver->info->print_name ?
            sh_audio->ad_driver->info->print_name :
            sh_audio->ad_driver->info->short_name);
    mp_tmsg(MSGT_DECAUDIO, MSGL_V,
            "Audio codecs.conf entry: %s (%s)  afm: %s\n",
	   sh_audio->codec->name, sh_audio->codec->info, sh_audio->codec->drv);
    mp_msg(MSGT_DECAUDIO, MSGL_INFO,
	   "AUDIO: %d Hz, %d ch, %s, %3.1f kbit/%3.2f%% (ratio: %d->%d)\n",
	   sh_audio->samplerate, sh_audio->channels,
	   af_fmt2str_short(sh_audio->sample_format),
	   sh_audio->i_bps * 8 * 0.001,
	   ((float) sh_audio->i_bps / sh_audio->o_bps) * 100.0,
	   sh_audio->i_bps, sh_audio->o_bps);
    mp_msg(MSGT_IDENTIFY, MSGL_INFO,
	   "ID_AUDIO_BITRATE=%d\nID_AUDIO_RATE=%d\n" "ID_AUDIO_NCH=%d\n",
	   sh_audio->i_bps * 8, sh_audio->samplerate, sh_audio->channels);

    return 1;   // success
}

void uninit_audio(sh_audio_t *sh_audio)
{
    if (sh_audio->afilter) {
	mp_msg(MSGT_DECAUDIO, MSGL_V, "Uninit audio filters...\n");
	af_uninit(sh_audio->afilter);
	free(sh_audio->afilter);
	sh_audio->afilter = NULL;
    }
    if (sh_audio->initialized) {
	mp_tmsg(MSGT_DECAUDIO, MSGL_V, "Uninit audio: %s\n",
	       sh_audio->codec->drv);
	sh_audio->ad_driver->uninit(sh_audio);
	sh_audio->initialized = 0;
    }
    av_freep(&sh_audio->a_buffer);
    av_freep(&sh_audio->a_in_buffer);
}


int init_audio_filters(sh_audio_t *sh_audio, int in_samplerate,
		       int *out_samplerate, int *out_channels, int *out_format)
{
    af_stream_t *afs = sh_audio->afilter;
    if (!afs) {
	afs = calloc(1, sizeof(struct af_stream));
	afs->opts = sh_audio->opts;
    }
    // input format: same as codec's output format:
    afs->input.rate   = in_samplerate;
    afs->input.nch    = sh_audio->channels;
    afs->input.format = sh_audio->sample_format;
    af_fix_parameters(&(afs->input));

    // output format: same as ao driver's input format (if missing, fallback to input)
    afs->output.rate   = *out_samplerate;
    afs->output.nch    = *out_channels;
    afs->output.format = *out_format;
    af_fix_parameters(&(afs->output));

    // filter config:
    memcpy(&afs->cfg, &af_cfg, sizeof(af_cfg_t));

    mp_tmsg(MSGT_DECAUDIO, MSGL_V, "Building audio filter chain for %dHz/%dch/%s -> %dHz/%dch/%s...\n",
	   afs->input.rate, afs->input.nch,
	   af_fmt2str_short(afs->input.format), afs->output.rate,
	   afs->output.nch, af_fmt2str_short(afs->output.format));

    // let's autoprobe it!
    if (0 != af_init(afs)) {
	sh_audio->afilter = NULL;
	free(afs);
	return 0;   // failed :(
    }

    *out_samplerate = afs->output.rate;
    *out_channels = afs->output.nch;
    *out_format = afs->output.format;

    // ok!
    sh_audio->afilter = (void *) afs;
    return 1;
}

static void set_min_out_buffer_size(struct bstr *outbuf, int len)
{
    size_t oldlen = talloc_get_size(outbuf->start);
    if (oldlen < len) {
        assert(outbuf->start);  // talloc context should be already set
	mp_msg(MSGT_DECAUDIO, MSGL_V, "Increasing filtered audio buffer size "
	       "from %zd to %d\n", oldlen, len);
        outbuf->start = talloc_realloc_size(NULL, outbuf->start, len);
    }
}

static int filter_n_bytes(sh_audio_t *sh, struct bstr *outbuf, int len)
{
    assert(len-1 + sh->audio_out_minsize <= sh->a_buffer_size);

    int error = 0;

    // Decode more bytes if needed
    int old_samplerate = sh->samplerate;
    int old_channels = sh->channels;
    int old_sample_format = sh->sample_format;
    while (sh->a_buffer_len < len) {
	unsigned char *buf = sh->a_buffer + sh->a_buffer_len;
	int minlen = len - sh->a_buffer_len;
	int maxlen = sh->a_buffer_size - sh->a_buffer_len;
	int ret = sh->ad_driver->decode_audio(sh, buf, minlen, maxlen);
	int format_change = sh->samplerate != old_samplerate
                            || sh->channels != old_channels
                            || sh->sample_format != old_sample_format;
	if (ret <= 0 || format_change) {
	    error = format_change ? -2 : -1;
            // samples from format-changing call get discarded too
	    len = sh->a_buffer_len;
	    break;
	}
	sh->a_buffer_len += ret;
    }

    // Filter
    af_data_t filter_input = {
	.audio = sh->a_buffer,
	.len = len,
	.rate = sh->samplerate,
	.nch = sh->channels,
	.format = sh->sample_format
    };
    af_fix_parameters(&filter_input);
    af_data_t *filter_output = af_play(sh->afilter, &filter_input);
    if (!filter_output)
	return -1;
    set_min_out_buffer_size(outbuf, outbuf->len + filter_output->len);
    memcpy(outbuf->start + outbuf->len, filter_output->audio,
           filter_output->len);
    outbuf->len += filter_output->len;

    // remove processed data from decoder buffer:
    sh->a_buffer_len -= len;
    memmove(sh->a_buffer, sh->a_buffer + len, sh->a_buffer_len);

    return error;
}

/* Try to get at least minlen decoded+filtered bytes in outbuf
 * (total length including possible existing data).
 * Return 0 on success, -1 on error/EOF (not distinguished).
 * In the former case outbuf->len is always >= minlen on return.
 * In case of EOF/error it might or might not be.
 * Outbuf.start must be talloc-allocated, and will be reallocated
 * if needed to fit all filter output. */
int decode_audio(sh_audio_t *sh_audio, struct bstr *outbuf, int minlen)
{
    // Indicates that a filter seems to be buffering large amounts of data
    int huge_filter_buffer = 0;
    // Decoded audio must be cut at boundaries of this many bytes
    int unitsize = sh_audio->channels * sh_audio->samplesize * 16;

    /* Filter output size will be about filter_multiplier times input size.
     * If some filter buffers audio in big blocks this might only hold
     * as average over time. */
    double filter_multiplier = af_calc_filter_multiplier(sh_audio->afilter);

    /* If the decoder set audio_out_minsize then it can do the equivalent of
     * "while (output_len < target_len) output_len += audio_out_minsize;",
     * so we must guarantee there is at least audio_out_minsize-1 bytes
     * more space in the output buffer than the minimum length we try to
     * decode. */
    int max_decode_len = sh_audio->a_buffer_size - sh_audio->audio_out_minsize;
    max_decode_len -= max_decode_len % unitsize;

    while (outbuf->len < minlen) {
	int declen = (minlen - outbuf->len) / filter_multiplier
	    + (unitsize << 5); // some extra for possible filter buffering
	if (huge_filter_buffer)
	/* Some filter must be doing significant buffering if the estimated
	 * input length didn't produce enough output from filters.
	 * Feed the filters 2k bytes at a time until we have enough output.
	 * Very small amounts could make filtering inefficient while large
	 * amounts can make MPlayer demux the file unnecessarily far ahead
	 * to get audio data and buffer video frames in memory while doing
	 * so. However the performance impact of either is probably not too
	 * significant as long as the value is not completely insane. */
	    declen = 2000;
	declen -= declen % unitsize;
	if (declen > max_decode_len)
	    declen = max_decode_len;
	else
	    /* if this iteration does not fill buffer, we must have lots
	     * of buffering in filters */
	    huge_filter_buffer = 1;
	int res = filter_n_bytes(sh_audio, outbuf, declen);
	if (res < 0)
	    return res;
    }
    return 0;
}

void decode_audio_prepend_bytes(struct bstr *outbuf, int count, int byte)
{
    set_min_out_buffer_size(outbuf, outbuf->len + count);
    memmove(outbuf->start + count, outbuf->start, outbuf->len);
    memset(outbuf->start, byte, count);
    outbuf->len += count;
}


void resync_audio_stream(sh_audio_t *sh_audio)
{
    sh_audio->a_in_buffer_len = 0;	// clear audio input buffer
    sh_audio->pts = MP_NOPTS_VALUE;
    if (!sh_audio->initialized)
	return;
    sh_audio->ad_driver->control(sh_audio, ADCTRL_RESYNC_STREAM, NULL);
}

void skip_audio_frame(sh_audio_t *sh_audio)
{
    if (!sh_audio->initialized)
	return;
    if (sh_audio->ad_driver->control(sh_audio, ADCTRL_SKIP_FRAME, NULL) ==
	CONTROL_TRUE)
	return;
    // default skip code:
    ds_fill_buffer(sh_audio->ds);	// skip block
}
