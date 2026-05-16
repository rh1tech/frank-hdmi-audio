/*
 * frank-hdmi-sound — TMDS encoder driver glue.
 *
 * (c) 2026 Mikhail Matveev <xtreme@rh1.tech>, https://rh1.tech
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Trimmed-down derivative of the TMDS encoder in libdvi by Luke Wren
 * and contributors (PicoDVI, https://github.com/Wren6991/PicoDVI),
 * with HDMI audio additions by shuichitakano (PicoDVI-audio,
 * https://github.com/shuichitakano/PicoDVI-audio).  The fullres /
 * palette / 1bpp paths have been removed; we only ship the
 * pixel-doubling 8bpp and 16bpp encoders that the frank-hdmi-sound
 * scanline pipeline actually uses.
 *
 * Copyright (c) 2021 Luke Wren and contributors.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "hardware/interp.h"
#include "frank_tmds.h"

/*
 * The TMDS lookup table maps a 6-bit colour-channel value to a pair
 * of 10-bit TMDS symbols.  Pinned in scratch_x so reads don't compete
 * with framebuffer traffic on the main SRAM banks.
 */
static const uint32_t __scratch_x("tmds_table") tmds_table[] = {
#include "frank_tmds_table.h"
};

/*
 * Configure one of the SIO interpolators so that, given a pair of
 * pixels packed into a single uint32 word, lane 0 produces the LUT
 * address for the first pixel's colour channel and lane 1 produces
 * the address for the second.
 *
 * Returns the number of bits of left shift the inner loop needs to
 * apply in software before the interpolator gets a clean shot at the
 * channel.  The Pico interpolator can only shift right, and the blue
 * channel of an RGB565 pixel sits in the bottom bits, so the encoder
 * loop fixes up that channel's shift amount manually.
 */
static int __not_in_flash_func(configure_interp_for_addrgen)(interp_hw_t *interp,
                                                             uint channel_msb,
                                                             uint channel_lsb,
                                                             uint pixel_lsb,
                                                             uint pixel_width,
                                                             uint lut_index_width,
                                                             const uint32_t *lutbase) {
    interp_config c;
    const uint index_shift = 2;   /* LUT entries are 4 bytes */

    int shift_channel_to_index = pixel_lsb + channel_msb - (lut_index_width - 1) - index_shift;
    int oops = 0;
    if (shift_channel_to_index < 0) {
        oops = -shift_channel_to_index;
        shift_channel_to_index = 0;
    }

    uint index_msb = index_shift + lut_index_width - 1;

    c = interp_default_config();
    interp_config_set_shift(&c, shift_channel_to_index);
    interp_config_set_mask(&c, index_msb - (channel_msb - channel_lsb), index_msb);
    interp_set_config(interp, 0, &c);

    c = interp_default_config();
    interp_config_set_shift(&c, pixel_width + shift_channel_to_index);
    interp_config_set_mask(&c, index_msb - (channel_msb - channel_lsb), index_msb);
    interp_config_set_cross_input(&c, true);
    interp_set_config(interp, 1, &c);

    interp->base[0] = (uint32_t)lutbase;
    interp->base[1] = (uint32_t)lutbase;

    return oops;
}

/*
 * Encode one colour channel of a 16bpp (RGB565) pixel buffer into a
 * buffer of 10-bit TMDS symbols.  n_pix must be even and the input
 * buffer must be word-aligned.  This is the workhorse of the frank-
 * hdmi-sound scanline path.
 */
void __not_in_flash_func(tmds_encode_data_channel_16bpp)(const uint32_t *pixbuf,
                                                         uint32_t *symbuf,
                                                         size_t n_pix,
                                                         uint channel_msb,
                                                         uint channel_lsb) {
    interp_hw_save_t interp0_save;
    interp_save(interp0_hw, &interp0_save);
    int require_lshift = configure_interp_for_addrgen(interp0_hw, channel_msb, channel_lsb,
                                                      0, 16, 6, tmds_table);
    if (require_lshift) {
        tmds_encode_loop_16bpp_leftshift(pixbuf, symbuf, n_pix, require_lshift);
    } else {
        tmds_encode_loop_16bpp(pixbuf, symbuf, n_pix);
    }
    interp_restore(interp0_hw, &interp0_save);
}

/*
 * 8bpp variant — used by libdvi's framebuffer-mode 8bpp consumer.
 * frank-hdmi-sound does not exercise this path itself but it stays in
 * the build so dvi_scanbuf_main_8bpp / dvi_framebuf_main_8bpp link
 * cleanly for callers that want them.
 */
void __not_in_flash_func(tmds_encode_data_channel_8bpp)(const uint32_t *pixbuf,
                                                        uint32_t *symbuf,
                                                        size_t n_pix,
                                                        uint channel_msb,
                                                        uint channel_lsb) {
    interp_hw_save_t interp0_save, interp1_save;
    interp_save(interp0_hw, &interp0_save);
    interp_save(interp1_hw, &interp1_save);
    int require_lshift = configure_interp_for_addrgen(interp0_hw, channel_msb, channel_lsb,
                                                      0, 8, 6, tmds_table);
    int lshift_upper   = configure_interp_for_addrgen(interp1_hw, channel_msb, channel_lsb,
                                                      16, 8, 6, tmds_table);
    (void)lshift_upper;
    if (require_lshift || (DVI_SYMBOLS_PER_WORD == 1)) {
        tmds_encode_loop_8bpp_leftshift(pixbuf, symbuf, n_pix, require_lshift);
    } else {
        tmds_encode_loop_8bpp(pixbuf, symbuf, n_pix);
    }
    interp_restore(interp0_hw, &interp0_save);
    interp_restore(interp1_hw, &interp1_save);
}
