/*
    PiFmRds - FM/RDS transmitter for the Raspberry Pi
    Copyright (C) 2014 Christophe Jacquet, F8FTK

    See https://github.com/ChristopheJacquet/PiFmRds

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

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>

#include "rds.h"
#include "rds_internal.h"
#include "rds_strings.h"
#include "waveforms.h"
#include "pifm_common.h"

#define GROUP_LENGTH 4

/* The RDS error-detection code generator polynomial is
   x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + x^0
*/
#define POLY 0x1B9
#define POLY_DEG 10
#define MSB_BIT 0x8000
#define BLOCK_SIZE 16

#define BITS_PER_GROUP (GROUP_LENGTH * (BLOCK_SIZE+POLY_DEG))
#define SAMPLES_PER_BIT 192
/* Cast to int so the loop counters (also int) don't provoke
 * -Wsign-compare. sizeof(...) is size_t; both values fit in int by a
 * wide margin (FILTER_SIZE == 576, SAMPLE_BUFFER_SIZE == 768). */
#define FILTER_SIZE ((int)(sizeof(waveform_biphase)/sizeof(float)))
#define SAMPLE_BUFFER_SIZE (SAMPLES_PER_BIT + FILTER_SIZE)

static const uint16_t offset_words[] = {0x0FC, 0x198, 0x168, 0x1B4};
/* We don't handle offset word C' here for the sake of simplicity. */

/* Precomputed 57 kHz / 228 kHz subcarrier LUT (§2.3).
 * 228 kHz is exactly 4 x 57 kHz, so the modulation sequence is a
 * 4-element period. */
static const float rds_carrier57[4] = { 0.0f, 1.0f, 0.0f, -1.0f };

/* All RDS per-instance state. Previously this lived as a mix of
 * file-scope non-static globals (`rds_params`) and function-local
 * `static` variables inside `get_rds_group` and `rds_get_samples`.
 * Collecting it here unlocks multi-instance use and is a prerequisite
 * for the producer/consumer thread split. */
struct rds_ctx {
    /* Metadata set by the caller. */
    uint16_t pi;
    int      ta;
    char     ps[PS_LENGTH];
    char     rt[RT_LENGTH];

    /* Group sequencer (was function-local statics in get_rds_group). */
    int      group_state;
    int      ps_state;
    int      rt_state;
    int      latest_minutes;

    /* Differential encoder + bit pump (was function-local statics in
     * rds_get_samples). */
    int      bit_buffer[BITS_PER_GROUP];
    int      bit_pos;
    float    sample_buffer[SAMPLE_BUFFER_SIZE];
    int      prev_output;
    int      cur_output;
    int      sample_count;
    int      inverting;
    int      phase;
    int      in_sample_index;
    int      out_sample_index;
};

/* Classical CRC computation (pure function, no state). Exposed to
 * unit tests via rds_internal.h. Result is the POLY_DEG-bit (10-bit)
 * checkword, zero-extended into the returned uint16_t. Bits above
 * bit 9 are always zero. */
uint16_t rds_crc(uint16_t block) {
    uint16_t crc = 0;

    for(int j=0; j<BLOCK_SIZE; j++) {
        int bit = (block & MSB_BIT) != 0;
        block <<= 1;

        int msb = (crc >> (POLY_DEG-1)) & 1;
        crc <<= 1;
        if((msb ^ bit) != 0) {
            crc = crc ^ POLY;
        }
    }

    /* Mask off any debris bits the uint16_t shift accumulated above
     * bit 9. The production caller ignores them (the checkword
     * emitter only reads bits 9..0), but well-defined function
     * contract + unit tests want a canonical 10-bit result. */
    return crc & ((1u << POLY_DEG) - 1);
}

/* MJD computation per EN 50067. Extracted into its own function so
 * unit tests can exercise it without setting the system clock. The
 * `l` trick ("shift Jan/Feb into the previous year") makes the rest
 * of the formula work without a month-length table. Integer
 * arithmetic throughout (1461/4 == exactly 365.25,
 * 306001/10000 == 30.6001 on the month range we care about) so
 * -ffast-math cannot introduce off-by-one errors on boundary dates. */
