/*
    PiFmRds - mjd_test: regression test for the Modified Julian Date
    helper used by the RDS clock-time (CT) group.

    MJD definition: the number of days since 1858-11-17. Known
    anchor points are used as golden vectors, including boundary
    cases around leap years and year transitions.

    Released under the GPLv3.
*/

#include <stdio.h>

#include "rds_internal.h"

/* struct tm convention: year = years since 1900, mon = 0..11, mday = 1..31. */
struct d { int y_1900; int mon0; int mday; int expected_mjd; const char *label; };

/* Anchor values. Cross-checked against Wikipedia's Julian day table
 * and the NASA/GSFC date converter. */
static const struct d vectors[] = {
    /* MJD epoch itself: 1858-11-17 = MJD 0. tm_year = 1858 - 1900 = -42. */
    { -42, 10, 17, 0,     "epoch 1858-11-17"  },
    /* 2000-01-01 = MJD 51544. Classic landmark. */
    { 100,  0,  1, 51544, "2000-01-01"        },
    /* 2020-02-29 = MJD 58908. Leap-day-in-leap-century. */
    { 120,  1, 29, 58908, "2020-02-29"        },
    /* 2020-03-01 = MJD 58909. Day after leap-day. */
    { 120,  2,  1, 58909, "2020-03-01"        },
    /* 2021-01-01 = MJD 59215. Year boundary out of a leap year. */
    { 121,  0,  1, 59215, "2021-01-01"        },
    /* 2024-02-29 = MJD 60369. Another leap day. */
    { 124,  1, 29, 60369, "2024-02-29"        },
    /* 2024-12-31 = MJD 60675. Last day of a leap year. */
    { 124, 11, 31, 60675, "2024-12-31"        },
    /* 1970-01-01 = MJD 40587. Unix epoch. */
    {  70,  0,  1, 40587, "1970-01-01"        },
    /* 1900-03-01 = MJD 15079. Post-century-non-leap (1900 is NOT a leap year). */
    {   0,  2,  1, 15079, "1900-03-01"        },
};

int main(void) {
    int failures = 0;
    for (size_t i = 0; i < sizeof(vectors)/sizeof(vectors[0]); i++) {
        int got = rds_mjd(vectors[i].y_1900, vectors[i].mon0, vectors[i].mday);
        if (got != vectors[i].expected_mjd) {
            fprintf(stderr, "FAIL: %s -> rds_mjd = %d, expected %d\n",
                    vectors[i].label, got, vectors[i].expected_mjd);
            failures++;
        }
    }

    /* Additional invariant: consecutive days differ by exactly 1, and
     * 365 / 366 days in a year depending on leap-ness. Iterate through
     * 2019 (non-leap) and 2020 (leap). */
    for (int test_year = 119; test_year <= 120; test_year++) {
        int expected_days = (test_year == 120) ? 366 : 365;
        int start = rds_mjd(test_year, 0, 1);
        int end   = rds_mjd(test_year + 1, 0, 1);
        if (end - start != expected_days) {
            fprintf(stderr, "FAIL: year %d has %d days, rds_mjd says %d\n",
                    test_year + 1900, expected_days, end - start);
            failures++;
        }
    }

    if (failures) {
        fprintf(stderr, "mjd_test: %d failure(s).\n", failures);
        return 1;
    }
    printf("mjd_test: OK.\n");
    return 0;
}
