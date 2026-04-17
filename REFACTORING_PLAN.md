# PiFmRds — Incremental Refactoring Plan

This plan translates `IMPROVEMENT_SUGGESTIONS.md` into a concrete, phased
roll-out. The guiding principles are:

1. **Safety first.** Every phase leaves the tree buildable and
   behaviour-compatible with the previous one. No "big bang" rewrites.
2. **Risk gradient.** Early phases are zero-behaviour renames /
   annotations; later phases change behaviour; the riskiest structural
   refactors come last.
3. **Verifiability.** Each phase lists *exactly* what to test so a
   regression is caught immediately, before the next phase builds on it.
4. **Atomic commits.** Each phase (and most sub-steps) should land as
   one reviewable commit.
5. **Reversibility.** Any phase can be reverted by `git revert` without
   touching later phases (except where explicitly noted as a
   prerequisite).

Numbers in parentheses reference the section ids in
`IMPROVEMENT_SUGGESTIONS.md`.

---

## Ground Rules

Before starting, establish a baseline you can return to:

- **G1.** Tag the current `HEAD` as `pre-refactor-baseline`.
- **G2.** Record a short manual smoke-test script:
  1. `cd src && make` on a Raspberry Pi (Pi 1 / Pi 3 / Pi 4 if possible).
  2. `sudo ./pi_fm_rds -audio sound.wav -freq 107.9 -ps TEST -rt "hello world"`
     and confirm a nearby FM receiver hears audio + shows `TEST`/`hello world`.
  3. `./rds_wav /dev/null test.wav` compiles and runs (no Pi needed).
  4. `./rds_strings_test` exits 0.
- **G3.** Capture a reference MPX output once — `rds_wav` into a file —
  so later phases can byte-compare output for regressions:
  ```bash
  ./rds_wav sound.wav baseline_mpx.wav
  sha256sum baseline_mpx.wav > baseline_mpx.sha256
  ```
  This is the single most valuable regression oracle we have. Any phase
  that should be behaviour-preserving must keep this hash identical.
- **G4.** Work on a feature branch per phase (`refactor/phase-01-headers`
  etc.) and merge only after the smoke-tests pass.

> **Note on "no behaviour change":** Phases marked *behaviour-preserving*
> must not alter the bytes `rds_wav` produces for a given input.
> Phases marked *behaviour-changing* describe exactly what changes and
> why the change is safe.

---

## Phase Overview (at a glance)

| Phase | Theme | Risk | Behaviour change? | Depends on |
|-------|-------|------|-------------------|------------|
| 0 | Tooling foundation (`.gitattributes`, `.clang-format`, `.editorconfig`) | none | no | — |
| 1 | Header hygiene (guards, `extern`, `const`, banners) | none | no | 0 |
| 2 | Encapsulation (`static`, shared constants) | very low | no | 1 |
| 3 | Trivial correctness fixes (C1–C8, C11) | low | **yes** (fixes bugs) | 2 |
| 4 | Signal handling overhaul (§1.1) | medium | yes (fewer signals caught) | 3 |
| 5 | Control pipe robustness (§1.12–1.14, §5.S22) | medium | yes (better parsing) | 3 |
| 6 | Heap-allocate large buffers, guard library calls (§1.8, §1.15, §1.17) | medium | no | 3 |
| 7 | `fm_mpx` mono/stereo bug fix (§1.5, §1.6, §1.7) | medium | **yes** (mono fixed) | 3 |
| 8 | Build system improvements (Makefile, deps, install) | low | no | 0 |
| 9 | Magic numbers → named constants (§4.1, §5.S7) | low | no | 2 |
| 10 | Naming convention unification (§5.S1) | low | no | 2 |
| 11 | Argument parser → `getopt_long` (§4.4, §5.S18) | medium | yes (better UX) | 3 |
| 12 | Status enum + unified return codes (§5.S6) | medium | no | 10 |
| 13 | Logging macros (§5.S19, §6.4 partial, ref §8.4) | low | no | 0 |
| 14 | Performance passes (§2.x) | medium | no | G3 (hash check!) |
| 15 | Unit tests + CI (§6.2, §6.3) | none | no | 12 |
| 16 | Structural refactors: context structs, hw split (§8) | high | no | 15 |
| 17 | Polish: `--version`, man page, README sync (§6.13, §8.6) | none | no | 11 |

Each phase is expanded below with step-by-step actions and the
verification gate that must pass before moving on.

---

## Phase 0 — Tooling Foundation

**Goal:** make every later phase mechanically reproducible.
**Risk:** none. **Behaviour change:** none.

