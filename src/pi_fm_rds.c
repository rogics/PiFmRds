/*
 * PiFmRds - FM/RDS transmitter for the Raspberry Pi
 * Copyright (C) 2014, 2015 Christophe Jacquet, F8FTK
 * Copyright (C) 2012, 2015 Richard Hirst
 * Copyright (C) 2012 Oliver Mattos and Oskar Weigl
 *
 * See https://github.com/ChristopheJacquet/PiFmRds
 *
 * PI-FM-RDS: RaspberryPi FM transmitter, with RDS.
 *
 * This file contains the VHF FM modulator. All credit goes to the original
 * authors, Oliver Mattos and Oskar Weigl for the original idea, and to
 * Richard Hirst for using the Pi's DMA engine, which reduced CPU usage
 * dramatically.
 *
 * I (Christophe Jacquet) have adapted their idea to transmitting samples
 * at 228 kHz, allowing to build the 57 kHz subcarrier for RDS BPSK data.
 *
 * To make it work on the Raspberry Pi 2, I used a fix by Richard Hirst
 * (again) to request memory using Broadcom's mailbox interface. This fix
 * was published for ServoBlaster here:
 * https://www.raspberrypi.org/forums/viewtopic.php?p=699651#p699651
 *
 * Never use this to transmit VHF-FM data through an antenna, as it is
 * illegal in most countries. This code is for testing purposes only.
 * Always connect a shielded transmission line from the RaspberryPi directly
 * to a radio receiver, so as *not* to emit radio waves.
 *
 * ---------------------------------------------------------------------------
 * These are the comments from Richard Hirst's version:
 *
 * RaspberryPi based FM transmitter.  For the original idea, see:
 *
 * http://www.icrobotics.co.uk/wiki/index.php/Turning_the_Raspberry_Pi_Into_an_FM_Transmitter
 *
 * All credit to Oliver Mattos and Oskar Weigl for creating the original code.
 *
 * I have taken their idea and reworked it to use the Pi DMA engine, so
 * reducing the CPU overhead for playing a .wav file from 100% to about 1.6%.
 *
 * I have implemented this in user space, using an idea I picked up from Joan
 * on the Raspberry Pi forums - credit to Joan for the DMA from user space
 * idea.
 *
 * The idea of feeding the PWM FIFO in order to pace DMA control blocks comes
 * from ServoBlaster, and I take credit for that :-)
 *
 * This code uses DMA channel 0 and the PWM hardware, with no regard for
 * whether something else might be trying to use it at the same time (such as
 * the 3.5mm jack audio driver).
 *
 * I know nothing much about sound, subsampling, or FM broadcasting, so it is
 * quite likely the sound quality produced by this code can be improved by
 * someone who knows what they are doing.  There may be issues realting to
 * caching, as the user space process just writes to its virtual address space,
 * and expects the DMA controller to see the data; it seems to work for me
 * though.
 *
 * NOTE: THIS CODE MAY WELL CRASH YOUR PI, TRASH YOUR FILE SYSTEMS, AND
 * POTENTIALLY EVEN DAMAGE YOUR HARDWARE.  THIS IS BECAUSE IT STARTS UP THE DMA
 * CONTROLLER USING MEMORY OWNED BY A USER PROCESS.  IF THAT USER PROCESS EXITS
 * WITHOUT STOPPING THE DMA CONTROLLER, ALL HELL COULD BREAK LOOSE AS THE
 * MEMORY GETS REALLOCATED TO OTHER PROCESSES WHILE THE DMA CONTROLLER IS STILL
 * USING IT.  I HAVE ATTEMPTED TO MINIMISE ANY RISK BY CATCHING SIGNALS AND
 * RESETTING THE DMA CONTROLLER BEFORE EXITING, BUT YOU HAVE BEEN WARNED.  I
 * ACCEPT NO LIABILITY OR RESPONSIBILITY FOR ANYTHING THAT HAPPENS AS A RESULT
 * OF YOU RUNNING THIS CODE.  IF IT BREAKS, YOU GET TO KEEP ALL THE PIECES.
 *
 * NOTE ALSO:  THIS MAY BE ILLEGAL IN YOUR COUNTRY.  HERE ARE SOME COMMENTS
 * FROM MORE KNOWLEDGEABLE PEOPLE ON THE FORUM:
 *
 * "Just be aware that in some countries FM broadcast and especially long
 * distance FM broadcast could get yourself into trouble with the law, stray FM
 * broadcasts over Airband aviation is also strictly forbidden."
 *
 * "A low pass filter is really really required for this as it has strong
 * harmonics at the 3rd, 5th 7th and 9th which sit in licensed and rather
 * essential bands, ie GSM, HAM, emergency services and others. Polluting these
 * frequencies is immoral and dangerous, whereas "breaking in" on FM bands is
 * just plain illegal."
 *
 * "Don't get caught, this GPIO use has the potential to exceed the legal
 * limits by about 2000% with a proper aerial."
 *
 *
 * As for the original code, this code is released under the GPL.
 *
 * Richard Hirst <richardghirst@gmail.com>  December 2012
 */

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sndfile.h>

