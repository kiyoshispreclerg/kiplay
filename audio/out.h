/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KA_OUT_H
#define KA_OUT_H

#include <stdbool.h>
#include <stdint.h>

/* ALSA playback, 16-bit stereo at KA_RATE, blocking writes (the engine
 * thread is paced by them). With PipeWire or PulseAudio the "default"
 * device goes through their ALSA plugin.
 *
 * "null" is ALSA's sink that takes everything at once; for tests, ka_out
 * then paces itself by the clock as a real card would. */

#define KA_RATE 48000
#define KA_CHANNELS 2

typedef struct ka_out ka_out;

/* NULL with an ALSA message in err (size errsize). */
ka_out *ka_out_open(const char *device, char *err, int errsize);
void ka_out_close(ka_out *o);

/* Writes all frames (blocks while the device buffer is full). false if the
 * device is gone. */
bool ka_out_write(ka_out *o, const int16_t *frames, long n);
/* Frames written but not heard yet. */
long ka_out_delay(ka_out *o);
/* Throws away what is buffered (seek, stop). */
void ka_out_drop(ka_out *o);
/* Plays out what is buffered (end of the last file). */
void ka_out_drain(ka_out *o);
/* Holds the sound where it is; false if the device can't (then the caller
 * drops and seeks back on resume). */
bool ka_out_pause(ka_out *o, bool on);

#endif
