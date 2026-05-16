/*
 * frank-hdmi-sound — minimal HDMI video + audio driver for RP2350.
 *
 * (c) 2026 Mikhail Matveev <xtreme@rh1.tech>, https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.  See the LICENSE file at
 * the root of this repository, or <https://www.gnu.org/licenses/>.
 *
 * Acknowledgements
 * ----------------
 * Forked from the libdvi-based HDMI path in frank-snes; uses libdvi
 * by Luke Wren (https://github.com/Wren6991/PicoDVI) via shuichitakano's
 * HDMI-audio fork (https://github.com/shuichitakano/PicoDVI-audio).
 * The integration pattern follows pico-zxspectrum by fruit-bat and
 * contributors (https://github.com/fruit-bat/pico-zxspectrum).
 *
 * Pipeline summary
 * ----------------
 *  - 640x480p60 output, vertical-doubled to a 320x240 logical
 *    canvas; libdvi's 16bpp encoder is also pixel-doubling, so the
 *    per-line scanbuf is 320 RGB565 pixels wide.
 *  - The caller-provided 8-bit palette-indexed framebuffer is read
 *    live every scanline and converted to RGB565 via a 256-entry LUT
 *    pinned in scratch_y memory for fast access on Core 1.
 *  - HDMI audio rides data-island packets; the producer pushes
 *    int16 stereo frames into a ring buffer, the consumer is libdvi.
 *
 * Threading
 * ---------
 *  - Core 1 owns DVI: registers DMA_IRQ_1, runs producer + consumer
 *    in one tight loop (we inline the encoder body so emulation /
 *    application work can stay on Core 0).
 *  - Core 0 calls frank_hdmi_audio_write() to push packed stereo
 *    samples into the libdvi audio ring.
 */

#include "frank_hdmi.h"

#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/structs/bus_ctrl.h"

#include "frank_dvi.h"
#include "frank_serialiser.h"
#include "frank_dvi_config.h"
#include "frank_audio_ring.h"

/* ------------------------------------------------------------------ */
/* PIO and pin configuration                                          */
/* ------------------------------------------------------------------ */

#ifndef FRANK_HDMI_PIO
#define FRANK_HDMI_PIO  pio0
#endif

/*
 * Default polarity for the FRANK / MURMULATOR-2 board layout.  The
 * working driver in frank-snes ships with invert_diffpairs = true
 * for these boards; the libdvi PIO + this board's PCB wiring give
 * the spec-correct differential output only when the GPIO override
 * inverter is enabled.  Boards with the opposite N/P wiring should
 * set FRANK_HDMI_INVERT_DIFFPAIRS=0 in their build.
 */
#ifndef FRANK_HDMI_INVERT_DIFFPAIRS
#define FRANK_HDMI_INVERT_DIFFPAIRS  true
#endif

static const struct dvi_serialiser_cfg frank_dvi_cfg = {
    .pio              = FRANK_HDMI_PIO,
    .sm_tmds          = { 0, 1, 2 },
    .pins_tmds        = { FRANK_HDMI_PIN_D0, FRANK_HDMI_PIN_D1, FRANK_HDMI_PIN_D2 },
    .pins_clk         = FRANK_HDMI_PIN_CLK,
    .invert_diffpairs = FRANK_HDMI_INVERT_DIFFPAIRS,
};

/* ------------------------------------------------------------------ */
/* Mode constants                                                     */
/* ------------------------------------------------------------------ */

#define DVI_TIMING_PRESET   dvi_timing_640x480p_60hz

/* libdvi's encoder is pixel-doubling, so logical_width = h_active/2. */
#define LOGICAL_W           FRANK_HDMI_LOGICAL_WIDTH    /* = 320 */
#define LOGICAL_H           FRANK_HDMI_LOGICAL_HEIGHT   /* = 240 */

/* Two scanline buffers in flight gives one frame of slack between
 * producer and consumer — important when Core 0 is doing heavy SRAM
 * or PSRAM work and stalls Core 1 momentarily. */
#define N_SCANLINE_BUFS     2

/* CEA-861 N-value for 32 kHz. */
#define HDMI_AUDIO_N        4096

/* Power-of-two size for the data-island ring.  Producers typically
 * push in bursts of one chunk per video frame; the ring needs to
 * absorb at least one chunk plus a couple of catch-up bursts.  2048
 * frames = 8 KB and ~64 ms of buffering. */
