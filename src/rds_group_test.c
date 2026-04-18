/*
    PiFmRds - rds_group_test: smoke test for rds_ctx_get_samples.

    We don't attempt to compare the full 104-bit group output against
    a committed vector here (the RDS CT group fires on minute changes
    and would make the output time-dependent). Instead we verify:

      1. Determinism: two freshly-initialised contexts driven with the
         same PI/PS/RT produce bit-exact output, provided we run them
         short enough to stay inside a single UTC minute.
      2. Non-triviality: the output is not all-zero -- rules out a
         latent bug where rds_get_samples accidentally writes nothing.

    Released under the GPLv3.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rds.h"

/* One RDS group = 104 bits. At 228 kHz MPX with 192 samples per bit,
 * that's 104 * 192 = 19968 samples per group. We grab ~1 group. */
#define NUM_SAMPLES 19968

int main(void) {
    rds_ctx_t *a = rds_ctx_new();
    rds_ctx_t *b = rds_ctx_new();
    if (a == NULL || b == NULL) {
        fprintf(stderr, "rds_ctx_new failed\n");
        return 1;
    }

    rds_ctx_set_pi(a, 0x1234);
    rds_ctx_set_ps(a, "TEST");
    rds_ctx_set_rt(a, "Test RadioText");
    rds_ctx_set_pi(b, 0x1234);
    rds_ctx_set_ps(b, "TEST");
    rds_ctx_set_rt(b, "Test RadioText");

    float *buf_a = malloc(sizeof(float) * NUM_SAMPLES);
    float *buf_b = malloc(sizeof(float) * NUM_SAMPLES);
    if (buf_a == NULL || buf_b == NULL) {
        fprintf(stderr, "OOM\n"); return 1;
    }

    /* Try to stay within a single UTC minute so the CT group firing
     * doesn't vary between the two runs. If we straddle a minute we
     * skip the determinism check -- the non-triviality assertion still
     * catches a broken generator. */
    time_t t0 = time(NULL);
    rds_ctx_get_samples(a, buf_a, NUM_SAMPLES);
    rds_ctx_get_samples(b, buf_b, NUM_SAMPLES);
    time_t t1 = time(NULL);

    int failures = 0;

    /* Non-triviality: at least one sample must be non-zero. */
    int any_nonzero = 0;
    for (int i = 0; i < NUM_SAMPLES; i++) {
        if (buf_a[i] != 0.0f) { any_nonzero = 1; break; }
    }
    if (!any_nonzero) {
        fprintf(stderr, "FAIL: output is all-zero\n");
        failures++;
    }

    /* Determinism (only if we stayed within one UTC minute). */
    struct tm *t0_tm = gmtime(&t0);
    int t0_min = t0_tm->tm_min;
    struct tm *t1_tm = gmtime(&t1);
    int t1_min = t1_tm->tm_min;
    if (t0_min == t1_min) {
        if (memcmp(buf_a, buf_b, sizeof(float) * NUM_SAMPLES) != 0) {
            fprintf(stderr, "FAIL: two identically-configured contexts produced "
                    "divergent output within a single UTC minute\n");
            failures++;
        }
    } else {
        printf("rds_group_test: skipped determinism check "
               "(UTC minute changed during test)\n");
    }

    free(buf_a);
    free(buf_b);
    rds_ctx_free(&a);
    rds_ctx_free(&b);

    if (failures) {
        fprintf(stderr, "rds_group_test: %d failure(s).\n", failures);
        return 1;
    }
    printf("rds_group_test: OK.\n");
    return 0;
}
