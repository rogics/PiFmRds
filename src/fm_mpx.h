/*
    PiFmRds - FM/RDS transmitter for the Raspberry Pi
    Copyright (C) 2014 Christophe Jacquet, F8FTK

    See https://github.com/ChristopheJacquet/PiFmRds

    fm_mpx.h: public interface of the FM multiplex (MPX) generator
    implemented in fm_mpx.c.

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

#ifndef FM_MPX_H
#define FM_MPX_H

#include <stdlib.h>

#include "pifm_common.h"

pifm_status_t fm_mpx_open(const char *filename, size_t len);
pifm_status_t fm_mpx_get_samples(float *mpx_buffer);
pifm_status_t fm_mpx_close(void);

#endif /* FM_MPX_H */