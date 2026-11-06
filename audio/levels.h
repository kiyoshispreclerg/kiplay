/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KA_LEVELS_H
#define KA_LEVELS_H

#include <stdatomic.h>
#include <stdint.h>

/* The levels for the VU meter and the spectrum on Kiplay's transport bar,
 * shared in memory (kiplay-audio -l FD maps FD, which Kiplay made;
 * "levels N" says what to keep, KA_LEVELS_* bits): no event lines, no
 * extra wakeups.
 *
 * For every block of `block` frames it writes the engine keeps the
 * loudness of each channel (KA_LEVELS_VU: RMS, as a VU meter shows it; a
 * peak would sit at the top with any loud master) and KA_LEVELS_BANDS
 * bands (KA_LEVELS_SPECTRUM: low to high, log spaced), both in dB of full
 * scale in 1/4 steps from -64. Blocks are numbered by frames written since
 * the engine started (a counter that never goes back, across files and
 * seeks); after each wakeup where the device says how much is still
 * queued, it notes that `heard` frames had sounded at `stamp_ns`.
 * The reader goes on from there with the clock and takes the blocks that
 * sounded since it last looked.
 *
 * Kiplay owns the memory; the engine only writes. `seq` is odd while the
 * engine changes the header (a reader that sees it change reads again);
 * a block's id is stored after its values. */

#define KA_LEVELS_BLOCKS 512 /* x 20 ms: more than any buffer */
#define KA_LEVELS_BANDS 12
#define KA_LEVELS_VU 1
#define KA_LEVELS_SPECTRUM 2

typedef struct {
    atomic_uint seq;
    uint32_t rate;    /* frames per second, 0 = nothing played yet */
    uint32_t block;   /* frames per block */
    uint32_t playing; /* 0 = paused, stopped or between files */
    int64_t heard;    /* frames heard at stamp_ns */
    int64_t stamp_ns; /* CLOCK_MONOTONIC */
    struct {
        _Atomic int64_t id;            /* frame / block, -1 = never written */
        uint8_t vu[2];                 /* left, right RMS: (dB + 64) * 4 */
        uint8_t band[KA_LEVELS_BANDS]; /* (dB + 64) * 4 */
    } blocks[KA_LEVELS_BLOCKS];
} ka_levels;

#endif
