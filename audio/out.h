/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KA_OUT_H
#define KA_OUT_H

#include <stdbool.h>
#include <stdint.h>

/* Playback, 16-bit stereo at the rate it was opened with. Writes never
 * block: the engine asks how much fits, writes about a period, and sleeps
 * until the next one fits (or a command comes). Half a second of buffer in
 * two periods: about four wakeups a second instead of the ~75 of small
 * blocking writes through the ALSA plugin, and seek, pause and stop don't
 * wait for it (they drop or hold it).
 *
 * "default" is PulseAudio (or PipeWire's pulse server) spoken to directly,
 * as Audacious and mpv do: no plugin thread in between, and the volume is
 * the server's, so a change is heard at once instead of after the buffer.
 * Without a server, and for any other device, it's ALSA.
 *
 * "null" is ALSA's sink that takes everything at once; for tests, ka_out
 * then keeps time by the clock as a real card would. */

#define KA_CHANNELS 2

typedef struct ka_out ka_out;

/* At rate frames a second; NULL with a message in err (size errsize). */
ka_out *ka_out_open(const char *device, int rate, char *err, int errsize);
void ka_out_close(ka_out *o);
int ka_out_rate(const ka_out *o);
/* How much the engine writes at a time, in frames. */
long ka_out_period(const ka_out *o);

/* Frames that can be written now; -1 if the device is gone. */
long ka_out_room(ka_out *o);
/* Nanoseconds until frames will fit (0: they do now). */
long long ka_out_wait_ns(ka_out *o, long frames);
/* Writes n frames, n <= ka_out_room. false if the device is gone. */
bool ka_out_write(ka_out *o, const int16_t *frames, long n);
/* Frames written but not heard yet. */
long ka_out_delay(ka_out *o);
/* Throws away what is buffered (seek, stop). */
void ka_out_drop(ka_out *o);
/* Plays out what is buffered (end of the last file); blocks until then. */
void ka_out_drain(ka_out *o);
/* Holds the sound where it is; false if the device can't (then the caller
 * drops and seeks back on resume). */
bool ka_out_pause(ka_out *o, bool on);
/* 0..100 on mpv's cubic curve, applied by the sound server; false if this
 * device has no volume of its own (the caller scales the samples). */
bool ka_out_set_volume(ka_out *o, int volume);

#endif
