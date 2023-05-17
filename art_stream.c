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

static int bh4_window;
static int hann_window;
static int verbosity;
static int interpolate = 1;
static int pre_post_filter;
const int num_channels=2;
const int outbits=16;
const int inbits=16;

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

unsigned int process_audio_init()
{
	process_context.num_taps=16;
	process_context.num_filters=16;
	process_context.gain=1.0;

	process_context.sample_ratio = (double) process_context.resample_rate / (double)process_context.sample_rate;
	process_context.lowpass_ratio = 1.0;

	process_context.outbuffer_samples = (int) floor (BUFFER_SAMPLES * process_context.sample_ratio * 1.1 + 100.0);
	process_context.remaining_samples = process_context.num_samples, process_context.output_samples = 0, process_context.clipped_samples = 0;

	process_context.outbuffer = malloc (process_context.outbuffer_samples * num_channels * sizeof (float));
	process_context.inbuffer = malloc (BUFFER_SAMPLES * num_channels * sizeof (float));

	process_context.flags = interpolate ? SUBSAMPLE_INTERPOLATE : 0;
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

    if (bh4_window || !hann_window)
    	process_context.flags |= BLACKMAN_HARRIS;

    if (process_context.lowpass_ratio * process_context.sample_ratio < 0.98 && pre_post_filter) {
        double cutoff = process_context.lowpass_ratio * process_context.sample_ratio / 2.0;
        biquad_lowpass (&process_context.lowpass_coeff, cutoff);
        process_context.pre_filter = 1;

        if (verbosity > 0)
            fprintf (stderr, "cascaded biquad pre-filter at %g Hz\n", process_context.sample_rate * cutoff);
    }

    if (process_context.sample_ratio < 1.0) {
    	process_context.resampler = resampleInit (num_channels, process_context.num_taps, process_context.num_filters, process_context.sample_ratio * process_context.lowpass_ratio, process_context.flags | INCLUDE_LOWPASS);

        if (verbosity > 0)
            fprintf (stderr, "%d-tap sinc downsampler with lowpass at %g Hz\n", process_context.num_taps, process_context.sample_ratio * process_context.lowpass_ratio * process_context.sample_rate / 2.0);
    }
    else if (process_context.lowpass_ratio < 1.0) {
    	process_context.resampler = resampleInit (num_channels, process_context.num_taps, process_context.num_filters, process_context.lowpass_ratio, process_context.flags | INCLUDE_LOWPASS);

        if (verbosity > 0)
            fprintf (stderr, "%d-tap sinc resampler with lowpass at %g Hz\n", process_context.num_taps, process_context.lowpass_ratio * process_context.sample_rate / 2.0);
    }
    else {
    	process_context.resampler = resampleInit (num_channels, process_context.num_taps, process_context.num_filters, 1.0, process_context.flags);

        if (verbosity > 0)
            fprintf (stderr, "%d-tap pure sinc resampler (no lowpass), %g Hz Nyquist\n", process_context.num_taps, process_context.sample_rate / 2.0);
    }

    if (process_context.lowpass_ratio / process_context.sample_ratio < 0.98 && pre_post_filter && !process_context.pre_filter) {
        double cutoff = process_context.lowpass_ratio / process_context.sample_ratio / 2.0;
        biquad_lowpass (&process_context.lowpass_coeff, cutoff);
        process_context.post_filter = 1;

        if (verbosity > 0)
            fprintf (stderr, "cascaded biquad post-filter at %g Hz\n", process_context.resample_rate * cutoff);
    }

    if (process_context.pre_filter || process_context.post_filter)
        for (int i = 0; i < num_channels; ++i) {
            biquad_init (&process_context.lowpass [i] [0], &process_context.lowpass_coeff, 1.0);
            biquad_init (&process_context.lowpass [i] [1], &process_context.lowpass_coeff, 1.0);
        }

    if (outbits != 32) {
        memset (process_context.error, 0, sizeof (process_context.error));
        tpdf_dither_init (num_channels);
    }

    if (inbits != 32 || outbits != 32) {
        int max_samples = BUFFER_SAMPLES, max_bytes = 2;

        if (process_context.outbuffer_samples > BUFFER_SAMPLES)
            max_samples = process_context.outbuffer_samples;

        if (inbits > 16 || outbits > 16)
            max_bytes = 3;

        process_context.tmpbuffer = malloc (max_samples * num_channels * max_bytes);

        if (inbits != 32)
        	process_context.readbuffer = process_context.tmpbuffer;
    }

    // this takes care of the filter delay and any user-specified phase shift
    resampleAdvancePosition (process_context.resampler, process_context.num_taps / 2.0 + process_context.phase_shift);

    return 0;
}

