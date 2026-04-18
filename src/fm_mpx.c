/*
    PiFmRds - FM/RDS transmitter for the Raspberry Pi
    Copyright (C) 2014 Christophe Jacquet, F8FTK

    See https://github.com/ChristopheJacquet/PiFmRds

    fm_mpx.c: generates an FM multiplex signal containing RDS plus possibly
    monaural or stereo audio. Depends on libsndfile for audio input.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define _USE_MATH_DEFINES
#include <math.h>

#include "rds.h"
#include "fm_mpx.h"
#include "pifm_common.h"


#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


#define FIR_HALF_SIZE 30
#define FIR_SIZE (2*FIR_HALF_SIZE-1)
/* §2.1: Doubled ring buffer so the symmetric-fold loop below walks a
 * contiguous FIR_SIZE-long slice without modulo or wrap branches.
 * Every insert writes the new sample into two positions (idx and
 * idx + FIR_SIZE); the loop can then do buf[w+fi] + buf[w+FIR_SIZE-1-fi]
 * with no conditional, which GCC auto-vectorizes on ARMv7/v8. */
#define FIR_BUF_SIZE (2*FIR_SIZE)


static const float carrier_38[] = {
    0.0f,
    0.8660254037844386f,
    0.8660254037844388f,
    1.2246467991473532e-16f,
    -0.8660254037844384f,
    -0.8660254037844386f,
};

static const float carrier_19[] = {
    0.0f,
    0.5f,
    0.8660254037844386f,
    1.0f,
    0.8660254037844388f,
    0.5f,
    1.2246467991473532e-16f,
    -0.5f,
    -0.8660254037844384f,
    -1.0f,
    -0.8660254037844386f,
    -0.5f,
};


/* All MPX per-instance state. Previously a dozen file-scope globals;
 * collecting them in a struct makes the lifetime explicit and allows
 * the upcoming producer/consumer thread split to pass a single handle
 * around. */
struct fm_mpx_ctx {
    size_t length;

    /* Low-pass FIR filter (symmetric, only half stored). */
    float  low_pass_fir[FIR_HALF_SIZE];

    /* 19 kHz pilot / 38 kHz stereo subcarrier phase indices. */
    int    phase_19;
    int    phase_38;

    /* Resampler: audio samplerate -> MPX sample rate. */
    float  downsample_factor;
    float  audio_pos;

    /* Audio input state. */
    SNDFILE    *inf;
    float      *audio_buffer;
    sf_count_t  audio_index;
    sf_count_t  audio_len;
    int         channels;

    /* FIR ring buffers (doubled; see FIR_BUF_SIZE comment above). */
    float  fir_buffer_mono[FIR_BUF_SIZE];
    float  fir_buffer_stereo[FIR_BUF_SIZE];
    int    fir_index;   /* in [0, FIR_SIZE); the write position. */

    /* Non-owning pointer to an RDS context; NULL means "no RDS". */
    rds_ctx_t *rds;
};

static float *alloc_empty_buffer(size_t length) {
    float *p = calloc(length, sizeof(float));
    return p;
}


