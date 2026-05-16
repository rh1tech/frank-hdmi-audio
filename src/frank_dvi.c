/*
 * frank-hdmi-sound. DVI engine implementation.
 *
 * Wires up the per-instance state (`dvi_inst`), claims the three TMDS
 * DMA channel pairs (control + data per lane), pre-builds the
 * scanline DMA control-block lists for vsync, vblank, active,
 * blanked-active and error states, and runs the IRQ that hot-swaps
 * those lists at every scanline boundary.  Also exposes the worker
 * entry points the application calls from Core 1 to feed the encoder
 * (scanbuf and framebuf modes, 8bpp or 16bpp), plus the HDMI audio
 * data-island setup that adds CEA-861 InfoFrames and audio sample
 * packets to the stream.
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
#include <stdlib.h>
#include "hardware/dma.h"
#include "hardware/irq.h"

#include "frank_dvi.h"
#include "frank_dvi_timing.h"
#include "frank_serialiser.h"
#include "frank_tmds.h"

// Time-critical functions pulled into RAM but each in a unique section to
// allow garbage collection
#define __dvi_func(f) __not_in_flash_func(f)
#define __dvi_func_x(f) __scratch_x(__STRING(f)) f

// We require exclusive use of a DMA IRQ line. (you wouldn't want to share
// anyway). It's possible in theory to hook both IRQs and have two DVI outs.
static struct dvi_inst *dma_irq_privdata[2];
static void dvi_dma0_irq();
static void dvi_dma1_irq();

static inline void dvi_update_data_packet(struct dvi_inst *inst) {
    data_packet_t packet;
    if (!dvi_update_data_packet_(inst, &packet)) {
        set_null(&packet, sizeof(data_packet_t));
    }
    bool vsync = inst->timing_state.v_state == DVI_STATE_SYNC;
    encode(&inst->next_data_stream, &packet, inst->timing->v_sync_polarity == vsync, inst->timing->h_sync_polarity);
}

/*
 * One-shot bring-up for a DVI instance.
 *
 * The caller fills in the static fields of `inst` first.  The
 * important ones are `timing` (which mode to drive) and `ser_cfg`
 * (which PIO and GPIOs to drive).  This function:
 *
 *   1. Resets the runtime state of the timing state machine.
 *   2. Sets the audio-data-island sub-state to "no audio".
 *   3. Brings up the TMDS serialiser PIO state machines.
 *   4. Claims six DMA channels (two per TMDS lane: one for the
 *      control-block list, one for the symbol stream itself).
 *   5. Creates the four blocking queues that move scanlines and
 *      TMDS buffers between the producer (application) and the
 *      consumer (the IRQ-driven DMA chain).
 *   6. Pre-builds the DMA control-block lists for the scanline
 *      "shapes": vsync line, vblank line, active line, blanked-
 *      active line, plus an "error" line for when the producer
 *      underruns.
 *   7. Carves the TMDS symbol buffers out of a static pool sized at
 *      compile time (DVI_STATIC_TMDS_MAX_PIX, default 640) and
 *      pushes them into q_tmds_free so the encoder can pick them up.
 *   8. Fills the AVI InfoFrame with sensible defaults (RGB, 4:3
 *      aspect, full range, picked from the timing).
 *
 * The two `spinlock_*` arguments are pico_util spinlock numbers used
 * to make queue accesses safe across cores.  Pass distinct values
 * obtained via `next_striped_spin_lock_num()`.
 */