### Steps

1. Add a project-root `.gitattributes`:
   ```
   *.c     text eol=lf
   *.h     text eol=lf
   *.py    text eol=lf
   Makefile text eol=lf
   *.md    text eol=lf
   *.wav   binary
   ```
   (Addresses §5.S16.)

2. Add a project-root `.editorconfig`:
   ```
   root = true
   [*]
   end_of_line = lf
   insert_final_newline = true
   charset = utf-8
   trim_trailing_whitespace = true
   indent_style = space
   indent_size = 4
   [Makefile]
   indent_style = tab
   ```
   (Addresses §4.2, §5.S3, §6.4.)

3. Add `src/.clang-format` (start with LLVM preset, 4-space indent, no
   tabs — this matches 80 % of the existing code):
   ```
   BasedOnStyle: LLVM
   IndentWidth: 4
   UseTab: Never
   ColumnLimit: 100
   ```
   **Do NOT run `clang-format -i` yet.** That's a separate, bigger commit
   in Phase 1.

4. Add a `.gitignore` entry for build artefacts if missing:
   ```
   src/*.o
   src/*.d
   src/pi_fm_rds
   src/rds_wav
   src/rds_strings_test
   ```

### Verification gate

- `git diff --check` shows no whitespace errors.
- `cd src && make clean && make` still works.
- No behavioural change — G3 hash unchanged.

---

## Phase 1 — Header Hygiene

**Goal:** make headers safe to include in any order, fix top-of-file
comments, make string-type signatures correct.
**Risk:** none (all changes are compile-time-checked).
**Behaviour change:** none.
**Addresses:** §1.22, §1.23, §5.S2, §5.S11, §5.S14 (banners), §6.1, C13, C14, C15, C16, C17, C18.

### Steps (one commit each)

1. **Unique include guards** on all headers. Rename `rds_strings.h`
   guard `RDS_H` → `RDS_STRINGS_H`. Add guards to `fm_mpx.h`,
   `control_pipe.h`, `waveforms.h` if missing. Fix §1.23.

2. **Fix copy-pasted file banners** in `control_pipe.c`, `control_pipe.h`,
   `fm_mpx.c`, `fm_mpx.h` so the top-of-file description matches reality
   (§5.S14 bullet 4).

3. **Remove redundant `extern` keyword** from all function prototypes
   (§5.S2). Mechanical — no meaning change.

4. **`const`-correct string parameters** (§5.S11, C18). In order:
   - `set_rds_ps(char *)` → `set_rds_ps(const char *)`
   - `set_rds_rt(char *)` → `set_rds_rt(const char *)`
   - `fill_rds_string(char *dst, const char *src, size_t n)`
   - `open_control_pipe(const char *filename)`
   - `fm_mpx_open(const char *filename, size_t len)`
   - `tx(uint32_t, const char *audio_file, uint16_t, const char *ps, const char *rt, float, const char *control_pipe)`

   The caller site `char *rt = "PiFmRds: live..."` in `main()` becomes
   `const char *rt`. Add `-Wwrite-strings` to `CFLAGS` in a follow-up
   Phase 8 step.

5. **`const` the generated tables** (§1.22, C15):
   - `waveforms.h`: `extern const float waveform_biphase[576];`
   - `waveforms.c`: `const float waveform_biphase[576] = { ... };`
   - Re-generate the `.c` via the Python script to stay in sync
     (also see Phase 8 step 4).

### Verification gate

- Compile with `-Wall -Wextra -Wwrite-strings` — zero new warnings.
- `./rds_wav sound.wav out.wav && sha256sum -c baseline_mpx.sha256`
  (G3) — identical output.
- Manual smoke-test still works.

---

## Phase 2 — Encapsulation

**Goal:** stop leaking internal symbols; establish shared constants.
**Risk:** very low (detected at link time).
**Behaviour change:** none.
**Addresses:** §1.20, §1.21, §5.S21, C9, C10.

### Steps

1. **Mark every file-scope variable `static`** unless it is deliberately
   exported via a header. The offenders identified in §5.S21 and C9:
   - `fm_mpx.c`: `phase_19`, `phase_38`, `audio_buffer`, `audio_index`,
     `audio_len`, `audio_pos`, `fir_buffer_mono`, `fir_buffer_stereo`,
     `fir_index`, `channels`, `inf`, `length`, `downsample_factor`,
     `low_pass_fir[]`, `carrier_19[]`, `carrier_38[]`.
   - `rds.c`: `offset_words[]` → `static const`.

2. **Audit `nm src/*.o | grep ' [BDCGR] '`** — fail the phase if any
   non-API symbol is still externally visible.

