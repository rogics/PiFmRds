# PiFmRds - Code Review & Improvement Suggestions

This document collects suggestions after a review of the C source files in
`src/`. Items are grouped by severity / category. Line numbers are given
as of the original revision against which the review was written (see git
history for the snapshot).

## Status (updated 2026-04-18)

Most of §1 (bugs) and §5 (readability/consistency) have been addressed
across phases 0–14. The Tier-2 architectural refactor
(pifm-tier2-architectural-refactor, April 2026) closed the remaining
high-impact items in §2, §6, and §8. See `ARCHITECTURE.md` for the
threading model and library/driver split.

Resolved so far:
- **§1 bugs:** 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9, 1.10, 1.11,
  1.12, 1.13, 1.14, 1.15, 1.16, 1.17, 1.18, 1.19, 1.20, 1.21, 1.22, 1.23
- **§2 performance:** 2.1 (doubled FIR ring buffer), 2.2 (deadline
  scheduling via `clock_nanosleep`), 2.3 (57 kHz LUT), 2.4 (round via
  `lrintf`), 2.6 (dead SUBSIZE/frac gone), 2.8 (RDS one-pass sample
  buffer — carrier LUT eliminates the per-sample branch; the
  read-and-zero accumulator remains but is now the only memory write
  per sample), 2.9 (`-mtune` per model)
- **§3 efficiency:** 3.3 (drop `<strings.h>`/`bzero`)
- **§4 elegance:** 4.2, 4.3, 4.5, 4.7, 4.8 (folded into 1.10 fix)
- **§5 readability:** 5.S1, 5.S2, 5.S3, 5.S4, 5.S5, 5.S6, 5.S7, 5.S11,
  5.S12 (partial), 5.S13, 5.S14, 5.S15, 5.S17, 5.S18, 5.S21, 5.S22
- **§6 engineering:** 6.1, 6.5, 6.7, 6.8, 6.10, 6.11 (shutdown
  watchdog + setitimer), 6.13, 6.14, 6.15
- **§8 refactors:** 8.1 (fm_mpx_ctx_t / rds_ctx_t), 8.2 (hw_rpi / hw_stub
  split), 8.3 (deadline-driven feeder via clock_nanosleep), 8.4
  (logging.h already existed; now propagated), 8.6 (--version and
  --help done, --dry-run and --seconds added for CI)

Still open (not fixed):
- **§2 performance:** 2.5, 2.7, 2.10
- **§3 efficiency:** 3.1 (WAVs in `src/`), 3.2 (`NUM_SAMPLES` sizing)
- **§4 elegance:** 4.1 (magic numbers, partial), 4.6, 4.9 (Makefile regen)
- **§5 readability:** 5.S8, 5.S9, 5.S10, 5.S16, 5.S19, 5.S20
- **§6 engineering:** 6.2, 6.3, 6.4 (partial via `.editorconfig` /
  `.clang-format`), 6.6, 6.9, 6.12, 6.16
- **§8 refactors:** 8.5 (kernel module -- explicitly deferred)

---

## 1. Bugs & Correctness Issues

### 1.1 `pi_fm_rds.c` — Signal handler installed for every signal including SIGKILL/SIGSTOP (and many reserved slots)

`src/pi_fm_rds.c:324`

```c
for (int i = 0; i < 64; i++) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = terminate;
    sigaction(i, &sa, NULL);
}
```

Problems:
- `SIGKILL` (9) and `SIGSTOP` (19) cannot be caught; `sigaction` silently
  returns `EINVAL`. Not harmful, but noisy and misleading.
- Signals such as `SIGCHLD`, `SIGURG`, `SIGWINCH`, `SIGPIPE` will be
  overridden to call `terminate()` — a broken pipe to stdout would kill
  the transmitter unexpectedly.
- Slots 32/33 are reserved by the glibc NPTL implementation; overwriting
  them breaks pthread cancellation (if pthreads were ever introduced).
- `terminate()` calls `printf`, `fclose`, `munmap`, `exit` — none of
  which are async-signal-safe. A signal that arrives mid-`malloc` can
  deadlock or corrupt state.

Suggested fix: handle only the signals that actually need cleanup
(`SIGINT`, `SIGTERM`, `SIGHUP`, `SIGQUIT`, `SIGSEGV`, `SIGBUS`,
`SIGFPE`, `SIGILL`, `SIGABRT`), set a `volatile sig_atomic_t` flag in
the handler, and do the real cleanup from the main loop.

### 1.2 `pi_fm_rds.c` — `terminate()` in `fatal()` passes exit code 0 on error

`src/pi_fm_rds.c:282`

```c
static void fatal(char *fmt, ...) {
    ...
    terminate(0);
}
```

A fatal error exits with status 0, masking errors from supervisors like
`systemd`, shell scripts, etc. Pass `1` (or `EXIT_FAILURE`).

### 1.3 `pi_fm_rds.c` — `freq_ctl` integer overflow / precision loss — **RESOLVED (phase 14)**

`src/pi_fm_rds.c:373`

```c
uint32_t freq_ctl = ((float)(PLLFREQ / carrier_freq)) * ( 1 << 12 );
```

- `PLLFREQ` is a `double` literal (`500000000.`), `carrier_freq` is
  `uint32_t`. The division is done in `double`, then narrowed to
  `float`. For a 107.9 MHz carrier the value ≈ 4.635, then
  multiplied by 4096 ≈ 18985. No overflow here, but the `(float)` cast
  throws away precision versus keeping it in `double` until the very
  end. This is the same computation the `idivider` / `fdivider` block
  does correctly a few lines below.
- Recommendation: compute integer + fractional parts explicitly (like
  lines 413–415) instead of relying on truncation.

### 1.4 `pi_fm_rds.c` — race in DMA pointer check when the DMA wraps — **RESOLVED (phase 14)**

`src/pi_fm_rds.c:511`

