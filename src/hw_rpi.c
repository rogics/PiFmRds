/*
    PiFmRds - FM/RDS transmitter for the Raspberry Pi
    Copyright (C) 2014, 2015 Christophe Jacquet, F8FTK
    Copyright (C) 2012, 2015 Richard Hirst
    Copyright (C) 2012 Oliver Mattos and Oskar Weigl

    hw_rpi.c: Raspberry Pi DMA / PWM / GPCLK driver implementation.
    Extracted from pi_fm_rds.c so the DSP library and CLI can be built
    and tested without any hardware dependency.

    See https://github.com/ChristopheJacquet/PiFmRds

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "hw_rpi.h"
#include "mailbox.h"
#include "pifm_common.h"

#if (RASPI)==1
#define PERIPH_VIRT_BASE 0x20000000
#define PERIPH_PHYS_BASE 0x7e000000
#define DRAM_PHYS_BASE 0x40000000
#define MEM_FLAG 0x0c
#define PLLFREQ 500000000.
#elif (RASPI)==2
#define PERIPH_VIRT_BASE 0x3f000000
#define PERIPH_PHYS_BASE 0x7e000000
#define DRAM_PHYS_BASE 0xc0000000
#define MEM_FLAG 0x04
#define PLLFREQ 500000000.
#elif (RASPI)==4
#define PERIPH_VIRT_BASE 0xfe000000
#define PERIPH_PHYS_BASE 0x7e000000
#define DRAM_PHYS_BASE 0xc0000000
#define MEM_FLAG 0x04
#define PLLFREQ 750000000.
#else
#error Unknown Raspberry Pi version (variable RASPI)
#endif

/* DMA control register (CS) flags. See BCM2835 ARM Peripherals
 * section 4.2.1 ("DMA Control and Status register"). */
#define BCM2708_DMA_NO_WIDE_BURSTS    (1<<26)
#define BCM2708_DMA_WAIT_RESP         (1<<3)
#define BCM2708_DMA_D_DREQ            (1<<6)
#define BCM2708_DMA_PER_MAP(x)        ((x)<<16)
#define BCM2708_DMA_END               (1<<1)
#define BCM2708_DMA_RESET             (1<<31)
#define BCM2708_DMA_INT               (1<<2)

#define BCM2708_DMA_PRIORITY(x)       ((x)<<16)
#define BCM2708_DMA_PANIC_PRIORITY(x) ((x)<<20)
#define BCM2708_DMA_WAIT_FOR_OUTSTANDING_WRITES  (1<<28)
#define BCM2708_DMA_ACTIVE            (1<<0)

#define DMA_CS_GO                                                    \
    (BCM2708_DMA_WAIT_FOR_OUTSTANDING_WRITES |                       \
     BCM2708_DMA_PANIC_PRIORITY(8) |                                 \
     BCM2708_DMA_PRIORITY(8) |                                       \
     BCM2708_DMA_ACTIVE)

#define DMA_CS             (0x00/4)
#define DMA_CONBLK_AD      (0x04/4)
#define DMA_DEBUG          (0x20/4)

#define DMA_BASE_OFFSET    0x00007000
#define DMA_LEN            0x24
#define PWM_BASE_OFFSET    0x0020C000
#define PWM_LEN            0x28
#define CLK_BASE_OFFSET    0x00101000
#define CLK_LEN            0xA8
#define GPIO_BASE_OFFSET   0x00200000
#define GPIO_LEN           0x100

#define DMA_VIRT_BASE      (PERIPH_VIRT_BASE + DMA_BASE_OFFSET)
#define PWM_VIRT_BASE      (PERIPH_VIRT_BASE + PWM_BASE_OFFSET)
#define CLK_VIRT_BASE      (PERIPH_VIRT_BASE + CLK_BASE_OFFSET)
#define GPIO_VIRT_BASE     (PERIPH_VIRT_BASE + GPIO_BASE_OFFSET)

#define PWM_PHYS_BASE      (PERIPH_PHYS_BASE + PWM_BASE_OFFSET)