3. **Create `src/pifm_common.h`** with project-wide constants that are
   currently duplicated:
   ```c
   #ifndef PIFM_COMMON_H
   #define PIFM_COMMON_H
   #define PS_LENGTH 8
   #define RT_LENGTH 64
   #define PS_BUF_SIZE (PS_LENGTH + 1)
   #define RT_BUF_SIZE (RT_LENGTH + 1)
   #endif
   ```
   (Don't replace call sites yet — that's Phase 9, to keep this commit
   focused on encapsulation only.)

### Verification gate

- `nm src/pi_fm_rds | grep ' T ' | wc -l` — confirm fewer exported
  globals than before.
- G3 hash unchanged.

---

## Phase 3 — Trivial Correctness Fixes (C1–C8, C11)

**Goal:** land all the one-liner bug fixes that are mechanically
obviously correct.
**Risk:** low. **Behaviour change:** yes — these fix real bugs. Each
change is small enough that a bisect is easy.
**Addresses:** §1.2, §1.5, §1.8, §1.12, §1.18, §2.4, §6.15.

### Steps (one commit each so bisecting is precise)

1. **C1** — `pi_fm_rds.c:282`: `terminate(0)` → `terminate(EXIT_FAILURE)`
   inside `fatal()`. (§1.2)

2. **C7** — `rds_wav.c:89,95`: swap `argv[1]` → `argv[2]` in the write /
   close error messages. (§1.18)

3. **C8** — `control_pipe.c` PS/RT parser: add length check before
   writing the NUL. (§1.12)
   ```c
   size_t n = strlen(arg);
   if (n > PS_LENGTH) arg[PS_LENGTH] = 0;
   ```
   Do the same for RT with `RT_LENGTH`.

4. **C4** — `fm_mpx.c:258`: guard `sf_close` with `if (inf && sf_close(inf))`
   and add missing `\n`. (§1.8)

5. **C5 + C6** — `fm_mpx.c`: replace `bzero(..)` with `memset(.., 0, ..)`,
   drop `#include <strings.h>`, drop `#define PI`, use `M_PI`. (§6.15,
   §2.5, §3.3)

6. **C2** — `pi_fm_rds.c:533`: `(int)((floor)(dval))` → `lrintf(dval)`.
   (§2.4) **Caveat:** this *changes rounding* (floor → round-to-nearest).
   Quantization noise goes down slightly; the G3 hash will change.
   **Update `baseline_mpx.sha256` after this commit** and note it in the
   commit message.

7. **C3** — `fm_mpx.c:196`: `channels == 0` → `channels == 1`. **This is
   a behavioural bug fix for mono files.** The G3 hash *must* change if
   `sound.wav` is mono. Regenerate the baseline after landing.

### Verification gate

- Build + smoke-test after each commit.
- After commits 1–5: G3 hash unchanged.
- After commit 6: G3 hash changed, *new* hash pinned as baseline,
  spot-checked on a Pi to confirm MPX still sounds correct.
- After commit 7: mono input file explicitly re-tested (previously
  broken path).

---

## Phase 4 — Signal Handling Overhaul

**Goal:** fix §1.1 — stop installing a handler on every signal slot.
**Risk:** medium. **Behaviour change:** signals like `SIGPIPE`,
`SIGCHLD`, `SIGWINCH` now use default disposition (usually ignore).

### Steps

1. Replace the `for (i=0; i<64; i++)` loop in `pi_fm_rds.c` with
   explicit handlers for:
   `SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT`.

2. Convert `terminate()` into two pieces:
   - `static volatile sig_atomic_t g_terminate_requested = 0;`
   - A tiny async-signal-safe handler that only sets the flag (and
     records which signal, for exit-code propagation).
   - The existing cleanup (printf, fclose, munmap, exit) moves into a
     new `do_cleanup_and_exit(int code)` called from the main loop when
     the flag is set.

3. Main loop (`tx()`) polls `g_terminate_requested` at the top of its
   iteration and calls `do_cleanup_and_exit` if set.

4. For the "real" fatal signals (SEGV/BUS/FPE/ILL/ABRT) we still need
   cleanup *from* the handler — keep the current behaviour but
   `sigaction` with `SA_RESETHAND` so a second identical signal
   core-dumps normally.

### Verification gate

- `kill -INT`, `kill -TERM`, `kill -HUP` during a live transmission
  each cleanly shut down (carrier stops, no leaked DMA).
- `yes | ./pi_fm_rds -audio - ...` — pipe the writer closed (SIGPIPE)
  — transmitter now survives (it used to die).
