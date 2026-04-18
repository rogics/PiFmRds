/*
 * PiFmRds - FM/RDS transmitter for the Raspberry Pi
 * Copyright (C) 2014, 2015 Christophe Jacquet, F8FTK
 * Copyright (C) 2012, 2015 Richard Hirst
 * Copyright (C) 2012 Oliver Mattos and Oskar Weigl
 *
 * See https://github.com/ChristopheJacquet/PiFmRds
 *
 * Main entry point for the PiFmRds FM/RDS transmitter.
 *
 * Threading model:
 *   main thread    -- signal handling, PS rotation, control-pipe poll,
 *                     shutdown watchdog.
 *   DSP thread     -- fm_mpx_ctx_get_samples() -> SPSC ring (floats).
 *                     SCHED_OTHER; libsndfile I/O may block here.
 *   feeder thread  -- SPSC ring -> hw_rpi_push_deltas().
 *                     SCHED_FIFO (falls back to SCHED_OTHER if
 *                     CAP_SYS_NICE is not available). Wakes on a
 *                     deadline derived from the 228 kHz sample rate
 *                     (see hw_rpi_wait_space).
 *
 * The ring decouples audio-decode stalls from the DMA consumer: a
 * brief sndfile stall no longer starves the RF output, and a long
 * DMA backlog can no longer block audio decode.
 *
 * WARNING: transmitting on the VHF FM band is illegal in most
 * countries without a licence. Use with a dummy load or shielded
 * cable; not with an antenna.
 *
 * Released under the GPLv3.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sndfile.h>

#include "control_pipe.h"
#include "fm_mpx.h"
#include "hw_rpi.h"
#include "logging.h"
#include "pifm_common.h"
#include "rds.h"
#include "ring_spsc.h"

int g_log_level = LOG_LEVEL_INFO;

/* Samples the DSP generates per fm_mpx call. Sized so DATA_SIZE is
 * substantially less than both the DMA ring and the SPSC ring, so the
 * producer can usually push a full batch without blocking. */
#define DATA_SIZE 5000

/* Deviation: 25.0 for WBFM (broadcast), ~3.5 for NBFM. */
#define DEVIATION 25.0f

/* DMA ring size (in samples). */
#define DEFAULT_NUM_SAMPLES 50000

/* SPSC ring capacity. Must be a power of two. 2^17 = 131072 floats
 * = 512 KB, ~575 ms of slack at 228 kHz. Generous, at the cost of
 * half a megabyte of heap; this is what keeps the DMA fed across
 * audio-I/O stalls. */
#define RING_CAPACITY_LOG2 17
#define RING_CAPACITY      (1u << RING_CAPACITY_LOG2)

/* Feeder refill watermark, in DMA slots. The feeder only wakes once
 * this many slots have drained; it then refills them in one pass.
 * At 228 kHz, 1140 samples = 5 ms, matching the historical
 * `usleep(5000)` pacing. Anything much smaller and the feeder burns
 * a CPU core on syscall/poll overhead; anything much larger adds
 * latency to the RDS/MPX output. */
#define FEEDER_REFILL_WATERMARK 1140

/* --- signal handler / globals ------------------------------------------- */
static volatile sig_atomic_t g_terminate_requested = 0;
static volatile sig_atomic_t g_terminate_signal    = 0;

static hw_rpi_t    *g_hw  = NULL;
static fm_mpx_ctx_t *g_mpx = NULL;

/* --dry-run: exercise the entire DSP + threading pipeline without
 * touching the Pi's DMA/PWM/GPCLK hardware. Useful for CI on non-Pi
 * hosts and for quick smoke tests before a real broadcast. */
static int g_dry_run = 0;
/* --seconds N: auto-exit after N wall-clock seconds. 0 means "run
 * forever" (the historical default). */
static int g_max_seconds = 0;

static void terminate_signal_handler(int signum) {
    g_terminate_signal    = signum;
    g_terminate_requested = 1;
}

