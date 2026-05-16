/*
 * SPSC ring used between the producer and the HDMI audio data-island
 * packetiser.  Power-of-two sized; head and tail are managed without
 * locks (the producer owns the write index, the consumer owns the
 * read index).
 *
 * (c) 2026 Mikhail Matveev <xtreme@rh1.tech>, https://rh1.tech
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Based on libdvi by Luke Wren and contributors
 * (https://github.com/Wren6991/PicoDVI), with HDMI audio additions
 * from shuichitakano's PicoDVI-audio fork
 * (https://github.com/shuichitakano/PicoDVI-audio).
 *
 * Copyright (c) 2021 Luke Wren and contributors.
 */
#include "frank_audio_ring.h"
#include <hardware/sync.h>

/*
 * Attach a backing buffer to the ring and reset its read and write
 * indices to zero.  `size` must be a power of two greater than 1.
 * The rest of the ring uses bitwise AND with (size - 1) for
 * wrap-around, so non-power-of-two sizes silently corrupt the
 * indexing.
 */
void audio_ring_set(audio_ring_t *audio_ring, audio_sample_t *buffer, uint32_t size) {
    assert(size > 1);
    audio_ring->buffer = buffer;
    audio_ring->size   = size;
    audio_ring->read   = 0;
    audio_ring->write  = 0;
}

/*
 * Number of frames the producer can safely write right now without
 * overtaking the consumer.  Pass `full=true` to let the producer
 * fill the buffer completely (size - 1 useful slots, since one slot
 * is reserved to disambiguate full vs empty).
 *
 * Use `full=true` from frank-hdmi-sound's audio writer.  The
 * `full=false` branch over-reports free space when wp >= rp and
 * lets the producer corrupt samples the consumer hasn't read yet.
 */
uint32_t __not_in_flash_func(get_write_size)(audio_ring_t *audio_ring, bool full) {
    //__mem_fence_acquire();
    uint32_t rp = audio_ring->read;
    uint32_t wp = audio_ring->write;
    if (wp < rp) {
        return rp - wp - 1;
    } else {
        return audio_ring->size - wp + (full ? rp - 1 : (rp == 0 ? -1 : 0));
    }
}

/*
 * Mirror of get_write_size for the consumer side.  How many frames
 * are available to read right now.
 */
uint32_t __not_in_flash_func(get_read_size)(audio_ring_t *audio_ring, bool full) {
    //__mem_fence_acquire();
    uint32_t rp = audio_ring->read;
    uint32_t wp = audio_ring->write;
    
    if (wp < rp) {
        return audio_ring->size - rp + (full ? wp : 0);
    } else {
        return wp - rp;
    }    
}
