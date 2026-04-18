# PiFmRds - Release Notes

This document summarises every bug fix, performance improvement, and
feature added on top of the upstream PiFmRds tree (last synced at
commit [`777f8e5`](../../commit/777f8e5) / [`c8b7754`](../../commit/c8b7754),
"Add support for Raspberry Pi Zero and Zero 2").

Changes are grouped by phase roughly in the order they landed. For
the architectural overview of the resulting code base, see
[`ARCHITECTURE.md`](ARCHITECTURE.md).

---

## Phase 0 — Tooling foundation

- Added `.gitattributes` to normalise line endings (LF in-repo, native
  in working tree) so edits on Windows no longer re-write every file.
- Added `.editorconfig` (4-space indents, LF, final newline) so any
  IDE/editor honours the project style.
- Added `.clang-format` mirroring the existing hand-rolled style, so
  `clang-format -i` is safe to run on any TU.

## Phase 1 — Header hygiene & const-correctness

- **Fix header-guard collision** between `rds.h` and `rds_strings.h`
  (both used `RDS_H`). Including both headers in a TU silently dropped
  one, which was the root cause of the crash described in
  review bug 1.23.
- Added missing `#include` guards to `fm_mpx.h`, `control_pipe.h`,
  `waveforms.h`.
- Fixed copy-pasted GPL banner comments in `control_pipe.{c,h}` and
  `fm_mpx.{c,h}` that still claimed to describe `rds_wav.c`.
- Removed redundant `extern` from every function prototype; added
  `(void)` to zero-argument prototypes.
- Made every string parameter `const`-correct
  (`rds_set_ps`, `rds_set_rt`, `rds_fill_string`, `control_pipe_open`,
  `fm_mpx_open`, `tx()`, `fatal()`).
- `waveform_biphase[576]` is now declared `const` with an explicit
  size; `generate_waveforms.py` emits matching declarations.

## Phase 2 — Stop leaking internal state across TUs

Every file-scope symbol that is not part of a module's public API is
now `static`, so the linker can no longer accidentally resolve a name
from one TU against another (review 1.20, 1.21):

- `fm_mpx.c`: `low_pass_fir`, `carrier_19`, `carrier_38`, `phase_*`,
  `downsample_factor`, `audio_buffer`, `audio_index`, `audio_len`,
  `audio_pos`, `fir_buffer_*`, `fir_index`, `channels`, `inf`, and
  `alloc_empty_buffer()` all `static`. The carrier tables are
  `static const`.
- `rds.c`: `rds_params` / `offset_words` / `crc()` /
  `get_rds_ct_group()` / `get_rds_group()` all `static`.
- `control_pipe.c`: `f_ctl` now `static`.
- `rds_strings.c`: `codepoint_to_rds_char()` now `static`.
- Introduced `src/pifm_common.h` with shared constants
  (`PS_LENGTH`, `RT_LENGTH`, `PS_BUF_SIZE`, `RT_BUF_SIZE`,
  `MPX_SAMPLE_RATE`) so callers stop re-defining them.

## Phase 3 — Bug fixes against the review backlog

| Review ID | Fix                                                                                                    |
|-----------|--------------------------------------------------------------------------------------------------------|
| 1.2       | `fatal()` now exits with `EXIT_FAILURE` so systemd / `\|\|` chains detect the failure.                 |
| 1.5       | Fixed the `channels == 0` bug in `fm_mpx_get_samples`; mono inputs no longer read past the buffer.     |
| 1.8       | `fm_mpx_close()` null-guards `sf_close` and `free`; double-close is now a no-op.                       |
| 1.12      | `control_pipe_poll()` bound-checks PS/RT length before writing the NUL; short inputs no longer scribble into the static buffer. |
| 1.18      | `rds_wav` write/close error messages now name the *output* file, not the input.                       |
| 2.4       | Replaced `(int)((floor)(dval))` with `lrintf(dval)` — true nearest-rounding, no double promotion.     |
| 2.5, 3.3, 6.15 | Dropped `<strings.h>` / `bzero` / local `#define PI` in favour of `<string.h>` / `memset` / `M_PI`. |

## Phase 4 — Safe signal handling

- Replaced the `for (i = 0; i < 64) sigaction(i, ...)` loop
  (review 1.1) with an explicit `install_signal_handlers()` that only
  hooks `SIGINT`, `SIGTERM`, `SIGHUP`, `SIGQUIT` (graceful) and
  `SIGSEGV`, `SIGBUS`, `SIGFPE`, `SIGILL`, `SIGABRT` (fatal, with
  `SA_RESETHAND`).
