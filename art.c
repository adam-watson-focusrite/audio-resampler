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


#define IS_BIG_ENDIAN (*(uint16_t *)"\0\xff" < 0x0100)

static const char *sign_on = "\n"
" ART  Audio Resampling Tool  Version 0.2\n"
" Copyright (c) 2006 - 2023 David Bryant.\n\n";

static const char *usage =
" Usage:     ART [-options] infile.wav outfile.wav\n\n"
" Options:  -1|2|3|4    = quality presets, default = 3\n"
"           -r<Hz>      = resample to specified rate\n"
"           -g<dB>      = apply gain (default = 0 dB)\n"
"           -s<degrees> = add specified phase shift (+/-360 degrees)\n"
"           -l<Hz>      = specify alternate lowpass frequency\n"
"           -f<num>     = number of sinc filters (2-1024)\n"
"           -t<num>     = number of sinc taps (4-1024, multiples of 4)\n"
"           -o<bits>    = change output file bitdepth (4-24 or 32)\n"
"           -n          = use nearest filter (don't interpolate)\n"
"           -b          = Blackman-Harris windowing (best stopband)\n"
"           -h          = Hann windowing (fastest transition)\n"
"           -p          = pre/post filtering (cascaded biquads)\n"
"           -q          = quiet mode (display errors only)\n"
"           -v          = verbose (display lots of info)\n"
"           -y          = overwrite outfile if it exists\n\n"
" Web:       Visit www.github.com/dbry/audio-resampler for latest version and info\n\n";

static int wav_process (char *infilename, char *outfilename);

process_context_t process_context={};

int main (argc, argv) int argc; char **argv;
{
    char *infilename = NULL, *outfilename = NULL;

    // loop through command-line arguments

    while (--argc) {
#if defined (_WIN32)
        if ((**++argv == '-' || **argv == '/') && (*argv)[1])
#else
        if ((**++argv == '-') && (*argv)[1])
#endif
            while (*++*argv)
                switch (**argv) {

			case '1':
				process_context.num_filters = process_context.num_taps = 16;
			break;

		    case '2':
		    	process_context.num_filters = process_context.num_taps = 64;
			break;

		    case '3':
		    	process_context.num_filters = process_context.num_taps = 256;
			break;

		    case '4':
		    	process_context.num_filters = process_context.num_taps = 1024;
			break;

                    case 'P': case 'p':
                    	process_context.pre_post_filter = 1;
                        break;

                    case 'Q': case 'q':
                    	process_context.verbosity = -1;
                        break;

                    case 'V': case 'v':
                    	process_context.verbosity = 1;
                        break;

		    case 'R': case 'r':
		    	process_context.resample_rate = strtod (++*argv, argv);
			--*argv;
			break;

		    case 'S': case 's':
		    	process_context.phase_shift = strtod (++*argv, argv) / 360.0;

                        if (process_context.phase_shift <= -1.0 || process_context.phase_shift >= 1.0) {
                            fprintf (stderr, "\nphase shift must be less than +/- 1 sample!\n");
                            return 1;
                        }

			--*argv;
			break;

		    case 'G': case 'g':
		    	process_context.gain = pow (10.0, strtod (++*argv, argv) / 20.0);
			--*argv;
			break;

		    case 'L': case 'l':
		    	process_context.lowpass_freq = strtod (++*argv, argv);
			--*argv;
			break;

		    case 'F': case 'f':
		    	process_context.num_filters = strtod (++*argv, argv);

                        if (process_context.num_filters < 2 || process_context.num_filters > 1024) {
                            fprintf (stderr, "\nnum of filters must be 2 - 1024!\n");
                            return 1;
                        }

			--*argv;
			break;

			--*argv;
			break;

		    case 'T': case 't':
		    	process_context.num_taps = strtod (++*argv, argv);

                        if ((process_context.num_taps & 3) || process_context.num_taps < 4 || process_context.num_taps > 1024) {
                            fprintf (stderr, "\nnum of taps must be 4 - 1024 and a multiple of 4!\n");
                            return 1;
                        }

			--*argv;
			break;

		    case 'N': case 'n':
		    	process_context.interpolate = 0;
			break;

		    case 'B': case 'b':
		    	process_context.bh4_window = 1;
			break;

		    case 'H': case 'h':
		    	process_context.hann_window = 1;
			break;

                    default:
                        fprintf (stderr, "\nillegal option: %c !\n", **argv);
                        return 1;
                }
        else if (!infilename) {
            infilename = malloc (strlen (*argv) + 10);
            strcpy (infilename, *argv);
        }
        else if (!outfilename) {
            outfilename = malloc (strlen (*argv) + 10);
            strcpy (outfilename, *argv);
        }
        else {
            fprintf (stderr, "\nextra unknown argument: %s !\n", *argv);
            return 1;
        }
    }

    if (process_context.verbosity >= 0)
        fprintf (stderr, "%s", sign_on);

    if (!outfilename) {
        printf ("%s", usage);
        return 0;
    }

    if (!strcmp (infilename, outfilename)) {
        fprintf (stderr, "can't overwrite input file (specify different/new output file name)\n");
        return -1;
    }

    int res = wav_process (infilename, outfilename);

    free (infilename);
    free (outfilename);

    return res;
}