```c
size_t cur_cb = mem_phys_to_virt(dma_reg[DMA_CONBLK_AD]);
int last_sample = (last_cb - (size_t)mbox.virt_addr) / (sizeof(dma_cb_t) * 2);
int this_sample = (cur_cb - (size_t)mbox.virt_addr) / (sizeof(dma_cb_t) * 2);
int free_slots = this_sample - last_sample;
if (free_slots < 0) free_slots += NUM_SAMPLES;
```

If `DMA_CONBLK_AD` reads 0 (end of transfer) or a value that is not
within the allocated region (can happen briefly between DMA reset and
the first CB), `mem_phys_to_virt` returns a nonsensical pointer and the
subsequent division produces garbage. Guard with bounds check and retry,
or read `DMA_CONBLK_AD` twice and take the stable value.

### 1.5 `fm_mpx.c` — `channels == 0` check is impossible, real bug masked

`src/fm_mpx.c:196`

```c
if(channels == 0) {
    fir_buffer_mono[fir_index] = audio_buffer[audio_index];
} else {
    // stereo path ...
}
```

`sfinfo.channels` is always ≥ 1 for a valid file. The intent was clearly
`channels == 1` — as written, mono files take the *stereo* branch and
read `audio_buffer[audio_index+1]` which is out of bounds for a mono
frame (reading the next sample, shifting audio).

Fix:

```c
if (channels == 1) {
    fir_buffer_mono[fir_index] = audio_buffer[audio_index];
} else {
    // sum/difference
}
```

### 1.6 `fm_mpx.c` — `audio_buffer` sized without considering sample interleaving

`src/fm_mpx.c:147`

```c
audio_buffer = alloc_empty_buffer(length * channels);
```

Good — but `sf_read_float` in `fm_mpx_get_samples` asks for `length`
items, not `length * channels`, potentially leaving the buffer starved
of a full frame on stereo files. Read `length * channels` samples to
keep the invariant.

```c
audio_len = sf_read_float(inf, audio_buffer, length * channels);
```

Additionally `audio_index += channels` / `audio_len -= channels` is only
safe if the returned count is a multiple of `channels`
(`sf_read_float` does guarantee that for frame-based reads, but only if
the request is a multiple of `channels`).

### 1.7 `fm_mpx.c` — `audio_len` compared as `int` against return of `sf_read_float`

`sf_read_float` returns `sf_count_t` (64-bit signed). Storing it in an
`int` silently truncates on very large reads. Use `sf_count_t`.

### 1.8 `fm_mpx.c` — `fm_mpx_close()` crashes if `inf == NULL`

`src/fm_mpx.c:258`

```c
int fm_mpx_close() {
    if(sf_close(inf) ) {           // <-- inf may be NULL
        fprintf(stderr, "Error closing audio file");
    }
    if(audio_buffer != NULL) free(audio_buffer);
    return 0;
}
```

`sf_close(NULL)` is not specified as safe by libsndfile. Guard it, and
print a trailing `\n`:

```c
if (inf && sf_close(inf)) {
    fprintf(stderr, "Error closing audio file\n");
}
```

### 1.9 `rds.c` — `get_rds_group()` comment says state 5 but condition is `state < 4 / else` — **RESOLVED (phase 14)**

`src/rds.c:130`

```c
if(state < 4) { ... }
else { // state == 5
```

Comment says "state == 5", but the else branch is reached for state
`== 4` (because `state++; if(state>=5) state=0` wraps at 5). The PS/RT
pattern is 4 × 0A + 1 × 2A = pattern length 5. The comment should read
`state == 4`. Not a runtime bug, but misleading.

### 1.10 `rds.c` — `get_rds_ct_group` DST offset computation uses `tm_gmtoff` unconditionally — **RESOLVED (phase 14)**

`src/rds.c:106`

```c
utc = localtime(&now);
int offset = utc->tm_gmtoff / (30 * 60);
```

`tm_gmtoff` is a glibc/BSD extension; builds on musl may differ. Works
on Raspbian, but wrap with `#ifdef __USE_MISC` or provide a fallback
for portability. Also `localtime` reuses the static buffer previously
pointed to by `utc` — since we already read the fields, that's fine, but
consider `localtime_r`.

### 1.11 `rds.c` — Julian date formula has edge case for January/February — **RESOLVED (phase 14)**

`src/rds.c:97`

```c
int l = utc->tm_mon <= 1 ? 1 : 0;
int mjd = 14956 + utc->tm_mday +
                (int)((utc->tm_year - l) * 365.25) +
                (int)((utc->tm_mon + 2 + l*12) * 30.6001);
```

The algorithm is correct for the RDS spec's MJD, but intermediate
products in `double` then truncated to `int` can produce off-by-one
errors on some compilers with `-ffast-math` (which *is* enabled in the
Makefile). Prefer integer arithmetic:

```c
int y = utc->tm_year - l;
int m = utc->tm_mon + 2 + l*12;
int mjd = 14956 + utc->tm_mday + (y * 1461)/4 + (m * 306001)/10000;
```

### 1.12 `control_pipe.c` — Missing length checks before indexing

`src/control_pipe.c:70, 76`

```c
arg[8] = 0;    // PS — assumes arg has at least 9 bytes
arg[64] = 0;   // RT — assumes arg has at least 65 bytes
```

If the user writes `PS AB\n` (6 bytes), `arg` is only 3 bytes long, and
writing to `arg[8]` overflows the stack-resident `buf`. Use
`strnlen`/`strlen` before truncating:

```c
size_t n = strlen(arg);
if (n > 8) arg[8] = 0;
```

### 1.13 `control_pipe.c` — `poll_control_pipe()` reports "no command" the same as EOF and error

`fgets` returning NULL can mean EOF or EAGAIN; the function returns `-1`
in all cases, so the caller cannot tell "pipe closed" from "no data
yet". After the writer closes the pipe, subsequent reads will always
return NULL and the transmitter will never be able to receive PS/RT
updates again. Reopen the pipe when EOF is reached.