#include "rds.h"
#include "fm_mpx.h"
#include "control_pipe.h"

#include "mailbox.h"
#include "pifm_common.h"
#define MBFILE            DEVICE_FILE_NAME    /* From mailbox.h */

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

#define NUM_SAMPLES        50000
#define NUM_CBS            (NUM_SAMPLES * 2)

/* DMA control register (CS) flags. See BCM2835 ARM Peripherals
 * section 4.2.1 ("DMA Control and Status register"). */
#define BCM2708_DMA_NO_WIDE_BURSTS    (1<<26)
#define BCM2708_DMA_WAIT_RESP         (1<<3)
#define BCM2708_DMA_D_DREQ            (1<<6)
#define BCM2708_DMA_PER_MAP(x)        ((x)<<16)
#define BCM2708_DMA_END               (1<<1)
#define BCM2708_DMA_RESET             (1<<31)
#define BCM2708_DMA_INT               (1<<2)

/* Additional DMA CS bits used to start the engine (decomposes the
 * previously bare 0x10880001 literal). */
#define BCM2708_DMA_PRIORITY(x)       ((x)<<16)   /* AXI priority (0-15) */
#define BCM2708_DMA_PANIC_PRIORITY(x) ((x)<<20)   /* AXI panic priority */
#define BCM2708_DMA_WAIT_FOR_OUTSTANDING_WRITES  (1<<28)
#define BCM2708_DMA_ACTIVE            (1<<0)

/* DMA CS "start" word: priority 8, panic priority 8,
 * wait-for-outstanding-writes, start the engine.  Previously just
 * the bare literal 0x10880001. */
#define DMA_CS_GO                                                    \
    (BCM2708_DMA_WAIT_FOR_OUTSTANDING_WRITES |                       \
     BCM2708_DMA_PANIC_PRIORITY(8) |                                 \
     BCM2708_DMA_PRIORITY(8) |                                       \
     BCM2708_DMA_ACTIVE)

#define DMA_CS            (0x00/4)
#define DMA_CONBLK_AD        (0x04/4)
#define DMA_DEBUG        (0x20/4)

#define DMA_BASE_OFFSET        0x00007000
#define DMA_LEN            0x24
#define PWM_BASE_OFFSET        0x0020C000
#define PWM_LEN            0x28
#define CLK_BASE_OFFSET            0x00101000
#define CLK_LEN            0xA8
#define GPIO_BASE_OFFSET    0x00200000
#define GPIO_LEN        0x100

#define DMA_VIRT_BASE        (PERIPH_VIRT_BASE + DMA_BASE_OFFSET)
#define PWM_VIRT_BASE        (PERIPH_VIRT_BASE + PWM_BASE_OFFSET)
#define CLK_VIRT_BASE        (PERIPH_VIRT_BASE + CLK_BASE_OFFSET)
#define GPIO_VIRT_BASE        (PERIPH_VIRT_BASE + GPIO_BASE_OFFSET)
#define PCM_VIRT_BASE        (PERIPH_VIRT_BASE + PCM_BASE_OFFSET)