- The handler is now async-signal-safe: it only sets
  `g_terminate_requested` / `g_terminate_signal`; the real cleanup runs
  from the main loop in non-signal context.
- **Behaviour change**: `SIGPIPE` no longer tears down the transmitter
  on a closed stdin/stdout; piping audio into `--audio -` is now
  recoverable.

## Phase 5 — Robust control pipe

Rewrote `poll_control_pipe()` (review 1.13, 1.14):

- Non-blocking raw-`read()` line accumulator instead of `fgets()` over
  a non-blocking `FILE*`. No more partial-line data loss, sticky EOF,
  or silent truncation.
- The FIFO is automatically re-opened on writer close, so
  `cat > /tmp/rds` followed by a new `cat > /tmp/rds` just works.
- Over-long lines (>256 B) are flagged as "overflow" and discarded at
  the next `\n`; they cannot be mis-dispatched as valid commands.
- Command dispatch is now a static `{prefix, max_arg_len, handler}`
  table; adding a new command is one row.

## Phase 6 — `rds_wav` and mailbox clean-ups

- **`rds_wav`** (review 1.17): moved the ~456 kB `mpx_buffer` off the
  stack onto the heap — previously crashed under a 256 kB stack limit.
  Added a single `goto cleanup` path so partial failures still free
  the buffer and close the output.
- **`mailbox.c`** (review 1.15, 1.16):
  - `mapmem()` / `unmapmem()` no longer call `exit(-1)` on failure.
    Library functions aborting the whole process was a layering
    violation; they now return `NULL` and let the caller handle it.
  - Fixed a subtle `munmap` page-alignment bug: `mapmem` added a
    sub-page offset to the `mmap` return, but `unmapmem` passed the
    offset pointer straight to `munmap`, silently leaking the mapping
    on strict kernels. Now re-computes the base before unmapping.
  - Diagnostics go to stderr with `strerror(errno)`.
  - `unmapmem(NULL, 0)` is a no-op, so the cleanup path can run twice
    safely.

## Phase 7 — libsndfile frame accounting

- **Fixed the stereo MPX bug** (review 1.6): `sf_read_float` counts
  in *samples*, not *frames*, but the refill loop used `length`
  instead of `length * channels`. Stereo inputs were effectively
  half-sampled; the second half of each DSP chunk was uninitialised
  memory.
- Upgraded `audio_len` / `audio_index` from `int` to `sf_count_t`
  (review 1.7), enabling audio files >2 GB on 64-bit builds and
  eliminating sign/conversion warnings.

## Phase 8 — Modern Makefile

- Automatic dependency tracking (`-MMD -MP`, auto-included `*.d`):
  editing a header now rebuilds every TU that includes it.
- New phony targets: `clean`, `distclean`, `install`, `uninstall`,
  `asan`, `test`. `DESTDIR` / `PREFIX` support for packagers.
- Default warning flags raised to `-Wall -Wextra -Wwrite-strings
  -Wno-unused-parameter`.
- Per-model `-mtune` (review 2.9): `arm1176jzf-s` on Pi 1/Zero,
  `cortex-a7` on Pi 2, `cortex-a53` on Pi 3, `cortex-a72` on Pi 4.
- `-ffast-math` scoped to the DSP TUs only (`fm_mpx.o`, `rds.o`).
  The CRC and MJD integer math stays bit-exact.
- Host-only build falls back to `rds_wav` + tests when no Pi is
  detected.

## Phase 9 — Named constants, no more magic numbers

- `MPX_SAMPLE_RATE` (228 000), `AUDIO_LPF_CUTOFF_HZ`,
  `MPX_AUDIO_GAIN`, `MPX_STEREO_DIFF_GAIN`, `MPX_PILOT_GAIN`,
  `MPX_SCALE_DIV` all collected in `pifm_common.h`.
- BCM2835 clock-manager password (`0x5A << 24`) is now `CM_PASSWD`.
- The DMA start word `0x10880001` is now spelled out as
  `WAIT_FOR_OUTSTANDING_WRITES | PANIC_PRIORITY(8) | PRIORITY(8) |
  ACTIVE`, with the flag definitions also in the header. (While doing
  this we verified that it actually is *priority 8 / panic 8*, not
  the naive reading as *priority 8 / panic 1*.)

## Phase 10 — Consistent naming (`module_verb[_object]`)

Pure mechanical rename pass, now uniform across the public API:

