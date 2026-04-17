/*
    PiFmRds - FM/RDS transmitter for the Raspberry Pi
    Copyright (C) 2014 Christophe Jacquet, F8FTK

    See https://github.com/ChristopheJacquet/PiFmRds

    logging.h: lightweight, gateable logging macros used throughout
    the codebase. New code should prefer the LOG_* macros over
    unconditional printf / fprintf(stderr, ...).

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

#ifndef LOGGING_H
#define LOGGING_H

#include <stdio.h>

#define LOG_LEVEL_ERR   0
#define LOG_LEVEL_WARN  1
#define LOG_LEVEL_INFO  2
#define LOG_LEVEL_DBG   3

/* Runtime-adjustable log level. Defaults to LOG_LEVEL_INFO so existing
 * output is preserved. --verbose / -v in main() bumps it to
 * LOG_LEVEL_DBG. */
extern int g_log_level;

/* Implementation detail: stderr for warnings and errors, stdout for
 * info and debug. This matches the pre-existing split used in
 * pi_fm_rds.c where status chatter goes to stdout and failures to
 * stderr (§6.10). */
#define LOG_ERR(fmt, ...)  do { \
    if (g_log_level >= LOG_LEVEL_ERR)  \
        fprintf(stderr, "[ERR] "  fmt "\n", ##__VA_ARGS__); \
} while (0)

#define LOG_WARN(fmt, ...) do { \
    if (g_log_level >= LOG_LEVEL_WARN) \
        fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__); \
} while (0)

#define LOG_INFO(fmt, ...) do { \
    if (g_log_level >= LOG_LEVEL_INFO) \
        fprintf(stdout, fmt "\n", ##__VA_ARGS__); \
} while (0)

#define LOG_DBG(fmt, ...)  do { \
    if (g_log_level >= LOG_LEVEL_DBG)  \
        fprintf(stdout, "[DBG] "  fmt "\n", ##__VA_ARGS__); \
} while (0)

#endif /* LOGGING_H */