#define PWM_PHYS_BASE        (PERIPH_PHYS_BASE + PWM_BASE_OFFSET)
#define PCM_PHYS_BASE        (PERIPH_PHYS_BASE + PCM_BASE_OFFSET)
#define GPIO_PHYS_BASE        (PERIPH_PHYS_BASE + GPIO_BASE_OFFSET)


#define PWM_CTL            (0x00/4)
#define PWM_DMAC        (0x08/4)
#define PWM_RNG1        (0x10/4)
#define PWM_FIFO        (0x18/4)

#define PWMCLK_CNTL        40
#define PWMCLK_DIV        41

#define CM_GP0DIV (0x7e101074)

#define GPCLK_CNTL        (0x70/4)
#define GPCLK_DIV        (0x74/4)

#define PWMCTL_MODE1        (1<<1)
#define PWMCTL_PWEN1        (1<<0)
#define PWMCTL_CLRF        (1<<6)
#define PWMCTL_USEF1        (1<<5)

#define PWMDMAC_ENAB        (1<<31)
// I think this means it requests as soon as there is one free slot in the FIFO
// which is what we want as burst DMA would mess up our timing.
#define PWMDMAC_THRSHLD        ((15<<8)|(15<<0))

#define GPFSEL0            (0x00/4)

// The deviation specifies how wide the signal is. Use 25.0 for WBFM
// (broadcast radio) and about 3.5 for NBFM (walkie-talkie style radio)
#define DEVIATION        25.0


typedef struct {
    uint32_t info, src, dst, length,
         stride, next, pad[2];
} dma_cb_t;

#define BUS_TO_PHYS(x) ((x)&~0xC0000000)


static struct {
    int handle;            /* From mbox_open() */
    unsigned mem_ref;    /* From mbox_mem_alloc() */
    unsigned bus_addr;    /* From mbox_mem_lock() */
    uint8_t *virt_addr;    /* From mapmem() */
} mbox;

/* Set by the signal handler; polled by the main loop. Using
 * sig_atomic_t so reads/writes are guaranteed to be atomic with
 * respect to signal delivery. */
static volatile sig_atomic_t g_terminate_requested = 0;
static volatile sig_atomic_t g_terminate_signal = 0;



static volatile uint32_t *pwm_reg;
static volatile uint32_t *clk_reg;
static volatile uint32_t *dma_reg;
static volatile uint32_t *gpio_reg;

struct control_data_s {
    dma_cb_t cb[NUM_CBS];
    uint32_t sample[NUM_SAMPLES];
};

#define PAGE_SIZE    4096
#define PAGE_SHIFT    12
#define NUM_PAGES    ((sizeof(struct control_data_s) + PAGE_SIZE - 1) >> PAGE_SHIFT)

static struct control_data_s *ctl;

static void
udelay(int us)
{
    struct timespec ts = { 0, us * 1000 };

    nanosleep(&ts, NULL);
}

/* Async-signal-safe: must not call printf/malloc/etc.
 * All we do is record that a signal arrived; the real cleanup runs
 * from the main loop in do_cleanup_and_exit(). For the truly fatal
 * signals (SIGSEGV/BUS/FPE/ILL/ABRT) we do still perform cleanup
 * here, accepting the async-signal-unsafety risk because the
 * alternative is leaving the DMA engine + carrier running after the
 * process is killed. SA_RESETHAND is used so a repeat of the same
 * fatal signal will reach the default handler and core-dump. */
static void
terminate_signal_handler(int signum)
{
    g_terminate_signal = signum;
    g_terminate_requested = 1;
}