#define AUDIO_RING_FRAMES   2048

/* ------------------------------------------------------------------ */
/* libdvi state and buffers                                           */
/* ------------------------------------------------------------------ */

static struct dvi_inst dvi0;

static uint16_t __attribute__((aligned(4)))
    scanline_buf[N_SCANLINE_BUFS][LOGICAL_W];

static audio_sample_t __attribute__((aligned(4)))
    audio_ring_storage[AUDIO_RING_FRAMES];

/* ------------------------------------------------------------------ */
/* Caller-supplied framebuffer + palette                              */
/* ------------------------------------------------------------------ */

static const uint8_t *fb_buf = NULL;
static int  fb_w = 0;
static int  fb_h = 0;
static int  fb_x_offset = 0;   /* logical-canvas X where source col 0 lands */
static int  fb_y_offset = 0;   /* logical-canvas Y where source row 0 lands */

static uint32_t palette_rgb888[256];
/* Hot-path LUT placed in scratch_y so per-pixel lookups on Core 1
 * don't contend with Core 0 SRAM traffic. */
static uint16_t __scratch_y("frank_hdmi_pal565") palette_rgb565[256];

static inline uint16_t rgb888_to_rgb565(uint32_t rgb) {
    uint8_t r = (rgb >> 16) & 0xff;
    uint8_t g = (rgb >> 8)  & 0xff;
    uint8_t b = (rgb >> 0)  & 0xff;
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

void frank_hdmi_set_buffer(const uint8_t *fb, int w, int h) {
    /* Clip oversized buffers to the logical canvas. */
    if (w > LOGICAL_W) w = LOGICAL_W;
    if (h > LOGICAL_H) h = LOGICAL_H;
    fb_buf = fb;
    fb_w = w;
    fb_h = h;
    fb_x_offset = (LOGICAL_W - w) / 2;
    fb_y_offset = (LOGICAL_H - h) / 2;
}

void frank_hdmi_set_palette(uint8_t i, uint32_t color888) {
    color888 &= 0x00ffffffu;
    palette_rgb888[i] = color888;
    palette_rgb565[i] = rgb888_to_rgb565(color888);
}

uint32_t frank_hdmi_get_palette(uint8_t i) {
    return palette_rgb888[i];
}

/* ------------------------------------------------------------------ */
/* Producer + consumer loop on Core 1                                 */
/* ------------------------------------------------------------------ */

/* Forward decl for the inlined encoder body — defined after the
 * fill_scanline producer so both can sit in scratch_y. */
static void encode_one_scanline_16bpp(struct dvi_inst *inst);

/*
 * Fill one logical scanline into a 320-wide RGB565 buffer.
 *
 * Pillarbox columns (left + right of the source rectangle) are
 * pre-zeroed in frank_hdmi_init() and never rewritten per frame.
 * The hot path therefore reduces to a 320-byte palette LUT lookup
 * across only the source rectangle — short enough that Core 1 keeps
 * up with the TMDS encoder even under heavy Core 0 SRAM contention.
 *
 * Placed in scratch_y so the per-scanline lookup runs out of Core
 * 1's local SRAM bank and competes minimally with Core 0 traffic on
 * the main banks.
 */
static void __scratch_y("fill_scanline") fill_scanline(uint16_t *dst, int logical_y) {
    int src_y = logical_y - fb_y_offset;
    uint16_t *out = dst + fb_x_offset;

    if (src_y < 0 || src_y >= fb_h || fb_buf == NULL) {
        memset(out, 0, fb_w * sizeof(uint16_t));
        return;
    }

    const uint8_t *src = fb_buf + (size_t)src_y * (size_t)fb_w;
    int w = fb_w;
    if (w > LOGICAL_W - fb_x_offset) w = LOGICAL_W - fb_x_offset;
    for (int x = 0; x < w; ++x) {
        out[x] = palette_rgb565[src[x]];
    }
}

/* TMDS encoder is exported by libdvi. */
extern void tmds_encode_data_channel_16bpp(const uint32_t *pixbuf,
                                           uint32_t *symbuf, size_t n_pix,
                                           uint channel_msb, uint channel_lsb);

/*
 * Single-iteration version of dvi_scanbuf_main_16bpp's loop body.
 * libdvi's own copy is an infinite loop, so we replicate the body
 * here to keep producer + consumer interleaved on Core 1.
 */
static void __not_in_flash_func(encode_one_scanline_16bpp)(struct dvi_inst *inst) {
    uint32_t *scanbuf = NULL;
    queue_remove_blocking(&inst->q_colour_valid, &scanbuf);

    uint32_t *tmdsbuf = NULL;
    queue_remove_blocking(&inst->q_tmds_free, &tmdsbuf);
    uint pixwidth       = inst->timing->h_active_pixels;
    uint words_per_chan = pixwidth / DVI_SYMBOLS_PER_WORD;
    tmds_encode_data_channel_16bpp(scanbuf, tmdsbuf + 0 * words_per_chan,
                                   pixwidth / 2,
                                   DVI_16BPP_BLUE_MSB,  DVI_16BPP_BLUE_LSB);
    tmds_encode_data_channel_16bpp(scanbuf, tmdsbuf + 1 * words_per_chan,
                                   pixwidth / 2,
                                   DVI_16BPP_GREEN_MSB, DVI_16BPP_GREEN_LSB);
    tmds_encode_data_channel_16bpp(scanbuf, tmdsbuf + 2 * words_per_chan,
                                   pixwidth / 2,
                                   DVI_16BPP_RED_MSB,   DVI_16BPP_RED_LSB);
    queue_add_blocking(&inst->q_tmds_valid, &tmdsbuf);

    queue_add_blocking(&inst->q_colour_free, &scanbuf);
}

/*
 * Core 1 entry: producer + consumer in one tight loop.
 *
 * We register on DMA_IRQ_1 (instead of libdvi's default DMA_IRQ_0)
 * to avoid contention with anything on Core 0 that uses DMA_IRQ_0.
 *
 * Before calling dvi_start(), we prime the TMDS pipeline by pushing
 * one fully encoded scanline into q_tmds_valid: the receiver only
 * locks once it sees real TMDS data, and dvi_start's first DMA chain
 * otherwise reads a placeholder buffer and emits garbage TMDS while
 * sync rolls.
 */

/* Heartbeat counter — bumped every encoded scanline by Core 1.
 * Read by Core 0 to confirm the encode loop is alive.  Volatile so
 * the compiler reloads it each read; cross-core visibility relies on
 * the natural cache coherence of RP2350 SRAM. */
volatile uint32_t frank_hdmi_heartbeat_lines = 0;
volatile uint32_t frank_hdmi_heartbeat_frames = 0;

static void __not_in_flash_func(core1_main)(void) {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_1);

    for (int i = 0; i < N_SCANLINE_BUFS; ++i) {
        uint16_t *scanbuf = NULL;
        queue_remove_blocking(&dvi0.q_colour_free, &scanbuf);
        fill_scanline(scanbuf, i);
        queue_add_blocking(&dvi0.q_colour_valid, &scanbuf);
        encode_one_scanline_16bpp(&dvi0);
    }

    dvi_start(&dvi0);

    int logical_y = 0;
    while (1) {
        uint16_t *scanbuf = NULL;
        queue_remove_blocking(&dvi0.q_colour_free, &scanbuf);
        fill_scanline(scanbuf, logical_y);
        queue_add_blocking(&dvi0.q_colour_valid, &scanbuf);

        encode_one_scanline_16bpp(&dvi0);

        ++frank_hdmi_heartbeat_lines;
        if (++logical_y >= LOGICAL_H) {
            logical_y = 0;
            ++frank_hdmi_heartbeat_frames;
        }
    }
}

