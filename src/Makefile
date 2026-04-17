CC ?= gcc
PREFIX ?= /usr/local
# Install staging dir, empty by default. Downstream packagers (Debian,
# Arch PKGBUILD) expect to be able to override this.
DESTDIR ?=

# Base warning and diagnostic flags that apply to every TU.
WARN_CFLAGS = -Wall -Wextra -Wwrite-strings -Wno-unused-parameter
# -MMD -MP produces .d files that we include below, so editing a
# header rebuilds every .o that includes it (addresses S6.8).
STD_CFLAGS = $(WARN_CFLAGS) -std=gnu99 -c -g -MMD -MP

# Enable ARM-specific options only on ARM, and compilation of the app only on ARM
UNAME := $(shell uname -m)

# Determine Raspberry Pi version (if 2 or greater)
RPI_MODEL := $(shell cat /proc/device-tree/model 2>/dev/null)
RPI_VERSION := $(shell echo "$(RPI_MODEL)" | grep -a -o "Raspberry\sPi\s[0-9]" | grep -o "[0-9]")

ifeq ($(findstring Raspberry Pi Zero 2,$(RPI_MODEL)),Raspberry Pi Zero 2)
	# Treat Raspberry Pi Zero 2 as model 3.
	RPI_VERSION := 3
else ifeq ($(findstring Raspberry Pi Zero,$(RPI_MODEL)),Raspberry Pi Zero)
	# Treat Raspberry Pi Zero as model 1.
	RPI_VERSION := 1
endif

$(info RPI_VERSION = $(RPI_VERSION))

# Warn when we cannot detect a Raspberry Pi: the `pi_fm_rds` binary
# will not be built in that case. The host-only `rds_wav` always
# builds.
ifeq ($(strip $(RPI_VERSION)),)
$(warning Could not detect a Raspberry Pi model -- only host-side targets (rds_wav, rds_strings_test) will be built. Override with e.g. `make RPI_VERSION=3`.)
endif

# Per-model tuning (S2.9). Fall back to a safe default otherwise.
ifeq ($(UNAME), armv6l)
	# Pi 1 / Pi Zero.
	ARCH_CFLAGS = -march=armv6 -O3 -mtune=arm1176jzf-s -mfloat-abi=hard -mfpu=vfp
	TARGET = 1
else ifeq ($(shell expr $(RPI_VERSION) \>= 4 2>/dev/null), 1)
	TARGET = 4
	ifeq ($(UNAME), aarch64)
		ARCH_CFLAGS = -march=armv8-a -mtune=cortex-a72 -O2 -pipe -fstack-protector-strong -fno-plt
	else
		ARCH_CFLAGS = -march=armv7-a -mtune=cortex-a72 -O3 -mfloat-abi=hard -mfpu=neon-vfpv4
	endif
else ifeq ($(shell expr $(RPI_VERSION) \>= 2 2>/dev/null), 1)
	TARGET = 2
	ifeq ($(UNAME), aarch64)
		ARCH_CFLAGS = -march=armv8-a -mtune=cortex-a53 -O2 -pipe -fstack-protector-strong -fno-plt
	else
		ARCH_CFLAGS = -march=armv7-a -mtune=cortex-a7 -O3 -mfloat-abi=hard -mfpu=neon-vfpv4
	endif
else ifneq ($(strip $(RPI_VERSION)),)
	ARCH_CFLAGS = -O3
	TARGET = 1
else
	ARCH_CFLAGS = -O3
	TARGET = other
endif

CFLAGS = $(STD_CFLAGS) $(ARCH_CFLAGS)
ifneq ($(TARGET), other)
CFLAGS += -DRASPI=$(TARGET)
endif

# Version string baked into the binary for --version (Phase 11).
# `git describe` gives a human-readable name (tag + commit). Fallback
# to the short SHA, then "unknown" if this isn't a git checkout at all.
PIFM_VERSION ?= $(shell git describe --tags --dirty --always 2>/dev/null || echo unknown)
CFLAGS += -DPIFM_VERSION=\"$(PIFM_VERSION)\"

# DSP translation units get -ftree-vectorize and -ffast-math. Note: we
# intentionally DO NOT apply -ffast-math project-wide, because it
# breaks the RDS CRC bit manipulation and the Modified Julian Day
# calculation in rds.c (S2.10).
DSP_CFLAGS = -ftree-vectorize -ffast-math

APP_OBJS = rds.o waveforms.o pi_fm_rds.o rds_strings.o fm_mpx.o control_pipe.o mailbox.o
RDSWAV_OBJS = rds_wav.o rds.o rds_strings.o waveforms.o fm_mpx.o

# Default target: build rds_wav always, pi_fm_rds only on a Pi.
ifneq ($(TARGET), other)
all: pi_fm_rds rds_wav
else
all: rds_wav
endif

.PHONY: all app clean distclean install uninstall asan test

ifneq ($(TARGET), other)
app: pi_fm_rds

pi_fm_rds: $(APP_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(APP_OBJS) -lsndfile -lm
endif

rds_wav: $(RDSWAV_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(RDSWAV_OBJS) -lsndfile -lm

# DSP TUs get the extra vectorizer / fast-math flags.
fm_mpx.o: fm_mpx.c
	$(CC) $(CFLAGS) $(DSP_CFLAGS) -c $< -o $@

rds.o: rds.c
	$(CC) $(CFLAGS) $(DSP_CFLAGS) -c $< -o $@

waveforms.o: waveforms.c
	$(CC) $(CFLAGS) -c $< -o $@

# Auto-regenerate waveforms.c when the Python source changes (S4.9).
# Kept commented out by default because not all developers will have
# pydemod installed; uncomment to opt in.
#waveforms.c: generate_waveforms.py
#	python3 $< > $@

mailbox.o: mailbox.c
	$(CC) $(CFLAGS) -c $< -o $@

rds_strings.o: rds_strings.c
	$(CC) $(CFLAGS) -c $< -o $@

pi_fm_rds.o: pi_fm_rds.c
	$(CC) $(CFLAGS) -c $< -o $@

control_pipe.o: control_pipe.c
	$(CC) $(CFLAGS) -c $< -o $@

rds_wav.o: rds_wav.c
	$(CC) $(CFLAGS) -c $< -o $@

# ---- Tests ----

rds_strings_test: rds_strings.o rds_strings_test.c
	$(CC) $(WARN_CFLAGS) -std=gnu99 -g -o $@ rds_strings.o rds_strings_test.c
	./rds_strings_test

test: rds_strings_test

# ---- ASan developer build (S6.14) ----

asan: CFLAGS += -fsanitize=address -g -O1
asan: LDFLAGS += -fsanitize=address
asan: rds_wav

# ---- Install ----

install: pi_fm_rds
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 pi_fm_rds $(DESTDIR)$(PREFIX)/bin/pi_fm_rds

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/pi_fm_rds

clean:
	rm -f *.o *.d pi_fm_rds rds_wav *_test

distclean: clean
	rm -f ../baseline_mpx.wav ../baseline_mpx.sha256

# Include automatically-generated .d header dependency files (S6.8).
-include $(wildcard *.d)