void dvi_init(struct dvi_inst *inst, uint spinlock_tmds_queue, uint spinlock_colour_queue) {
    inst->dvi_started = false;
    inst->timing_state.v_ctr  = 0;
    inst->dvi_frame_count = 0;

    dvi_audio_init(inst);
    dvi_timing_state_init(&inst->timing_state);
    dvi_serialiser_init(&inst->ser_cfg);
    for (int i = 0; i < N_TMDS_LANES; ++i) {
        inst->dma_cfg[i].chan_ctrl = dma_claim_unused_channel(true);
        inst->dma_cfg[i].chan_data = dma_claim_unused_channel(true);
        inst->dma_cfg[i].tx_fifo = (void*)&inst->ser_cfg.pio->txf[inst->ser_cfg.sm_tmds[i]];
        inst->dma_cfg[i].dreq = pio_get_dreq(inst->ser_cfg.pio, inst->ser_cfg.sm_tmds[i], true);
    }
    inst->late_scanline_ctr = 0;
    inst->tmds_buf_release[0] = NULL;
    inst->tmds_buf_release[1] = NULL;
    queue_init_with_spinlock(&inst->q_tmds_valid,   sizeof(void*),  8, spinlock_tmds_queue);
    queue_init_with_spinlock(&inst->q_tmds_free,    sizeof(void*),  8, spinlock_tmds_queue);
    queue_init_with_spinlock(&inst->q_colour_valid, sizeof(void*),  8, spinlock_colour_queue);
    queue_init_with_spinlock(&inst->q_colour_free,  sizeof(void*),  8, spinlock_colour_queue);

    dvi_setup_scanline_for_vblank(inst->timing, inst->dma_cfg, true, &inst->dma_list_vblank_sync);
    dvi_setup_scanline_for_vblank(inst->timing, inst->dma_cfg, false, &inst->dma_list_vblank_nosync);
    dvi_setup_scanline_for_active(inst->timing, inst->dma_cfg, (void*)SRAM_BASE, &inst->dma_list_active, false);
    dvi_setup_scanline_for_active(inst->timing, inst->dma_cfg, NULL, &inst->dma_list_error, false);
    dvi_setup_scanline_for_active(inst->timing, inst->dma_cfg, NULL, &inst->dma_list_active_blank, true);

    uint16_t mask = 0;

#ifdef DVI_1BPP_BUFFER
    mask = 0x1f;  // To account for worst case of 1bpp horizontal pixels generated 32 bits at a time (e.g. 720x568)
#endif

    // PATCH (frank-hdmi-sound): the upstream code malloc()s TMDS buffers, which
    // requires a heap large enough to hold ~12 KB.  On RP2350 builds with
    // a tight SRAM budget that pushes the firmware over the SRAM region.
    // Use a static buffer pool sized for the largest mode we care about
    // (720x576p, the worst case in dvi_timing.c).  Worst-case size:
    //   TMDS_CHANNELS (3) * ((720 + 31) & ~31) / 2 * 4 = 4608 bytes/buf
    // x DVI_N_TMDS_BUFFERS (3) = 13824 bytes total.
#ifndef DVI_STATIC_TMDS_MAX_PIX
#define DVI_STATIC_TMDS_MAX_PIX  640
#endif
#if DVI_MONOCHROME_TMDS
#define DVI_STATIC_TMDS_BYTES_PER_BUF \
    (((DVI_STATIC_TMDS_MAX_PIX) / DVI_SYMBOLS_PER_WORD) * sizeof(uint32_t))
#else
#define DVI_STATIC_TMDS_BYTES_PER_BUF \
    (TMDS_CHANNELS * ((DVI_STATIC_TMDS_MAX_PIX) / DVI_SYMBOLS_PER_WORD) * sizeof(uint32_t))
#endif
    static uint32_t __attribute__((aligned(4)))
        static_tmds_pool[DVI_N_TMDS_BUFFERS]
                        [DVI_STATIC_TMDS_BYTES_PER_BUF / sizeof(uint32_t)];

    /* Sanity check: if the user has selected a wider mode than the
     * static pool was sized for, fall back to malloc with a clear panic
     * instead of silent corruption. */
    {
        uint16_t needed_pix = (inst->timing->h_active_pixels + mask) & (~mask);
        if (needed_pix > DVI_STATIC_TMDS_MAX_PIX) {
            panic("DVI mode wider than static TMDS pool (%u > %u)",
                  needed_pix, DVI_STATIC_TMDS_MAX_PIX);
        }
    }

    for (int i = 0; i < DVI_N_TMDS_BUFFERS; ++i) {
        void *tmdsbuf = static_tmds_pool[i];
        queue_add_blocking_u32(&inst->q_tmds_free, &tmdsbuf);
    }

    set_AVI_info_frame(&inst->avi_info_frame, UNDERSCAN, RGB, ITU601, PIC_ASPECT_RATIO_4_3, SAME_AS_PAR, FULL,
                       (inst->timing->h_active_pixels == 720) ? _720x576P50 : _640x480P60);

}

