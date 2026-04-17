/*
    PiFmRds - FM/RDS transmitter for the Raspberry Pi
    Copyright (C) 2014 Christophe Jacquet, F8FTK

    See https://github.com/ChristopheJacquet/PiFmRds

    control_pipe.c: handles commands written to a non-blocking control pipe,
    in order to change RDS PS, RT and TA at runtime.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#include "rds.h"
#include "control_pipe.h"
#include "pifm_common.h"

/* POSIX.1 guarantees PATH_MAX from <limits.h> but glibc only defines
 * it when _POSIX_C_SOURCE is set. Use a sane fallback if missing. */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Hold at least one full "RT <up to 64 chars>\n\0" line plus slop. */
#define CTL_LINE_MAX 256

/* Persistent state across control_pipe_poll() calls. We own the fd
 * directly (not through a FILE*) so we can reliably non-block, detect
 * writer-close (EOF on read), and re-open. */
static int  ctl_fd = -1;
static char ctl_filename[PATH_MAX];
static char ctl_line[CTL_LINE_MAX];
static size_t ctl_line_len = 0;
/* True when the current accumulated line has overflowed CTL_LINE_MAX;
 * we drop everything until the next newline and log a warning. */
static int  ctl_line_overflow = 0;

/*
 * Open `filename` as a non-blocking control FIFO / file.
 */
static pifm_status_t ctl_reopen(void) {
    if (ctl_fd >= 0) {
        close(ctl_fd);
        ctl_fd = -1;
    }
    int fd = open(ctl_filename, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return PIFM_ERR_IO;
    ctl_fd = fd;
    ctl_line_len = 0;
    ctl_line_overflow = 0;
    return PIFM_OK;
}

pifm_status_t control_pipe_open(const char *filename) {
    if (filename == NULL) return PIFM_ERR_ARG;
    size_t n = strnlen(filename, sizeof(ctl_filename));
    if (n >= sizeof(ctl_filename)) return PIFM_ERR_ARG;
    memcpy(ctl_filename, filename, n + 1);
    return ctl_reopen();
}

/* Dispatcher table: one entry per supported command.
 * `prefix`  is the two-letter command followed by a space ("PS ", "RT ", "TA ").
 * `handler` receives the NUL-terminated argument (already truncated to
 *           max_len if needed) and returns the status code to report.
 */
struct command_entry {
    const char *prefix;
    size_t      prefix_len;
    size_t      max_arg_len;
    pifm_status_t (*handler)(char *arg);
};

static pifm_status_t handle_ps(char *arg) {
    rds_set_ps(arg);
    printf("PS set to: \"%s\"\n", arg);
    return PIFM_PIPE_PS_SET;
}

static pifm_status_t handle_rt(char *arg) {
    rds_set_rt(arg);
    printf("RT set to: \"%s\"\n", arg);
    return PIFM_PIPE_RT_SET;
}

static pifm_status_t handle_ta(char *arg) {
    int ta = (strcmp(arg, "ON") == 0);
    rds_set_ta(ta);
    printf("Set TA to %s\n", ta ? "ON" : "OFF");
    return PIFM_PIPE_TA_SET;
}

static const struct command_entry commands[] = {
    { "PS ", 3, PS_LENGTH,              handle_ps },
    { "RT ", 3, RT_LENGTH,              handle_rt },
    { "TA ", 3, 3 /* "ON" / "OFF" */,   handle_ta },
};

/* Dispatch a single, already-NUL-terminated command line. Returns the
 * command's status code, or PIFM_PIPE_NO_CMD for an unrecognised
 * / malformed line. */
static pifm_status_t dispatch_line(char *line) {
    for (size_t i = 0; i < sizeof(commands)/sizeof(commands[0]); i++) {
        const struct command_entry *c = &commands[i];
        if (strncmp(line, c->prefix, c->prefix_len) == 0) {
            char *arg = line + c->prefix_len;
            size_t arg_len = strlen(arg);
            if (arg_len > c->max_arg_len) {
                arg[c->max_arg_len] = 0;
            }
            return c->handler(arg);
        }
    }
    fprintf(stderr, "control_pipe: ignoring unrecognised command: \"%s\"\n", line);
    return PIFM_PIPE_NO_CMD;
}

/*
 * Polls the control file (pipe), non-blockingly. A command is dispatched
 * every time a '\n' is seen in the accumulated line buffer. If no complete
 * line is available yet (short read / writer has not flushed), returns
 * PIFM_PIPE_NO_CMD so the caller can just continue.
 *
 * The previous implementation used fgets() on a non-blocking fd, which
 * silently discarded partial lines and occasionally returned spurious
 * EOF. This version is robust to:
 *   - partial writes (PS set via `printf "PS " ; sleep 1 ; printf "ABC\n"`),
 *   - writer close + later re-open (the FIFO is transparently re-opened),
 *   - overflowed lines (dropped with a warning).
 */
pifm_status_t control_pipe_poll(void) {
    if (ctl_fd < 0) return PIFM_PIPE_NO_CMD;

    char scratch[CTL_LINE_MAX];
    ssize_t n = read(ctl_fd, scratch, sizeof(scratch));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return PIFM_PIPE_NO_CMD;
        }
        fprintf(stderr, "control_pipe: read error: %s\n", strerror(errno));
        return PIFM_PIPE_NO_CMD;
    }
    if (n == 0) {
        /* Writer closed the FIFO. Re-open non-blocking so the next
         * `cat > ctl_pipe` starts with a clean slate. */
        ctl_reopen();
        return PIFM_PIPE_NO_CMD;
    }

    pifm_status_t last_status = PIFM_PIPE_NO_CMD;
    for (ssize_t i = 0; i < n; i++) {
        char ch = scratch[i];
        if (ch == '\n') {
            if (ctl_line_overflow) {
                fprintf(stderr, "control_pipe: command too long, discarded\n");
                ctl_line_overflow = 0;
                ctl_line_len = 0;
                continue;
            }
            ctl_line[ctl_line_len] = 0;
            if (ctl_line_len > 0) {
                pifm_status_t st = dispatch_line(ctl_line);
                if (st != PIFM_PIPE_NO_CMD) last_status = st;
            }
            ctl_line_len = 0;
        } else if (ctl_line_overflow) {
            continue;
        } else if (ctl_line_len + 1 >= sizeof(ctl_line)) {
            /* Reserve one byte for the NUL. Mark overflow. */
            ctl_line_overflow = 1;
        } else {
            ctl_line[ctl_line_len++] = ch;
        }
    }

    return last_status;
}

pifm_status_t control_pipe_close(void) {
    if (ctl_fd >= 0) {
        close(ctl_fd);
        ctl_fd = -1;
    }
    return PIFM_OK;
}