| Old                  | New                 |
|----------------------|---------------------|
| `set_rds_pi`         | `rds_set_pi`        |
| `set_rds_ps`         | `rds_set_ps`        |
| `set_rds_rt`         | `rds_set_rt`        |
| `set_rds_ta`         | `rds_set_ta`        |
| `get_rds_samples`    | `rds_get_samples`   |
| `fill_rds_string`    | `rds_fill_string`   |
| `open_control_pipe`  | `control_pipe_open` |
| `close_control_pipe` | `control_pipe_close`|
| `poll_control_pipe`  | `control_pipe_poll` |
| `mem_alloc`          | `mbox_mem_alloc`    |
| `mem_free`           | `mbox_mem_free`    |
| `mem_lock`           | `mbox_mem_lock`    |
| `mem_unlock`         | `mbox_mem_unlock`  |

Also renamed the opaque local variables flagged by the review
(`count` → `ps_cycle_counter`, `myps` → `generated_ps`,
`ifbi`/`dfbi` → `fir_idx_forward`/`fir_idx_backward`, etc.).

## Phase 11 — Real command-line parser

Replaced the hand-rolled `strcmp`/`atof` chain with
`getopt_long_only()`:

- Canonical options: `--freq`, `--audio`, `--wav`, `--ppm`, `--pi`,
  `--ps`, `--rt`, `--ctl`. Legacy single-dash forms still work but
  emit a deprecation warning once per unique option.
- New: `-h` / `--help`, `-V` / `--version`.
- Proper input validation: `--freq` range-checked (76–108 MHz),
  `--pi` range-checked (0–0xFFFF), PS/RT length warnings.
- Typos are now rejected with a usage summary and a non-zero exit
  code instead of silently falling through.
- Version string is baked in from `git describe --tags --dirty
  --always` at build time.

## Phase 12 — Unified return codes

- New `pifm_status_t` enum in `pifm_common.h`:
  `PIFM_OK = 0`, `PIFM_ERR_{IO,MEM,HW,ARG}` negative,
  `PIFM_PIPE_{NO_CMD,PS_SET,RT_SET,TA_SET}` positive.
- Adopted as the return type of `fm_mpx_*`, `control_pipe_*`, and
  `tx()`. The numeric values are compatible with the old ad-hoc
  `0`/`-1`/`CONTROL_PIPE_*` conventions, so behaviour at the
  boundary is unchanged.
- `main()` now maps `PIFM_OK → EXIT_SUCCESS` and anything else to
  `EXIT_FAILURE`, preserving shell-level semantics.

## Phase 13 — Logging infrastructure

- New `src/logging.h` with `LOG_ERR` / `LOG_WARN` / `LOG_INFO` /
  `LOG_DBG` macros and a runtime-tunable `g_log_level`.
- `ERR` / `WARN` go to `stderr`, `INFO` / `DBG` to `stdout`.
- New CLI flags: `-v` / `--verbose` (repeatable, bumps verbosity),
  `-q` / `--quiet` (suppresses). Default output is byte-identical to
  previous versions so log scrapers keep working.
- The "Failed to open control pipe" message is now routed through
  `LOG_ERR` to stderr (review 6.10).

## Phase 14 — Residual correctness fixes

Closes the remaining section-1 entries of the review:

- **1.3**: `freq_ctl` is now computed as explicit integer plus 12-bit
  fractional parts in `double`, instead of through a `(float)` cast
  that silently dropped ~6 bits of precision.
- **1.4**: guard `DMA_CONBLK_AD` reads. The register can latch `0`
  between `BCM2708_DMA_RESET` and the first CB, or a partially-updated
  value outside the CB region. Now read twice and require agreement
  and in-range, else skip the iteration.
- **1.9**: corrected a stale `state == 5` comment in `rds.c` (the
  branch actually runs at `state == 4`).
- **1.10**: `tm_gmtoff` is glibc/BSD-specific; now wrapped in a
  feature test with a `timegm()`+`difftime()` fallback.
- **1.11**: replaced `(y*365.25)` / `(m*30.6001)` in the MJD
  calculation with exact integer forms (`×1461/4`, `×306001/10000`),
  removing the `-ffast-math` reordering risk called out in the
  review.
- **4.8**: `abs(offset)` replaced with an explicit sign split so
  `INT_MIN` can no longer be UB.

Also silenced `-Wsign-compare` (`FILTER_SIZE` / `SAMPLE_BUFFER_SIZE`
are now `int` at the macro level) and `-Wimplicit-fallthrough` in
the `getopt_long_only` switch. `terminate()` and
`do_cleanup_and_exit()` are now `__attribute__((noreturn))`, so GCC
can verify the `fatal()` control flow.

Build completes warning-free on Pi 2 with `-Wall -Wextra -std=gnu99`.

---

## Tier 2 — Architectural refactor