/*
 * Hook the DMA-completion IRQ for the sync-lane data DMA.  Whichever
 * core calls this is the one the IRQ will fire on, which is why it's
 * a separate call from dvi_init().  The typical pattern: the
 * application's Core 0 calls dvi_init() (no IRQs of its own) and
 * Core 1 calls this from its entry point.
 *
 * irq_num is DMA_IRQ_0 or DMA_IRQ_1.  Both are wired up internally,
 * so callers can park their own DMA IRQs on the other one.
 */
void dvi_register_irqs_this_core(struct dvi_inst *inst, uint irq_num) {
    uint32_t mask_sync_channel = 1u << inst->dma_cfg[TMDS_SYNC_LANE].chan_data;
    uint32_t mask_all_channels = 0;
    for (int i = 0; i < N_TMDS_LANES; ++i)
        mask_all_channels |= 1u << inst->dma_cfg[i].chan_ctrl | 1u << inst->dma_cfg[i].chan_data;

    dma_hw->ints0 = mask_sync_channel;
    if (irq_num == DMA_IRQ_0) {
        hw_write_masked(&dma_hw->inte0, mask_sync_channel, mask_all_channels);
        dma_irq_privdata[0] = inst;
        irq_set_exclusive_handler(DMA_IRQ_0, dvi_dma0_irq);
    }
    else {
        hw_write_masked(&dma_hw->inte1, mask_sync_channel, mask_all_channels);
        dma_irq_privdata[1] = inst;
        irq_set_exclusive_handler(DMA_IRQ_1, dvi_dma1_irq);
    }
    irq_set_enabled(irq_num, true);
}

void dvi_unregister_irqs_this_core(struct dvi_inst *inst, uint irq_num) {
    irq_set_enabled(irq_num, false);
    if (irq_num == DMA_IRQ_0) {
         irq_remove_handler(DMA_IRQ_0, dvi_dma0_irq);
    } else {
         irq_remove_handler(DMA_IRQ_1, dvi_dma1_irq);
    }
    if (inst->tmds_buf_release[1]) {
        queue_try_add_u32(&inst->q_tmds_free, &inst->tmds_buf_release[1]);
    }
    if (inst->tmds_buf_release[0]) {
        queue_try_add_u32(&inst->q_tmds_free, &inst->tmds_buf_release[0]);
    }
    inst->tmds_buf_release[1] = NULL;
    inst->tmds_buf_release[0] = NULL;
}

// Set up control channels to make transfers to data channels' control
// registers (but don't trigger the control channels -- this is done either by
// data channel CHAIN_TO or an initial write to MULTI_CHAN_TRIGGER)
static inline void __attribute__((always_inline)) _dvi_load_dma_op(const struct dvi_lane_dma_cfg dma_cfg[], struct dvi_scanline_dma_list *l) {
    for (int i = 0; i < N_TMDS_LANES; ++i) {
        dma_channel_config cfg = dma_channel_get_default_config(dma_cfg[i].chan_ctrl);
        channel_config_set_ring(&cfg, true, 4); // 16-byte write wrap
        channel_config_set_read_increment(&cfg, true);
        channel_config_set_write_increment(&cfg, true);
        dma_channel_configure(
            dma_cfg[i].chan_ctrl,
            &cfg,
            &dma_hw->ch[dma_cfg[i].chan_data],
            dvi_lane_from_list(l, i),
            4, // Configure all 4 registers then halt until next CHAIN_TO
            false
        );
    }
}

