/*
 * libdvi — TMDS / HDMI library for the Raspberry Pi Pico family.
 *
 * Vendored verbatim into frank-hdmi-sound from
 *   https://github.com/shuichitakano/PicoDVI-audio
 * which is shuichitakano's HDMI-audio fork of
 *   https://github.com/Wren6991/PicoDVI
 * by Luke Wren.  The integration pattern this driver uses is taken
 * from
 *   https://github.com/fruit-bat/pico-zxspectrum
 * by fruit-bat and contributors.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2021 Luke Wren and contributors.
 *
 * Local patches against upstream are tagged with
 * `PATCH (frank-hdmi-sound):` comments inside the file.  See the
 * top-level README and src/libdvi/UPSTREAM_README.md for the original
 * upstream notice.
 */
#include "audio_ring.h"
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
