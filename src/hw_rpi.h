/*
    PiFmRds - FM/RDS transmitter for the Raspberry Pi
    Copyright (C) 2014, 2015 Christophe Jacquet, F8FTK
    Copyright (C) 2012, 2015 Richard Hirst

    hw_rpi.h: Raspberry Pi DMA / PWM / GPCLK driver interface.

    This header isolates all Pi-specific hardware access behind an
    opaque handle so the rest of the codebase (DSP library, CLI, tests)
    is portable. A matching stub implementation (hw_stub.c) exists for
    host builds where the mailbox device is absent.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef HW_RPI_H
#define HW_RPI_H

#include <stddef.h>
#include <stdint.h>

#include "pifm_common.h"

typedef struct hw_rpi hw_rpi_t;

/* Driver configuration. All fields are inputs. */
typedef struct {
    uint32_t carrier_freq_hz;  /* FM carrier, typically 76e6..108e6 */
    float    ppm;              /* crystal error correction */
    int      num_samples;      /* size of the DMA sample ring; default 50000 */
} hw_rpi_cfg_t;

/* Lifecycle */
pifm_status_t hw_rpi_init(hw_rpi_t **out, const hw_rpi_cfg_t *cfg);
pifm_status_t hw_rpi_start(hw_rpi_t *hw);
void          hw_rpi_stop(hw_rpi_t *hw);
void          hw_rpi_destroy(hw_rpi_t **hw);

/* Force the DMA engine into the reset state. Safe to call from a
 * signal handler; performs only a single MMIO write plus a short
 * delay. Intended as a last-ditch cleanup from the watchdog. */
void          hw_rpi_reset_dma(hw_rpi_t *hw);

/* Returns the number of sample slots the producer can write to without
 * racing the DMA consumer, or -1 if the DMA cursor is momentarily
 * unreadable (caller should retry). */
int           hw_rpi_free_slots(hw_rpi_t *hw);

/* Write `n` frequency deltas (in units of the fractional-divider LSB)
 * into the next `n` DMA slots. `n` must be <= hw_rpi_free_slots(). */
pifm_status_t hw_rpi_push_deltas(hw_rpi_t *hw, const int *deltas, size_t n);

#endif /* HW_RPI_H */