### 1.14 `control_pipe.c` — `fgets` may return a partial line in non-blocking mode

Non-blocking reads on pipes can split at any byte. The caller treats the
result as a full command. Accumulate into a local buffer and only
dispatch on newline.

### 1.15 `mailbox.c` — `exit(-1)` on failure inside a library function

`src/mailbox.c:51, 66, 77`

Library code should not unilaterally call `exit()`; it traps the caller
(`tx`) into never executing its cleanup path — ironically defeating the
whole point of the `terminate` handler. Return an error value and let
the caller decide.

### 1.16 `mailbox.c` — `unmapmem` doesn't undo `mapmem`'s alignment offset

`mapmem` returns `(char *)mem + offset`. `unmapmem(addr, size)` passes
`addr` (the user-facing pointer) directly to `munmap`, which requires
the page-aligned start. On most kernels this works because the offset
is 0, but on any peripheral base that isn't page-aligned this will
`EINVAL`.

### 1.17 `rds_wav.c` — `mpx_buffer[LENGTH]` is 456 KB on the stack

`src/rds_wav.c:78`

```c
float mpx_buffer[LENGTH];   // LENGTH = 114000 → 456000 bytes
```

Exceeds the default 8 MB pthread stack on Linux but is dangerously
close on constrained builds and blows `ulimit -s 256`. Allocate on the
heap with `malloc`/`free`.

### 1.18 `rds_wav.c` — error message prints input filename on a write/close error

`src/rds_wav.c:89, 95`

```c
fprintf(stderr, "Error: writing to file %s.\n", argv[1]);   // should be argv[2]
fprintf(stderr, "Error: closing file %s.\n", argv[1]);      // should be argv[2]
```

### 1.19 `pi_fm_rds.c` — `set_rds_ps(ps)` called before `set_rds_pi` verifies PS length

Nothing enforces that `ps` on the command line is ≤ 8 chars. `fill_rds_string`
truncates silently, but there's no warning to the user.

### 1.20 Implicit use of `inf`, `length`, `audio_buffer` etc. as globals — not thread-safe and not re-entrant

`fm_mpx.c` uses ~10 file-scope non-`static` globals. They're exported
symbols (violating encapsulation — another TU can clash) and make it
impossible to run two MPX generators. Either mark them all `static` or
pack them into a context struct that is passed to each function.

### 1.21 `rds.c` — `offset_words[]` is a non-`static` global and exported

`src/rds.c:59` — should be `static const`.

### 1.22 `waveforms.c` / `waveforms.h` — generated data should be `const`

Large tables should be `static const`; currently they go into `.data`
rather than `.rodata`, doubling their memory cost and losing the
Linker's ability to share them across processes. `waveforms.h`
declares `extern float waveform_biphase[576];` — should be
`extern const float ...`.

### 1.23 `rds_strings.h` — include guard name collides with `rds.h`

`src/rds_strings.h:21-22`

```c
#ifndef RDS_H
#define RDS_H
```

`rds.h` uses the *same* `RDS_H` guard. Today no translation unit
includes both, but the moment one does, the second header is silently
skipped and its declarations disappear, producing confusing
`implicit-declaration` warnings or, worse, UB at link time. Rename to
`RDS_STRINGS_H`.

---

## 2. Performance

### 2.1 FIR filter in `fm_mpx_get_samples` is the hot loop — vectorize

The inner loop runs **228 000 times / second × 30 taps = 6.84 M mul-adds/s**
just for the mono path, doubling for stereo. On ARMv6 with
`-ffast-math` the compiler *might* vectorize, but the double-ended
ring buffer traversal defeats auto-vectorization:

```c
int ifbi = fir_index;
int dfbi = fir_index;
for (int fi = 0; fi < FIR_HALF_SIZE; fi++) {
    dfbi--; if (dfbi < 0) dfbi = FIR_SIZE - 1;
    out_mono += low_pass_fir[fi] * (fir_buffer_mono[ifbi] + fir_buffer_mono[dfbi]);
    ...
    ifbi++; if (ifbi >= FIR_SIZE) ifbi = 0;
}
```

Improvements:
- Use a **doubled** ring buffer (`fir_buffer_mono[2*FIR_SIZE]`) so the
  symmetric access is contiguous and no modulo/branch is needed.
- Pre-compute `fir_buffer_mono[ifbi] + fir_buffer_mono[dfbi]` into a
  single temporary to help the scheduler.
- Compile with `-O3 -ftree-vectorize -mfpu=neon-vfpv4` on RPi 2/3/4
  (NEON) — the current Makefile uses `-mfpu=vfp`.

### 2.2 Busy-wait loop with `usleep(5000)` in main loop

`src/pi_fm_rds.c:509`

```c
usleep(5000);
size_t cur_cb = mem_phys_to_virt(dma_reg[DMA_CONBLK_AD]);
...
```

5 ms is arbitrary. When `free_slots` is large (after a stall) we do a
lot of work, then sleep 5 ms regardless of backlog. A better pattern is
to compute how long until next slot opens up and sleep that duration,
or use `poll()` on an `eventfd` the DMA interrupt fires (requires
kernel driver).

### 2.3 `get_rds_samples` uses `switch(phase)` in the inner loop

`src/rds.c:223` — on every sample (228 kHz), a 4-way `switch` evaluates.
Replace with a precomputed `multiplier[4]` array:

```c
static const float carrier57[4] = {0.f, 1.f, 0.f, -1.f};
sample *= carrier57[phase];
phase = (phase + 1) & 3;
```

### 2.4 `pi_fm_rds.c` main loop — branchy `int intval = (int)floor(dval);`

`src/pi_fm_rds.c:533`

```c
int intval = (int)((floor)(dval));
```

