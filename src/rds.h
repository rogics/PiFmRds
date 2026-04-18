/*
    PiFmRds - FM/RDS transmitter for the Raspberry Pi
    Copyright (C) 2014 Christophe Jacquet, F8FTK

    See https://github.com/ChristopheJacquet/PiFmRds

    rds.h: public interface of the RDS baseband generator.

    Two APIs are exposed: a classic singleton API kept for CLI
    compatibility, and a context-based API (`rds_ctx_*`) that operates
    on an explicit `rds_ctx_t *` handle and is required for tests and
    multi-instance use.

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

#ifndef RDS_H
#define RDS_H

#include <stdint.h>

/* Opaque handle; definition lives in rds.c. */
typedef struct rds_ctx rds_ctx_t;

/* --- context-based API -------------------------------------------------- */
rds_ctx_t *rds_ctx_new(void);
void       rds_ctx_free(rds_ctx_t **ctx);
void       rds_ctx_get_samples(rds_ctx_t *ctx, float *buffer, int count);
void       rds_ctx_set_pi(rds_ctx_t *ctx, uint16_t pi_code);
void       rds_ctx_set_rt(rds_ctx_t *ctx, const char *rt);
void       rds_ctx_set_ps(rds_ctx_t *ctx, const char *ps);
void       rds_ctx_set_ta(rds_ctx_t *ctx, int ta);

/* --- classic singleton API (unchanged behaviour) ----------------------- */
void rds_get_samples(float *buffer, int count);
void rds_set_pi(uint16_t pi_code);
void rds_set_rt(const char *rt);
void rds_set_ps(const char *ps);
void rds_set_ta(int ta);

/* Accessor used by fm_mpx to chain the default RDS singleton into the
 * default MPX singleton (needed because the old CLI path calls
 * rds_set_* before fm_mpx_open). */
rds_ctx_t *rds_default_ctx(void);

#endif /* RDS_H */