/*
 * Set the DMA chain in motion and unblank the TMDS serialiser.
 *
 * Configures each lane's control channel to feed the data channel's
 * registers from the pre-built scanline DMA list, then triggers all
 * three control channels in lockstep.  After the first scanline
 * runs, each data channel's CHAIN_TO retriggers its own control
 * channel, so the chain runs forever from a single trigger.
 *
 * The TMDS PIO state machines are deliberately enabled *after* their
 * TX FIFOs are full.  Starting them with a partially-filled FIFO
 * guarantees an underrun on the very first scanline and the receiver
 * never locks.
 *
 * The DMA IRQ handler must be registered
 * (dvi_register_irqs_this_core) before this is called.  The chain
 * needs an IRQ every scanline to swap the next list in.
 */
void dvi_start(struct dvi_inst *inst) {
    if (inst->dvi_started) {
        return;
    }
    _dvi_load_dma_op(inst->dma_cfg, &inst->dma_list_vblank_nosync);
    dma_start_channel_mask(
        (1u << inst->dma_cfg[0].chan_ctrl) |
        (1u << inst->dma_cfg[1].chan_ctrl) |
        (1u << inst->dma_cfg[2].chan_ctrl));

    // We really don't want the FIFOs to bottom out, so wait for full before
    // starting the shift-out.
    for (int i = 0; i < N_TMDS_LANES; ++i)
        while (!pio_sm_is_tx_fifo_full(inst->ser_cfg.pio, inst->ser_cfg.sm_tmds[i]))
            tight_loop_contents();
    dvi_serialiser_enable(&inst->ser_cfg, true);
    inst->dvi_started = true;
}

/*
 * Tear the DMA chain down and silence the TMDS lanes.  Aborts every
 * lane's control and data channel, acks any pending IRQ, and turns
 * off the serialiser PIO state machines.  Safe to call when the
 * instance isn't running; early-exits in that case.
 */
void dvi_stop(struct dvi_inst *inst) {
    if (!inst->dvi_started) {
        return;
    }
    uint mask  = 0;
    for (int i = 0; i < N_TMDS_LANES; ++i) {
        dma_channel_config cfg = dma_channel_get_default_config(inst->dma_cfg[i].chan_ctrl);
        dma_channel_set_config(inst->dma_cfg[i].chan_ctrl, &cfg, false);
        cfg = dma_channel_get_default_config(inst->dma_cfg[i].chan_data);
        dma_channel_set_config(inst->dma_cfg[i].chan_data, &cfg, false);
        mask |= 1 << inst->dma_cfg[i].chan_data;
        mask |= 1 << inst->dma_cfg[i].chan_ctrl;
    }

    dma_channel_abort(mask);
    dma_irqn_acknowledge_channel(0, inst->dma_cfg[TMDS_SYNC_LANE].chan_data);
    dma_hw->ints0 = 1u << inst->dma_cfg[TMDS_SYNC_LANE].chan_data;

    dvi_serialiser_enable(&inst->ser_cfg, false);
    inst->dvi_started = false;
}

static inline void __dvi_func_x(_dvi_prepare_scanline_8bpp)(struct dvi_inst *inst, uint32_t *scanbuf) {
    uint32_t *tmdsbuf = NULL;
    queue_remove_blocking_u32(&inst->q_tmds_free, &tmdsbuf);
    uint pixwidth = inst->timing->h_active_pixels;
    uint words_per_channel = pixwidth / DVI_SYMBOLS_PER_WORD;
    // Scanline buffers are half-resolution; the functions take the number of *input* pixels as parameter.
    tmds_encode_data_channel_8bpp(scanbuf, tmdsbuf + 0 * words_per_channel, pixwidth / 2, DVI_8BPP_BLUE_MSB,  DVI_8BPP_BLUE_LSB );
    tmds_encode_data_channel_8bpp(scanbuf, tmdsbuf + 1 * words_per_channel, pixwidth / 2, DVI_8BPP_GREEN_MSB, DVI_8BPP_GREEN_LSB);
    tmds_encode_data_channel_8bpp(scanbuf, tmdsbuf + 2 * words_per_channel, pixwidth / 2, DVI_8BPP_RED_MSB,   DVI_8BPP_RED_LSB  );
    queue_add_blocking_u32(&inst->q_tmds_valid, &tmdsbuf);
}