`floor()` is a library call unless inlined. For finite positive/negative
floats, a plain cast with `(int)dval` rounds toward zero; use
`lrintf(dval)` or `__builtin_lroundf` to get proper rounding with a
single instruction on ARM. The parenthesized `(floor)(dval)` specifically
disables potential macro/inlining.

### 2.5 `fm_mpx.c` — `cos`, `sin`, `PI` math

`src/fm_mpx.c:35, 133`

- `PI` is redefined; use `M_PI` from `<math.h>`.
- The carrier tables `carrier_19[]`, `carrier_38[]` use `double`
  literals but the variable is declared `float` — the compiler will
  emit double→float conversions unless `-ffast-math` is set. Mark
  literals with `f` suffix.

### 2.6 RDS waveform interpolation is not done (`frac` is commented out)

`src/pi_fm_rds.c:534-537`:

```c
//int frac = (int)((dval - (float)intval) * SUBSIZE);
ctl->sample[last_sample++] = (0x5A << 24 | freq_ctl) + intval; //(frac > j ? intval + 1 : intval);
```

`SUBSIZE == 1`, so no interpolation happens. Either remove the dead
comment *and* the `SUBSIZE` macro, or implement the fractional update —
it would significantly lower quantization noise in the transmitted
signal.

### 2.7 `rds.c` — bit buffer stored as `int` rather than packed bits

`bit_buffer[BITS_PER_GROUP]` = 104 ints = 416 bytes to store 104 bits.
Could be a single `uint64_t` plus `uint64_t` for headroom, halving the
cache footprint. Not critical, but elegant.

### 2.8 `get_rds_samples` — zeros the output buffer slot via write then re-accumulates

`src/rds.c:215`

```c
float sample = sample_buffer[out_sample_index];
sample_buffer[out_sample_index] = 0;
```

Good pattern, but combined with `sample_buffer[idx++] += val` above,
it means every sample position is read/written twice. Consider a plain
circular convolution with a precomputed template lookup.

### 2.9 Makefile uses `-mtune=arm1176jzf-s` for armv7 and aarch64

`src/Makefile:27`

```makefile
ARCH_CFLAGS = -march=armv7-a -O3 -mtune=arm1176jzf-s ...
```

`arm1176jzf-s` is the Pi 1 core. For Pi 2/3 use `-mtune=cortex-a7`
(Pi 2) or `-mtune=cortex-a53` (Pi 3/Zero 2). Wrong tune costs maybe
3–5 % but it's free to fix.

### 2.10 `-ffast-math` turned on globally

This is risky: it allows the compiler to assume no NaN/Inf, reorder FP
ops, and drop signed zero. The MJD calculation (1.11) and the FIR
filter (2.1) could silently change behavior. Consider scoping it to
only the DSP TUs.

---

## 3. Efficiency / Memory

### 3.1 Large static WAV files shipped in `src/`

`sound.wav` 1.9 MB, `stereo_44100.wav` 1.2 MB, `pulses.wav` 900 KB,
etc. — total ≈ 4 MB. These should be in an `assets/` or `examples/`
directory, not on the compile path, and probably served from a release
tarball instead of being tracked in git. Blame every clone for
downloading them.

### 3.2 `NUM_SAMPLES = 50000` sample ring buffer = ~200 KB

This is pinned physical memory (`mem_alloc` from VideoCore). Could be
halved with no latency penalty. Worth profiling.

### 3.3 Duplicate `#include` of `<strings.h>` (BSD) when `<string.h>` already included

`src/fm_mpx.c:29` uses `bzero` from `<strings.h>`. Prefer `memset`
(standard C) and drop the extra header.

---

## 4. Elegance / Readability

### 4.1 Replace "magic numbers" with named constants

Examples:
- `0x5A` (clock manager password) — `#define CM_PASSWD (0x5A << 24)`
- `228000` — `#define MPX_SAMPLE_RATE 228000`
- `57000` (RDS subcarrier) implied by `228000/4`
- `0x10880001` DMA CS value — unpack into named flags
- `4.05`, `.9` MPX scaling — `#define MPX_MONO_GAIN 4.05f`

### 4.2 Inconsistent indentation / mixed tabs & spaces

Files mix 4-space and tab indentation (e.g. `control_pipe.c` uses both
within the same function). Run `clang-format` with a project-level
`.clang-format`.

### 4.3 Inconsistent function declaration style

- `int tx(...)` — K&R placement
- `static void terminate(int num)` — K&R placement, but split across
  two lines (`static void\nterminate(int num)`)
- `int fm_mpx_open(char *filename, size_t len)` — one-liner

Pick one (preferably the one-line ANSI style).

### 4.4 Argument parser is ad-hoc

`src/pi_fm_rds.c:561` — hand-rolled `strcmp` chain. `getopt_long` is in
glibc, handles `--help`, abbreviated options, and free-ordered
arguments for free. Also rejects `-freqq 100` as unknown, not as a
typo on `-freq`.

### 4.5 Exit codes

`tx()` returns 1 on `fm_mpx_open` failure, otherwise runs forever. The
return value is then passed to `terminate(errcode)` which `exit`s with
it. Use `EXIT_SUCCESS` / `EXIT_FAILURE` consistently.

### 4.6 `fatal("...%m...")` uses glibc `%m` extension

Fine on Linux/glibc (which is the target), but document or prefer
`strerror(errno)` for portability.

### 4.7 `char *rt = "PiFmRds: live FM-RDS ..."` — should be `const char *`

Several `char *` parameters point to string literals; `-Wwrite-strings`
flags these. Change signatures to `const char *`.

### 4.8 `rds.c`: `blocks[3] |= abs(offset)` — `abs(INT_MIN)` UB — **RESOLVED (phase 14)**

Offset can't reach `INT_MIN` in practice, but the pattern is fragile.
Use a simple `if/else` split.

Folded into the 1.10 fix: sign is now split explicitly instead of via `abs()`.

### 4.9 `generate_pulses.py` / `generate_waveforms.py` regeneration not hooked into Makefile