typedef struct {
    char ckID [4];
    uint32_t ckSize;
    char formType [4];
} RiffChunkHeader;

typedef struct {
    char ckID [4];
    uint32_t ckSize;
} ChunkHeader;

#define ChunkHeaderFormat "4L"

typedef struct {
    uint16_t FormatTag, NumChannels;
    uint32_t SampleRate, BytesPerSecond;
    uint16_t BlockAlign, BitsPerSample;
    uint16_t cbSize;
    union {
        uint16_t ValidBitsPerSample;
        uint16_t SamplesPerBlock;
        uint16_t Reserved;
    } Samples;
    int32_t ChannelMask;
    uint16_t SubFormat;
    char GUID [14];
} WaveHeader;

#define WaveHeaderFormat "SSLLSSSSLS"

#define WAVE_FORMAT_PCM         0x1
#define WAVE_FORMAT_IEEE_FLOAT  0x3
#define WAVE_FORMAT_EXTENSIBLE  0xfffe

static int write_pcm_wav_header (FILE *outfile, int bps, int num_channels, unsigned long num_samples, unsigned long sample_rate, uint32_t channel_mask);
static void little_endian_to_native (void *data, char *format);
static void native_to_little_endian (void *data, char *format);

static int wav_process (char *infilename, char *outfilename)
{
    int format = 0, res = 0;
    uint32_t channel_mask = 0;

    RiffChunkHeader riff_chunk_header;
    ChunkHeader chunk_header;
    WaveHeader WaveHeader;

    // open both input and output files

    if (!(process_context.in_stream = fopen (infilename, "rb"))) {
        fprintf (stderr, "can't open file \"%s\" for reading!\n", infilename);
        return -1;
    }

    if (!(process_context.out_stream = fopen (outfilename, "wb"))) {
        fprintf (stderr, "can't open file \"%s\" for writing!\n", outfilename);
        fclose (process_context.in_stream);
        return -1;
    }

    // read (and write) initial RIFF form header

    if (!fread (&riff_chunk_header, sizeof (RiffChunkHeader), 1, process_context.in_stream) ||
        strncmp (riff_chunk_header.ckID, "RIFF", 4) ||
        strncmp (riff_chunk_header.formType, "WAVE", 4)) {
            fprintf (stderr, "\"%s\" is not a valid .WAV file!\n", infilename);
            fclose (process_context.out_stream);
            fclose (process_context.in_stream);
            return -1;
    }

    // loop through all elements of the RIFF wav header (until the data chuck)

    while (1) {

        if (!fread (&chunk_header, sizeof (ChunkHeader), 1, process_context.in_stream)) {
            fprintf (stderr, "\"%s\" is not a valid .WAV file!\n", infilename);
            fclose (process_context.out_stream);
            fclose (process_context.in_stream);
            return -1;
        }

        little_endian_to_native (&chunk_header, ChunkHeaderFormat);

        // if it's the format chunk, we want to get some info out of there and
        // make sure it's a .wav file we can handle

        if (!strncmp (chunk_header.ckID, "fmt ", 4)) {
            int supported = 1;

            if (chunk_header.ckSize < 16 || chunk_header.ckSize > sizeof (WaveHeader) ||
                !fread (&WaveHeader, chunk_header.ckSize, 1, process_context.in_stream)) {
                    fprintf (stderr, "\"%s\" is not a valid .WAV file!\n", infilename);
                    fclose (process_context.out_stream);
                    fclose (process_context.in_stream);
                    return -1;
            }

            little_endian_to_native (&WaveHeader, WaveHeaderFormat);

            format = (WaveHeader.FormatTag == WAVE_FORMAT_EXTENSIBLE && chunk_header.ckSize == 40) ?
                WaveHeader.SubFormat : WaveHeader.FormatTag;

            channel_mask = (WaveHeader.FormatTag == WAVE_FORMAT_EXTENSIBLE && chunk_header.ckSize == 40) ?
                WaveHeader.ChannelMask : 0;

            process_context.inbits = (chunk_header.ckSize == 40 && WaveHeader.Samples.ValidBitsPerSample) ?
                WaveHeader.Samples.ValidBitsPerSample : WaveHeader.BitsPerSample;

            if (WaveHeader.NumChannels < 1 || WaveHeader.NumChannels > 32)
                supported = 0;
            else if (format == WAVE_FORMAT_PCM) {

                if (process_context.inbits < 4 || process_context.inbits > 24)
                    supported = 0;

                if (WaveHeader.BlockAlign != WaveHeader.NumChannels * ((process_context.inbits + 7) / 8))
                    supported = 0;
            }
            else if (format == WAVE_FORMAT_IEEE_FLOAT) {

                if (process_context.inbits != 32)
                    supported = 0;

                if (WaveHeader.BlockAlign != WaveHeader.NumChannels * 4)
                    supported = 0;
            }
            else
                supported = 0;

            if (!supported) {
                fprintf (stderr, "\"%s\" is an unsupported .WAV format!\n", infilename);
                fclose (process_context.out_stream);
                fclose (process_context.in_stream);
                return -1;
            }

            if (process_context.verbosity > 0) {
                fprintf (stderr, "format tag size = %d\n", chunk_header.ckSize);
                fprintf (stderr, "FormatTag = 0x%x, NumChannels = %u, BitsPerSample = %u\n",
                    WaveHeader.FormatTag, WaveHeader.NumChannels, WaveHeader.BitsPerSample);
                fprintf (stderr, "BlockAlign = %u, SampleRate = %lu, BytesPerSecond = %lu\n",
                    WaveHeader.BlockAlign, (unsigned long) WaveHeader.SampleRate, (unsigned long) WaveHeader.BytesPerSecond);

                if (chunk_header.ckSize > 16)
                    fprintf (stderr, "cbSize = %d, ValidBitsPerSample = %d\n", WaveHeader.cbSize,
                        WaveHeader.Samples.ValidBitsPerSample);

                if (chunk_header.ckSize > 20)
                    fprintf (stderr, "ChannelMask = %x, SubFormat = %d\n",
                        WaveHeader.ChannelMask, WaveHeader.SubFormat);
            }
        }
        else if (!strncmp (chunk_header.ckID, "data", 4)) {

            // on the data chunk, get size and exit parsing loop

            if (!WaveHeader.NumChannels) {      // make sure we saw a "fmt" chunk...
                fprintf (stderr, "\"%s\" is not a valid .WAV file!\n", infilename);
                fclose (process_context.out_stream);
                fclose (process_context.in_stream);
                return -1;
            }

            if (!chunk_header.ckSize) {
                fprintf (stderr, "this .WAV file has no audio samples, probably is corrupt!\n");
                fclose (process_context.out_stream);
                fclose (process_context.in_stream);
                return -1;
            }

            if (chunk_header.ckSize % WaveHeader.BlockAlign) {
                fprintf (stderr, "\"%s\" is not a valid .WAV file!\n", infilename);
                fclose (process_context.out_stream);
                fclose (process_context.in_stream);
                return -1;
            }

            process_context.num_samples = chunk_header.ckSize / WaveHeader.BlockAlign;

            if (!process_context.num_samples) {
                fprintf (stderr, "this .WAV file has no audio samples, probably is corrupt!\n");
                fclose (process_context.out_stream);
                fclose (process_context.in_stream);
                return -1;
            }

            if (process_context.verbosity > 0)
                fprintf (stderr, "num samples = %u\n", process_context.num_samples);

            process_context.num_channels = WaveHeader.NumChannels;
            process_context.sample_rate = WaveHeader.SampleRate;
            break;
        }
        else {          // just ignore/copy unknown chunks
            unsigned int bytes_to_copy = (chunk_header.ckSize + 1) & ~1L;

            if (process_context.verbosity > 0)
                fprintf (stderr, "extra unknown chunk \"%c%c%c%c\" of %u bytes\n",
                    chunk_header.ckID [0], chunk_header.ckID [1], chunk_header.ckID [2],
                    chunk_header.ckID [3], bytes_to_copy);

            while (bytes_to_copy) {
                unsigned int bytes_to_read = bytes_to_copy, bytes_read;
                char temp_buffer [256];

                if (bytes_to_read > sizeof (temp_buffer))
                    bytes_to_read = sizeof (temp_buffer);

                bytes_read = fread (temp_buffer, 1, bytes_to_read, process_context.in_stream);

                if (bytes_read != bytes_to_read) {
                    fprintf (stderr, "\"%s\" is not a valid .WAV file!\n", infilename);
                    fclose (process_context.out_stream);
                    fclose (process_context.in_stream);
                    return -1;
                }

                bytes_to_copy -= bytes_read;
            }
        }
    }

    if (!process_context.num_channels || !process_context.sample_rate || !process_context.inbits || !process_context.num_samples) {
        fprintf (stderr, "\"%s\" is not a valid .WAV file!\n", infilename);
        fclose (process_context.out_stream);
        fclose (process_context.in_stream);
        return -1;
    }

    // if not specified, preserve sample rate and bitdepth of input

    if (!process_context.resample_rate)
    	process_context.resample_rate = process_context.sample_rate;

    if (process_context.verbosity >= 0)
        fprintf (stderr, "resampling %d-channel file \"%s\" (%db/%dk) to \"%s\" (%db/%dk)...\n",
        		process_context.num_channels, infilename, process_context.inbits, (int)((process_context.sample_rate + 500) / 1000),
            outfilename, process_context.outbits, (int)((process_context.resample_rate + 500) / 1000));

    if (!write_pcm_wav_header (process_context.out_stream, process_context.outbits, process_context.num_channels, process_context.num_samples, process_context.resample_rate, channel_mask)) {
        fprintf (stderr, "can't write to file \"%s\"!\n", outfilename);
        fclose (process_context.out_stream);
        fclose (process_context.in_stream);
        return -1;
    }

    unsigned int output_samples = art_resample_process_audio(process_context.num_samples);

    rewind (process_context.out_stream);

    if (!write_pcm_wav_header (process_context.out_stream, process_context.outbits, process_context.num_channels, output_samples, process_context.resample_rate, channel_mask)) {
        fprintf (stderr, "can't write to file \"%s\"!\n", outfilename);
        fclose (process_context.out_stream);
        fclose (process_context.in_stream);
        return -1;
    }

    fclose (process_context.out_stream);
    fclose (process_context.in_stream);
    return res;
}