- `gdb` on a SIGSEGV test still gets a usable backtrace.

---

## Phase 5 — Control Pipe Robustness

**Goal:** make `-ctl` safe against short reads, closed writers, and
overflowing commands. Also clean up the dispatch.
**Risk:** medium.
**Addresses:** §1.12 (completed in Phase 3), §1.13, §1.14, §5.S22.

### Steps

1. **Accumulate partial lines.** Keep a persistent `char line_buf[256]`
   across `poll_control_pipe()` calls. Only dispatch when a `\n` is
   seen. Discard over-long lines with a warning. (§1.14)

2. **Re-open on EOF.** When `read()` returns 0 (writer closed the FIFO),
   close the fd and re-open it non-blocking, so subsequent writers are
   received. (§1.13)

3. **Dispatch table.** Replace the `if/else if` chain with:
   ```c
   static const struct {
       const char *prefix;
       size_t max_len;
       void (*handler)(const char *arg);
       int status_code;
   } commands[] = { ... };
   ```
   (§5.S22)

4. **Distinguish "no command" from error.** Introduce explicit status
   codes (full enum comes in Phase 12; use a `#define CONTROL_PIPE_NO_CMD 0`
   for now). Update `tx()` main loop accordingly.

### Verification gate

- Test script:
  ```bash
  mkfifo /tmp/rds
  ./pi_fm_rds -ctl /tmp/rds ... &
  printf "PS TEST\n" > /tmp/rds
  # then close and reopen:
  printf "RT hello\n" > /tmp/rds
  printf "PS " > /tmp/rds; sleep 1; printf "SPLIT\n" > /tmp/rds   # partial line
  ```
  All three should update the on-air RDS; the partial-line test
  specifically exercises the fix.

---

## Phase 6 — Memory / Library Call Hygiene

**Goal:** stop putting 456 KB on the stack; stop calling `exit()` from a
library; undo the mmap alignment offset on unmap.
**Risk:** medium.
**Addresses:** §1.15, §1.16, §1.17.

### Steps

1. **`rds_wav.c`** — move `float mpx_buffer[LENGTH]` from stack to heap
   (`malloc`/`free` with error check and a `goto cleanup;` path, which
   also fixes §5.S20). (§1.17)

2. **`mailbox.c`** — convert three `exit(-1)` call sites into `return`s
   with an error code; propagate up to `pi_fm_rds.c` which now calls
   `fatal(...)` (which performs the proper DMA/mbox cleanup). (§1.15)
   - Changes the API of `mbox_open`, `mem_alloc`, `mem_lock`,
     `mapmem`. Update `pi_fm_rds.c` call sites.

3. **`mailbox.c`** — record the page offset in `mapmem` (perhaps in a
   wrapper struct or via rounding in `unmapmem`) so `munmap` receives a
   page-aligned pointer. (§1.16)

4. **`fm_mpx_close()`** — audit the cleanup path; ensure `audio_buffer`
   is freed even when `inf == NULL` (§6.14).

### Verification gate

- `ulimit -s 256 && ./rds_wav sound.wav out.wav` — used to crash,
  now succeeds. (§1.17)
- Run under ASan (Phase 8 will add the target; for now: manual
  `make CFLAGS+='-fsanitize=address -g -O1' LDFLAGS='-fsanitize=address'`).
  No leaks, no use-after-free.
- G3 hash unchanged.

---

## Phase 7 — `fm_mpx` Mono/Stereo + `sf_count_t`

**Goal:** finish the §1.5–§1.7 trifecta started in Phase 3.
**Risk:** medium. **Behaviour change:** mono files now read correctly;
stereo files read a full frame (§1.6).

Phase 3 already fixed §1.5 (the `==0` typo). This phase tackles
§1.6 and §1.7 together since they touch the same read loop.

### Steps

1. Change `audio_len` type from `int` to `sf_count_t` throughout
   `fm_mpx.c`. (§1.7)

2. `sf_read_float(inf, audio_buffer, length * channels)` — request a
   full multi-channel frame count. Adjust `audio_index`/`audio_len`
   bookkeeping if the read returns a non-multiple count (libsndfile
   guarantees multiple of `channels` when the request is).

3. Add an impulse-response self-test to `rds_strings_test.c` (or a new
   `fm_mpx_test.c`) that feeds a known mono and stereo signal through
   `fm_mpx_get_samples` and checks invariants: e.g. the sum of pilot
   tone amplitude equals expected; for mono, L == R behaviour, etc.
   (Prepares for Phase 15.)

### Verification gate