static void
do_cleanup_and_exit(int code)
{
    // Stop outputting and generating the clock.
    if (clk_reg && gpio_reg && mbox.virt_addr) {
        // Set GPIO4 to be an output (instead of ALT FUNC 0, which is the clock).
        gpio_reg[GPFSEL0] = (gpio_reg[GPFSEL0] & ~(7 << 12)) | (1 << 12);

        // Disable the clock generator (write password only, all enable bits zero).
        clk_reg[GPCLK_CNTL] = CM_PASSWD >> 24;
    }

    if (dma_reg && mbox.virt_addr) {
        dma_reg[DMA_CS] = BCM2708_DMA_RESET;
        udelay(10);
    }

    fm_mpx_close();
    control_pipe_close();

    if (mbox.virt_addr != NULL) {
        unmapmem(mbox.virt_addr, NUM_PAGES * 4096);
        mbox_mem_unlock(mbox.handle, mbox.mem_ref);
        mbox_mem_free(mbox.handle, mbox.mem_ref);
        mbox.virt_addr = NULL;
    }

    printf("Terminating: cleanly deactivated the DMA engine and killed the carrier.\n");

    exit(code);
}

/* Retained name for the remaining call sites that still want the
 * combined "clean up + exit" semantics (main() on success, fatal()
 * on startup errors). */
static void
terminate(int num)
{
    do_cleanup_and_exit(num);
}

/* Install handlers only on the signals we actually care about. The
 * old code called sigaction() for every integer in 0..63, which
 * installed a handler on SIGPIPE, SIGCHLD, SIGWINCH, etc. -- any of
 * which would kill the transmitter. */
static void
install_signal_handlers(void)
{
    /* "Graceful shutdown" signals: run cleanup from the main loop. */
    static const int graceful_signals[] = {
        SIGINT, SIGTERM, SIGHUP, SIGQUIT,
    };
    /* "We just crashed" signals: cleanup from the handler itself
     * and reset to the default disposition so a second identical
     * signal will core-dump. */
    static const int fatal_signals[] = {
        SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT,
    };

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = terminate_signal_handler;
    sigemptyset(&sa.sa_mask);

    for (size_t i = 0; i < sizeof(graceful_signals)/sizeof(graceful_signals[0]); i++) {
        sigaction(graceful_signals[i], &sa, NULL);
    }

    sa.sa_flags = SA_RESETHAND;
    for (size_t i = 0; i < sizeof(fatal_signals)/sizeof(fatal_signals[0]); i++) {
        sigaction(fatal_signals[i], &sa, NULL);
    }
}

static void
fatal(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    terminate(EXIT_FAILURE);
}

static size_t
mem_virt_to_phys(void *virt)
{
    size_t offset = (size_t)virt - (size_t)mbox.virt_addr;

    return mbox.bus_addr + offset;
}

static size_t
mem_phys_to_virt(size_t phys)
{
    return (size_t) (phys - mbox.bus_addr + mbox.virt_addr);
}

static void *
map_peripheral(uint32_t base, uint32_t len)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    void * vaddr;

    if (fd < 0)
        fatal("Failed to open /dev/mem: %m.\n");
    vaddr = mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, base);
    if (vaddr == MAP_FAILED)
        fatal("Failed to map peripheral at 0x%08x: %m.\n", base);
    close(fd);

    return vaddr;
}



#define SUBSIZE 1
#define DATA_SIZE 5000