There's no rule to regenerate `waveforms.c` from the Python scripts, so
the derived file and its source can drift. Add a target:

```makefile
waveforms.c: generate_waveforms.py
	python3 generate_waveforms.py > waveforms.c
```

---

## 5. Simplicity, Consistency & Readability

This section focuses purely on cognitive load — code that works but is
harder to read, review, or modify than it needs to be.

### 5.S1 Inconsistent module naming convention

Three subsystems, three different naming schemes for public functions:

| Module          | Prefix pattern              |
|-----------------|-----------------------------|
| `rds.c`         | `set_rds_pi`, `get_rds_samples` (`verb_module_noun`) |
| `fm_mpx.c`      | `fm_mpx_open`, `fm_mpx_close` (`module_verb`)        |
| `control_pipe.c`| `open_control_pipe`, `close_control_pipe` (`verb_module`) |
| `mailbox.c`     | `mbox_open`, `mem_alloc` (mixed `mbox_`/`mem_`)      |

Pick one — ideally `<module>_<verb>` (e.g. `rds_set_pi`,
`control_pipe_open`) — and apply uniformly. This is the single biggest
readability win with almost no behavior risk.

### 5.S2 `extern` keyword inconsistently used on prototypes

`rds.h` and `fm_mpx.h` mark function prototypes `extern`; `control_pipe.h`
does too; `rds_strings.h` does too; but `mailbox.h` does not. The
`extern` on function declarations is redundant (functions have external
linkage by default). Remove it everywhere for consistency.

### 5.S3 Inconsistent brace / spacing style across files

- `control_pipe.c` uses **tabs** for some lines and **4 spaces** for
  others in the *same function* (e.g. `src/control_pipe.c:42-53`).
- `pi_fm_rds.c` mostly uses 4 spaces.
- `fm_mpx.c` uses 4 spaces but trailing whitespace lingers.
- `if(cond)` vs `if (cond)` — both are used, sometimes in adjacent
  lines.
- `for(int i=0; i<N; i++)` vs `for (int i = 0; i < N; i++)` — mixed.

A one-time `clang-format -i src/*.c src/*.h` with a committed
`.clang-format` (LLVM or WebKit preset) fixes this permanently.

### 5.S4 Terse/cryptic identifiers in `pi_fm_rds.c`

`src/pi_fm_rds.c:462-463, 448`

```c
uint16_t count = 0;
uint16_t count2 = 0;
...
size_t last_cb = (size_t)ctl->cb;
```

`count` / `count2` convey nothing. Rename to `ps_cycle_counter`,
`ps_numeric_counter`, `last_cb_virt_addr`. Similarly `myps` →
`generated_ps`.

### 5.S5 `ifbi` / `dfbi` abbreviations

`src/fm_mpx.c:216-217`

```c
int ifbi = fir_index;  // ifbi = increasing FIR Buffer Index
int dfbi = fir_index;  // dfbi = decreasing FIR Buffer Index
```

The comment is only required because the names are cryptic. Expand to
`fir_idx_forward`, `fir_idx_backward` and drop the comment.

### 5.S6 Magic return codes instead of named enums

Multiple functions use ad-hoc integer returns:

- `open_control_pipe` returns `0` / `-1`.
- `poll_control_pipe` returns `-1` / `CONTROL_PIPE_PS_SET` /
  `CONTROL_PIPE_RT_SET` / `CONTROL_PIPE_TA_SET` — but
  `CONTROL_PIPE_NO_CMD` (or similar) is *not* defined; callers compare
  against `-1` literally.
- `fm_mpx_open` returns `0` / `-1`.
- `fm_mpx_get_samples` returns `0` / `-1`.
- `tx()` returns `0` / `1`.
- `mem_alloc` returns `0` on failure — but `0` is also a legitimate
  handle value on some firmware.

Define a single enum in a shared header:

```c
typedef enum {
    PIFM_OK = 0,
    PIFM_ERR_IO = -1,
    PIFM_ERR_MEM = -2,
    PIFM_ERR_HW = -3,
    PIFM_PIPE_NO_CMD = 1,
    PIFM_PIPE_PS_SET,
    PIFM_PIPE_RT_SET,
    PIFM_PIPE_TA_SET,
} pifm_status_t;
```

### 5.S7 Duplicated literal sizes

PS length `8` appears as a bare literal in 5 places; `9` (PS + NUL)
appears in 3:

- `rds.c:31` — `#define PS_LENGTH 8` ✓
- `control_pipe.c:70` — `arg[8] = 0;` ✗
- `pi_fm_rds.c:459` — `char myps[9] = {0};` ✗
- `pi_fm_rds.c:494` — `snprintf(myps, 9, ...)` ✗
- `rds.c:246` — `fill_rds_string(rds_params.ps, ps, 8);` ✗

Same for RT length `64`:

- `rds.c:30` — `#define RT_LENGTH 64` ✓
- `control_pipe.c:76` — `arg[64] = 0;` ✗
- `rds.c:242` — `fill_rds_string(rds_params.rt, rt, 64);` ✗

Expose `PS_LENGTH` / `RT_LENGTH` from a shared header and use them
everywhere.

### 5.S8 `rds.c:get_rds_group` — two near-identical bit-shift loops

`src/rds.c:153-160`

```c
for(int j=0; j<BLOCK_SIZE; j++) {
    *buffer++ = ((block & (1<<(BLOCK_SIZE-1))) != 0);
    block <<= 1;
}
for(int j=0; j<POLY_DEG; j++) {
    *buffer++= ((check & (1<<(POLY_DEG-1))) != 0);
    check <<= 1;
}
```

Extract a helper:

```c
static int *emit_bits_msb_first(int *dst, uint16_t word, int nbits) {
    for (int i = nbits - 1; i >= 0; i--) *dst++ = (word >> i) & 1;
    return dst;
}
```

Then the two loops collapse to two one-liners and the intent is obvious.

