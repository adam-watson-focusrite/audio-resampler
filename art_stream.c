////////////////////////////////////////////////////////////////////////////
//                            **** ART ****                               //
//                        Audio Resampling Tool                           //
//                 Copyright (c) 2006-2023 David Bryant                   //
//                         All Rights Reserved                            //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>

#include "resampler.h"
#include "biquad.h"

#include "art_stream.h"

extern process_context_t process_context;

// Return a tpdf random value in the range: -1.0 <= n < 1.0
// type: -1: negative intersample correlation (HF boost)
//        0: no correlation (independent samples, flat spectrum)
//        1: positive intersample correlation (LF boost)
// Note: not thread-safe on the same channel

static uint32_t *tpdf_generators;

static void tpdf_dither_init (int num_channels)
{
    int generator_bytes = num_channels * sizeof (uint32_t);
    unsigned char *seed = malloc (generator_bytes);
    uint32_t random = 0x31415926;

    tpdf_generators = (uint32_t *) seed;

    while (generator_bytes--) {
        *seed++ = random >> 24;
        random = ((random << 4) - random) ^ 1;
        random = ((random << 4) - random) ^ 1;
        random = ((random << 4) - random) ^ 1;
    }
}

static inline double tpdf_dither (int channel, int type)
{
    uint32_t random = tpdf_generators [channel];
    random = ((random << 4) - random) ^ 1;
    random = ((random << 4) - random) ^ 1;
    uint32_t first = type ? tpdf_generators [channel] ^ ((int32_t) type >> 31) : ~random;
    random = ((random << 4) - random) ^ 1;
    random = ((random << 4) - random) ^ 1;
    random = ((random << 4) - random) ^ 1;
    tpdf_generators [channel] = random;
    return (((first >> 1) + (random >> 1)) / 2147483648.0) - 1.0;
}

static void tpdf_dither_free (void)
{
    free (tpdf_generators);
}

static size_t fread_stream(void * buffer, size_t size, size_t count)
{
	return fread(buffer,size,count,process_context.in_stream);
}

static size_t fwrite_stream(void * buffer, size_t size, size_t count)
{
	return fwrite(buffer,size,count,process_context.out_stream);
}

