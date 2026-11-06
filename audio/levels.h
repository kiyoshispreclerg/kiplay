/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KA_LEVELS_H
#define KA_LEVELS_H

#include <stdatomic.h>
#include <stdint.h>

/* The sound level for a meter, shared with Kiplay in memory (kiplay-audio
 * -l FD maps FD, which Kiplay made; "levels 1" turns it on): no event lines, no extra wakeups.
 *
 * The engine keeps the peak of every block of `block` frames it writes,
 * numbered by frames written since it started (a counter that never goes
 * back, across files and seeks), and after each wakeup where the device
 * says how much is still queued: `heard` frames had sounded at `stamp_ns`.
 * The reader goes on from there with the clock and takes the peaks of the
 * blocks that sounded since it last looked.
 *
 * Kiplay owns the memory; the engine only writes. `seq` is odd while the
 * engine changes the header (a reader that sees it change reads again);
 * a block's id is stored after its peaks. */

#define KA_LEVELS_BLOCKS 512 /* x 20 ms: more than any buffer */

typedef struct {
    atomic_uint seq;
    uint32_t rate;    /* frames per second, 0 = nothing played yet */
    uint32_t block;   /* frames per block */
    uint32_t playing; /* 0 = paused, stopped or between files */
    int64_t heard;    /* frames heard at stamp_ns */
    int64_t stamp_ns; /* CLOCK_MONOTONIC */
    struct {
        _Atomic int64_t id; /* frame / block, -1 = never written */
        uint8_t peak[2];    /* left, right: 0..255 of full scale */
    } blocks[KA_LEVELS_BLOCKS];
} ka_levels;

#endif