- Stereo smoke-test (`stereo_44100.wav`): receiver shows stereo
  indicator, L/R separation audible.
- Mono smoke-test: no audio shift / frequency offset.
- G3 hash *will* change; re-pin and spot-check.

---

## Phase 8 — Build System

**Goal:** bring the Makefile up to modern conventions.
**Risk:** low. **Behaviour change:** none for built artefacts.
**Addresses:** §4.9, §6.5, §6.6, §6.7, §6.8, §6.14, C12, plus §2.9, §2.10 (scoped).

### Steps

1. **Dependency tracking** (§6.8):
   ```makefile
   CFLAGS += -MMD -MP
   -include $(wildcard *.d)
   ```

2. **`clean` + `distclean`** (§6.7):
   ```makefile
   clean:
       rm -f *.o *.d pi_fm_rds rds_wav *_test
   distclean: clean
       rm -f baseline_mpx.wav
   ```

3. **`install` / `uninstall` with `PREFIX`/`DESTDIR`** (§6.5):
   ```makefile
   PREFIX ?= /usr/local
   install: pi_fm_rds
       install -Dm755 pi_fm_rds $(DESTDIR)$(PREFIX)/bin/pi_fm_rds
   uninstall:
       rm -f $(DESTDIR)$(PREFIX)/bin/pi_fm_rds
   ```

4. **Regenerate `waveforms.c`** (§4.9):
   ```makefile
   waveforms.c: generate_waveforms.py
       python3 $< > $@
   ```

5. **Non-Pi warning** (§6.6): `$(warning ...)` when `RPI_VERSION` is
   empty.

6. **Per-model tuning** (§2.9): dispatch on `RPI_VERSION` to pick
   `-mtune=arm1176jzf-s` / `cortex-a7` / `cortex-a53` / `cortex-a72`.

7. **Scoped `-ffast-math`** (§2.10): apply only to `fm_mpx.o` and
   `rds.o` — the DSP TUs — *not* to the RDS CRC / MJD date math.
   (Before this is landed, verify §1.11's MJD still produces correct
   MJD for a handful of known (date, MJD) pairs with and without
   `-ffast-math`.)

8. **ASan target** (§6.14):
   ```makefile
   asan: CFLAGS += -fsanitize=address -g -O1
   asan: LDFLAGS += -fsanitize=address
   asan: rds_wav
   ```

### Verification gate

- `make clean && make && make install DESTDIR=/tmp/pi && ls /tmp/pi/usr/local/bin`.
- `touch rds.h && make` rebuilds `pi_fm_rds.o` (proves deps work).
- `make asan && ./rds_wav sound.wav out.wav` — no ASan diagnostics.
- G3 hash unchanged (after re-pinning from Phase 7).

---

## Phase 9 — Magic Numbers → Named Constants

**Goal:** kill the `8`/`9`/`64`/`0x5A`/`228000`/`0x10880001`/`4.05`
magic literals.
**Risk:** low (mechanical). **Behaviour change:** none.
**Addresses:** §4.1, §5.S7, C20.

### Steps

1. Extend `pifm_common.h` (from Phase 2):
   ```c
   #define CM_PASSWD           (0x5A << 24)
   #define MPX_SAMPLE_RATE     228000
   #define MPX_MONO_GAIN       4.05f
   #define MPX_STEREO_L_PLUS_R 4.05f   /* or the actual value */
   #define MPX_PILOT_GAIN      0.9f
   ```

2. Decompose `0x10880001` (DMA CS flag word) into named DMA flags
   inside `pi_fm_rds.c` — each bit meaning documented in a comment.

3. Replace bare `8`, `9`, `64` with `PS_LENGTH`, `PS_BUF_SIZE`, `RT_LENGTH`
   (§5.S7 list). `char myps[9]` → `char myps[PS_BUF_SIZE]`. Etc.

### Verification gate

- G3 hash unchanged.
- `grep -nE '\b(0x5A|228000|4\.05|\b8\b|\b64\b)' src/*.c` returns only
  clearly intended uses (e.g. offsets, not PS/RT lengths).

---

## Phase 10 — Naming Convention Unification

**Goal:** adopt `module_verb_object` for all public APIs.
**Risk:** low (mechanical), but high churn — do in a **dedicated PR**.
**Addresses:** §5.S1, §5.S4, §5.S5.

### Steps

1. Pick the convention: **`<module>_<verb>[_<object>]`**. Old names
   become thin static inline `#define` aliases for one deprecation
   release if backward compatibility for any downstream tool is
   desired; otherwise rename hard.

