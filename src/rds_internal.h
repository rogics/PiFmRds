/*
    PiFmRds - FM/RDS transmitter for the Raspberry Pi
    Copyright (C) 2014 Christophe Jacquet, F8FTK

    rds_internal.h: test-only access to otherwise-static helpers in
    rds.c. NOT a public API -- consumers outside the tests should use
    rds.h.

    Released under the GPLv3.
*/

#ifndef RDS_INTERNAL_H
#define RDS_INTERNAL_H

#include <stdint.h>

/* RDS error-correction CRC (generator polynomial 0x1B9, degree 10).
 * Input: 16-bit block (top-bit aligned). Output: 10-bit checkword in
 * the low bits of a uint16_t. Pure function, no state. */
uint16_t rds_crc(uint16_t block);

/* Modified Julian Date (per EN 50067, 6.1.5.2 / 4.4.4.4). Inputs are
 * in the same convention as `struct tm`: year = years since 1900,
 * month0 = 0..11, day = 1..31. */
int rds_mjd(int tm_year, int tm_mon, int tm_mday);

#endif /* RDS_INTERNAL_H */
