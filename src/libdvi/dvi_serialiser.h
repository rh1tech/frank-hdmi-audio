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
#ifndef _DVI_SERIALISER_H
#define _DVI_SERIALISER_H

#include "hardware/pio.h"
#include "dvi_config_defs.h"

#define N_TMDS_LANES 3

struct dvi_serialiser_cfg {
	PIO pio;
	uint sm_tmds[N_TMDS_LANES];
	uint pins_tmds[N_TMDS_LANES];
	uint pins_clk;
	bool invert_diffpairs;
	uint prog_offs;
};

void dvi_serialiser_init(struct dvi_serialiser_cfg *cfg);
void dvi_serialiser_enable(struct dvi_serialiser_cfg *cfg, bool enable);
uint32_t dvi_single_to_diff(uint32_t in);

#endif
