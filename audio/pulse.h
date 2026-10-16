/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KA_PULSE_H
#define KA_PULSE_H

#include <stdbool.h>
#include <stdint.h>

/* PulseAudio playback for out.c, which has the contract (out.h). No thread
 * of its own: the protocol is driven from the engine thread whenever it
 * asks something (pa_mainloop, never blocking except to connect, drop,
 * cork and drain). Works with PipeWire's pulse server too.
 *
 * Memory: ka_pulse owns the connection. */

typedef struct ka_pulse ka_pulse;

/* NULL (with a message) when there's no server: out.c falls back to ALSA. */
ka_pulse *ka_pulse_open(int rate, int buffer_ms, char *err, int errsize);
void ka_pulse_close(ka_pulse *p);
long ka_pulse_period(const ka_pulse *p);
long ka_pulse_room(ka_pulse *p);
bool ka_pulse_write(ka_pulse *p, const int16_t *frames, long n);
long ka_pulse_delay(ka_pulse *p);
void ka_pulse_drop(ka_pulse *p);
void ka_pulse_drain(ka_pulse *p);
bool ka_pulse_pause(ka_pulse *p, bool on);
bool ka_pulse_set_volume(ka_pulse *p, int volume);

#endif