The big one. Decouples DSP from hardware and splits the main loop
into three cooperating threads so audio-decode stalls can no longer
starve the DMA.

### Library / driver split

- **`libpifmrds.a`** — archives the pure-DSP TUs (`rds.c`,
  `fm_mpx.c`, `rds_strings.c`, `waveforms.c`). Linked by
  `pi_fm_rds`, `rds_wav`, and every unit test, so the DSP code is
  exercised identically on and off a Pi.
- **`hw_rpi.{c,h}`** — new opaque driver handle wrapping every
  `/dev/mem` mmap, mailbox allocation, DMA control-block, PWM, and
  GPCLK access. Adds `hw_rpi_reset_dma()` as an
  async-signal-safe last-ditch cleanup.

### Context structs (no more file-scope globals)

- `fm_mpx_ctx_t` — every file-scope global in `fm_mpx.c` is now a
  context field.
- `rds_ctx_t` — every function-scope `static` in `rds.c` is now a
  context field.
- Classic singleton API (`rds_set_ps`, `fm_mpx_get_samples`, etc.) is
  retained as a thin wrapper over a lazily-initialised default
  context, so `rds_wav` and the existing CLI keep working unchanged.

### Threading model

Main + DSP producer (`SCHED_OTHER`) + DMA feeder (`SCHED_FIFO`,
falls back to `SCHED_OTHER` if `CAP_SYS_NICE` is unavailable),
connected by a header-only lock-free SPSC float ring
(`src/ring_spsc.h`, 131 072 floats ≈ 575 ms of slack). `mlockall`
pins the pages so the real-time feeder never stalls on demand
paging.

### Performance closures

- **§2.1** — doubled FIR ring buffer with branch-free symmetric
  fold. Auto-vectorises on ARMv7 / ARMv8 (NEON).
- **§2.3** — 57 kHz carrier is now a precomputed LUT; the per-sample
  switch is gone.
- **§2.6** — dead `SUBSIZE` / fractional-downsample code removed.
- **§2.8** — RDS output is now a single-pass sample buffer.

### Shutdown watchdog (review 6.11)

- 5 s `setitimer(ITIMER_REAL)` watchdog: if normal cleanup hangs for
  more than 5 s, `SIGALRM` fires, `hw_rpi_reset_dma()` runs directly,
  and `_exit()` terminates the process. The DMA engine can no longer
  leak past the process, even if cleanup itself has a bug.

### Tests

- `rds_crc_test` — full 65 536-entry CRC sweep against an independent
  reference implementation.
- `mjd_test` — known-answer Modified Julian Dates across leap-year
  boundaries, the RDS epoch, and the Unix epoch.
- `rds_group_test` — determinism smoke test (two identical contexts
  produce bit-exact output).
- `ring_spsc_test` — wrap-around / empty-full transitions.
- All wired into `make test`. All link against `libpifmrds.a` only, so
  they run on any host.

### CI / host support

- New `--dry-run` flag: skips `hw_rpi_init` / `_start` / `_push_deltas`
  but otherwise runs the full pipeline. Paces the feeder from a wall
  clock so the DSP and threading code path can be exercised without a
  Pi or root.
- New `--seconds N` flag: auto-exits after N wall-clock seconds.
  Combined with `--dry-run` this gives CI a deterministic smoke test.
- `pi_fm_rds` now builds on non-Pi hosts (defaults to `RASPI=1`),
  enabling the CI dry-run path.
- Added `ARCHITECTURE.md` documenting the threading model, pacing
  strategy, source layout, shutdown path, and SPSC-ring design.

---

## Tier 2 follow-ups

- **32-bit overflow fix** (`pi_fm_rds.c`): the dry-run `nanosleep`
  interval used `(long)DATA_SIZE * 1000000000L / 228000L`, which
  overflows 32-bit `long` on ARMv6/v7 and produced a ~0.66 s sleep
  instead of the intended ~21.9 ms. Now computed in `long long` and
  split into `tv_sec` / `tv_nsec`.
- **CPU regression fix**: the initial refactor used
  `hw_rpi_wait_space()` as a deadline-based pacing primitive, which
  returned immediately whenever the DMA had overshot the watermark.
  The feeder ended up iterating tens of thousands of times per second
  and burning >100 % CPU on syscall overhead. First iteration added a
  5 ms watermark; final iteration replaced the whole `wait_space`
  scheme with a flat 5 ms `nanosleep`, matching the pre-refactor
  `usleep(5000)` cadence. CPU usage on a Pi 2 dropped from **130 %
  → 18 %**.
- **Dead-code removal**: after the pacing rewrite,
  `hw_rpi_wait_space()`, `FEEDER_REFILL_WATERMARK`, and the
  never-linked `hw_stub.c` were removed. `ARCHITECTURE.md` updated to
  reflect the final design.

