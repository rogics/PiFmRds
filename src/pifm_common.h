/*
    PiFmRds - FM/RDS transmitter for the Raspberry Pi
    Copyright (C) 2014 Christophe Jacquet, F8FTK

    See https://github.com/ChristopheJacquet/PiFmRds

    pifm_common.h: project-wide constants that would otherwise be duplicated
    across translation units. New code should prefer the names defined here
    over bare numeric literals.

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

#ifndef PIFM_COMMON_H
#define PIFM_COMMON_H

/* Unified status / return code enum for the public C API.
 *
 * Functions that previously returned ad-hoc 0/-1 integers now return
 * pifm_status_t. Negative values are errors, PIFM_OK is success, and
 * positive values are control-pipe "event" codes (see the
 * CONTROL_PIPE_* compatibility aliases in control_pipe.h).
 *
 * Compatibility note: existing call sites compare against 0 and
 * negative literals, which keeps working because of the chosen
 * numeric values. */
typedef enum {
    PIFM_OK             =  0,
    PIFM_ERR_IO         = -1,
    PIFM_ERR_MEM        = -2,
    PIFM_ERR_HW         = -3,
    PIFM_ERR_ARG        = -4,
    PIFM_PIPE_NO_CMD    =  0, /* alias of PIFM_OK for control_pipe_poll */
    PIFM_PIPE_PS_SET    =  1,
    PIFM_PIPE_RT_SET    =  2,
    PIFM_PIPE_TA_SET    =  3,
} pifm_status_t;

/* RDS Programme Service: exactly 8 characters on air (IEC 62106 1.5.1). */
#define PS_LENGTH       8
/* RDS RadioText: up to 64 characters on air (IEC 62106 3.1.5.3). */
#define RT_LENGTH       64

/* Buffer sizes (= on-air length plus a trailing NUL). Use these when
 * allocating storage for C-strings that hold PS/RT content. */
#define PS_BUF_SIZE     (PS_LENGTH + 1)
#define RT_BUF_SIZE     (RT_LENGTH + 1)

/* FM multiplex sample rate in Hz. The whole DSP pipeline runs at this
 * rate; 57 kHz (RDS subcarrier) = MPX_SAMPLE_RATE / 4, which is exploited
 * in rds.c for efficient modulation. */
#define MPX_SAMPLE_RATE 228000

/* Audio filter cutoff: default low-pass cutoff for the input audio
 * (slightly below the nominal 15 kHz to allow for filter slope). */
#define AUDIO_LPF_CUTOFF_HZ 15000
#define AUDIO_LPF_CUTOFF_FACTOR 0.8f

/* BCM2835 clock manager password.  Every write to a CM register must
 * OR in this constant or the hardware silently ignores the write.
 * See BCM2835 ARM peripherals datasheet section 6.3. */
#define CM_PASSWD               (0x5A << 24)

/* MPX mixing gains. These are the amplitude scalars applied to the
 * individual signal components before they are summed into the MPX
 * multiplex, chosen so the peak combined amplitude does not clip. */
#define MPX_AUDIO_GAIN          4.05f   /* mono (or L+R) audio */
#define MPX_STEREO_DIFF_GAIN    4.05f   /* stereo (L-R) audio */
#define MPX_PILOT_GAIN          0.9f    /* 19 kHz stereo pilot */

/* RDS group multiplex ratio: the RDS BPSK envelope is scaled by
 * 1/10 when mixed into the MPX, and the final MPX sample is divided
 * by 10 before DMA -- MPX_SCALE_DIV is that divisor. */
#define MPX_SCALE_DIV           10.0f

#endif /* PIFM_COMMON_H */