pifm_status_t fm_mpx_ctx_open(fm_mpx_ctx_t **out, const char *filename,
                              size_t len, rds_ctx_t *rds) {
    if(out == NULL) return PIFM_ERR_ARG;
    *out = NULL;

    struct fm_mpx_ctx *ctx = calloc(1, sizeof(*ctx));
    if(ctx == NULL) return PIFM_ERR_MEM;

    ctx->length = len;
    ctx->rds    = rds;

    if(filename != NULL) {
        SF_INFO sfinfo;

        /* stdin or file on the filesystem? */
        if(filename[0] == '-') {
            if(! (ctx->inf = sf_open_fd(fileno(stdin), SFM_READ, &sfinfo, 0))) {
                fprintf(stderr, "Error: could not open stdin for audio input.\n");
                free(ctx);
                return PIFM_ERR_IO;
            }
            printf("Using stdin for audio input.\n");
        } else {
            if(! (ctx->inf = sf_open(filename, SFM_READ, &sfinfo))) {
                fprintf(stderr, "Error: could not open input file %s.\n", filename);
                free(ctx);
                return PIFM_ERR_IO;
            }
            printf("Using audio file: %s\n", filename);
        }

        int in_samplerate = sfinfo.samplerate;
        ctx->downsample_factor = (float)MPX_SAMPLE_RATE / in_samplerate;

        printf("Input: %d Hz, upsampling factor: %.2f\n", in_samplerate, ctx->downsample_factor);

        ctx->channels = sfinfo.channels;
        if(ctx->channels > 1) {
            printf("%d channels, generating stereo multiplex.\n", ctx->channels);
        } else {
            printf("1 channel, monophonic operation.\n");
        }

        /* Create the low-pass FIR filter. */
        float cutoff_freq = AUDIO_LPF_CUTOFF_HZ * AUDIO_LPF_CUTOFF_FACTOR;
        if(in_samplerate/2 < cutoff_freq) cutoff_freq = in_samplerate/2 * AUDIO_LPF_CUTOFF_FACTOR;

        ctx->low_pass_fir[FIR_HALF_SIZE-1] = 2 * cutoff_freq / MPX_SAMPLE_RATE / 2;
        /* The central coefficient is divided by two because the
         * symmetric-fold inner loop adds it twice. */

        /* Only store half of the filter since it is symmetric. */
        for(int i=1; i<FIR_HALF_SIZE; i++) {
            ctx->low_pass_fir[FIR_HALF_SIZE-1-i] =
                sin(2 * M_PI * cutoff_freq * i / MPX_SAMPLE_RATE) / (M_PI * i)
                * (.54 - .46 * cos(2*M_PI * (i+FIR_HALF_SIZE) / (2*FIR_HALF_SIZE)));
        }
        printf("Created low-pass FIR filter for audio channels, with cutoff at %.1f Hz\n", cutoff_freq);

        ctx->audio_pos    = ctx->downsample_factor;
        ctx->audio_buffer = alloc_empty_buffer(len * ctx->channels);
        if(ctx->audio_buffer == NULL) {
            sf_close(ctx->inf);
            free(ctx);
            return PIFM_ERR_MEM;
        }
    } else {
        /* inf == NULL means RDS-only, no audio. */
        ctx->inf = NULL;
    }

    *out = ctx;
    return PIFM_OK;
}


/* Generate `ctx->length` MPX samples into `mpx_buffer`. Samples are in
 * roughly [-10, +10]; divide by 10 before D/A. */