int tx(uint32_t carrier_freq, const char *audio_file, uint16_t pi, const char *ps,
       const char *rt, float ppm, const char *control_pipe) {
    // Install handlers on the signals we actually want to intercept
    // (see install_signal_handlers for the list). Crucially, we no
    // longer hook SIGPIPE / SIGCHLD / real-time signals.
    install_signal_handlers();

    dma_reg = map_peripheral(DMA_VIRT_BASE, DMA_LEN);
    pwm_reg = map_peripheral(PWM_VIRT_BASE, PWM_LEN);
    clk_reg = map_peripheral(CLK_VIRT_BASE, CLK_LEN);
    gpio_reg = map_peripheral(GPIO_VIRT_BASE, GPIO_LEN);

    // Use the mailbox interface to the VC to ask for physical memory.
    mbox.handle = mbox_open();
    if (mbox.handle < 0)
        fatal("Failed to open mailbox. Check kernel support for vcio / BCM2708 mailbox.\n");
    printf("Allocating physical memory: size = %zu     ", NUM_PAGES * 4096);
    if(! (mbox.mem_ref = mbox_mem_alloc(mbox.handle, NUM_PAGES * 4096, 4096, MEM_FLAG))) {
        fatal("Could not allocate memory.\n");
    }
    // TODO: How do we know that succeeded?
    printf("mem_ref = %u     ", mbox.mem_ref);
    if(! (mbox.bus_addr = mbox_mem_lock(mbox.handle, mbox.mem_ref))) {
        fatal("Could not lock memory.\n");
    }
    printf("bus_addr = %x     ", mbox.bus_addr);
    if(! (mbox.virt_addr = mapmem(BUS_TO_PHYS(mbox.bus_addr), NUM_PAGES * 4096))) {
        fatal("Could not map memory.\n");
    }
    printf("virt_addr = %p\n", mbox.virt_addr);


    // GPIO4 needs to be ALT FUNC 0 to output the clock
    gpio_reg[GPFSEL0] = (gpio_reg[GPFSEL0] & ~(7 << 12)) | (4 << 12);

    // Program GPCLK to use MASH setting 1, so fractional dividers work
    clk_reg[GPCLK_CNTL] = CM_PASSWD | 6;
    udelay(100);
    clk_reg[GPCLK_CNTL] = CM_PASSWD | 1 << 9 | 1 << 4 | 6;

    ctl = (struct control_data_s *) mbox.virt_addr;
    dma_cb_t *cbp = ctl->cb;
    uint32_t phys_sample_dst = CM_GP0DIV;
    uint32_t phys_pwm_fifo_addr = PWM_PHYS_BASE + 0x18;


    // Calculate the frequency control word
    // The fractional part is stored in the lower 12 bits
    uint32_t freq_ctl = ((float)(PLLFREQ / carrier_freq)) * ( 1 << 12 );


    for (int i = 0; i < NUM_SAMPLES; i++) {
        ctl->sample[i] = CM_PASSWD | freq_ctl;    // Silence
        // Write a frequency sample
        cbp->info = BCM2708_DMA_NO_WIDE_BURSTS | BCM2708_DMA_WAIT_RESP;
        cbp->src = mem_virt_to_phys(ctl->sample + i);
        cbp->dst = phys_sample_dst;
        cbp->length = 4;
        cbp->stride = 0;
        cbp->next = mem_virt_to_phys(cbp + 1);
        cbp++;
        // Delay
        cbp->info = BCM2708_DMA_NO_WIDE_BURSTS | BCM2708_DMA_WAIT_RESP | BCM2708_DMA_D_DREQ | BCM2708_DMA_PER_MAP(5);
        cbp->src = mem_virt_to_phys(mbox.virt_addr);
        cbp->dst = phys_pwm_fifo_addr;
        cbp->length = 4;
        cbp->stride = 0;
        cbp->next = mem_virt_to_phys(cbp + 1);
        cbp++;
    }
    cbp--;
    cbp->next = mem_virt_to_phys(mbox.virt_addr);

    // Here we define the rate at which we want to update the GPCLK control
    // register.
    //
    // Set the range to 2 bits. PLLD is at 500 MHz, therefore to get 228 kHz
    // we need a divisor of 500000000 / 2000 / 228 = 1096.491228
    //
    // This is 1096 + 2012*2^-12 theoretically
    //
    // However the fractional part may have to be adjusted to take the actual
    // frequency of your Pi's oscillator into account. For example on my Pi,
    // the fractional part should be 1916 instead of 2012 to get exactly
    // 228 kHz. However RDS decoding is still okay even at 2012.
    //
    // So we use the 'ppm' parameter to compensate for the oscillator error

    float divider = (PLLFREQ/(2000*228*(1.+ppm/1.e6)));
    uint32_t idivider = (uint32_t) divider;
    uint32_t fdivider = (uint32_t) ((divider - idivider)*pow(2, 12));

    printf("ppm corr is %.4f, divider is %.4f (%d + %d*2^-12) [nominal 1096.4912].\n",
                ppm, divider, idivider, fdivider);

    pwm_reg[PWM_CTL] = 0;
    udelay(10);
    clk_reg[PWMCLK_CNTL] = CM_PASSWD | 0x06;        // Source=PLLD and disable
    udelay(100);
    // theorically : 1096 + 2012*2^-12
    clk_reg[PWMCLK_DIV] = CM_PASSWD | (idivider<<12) | fdivider;
    udelay(100);
    clk_reg[PWMCLK_CNTL] = CM_PASSWD | 0x0216;      // Source=PLLD and enable + MASH filter 1
    udelay(100);
    pwm_reg[PWM_RNG1] = 2;
    udelay(10);
    pwm_reg[PWM_DMAC] = PWMDMAC_ENAB | PWMDMAC_THRSHLD;
    udelay(10);
    pwm_reg[PWM_CTL] = PWMCTL_CLRF;
    udelay(10);
    pwm_reg[PWM_CTL] = PWMCTL_USEF1 | PWMCTL_PWEN1;
    udelay(10);


    // Initialise the DMA
    dma_reg[DMA_CS] = BCM2708_DMA_RESET;
    udelay(10);
    dma_reg[DMA_CS] = BCM2708_DMA_INT | BCM2708_DMA_END;
    dma_reg[DMA_CONBLK_AD] = mem_virt_to_phys(ctl->cb);
    dma_reg[DMA_DEBUG] = 7; // clear debug error flags
    dma_reg[DMA_CS] = DMA_CS_GO;    // go, priority 8/panic 8, wait for outstanding writes


    size_t last_cb_virt_addr = (size_t)ctl->cb;

    // Data structures for baseband data
    float data[DATA_SIZE];
    int data_len = 0;
    int data_index = 0;

    // Initialize the baseband generator
    if(fm_mpx_open(audio_file, DATA_SIZE) < 0) return 1;

    // Initialize the RDS modulator
    char generated_ps[PS_BUF_SIZE] = {0};
    rds_set_pi(pi);
    rds_set_rt(rt);
    uint16_t ps_cycle_counter = 0;
    uint16_t ps_numeric_counter = 0;
    int varying_ps = 0;

    if(ps) {
        rds_set_ps(ps);
        printf("PI: %04X, PS: \"%s\".\n", pi, ps);
    } else {
        printf("PI: %04X, PS: <Varying>.\n", pi);
        varying_ps = 1;
    }
    printf("RT: \"%s\"\n", rt);

    // Initialize the control pipe reader
    if(control_pipe) {
        printf("Waiting for control pipe `%s` to be opened by the writer, e.g. "
               "by running `cat >%s`.\n", control_pipe, control_pipe);
        if(control_pipe_open(control_pipe) == 0) {
            printf("Reading control commands on %s.\n", control_pipe);
        } else {
            printf("Failed to open control pipe: %s.\n", control_pipe);
            control_pipe = NULL;
        }
    }


    printf("Starting to transmit on %3.1f MHz.\n", carrier_freq/1e6);

    for (;;) {
        if (g_terminate_requested) {
            int sig = g_terminate_signal;
            printf("Caught signal %d; shutting down.\n", sig);
            do_cleanup_and_exit(EXIT_SUCCESS);
        }

        // Default (varying) PS
        if(varying_ps) {
            if(ps_cycle_counter == 512) {
                snprintf(generated_ps, PS_BUF_SIZE, "%08d", ps_numeric_counter);
                rds_set_ps(generated_ps);
                ps_numeric_counter++;
            }
            if(ps_cycle_counter == 1024) {
                rds_set_ps("RPi-Live");
                ps_cycle_counter = 0;
            }
            ps_cycle_counter++;
        }

        if(control_pipe && control_pipe_poll() == CONTROL_PIPE_PS_SET) {
            varying_ps = 0;
        }

        usleep(5000);

        size_t cur_cb = mem_phys_to_virt(dma_reg[DMA_CONBLK_AD]);
        int last_sample = (last_cb_virt_addr - (size_t)mbox.virt_addr) / (sizeof(dma_cb_t) * 2);
        int this_sample = (cur_cb - (size_t)mbox.virt_addr) / (sizeof(dma_cb_t) * 2);
        int free_slots = this_sample - last_sample;

        if (free_slots < 0)
            free_slots += NUM_SAMPLES;

        while (free_slots >= SUBSIZE) {
            // get more baseband samples if necessary
            if(data_len == 0) {
                if( fm_mpx_get_samples(data) < 0 ) {
                    terminate(0);
                }
                data_len = DATA_SIZE;
                data_index = 0;
            }

            float dval = data[data_index] * (DEVIATION / MPX_SCALE_DIV);
            data_index++;
            data_len--;

            int intval = lrintf(dval);
            //int frac = (int)((dval - (float)intval) * SUBSIZE);


            ctl->sample[last_sample++] = (CM_PASSWD | freq_ctl) + intval; //(frac > j ? intval + 1 : intval);
            if (last_sample == NUM_SAMPLES)
                last_sample = 0;

            free_slots -= SUBSIZE;
        }
        last_cb_virt_addr = (size_t)(mbox.virt_addr + last_sample * sizeof(dma_cb_t) * 2);
    }

    return 0;
}


