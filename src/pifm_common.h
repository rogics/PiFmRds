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

#endif /* PIFM_COMMON_H */