uint16_t art_resample_init()
{
	process_context.interpolate = 1;

	process_context.verbosity=1;

	process_context.BUFFER_SAMPLES = 441;

	process_context.num_channels=ART_STREAM_NUM_CHANNELS;
	process_context.outbits=16;
	process_context.inbits=16;


	process_context.num_taps=4;
	process_context.num_filters=2;
	process_context.gain=1.0;

	process_context.sample_ratio = (double) process_context.resample_rate / (double)process_context.sample_rate;
	process_context.lowpass_ratio = 1.0;

	process_context.outbuffer_samples = (int) floor (process_context.BUFFER_SAMPLES * process_context.sample_ratio * 1.1 + 100.0);
	process_context.remaining_samples = process_context.num_samples;
	process_context.output_samples = 0;
#ifdef ART_STREAM_CLIP_CHECK
	process_context.clipped_samples = 0;
#endif

	process_context.outbuffer = malloc (process_context.outbuffer_samples * process_context.num_channels * sizeof (float));
	process_context.inbuffer = malloc (process_context.BUFFER_SAMPLES * process_context.num_channels * sizeof (float));

	process_context.flags = process_context.interpolate ? SUBSAMPLE_INTERPOLATE : 0;
    process_context.samples_to_append = process_context.num_taps / 2;

    process_context.pre_filter = 0;
    process_context.post_filter = 0;

    process_context.readbuffer = process_context.inbuffer;

    if (process_context.sample_ratio < 1.0) {
    	process_context.lowpass_ratio -= (10.24 / process_context.num_taps);

        if (process_context.lowpass_ratio < 0.84)           // limit the lowpass for very short filters
        	process_context.lowpass_ratio = 0.84;

        if (process_context.lowpass_ratio < process_context.sample_ratio)   // avoid discontinuities near unity sample ratios
        	process_context.lowpass_ratio = process_context.sample_ratio;
    }

    fprintf (stderr, "sample_ratio: %0.6f, (resample_rate %d / sample_rate %d)\n",process_context.sample_ratio, process_context.resample_rate, process_context.sample_rate);

    if (process_context.lowpass_freq) {
        double user_lowpass_ratio;

        if (process_context.sample_ratio < 1.0)
            user_lowpass_ratio = process_context.lowpass_freq / (process_context.resample_rate / 2.0);
        else
            user_lowpass_ratio = process_context.lowpass_freq / (process_context.sample_rate / 2.0);

        if (user_lowpass_ratio >= 1.0)
            fprintf (stderr, "warning: ignoring invalid lowpass frequency specification (at or over Nyquist)\n");
        else
        	process_context.lowpass_ratio = user_lowpass_ratio;
    }

    if (process_context.bh4_window || !process_context.hann_window)
    	process_context.flags |= BLACKMAN_HARRIS;

    if (process_context.lowpass_ratio * process_context.sample_ratio < 0.98 && process_context.pre_post_filter) {
        double cutoff = process_context.lowpass_ratio * process_context.sample_ratio / 2.0;
        biquad_lowpass (&process_context.lowpass_coeff, cutoff);
        process_context.pre_filter = 1;

        if (process_context.verbosity > 0)
            fprintf (stderr, "cascaded biquad pre-filter at %g Hz\n", process_context.sample_rate * cutoff);
    }

    if (process_context.sample_ratio < 1.0) {
    	process_context.resampler = resampleInit (process_context.num_channels, process_context.num_taps, process_context.num_filters, process_context.sample_ratio * process_context.lowpass_ratio, process_context.flags | INCLUDE_LOWPASS);

        if (process_context.verbosity > 0)
            fprintf (stderr, "%d-tap sinc downsampler with lowpass at %g Hz\n", process_context.num_taps, process_context.sample_ratio * process_context.lowpass_ratio * process_context.sample_rate / 2.0);
    }
    else if (process_context.lowpass_ratio < 1.0) {
    	process_context.resampler = resampleInit (process_context.num_channels, process_context.num_taps, process_context.num_filters, process_context.lowpass_ratio, process_context.flags | INCLUDE_LOWPASS);

        if (process_context.verbosity > 0)
            fprintf (stderr, "%d-tap sinc resampler with lowpass at %g Hz\n", process_context.num_taps, process_context.lowpass_ratio * process_context.sample_rate / 2.0);
    }
    else {
    	process_context.resampler = resampleInit (process_context.num_channels, process_context.num_taps, process_context.num_filters, 1.0, process_context.flags);

        if (process_context.verbosity > 0)
            fprintf (stderr, "%d-tap pure sinc resampler (no lowpass), %g Hz Nyquist\n", process_context.num_taps, process_context.sample_rate / 2.0);
    }

    if (process_context.lowpass_ratio / process_context.sample_ratio < 0.98 && process_context.pre_post_filter && !process_context.pre_filter) {
        double cutoff = process_context.lowpass_ratio / process_context.sample_ratio / 2.0;
        biquad_lowpass (&process_context.lowpass_coeff, cutoff);
        process_context.post_filter = 1;

        if (process_context.verbosity > 0)
            fprintf (stderr, "cascaded biquad post-filter at %g Hz\n", process_context.resample_rate * cutoff);
    }

    if (process_context.pre_filter || process_context.post_filter)
        for (int i = 0; i < process_context.num_channels; ++i) {
            biquad_init (&process_context.lowpass [i] [0], &process_context.lowpass_coeff, 1.0);
            biquad_init (&process_context.lowpass [i] [1], &process_context.lowpass_coeff, 1.0);
        }

    if (process_context.outbits != 32) {
        memset (process_context.error, 0, sizeof (process_context.error));
        tpdf_dither_init (process_context.num_channels);
    }

    if (process_context.inbits != 32 || process_context.outbits != 32) {
        int max_samples = process_context.BUFFER_SAMPLES, max_bytes = 2;

        if (process_context.outbuffer_samples > process_context.BUFFER_SAMPLES)
            max_samples = process_context.outbuffer_samples;

        if (process_context.inbits > 16 || process_context.outbits > 16)
            max_bytes = 3;

        process_context.tmpbuffer = malloc (max_samples * process_context.num_channels * max_bytes);

        if (process_context.inbits != 32)
        	process_context.readbuffer = process_context.tmpbuffer;
    }

    // this takes care of the filter delay and any user-specified phase shift
    resampleAdvancePosition (process_context.resampler, process_context.num_taps / 2.0 + process_context.phase_shift);

    return 0;
}

