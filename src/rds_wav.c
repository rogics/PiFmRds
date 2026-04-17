/*
    PiFmRds - FM/RDS transmitter for the Raspberry Pi
    Copyright (C) 2014 Christophe Jacquet, F8FTK
    
    See https://github.com/ChristopheJacquet/PiFmRds
    
    rds_wav.c is a test program that writes a RDS baseband signal to a WAV
    file. It requires libsndfile.

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


#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sndfile.h>
#include <string.h>

#include "rds.h"
#include "fm_mpx.h"


#define LENGTH 114000


/* Simple test program */
int main(int argc, char **argv) {
    int exit_code = EXIT_FAILURE;
    int fm_mpx_opened = 0;
    SNDFILE *outf = NULL;
    float *mpx_buffer = NULL;

    if(argc < 4) {
        fprintf(stderr, "Error: missing argument.\n");
        fprintf(stderr, "Syntax: rds_wav <in_audio.wav> <out_mpx.wav> <text>\n");
        return EXIT_FAILURE;
    }

    rds_set_pi(0x1234);
    rds_set_ps(argv[3]);
    rds_set_rt(argv[3]);

    const char *in_file = argv[1];
    if(strcmp("NONE", argv[1]) == 0) in_file = NULL;

    if(fm_mpx_open(in_file, LENGTH) != 0) {
        fprintf(stderr, "Could not setup FM multiplex generator.\n");
        goto cleanup;
    }
    fm_mpx_opened = 1;

    /* LENGTH is 114000 floats = 456 kB; too large to keep on the
     * stack on targets with small default stack limits. */
    mpx_buffer = malloc(LENGTH * sizeof(float));
    if (mpx_buffer == NULL) {
        fprintf(stderr, "Error: out of memory allocating MPX buffer (%zu bytes).\n",
                (size_t)LENGTH * sizeof(float));
        goto cleanup;
    }

    // Set the format of the output file
    SF_INFO sfinfo;

    sfinfo.frames = LENGTH;
    sfinfo.samplerate = 228000;
    sfinfo.channels = 1;
    sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    sfinfo.sections = 1;
    sfinfo.seekable = 0;

    // Open the output file
    const char *out_file = argv[2];
    if (! (outf = sf_open(out_file, SFM_WRITE, &sfinfo))) {
        fprintf(stderr, "Error: could not open output file %s.\n", out_file);
        goto cleanup;
    }

    for(int j=0; j<40; j++) {
        if( fm_mpx_get_samples(mpx_buffer) < 0 ) break;

        for(int i=0; i<LENGTH; i++) {
            mpx_buffer[i] /= 10.;
        }

        if(sf_write_float(outf, mpx_buffer, LENGTH) != LENGTH) {
            fprintf(stderr, "Error: writing to file %s.\n", out_file);
            goto cleanup;
        }
    }

    exit_code = EXIT_SUCCESS;

cleanup:
    if (outf && sf_close(outf)) {
        fprintf(stderr, "Error: closing file %s.\n", argv[2]);
    }
    if (fm_mpx_opened) {
        fm_mpx_close();
    }
    free(mpx_buffer);
    return exit_code;
}