int rds_mjd(int tm_year, int tm_mon, int tm_mday) {
    int l = tm_mon <= 1 ? 1 : 0;
    int y = tm_year - l;
    int m = tm_mon + 2 + l*12;
    return 14956 + tm_mday + (y * 1461) / 4 + (m * 306001) / 10000;
}

/* Possibly generates a CT (clock time) group if the minute has just
 * changed. Returns 1 if the CT group was generated, 0 otherwise. */
static int get_rds_ct_group(struct rds_ctx *ctx, uint16_t *blocks) {
    time_t now;
    struct tm *utc;

    now = time(NULL);
    utc = gmtime(&now);

    if(utc->tm_min != ctx->latest_minutes) {
        ctx->latest_minutes = utc->tm_min;

        int mjd = rds_mjd(utc->tm_year, utc->tm_mon, utc->tm_mday);

        blocks[1] = 0x4400 | (mjd>>15);
        blocks[2] = (mjd<<1) | (utc->tm_hour>>4);
        blocks[3] = (utc->tm_hour & 0xF)<<12 | utc->tm_min<<6;

        /* §1.10: tm_gmtoff is a BSD/glibc extension. Recompute from
         * timegm() if it isn't available. */
        utc = localtime(&now);
        long gmt_off_seconds;
#if defined(__USE_MISC) || defined(__USE_BSD) || defined(__GLIBC__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
        gmt_off_seconds = utc->tm_gmtoff;
#else
        {
            struct tm local_tm = *utc;
            time_t local_as_utc = timegm(&local_tm);
            gmt_off_seconds = (long)difftime(local_as_utc, now);
        }
#endif
        int offset = (int)(gmt_off_seconds / (30 * 60));
        if (offset < 0) {
            blocks[3] |= 0x20;
            blocks[3] |= (unsigned)(-offset) & 0x1F;
        } else {
            blocks[3] |= (unsigned)offset & 0x1F;
        }

        return 1;
    }
    return 0;
}

/* Creates an RDS group. Pattern: 4x 0A followed by 1x 2A (length 5). */
static void get_rds_group(struct rds_ctx *ctx, int *buffer) {
    uint16_t blocks[GROUP_LENGTH] = {ctx->pi, 0, 0, 0};

    /* CT (clock time) has priority over other group types. */
    if(! get_rds_ct_group(ctx, blocks)) {
        if(ctx->group_state < 4) {
            blocks[1] = 0x0400 | ctx->ps_state;
            if(ctx->ta) blocks[1] |= 0x0010;
            blocks[2] = 0xCDCD;     /* no AF */
            blocks[3] = ctx->ps[ctx->ps_state*2]<<8 | ctx->ps[ctx->ps_state*2+1];
            ctx->ps_state++;
            if(ctx->ps_state >= 4) ctx->ps_state = 0;
        } else { /* state == 4 (pattern length 5: 4x0A + 1x2A) */
            blocks[1] = 0x2400 | ctx->rt_state;
            blocks[2] = ctx->rt[ctx->rt_state*4+0]<<8 | ctx->rt[ctx->rt_state*4+1];
            blocks[3] = ctx->rt[ctx->rt_state*4+2]<<8 | ctx->rt[ctx->rt_state*4+3];
            ctx->rt_state++;
            if(ctx->rt_state >= 16) ctx->rt_state = 0;
        }

        ctx->group_state++;
        if(ctx->group_state >= 5) ctx->group_state = 0;
    }

    /* Calculate the checkword for each block and emit the bits. */
    for(int i=0; i<GROUP_LENGTH; i++) {
        uint16_t block = blocks[i];
        uint16_t check = rds_crc(block) ^ offset_words[i];
        for(int j=0; j<BLOCK_SIZE; j++) {
            *buffer++ = ((block & (1<<(BLOCK_SIZE-1))) != 0);
            block <<= 1;
        }
        for(int j=0; j<POLY_DEG; j++) {
            *buffer++= ((check & (1<<(POLY_DEG-1))) != 0);
            check <<= 1;
        }
    }
}

/* Get `count` RDS samples. Generates the envelope from pre-computed
 * elementary waveform samples and amplitude-modulates it with a 57 kHz
 * carrier. */
