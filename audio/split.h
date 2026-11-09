/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KA_SPLIT_H
#define KA_SPLIT_H

/* "Split into tracks": one audio file cut into several, without touching
 * the sound where that can be helped.
 *
 * Lossy formats (MP3, AAC, Opus, Vorbis...) are cut by copying their
 * packets as they are: no re-encoding, so no loss, and a cut lands on a
 * packet edge (20 to 30 ms). Lossless ones (FLAC, WAV, ALAC, WavPack...)
 * are decoded and encoded again in the same format, which loses nothing
 * either and cuts on the exact sample. Pieces are contiguous: played one
 * after the other (gapless) they are the original again.
 *
 * Tags: the original's, minus what only made sense for the whole file
 * (cue sheet, track gain, lyrics), with the title, artist and album given
 * for each piece and "track" = n/total. The cover goes along where the
 * format keeps one.
 *
 *   kiplay-audio --split-info FILE
 *       duration <seconds>
 *       mode exact|copy
 *       chapter <start seconds>\t<title>       (zero or more)
 *     or one "error <message>" line (exit 1). Files with video are refused.
 *
 *   kiplay-audio --split, with on stdin
 *       src <file>
 *       piece <start>\t<end, -1 = to the end>\t<title>\t<artist>\t<album>\t<destination>
 *       ...
 *       go
 *     prints
 *       mode exact|copy
 *       progress <0..1>                        (now and then)
 *     and at the end "ok" or "error <message>". Empty tag fields keep the
 *     original's. Each piece is written as <destination>.part, and only
 *     once all are done do they take their names; an existing file is
 *     never replaced (that is an error). On an error or SIGTERM the .part
 *     files go away. */

int ka_split_info(const char *path);
int ka_split_serve(void);

#endif