#ifndef PIFM_VERSION
#define PIFM_VERSION "unknown"
#endif

static const char *g_progname = "pi_fm_rds";

static void print_usage(FILE *out) {
    fprintf(out,
        "Usage: %s [options]\n"
        "\n"
        "  --freq FREQ       carrier frequency in MHz (76.0 .. 108.0)\n"
        "  --audio FILE      WAV/OGG/FLAC audio file to transmit (or - for stdin)\n"
        "  --wav   FILE      alias for --audio\n"
        "  --ppm   PPM       crystal error in parts-per-million\n"
        "  --pi    HEX       RDS PI code (4 hex digits)\n"
        "  --ps    TEXT      RDS Programme Service name (<= %d chars)\n"
        "  --rt    TEXT      RDS RadioText (<= %d chars)\n"
        "  --ctl   FILE      FIFO / file used to update PS/RT/TA at runtime\n"
        "  -h, --help        print this message and exit\n"
        "  -V, --version     print the program version and exit\n"
        "\n"
        "Single-dash forms (-freq, -ps, ...) are accepted for backward\n"
        "compatibility; please migrate to the double-dash forms.\n",
        g_progname, PS_LENGTH, RT_LENGTH);
}

static void print_version(FILE *out) {
    fprintf(out, "PiFmRds %s\n", PIFM_VERSION);
}