#define PWM_CTL            (0x00/4)
#define PWM_DMAC           (0x08/4)
#define PWM_RNG1           (0x10/4)
#define PWM_FIFO           (0x18/4)

#define PWMCLK_CNTL        40
#define PWMCLK_DIV         41

#define CM_GP0DIV          (0x7e101074)

#define GPCLK_CNTL         (0x70/4)
#define GPCLK_DIV          (0x74/4)

#define PWMCTL_MODE1       (1<<1)
#define PWMCTL_PWEN1       (1<<0)
#define PWMCTL_CLRF        (1<<6)
#define PWMCTL_USEF1       (1<<5)

#define PWMDMAC_ENAB       (1<<31)
#define PWMDMAC_THRSHLD    ((15<<8)|(15<<0))

#define GPFSEL0            (0x00/4)

#define BUS_TO_PHYS(x)     ((x) & ~0xC0000000)

#define PAGE_SIZE          4096
#define PAGE_SHIFT         12

typedef struct {
    uint32_t info, src, dst, length, stride, next, pad[2];
} dma_cb_t;

struct hw_rpi {
    hw_rpi_cfg_t cfg;
    size_t num_cbs;
    size_t num_pages;

    /* Peripherals */
    volatile uint32_t *pwm_reg;
    volatile uint32_t *clk_reg;
    volatile uint32_t *dma_reg;
    volatile uint32_t *gpio_reg;

    /* Mailbox-allocated uncached physical ring */
    struct mbox_s {
        int      handle;
        unsigned mem_ref;
        unsigned bus_addr;
        uint8_t *virt_addr;
    } mbox;

    /* Control-block / sample ring pointers */
    dma_cb_t *cb;
    uint32_t *sample;

    /* Composed 0x5A-prefixed divider word for the carrier. The
     * per-sample CM_GP0DIV write is (freq_ctl_word + delta). */
    uint32_t freq_ctl_word;

    /* Producer cursor: the virtual address of the last control block
     * we wrote to. Used to compute free_slots against the DMA cursor. */
    size_t   last_cb_virt;

    int      started;
};

static void udelay(int us) {
    struct timespec ts = { 0, us * 1000 };
    nanosleep(&ts, NULL);
}

static void *map_peripheral(uint32_t base, uint32_t len) {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "hw_rpi: open(/dev/mem) failed: %s\n", strerror(errno));
        return NULL;
    }
    void *vaddr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
    close(fd);
    if (vaddr == MAP_FAILED) {
        fprintf(stderr, "hw_rpi: mmap of 0x%08x failed: %s\n", base, strerror(errno));
        return NULL;
    }
    return vaddr;
}

static size_t mem_virt_to_phys(struct hw_rpi *hw, void *virt) {
    size_t offset = (size_t)virt - (size_t)hw->mbox.virt_addr;
    return hw->mbox.bus_addr + offset;
}

static size_t mem_phys_to_virt(struct hw_rpi *hw, size_t phys) {
    return (size_t)(phys - hw->mbox.bus_addr + (size_t)hw->mbox.virt_addr);
}