void rds_ctx_get_samples(struct rds_ctx *ctx, float *buffer, int count) {
    for(int i=0; i<count; i++) {
        if(ctx->sample_count >= SAMPLES_PER_BIT) {
            if(ctx->bit_pos >= BITS_PER_GROUP) {
                get_rds_group(ctx, ctx->bit_buffer);
                ctx->bit_pos = 0;
            }

            /* Differential encoding. */
            int cur_bit = ctx->bit_buffer[ctx->bit_pos];
            ctx->prev_output = ctx->cur_output;
            ctx->cur_output  = ctx->prev_output ^ cur_bit;
            ctx->inverting   = (ctx->cur_output == 1);

            const float *src = waveform_biphase;
            int idx = ctx->in_sample_index;

            for(int j=0; j<FILTER_SIZE; j++) {
                float val = (*src++);
                if(ctx->inverting) val = -val;
                ctx->sample_buffer[idx++] += val;
                if(idx >= SAMPLE_BUFFER_SIZE) idx = 0;
            }

            ctx->in_sample_index += SAMPLES_PER_BIT;
            if(ctx->in_sample_index >= SAMPLE_BUFFER_SIZE) ctx->in_sample_index -= SAMPLE_BUFFER_SIZE;

            ctx->bit_pos++;
            ctx->sample_count = 0;
        }

        float sample = ctx->sample_buffer[ctx->out_sample_index];
        ctx->sample_buffer[ctx->out_sample_index] = 0;
        ctx->out_sample_index++;
        if(ctx->out_sample_index >= SAMPLE_BUFFER_SIZE) ctx->out_sample_index = 0;

        /* §2.3: modulate at 57 kHz via a 4-element LUT (228 kHz / 4). */
        sample *= rds_carrier57[ctx->phase];
        ctx->phase = (ctx->phase + 1) & 3;

        *buffer++ = sample;
        ctx->sample_count++;
    }
}

void rds_ctx_set_pi(struct rds_ctx *ctx, uint16_t pi_code) {
    ctx->pi = pi_code;
}

void rds_ctx_set_rt(struct rds_ctx *ctx, const char *rt) {
    rds_fill_string(ctx->rt, rt, RT_LENGTH);
}

void rds_ctx_set_ps(struct rds_ctx *ctx, const char *ps) {
    rds_fill_string(ctx->ps, ps, PS_LENGTH);
}

void rds_ctx_set_ta(struct rds_ctx *ctx, int ta) {
    ctx->ta = ta;
}

rds_ctx_t *rds_ctx_new(void) {
    struct rds_ctx *ctx = calloc(1, sizeof(*ctx));
    if(ctx == NULL) return NULL;
    /* Mirrors the original function-local static initializers. */
    ctx->bit_pos          = BITS_PER_GROUP;
    ctx->sample_count     = SAMPLES_PER_BIT;
    ctx->out_sample_index = SAMPLE_BUFFER_SIZE - 1;
    ctx->latest_minutes   = -1;
    return ctx;
}

void rds_ctx_free(rds_ctx_t **ctx) {
    if(ctx == NULL || *ctx == NULL) return;
    free(*ctx);
    *ctx = NULL;
}

/* --- classic singleton API ---------------------------------------------- */

/* A single process-wide default context used by the legacy API below.
 * Lazily initialized on first access so that calls to `rds_set_*`
 * before `rds_default_ctx()` (or before `main()` in test harnesses)
 * still behave. */
static struct rds_ctx *default_ctx = NULL;

static struct rds_ctx *ensure_default_ctx(void) {
    if(default_ctx == NULL) {
        default_ctx = rds_ctx_new();
    }
    return default_ctx;
}

rds_ctx_t *rds_default_ctx(void) {
    return ensure_default_ctx();
}

void rds_get_samples(float *buffer, int count) {
    rds_ctx_get_samples(ensure_default_ctx(), buffer, count);
}

void rds_set_pi(uint16_t pi_code) {
    rds_ctx_set_pi(ensure_default_ctx(), pi_code);
}

void rds_set_rt(const char *rt) {
    rds_ctx_set_rt(ensure_default_ctx(), rt);
}

void rds_set_ps(const char *ps) {
    rds_ctx_set_ps(ensure_default_ctx(), ps);
}

void rds_set_ta(int ta) {
    rds_ctx_set_ta(ensure_default_ctx(), ta);
}