/* Parse a carrier frequency given in MHz as a string. Returns the
 * frequency in Hz, or 0 on parse / range error. */
static uint32_t parse_carrier_freq(const char *s) {
    char *end = NULL;
    double mhz = strtod(s, &end);
    if (end == s || *end != '\0') return 0;
    if (mhz < 76.0 || mhz > 108.0) return 0;
    return (uint32_t)(mhz * 1e6);
}

int main(int argc, char **argv) {
    const char *audio_file = NULL;
    const char *control_pipe = NULL;
    uint32_t carrier_freq = 107900000;
    const char *ps = NULL;
    const char *rt = "PiFmRds: live FM-RDS transmission from the RaspberryPi";
    uint16_t pi = 0x1234;
    float ppm = 0;

    if (argc > 0 && argv[0] != NULL && argv[0][0] != '\0') {
        g_progname = argv[0];
    }

    /* Deprecation warning: emit one-off warning per legacy single-dash
     * long option (-freq, -ps, ...). getopt_long_only() still accepts
     * them so behaviour is preserved; they are scheduled for removal
     * in a future release per Phase 11 of the refactoring plan. */
    static const char * const legacy_singles[] = {
        "-freq", "-audio", "-wav", "-ppm", "-pi", "-ps", "-rt", "-ctl", NULL
    };
    for (int i = 1; i < argc; i++) {
        for (int j = 0; legacy_singles[j] != NULL; j++) {
            if (strcmp(argv[i], legacy_singles[j]) == 0) {
                fprintf(stderr, "warning: '%s' is deprecated, use '-%s' "
                        "(or just --%s) instead.\n",
                        legacy_singles[j], legacy_singles[j], legacy_singles[j] + 1);
                break;
            }
        }
    }

    /* Long-option table. We use getopt_long_only so the classic
     * single-dash spellings (-freq, -ps, ...) still work alongside
     * the modern double-dash forms. All options take a required
     * argument except --help / --version.
     *
     * `val` doubles as a synthetic "short option" identifier; using
     * values above 127 avoids clashes with the real -h / -V shorts. */
    enum {
        OPT_FREQ = 0x100,
        OPT_AUDIO, OPT_WAV, OPT_PPM, OPT_PI, OPT_PS, OPT_RT, OPT_CTL,
    };
    static const struct option long_opts[] = {
        { "freq",    required_argument, NULL, OPT_FREQ   },
        { "audio",   required_argument, NULL, OPT_AUDIO  },
        { "wav",     required_argument, NULL, OPT_WAV    },
        { "ppm",     required_argument, NULL, OPT_PPM    },
        { "pi",      required_argument, NULL, OPT_PI     },
        { "ps",      required_argument, NULL, OPT_PS     },
        { "rt",      required_argument, NULL, OPT_RT     },
        { "ctl",     required_argument, NULL, OPT_CTL    },
        { "help",    no_argument,       NULL, 'h'        },
        { "version", no_argument,       NULL, 'V'        },
        { NULL, 0, NULL, 0 }
    };

    /* Leading ':' enables distinguishing unknown option (?) from
     * missing argument (:) in the switch below. */
    int opt;
    int opt_index = 0;
    while ((opt = getopt_long_only(argc, argv, ":hV", long_opts, &opt_index)) != -1) {
        switch (opt) {
        case OPT_FREQ: {
            uint32_t f = parse_carrier_freq(optarg);
            if (f == 0) {
                fatal("Incorrect frequency specification '%s'. Must be in megahertz, "
                      "of the form 107.9, between 76 and 108.\n", optarg);
            }
            carrier_freq = f;
            break;
        }
        case OPT_AUDIO:
        case OPT_WAV:
            audio_file = optarg;
            break;
        case OPT_PPM:
            ppm = atof(optarg);
            break;
        case OPT_PI: {
            char *end = NULL;
            long v = strtol(optarg, &end, 16);
            if (end == optarg || *end != '\0' || v < 0 || v > 0xFFFF) {
                fatal("Incorrect PI code '%s'. Must be four hex digits (e.g. 1234).\n",
                      optarg);
            }
            pi = (uint16_t)v;
            break;
        }
        case OPT_PS:
            if (strlen(optarg) > PS_LENGTH) {
                fprintf(stderr, "warning: PS text '%s' is longer than %d characters; "
                        "will be truncated.\n", optarg, PS_LENGTH);
            }
            ps = optarg;
            break;
        case OPT_RT:
            if (strlen(optarg) > RT_LENGTH) {
                fprintf(stderr, "warning: RT text is longer than %d characters; "
                        "will be truncated.\n", RT_LENGTH);
            }
            rt = optarg;
            break;
        case OPT_CTL:
            control_pipe = optarg;
            break;
        case 'h':
            print_usage(stdout);
            return EXIT_SUCCESS;
        case 'V':
            print_version(stdout);
            return EXIT_SUCCESS;
        case ':':
            fatal("Option '%s' requires an argument.\n", argv[optind - 1]);
        case '?':
        default:
            print_usage(stderr);
            return EXIT_FAILURE;
        }
    }

    if (optind < argc) {
        fatal("Unexpected positional argument: %s\n", argv[optind]);
    }

    /* Set locale based on the environment variables. Necessary to
     * decode non-ASCII characters using mbtowc() in rds_strings.c. */
    char* locale = setlocale(LC_ALL, "");
    printf("Locale set to %s.\n", locale);

    int errcode = tx(carrier_freq, audio_file, pi, ps, rt, ppm, control_pipe);

    terminate(errcode);
}
