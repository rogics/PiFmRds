/*
    PiFmRds - FM/RDS transmitter for the Raspberry Pi
    Copyright (C) 2014 Christophe Jacquet, F8FTK

    hw_stub.c: host-only stub implementation of the hw_rpi API. All
    operations are no-ops. Used by `rds_wav` (which writes to a WAV
    file) and by unit tests that need to link against code which would
    otherwise drag in Pi-specific hardware dependencies.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "hw_rpi.h"

struct hw_rpi {
    hw_rpi_cfg_t cfg;
    int cursor;
};

pifm_status_t hw_rpi_init(hw_rpi_t **out, const hw_rpi_cfg_t *cfg) {
    if (out == NULL || cfg == NULL) return PIFM_ERR_ARG;
    struct hw_rpi *hw = calloc(1, sizeof(*hw));
    if (hw == NULL) return PIFM_ERR_MEM;
    hw->cfg = *cfg;
    *out = hw;
    return PIFM_OK;
}

pifm_status_t hw_rpi_start(hw_rpi_t *hw) { (void)hw; return PIFM_OK; }
void hw_rpi_stop(hw_rpi_t *hw) { (void)hw; }
void hw_rpi_reset_dma(hw_rpi_t *hw) { (void)hw; }

void hw_rpi_destroy(hw_rpi_t **hw_pp) {
    if (hw_pp == NULL || *hw_pp == NULL) return;
    free(*hw_pp);
    *hw_pp = NULL;
}

/* Stub: claim the full ring is always free so the producer never blocks. */
int hw_rpi_free_slots(hw_rpi_t *hw) {
    if (hw == NULL) return -1;
    return hw->cfg.num_samples;
}

pifm_status_t hw_rpi_push_deltas(hw_rpi_t *hw, const int *deltas, size_t n) {
    (void)hw; (void)deltas; (void)n;
    return PIFM_OK;
}

void hw_rpi_wait_space(hw_rpi_t *hw, int min_slots) { (void)hw; (void)min_slots; }
