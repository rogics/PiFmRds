/*
    PiFmRds - FM/RDS transmitter for the Raspberry Pi
    Copyright (C) 2014 Christophe Jacquet, F8FTK

    See https://github.com/ChristopheJacquet/PiFmRds

    control_pipe.h: public interface of the runtime RDS control pipe
    reader implemented in control_pipe.c.

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

#ifndef CONTROL_PIPE_H
#define CONTROL_PIPE_H

#include "pifm_common.h"

/* Backwards-compatible aliases. Prefer PIFM_PIPE_* in new code. */
#define CONTROL_PIPE_PS_SET PIFM_PIPE_PS_SET
#define CONTROL_PIPE_RT_SET PIFM_PIPE_RT_SET
#define CONTROL_PIPE_TA_SET PIFM_PIPE_TA_SET

pifm_status_t control_pipe_open(const char *filename);
pifm_status_t control_pipe_close(void);
pifm_status_t control_pipe_poll(void);

#endif /* CONTROL_PIPE_H */
