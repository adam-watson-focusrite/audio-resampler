#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>

#define IS_BIG_ENDIAN (*(uint16_t *)"\0\xff" < 0x0100)

#define ART_STREAM_NUM_CHANNELS 2
#define ART_STREAM_CLIP_CHECK


typedef struct
{
	uint32_t resample_rate;
	uint32_t sample_rate;
	uint32_t lowpass_freq;
	uint16_t num_taps;
	uint16_t num_filters;
	double phase_shift;
	double gain;

	double sample_ratio;
    double lowpass_ratio;

    uint32_t outbuffer_samples;
    uint32_t remaining_samples;
    uint32_t output_samples;

#ifdef ART_STREAM_CLIP_CHECK
    uint32_t clipped_samples;
#endif

    uint32_t num_samples;

    float *outbuffer;
    float *inbuffer;
    uint8_t *tmpbuffer; // used as a go between for integer data!

    void *readbuffer;

    uint16_t flags;
    uint16_t samples_to_append;

    uint8_t pre_filter;
    uint8_t post_filter;

    uint8_t bh4_window;
    uint8_t hann_window;
    uint8_t verbosity;
    uint8_t interpolate;
    uint8_t pre_post_filter;

    uint8_t num_channels;
    uint8_t outbits;
    uint8_t inbits;

    Biquad lowpass [ART_STREAM_NUM_CHANNELS][2];
    BiquadCoefficients lowpass_coeff;
    Resample *resampler;

    float error [ART_STREAM_NUM_CHANNELS];

    uint32_t BUFFER_SAMPLES;

    FILE* in_stream;
    FILE* out_stream;
}process_context_t;

uint16_t art_resample_init();
uint16_t art_resample_deinit();
uint16_t art_resample_process_audio();