static inline void __dvi_func_x(_dvi_prepare_scanline_16bpp)(struct dvi_inst *inst, uint32_t *scanbuf) {
    uint32_t *tmdsbuf = NULL;
    queue_remove_blocking_u32(&inst->q_tmds_free, &tmdsbuf);
    uint pixwidth = inst->timing->h_active_pixels;
    uint words_per_channel = pixwidth / DVI_SYMBOLS_PER_WORD;
    tmds_encode_data_channel_16bpp(scanbuf, tmdsbuf + 0 * words_per_channel, pixwidth / 2, DVI_16BPP_BLUE_MSB,  DVI_16BPP_BLUE_LSB );
    tmds_encode_data_channel_16bpp(scanbuf, tmdsbuf + 1 * words_per_channel, pixwidth / 2, DVI_16BPP_GREEN_MSB, DVI_16BPP_GREEN_LSB);
    tmds_encode_data_channel_16bpp(scanbuf, tmdsbuf + 2 * words_per_channel, pixwidth / 2, DVI_16BPP_RED_MSB,   DVI_16BPP_RED_LSB  );
    queue_add_blocking_u32(&inst->q_tmds_valid, &tmdsbuf);
}

/*
 * "Worker thread" entry points.  Each of these consumes scanlines
 * from `q_colour_valid`, runs the encoder over them, and pushes the
 * resulting TMDS buffer into `q_tmds_valid` for the DMA chain to
 * pick up.  They never return; the calling core enters one and stays
 * inside the loop forever (still servicing the DMA IRQ that was
 * registered earlier).
 *
 * The "scanbuf" variants treat each `q_colour_valid` entry as a
 * single scanline.  The "framebuf" variants below treat it as a
 * pointer to the start of a whole framebuffer and walk through it
 * line-by-line internally, which is useful if you'd rather not push
 * every line through the queue.
 */
void __dvi_func(dvi_scanbuf_main_8bpp)(struct dvi_inst *inst) {
    while (1) {
        uint32_t *scanbuf = NULL;
        queue_remove_blocking_u32(&inst->q_colour_valid, &scanbuf);
        _dvi_prepare_scanline_8bpp(inst, scanbuf);
        queue_add_blocking_u32(&inst->q_colour_free, &scanbuf);
    }
    __builtin_unreachable();
}

/*
 * Same shape as the 8bpp worker, but for RGB565 inputs.  The two
 * versions are kept as separate functions on purpose: that way the
 * linker can garbage-collect whichever encoder loops the application
 * doesn't reach, instead of dragging both into scratch_x.
 */
void __dvi_func(dvi_scanbuf_main_16bpp)(struct dvi_inst *inst) {
    while (1) {
        uint32_t *scanbuf = NULL;
        queue_remove_blocking_u32(&inst->q_colour_valid, &scanbuf);
        _dvi_prepare_scanline_16bpp(inst, scanbuf);
        queue_add_blocking_u32(&inst->q_colour_free, &scanbuf);
    }
    __builtin_unreachable();
}