2. Renames (non-exhaustive, exemplary):
   - `set_rds_pi` → `rds_set_pi`
   - `set_rds_ps` → `rds_set_ps`
   - `set_rds_rt` → `rds_set_rt`
   - `set_rds_ta` → `rds_set_ta`
   - `get_rds_samples` → `rds_get_samples`
   - `fill_rds_string` → `rds_fill_string` (static, could be renamed more radically)
   - `open_control_pipe` → `control_pipe_open`
   - `close_control_pipe` → `control_pipe_close`
   - `poll_control_pipe` → `control_pipe_poll`
   - `mbox_open` → keep (already prefixed), but `mem_alloc` →
     `mbox_mem_alloc` so the module prefix is consistent.

3. **In the same PR** rename the bad locals in `pi_fm_rds.c`:
   `count`/`count2` → `ps_cycle_counter` / `ps_numeric_counter`,
   `myps` → `generated_ps`, `last_cb` → `last_cb_virt_addr`,
   `ifbi`/`dfbi` → `fir_idx_forward` / `fir_idx_backward` (§5.S4, §5.S5).

4. Use `clang-rename` or just a careful `sed -i` + manual review. Run
   `clang-format -i` on the renamed files as a separate commit.

### Verification gate

- `nm src/*.o` — only the intended symbols are exported.
- G3 hash unchanged.
- A full fresh clone + build on a Pi still works end-to-end.

---

## Phase 11 — `getopt_long` Argument Parser

**Goal:** replace the hand-rolled `strcmp` chain in `main`.
**Risk:** medium. **Behaviour change:** yes — gains `--help`,
`--version`, long options, and rejects typos.
**Addresses:** §4.4, §5.S18, §6.13.

### Steps

1. Define the options table with both short (`-f`) and long (`--freq`)
   forms. Map the legacy `-freq` / `-ps` / `-rt` / `-audio` / `-wav` /
   `-ctl` / `-ppm` / `-pi` to long options; emit a warning on the
   deprecated single-dash forms **for one release**, then drop them
   (§6.13).

2. Implement `--help` that prints the same text as today, and
   `--version` that prints `PiFmRds <git-describe>` (set at build time
   via `-DPIFM_VERSION=...` in Phase 8).

3. Validate `ps` length ≤ 8 chars; warn if longer (§1.19).

### Verification gate

- `./pi_fm_rds --help` shows all options.
- `./pi_fm_rds -freqq 100` is rejected (previously accepted as typo?).
- `./pi_fm_rds -freq 107.9 -ps TEST ...` still works (legacy).
- `./pi_fm_rds --freq 107.9 --ps TEST` works.

---

## Phase 12 — Status Enum / Unified Return Codes

**Goal:** one `pifm_status_t` for the whole project.
**Risk:** medium — touches every public function's signature.
**Addresses:** §5.S6.

Must come *after* Phase 10 (naming) and Phase 11 (argparser), because
those stabilize the API surface this enum describes.

### Steps

1. Add enum (as in §5.S6) to `pifm_common.h`.

2. Convert return types of `control_pipe_open`, `control_pipe_poll`,
   `fm_mpx_open`, `fm_mpx_get_samples`, `tx` to `pifm_status_t`.
   `mem_alloc` becomes a two-out-param function (handle via `*out`,
   status via return) to disambiguate handle==0.

3. Update all call sites and error-log formatting.

### Verification gate

- G3 hash unchanged (pure type refactor).
- All error paths exercised via test script (kill, bad file, bad PS
  length, ...) emit a sensible exit code.

---

## Phase 13 — Logging Macros

**Goal:** consistent, gateable logs.
**Risk:** low. **Behaviour change:** none at default verbosity.
**Addresses:** §5.S19, §6.10, §8.4.

### Steps

1. New `src/logging.h`:
   ```c
   #define LOG_LEVEL_ERR  0
   #define LOG_LEVEL_WARN 1
   #define LOG_LEVEL_INFO 2
   #define LOG_LEVEL_DBG  3
   extern int g_log_level;
   #define LOG_ERR(fmt, ...)  do { if (g_log_level >= LOG_LEVEL_ERR)  fprintf(stderr, "[ERR] "  fmt "\n", ##__VA_ARGS__); } while (0)
   /* ... */
   ```

2. Replace every `fprintf(stderr, ...)` / `printf(...)` diagnostic with
   the appropriate macro. Route "Failed to open control pipe"
   (§6.10) from stdout to `LOG_ERR`.

3. `--verbose` / `-v` CLI flag (added in Phase 11) bumps
   `g_log_level`.

### Verification gate

- `./pi_fm_rds -v ...` emits extra diagnostics.
- Default output is unchanged for successful runs.