### 5.S9 `rds.c:crc` — loop structure mirrors typical CRC idiom but is 13 lines

The body of `crc()` can be written in 4 lines using the standard LFSR
idiom:

```c
static uint16_t crc(uint16_t block) {
    uint16_t crc = 0;
    for (int i = BLOCK_SIZE - 1; i >= 0; i--) {
        int fb = ((crc >> (POLY_DEG - 1)) ^ (block >> i)) & 1;
        crc = (crc << 1) & ((1u << POLY_DEG) - 1);
        if (fb) crc ^= POLY;
    }
    return crc;
}
```

Makes it instantly recognizable to anyone familiar with CRC code.

### 5.S10 `rds_strings.c:codepoint_to_rds_char` — 200-line switch

A single `switch` with 230 case labels is correct and fast (the
compiler builds a binary search or jump table) but painful to review
and maintain. Replace with a sorted data-driven table:

```c
static const struct { uint16_t cp; uint8_t rds; } map[] = {
    {0x000A, 0x0A}, {0x000B, 0x0B}, ...
};
// binary search by cp
```

For the ASCII range `0x20..0x7D` the mapping is identity; special-case
it and skip the table lookup entirely, shrinking both code and runtime
cost.

### 5.S11 Inconsistent parameter types: `char *` vs `const char *`

Every string-input parameter is `char *` although none of the
functions modify the string:

- `set_rds_ps(char *ps)` → `set_rds_ps(const char *ps)`
- `set_rds_rt(char *rt)`
- `fill_rds_string(char* rds_string, char* src_string, size_t ...)` —
  here `src_string` is the source, should be `const char *`.
- `open_control_pipe(char *filename)`
- `fm_mpx_open(char *filename, size_t len)`
- `tx(uint32_t, char *audio_file, uint16_t, char *ps, char *rt, float, char *control_pipe)`

The default `char *rt = "PiFmRds: live..."` literal in `main()` is a
string literal, assigned to a non-`const` pointer, which is undefined
behavior to write through. Compile with `-Wwrite-strings`.

### 5.S12 Inconsistent integer width for sizes / indices

- `length` is `size_t` (global) but `audio_len`, `audio_index`,
  `fir_index` are `int`.
- `data_len`, `data_index`, `last_sample`, `this_sample`, `free_slots`
  in `pi_fm_rds.c` are all `int` — but the expressions they derive from
  (`(last_cb - (size_t)mbox.virt_addr) / ...`) are `size_t`. The
  implicit narrowing fires `-Wconversion`.

Use `size_t` for counts and `ptrdiff_t` for signed differences
throughout.

### 5.S13 Dead code left commented in place

- `src/rds.c:112` — `//printf("Generated CT: ...`
- `src/fm_mpx.c:139-144` — debug FIR coefficient print loop
- `src/pi_fm_rds.c:534` — `//int frac = (int)((dval - (float)intval) * SUBSIZE);`
- `src/pi_fm_rds.c:537` — `//(frac > j ? intval + 1 : intval);`

Either delete it (git history remembers) or gate behind
`#ifdef DEBUG`. Comment-out debug code accumulates noise and
occasionally gets re-enabled accidentally.

### 5.S14 Stale/misleading comments

- `src/rds.c:137` — `// state == 5` (really state == 4, see §1.9).
- `src/fm_mpx.c:38-39`:
  ```c
  #define FIR_HALF_SIZE 30
  #define FIR_SIZE (2*FIR_HALF_SIZE-1)
  ```
  `FIR_SIZE = 59`, which is odd, so "half size" is actually
  `(FIR_SIZE + 1) / 2` not `FIR_SIZE / 2`. The name suggests the
  filter has `2 * 30 = 60` taps but it has 59. Rename to
  `FIR_TAPS_HALF_PLUS_1` or rewrite as
  `#define FIR_TAPS 59; #define FIR_HALF ((FIR_TAPS+1)/2)`.
- `src/fm_mpx.c:127` — "divide this coefficient by two because it will
  be counted twice" — but it is *not* counted twice when using the
  symmetric folding pattern at line 222. Verify or remove.
- `src/control_pipe.c:7-8` — file-header comment says
  "rds_wav.c is a test program..." — copy-paste from `rds_wav.c`.
  Same bug in `fm_mpx.h`, `control_pipe.h`, `fm_mpx.c` — **every**
  header has the wrong top-of-file description. This is a red flag for
  reviewers.

### 5.S15 `TODO` left unresolved

`src/pi_fm_rds.c:345`

```c
// TODO: How do we know that succeeded?
```

Either investigate (the VC firmware spec says `mem_alloc` returns the
handle or 0 on failure, which the code already checks) and delete the
TODO, or file an issue.

### 5.S16 Spurious extra whitespace / blank lines

- Triple blank lines in `pi_fm_rds.c` between sections (lines 356-370,
  438-446).
- Trailing whitespace at end of many lines in `fm_mpx.c` and `rds.c`.
- Mixed `\r\n` / `\n` — confirm with `git ls-files --eol`.

Add a `.gitattributes`:

```
*.c text eol=lf
*.h text eol=lf
*.py text eol=lf
```

### 5.S17 Boolean expressions that could be simplified

- `src/rds.c:67` — `int bit = (block & MSB_BIT) != 0;` — idiomatic, OK.
- `src/rds.c:154, 158` — `((block & (1<<(BLOCK_SIZE-1))) != 0)` — the
  explicit `!= 0` is redundant inside an integer context (`*buffer++`
  is `int`); a plain shift right is clearer (see §5.S8).
- `src/control_pipe.c:82` — `int ta = ( strcmp(arg, "ON") == 0 );` —
  fine, but the `!= 0` / `== 0` discipline is applied inconsistently
  elsewhere. Pick a style and stick with it.

### 5.S18 `pi_fm_rds.c:main` — argument parsing has repetitive boilerplate