pifm_status_t fm_mpx_ctx_get_samples(fm_mpx_ctx_t *ctx, float *mpx_buffer) {
    if(ctx == NULL) return PIFM_ERR_ARG;

    if(ctx->rds) {
        rds_ctx_get_samples(ctx->rds, mpx_buffer, (int)ctx->length);
    } else {
        memset(mpx_buffer, 0, ctx->length * sizeof(float));
    }

    if(ctx->inf == NULL) return PIFM_OK;

    for(size_t i=0; i<ctx->length; i++) {
        if(ctx->audio_pos >= ctx->downsample_factor) {
            ctx->audio_pos -= ctx->downsample_factor;

            if(ctx->audio_len == 0) {
                for(int j=0; j<2; j++) { /* one retry */
                    /* sf_read_float takes an item count = samples, not
                     * frames. Request a whole number of multi-channel
                     * frames so the (index, index+1) stereo pair below
                     * always reads valid data. */
                    ctx->audio_len = sf_read_float(ctx->inf, ctx->audio_buffer,
                                                   (sf_count_t)ctx->length * ctx->channels);
                    if(ctx->audio_len < 0) {
                        fprintf(stderr, "Error reading audio\n");
                        return PIFM_ERR_IO;
                    }
                    if(ctx->audio_len == 0) {
                        if(sf_seek(ctx->inf, 0, SEEK_SET) < 0) {
                            fprintf(stderr, "Could not rewind in audio file, terminating\n");
                            return PIFM_ERR_IO;
                        }
                    } else {
                        break;
                    }
                }
                ctx->audio_index = 0;
            } else {
                ctx->audio_index += ctx->channels;
                ctx->audio_len   -= ctx->channels;
            }
        }

        /* Store the new sample at both wi and wi+FIR_SIZE so the
         * symmetric-fold loop can walk a contiguous FIR_SIZE-long
         * slice without modulo or wrap branches. */
        int wi = ctx->fir_index;
        if(ctx->channels == 1) {
            float m_in = ctx->audio_buffer[ctx->audio_index];
            ctx->fir_buffer_mono[wi]            = m_in;
            ctx->fir_buffer_mono[wi + FIR_SIZE] = m_in;
        } else {
            /* Stereo: sum (L+R) in mono path, diff (L-R) in stereo path. */
            float l = ctx->audio_buffer[ctx->audio_index];
            float r = ctx->audio_buffer[ctx->audio_index + 1];
            float m_in = l + r;
            float s_in = l - r;
            ctx->fir_buffer_mono[wi]              = m_in;
            ctx->fir_buffer_mono[wi + FIR_SIZE]   = m_in;
            ctx->fir_buffer_stereo[wi]            = s_in;
            ctx->fir_buffer_stereo[wi + FIR_SIZE] = s_in;
        }

        /* Advance the write cursor; `read_start` selects the slice
         * buf[read_start .. read_start + FIR_SIZE - 1], which holds
         * the last FIR_SIZE samples in oldest-to-newest order thanks
         * to the mirrored write above. */
        int read_start = (wi + 1) % FIR_SIZE;
        ctx->fir_index = read_start;

        const float *bufm = &ctx->fir_buffer_mono  [read_start];
        const float *bufs = &ctx->fir_buffer_stereo[read_start];
        const float *lp   = ctx->low_pass_fir;
        float out_mono   = 0.0f;
        float out_stereo = 0.0f;
        /* Symmetric filter: lp[fi] is c_{fi} = c_{FIR_SIZE-1-fi}, so
         * we pair the two mirrored positions and save half the mul-adds.
         * GCC -O3 -ftree-vectorize auto-vectorizes this on ARMv7/v8
         * (no NEON intrinsics needed). */
        for(int fi=0; fi<FIR_HALF_SIZE; fi++) {
            out_mono += lp[fi] * (bufm[fi] + bufm[FIR_SIZE - 1 - fi]);
        }
        if(ctx->channels > 1) {
            for(int fi=0; fi<FIR_HALF_SIZE; fi++) {
                out_stereo += lp[fi] * (bufs[fi] + bufs[FIR_SIZE - 1 - fi]);
            }
        }

        mpx_buffer[i] =
            mpx_buffer[i] +                    /* RDS samples already present */
            MPX_AUDIO_GAIN * out_mono;         /* monophonic (or stereo-sum) */

        if(ctx->channels > 1) {
            mpx_buffer[i] +=
                MPX_STEREO_DIFF_GAIN * carrier_38[ctx->phase_38] * out_stereo +
                MPX_PILOT_GAIN       * carrier_19[ctx->phase_19];

            ctx->phase_19++;
            ctx->phase_38++;
            if(ctx->phase_19 >= 12) ctx->phase_19 = 0;
            if(ctx->phase_38 >= 6)  ctx->phase_38 = 0;
        }

        ctx->audio_pos++;
    }

    return PIFM_OK;
}


pifm_status_t fm_mpx_ctx_close(fm_mpx_ctx_t **ctx_pp) {
    if(ctx_pp == NULL || *ctx_pp == NULL) return PIFM_OK;
    struct fm_mpx_ctx *ctx = *ctx_pp;

    if(ctx->inf != NULL && sf_close(ctx->inf)) {
        fprintf(stderr, "Error closing audio file\n");
    }
    ctx->inf = NULL;

    free(ctx->audio_buffer);
    ctx->audio_buffer = NULL;

    free(ctx);
    *ctx_pp = NULL;
    return PIFM_OK;
}


/* --- classic singleton API ---------------------------------------------- */

/* A single process-wide MPX context. The old `fm_mpx_open/close/get`
 * API is preserved by routing through this singleton. */
static struct fm_mpx_ctx *default_ctx = NULL;

pifm_status_t fm_mpx_open(const char *filename, size_t len) {
    /* The legacy API always mixes RDS in; hook up the default RDS
     * singleton. `rds_default_ctx()` lazily creates one if the caller
     * hasn't already set up PS/RT via `rds_set_*`. */
    return fm_mpx_ctx_open(&default_ctx, filename, len, rds_default_ctx());
}

pifm_status_t fm_mpx_get_samples(float *mpx_buffer) {
    if(default_ctx == NULL) return PIFM_ERR_ARG;
    return fm_mpx_ctx_get_samples(default_ctx, mpx_buffer);
}

pifm_status_t fm_mpx_close(void) {
    return fm_mpx_ctx_close(&default_ctx);
}
