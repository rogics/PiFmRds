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
     │ libsndfile I/O  │               │ clock_nanosleep  │
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
  device, or any peripheral register. A stub (`hw_stub.c`) satisfies
  the same ABI for host-side programs that don't drive hardware.

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
2. The polling was best-effort: a late wakeup could let the ring drain
   to zero, producing inter-sample "pops".

With the split, audio decode and RF pacing live on separate threads
that communicate through a lock-free SPSC ring. The ring absorbs any
stall shorter than its capacity (~575 ms at 228 kHz), so bounded
audio-I/O jitter is invisible at RF.

## SPSC ring

`src/ring_spsc.h` is a header-only, power-of-two, single-producer /
single-consumer ring of `float`. It uses `stdatomic.h` on ARMv7+ /
AArch64 and the `__sync_*` built-ins on ARMv6 (where `stdatomic.h`
can otherwise generate libatomic calls that add pointless overhead in
a hot loop).

## Shutdown path

1. A signal handler (`SIGINT`, `SIGTERM`, `SIGHUP`, `SIGQUIT`) sets
   `g_terminate_requested = 1`. Only async-signal-safe operations run
   here.
2. The main loop notices the flag, calls `do_cleanup_and_exit`, which:
   a. arms a `SIGALRM`-based watchdog (`WATCHDOG_MS = 5000`),
   b. posts both thread-wakeup semaphores,
   c. joins the DSP and feeder threads,
   d. stops hw_rpi (DMA reset, GPIO back to output, GPCLK disabled),
   e. closes libsndfile / control pipe,
   f. disarms the watchdog and exits normally.
3. If any step in (2) hangs for more than `WATCHDOG_MS`, SIGALRM fires
   the watchdog handler which issues `hw_rpi_reset_dma()` directly
   and `_exit()`s. The DMA engine therefore never leaks past the
   process, even if the cleanup path itself has a bug.

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
ring, both threads) but skips the hw_rpi init/start/push calls. It is
the recommended way to smoke-test DSP changes on a non-Pi machine or
to validate a build artefact before broadcasting.

`--seconds N` auto-exits after N wall-clock seconds. Combined with
`--dry-run` it gives CI a deterministic way to exercise the code path.