static void install_signal_handlers(void) {
    static const int graceful_signals[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT };
    static const int fatal_signals[]    = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = terminate_signal_handler;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof(graceful_signals)/sizeof(graceful_signals[0]); i++)
        sigaction(graceful_signals[i], &sa, NULL);
    sa.sa_flags = SA_RESETHAND;
    for (size_t i = 0; i < sizeof(fatal_signals)/sizeof(fatal_signals[0]); i++)
        sigaction(fatal_signals[i], &sa, NULL);
}

/* --- DSP + feeder thread plumbing --------------------------------------- */

struct pifm_app {
    hw_rpi_t       *hw;
    fm_mpx_ctx_t   *mpx;
    ring_spsc_t     ring;
    float          *ring_storage;

    /* Wakeup semaphores. DSP posts `feeder_wake` after pushing; feeder
     * posts `dsp_wake` after draining. Both use sem_timedwait so no
     * post is ever "lost" -- a missed post just means the receiver
     * sleeps the timeout and re-polls. */
    sem_t           dsp_wake;
    sem_t           feeder_wake;

    pthread_t       dsp_tid;
    pthread_t       feeder_tid;
    int             dsp_spawned;
    int             feeder_spawned;
};

static struct pifm_app g_app;

static void timespec_add_ms(struct timespec *ts, long ms) {
    ts->tv_nsec += ms * 1000000L;
    while (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

/* DSP producer thread. Generates MPX samples and pushes them into the
 * SPSC ring. When the ring is full, sleeps on dsp_wake so we don't
 * spin. Terminates when g_terminate_requested is raised. */
static void *dsp_thread_main(void *arg) {
    struct pifm_app *app = arg;
    float batch[DATA_SIZE];
    while (!g_terminate_requested) {
        if (fm_mpx_ctx_get_samples(app->mpx, batch) != PIFM_OK) {
            LOG_ERR("DSP thread: fm_mpx_ctx_get_samples failed; exiting");
            g_terminate_requested = 1;
            break;
        }

        size_t written = 0;
        while (written < DATA_SIZE && !g_terminate_requested) {
            size_t n = ring_spsc_push(&app->ring, batch + written, DATA_SIZE - written);
            written += n;
            if (n > 0) sem_post(&app->feeder_wake);
            if (written < DATA_SIZE) {
                /* Ring full: wait for the feeder to drain. 50 ms is
                 * far longer than any realistic drain interval; this
                 * is a safety net, not the normal wake path. */
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                timespec_add_ms(&ts, 50);
                sem_timedwait(&app->dsp_wake, &ts);
            }
        }
    }
    return NULL;
}

/* DMA feeder thread. Pops MPX samples out of the SPSC ring, converts
 * them to integer frequency deltas, and hands them to hw_rpi. Uses
 * hw_rpi_wait_space (deadline-based via clock_nanosleep) for pacing. */
static void *feeder_thread_main(void *arg) {
    struct pifm_app *app = arg;
    float  scratch[DATA_SIZE];
    int    deltas[DATA_SIZE];
    const float scale = DEVIATION / MPX_SCALE_DIV;

    while (!g_terminate_requested) {
        int free_slots;
        if (app->hw != NULL) {
            /* Pace at a fixed 5 ms cadence, matching the old
             * `usleep(5000)` main loop. Exactly one nanosleep syscall
             * per iteration: no risk of `hw_rpi_free_slots` returning
             * above the watermark and letting the loop spin. */
            struct timespec ts = { 0, 5L * 1000L * 1000L };
            nanosleep(&ts, NULL);
            free_slots = hw_rpi_free_slots(app->hw);
            if (free_slots < 0) continue;
        } else {
            /* --dry-run: consume at 228 kHz via clock_nanosleep so the
             * DSP producer can't race ahead indefinitely. Pull a large
             * batch so the pacing overhead stays tiny. */
            /* Use long long for the intermediate product: on 32-bit ARM,
             * long is 32-bit and DATA_SIZE * 1e9 would overflow. */
            long long ns = (long long)DATA_SIZE * 1000000000LL / 228000LL;
            struct timespec ts;
            ts.tv_sec  = (time_t)(ns / 1000000000LL);
            ts.tv_nsec = (long)  (ns % 1000000000LL);
            nanosleep(&ts, NULL);
            free_slots = DATA_SIZE;
        }
        if (free_slots > DATA_SIZE) free_slots = DATA_SIZE;

        size_t got = ring_spsc_pop(&app->ring, scratch, (size_t)free_slots);
        if (got == 0) {
            /* DSP producer has stalled. Sleep briefly and re-check so
             * we don't burn CPU. 10 ms is short enough to keep the
             * ~220 ms DMA backlog from underflowing. */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            timespec_add_ms(&ts, 10);
            sem_timedwait(&app->feeder_wake, &ts);
            continue;
        }

        for (size_t i = 0; i < got; i++) {
            deltas[i] = lrintf(scratch[i] * scale);
        }
        if (app->hw != NULL) {
            hw_rpi_push_deltas(app->hw, deltas, got);
        }
        /* In --dry-run mode we deliberately DO still compute the deltas
         * (keeps the producer/consumer balance realistic and exercises
         * the whole lrintf path) but just discard the result. */

        /* Wake the DSP thread in case it blocked on a full ring. */
        sem_post(&app->dsp_wake);
    }
    return NULL;
}

/* Try to promote a pthread to SCHED_FIFO. Returns 1 on success, 0 if
 * we lacked the capability (common in desktop dev). */
static int try_sched_fifo(pthread_t tid, int priority) {
    struct sched_param sp = { .sched_priority = priority };
    if (pthread_setschedparam(tid, SCHED_FIFO, &sp) == 0) return 1;
    LOG_WARN("Could not set SCHED_FIFO priority %d (%s); running SCHED_OTHER.",
             priority, strerror(errno));
    return 0;
}

/* --- shutdown watchdog -------------------------------------------------- */

/* Global "cleanup in progress" deadline. If the main thread doesn't
 * manage to tear everything down within WATCHDOG_MS, this handler
 * forces a DMA reset and calls _exit. */
#define WATCHDOG_MS 5000

static void watchdog_signal_handler(int signum) {
    (void)signum;
    /* Async-signal-safe: single MMIO write + _exit. */
    if (g_hw) hw_rpi_reset_dma(g_hw);
    const char *msg = "pi_fm_rds: shutdown watchdog fired; forcing DMA reset.\n";
    write(STDERR_FILENO, msg, strlen(msg));
    _exit(EXIT_FAILURE);
}

static void arm_shutdown_watchdog(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = watchdog_signal_handler;
    sigaction(SIGALRM, &sa, NULL);

    struct itimerval it;
    memset(&it, 0, sizeof(it));
    it.it_value.tv_sec  = WATCHDOG_MS / 1000;
    it.it_value.tv_usec = (WATCHDOG_MS % 1000) * 1000;
    setitimer(ITIMER_REAL, &it, NULL);
}

/* --- main sequencing ---------------------------------------------------- */

static void do_cleanup_and_exit(int code) __attribute__((noreturn));
static void do_cleanup_and_exit(int code) {
    /* Arm a watchdog; if any of the joins/closes hangs for more than
     * WATCHDOG_MS, SIGALRM will trigger a forced DMA reset + _exit. */
    arm_shutdown_watchdog();

    g_terminate_requested = 1;

    /* Wake both threads so they see the flag promptly. */
    sem_post(&g_app.dsp_wake);
    sem_post(&g_app.feeder_wake);

    if (g_app.dsp_spawned)    pthread_join(g_app.dsp_tid,    NULL);
    if (g_app.feeder_spawned) pthread_join(g_app.feeder_tid, NULL);

    if (g_hw) hw_rpi_stop(g_hw);
    fm_mpx_ctx_close(&g_mpx);
    control_pipe_close();
    if (g_hw) hw_rpi_destroy(&g_hw);

    sem_destroy(&g_app.dsp_wake);
    sem_destroy(&g_app.feeder_wake);
    free(g_app.ring_storage);
    g_app.ring_storage = NULL;

    /* Disarm the watchdog now that cleanup completed. */
    struct itimerval off;
    memset(&off, 0, sizeof(off));
    setitimer(ITIMER_REAL, &off, NULL);

    printf("Terminating: cleanly deactivated the DMA engine and killed the carrier.\n");
    exit(code);
}

static void fatal(const char *fmt, ...) __attribute__((noreturn));
static void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    do_cleanup_and_exit(EXIT_FAILURE);
}

static pifm_status_t tx(uint32_t carrier_freq, const char *audio_file, uint16_t pi,
                        const char *ps, const char *rt, float ppm,
                        const char *control_pipe) {
    install_signal_handlers();

    /* Lock memory pages so the feeder thread isn't paged out mid-DMA. */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        LOG_WARN("mlockall() failed (%s); proceeding without locked pages.",
                 strerror(errno));
    }

    /* ---- Hardware init (skipped in --dry-run) ----------------------- */
    pifm_status_t st;
    if (!g_dry_run) {
        hw_rpi_cfg_t cfg = {
            .carrier_freq_hz = carrier_freq,
            .ppm             = ppm,
            .num_samples     = DEFAULT_NUM_SAMPLES,
        };
        st = hw_rpi_init(&g_hw, &cfg);
        if (st != PIFM_OK) fatal("hw_rpi_init failed (%d)\n", (int)st);
        st = hw_rpi_start(g_hw);
        if (st != PIFM_OK) fatal("hw_rpi_start failed (%d)\n", (int)st);
        g_app.hw = g_hw;
    } else {
        LOG_INFO("Dry run: skipping Raspberry Pi hardware initialisation.");
        g_app.hw = NULL;
    }
    (void)carrier_freq; (void)ppm;

    /* ---- RDS --------------------------------------------------------- */
    rds_ctx_t *rds = rds_default_ctx();
    rds_ctx_set_pi(rds, pi);
    rds_ctx_set_rt(rds, rt);

    char     generated_ps[PS_BUF_SIZE] = {0};
    uint16_t ps_cycle_counter   = 0;
    uint16_t ps_numeric_counter = 0;
    int      varying_ps         = 0;

    if (ps) {
        rds_ctx_set_ps(rds, ps);
        printf("PI: %04X, PS: \"%s\".\n", pi, ps);
    } else {
        printf("PI: %04X, PS: <Varying>.\n", pi);
        varying_ps = 1;
    }
    printf("RT: \"%s\"\n", rt);

    /* ---- MPX --------------------------------------------------------- */
    st = fm_mpx_ctx_open(&g_mpx, audio_file, DATA_SIZE, rds);
    if (st != PIFM_OK) return PIFM_ERR_IO;
    g_app.mpx = g_mpx;

    /* ---- SPSC ring + semaphores ------------------------------------- */
    g_app.ring_storage = malloc((size_t)RING_CAPACITY * sizeof(float));
    if (g_app.ring_storage == NULL) fatal("Out of memory allocating SPSC ring.\n");
    if (ring_spsc_init(&g_app.ring, g_app.ring_storage, RING_CAPACITY) != 0)
        fatal("ring_spsc_init failed.\n");
    if (sem_init(&g_app.dsp_wake, 0, 0) != 0
     || sem_init(&g_app.feeder_wake, 0, 0) != 0)
        fatal("sem_init failed: %s\n", strerror(errno));

    /* ---- Control pipe ----------------------------------------------- */
    if (control_pipe) {
        LOG_INFO("Waiting for control pipe `%s` to be opened by the writer, e.g. "
                 "by running `cat >%s`.", control_pipe, control_pipe);
        if (control_pipe_open(control_pipe) == PIFM_OK) {
            LOG_INFO("Reading control commands on %s.", control_pipe);
        } else {
            LOG_ERR("Failed to open control pipe: %s.", control_pipe);
            control_pipe = NULL;
        }
    }

    if (g_dry_run) {
        printf("Dry run: DSP pipeline running, no RF output. Carrier would be %3.1f MHz.\n",
               carrier_freq/1e6);
    } else {
        printf("Starting to transmit on %3.1f MHz.\n", carrier_freq/1e6);
    }

    /* Arm the --seconds auto-exit timer. We reuse the same SIGALRM
     * that the shutdown watchdog will install later, but at this
     * point there's no watchdog yet, so SIGALRM here routes to the
     * normal terminate handler. */
    time_t deadline = (g_max_seconds > 0) ? time(NULL) + g_max_seconds : 0;

    /* ---- Spawn worker threads --------------------------------------- */
    if (pthread_create(&g_app.dsp_tid, NULL, dsp_thread_main, &g_app) != 0)
        fatal("pthread_create(dsp) failed: %s\n", strerror(errno));
    g_app.dsp_spawned = 1;

    if (pthread_create(&g_app.feeder_tid, NULL, feeder_thread_main, &g_app) != 0)
        fatal("pthread_create(feeder) failed: %s\n", strerror(errno));
    g_app.feeder_spawned = 1;
    /* Only the feeder gets SCHED_FIFO -- the DSP thread may block in
     * libsndfile, which we don't want to do under RT priority. */
    try_sched_fifo(g_app.feeder_tid, 2);

    /* ---- Main thread: control plane --------------------------------- */
    for (;;) {
        if (g_terminate_requested) {
            int sig = g_terminate_signal;
            printf("Caught signal %d; shutting down.\n", sig);
            do_cleanup_and_exit(EXIT_SUCCESS);
        }

        if (deadline != 0 && time(NULL) >= deadline) {
            printf("--seconds %d elapsed; shutting down.\n", g_max_seconds);
            do_cleanup_and_exit(EXIT_SUCCESS);
        }

        if (varying_ps) {
            if (ps_cycle_counter == 512) {
                snprintf(generated_ps, PS_BUF_SIZE, "%08d", ps_numeric_counter);
                rds_ctx_set_ps(rds, generated_ps);
                ps_numeric_counter++;
            }
            if (ps_cycle_counter == 1024) {
                rds_ctx_set_ps(rds, "RPi-Live");
                ps_cycle_counter = 0;
            }
            ps_cycle_counter++;
        }

        if (control_pipe && control_pipe_poll() == CONTROL_PIPE_PS_SET) {
            varying_ps = 0;
        }

        /* 5 ms matches the old control-plane tick cadence; nothing
         * audio-critical happens here so we can afford to be lazy. */
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    /* unreachable */
}

/* --- CLI ---------------------------------------------------------------- */

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
        "  --dry-run         run the DSP pipeline without touching Pi hardware\n"
        "  --seconds N       auto-exit after N seconds (0 = run forever)\n"
        "  -h, --help        print this message and exit\n"
        "  -V, --version     print the program version and exit\n"
        "  -v, --verbose     increase logging verbosity (repeatable)\n"
        "  -q, --quiet       decrease logging verbosity (repeatable)\n"
        "\n"
        "Single-dash forms (-freq, -ps, ...) are accepted for backward\n"
        "compatibility; please migrate to the double-dash forms.\n",
        g_progname, PS_LENGTH, RT_LENGTH);
}