static int write_pcm_wav_header (FILE *outfile, int bps, int num_channels, unsigned long num_samples, unsigned long sample_rate, uint32_t channel_mask)
{
    RiffChunkHeader riffhdr;
    ChunkHeader datahdr, fmthdr;
    WaveHeader wavhdr;

    int wavhdrsize = 16;
    int bytes_per_sample = (bps + 7) / 8;
    int format = (bps == 32) ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    uint32_t total_data_bytes = num_samples * bytes_per_sample * num_channels;

    memset (&wavhdr, 0, sizeof (wavhdr));

    wavhdr.FormatTag = format;
    wavhdr.NumChannels = num_channels;
    wavhdr.SampleRate = sample_rate;
    wavhdr.BytesPerSecond = sample_rate * num_channels * bytes_per_sample;
    wavhdr.BlockAlign = bytes_per_sample * num_channels;
    wavhdr.BitsPerSample = bps;

    if (channel_mask) {
        wavhdrsize = sizeof (wavhdr);
        wavhdr.cbSize = 22;
        wavhdr.Samples.ValidBitsPerSample = bps;
        wavhdr.SubFormat = format;
        wavhdr.ChannelMask = channel_mask;
        wavhdr.FormatTag = WAVE_FORMAT_EXTENSIBLE;
        wavhdr.BitsPerSample = bps;
        wavhdr.GUID [4] = 0x10;
        wavhdr.GUID [6] = 0x80;
        wavhdr.GUID [9] = 0xaa;
        wavhdr.GUID [11] = 0x38;
        wavhdr.GUID [12] = 0x9b;
        wavhdr.GUID [13] = 0x71;
    }

    memcpy (riffhdr.ckID, "RIFF", sizeof (riffhdr.ckID));
    memcpy (riffhdr.formType, "WAVE", sizeof (riffhdr.formType));
    riffhdr.ckSize = sizeof (riffhdr) + wavhdrsize + sizeof (datahdr) + total_data_bytes;
    memcpy (fmthdr.ckID, "fmt ", sizeof (fmthdr.ckID));
    fmthdr.ckSize = wavhdrsize;

    memcpy (datahdr.ckID, "data", sizeof (datahdr.ckID));
    datahdr.ckSize = total_data_bytes;

    // write the RIFF chunks up to just before the data starts

    native_to_little_endian (&riffhdr, ChunkHeaderFormat);
    native_to_little_endian (&fmthdr, ChunkHeaderFormat);
    native_to_little_endian (&wavhdr, WaveHeaderFormat);
    native_to_little_endian (&datahdr, ChunkHeaderFormat);

    return fwrite (&riffhdr, sizeof (riffhdr), 1, outfile) &&
        fwrite (&fmthdr, sizeof (fmthdr), 1, outfile) &&
        fwrite (&wavhdr, wavhdrsize, 1, outfile) &&
        fwrite (&datahdr, sizeof (datahdr), 1, outfile);
}

