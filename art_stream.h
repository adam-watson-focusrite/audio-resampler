#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>

#define ART_STREAM_NUM_CHANNELS 2
typedef struct
{
	uint32_t resample_rate;
	uint32_t sample_rate;
	uint32_t lowpass_freq;
	int num_taps;
	int num_filters;
	double phase_shift;
	double gain;

	double sample_ratio;
    double lowpass_ratio;

    unsigned int outbuffer_samples;
    unsigned long remaining_samples;
    unsigned long output_samples;
    unsigned long clipped_samples;
    unsigned long num_samples;

    float *outbuffer;
    float *inbuffer;
    unsigned char *tmpbuffer;

    void *readbuffer;

    int flags;
    int samples_to_append;

    int pre_filter;
    int post_filter;

    int bh4_window;
    int hann_window;
    int verbosity;
    int interpolate;
    int pre_post_filter;

    int num_channels;
    int outbits;
    int inbits;

    Biquad lowpass [ART_STREAM_NUM_CHANNELS][2];
    BiquadCoefficients lowpass_coeff;
    Resample *resampler;

    float error [ART_STREAM_NUM_CHANNELS];

    FILE* in_stream;
    FILE* out_stream;
}process_context_t;

#define BUFFER_SAMPLES          441

#define IS_BIG_ENDIAN (*(uint16_t *)"\0\xff" < 0x0100)

unsigned int process_audio_init();
unsigned int process_audio_deinit();
unsigned int process_audio();

