/*
 * frank-hdmi-sound — TMDS encoder API.
 *
 * (c) 2026 Mikhail Matveev <xtreme@rh1.tech>, https://rh1.tech
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Trimmed-down derivative of the libdvi TMDS encoder header by
 * Luke Wren and contributors (https://github.com/Wren6991/PicoDVI),
 * with HDMI audio additions from shuichitakano's PicoDVI-audio fork
 * (https://github.com/shuichitakano/PicoDVI-audio).  The fullres,
 * 1bpp and palette functions from upstream are not exposed because
 * frank-hdmi-sound does not use them.
 *
 * Copyright (c) 2021 Luke Wren and contributors.
 */
#ifndef FRANK_TMDS_H_
#define FRANK_TMDS_H_

#include "hardware/interp.h"
#include "frank_dvi_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* C-side encoders.  Pixel-doubling on the wire: each input pixel
 * produces two output TMDS symbols, so n_pix is half the active
 * scanline width. */
void tmds_encode_data_channel_16bpp(const uint32_t *pixbuf, uint32_t *symbuf,
                                    size_t n_pix, uint channel_msb, uint channel_lsb);
void tmds_encode_data_channel_8bpp(const uint32_t *pixbuf, uint32_t *symbuf,
                                   size_t n_pix, uint channel_msb, uint channel_lsb);

/* Inner loops live in src/frank_tmds.S; declared here so the C
 * encoders above can call them.  Both the plain and "leftshift"
 * variants use SIO interpolator 0; the 8bpp pair also uses interp1. */
void tmds_encode_loop_16bpp(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix);
void tmds_encode_loop_16bpp_leftshift(const uint32_t *pixbuf, uint32_t *symbuf,
                                      size_t n_pix, uint leftshift);
void tmds_encode_loop_8bpp(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix);
void tmds_encode_loop_8bpp_leftshift(const uint32_t *pixbuf, uint32_t *symbuf,
                                     size_t n_pix, uint leftshift);

#ifdef __cplusplus
}
#endif

#endif /* FRANK_TMDS_H_ */
