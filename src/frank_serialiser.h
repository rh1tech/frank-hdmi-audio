/*
 * Differential TMDS serialiser config.  Picks the PIO instance, state
 * machine numbers and GPIO pin pairs that drive the three TMDS data
 * lanes plus the pixel clock.
 *
 * (c) 2026 Mikhail Matveev <xtreme@rh1.tech>, https://rh1.tech
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Based on libdvi by Luke Wren and contributors
 * (https://github.com/Wren6991/PicoDVI).
 *
 * Copyright (c) 2021 Luke Wren and contributors.
 */
#ifndef FRANK_SERIALISER_H_
#define FRANK_SERIALISER_H_

#include "hardware/pio.h"
#include "frank_dvi_config.h"

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
