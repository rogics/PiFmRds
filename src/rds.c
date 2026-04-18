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

#include "rds_strings.h"
#include "waveforms.h"
#include "pifm_common.h"

#define GROUP_LENGTH 4

static struct {
    uint16_t pi;
    int ta;
    char ps[PS_LENGTH];
    char rt[RT_LENGTH];
} rds_params = { 0 };
/* Here, the first member of the struct must be a scalar to avoid a
   warning on -Wmissing-braces with GCC < 4.8.3 
   (bug: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=53119)
*/

/* The RDS error-detection code generator polynomial is
   x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + x^0
*/
#define POLY 0x1B9
#define POLY_DEG 10
#define MSB_BIT 0x8000
#define BLOCK_SIZE 16

#define BITS_PER_GROUP (GROUP_LENGTH * (BLOCK_SIZE+POLY_DEG))
#define SAMPLES_PER_BIT 192
/* Cast to int so the loop counters in rds_get_samples() (also int)
 * don't provoke -Wsign-compare. sizeof(...) is size_t and would
 * otherwise promote the comparison. Both values fit in int by a wide
 * margin (FILTER_SIZE == 576, SAMPLE_BUFFER_SIZE == 768). */
#define FILTER_SIZE ((int)(sizeof(waveform_biphase)/sizeof(float)))
#define SAMPLE_BUFFER_SIZE (SAMPLES_PER_BIT + FILTER_SIZE)


static const uint16_t offset_words[] = {0x0FC, 0x198, 0x168, 0x1B4};
// We don't handle offset word C' here for the sake of simplicity

/* Classical CRC computation */
static uint16_t crc(uint16_t block) {
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
    
    return crc;
}

/* Possibly generates a CT (clock time) group if the minute has just changed
   Returns 1 if the CT group was generated, 0 otherwise
*/
static int get_rds_ct_group(uint16_t *blocks) {
    static int latest_minutes = -1;

    // Check time
    time_t now;
    struct tm *utc;
    
    now = time (NULL);
    utc = gmtime (&now);

    if(utc->tm_min != latest_minutes) {
        // Generate CT group
        latest_minutes = utc->tm_min;
        
        /* §1.11: RDS MJD per EN 50067, with the usual "shift Jan/Feb
         * back into the previous year" trick (l == 1 for those two
         * months). The original form did (y * 365.25) and
         * (m * 30.6001) as double and truncated to int; under
         * -ffast-math the compiler is free to reorder / fuse those
         * operations, which can produce an off-by-one on boundary
         * dates. Rewriting with pure integer arithmetic (1461/4 ==
         * 365.25 exactly, 306001/10000 == 30.6001 exactly for the
         * range of m we care about) removes that risk. */
        int l = utc->tm_mon <= 1 ? 1 : 0;
        int y = utc->tm_year - l;
        int m = utc->tm_mon + 2 + l*12;
        int mjd = 14956 + utc->tm_mday + (y * 1461) / 4 + (m * 306001) / 10000;

        blocks[1] = 0x4400 | (mjd>>15);
        blocks[2] = (mjd<<1) | (utc->tm_hour>>4);
        blocks[3] = (utc->tm_hour & 0xF)<<12 | utc->tm_min<<6;

        /* §1.10: tm_gmtoff is a BSD/glibc extension. Fall back to
         * recomputing the offset from timegm() if it isn't available
         * (e.g. some uClibc/musl configurations, strict-POSIX builds).
         * Either way we report the offset in half-hour steps as
         * required by the RDS CT group. */
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
        /* §4.8: abs(INT_MIN) is UB; split the sign-handling explicitly. */
        if (offset < 0) {
            blocks[3] |= 0x20;
            blocks[3] |= (unsigned)(-offset) & 0x1F;
        } else {
            blocks[3] |= (unsigned)offset & 0x1F;
        }
        
        //printf("Generated CT: %04X %04X %04X\n", blocks[1], blocks[2], blocks[3]);
        return 1;
    } else return 0;
}