unsigned int process_audio (unsigned long num_samples)
{
    // when downsampling, calculate the optimum lowpass based on resample filter
    // length (i.e., more taps allow us to lowpass closer to Nyquist)


    uint32_t progress_divider = 0, percent;

    if (verbosity >= 0 && process_context.remaining_samples > 1000) {
        progress_divider = (process_context.remaining_samples + 50) / 100;
        fprintf (stderr, "\rprogress: %d%% ", percent = 0); fflush (stderr);
    }

    while (process_context.remaining_samples + process_context.samples_to_append) {

        // first we read the audio data, converting to 32-bit float (if not already) and applying gain

        unsigned long samples_to_read = process_context.remaining_samples, stream_samples_read, samples_generated;
        ResampleResult res;

        if (samples_to_read > BUFFER_SAMPLES)
            samples_to_read = BUFFER_SAMPLES;

        int stream_read_size = num_channels * ((inbits + 7) / 8);

        stream_samples_read = fread_stream(process_context.readbuffer, stream_read_size, samples_to_read);

        process_context.remaining_samples -= stream_samples_read;

        if (!stream_samples_read) {
            int samples_to_append_now = process_context.samples_to_append;

            if (!samples_to_append_now)
                break;

            if (samples_to_append_now > BUFFER_SAMPLES)
                samples_to_append_now = BUFFER_SAMPLES;

            memset (process_context.readbuffer, (inbits <= 8) * 128, samples_to_append_now * num_channels * ((inbits + 7) / 8));
            stream_samples_read = samples_to_append_now;
            process_context.samples_to_append -= samples_to_append_now;
        }

        if (inbits <= 8) {
            float gain_factor = process_context.gain / 128.0;
            int i;

            for (i = 0; i < stream_samples_read * num_channels; ++i)
            	process_context.inbuffer [i] = ((int) process_context.tmpbuffer [i] - 128) * gain_factor;
        }
        else if (inbits <= 16) {
            float gain_factor = process_context.gain / 32768.0;
            int i, j;

            for (i = j = 0; i < stream_samples_read * num_channels; ++i) {
                int16_t value = process_context.tmpbuffer [j++];
                value += process_context.tmpbuffer [j++] << 8;
                process_context.inbuffer [i] = value * gain_factor;
            }
        }
        else if (inbits <= 24) {
            float gain_factor = process_context.gain / 8388608.0;
            int i, j;

            for (i = j = 0; i < stream_samples_read * num_channels; ++i) {
                int32_t value = process_context.tmpbuffer [j++];
                value += process_context.tmpbuffer [j++] << 8;
                value += (int32_t) (signed char) process_context.tmpbuffer [j++] << 16;
                process_context.inbuffer [i] = value * gain_factor;
            }
        }
        else {
            if (IS_BIG_ENDIAN) {
                unsigned char *bptr = (unsigned char *) process_context.inbuffer, word [4];
                int wcount = stream_samples_read * num_channels;

                while (wcount--) {
                    memcpy (word, bptr, 4);
                    *bptr++ = word [3];
                    *bptr++ = word [2];
                    *bptr++ = word [1];
                    *bptr++ = word [0];
                }
            }

            if (process_context.gain != 1.0)
                for (int i = 0; i < stream_samples_read * num_channels; ++i)
                	process_context.inbuffer [i] *= process_context.gain;
        }

        // common code to process the audio in 32-bit floats

        if (process_context.pre_filter)
            for (int i = 0; i < num_channels; ++i) {
                biquad_apply_buffer (&process_context.lowpass [i] [0], process_context.inbuffer + i, stream_samples_read, num_channels);
                biquad_apply_buffer (&process_context.lowpass [i] [1], process_context.inbuffer + i, stream_samples_read, num_channels);
            }

        res = resampleProcessInterleaved (process_context.resampler, process_context.inbuffer, stream_samples_read, process_context.outbuffer, process_context.outbuffer_samples, process_context.sample_ratio);
        samples_generated = res.output_generated;

        if (process_context.post_filter)
            for (int i = 0; i < num_channels; ++i) {
                biquad_apply_buffer (&process_context.lowpass [i] [0], process_context.outbuffer + i, samples_generated, num_channels);
                biquad_apply_buffer (&process_context.lowpass [i] [1], process_context.outbuffer + i, samples_generated, num_channels);
            }

        // finally write the audio, converting to appropriate integer format if requested

        if (outbits != 32) {
            float scaler = (1 << outbits) / 2.0;
            int32_t offset = (outbits <= 8) * 128;
            int32_t highclip = (1 << (outbits - 1)) - 1;
            int32_t lowclip = ~highclip;
            int leftshift = (24 - outbits) % 8;
            int i, j;

            for (i = j = 0; i < samples_generated * num_channels; ++i) {
                int chan = i % num_channels;
                int32_t output = floor ((process_context.outbuffer [i] *= scaler) - process_context.error [chan] + tpdf_dither (chan, -1) + 0.5);

                if (output > highclip) {
                	process_context.clipped_samples++;
                    output = highclip;
                }
                else if (output < lowclip) {
                	process_context.clipped_samples++;
                    output = lowclip;
                }

                process_context.error [chan] += output - process_context.outbuffer [i];
                process_context.tmpbuffer [j++] = output = (output << leftshift) + offset;

                if (outbits > 8) {
                	process_context.tmpbuffer [j++] = output >> 8;

                    if (outbits > 16)
                    	process_context.tmpbuffer [j++] = output >> 16;
                }
            }

            int stream_write_size = num_channels * ((outbits + 7) / 8);

            fwrite_stream (process_context.tmpbuffer, stream_write_size, samples_generated);
        }

        process_context.output_samples += samples_generated;

        if (progress_divider) {
            int new_percent = 100 - process_context.remaining_samples / progress_divider;

            if (new_percent != percent) {
                fprintf (stderr, "\rprogress: %d%% ", percent = new_percent);
                fflush (stderr);
            }
        }
    }

    if (verbosity >= 0)
        fprintf (stderr, "\r...completed successfully\n");

    resampleFree (process_context.resampler);
    tpdf_dither_free ();
    free (process_context.inbuffer);
    free (process_context.outbuffer);
    free (process_context.tmpbuffer);

    if (process_context.clipped_samples)
        fprintf (stderr, "warning: %lu samples were clipped, suggest reducing gain!\n", process_context.clipped_samples);

    if (process_context.remaining_samples)
        fprintf (stderr, "warning: file terminated early!\n");

    return process_context.output_samples;
}