/*
 * Per-scanline IRQ.  Fires four times per line, once each for the
 * front porch, sync, back porch and active region.  On the active
 * edge we have one full active scanline of "headroom" to install the
 * DMA control-block list for the *next* line.  In broad strokes:
 *
 *   1. Advance the timing state machine (front porch -> sync -> ...)
 *      so we know what kind of line is coming up.
 *   2. Park the previous TMDS buffer on the free queue.  This is
 *      deferred by one scanline because the data DMA may still be
 *      reading it when the IRQ fires.
 *   3. If we owe scanlines to the encoder (the "late" counter),
 *      drop the next valid buffer on the floor instead of displaying
 *      it: the buffer was generated for an earlier vertical position
 *      and using it now would tear the picture.
 *   4. Pick the right pre-built DMA list (vsync, vblank, active,
 *      blanked-active, error) for this line and load it onto the
 *      control channels.
 *   5. If audio is enabled, ask the data-island packetiser for the
 *      next packet to interleave into this line's blanking interval.
 */
static void __dvi_func(dvi_dma_irq_handler)(struct dvi_inst *inst) {
    // Every fourth interrupt marks the start of the horizontal active region. We
    // now have until the end of this region to generate DMA blocklist for next
    // scanline.
    dvi_timing_state_advance(inst->timing, &inst->timing_state);

    // Make sure all three channels have definitely loaded their last block
    // (should be within a few cycles of one another)
    for (int i = 0; i < N_TMDS_LANES; ++i) {
        while (dma_debug_hw->ch[inst->dma_cfg[i].chan_data].dbg_tcr != inst->timing->h_active_pixels / DVI_SYMBOLS_PER_WORD) {
            tight_loop_contents();
        }
    }

    if (inst->tmds_buf_release[1] && !queue_try_add_u32(&inst->q_tmds_free, &inst->tmds_buf_release[1])) {
        panic("TMDS free queue full in IRQ!");
    }
    inst->tmds_buf_release[1] = inst->tmds_buf_release[0];
    inst->tmds_buf_release[0] = NULL;

    uint32_t *tmdsbuf = NULL;
    while (inst->late_scanline_ctr > 0 && queue_try_remove_u32(&inst->q_tmds_valid, &tmdsbuf)) {
        // If we displayed this buffer then it would be in the wrong vertical
        // position on-screen. Just pass it back.
        queue_add_blocking_u32(&inst->q_tmds_free, &tmdsbuf);
        --inst->late_scanline_ctr;
    }

    struct dvi_scanline_dma_list *dma_list_selected = &inst->dma_list_vblank_nosync;
    switch (inst->timing_state.v_state) {
        case DVI_STATE_ACTIVE:
        {
            bool is_blank_line = false;
            if (inst->timing_state.v_ctr < inst->blank_settings.top ||
                inst->timing_state.v_ctr >= (inst->timing->v_active_lines - inst->blank_settings.bottom)) {
                // Is a Blank Line
                is_blank_line = true;
            } else {
                if (queue_try_peek_u32(&inst->q_tmds_valid, &tmdsbuf)) {
                    if (inst->timing_state.v_ctr % DVI_VERTICAL_REPEAT == DVI_VERTICAL_REPEAT - 1) {
                        queue_remove_blocking_u32(&inst->q_tmds_valid, &tmdsbuf);
                        inst->tmds_buf_release[0] = tmdsbuf;
                    }
                } else {
                    // No valid scanline was ready (generates solid red scanline)
                    tmdsbuf = NULL;
                    if (inst->timing_state.v_ctr % DVI_VERTICAL_REPEAT == DVI_VERTICAL_REPEAT - 1) {
                        ++inst->late_scanline_ctr;
                    }
                }

                if (inst->scanline_is_enabled && (inst->timing_state.v_ctr & 1)) {
                    is_blank_line = true;
                }
            }

            if (is_blank_line) {
                dma_list_selected = &inst->dma_list_active_blank;
            } else if (tmdsbuf) {
                dvi_update_scanline_data_dma(inst->timing, tmdsbuf, &inst->dma_list_active, inst->data_island_is_enabled);
                dma_list_selected =  &inst->dma_list_active;
            } else {
                dma_list_selected = &inst->dma_list_error;
            }

            if (inst->scanline_callback && inst->timing_state.v_ctr % DVI_VERTICAL_REPEAT == DVI_VERTICAL_REPEAT - 1) {
                inst->scanline_callback(inst->timing_state.v_ctr / DVI_VERTICAL_REPEAT);
            }
        }
        break;

        case DVI_STATE_SYNC:
            dma_list_selected = &inst->dma_list_vblank_sync;
            if (inst->timing_state.v_ctr == 0) {
                ++inst->dvi_frame_count;
            }
            break;
        default: break;
    }
    _dvi_load_dma_op(inst->dma_cfg, dma_list_selected);

    if (inst->data_island_is_enabled) {
        dvi_update_data_packet(inst);
    }
}