uint16_t art_resample_deinit()
{
    resampleFree (process_context.resampler);
    tpdf_dither_free ();
    free (process_context.inbuffer);
    free (process_context.outbuffer);
    free (process_context.tmpbuffer);

#ifdef ART_STREAM_CLIP_CHECK
    if (process_context.clipped_samples)
        fprintf (stderr, "warning: %u samples were clipped, suggest reducing gain!\n", process_context.clipped_samples);
#endif

    if (process_context.remaining_samples)
        fprintf (stderr, "warning: file terminated early!\n");

    return process_context.output_samples;
}

uint16_t art_resample_process_block (uint32_t stream_samples_read)
{
    ResampleResult res;
	if (process_context.inbits <= 8) {
		float gain_factor = process_context.gain / 128.0;
		int i;

		for (i = 0; i < stream_samples_read * process_context.num_channels; ++i)
			process_context.inbuffer [i] = ((int) process_context.tmpbuffer [i] - 128) * gain_factor;
	}
	else if (process_context.inbits <= 16) {
		float gain_factor = process_context.gain / 32768.0;
		int i, j;

		for (i = j = 0; i < stream_samples_read * process_context.num_channels; ++i) {
			int16_t value = process_context.tmpbuffer [j++];
			value += process_context.tmpbuffer [j++] << 8;
			process_context.inbuffer [i] = value * gain_factor;
		}
	}
	else if (process_context.inbits <= 24) {
		float gain_factor = process_context.gain / 8388608.0;
		int i, j;

		for (i = j = 0; i < stream_samples_read * process_context.num_channels; ++i) {
			int32_t value = process_context.tmpbuffer [j++];
			value += process_context.tmpbuffer [j++] << 8;
			value += (int32_t) (signed char) process_context.tmpbuffer [j++] << 16;
			process_context.inbuffer [i] = value * gain_factor;
		}
	}
	else {
		if (IS_BIG_ENDIAN) {
			unsigned char *bptr = (unsigned char *) process_context.inbuffer, word [4];
			int wcount = stream_samples_read * process_context.num_channels;

			while (wcount--) {
				memcpy (word, bptr, 4);
				*bptr++ = word [3];
				*bptr++ = word [2];
				*bptr++ = word [1];
				*bptr++ = word [0];
			}
		}

		if (process_context.gain != 1.0)
		{
			for (int i = 0; i < stream_samples_read * process_context.num_channels; ++i)
				process_context.inbuffer [i] *= process_context.gain;
		}
	}

	// common code to process the audio in 32-bit floats

	if (process_context.pre_filter)
		for (int i = 0; i < process_context.num_channels; ++i) {
			biquad_apply_buffer (&process_context.lowpass [i] [0], process_context.inbuffer + i, stream_samples_read, process_context.num_channels);
			biquad_apply_buffer (&process_context.lowpass [i] [1], process_context.inbuffer + i, stream_samples_read, process_context.num_channels);
		}

	res = resampleProcessInterleaved (process_context.resampler, process_context.inbuffer, stream_samples_read, process_context.outbuffer, process_context.outbuffer_samples, process_context.sample_ratio);
	uint32_t samples_generated = res.output_generated;

	if (process_context.post_filter)
		for (int i = 0; i < process_context.num_channels; ++i) {
			biquad_apply_buffer (&process_context.lowpass [i] [0], process_context.outbuffer + i, samples_generated, process_context.num_channels);
			biquad_apply_buffer (&process_context.lowpass [i] [1], process_context.outbuffer + i, samples_generated, process_context.num_channels);
		}

	// finally write the audio, converting to appropriate integer format if requested

	if (process_context.outbits != 32) {
		float scaler = (1 << process_context.outbits) / 2.0;
		int32_t offset = (process_context.outbits <= 8) * 128;
#ifdef ART_STREAM_CLIP_CHECK
		int32_t highclip = (1 << (process_context.outbits - 1)) - 1;
		int32_t lowclip = ~highclip;
#endif
		int leftshift = (24 - process_context.outbits) % 8;
		int i, j;

		for (i = j = 0; i < samples_generated * process_context.num_channels; ++i) {
			int chan = i % process_context.num_channels;
			int32_t output = floor ((process_context.outbuffer [i] *= scaler) - process_context.error [chan] + tpdf_dither (chan, -1) + 0.5);

#ifdef ART_STREAM_CLIP_CHECK
			if (output > highclip)
			{
				process_context.clipped_samples++;
				output = highclip;
			}
			else if (output < lowclip)
			{
				process_context.clipped_samples++;
				output = lowclip;
			}
#endif

			process_context.error [chan] += output - process_context.outbuffer [i];
			process_context.tmpbuffer [j++] = output = (output << leftshift) + offset;

			if (process_context.outbits > 8) {
				process_context.tmpbuffer [j++] = output >> 8;

				if (process_context.outbits > 16)
					process_context.tmpbuffer [j++] = output >> 16;
			}
		}

	}

	process_context.output_samples += samples_generated;

	return samples_generated;
}

