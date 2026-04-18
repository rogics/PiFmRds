/*
    PiFmRds - FM/RDS transmitter for the Raspberry Pi
    Copyright (C) 2014 Christophe Jacquet, F8FTK

    See https://github.com/ChristopheJacquet/PiFmRds

    fm_mpx.h: public interface of the FM multiplex (MPX) generator
    implemented in fm_mpx.c.

    Two APIs are exposed:

      1. The classic singleton API (`fm_mpx_open`, `fm_mpx_get_samples`,
         `fm_mpx_close`) kept for backward compatibility with the
         existing CLI and `rds_wav` binary.
      2. A context-based API (`fm_mpx_ctx_*`) that operates on an
         explicitly-passed `fm_mpx_ctx_t *`. Required for tests,
         multi-instance use, and the upcoming producer/consumer thread
         split.

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

#ifndef FM_MPX_H
#define FM_MPX_H

#include <stdlib.h>

#include "pifm_common.h"
#include "rds.h"

/* Opaque handle; definition lives in fm_mpx.c. */
typedef struct fm_mpx_ctx fm_mpx_ctx_t;

/* --- context-based API ---------------------------------------------------
 * `rds` is optional: pass NULL to emit audio only. When non-NULL the
 * context does NOT take ownership of the rds_ctx; caller must outlive
 * the fm_mpx_ctx. */
pifm_status_t fm_mpx_ctx_open(fm_mpx_ctx_t **out, const char *filename,
                              size_t samples_per_call, rds_ctx_t *rds);
pifm_status_t fm_mpx_ctx_get_samples(fm_mpx_ctx_t *ctx, float *mpx_buffer);
pifm_status_t fm_mpx_ctx_close(fm_mpx_ctx_t **ctx);

/* --- classic singleton API (unchanged behaviour) ------------------------ */
pifm_status_t fm_mpx_open(const char *filename, size_t len);
pifm_status_t fm_mpx_get_samples(float *mpx_buffer);
pifm_status_t fm_mpx_close(void);

#endif /* FM_MPX_H */