Each option is a 3-line block with the same `i++; x = param;` ritual.
Move to a data-driven table or `getopt_long`. Current size: 35 lines;
with `getopt_long`: ~15 lines and free `--help`/`--long-options`.

### 5.S19 Mixed printf format conventions

- Some messages end with a period + `\n` (`"Could not allocate memory.\n"`)
- Some have no period (`"Using stdin for audio input.\n"`)
- Some lack newline (`src/fm_mpx.c:260`: `"Error closing audio file"`)

Unify. Tip: a project-wide `LOG_ERR("...")` macro that appends `\n`
solves this automatically.

### 5.S20 `rds_wav.c:main` — return inside loop leaks `outf`

`src/rds_wav.c:91`

```c
if(sf_write_float(outf, mpx_buffer, LENGTH) != LENGTH) {
    fprintf(stderr, "Error: writing to file %s.\n", argv[1]);
    return EXIT_FAILURE;   // outf and mpx_buffer leaked, fm_mpx never closed
}
```

Use `goto cleanup;` pattern or a flag + break.

### 5.S21 Hidden coupling via globals across TUs

`fm_mpx.c` exports (non-`static`) `phase_19`, `phase_38`,
`audio_buffer`, `audio_index`, `audio_len`, `audio_pos`,
`fir_buffer_mono`, `fir_buffer_stereo`, `fir_index`, `channels`,
`inf`, `length`, `downsample_factor`, `low_pass_fir[]`, `carrier_19[]`,
`carrier_38[]`. Any other TU that declares the same symbol at
file scope will clash at link time — or worse, silently share state.

All of these are private; add `static`.

### 5.S22 `control_pipe.c` — dispatch chain should be a table

`src/control_pipe.c:69-87` is three near-identical `if` branches. A
small table of `{cmd, max_len, setter}` tuples reduces repetition and
makes adding commands (PTY, AF, etc.) a one-line change:

```c
static const struct {
    const char *cmd;
    size_t max_len;
    void (*setter)(const char *);
    int status;
} cmds[] = {
    {"PS", 8,  (void(*)(const char*))set_rds_ps, CONTROL_PIPE_PS_SET},
    {"RT", 64, (void(*)(const char*))set_rds_rt, CONTROL_PIPE_RT_SET},
    ...
};
```

---

## 6. Engineering Practice

### 6.1 No `#pragma once` / include guards in headers

`fm_mpx.h`, `control_pipe.h` lack include guards. `rds.h` has them,
but `rds_strings.h` reuses the same guard name (see §1.23).
Add unique guards everywhere for consistency.

### 6.2 No unit tests except `rds_strings_test.c`

- RDS CRC has no test vector verification.
- `get_rds_group` could be tested against known-good reference packets.
- The MJD computation is easy to unit-test.
- `fm_mpx` FIR can be tested with impulse input → known impulse response.

### 6.3 No continuous integration

No `.github/workflows`, no Travis/Circle. A simple build-only CI
(cross-compile for armv6/armv7/aarch64) would catch Makefile drift and
header-guard omissions before merge.

### 6.4 No `clang-format`, no `.editorconfig`

See 4.2 / 5.S3.

### 6.5 `Makefile` — no `install` / `uninstall` targets, no `DESTDIR`, no `PREFIX`

Packagers have to hand-copy the binary. Standard pattern:

```makefile
PREFIX ?= /usr/local
install: app
	install -Dm755 pi_fm_rds $(DESTDIR)$(PREFIX)/bin/pi_fm_rds
```

### 6.6 `Makefile` — hard dependency on `/proc/device-tree/model`

`cat /proc/device-tree/model` is silently empty on non-Pi systems,
leading to `RPI_VERSION := ` and then `TARGET = other`, which *skips*
the `app` target entirely without warning. Print a clear diagnostic:

```makefile
ifeq ($(RPI_VERSION),)
  $(warning Not running on a Raspberry Pi — skipping FM transmitter build)
endif
```

### 6.7 `Makefile` — `clean` rule is incomplete

```makefile
clean:
	rm -f *.o *_test
```

Does not remove the binaries `pi_fm_rds`, `rds_wav`. Also no
`distclean`.

### 6.8 No dependency tracking

If `rds.h` changes, `pi_fm_rds.o` is *not* rebuilt automatically
(object rules list some deps manually but not transitively). Use
`-MMD -MP` and include the `.d` files:

```makefile
CFLAGS += -MMD -MP
-include $(wildcard *.d)
```

### 6.9 No runtime GPIO safety check

Running on the wrong Pi model writes to hardware addresses that may not
be the clock manager. Adding a `/proc/device-tree/model` check at
startup and refusing to run on mismatched hardware would be safer.

### 6.10 Error messages go to `stdout` for success and `stderr` for failure inconsistently

`src/pi_fm_rds.c:482` — "Failed to open control pipe" goes to `stdout`.
It should go to `stderr`.

### 6.11 No shutdown timeout

If the audio input blocks in `sf_read_float` (for example reading from
stdin that a parent process stalled), `SIGINT` will fire the signal
handler which tries to `fm_mpx_close()` which calls `sf_close()` which
can block. A watchdog `alarm(5)` in the handler or non-blocking close
would help.

### 6.12 Licensing consistency

`mailbox.[ch]` is BSD, rest of the codebase is GPLv3. This is
compatible, but the `LICENSE` file is pure GPL — add a note that
`mailbox.[ch]` is BSD and retains its original copyright notice.

### 6.13 `README.md` outdated relative to `-ctl` / `-audio` / `-wav` aliases

The parser accepts both `-wav` and `-audio` (see
`src/pi_fm_rds.c:567`), but the help message printed at
line 592 only shows `-audio`. Consider either dropping `-wav` (deprecated
alias) or documenting it.

### 6.14 No valgrind / ASAN build target

`fm_mpx_close()` leaks `audio_buffer` on the error path where
`fm_mpx_open` returns early (because it sets `inf` but never frees the
in-progress allocations). An ASan build target would have caught this:

```makefile
asan: CFLAGS += -fsanitize=address -g -O1
asan: LDFLAGS += -fsanitize=address
asan: rds_wav
```

### 6.15 Use of `bzero` (deprecated in POSIX.1-2008)

`src/fm_mpx.c:77`. Replace with `memset(p, 0, n)` which is ISO C.

### 6.16 `rds_wav.c` — no bounds on number of frames written

Hardcoded `for(int j=0; j<40; j++)` → 40 × 114000 = 4.56 M samples =
~20 s at 228 kHz. Make the count a CLI argument.

---

## 7. Concrete Small Cleanups (low-risk patches)

| # | File:Line | Change |
|---|-----------|--------|
| C1 | `pi_fm_rds.c:282` | `terminate(0)` → `terminate(EXIT_FAILURE)` in `fatal()` |
| C2 | `pi_fm_rds.c:533` | `(int)((floor)(dval))` → `lrintf(dval)` |
| C3 | `fm_mpx.c:196`    | `channels == 0` → `channels == 1` |
| C4 | `fm_mpx.c:258`    | Guard `sf_close(inf)` with `inf != NULL` |
| C5 | `fm_mpx.c:77`     | `bzero` → `memset` |
| C6 | `fm_mpx.c:35`     | Drop `#define PI`, use `M_PI` |
| C7 | `rds_wav.c:89,95` | `argv[1]` → `argv[2]` in error messages |
| C8 | `control_pipe.c:70,76` | Add length checks before `arg[N] = 0` |
| C9 | `rds.c:59`        | `uint16_t offset_words[]` → `static const uint16_t offset_words[]` |
| C10 | `fm_mpx.c` globals | Add `static` to globals |
| C11 | `pi_fm_rds.c:324` | Handle only real signals, not 0..63 |
| C12 | `Makefile`        | Add `-MMD -MP`, `install`, proper `clean` |
| C13 | Headers without guards | Add `#ifndef FOO_H / #define FOO_H / #endif` |
| C14 | `rds_strings.h:21` | Rename guard `RDS_H` → `RDS_STRINGS_H` to avoid collision with `rds.h` |
| C15 | `waveforms.h:7`    | `extern float waveform_biphase[576]` → `extern const float ...` |
| C16 | All headers (file comments) | Fix copy-pasted "rds_wav.c is a test program..." banner |
| C17 | All C files        | Remove `extern` keyword from function prototypes |
| C18 | All string-input params | `char *` → `const char *` where not modified |
| C19 | `fm_mpx.c` carrier tables | Use `f` suffix on float literals |
| C20 | Everywhere         | Replace magic literals `8`, `9`, `64` for PS/RT with `PS_LENGTH` / `RT_LENGTH` |

---

## 8. Suggested Refactors (higher effort, higher payoff)

1. **Context structures instead of globals** — pass `fm_mpx_ctx_t *`
   into every `fm_mpx_*` function; same for `rds_ctx_t`. This unlocks
   multi-instance support and makes testing possible.
2. **Extract hardware access into `hw_rpi.c`** — isolate all mmap/DMA
   code behind a small interface (`hw_init`, `hw_push_sample`,
   `hw_shutdown`). The rest becomes portable and the `rds_wav` test
   build becomes a simpler link.
3. **Sample-accurate scheduling** — replace `usleep(5000)` with a
   computed delay based on free slots × sample period.
4. **Introduce `logging.h`** — wrap `printf/fprintf(stderr, ...)` in
   `LOG_INFO`, `LOG_ERR`, `LOG_DBG` with level gating. Useful for
   embedded debug.
5. **Replace Python-generated `waveforms.c`** with compile-time
   generation using C99 designated initializers, or load the file at
   runtime so the binary isn't polluted with 13 KB of floats.
6. **Add `--version` / `--help`** flags and a man page.

---

### Priority Summary

- **P0 (bug):** 1.1, 1.2, 1.5, 1.6, 1.12, 1.15, 1.17, 1.18, 1.23 — *all
  RESOLVED*
- **P1 (correctness / portability):** 1.3, 1.4, 1.7, 1.8, 1.13, 1.14,
  1.16, 1.22 — *all RESOLVED*
- **P2 (perf):** 2.4, 2.9 RESOLVED; 2.1, 2.2, 2.3, 2.10 still open
- **P3 (readability/consistency — high leverage, low risk):**
  5.S1, 5.S3, 5.S4, 5.S6, 5.S7, 5.S11, 5.S13, 5.S14, 5.S21 — *all
  RESOLVED*
- **P3 (engineering):** 6.1, 6.5, 6.7, 6.8 RESOLVED; 6.2, 6.3 still open
- **Nice-to-have:** remaining items in §2, §3, §5, §6, §7, §8

---

### Summary of New Readability Findings (Section 5)

Section 5 ("Simplicity, Consistency & Readability") was added in this
revision and surfaces three higher-level themes:

1. **Convention drift.** Four different function-naming patterns across
   four modules (§5.S1); `extern` used on some prototypes but not
   others (§5.S2); tab vs space vs mixed (§5.S3); CamelCase nowhere but
   abbreviations everywhere (§5.S4, §5.S5).
2. **Duplicated knowledge.** The same constants (PS=8, RT=64) and the
   same dispatch logic (§5.S22) are repeated in several places; CRC
   bit-extraction is inlined twice (§5.S8); boolean idioms differ
   line-to-line (§5.S17).
3. **Noise that obscures intent.** Dead commented code (§5.S13), stale
   or copy-pasted banners (§5.S14), TODOs left as breadcrumbs
   (§5.S15), and 16+ private globals masquerading as exported symbols
   (§5.S21).

Addressing §5.S1, §5.S6, §5.S11, §5.S21 alone — all zero-risk rename /
annotation changes — would make every subsequent review and bug fix
noticeably faster.
