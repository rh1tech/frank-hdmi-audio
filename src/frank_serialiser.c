/*
 * Brings the TMDS serialiser hardware up.  Configures three PIO state
 * machines (one per TMDS data lane), drives the pixel clock either
 * from a PWM slice or a fourth PIO SM, and applies the per-pad drive,
 * slew and inversion settings.
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
#include "pico.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/structs/padsbank0.h"

#include "frank_dvi.h"
#include "frank_serialiser.h"
#include "frank_serialiser.pio.h"
#include "frank_clock.pio.h"

#if defined(USE_PIO_TMDS_ENCODE) || !defined (DVI_USE_PIO_CLOCK)
#define USE_PWM_CLOCK
#endif

#ifndef USE_PWM_CLOCK
static int clk_sm = 0;
#endif

/*
 * Apply the pad-control settings appropriate for an HDMI line: low
 * drive strength with slew limiting (the 3V3 LDO stays cool and most
 * receivers are happy with the resulting edge rates) and disable the
 * digital input buffer (we never read these pins).  GPIO inversion
 * is applied on top, picked from `invert_diffpairs` in the config.
 * Boards that wire P/N the wrong way round flip the bit here without
 * touching the rest of the pipeline.
 */
static void dvi_configure_pad(uint gpio, bool invert) {
	// 2 mA drive, enable slew rate limiting (this seems fine even at 720p30, and
	// the 3V3 LDO doesn't get warm like when turning all the GPIOs up to 11).
	// Also disable digital receiver.
	hw_write_masked(
		&padsbank0_hw->io[gpio],
		(0 << PADS_BANK0_GPIO0_DRIVE_LSB),
		PADS_BANK0_GPIO0_DRIVE_BITS | PADS_BANK0_GPIO0_SLEWFAST_BITS | PADS_BANK0_GPIO0_IE_BITS
	);
	gpio_set_outover(gpio, invert ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);
}

/*
 * Bring up the TMDS serialiser hardware described by `cfg`.
 *
 * Loads the serialiser PIO program once, then for each of the three
 * data lanes claims the requested state machine, configures it for
 * the named GPIO pair, and applies the pad settings.
 *
 * The pixel clock is generated either by a PWM slice (the default;
 * the PWM hardware is rock-steady at 50% duty across both pins of
 * the pair) or by a fourth PIO state machine, depending on whether
 * the build defines DVI_USE_PIO_CLOCK.  PWM mode requires the clock
 * pin to be even (PWM slice constraint).
 */
void dvi_serialiser_init(struct dvi_serialiser_cfg *cfg) {
#if DVI_SERIAL_DEBUG
	uint offset = pio_add_program(cfg->pio, &dvi_serialiser_debug_program);
#else
	uint offset = pio_add_program(cfg->pio, &dvi_serialiser_program);
#endif
	cfg->prog_offs = offset;

	for (int i = 0; i < N_TMDS_LANES; ++i) {
		pio_sm_claim(cfg->pio, cfg->sm_tmds[i]);
		dvi_serialiser_program_init(
			cfg->pio,
			cfg->sm_tmds[i],
			offset,
			cfg->pins_tmds[i],
			DVI_SERIAL_DEBUG
		);
		dvi_configure_pad(cfg->pins_tmds[i], cfg->invert_diffpairs);
		dvi_configure_pad(cfg->pins_tmds[i] + 1, cfg->invert_diffpairs);
	}

#ifdef USE_PWM_CLOCK
	// Use a PWM slice to drive the pixel clock. Both GPIOs must be on the same
	// slice (lower-numbered GPIO must be even).
	assert(cfg->pins_clk % 2 == 0);
	uint slice = pwm_gpio_to_slice_num(cfg->pins_clk);
	// 5 cycles high, 5 low. Invert one channel so that we get complementary outputs.
	pwm_config pwm_cfg = pwm_get_default_config();
	pwm_config_set_output_polarity(&pwm_cfg, true, false);
	pwm_config_set_wrap(&pwm_cfg, 9);
	// PATCH (frank-hdmi-sound): when sys_clock is an integer multiple of the
	// TMDS bit clock, divide the PWM clock by the same factor so the
	// pixel-clock output runs at spec while the CPU stays fast.
#ifdef DVI_SM_CLKDIV
	pwm_config_set_clkdiv_int(&pwm_cfg, DVI_SM_CLKDIV);
#endif
	pwm_init(slice, &pwm_cfg, false);
	pwm_set_both_levels(slice, 5, 5);
#else
	// Use a state machine to generate the clock
	clk_sm = pio_claim_unused_sm(cfg->pio, true);
    offset = pio_add_program(cfg->pio, &dvi_clock_program);
	dvi_clock_program_init(cfg->pio, clk_sm, offset, cfg->pins_clk);
#endif

	for (uint i = cfg->pins_clk; i <= cfg->pins_clk + 1; ++i) {
#ifdef USE_PWM_CLOCK
		gpio_set_function(i, GPIO_FUNC_PWM);
#endif
		dvi_configure_pad(i, cfg->invert_diffpairs);
	}
}

/*
 * Master enable for the TMDS serialiser.  Toggles the three (or
 * four, with PIO clock) state machines and the pixel-clock source
 * together.  The DVI spec allows a phase offset between the data
 * and clock lanes, so the data SMs and the clock generator don't
 * have to be enabled in the same cycle.
 */
void dvi_serialiser_enable(struct dvi_serialiser_cfg *cfg, bool enable) {
	uint mask = 0;
	for (int i = 0; i < N_TMDS_LANES; ++i)
		mask |= 1u << (cfg->sm_tmds[i] + PIO_CTRL_SM_ENABLE_LSB);
	if (enable) {
		// The DVI spec allows for phase offset between clock and data links.
		// So PWM and PIO do not need to be synchronised perfectly.
		hw_set_bits(&cfg->pio->ctrl, mask);
#ifdef USE_PWM_CLOCK
		pwm_set_enabled(pwm_gpio_to_slice_num(cfg->pins_clk), true);
#else
    	pio_sm_set_enabled(cfg->pio, clk_sm, true);
#endif
	}
	else {
		hw_clear_bits(&cfg->pio->ctrl, mask);
#ifdef USE_PWM_CLOCK
		pwm_set_enabled(pwm_gpio_to_slice_num(cfg->pins_clk), false);
#else
    	pio_sm_set_enabled(cfg->pio, clk_sm, false);
#endif
	}
}