void __not_in_flash_func(frank_hdmi_run_core1)(void) {
    core1_main();
    __builtin_unreachable();
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                     */
/* ------------------------------------------------------------------ */

void frank_hdmi_init(void) {
    /*
     * libdvi's TMDS serialiser SMs and PWM pixel clock are
     * configured with DVI_SM_CLKDIV=2 (see src/libdvi/CMakeLists.txt)
     * so they stay at spec rate when sys_clock runs at 2x the TMDS
     * bit clock.  Concretely: typical CPU clock is 504 MHz driving
     * a 252 MHz TMDS link.  We do NOT change the CPU clock here —
     * the application picks its own.
     */

    /*
     * Give Core 1 (the DVI core) bus priority, and promote DMA bus
     * priority so the TMDS DMA channels aren't starved by Core 0
     * work.  Without this, heavy bursty traffic on Core 0 caused
     * libdvi's late_scanline_ctr to fire and the screen to flash a
     * red line on every dropped scanline.
     */
    hw_set_bits(&bus_ctrl_hw->priority,
                BUSCTRL_BUS_PRIORITY_PROC1_BITS |
                BUSCTRL_BUS_PRIORITY_DMA_R_BITS |
                BUSCTRL_BUS_PRIORITY_DMA_W_BITS);

    dvi0.timing  = &DVI_TIMING_PRESET;
    dvi0.ser_cfg = frank_dvi_cfg;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    /* Audio data-island setup.  CTS = pixel_clk * N / (128 * fs). */
    dvi_get_blank_settings(&dvi0)->top    = 0;
    dvi_get_blank_settings(&dvi0)->bottom = 0;
    dvi_audio_sample_buffer_set(&dvi0, audio_ring_storage, AUDIO_RING_FRAMES);

    /*
     * Phase-shift the producer half a ring ahead of the consumer so
     * the consumer never drains to empty while waiting for the next
     * producer burst (and the producer never overflows during a
     * catch-up burst).  Without this initial offset the rate-matched
     * producer/consumer pair runs at the boundary of underrun and
     * the audio is full of short gaps.
     */
    set_write_offset(&dvi0.audio_ring, AUDIO_RING_FRAMES >> 1);

    int cts = DVI_TIMING_PRESET.bit_clk_khz * HDMI_AUDIO_N
            / (FRANK_HDMI_AUDIO_RATE / 100) / 128;
    dvi_set_audio_freq(&dvi0, FRANK_HDMI_AUDIO_RATE, cts, HDMI_AUDIO_N);

    /*
     * Pre-fill scanline buffers with black RGB565.  fill_scanline()
     * never rewrites the pillarbox columns that flank the source
     * rectangle, so they have to start out zeroed — and pre-filling
     * the entire buffer is the simplest way to do that.
     */
    for (int i = 0; i < N_SCANLINE_BUFS; ++i) {
        memset(scanline_buf[i], 0, LOGICAL_W * sizeof(uint16_t));
    }

    /* Pre-feed scanline buffers into the colour-free queue. */
    for (int i = 0; i < N_SCANLINE_BUFS; ++i) {
        void *p = scanline_buf[i];
        queue_add_blocking(&dvi0.q_colour_free, &p);
    }

    /*
     * NOTE: do NOT zero the palette here.  Callers may legitimately
     * call frank_hdmi_set_palette() before frank_hdmi_init() to set
     * up colours; clearing the LUT in init would silently revert any
     * pre-init writes back to black.  C zero-initialises the static
     * arrays at boot, which is the correct default state.
     */
}

/* ------------------------------------------------------------------ */
/* Audio output                                                       */
/* ------------------------------------------------------------------ */

uint32_t frank_hdmi_audio_free(void) {
    /*
     * Pass full=true so libdvi's helper uses the accurate "wp >= rp"
     * arithmetic.  The full=false branch over-reports free space by
     * up to rp-1 frames (a sign-error in audio_ring.c), which causes
     * the producer to overwrite samples the consumer hasn't read
     * yet — audible as glitches/discontinuities mid-waveform.
     */
    return get_write_size(&dvi0.audio_ring, true);
}

uint32_t __not_in_flash_func(frank_hdmi_audio_write)(const int16_t *frames_lr,
                                                     uint32_t num_frames) {
    if (!dvi_is_started(&dvi0)) return 0;

    uint32_t free_frames = get_write_size(&dvi0.audio_ring, true);
    if (free_frames == 0) return 0;

    uint32_t to_write = num_frames < free_frames ? num_frames : free_frames;

    audio_sample_t *base = get_buffer_top(&dvi0.audio_ring);
    uint32_t mask  = get_buffer_size(&dvi0.audio_ring) - 1;
    uint32_t wpos  = get_write_offset(&dvi0.audio_ring);
    for (uint32_t i = 0; i < to_write; ++i) {
        base[wpos].channels[0] = frames_lr[i * 2 + 0];
        base[wpos].channels[1] = frames_lr[i * 2 + 1];
        wpos = (wpos + 1) & mask;
    }
    set_write_offset(&dvi0.audio_ring, wpos);
    return to_write;
}
