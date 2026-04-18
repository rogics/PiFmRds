/*
    PiFmRds - rds_crc_test: regression test for the RDS error-correction
    CRC (generator polynomial 0x1B9, degree 10). The test includes a
    second, bit-by-bit independent reference implementation written
    directly from the polynomial. Both implementations are run on
    the same set of inputs and their outputs must match.

    Rationale: committing hard-coded expected values would not protect
    against an identical bug in both places, but the reference version
    below is deliberately structured differently (MSB-first shift-in)
    to minimise the chance of a shared bug hiding a regression.

    Released under the GPLv3.
*/

#include <stdint.h>
#include <stdio.h>

#include "rds_internal.h"

#define RDS_POLY      0x1B9
#define RDS_POLY_DEG  10

/* Independent bit-by-bit reference. Written from the polynomial
 * x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1 = 0x1B9. Intentionally NOT
 * copied from rds.c so a shared bug is unlikely. */
static uint16_t ref_crc(uint16_t block) {
    uint32_t reg = (uint32_t)block << RDS_POLY_DEG; /* append 10 zero bits */
    for (int i = 25; i >= RDS_POLY_DEG; i--) {
        if (reg & (1u << i)) {
            reg ^= (uint32_t)RDS_POLY << (i - RDS_POLY_DEG);
        }
    }
    return (uint16_t)(reg & ((1u << RDS_POLY_DEG) - 1));
}

/* A handful of inputs spanning several bit patterns and parities. */
static const uint16_t inputs[] = {
    0x0000, 0xFFFF, 0x0001, 0x8000, 0x1234, 0xA5A5,
    0x5A5A, 0xDEAD, 0xBEEF, 0xCAFE, 0xBABE, 0x0100,
    0x7FFF, 0x8001, 0x0F0F, 0xF0F0,
};

int main(void) {
    int failures = 0;

    /* Spec invariant: CRC of 0 is 0. */
    if (rds_crc(0x0000) != 0) {
        fprintf(stderr, "FAIL: rds_crc(0) must be 0, got 0x%03X\n", rds_crc(0));
        failures++;
    }

    for (size_t i = 0; i < sizeof(inputs)/sizeof(inputs[0]); i++) {
        uint16_t got = rds_crc(inputs[i]);
        uint16_t want = ref_crc(inputs[i]);
        if (got != want) {
            fprintf(stderr, "FAIL: rds_crc(0x%04X) = 0x%03X, reference = 0x%03X\n",
                    inputs[i], got, want);
            failures++;
        }
    }

    /* Full 16-bit sweep as a bonus (fast enough to run in CI). */
    for (uint32_t x = 0; x <= 0xFFFFu; x++) {
        if (rds_crc((uint16_t)x) != ref_crc((uint16_t)x)) {
            fprintf(stderr, "FAIL (sweep): rds_crc(0x%04X) != ref_crc(0x%04X)\n",
                    (uint16_t)x, (uint16_t)x);
            failures++;
            break;
        }
    }

    if (failures) {
        fprintf(stderr, "rds_crc_test: %d failure(s).\n", failures);
        return 1;
    }
    printf("rds_crc_test: OK (%zu vectors + 65536-entry sweep).\n",
           sizeof(inputs)/sizeof(inputs[0]));
    return 0;
}