/* Creates an RDS group. This generates sequences of the form 0A, 0A, 0A, 0A, 2A, etc.
   The pattern is of length 5, the variable 'state' keeps track of where we are in the
   pattern. 'ps_state' and 'rt_state' keep track of where we are in the PS (0A) sequence
   or RT (2A) sequence, respectively.
*/
static void get_rds_group(int *buffer) {
    static int state = 0;
    static int ps_state = 0;
    static int rt_state = 0;
    uint16_t blocks[GROUP_LENGTH] = {rds_params.pi, 0, 0, 0};
    
    // Generate block content
    if(! get_rds_ct_group(blocks)) { // CT (clock time) has priority on other group types
        if(state < 4) {
            blocks[1] = 0x0400 | ps_state;
            if(rds_params.ta) blocks[1] |= 0x0010;
            blocks[2] = 0xCDCD;     // no AF
            blocks[3] = rds_params.ps[ps_state*2]<<8 | rds_params.ps[ps_state*2+1];
            ps_state++;
            if(ps_state >= 4) ps_state = 0;
        } else { // state == 4 (pattern length 5: 4x0A + 1x2A, wraps at 5)
            blocks[1] = 0x2400 | rt_state;
            blocks[2] = rds_params.rt[rt_state*4+0]<<8 | rds_params.rt[rt_state*4+1];
            blocks[3] = rds_params.rt[rt_state*4+2]<<8 | rds_params.rt[rt_state*4+3];
            rt_state++;
            if(rt_state >= 16) rt_state = 0;
        }
    
        state++;
        if(state >= 5) state = 0;
    }
    
    // Calculate the checkword for each block and emit the bits
    for(int i=0; i<GROUP_LENGTH; i++) {
        uint16_t block = blocks[i];
        uint16_t check = crc(block) ^ offset_words[i];
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

/* Get a number of RDS samples. This generates the envelope of the waveform using
   pre-generated elementary waveform samples, and then it amplitude-modulates the 
   envelope with a 57 kHz carrier, which is very efficient as 57 kHz is 4 times the
   sample frequency we are working at (228 kHz).
 */
void rds_get_samples(float *buffer, int count) {
    static int bit_buffer[BITS_PER_GROUP];
    static int bit_pos = BITS_PER_GROUP;
    static float sample_buffer[SAMPLE_BUFFER_SIZE] = {0};
    
    static int prev_output = 0;
    static int cur_output = 0;
    static int cur_bit = 0;
    static int sample_count = SAMPLES_PER_BIT;
    static int inverting = 0;
    static int phase = 0;

    static int in_sample_index = 0;
    static int out_sample_index = SAMPLE_BUFFER_SIZE-1;
        
    for(int i=0; i<count; i++) {
        if(sample_count >= SAMPLES_PER_BIT) {
            if(bit_pos >= BITS_PER_GROUP) {
                get_rds_group(bit_buffer);
                bit_pos = 0;
            }
            
            // do differential encoding
            cur_bit = bit_buffer[bit_pos];
            prev_output = cur_output;
            cur_output = prev_output ^ cur_bit;
            
            inverting = (cur_output == 1);

            const float *src = waveform_biphase;
            int idx = in_sample_index;

            for(int j=0; j<FILTER_SIZE; j++) {
                float val = (*src++);
                if(inverting) val = -val;
                sample_buffer[idx++] += val;
                if(idx >= SAMPLE_BUFFER_SIZE) idx = 0;
            }

            in_sample_index += SAMPLES_PER_BIT;
            if(in_sample_index >= SAMPLE_BUFFER_SIZE) in_sample_index -= SAMPLE_BUFFER_SIZE;
            
            bit_pos++;
            sample_count = 0;
        }
        
        float sample = sample_buffer[out_sample_index];
        sample_buffer[out_sample_index] = 0;
        out_sample_index++;
        if(out_sample_index >= SAMPLE_BUFFER_SIZE) out_sample_index = 0;
        
        
        // modulate at 57 kHz
        // use phase for this
        switch(phase) {
            case 0:
            case 2: sample = 0; break;
            case 1: break;
            case 3: sample = -sample; break;
        }
        phase++;
        if(phase >= 4) phase = 0;
        
        *buffer++ = sample;
        sample_count++;
    }
}

void rds_set_pi(uint16_t pi_code) {
    rds_params.pi = pi_code;
}

void rds_set_rt(const char *rt) {
    rds_fill_string(rds_params.rt, rt, RT_LENGTH);
}

void rds_set_ps(const char *ps) {
    rds_fill_string(rds_params.ps, ps, PS_LENGTH);
}

void rds_set_ta(int ta) {
    rds_params.ta = ta;
}