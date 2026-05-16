/*
 * frank-hdmi-sound — RP2350 HDMI driver internals.
 *
 * (c) 2026 Mikhail Matveev <xtreme@rh1.tech>, https://rh1.tech
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Derivative of libdvi by Luke Wren and contributors
 * (https://github.com/Wren6991/PicoDVI), with HDMI audio additions
 * from shuichitakano's PicoDVI-audio fork
 * (https://github.com/shuichitakano/PicoDVI-audio).  Renamed,
 * trimmed, and lightly patched for frank-hdmi-sound.
 *
 * Copyright (c) 2021 Luke Wren and contributors.
 */
#include "frank_audio_ring.h"
#include <hardware/sync.h>

void audio_ring_set(audio_ring_t *audio_ring, audio_sample_t *buffer, uint32_t size) {
    assert(size > 1);
    audio_ring->buffer = buffer;
    audio_ring->size   = size;
    audio_ring->read   = 0;
    audio_ring->write  = 0;
}

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