static void __dvi_func(dvi_dma0_irq)() {
    struct dvi_inst *inst = dma_irq_privdata[0];
    dma_hw->ints0 = 1u << inst->dma_cfg[TMDS_SYNC_LANE].chan_data;
    dvi_dma_irq_handler(inst);
}

static void __dvi_func(dvi_dma1_irq)() {
    struct dvi_inst *inst = dma_irq_privdata[1];
    dma_hw->ints1 = 1u << inst->dma_cfg[TMDS_SYNC_LANE].chan_data;
    dvi_dma_irq_handler(inst);
}

/* ----- HDMI data-island / audio API ----------------------------- */

/* Reset the audio sub-state to "no audio".  Called from dvi_init().
 * After this, calling dvi_audio_sample_buffer_set followed by
 * dvi_set_audio_freq turns audio back on. */
void dvi_audio_init(struct dvi_inst *inst) {
    inst->data_island_is_enabled = false;
    inst->scanline_is_enabled = false;
    inst->audio_freq = 0;
    inst->samples_per_frame = 0;
    inst->samples_per_line24 = 0;
    inst->audio_sample_pos = 0;
    inst->audio_frame_count = 0;
}

/*
 * Switch from a pure-DVI signal to an HDMI signal that carries data
 * islands.  Re-builds every scanline DMA list using the "with audio"
 * variants (which leave a gap inside the horizontal blanking
 * interval for the data-island packets) and points each list at the
 * shared `next_data_stream` buffer.
 *
 * If you actually want audio, call dvi_audio_sample_buffer_set
 * followed by dvi_set_audio_freq before this; on its own it only
 * enables the InfoFrame slot.
 */
void dvi_enable_data_island(struct dvi_inst *inst) {
    inst->data_island_is_enabled  = true;

    dvi_setup_scanline_for_vblank_with_audio(inst->timing, inst->dma_cfg, true, &inst->dma_list_vblank_sync);
    dvi_setup_scanline_for_vblank_with_audio(inst->timing, inst->dma_cfg, false, &inst->dma_list_vblank_nosync);
    dvi_setup_scanline_for_active_with_audio(inst->timing, inst->dma_cfg, (void*)SRAM_BASE, &inst->dma_list_active, false);
    dvi_setup_scanline_for_active_with_audio(inst->timing, inst->dma_cfg, NULL, &inst->dma_list_error, false);
    dvi_setup_scanline_for_active_with_audio(inst->timing, inst->dma_cfg, NULL, &inst->dma_list_active_blank, true);

    // Setup internal Data Packet streams
    dvi_update_data_island_ptr(&inst->dma_list_vblank_sync,   &inst->next_data_stream);
    dvi_update_data_island_ptr(&inst->dma_list_vblank_nosync, &inst->next_data_stream);
    dvi_update_data_island_ptr(&inst->dma_list_active,        &inst->next_data_stream);
    dvi_update_data_island_ptr(&inst->dma_list_error,         &inst->next_data_stream);
    dvi_update_data_island_ptr(&inst->dma_list_active_blank,  &inst->next_data_stream);
}

