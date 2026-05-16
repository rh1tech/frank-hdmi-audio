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
#ifndef AUDIO_RING_H
#define AUDIO_RING_H
#include "pico.h"
#include <hardware/sync.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_sample {
    int16_t channels[2];
} audio_sample_t;

typedef struct audio_ring {
    audio_sample_t    *buffer;
    uint32_t          size;
    volatile uint32_t read;
    volatile uint32_t write; 
} audio_ring_t;

inline audio_sample_t *get_buffer_top(audio_ring_t *audio_ring)    { return audio_ring->buffer; }
inline uint32_t get_buffer_size(audio_ring_t *audio_ring)          { return audio_ring->size;   }
inline uint32_t get_read_offset(audio_ring_t *audio_ring)          { return audio_ring->read;   }
inline uint32_t get_write_offset(audio_ring_t *audio_ring)         { return audio_ring->write;  }
inline audio_sample_t *get_write_pointer(audio_ring_t *audio_ring) { return audio_ring->buffer + audio_ring->write; }
inline audio_sample_t *get_read_pointer(audio_ring_t *audio_ring)  { return audio_ring->buffer + audio_ring->read;  }
inline static void increase_write_pointer(audio_ring_t *audio_ring, uint32_t size) {audio_ring->write = (audio_ring->write + size) & (audio_ring->size - 1); __dmb();}
inline static void increase_read_pointer(audio_ring_t *audio_ring, uint32_t size)  {audio_ring->read = (audio_ring->read + size) & (audio_ring->size - 1); __dmb();}
inline static void set_write_offset(audio_ring_t *audio_ring, uint32_t v) {audio_ring->write = v; __dmb();}
inline static void set_read_offset(audio_ring_t *audio_ring, uint32_t v){audio_ring->read = v; __dmb();}
void audio_ring_set(audio_ring_t *audio_ring, audio_sample_t *buffer, uint32_t size);
uint32_t get_write_size(audio_ring_t *audio_ring, bool full);
uint32_t get_read_size(audio_ring_t *audio_ring, bool full);

#ifdef __cplusplus
}
#endif
#endif
