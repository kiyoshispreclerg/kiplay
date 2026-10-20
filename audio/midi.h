/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KA_MIDI_H
#define KA_MIDI_H

#include <stdbool.h>
#include <stdint.h>

/* MIDI files (.mid, .midi, .kar) played through TinySoundFont and
 * TinyMidiLoader (vendor/tsf) with a General MIDI SoundFont. There is no
 * MIDI port or daemon: the notes become samples here, like game music.
 *
 * TinySoundFont has no reverb or chorus (CC 91 and 93 are ignored).
 *
 * Memory: the SoundFont is read whole, as floats (twice the file's size),
 * and shared by every open file. It is loaded by the first file that plays
 * and let go by ka_midi_trim, which the decoder calls when something that
 * isn't MIDI starts, so a MIDI after a MIDI doesn't read it again but an
 * MP3 after it costs nothing. ka_midi owns its message list and synth. */

typedef struct ka_midi ka_midi;

/* The SoundFont to use (copied); NULL or "" picks the first of the usual
 * ones installed (see midi.c). A change takes effect when none is loaded. */
void ka_midi_set_soundfont(const char *path);

/* Whether the path's extension is one of ours (it isn't opened). */
bool ka_midi_is(const char *path);

/* The file rendered at rate frames a second; rate 0 opens it for its
 * length only (no SoundFont needed, ka_midi_read can't be used).
 * NULL with a message in err. */
ka_midi *ka_midi_open(const char *path, int rate, char *err, int errsize);
void ka_midi_close(ka_midi *m);

/* Up to max stereo frames into out; 0 at the end (the last note's tail
 * included). */
long ka_midi_read(ka_midi *m, int16_t *out, long max);
/* To that second: the channels' programs, volumes and bends as they are
 * there, no note sounding. */
bool ka_midi_seek(ka_midi *m, double seconds);

/* Seconds, with the tail. */
double ka_midi_duration(const ka_midi *m);

/* Lets go of the SoundFont if no file holds it. */
void ka_midi_trim(void);

#endif