void dvi_update_data_island_ptr(struct dvi_scanline_dma_list *dma_list, data_island_stream_t *stream) {
    for (int i = 0; i < N_TMDS_LANES; ++i) {
        dma_cb_t *cblist = dvi_lane_from_list(dma_list, i);
        uint32_t *src = stream->data[i];

        if (i == TMDS_SYNC_LANE) {
            cblist[1].read_addr = src;
        } else {
            cblist[2].read_addr = src;
        }
    }
}

/*
 * Hand the driver the storage for the audio ring.  size must be a
 * power of two.  The producer pushes int16 stereo frames into it via
 * the public frank_hdmi_audio_write() helper, and the IRQ pulls
 * them out a few at a time per scanline.
 */
void dvi_audio_sample_buffer_set(struct dvi_inst *inst, audio_sample_t *buffer, int size) {
    audio_ring_set(&inst->audio_ring, buffer, size);
}

// video_freq: video sampling frequency
// audio_freq: audio sampling frequency
// CTS: Cycle Time Stamp
// N: HDMI Constant
// 128 * audio_freq = video_freq * N / CTS
// e.g.: video_freq = 23495525, audio_freq = 44100 , CTS = 28000, N = 6727
/*
 * Tell the driver which HDMI audio sample rate to advertise on the
 * wire, and how to clock-regenerate it on the receiver.
 *
 * `audio_freq` is the nominal sample rate in Hz (32000, 44100, 48000
 * or a CEA-861 multiple).  `cts` and `n` are the audio-clock-
 * regeneration values per CEA-861:
 *
 *     128 * audio_freq = pixel_freq * n / cts
 *
 * Pick n from the CEA-861 standard table for the chosen sample rate
 * and compute cts from the active video clock; the receiver stays
 * locked.  After this call, the data-island stream is automatically
 * enabled.
 */
void dvi_set_audio_freq(struct dvi_inst *inst, int audio_freq, int cts, int n) {
    inst->audio_freq = audio_freq;
    set_audio_clock_regeneration(&inst->audio_clock_regeneration, cts, n);
    set_audio_info_frame(&inst->audio_info_frame, audio_freq);
    uint pixelClock =   dvi_timing_get_pixel_clock(inst->timing);
    uint64_t nPixPerFrame = dvi_timing_get_pixels_per_frame(inst->timing);
    uint64_t nPixPerLine =  dvi_timing_get_pixels_per_line(inst->timing);
    inst->samples_per_frame  = (uint64_t)(audio_freq) * nPixPerFrame / pixelClock;
    uint64_t t = audio_freq * nPixPerLine * (uint64_t)0x1000000;
    inst->samples_per_line24 = t / pixelClock;
    dvi_enable_data_island(inst);
}

void dvi_wait_for_valid_line(struct dvi_inst *inst) {
    uint32_t *tmdsbuf = NULL;
    queue_peek_blocking_u32(&inst->q_colour_valid, &tmdsbuf);
}

bool __dvi_func(dvi_update_data_packet_)(struct dvi_inst *inst, data_packet_t *packet) {
    if (inst->samples_per_frame == 0) {
        return false;
    }

    inst->audio_sample_pos += inst->samples_per_line24;
    if (inst->timing_state.v_state == DVI_STATE_FRONT_PORCH) {
        if (inst->timing_state.v_ctr == 0) {
            if (inst->dvi_frame_count & 1) {
                *packet = inst->avi_info_frame;
            } else {
                *packet = inst->audio_info_frame;
            }
            return true;
        } else if (inst->timing_state.v_ctr == 1) {
            *packet = inst->audio_clock_regeneration;

            return true;
        }
    }
    const int sample_pos_24 = inst->audio_sample_pos >> 24;
    const int n = MIN(4, sample_pos_24);
    if (n)
    {
        inst->audio_sample_pos -= (n << 24);
        inst->audio_frame_count = set_audio_sample(packet, &inst->audio_ring, n, inst->audio_frame_count);
        return true;
    }

    return false;
}
