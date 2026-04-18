# PiFmRds architecture

This document describes the post-Tier-2-refactor structure of PiFmRds.
For a user-facing introduction see the top-level [README](README.md);
for the list of historical suggestions see
[IMPROVEMENT_SUGGESTIONS.md](IMPROVEMENT_SUGGESTIONS.md).

## Layered view

```
 ┌───────────────────────────────────────────────────────────────┐
 │ pi_fm_rds (CLI + orchestration)                               │
 │ -------------------------------                               │
 │  argv parsing, signal installation, PS rotation, control      │
 │  pipe polling, shutdown watchdog                              │
 └──────────────┬────────────────────────────┬───────────────────┘
                │                            │
   spawns DSP   │                            │   spawns feeder
   thread       ▼                            ▼   thread
     ┌─────────────────┐   SPSC ring   ┌──────────────────┐
     │ DSP producer    │──────────────▶│ DMA feeder       │
     │ SCHED_OTHER     │   (floats)    │ SCHED_FIFO       │
     │ libsndfile I/O  │               │ 5 ms nanosleep   │
     └─────┬───────────┘               └─────┬────────────┘
           │                                 │
           ▼                                 ▼
     ┌─────────────────┐               ┌──────────────────┐
     │ libpifmrds.a    │               │ hw_rpi           │
     │ -------------   │               │ -------------    │
     │ rds.c (ctx)     │               │ /dev/mem mmap    │
     │ fm_mpx.c (ctx)  │               │ mailbox alloc    │
     │ rds_strings.c   │               │ DMA control blocks│
     │ waveforms.c     │               │ PWM + GPCLK      │
     └─────────────────┘               └──────────────────┘
                                             │
                                             ▼
                                       (Raspberry Pi
                                        peripherals)
```

The library/driver split means:

- **libpifmrds.a** is pure C with no platform dependencies. It is
  exercised identically by `pi_fm_rds` on the Pi, by `rds_wav` on any
  host, and by the unit tests. A bug in the DSP pipeline shows up
  everywhere, not just when a Pi is present.
- **hw_rpi** is the only TU that touches `/dev/mem`, the mailbox
  device, or any peripheral register. `pi_fm_rds --dry-run` still
  compiles and links `hw_rpi.c` but skips `hw_rpi_init` /
  `hw_rpi_start` / `hw_rpi_push_deltas` so the hot path is exercised
  without needing a Pi or root privileges.

## Source layout

| file                         | role                                                       |
|------------------------------|------------------------------------------------------------|
| `src/pi_fm_rds.c`            | CLI, orchestration, threads, signals, shutdown watchdog    |
| `src/rds.c` / `.h`           | RDS bit pump, group sequencer, CT group, 57 kHz LUT        |
| `src/fm_mpx.c` / `.h`        | FM multiplex: FIR LPF, 19/38 kHz carriers, stereo encoder  |
| `src/rds_strings.c` / `.h`   | PS/RT truncation, UTF-8 → RDS codepage                     |
| `src/waveforms.c` / `.h`     | Pre-baked biphase filter impulse response                  |
| `src/hw_rpi.c` / `.h`        | Pi DMA/PWM/GPCLK/mailbox driver                            |
| `src/mailbox.c` / `.h`       | VideoCore mailbox property-interface helper                |
| `src/control_pipe.c` / `.h`  | Runtime PS/RT/TA updates via a FIFO                        |
| `src/ring_spsc.h`            | Header-only SPSC float ring                                |
| `src/rds_internal.h`         | Test-only exposure of `rds_crc` / `rds_mjd`                |
| `src/{rds_crc,mjd,rds_group,rds_strings,ring_spsc}_test.c` | Unit tests, wired into `make test` |
| `src/rds_wav.c`              | Host-only: writes a wav of the MPX signal                  |
| `src/logging.h`              | `LOG_ERR`/`LOG_WARN`/`LOG_INFO`/`LOG_DBG` macros           |
| `src/pifm_common.h`          | `pifm_status_t`, shared `#define`s                         |
| `src/Makefile`               | Builds `libpifmrds.a`, `pi_fm_rds`, `rds_wav`, tests       |

## Threading model

Three threads cooperate at runtime:

| thread           | scheduling policy  | duties                                                                               |
|------------------|--------------------|--------------------------------------------------------------------------------------|
| `main`           | `SCHED_OTHER`      | signal handling, PS rotation, control-pipe polling, shutdown watchdog.               |
| DSP producer     | `SCHED_OTHER`      | `fm_mpx_ctx_get_samples` → SPSC ring. May block in libsndfile; that's fine.          |
| DMA feeder       | `SCHED_FIFO` (2)   | SPSC ring → frequency-delta conversion → `hw_rpi_push_deltas`. Must not page-fault.  |

The feeder is promoted to `SCHED_FIFO` (falling back to `SCHED_OTHER`
when `CAP_SYS_NICE` is unavailable). `mlockall(MCL_CURRENT|MCL_FUTURE)`
pins the pages so the real-time feeder never stalls on demand paging.

### Why three threads