---

## Phase 14 — Performance Pass

**Goal:** measurable CPU reduction on Pi 1/Zero.
**Risk:** medium (DSP output can drift subtly).
**Addresses:** §2.1, §2.2, §2.3, §2.6 (decide), §2.7, §2.8.
**Prerequisite:** reliable G3 hash baseline + a spectrogram comparison
tool (e.g. `sox baseline_mpx.wav -n spectrogram -o baseline.png`).

### Steps (each its own commit + verification)

1. **§2.3** — replace the `switch(phase)` in `get_rds_samples` with a
   `carrier57[4]` LUT. Pure algebraic identity — G3 hash must be
   identical. (Do this first: it's free and verifies the whole
   regression-check harness.)

2. **§2.1a** — doubled ring buffer in `fm_mpx_get_samples`. Keep the
   arithmetic identical; only the index wrap changes. Verify via G3
   hash.

3. **§2.1b** — compiler flags: add `-ftree-vectorize`, bump from
   `-mfpu=vfp` to `-mfpu=neon-vfpv4` on Pi 2+. This is done per-target
   in Phase 8's multi-arch dispatch. Hash will likely change at the
   last-bit level; spot-check the spectrogram instead.

4. **§2.4** — already done in Phase 3 step 6.

5. **§2.6** — decide: either remove `SUBSIZE` / the dead interpolation
   code, or implement it properly. Recommendation: **remove**, because
   at `SUBSIZE=1` it's dead weight, and a real implementation belongs
   in a future "MPX 2.0" project. (§5.S13 alignment.)

6. **§2.2** — adaptive sleep. Compute
   `sleep_us = (NUM_SAMPLES / 2 - free_slots) * sample_period_us;`
   clamped to `[500, 5000]`. Verify no under-runs (`free_slots` never
   reaches `NUM_SAMPLES` mid-run).

7. **§2.8** — sample_buffer accumulation pattern. Only do this if
   profiling (`perf stat -e cache-misses`) shows it matters.

### Verification gate

- G3 hash identical for steps 1–2, 5.
- For steps where hash changes: null-difference spectrogram,
  on-Pi receiver test.
- `perf stat ./rds_wav sound.wav /dev/null` — cycle count non-regressing.

---

## Phase 15 — Unit Tests + CI

**Goal:** freeze current behaviour as regressions-against tests.
**Risk:** none. **Behaviour change:** none.
**Addresses:** §6.2, §6.3.

### Steps

1. **CRC test**: feed `crc()` the known-good RDS example blocks from
   the IEC 62106 spec; assert checkwords match.

2. **MJD test**: table of `(y/m/d → mjd)` pairs covering DST transitions
   and Jan/Feb edge cases (§1.11).

3. **`get_rds_group` test**: generate a group-0A packet with a fixed
   PS, compare to a golden binary vector.

4. **FIR impulse test**: feed a unit impulse to `fm_mpx_get_samples`;
   the output is the FIR coefficients. Assert.

5. **GitHub Actions workflow** `.github/workflows/build.yml`:
   - Matrix: `{ubuntu-latest x (armv6, armv7, aarch64)}` cross-compile
     with `gcc-arm-linux-gnueabihf` etc.
   - Run the host-side tests (`rds_strings_test`, new CRC/MJD/FIR tests).
   - `clang-format --dry-run --Werror` on all `src/*.[ch]`.

### Verification gate

- `make test` runs all tests locally.
- CI green on a clean PR.

---

## Phase 16 — Structural Refactors

**Goal:** eliminate globals; isolate hardware.
**Risk:** high. **Behaviour change:** none at runtime.
**Prerequisite:** Phase 15 tests, to catch silent breakage.
**Addresses:** §8.1, §8.2.

### Steps

1. **`fm_mpx_ctx_t`**: pack every `static` file-scope variable from
   Phase 2 into one struct. Every `fm_mpx_*` function takes
   `fm_mpx_ctx_t *ctx` as its first argument. Offer a convenience
   `fm_mpx_ctx_t *fm_mpx_default_ctx(void)` that returns a singleton for
   existing callers. Do the same for `rds_ctx_t` (housing
   `rds_params`, `state`, etc.).

2. **Split hardware code** into `hw_rpi.c` / `hw_rpi.h` with the API:
   ```c
   int  hw_init(uint32_t carrier_freq, float ppm);
   void hw_push_samples(const float *samples, size_t n);
   void hw_shutdown(void);
   ```
   Everything `dma_reg`, `gpio_reg`, `cm_reg`, `mbox` becomes internal
   to this TU. `pi_fm_rds.c` becomes the orchestrator.

