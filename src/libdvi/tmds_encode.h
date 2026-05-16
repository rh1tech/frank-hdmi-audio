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
#ifndef _TMDS_ENCODE_H_
#define _TMDS_ENCODE_H_

#include "hardware/interp.h"
#include "dvi_config_defs.h"

// Functions from tmds_encode.c
void tmds_encode_data_channel_16bpp(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix, uint channel_msb, uint channel_lsb);
void tmds_encode_data_channel_8bpp(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix, uint channel_msb, uint channel_lsb);
void tmds_encode_data_channel_fullres_16bpp(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix, uint channel_msb, uint channel_lsb);
void tmds_setup_palette_symbols(const uint16_t *palette, uint32_t *symbuf, size_t n_palette);
void tmds_setup_palette24_symbols(const uint32_t *palette, uint32_t *symbuf, size_t n_palette);
void tmds_encode_palette_data(const uint32_t *pixbuf, const uint32_t *tmds_palette, uint32_t *symbuf, size_t n_pix, uint32_t palette_bits);

// Functions from tmds_encode.S

void tmds_encode_1bpp(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix);
void tmds_encode_2bpp(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix);

// Uses interp0:
void tmds_encode_loop_16bpp(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix);
void tmds_encode_loop_16bpp_leftshift(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix, uint leftshift);

// Uses interp0 and interp1:
void tmds_encode_loop_8bpp(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix);
void tmds_encode_loop_8bpp_leftshift(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix, uint leftshift);

// Uses interp0 and interp1:
// (Note a copy is provided in scratch memories X and Y)
void tmds_fullres_encode_loop_16bpp_x(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix);
void tmds_fullres_encode_loop_16bpp_y(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix);
void tmds_fullres_encode_loop_16bpp_leftshift_x(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix, uint leftshift);
void tmds_fullres_encode_loop_16bpp_leftshift_y(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix, uint leftshift);
void tmds_palette_encode_loop_x(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix);
void tmds_palette_encode_loop_y(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix);

#endif