static void little_endian_to_native (void *data, char *format)
{
    unsigned char *cp = (unsigned char *) data;
    int32_t temp;

    while (*format) {
        switch (*format) {
            case 'L':
                temp = cp [0] + ((int32_t) cp [1] << 8) + ((int32_t) cp [2] << 16) + ((int32_t) cp [3] << 24);
                * (int32_t *) cp = temp;
                cp += 4;
                break;

            case 'S':
                temp = cp [0] + (cp [1] << 8);
                * (short *) cp = (short) temp;
                cp += 2;
                break;

            default:
                if (isdigit ((unsigned char) *format))
                    cp += *format - '0';

                break;
        }

        format++;
    }
}

static void native_to_little_endian (void *data, char *format)
{
    unsigned char *cp = (unsigned char *) data;
    int32_t temp;

    while (*format) {
        switch (*format) {
            case 'L':
                temp = * (int32_t *) cp;
                *cp++ = (unsigned char) temp;
                *cp++ = (unsigned char) (temp >> 8);
                *cp++ = (unsigned char) (temp >> 16);
                *cp++ = (unsigned char) (temp >> 24);
                break;

            case 'S':
                temp = * (short *) cp;
                *cp++ = (unsigned char) temp;
                *cp++ = (unsigned char) (temp >> 8);
                break;

            default:
                if (isdigit ((unsigned char) *format))
                    cp += *format - '0';

                break;
        }

        format++;
    }
}