3. `rds_wav.c` now links *only* `fm_mpx.o`, `rds.o`, `waveforms.o` —
   not `hw_rpi.o`, `mailbox.o` — making host builds trivial.

### Verification gate

- G3 hash unchanged.
- Every Phase 15 test still passes.
- Two instances of `fm_mpx_ctx_t` can coexist in a single process
  (write a test for this).

---

## Phase 17 — Polish

**Goal:** docs, polish, housekeeping.
**Risk:** none.
**Addresses:** §3.1, §5.S13, §5.S15, §5.S16, §6.12, §6.13, §8.6.

### Steps

1. Move shipped `*.wav` examples out of `src/` into `examples/`.
   Update `README.md`. Optionally, remove the heaviest (`sound.wav`
   1.9 MB) from git history with `git filter-repo` — this is a one-time
   repo-rewrite and needs downstream communication. (§3.1)

2. Delete all commented-out dead code (§5.S13) and stale TODOs
   (§5.S15). Trust git history.

3. `README.md`: document both `-audio` and `-wav` (or drop `-wav`),
   document `--help`, `--version`, `--verbose` (§6.13).

4. Add `man/pi_fm_rds.1` man page; install via Phase 8's `install`
   target. (§8.6)

5. `LICENSE` addendum noting BSD licence of `mailbox.[ch]` (§6.12).

6. Fix the `FIR_HALF_SIZE` / `FIR_SIZE` naming confusion (§5.S14).

### Verification gate

- `man ./man/pi_fm_rds.1` renders.
- `./pi_fm_rds --version` prints a git-derived version.

---

## Continuous Verification Checklist (per phase)

Each PR that implements a phase must:

- [ ] Build clean on Pi 1, Pi 3, Pi 4 (via CI after Phase 15, manually
      before).
- [ ] Build clean on host (`rds_wav`) with `-Wall -Wextra -Wwrite-strings
      -Wconversion`.
- [ ] G3 regression hash check passes *or* the phase writes a new
      baseline into the PR with a justification.
- [ ] Manual on-Pi smoke-test (G2) passes.
- [ ] If the phase adds tests, those tests are part of the phase's PR
      (not deferred).
- [ ] Commit message references the IMPROVEMENT_SUGGESTIONS.md section
      ids addressed, for future archaeology.

---

## Mapping to the Priority Summary

The phase ordering above is consistent with the P-ratings given at the
end of `IMPROVEMENT_SUGGESTIONS.md`:

- **P0 bugs** (1.1, 1.2, 1.5, 1.6, 1.12, 1.15, 1.17, 1.18, 1.23) —
  handled in Phase 1 (1.23), Phase 3 (1.2, 1.5 partial, 1.12, 1.18),
  Phase 4 (1.1), Phase 6 (1.15, 1.17), Phase 7 (1.6).
- **P1 correctness** — 1.3 (Phase 14 follow-up), 1.4 (Phase 4 /
  Phase 14.6), 1.7 (Phase 7), 1.8 (Phase 3), 1.13/1.14 (Phase 5),
  1.16 (Phase 6), 1.22 (Phase 1).
- **P2 performance** — Phase 14 and Phase 8 (-mtune, -ffast-math).
- **P3 readability** — Phases 1, 2, 9, 10, 13, 17.
- **P3 engineering** — Phases 8, 15.
- **Nice-to-have** — Phase 16, 17.

---

## Estimated Effort

| Phase | Rough effort | Can a single person land it in one sitting? |
|-------|--------------|----------------------------------------------|
| 0     | 30 min       | yes |
| 1     | 2 h          | yes |
| 2     | 1 h          | yes |
| 3     | 2 h          | yes (one commit per fix) |
| 4     | 4 h          | yes, if familiar with signals |
| 5     | 4 h          | yes |
| 6     | 3 h          | yes |
| 7     | 3 h          | yes |
| 8     | 3 h          | yes |
| 9     | 2 h          | yes |
| 10    | 4 h          | yes (big but mechanical) |
| 11    | 4 h          | yes |
| 12    | 3 h          | yes |
| 13    | 2 h          | yes |
| 14    | 6 h          | maybe split across two sessions |
| 15    | 6 h          | yes |
| 16    | 1–2 days     | **no** — needs review across multiple sittings |
| 17    | 2 h          | yes |

Total: ~6–8 focused engineering days to bring the tree to a state
where every item in `IMPROVEMENT_SUGGESTIONS.md` is either resolved,
deliberately deferred, or tracked as a follow-up issue.