Before the refactor the main loop was a hybrid DSP + DMA feeder that
used a fixed `usleep(5000)` as its clock and polled `DMA_CONBLK_AD`
for back-pressure. Two consequences:

1. A long `sf_read_float` could starve the DMA and cause audible glitches.
2. PS/RT rotation and control-pipe polling shared the same loop, so
   control-plane work stole time from the DSP tick.

With the split, audio decode and RF pacing live on separate threads
that communicate through a lock-free SPSC ring. The ring absorbs any
stall shorter than its capacity (~575 ms at 228 kHz), so bounded
audio-I/O jitter is invisible at RF.

## Pacing

The feeder wakes on a flat 5 ms `nanosleep` cadence (~200 Hz), matching
the pre-refactor `usleep(5000)` loop. Each wake it:

1. reads `DMA_CONBLK_AD` to compute how many sample slots the DMA has
   drained since the last refill,
2. pops up to that many floats from the SPSC ring,
3. converts each float to an integer frequency delta (`lrintf`) and
   writes the resulting divider word into the DMA sample ring.

The DMA itself paces at exactly 228 kHz through the PWM FIFO DREQ, so
the feeder's only job is to keep the sample ring ahead of the DMA
cursor. A half-filled ring (~25 k of 50 k slots) is typical at steady
state.

Earlier iterations of the refactor tried to wake the feeder only when
a watermark's worth of slots had drained (`hw_rpi_wait_space`), but
that turned out to return early whenever the DMA had overshot the
watermark, and the feeder ended up waking tens of thousands of times a
second and burning >100 % CPU on syscall overhead. A fixed 5 ms sleep
is simpler, cheaper, and empirically uses ~18 % CPU on a Pi 2
(vs ~30 % for the old single-thread loop, because the DSP work now
overlaps with the hw writes on a second core).

## SPSC ring

`src/ring_spsc.h` is a header-only, power-of-two, single-producer /
single-consumer ring of `float`. It uses `stdatomic.h` on ARMv7+ /
AArch64 and the `__sync_*` built-ins on ARMv6 (where `stdatomic.h`
can otherwise generate libatomic calls that add pointless overhead in
a hot loop). Capacity is 2^17 = 131 072 floats (~575 ms at 228 kHz),
sized so any bounded libsndfile or filesystem stall is invisible at
RF.

## Shutdown path

1. A signal handler (`SIGINT`, `SIGTERM`, `SIGHUP`, `SIGQUIT`) sets
   `g_terminate_requested = 1`. Only async-signal-safe operations run
   here. `--seconds N` expiry and a fatal DSP error route to the same
   flag.
2. The main loop notices the flag, calls `do_cleanup_and_exit`, which:
   a. arms a `SIGALRM`-based watchdog (`WATCHDOG_MS = 5000`) via
      `setitimer(ITIMER_REAL)`,
   b. posts both thread-wakeup semaphores,
   c. joins the DSP and feeder threads,
   d. stops hw_rpi (DMA reset, GPIO back to output, GPCLK disabled),
   e. closes libsndfile / control pipe,
   f. disarms the watchdog and exits normally.
3. If any step in (2) hangs for more than `WATCHDOG_MS`, SIGALRM fires
   the watchdog handler which issues `hw_rpi_reset_dma()` directly
   and `_exit()`s. The DMA engine therefore never leaks past the
   process, even if the cleanup path itself has a bug.
4. Fatal signals (`SIGSEGV`, `SIGBUS`, `SIGFPE`, `SIGILL`, `SIGABRT`)
   also route through `terminate_signal_handler`, registered with
   `SA_RESETHAND` so a repeat-fault on the same instruction falls
   through to the kernel default disposition and produces a core dump.
   The handler sets `g_terminate_requested`; whether the normal
   cleanup runs depends on which thread faulted, but the shutdown
   watchdog (point 3) is a last line of defence.

## Error handling

All public library functions return `pifm_status_t` (see
[src/pifm_common.h](src/pifm_common.h)):

- `PIFM_OK == 0` on success.
- `PIFM_ERR_*` negative on error.
- `PIFM_PIPE_*` positive for control-pipe events (so the CLI's
  old `> 0` discriminator keeps working).

## Tests

`make test` runs, in order:

1. `rds_strings_test` – PS/RT truncation and character-set handling.
2. `ring_spsc_test` – wrap-around, empty/full transitions.
3. `rds_crc_test` – full 65 536-entry sweep against an independent
   reference implementation of the CRC generator.
4. `mjd_test` – known-answer Modified Julian Dates across leap-year
   boundaries and the RDS epoch.
5. `rds_group_test` – determinism of the bit generator.

All tests link against `libpifmrds.a` only, so they run on any host.

## `--dry-run` and `--seconds`

`pi_fm_rds --dry-run` initialises the full pipeline (libsndfile,
ring, both threads) but skips the `hw_rpi_init` / `hw_rpi_start` /
`hw_rpi_push_deltas` calls and paces the feeder from a wall clock
instead. It is the recommended way to smoke-test DSP changes on a
non-Pi machine or to validate a build artefact before broadcasting.

`--seconds N` auto-exits after N wall-clock seconds. Combined with
`--dry-run` it gives CI a deterministic way to exercise the code path.