---

## Summary by category

### Correctness — bugs fixed

- Signal handler installed on every signal (`SIGPIPE` tore down the
  carrier on a closed pipe). *Phase 4.*
- Header-guard collision between `rds.h` and `rds_strings.h`.
  *Phase 1.*
- Mono-vs-stereo branch inverted in `fm_mpx_get_samples`. *Phase 3.*
- `sf_read_float` count was in samples, not frames, so stereo MPX
  read uninitialised memory on every second chunk. *Phase 7.*
- Carrier-offset sample was `floor()`-rounded, biasing every sample
  by up to −1 LSB. *Phase 3.*
- `fm_mpx_close` called `sf_close(NULL)` when no audio file was
  supplied. *Phase 3.*
- `control_pipe_poll` wrote 8 or 64 bytes past the end of a short
  PS/RT argument. *Phase 3.*
- `rds_wav` error messages named the wrong file. *Phase 3.*
- `fatal()` exited with 0, hiding failures from `systemd` and shell
  chains. *Phase 3.*
- `rds_wav` allocated 456 kB on the stack and crashed on tight-stack
  systems. *Phase 6.*
- `mapmem`/`unmapmem` called `exit(-1)` on failure, and `unmapmem`
  leaked non-page-aligned mappings. *Phase 6.*
- `freq_ctl` lost 6 bits of precision through an intermediate
  `(float)` cast. *Phase 14.*
- `DMA_CONBLK_AD` could latch `0` or an out-of-range value, producing
  nonsense sample indices. *Phase 14.*
- `(y*365.25) / (m*30.6001)` MJD calc was reordering-unstable under
  `-ffast-math`. *Phase 14.*
- `abs(offset)` is UB for `INT_MIN`. *Phase 14.*
- Stereo input silently half-sampled; the rest was garbage. *Phase 7.*
- `control_pipe` ate partial lines, hit sticky EOF after writer close,
  and silently truncated >100-byte commands. *Phase 5.*

### Performance

- Doubled FIR ring buffer → branch-free symmetric fold, NEON-
  auto-vectorisable. *Tier 2.*
- 57 kHz carrier LUT replaces the per-sample branch. *Tier 2.*
- Three-thread split: audio decode overlaps with RF pacing on
  separate cores. *Tier 2.*
- `-ffast-math` scoped to DSP TUs; CRC/MJD stay bit-exact. *Phase 8.*
- Per-model `-mtune` (arm1176jzf-s / cortex-a7 / a53 / a72). *Phase 8.*
- Dead `SUBSIZE`/fractional-downsample code removed. *Tier 2.*
- `lrintf` replaces `(int)(float)(double)floor(…)` in the hot sample
  loop. *Phase 3.*
- CPU usage on Pi 2: **~30 %** (original) → **~18 %** (final).

### New features

- `--help` / `--version`. *Phase 11.*
- `-v` / `-q` logging verbosity. *Phase 13.*
- `--dry-run` (host smoke test). *Tier 2.*
- `--seconds N` (CI auto-exit). *Tier 2.*
- Unified `pifm_status_t` return codes. *Phase 12.*

### Architecture & tooling

- `libpifmrds.a` / `hw_rpi` split. *Tier 2.*
- `fm_mpx_ctx_t` / `rds_ctx_t` contexts (no more file-scope globals).
  *Tier 2.*
- Lock-free SPSC ring (`stdatomic.h` on ARMv7+/v8, `__sync_*` on v6).
  *Tier 2.*
- Shutdown watchdog via `setitimer`, guarantees DMA reset on every
  exit path. *Tier 2.*
- Makefile: automatic dep tracking, `install` / `uninstall`,
  `asan`, `test`, host build. *Phase 8.*
- `.gitattributes` / `.editorconfig` / `.clang-format`. *Phase 0.*
- `logging.h` with runtime-tunable levels. *Phase 13.*
- `getopt_long_only` with proper validation. *Phase 11.*
- `ARCHITECTURE.md`, this file. *Docs.*

### Tests

- `rds_strings_test` (pre-existing, wired into `make test`). *Phase 8.*
- `rds_crc_test` — 65 536 cases. *Tier 2.*
- `mjd_test` — historical and future dates. *Tier 2.*
- `rds_group_test` — bit-generator determinism. *Tier 2.*
- `ring_spsc_test` — SPSC edge cases. *Tier 2.*

Build cleanly on Pi 1 / Zero / Zero 2 / 2 / 3 / 4 with no warnings at
`-Wall -Wextra`.