uint16_t art_resample_process_audio()
{
	art_resample_init();

    uint32_t progress_divider = 0, percent;

    if (process_context.verbosity >= 0 && process_context.remaining_samples > 1000) {
        progress_divider = (process_context.remaining_samples + 50) / 100;
        fprintf (stderr, "\rprogress: %d%% ", percent = 0); fflush (stderr);
    }

    while (process_context.remaining_samples + process_context.samples_to_append)
	{
        // first we read the audio data, converting to 32-bit float (if not already) and applying gain
        unsigned long samples_to_read = process_context.remaining_samples, stream_samples_read;

        if (samples_to_read > process_context.BUFFER_SAMPLES)
            samples_to_read = process_context.BUFFER_SAMPLES;

        int stream_read_size = process_context.num_channels * ((process_context.inbits + 7) / 8);

        stream_samples_read = fread_stream(process_context.readbuffer, stream_read_size, samples_to_read);

        process_context.remaining_samples -= stream_samples_read;

        if (stream_samples_read==0)
        {
        	// END OF THE STREAM!!!!
            int samples_to_append_now = process_context.samples_to_append;

            if (!samples_to_append_now)
            {
                break;
            }

            if (samples_to_append_now > process_context.BUFFER_SAMPLES)
                samples_to_append_now = process_context.BUFFER_SAMPLES;

            memset (process_context.readbuffer, (process_context.inbits <= 8) * 128, samples_to_append_now * process_context.num_channels * ((process_context.inbits + 7) / 8));
            stream_samples_read = samples_to_append_now;
            process_context.samples_to_append -= samples_to_append_now;
        }

        uint32_t samples_generated = art_resample_process_block (stream_samples_read);

        if(samples_generated)
        {
    		int stream_write_size = process_context.num_channels * ((process_context.outbits + 7) / 8);
    		fwrite_stream (process_context.tmpbuffer, stream_write_size, samples_generated);
        }

        if (progress_divider) {
            int new_percent = 100 - process_context.remaining_samples / progress_divider;

            if (new_percent != percent) {
                fprintf (stderr, "\rprogress: %d%% ", percent = new_percent);
                fflush (stderr);
            }
        }

	}

	art_resample_deinit();

	return 0;
}