pifm_status_t hw_rpi_init(hw_rpi_t **out, const hw_rpi_cfg_t *cfg) {
    if (out == NULL || cfg == NULL) return PIFM_ERR_ARG;
    *out = NULL;

    struct hw_rpi *hw = calloc(1, sizeof(*hw));
    if (hw == NULL) return PIFM_ERR_MEM;
    hw->cfg       = *cfg;
    hw->num_cbs   = (size_t)cfg->num_samples * 2;
    size_t bytes  = hw->num_cbs * sizeof(dma_cb_t)
                  + (size_t)cfg->num_samples * sizeof(uint32_t);
    hw->num_pages = (bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;

    hw->dma_reg  = map_peripheral(DMA_VIRT_BASE,  DMA_LEN);
    hw->pwm_reg  = map_peripheral(PWM_VIRT_BASE,  PWM_LEN);
    hw->clk_reg  = map_peripheral(CLK_VIRT_BASE,  CLK_LEN);
    hw->gpio_reg = map_peripheral(GPIO_VIRT_BASE, GPIO_LEN);
    if (!hw->dma_reg || !hw->pwm_reg || !hw->clk_reg || !hw->gpio_reg) {
        hw_rpi_destroy(&hw);
        *out = NULL;
        return PIFM_ERR_HW;
    }

    hw->mbox.handle = mbox_open();
    if (hw->mbox.handle < 0) {
        fprintf(stderr, "hw_rpi: mbox_open() failed; check /dev/vcio support.\n");
        hw_rpi_destroy(&hw);
        return PIFM_ERR_HW;
    }

    printf("Allocating physical memory: size = %zu     ", hw->num_pages * PAGE_SIZE);
    hw->mbox.mem_ref = mbox_mem_alloc(hw->mbox.handle,
                                      hw->num_pages * PAGE_SIZE,
                                      PAGE_SIZE, MEM_FLAG);
    if (hw->mbox.mem_ref == 0) {
        fprintf(stderr, "Could not allocate memory.\n");
        hw_rpi_destroy(&hw);
        return PIFM_ERR_MEM;
    }
    printf("mem_ref = %u     ", hw->mbox.mem_ref);

    hw->mbox.bus_addr = mbox_mem_lock(hw->mbox.handle, hw->mbox.mem_ref);
    if (hw->mbox.bus_addr == 0) {
        fprintf(stderr, "Could not lock memory.\n");
        hw_rpi_destroy(&hw);
        return PIFM_ERR_MEM;
    }
    printf("bus_addr = %x     ", hw->mbox.bus_addr);

    hw->mbox.virt_addr = mapmem(BUS_TO_PHYS(hw->mbox.bus_addr),
                                hw->num_pages * PAGE_SIZE);
    if (hw->mbox.virt_addr == NULL) {
        fprintf(stderr, "Could not map memory.\n");
        hw_rpi_destroy(&hw);
        return PIFM_ERR_MEM;
    }
    printf("virt_addr = %p\n", hw->mbox.virt_addr);

    hw->cb     = (dma_cb_t *)hw->mbox.virt_addr;
    hw->sample = (uint32_t *)(hw->cb + hw->num_cbs);

    /* Compute the frequency control word (20 bits integer | 12 bits
     * fractional) with full double precision.  See §1.3. */
    double   pll_ratio = PLLFREQ / (double)hw->cfg.carrier_freq_hz;
    uint32_t idiv      = (uint32_t)pll_ratio;
    uint32_t fdiv      = (uint32_t)((pll_ratio - idiv) * 4096.0);
    uint32_t freq_ctl  = (idiv << 12) | (fdiv & 0xFFF);
    hw->freq_ctl_word  = CM_PASSWD | freq_ctl;

    *out = hw;
    return PIFM_OK;
}

pifm_status_t hw_rpi_start(hw_rpi_t *hw) {
    if (hw == NULL) return PIFM_ERR_ARG;

    /* GPIO4 -> ALT FUNC 0 to output the clock. */
    hw->gpio_reg[GPFSEL0] = (hw->gpio_reg[GPFSEL0] & ~(7 << 12)) | (4 << 12);

    /* Program GPCLK to use MASH setting 1. */
    hw->clk_reg[GPCLK_CNTL] = CM_PASSWD | 6;
    udelay(100);
    hw->clk_reg[GPCLK_CNTL] = CM_PASSWD | 1 << 9 | 1 << 4 | 6;

    /* Build the circular DMA chain: each sample is served by two
     * control blocks -- one writes the frequency word to CM_GP0DIV,
     * one paces via the PWM FIFO DREQ. */
    dma_cb_t *cbp = hw->cb;
    uint32_t phys_sample_dst   = CM_GP0DIV;
    uint32_t phys_pwm_fifo_dst = PWM_PHYS_BASE + 0x18;

    for (int i = 0; i < hw->cfg.num_samples; i++) {
        hw->sample[i] = hw->freq_ctl_word; /* silence = plain carrier */

        /* CB1: write the frequency word. */
        cbp->info   = BCM2708_DMA_NO_WIDE_BURSTS | BCM2708_DMA_WAIT_RESP;
        cbp->src    = mem_virt_to_phys(hw, hw->sample + i);
        cbp->dst    = phys_sample_dst;
        cbp->length = 4;
        cbp->stride = 0;
        cbp->next   = mem_virt_to_phys(hw, cbp + 1);
        cbp++;

        /* CB2: pace via PWM FIFO DREQ. */
        cbp->info = BCM2708_DMA_NO_WIDE_BURSTS | BCM2708_DMA_WAIT_RESP
                  | BCM2708_DMA_D_DREQ | BCM2708_DMA_PER_MAP(5);
        cbp->src    = mem_virt_to_phys(hw, hw->mbox.virt_addr);
        cbp->dst    = phys_pwm_fifo_dst;
        cbp->length = 4;
        cbp->stride = 0;
        cbp->next   = mem_virt_to_phys(hw, cbp + 1);
        cbp++;
    }
    cbp--;
    cbp->next = mem_virt_to_phys(hw, hw->mbox.virt_addr);

    /* PWM clock divider programs the 228 kHz DREQ. */
    float    divider  = (float)(PLLFREQ / (2000.0 * 228.0 * (1.0 + hw->cfg.ppm/1.e6)));
    uint32_t idivider = (uint32_t)divider;
    uint32_t fdivider = (uint32_t)((divider - idivider) * 4096);

    printf("ppm corr is %.4f, divider is %.4f (%d + %d*2^-12) [nominal 1096.4912].\n",
           hw->cfg.ppm, divider, idivider, fdivider);

    hw->pwm_reg[PWM_CTL] = 0;
    udelay(10);
    hw->clk_reg[PWMCLK_CNTL] = CM_PASSWD | 0x06;           /* Source=PLLD, disable */
    udelay(100);
    hw->clk_reg[PWMCLK_DIV]  = CM_PASSWD | (idivider<<12) | fdivider;
    udelay(100);
    hw->clk_reg[PWMCLK_CNTL] = CM_PASSWD | 0x0216;         /* Source=PLLD, enable, MASH 1 */
    udelay(100);
    hw->pwm_reg[PWM_RNG1] = 2;
    udelay(10);
    hw->pwm_reg[PWM_DMAC] = PWMDMAC_ENAB | PWMDMAC_THRSHLD;
    udelay(10);
    hw->pwm_reg[PWM_CTL] = PWMCTL_CLRF;
    udelay(10);
    hw->pwm_reg[PWM_CTL] = PWMCTL_USEF1 | PWMCTL_PWEN1;
    udelay(10);

    /* Kick off DMA. */
    hw->dma_reg[DMA_CS] = BCM2708_DMA_RESET;
    udelay(10);
    hw->dma_reg[DMA_CS] = BCM2708_DMA_INT | BCM2708_DMA_END;
    hw->dma_reg[DMA_CONBLK_AD] = mem_virt_to_phys(hw, hw->cb);
    hw->dma_reg[DMA_DEBUG] = 7;
    hw->dma_reg[DMA_CS] = DMA_CS_GO;

    hw->last_cb_virt = (size_t)hw->cb;
    hw->started      = 1;
    return PIFM_OK;
}

void hw_rpi_reset_dma(hw_rpi_t *hw) {
    if (hw == NULL || hw->dma_reg == NULL) return;
    hw->dma_reg[DMA_CS] = BCM2708_DMA_RESET;
}

void hw_rpi_stop(hw_rpi_t *hw) {
    if (hw == NULL || !hw->started) return;

    if (hw->clk_reg && hw->gpio_reg && hw->mbox.virt_addr) {
        /* GPIO4 back to plain output to kill the carrier. */
        hw->gpio_reg[GPFSEL0] = (hw->gpio_reg[GPFSEL0] & ~(7 << 12)) | (1 << 12);
        /* Disable GPCLK (password only, all enable bits zero). */
        hw->clk_reg[GPCLK_CNTL] = CM_PASSWD >> 24;
    }
    if (hw->dma_reg) {
        hw->dma_reg[DMA_CS] = BCM2708_DMA_RESET;
        udelay(10);
    }
    hw->started = 0;
}

void hw_rpi_destroy(hw_rpi_t **hw_pp) {
    if (hw_pp == NULL || *hw_pp == NULL) return;
    struct hw_rpi *hw = *hw_pp;

    if (hw->started) hw_rpi_stop(hw);

    if (hw->mbox.virt_addr != NULL) {
        unmapmem(hw->mbox.virt_addr, hw->num_pages * PAGE_SIZE);
        hw->mbox.virt_addr = NULL;
    }
    if (hw->mbox.bus_addr != 0) {
        mbox_mem_unlock(hw->mbox.handle, hw->mbox.mem_ref);
    }
    if (hw->mbox.mem_ref != 0) {
        mbox_mem_free(hw->mbox.handle, hw->mbox.mem_ref);
    }
    if (hw->mbox.handle >= 0) {
        mbox_close(hw->mbox.handle);
    }

    /* Unmap peripherals. We only know the lengths via the constants;
     * it is safe to skip munmap on process exit since the kernel
     * reclaims the VA space. Do it anyway for valgrind cleanliness. */
    if (hw->dma_reg)  munmap((void *)hw->dma_reg,  DMA_LEN);
    if (hw->pwm_reg)  munmap((void *)hw->pwm_reg,  PWM_LEN);
    if (hw->clk_reg)  munmap((void *)hw->clk_reg,  CLK_LEN);
    if (hw->gpio_reg) munmap((void *)hw->gpio_reg, GPIO_LEN);

    free(hw);
    *hw_pp = NULL;
}

int hw_rpi_free_slots(hw_rpi_t *hw) {
    if (hw == NULL || hw->dma_reg == NULL) return -1;

    /* §1.4: DMA_CONBLK_AD is volatile DMA-engine state. It can briefly
     * read 0 (end-of-transfer) or a partially-latched value; require
     * two identical reads that fall inside our CB region. */
    uint32_t phys1 = hw->dma_reg[DMA_CONBLK_AD];
    uint32_t phys2 = hw->dma_reg[DMA_CONBLK_AD];
    if (phys1 == 0 || phys1 != phys2) return -1;

    size_t cur_cb  = mem_phys_to_virt(hw, phys1);
    size_t cb_base = (size_t)hw->cb;
    size_t cb_end  = cb_base + hw->num_cbs * sizeof(dma_cb_t);
    if (cur_cb < cb_base || cur_cb >= cb_end) return -1;

    int last_sample = (hw->last_cb_virt - (size_t)hw->mbox.virt_addr)
                      / (sizeof(dma_cb_t) * 2);
    int this_sample = (cur_cb - (size_t)hw->mbox.virt_addr)
                      / (sizeof(dma_cb_t) * 2);
    int free = this_sample - last_sample;
    if (free < 0) free += hw->cfg.num_samples;
    return free;
}

pifm_status_t hw_rpi_push_deltas(hw_rpi_t *hw, const int *deltas, size_t n) {
    if (hw == NULL || deltas == NULL) return PIFM_ERR_ARG;

    int last_sample = (hw->last_cb_virt - (size_t)hw->mbox.virt_addr)
                      / (sizeof(dma_cb_t) * 2);
    for (size_t i = 0; i < n; i++) {
        hw->sample[last_sample++] = hw->freq_ctl_word + deltas[i];
        if (last_sample == hw->cfg.num_samples) last_sample = 0;
    }
    hw->last_cb_virt = (size_t)hw->mbox.virt_addr
                       + (size_t)last_sample * sizeof(dma_cb_t) * 2;
    return PIFM_OK;
}