static void print_version(FILE *out) {
    fprintf(out, "PiFmRds %s\n", PIFM_VERSION);
}

static uint32_t parse_carrier_freq(const char *s) {
    char *end = NULL;
    double mhz = strtod(s, &end);
    if (end == s || *end != '\0') return 0;
    if (mhz < 76.0 || mhz > 108.0) return 0;
    return (uint32_t)(mhz * 1e6);
}

int main(int argc, char **argv) {
    const char *audio_file   = NULL;
    const char *control_pipe = NULL;
    uint32_t    carrier_freq = 107900000;
    const char *ps           = NULL;
    const char *rt           = "PiFmRds: live FM-RDS transmission from the RaspberryPi";
    uint16_t    pi           = 0x1234;
    float       ppm          = 0;

    if (argc > 0 && argv[0] != NULL && argv[0][0] != '\0') g_progname = argv[0];

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

    enum {
        OPT_FREQ = 0x100,
        OPT_AUDIO, OPT_WAV, OPT_PPM, OPT_PI, OPT_PS, OPT_RT, OPT_CTL,
        OPT_DRY_RUN, OPT_SECONDS,
    };
    static const struct option long_opts[] = {
        { "freq",    required_argument, NULL, OPT_FREQ    },
        { "audio",   required_argument, NULL, OPT_AUDIO   },
        { "wav",     required_argument, NULL, OPT_WAV     },
        { "ppm",     required_argument, NULL, OPT_PPM     },
        { "pi",      required_argument, NULL, OPT_PI      },
        { "ps",      required_argument, NULL, OPT_PS      },
        { "rt",      required_argument, NULL, OPT_RT      },
        { "ctl",     required_argument, NULL, OPT_CTL     },
        { "dry-run", no_argument,       NULL, OPT_DRY_RUN },
        { "seconds", required_argument, NULL, OPT_SECONDS },
        { "help",    no_argument,       NULL, 'h'         },
        { "version", no_argument,       NULL, 'V'         },
        { "verbose", no_argument,       NULL, 'v'         },
        { "quiet",   no_argument,       NULL, 'q'         },
        { NULL, 0, NULL, 0 }
    };

    int opt, opt_index = 0;
    while ((opt = getopt_long_only(argc, argv, ":hVvq", long_opts, &opt_index)) != -1) {
        switch (opt) {
        case OPT_FREQ: {
            uint32_t f = parse_carrier_freq(optarg);
            if (f == 0) fatal("Incorrect frequency specification '%s'. Must be in megahertz, "
                              "of the form 107.9, between 76 and 108.\n", optarg);
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
            if (end == optarg || *end != '\0' || v < 0 || v > 0xFFFF)
                fatal("Incorrect PI code '%s'. Must be four hex digits (e.g. 1234).\n", optarg);
            pi = (uint16_t)v;
            break;
        }
        case OPT_PS:
            if (strlen(optarg) > PS_LENGTH)
                fprintf(stderr, "warning: PS text '%s' is longer than %d characters; "
                        "will be truncated.\n", optarg, PS_LENGTH);
            ps = optarg;
            break;
        case OPT_RT:
            if (strlen(optarg) > RT_LENGTH)
                fprintf(stderr, "warning: RT text is longer than %d characters; "
                        "will be truncated.\n", RT_LENGTH);
            rt = optarg;
            break;
        case OPT_CTL: control_pipe = optarg; break;
        case OPT_DRY_RUN: g_dry_run = 1; break;
        case OPT_SECONDS: {
            char *end = NULL;
            long v = strtol(optarg, &end, 10);
            if (end == optarg || *end != '\0' || v <= 0 || v > INT32_MAX)
                fatal("Invalid --seconds value '%s'.\n", optarg);
            g_max_seconds = (int)v;
            break;
        }
        case 'h':     print_usage(stdout);   return EXIT_SUCCESS;
        case 'V':     print_version(stdout); return EXIT_SUCCESS;
        case 'v':     if (g_log_level < LOG_LEVEL_DBG) g_log_level++; break;
        case 'q':     if (g_log_level > LOG_LEVEL_ERR) g_log_level--; break;
        case ':':     fatal("Option '%s' requires an argument.\n", argv[optind - 1]);
        case '?':
        default:      print_usage(stderr); return EXIT_FAILURE;
        }
    }

    if (optind < argc) fatal("Unexpected positional argument: %s\n", argv[optind]);

    char *locale = setlocale(LC_ALL, "");
    printf("Locale set to %s.\n", locale);

    pifm_status_t status = tx(carrier_freq, audio_file, pi, ps, rt, ppm, control_pipe);
    do_cleanup_and_exit(status == PIFM_OK ? EXIT_SUCCESS : EXIT_FAILURE);
}
