/*
 * TMDS encoder inner loops, written in Cortex-M0+ thumb asm. Each loop
 * processes one colour channel of a buffer of 16-bit (or 8-bit) packed
 * pixels and emits a buffer of 10-bit TMDS symbols. The plain and
 * leftshift variants differ in whether the channel needs an extra
 * software shift before the interpolator can address the LUT (blue
 * channel of RGB565 needs it, others don't).
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

// pico.h must be included before any header that pulls in the board
// definitions, otherwise gnu-as sees the board's pico_board_cmake_set
// tokens as machine instructions and chokes.
#include "pico.h"
#include "hardware/regs/addressmap.h"
#include "hardware/regs/sio.h"
#include "frank_dvi_config.h"

// Offsets suitable for ldr/str (must be <= 0x7c):
#define ACCUM0_OFFS     (SIO_INTERP0_ACCUM0_OFFSET     - SIO_INTERP0_ACCUM0_OFFSET)
#define ACCUM1_OFFS     (SIO_INTERP0_ACCUM1_OFFSET     - SIO_INTERP0_ACCUM0_OFFSET)
#define ACCUM1_ADD_OFFS (SIO_INTERP0_ACCUM1_ADD_OFFSET - SIO_INTERP0_ACCUM0_OFFSET)
#define PEEK0_OFFS      (SIO_INTERP0_PEEK_LANE0_OFFSET - SIO_INTERP0_ACCUM0_OFFSET)
#define PEEK1_OFFS      (SIO_INTERP0_PEEK_LANE1_OFFSET - SIO_INTERP0_ACCUM0_OFFSET)
#define PEEK2_OFFS      (SIO_INTERP0_PEEK_FULL_OFFSET  - SIO_INTERP0_ACCUM0_OFFSET)
#define INTERP1         (SIO_INTERP1_ACCUM0_OFFSET     - SIO_INTERP0_ACCUM0_OFFSET)
// Note the entirety of INTERP0 and INTERP1 fits inside this 5-bit
// word-addressed space... almost as though it were intentional! :)

.syntax unified
.cpu cortex-m0plus
.thumb

.macro decl_func_x name
.section .scratch_x.\name, "ax"
.global \name
.type \name,%function
.thumb_func
\name:
.endm

.macro decl_func_y name
.section .scratch_y.\name, "ax"
.global \name
.type \name,%function
.thumb_func
\name:
.endm

#define decl_func decl_func_x

// ----------------------------------------------------------------------------
// Pixel-doubling encoders for RGB

// r0: Input buffer (word-aligned)
// r1: Output buffer (word-aligned)
// r2: Input size (pixels)

.macro do_channel_16bpp r_ibase r_inout0 r_out1
	str \r_inout0, [\r_ibase, #ACCUM0_OFFS]
	ldr \r_inout0, [\r_ibase, #PEEK0_OFFS]
	ldr \r_inout0, [\r_inout0]
	ldr \r_out1, [\r_ibase, #PEEK1_OFFS]
	ldr \r_out1, [\r_out1]
.endm

decl_func tmds_encode_loop_16bpp
	push {r4, r5, r6, r7, lr}
	lsls r2, #2
	add r2, r1
	mov ip, r2
	ldr r2, =(SIO_BASE + SIO_INTERP0_ACCUM0_OFFSET)
	b 2f
.align 2
1:
.rept TMDS_ENCODE_UNROLL
	ldmia r0!, {r4, r6}
	do_channel_16bpp r2, r4, r5
	do_channel_16bpp r2, r6, r7
	stmia r1!, {r4, r5, r6, r7}
.endr
2:
	cmp r1, ip
	bne 1b
	pop {r4, r5, r6, r7, pc}

// Same as above, but scale data to make up for lack of left shift
// in interpolator (costs 1 cycle per 2 pixels)
//
// r0: Input buffer (word-aligned)
// r1: Output buffer (word-aligned)
// r2: Input size (pixels)
// r3: Left shift amount

decl_func tmds_encode_loop_16bpp_leftshift
	push {r4, r5, r6, r7, lr}
	lsls r2, #2
	add r2, r1
	mov ip, r2
	ldr r2, =(SIO_BASE + SIO_INTERP0_ACCUM0_OFFSET)
	b 2f
.align 2
1:
.rept TMDS_ENCODE_UNROLL
	ldmia r0!, {r4, r6}
	lsls r4, r3
	do_channel_16bpp r2, r4, r5
	lsls r6, r3
	do_channel_16bpp r2, r6, r7
	stmia r1!, {r4, r5, r6, r7}
.endr
2:
	cmp r1, ip
	bne 1b
	pop {r4, r5, r6, r7, pc}

// r0: Input buffer (word-aligned)
// r1: Output buffer (word-aligned)
// r2: Input size (pixels)

decl_func tmds_encode_loop_8bpp
	push {r4, r5, r6, r7, lr}
	lsls r2, #2
	add r2, r1
	mov ip, r2
	ldr r2, =(SIO_BASE + SIO_INTERP0_ACCUM0_OFFSET)
	b 2f
.align 2
1:
.rept TMDS_ENCODE_UNROLL
	ldmia  r0!, {r4}
	str r4, [r2, #ACCUM0_OFFS + INTERP1]
	str r4, [r2, #ACCUM0_OFFS]
	ldr r4, [r2, #PEEK0_OFFS]
	ldr r4, [r4]
	ldr r5, [r2, #PEEK1_OFFS]
	ldr r5, [r5]
	ldr r6, [r2, #PEEK0_OFFS + INTERP1]
	ldr r6, [r6]
	ldr r7, [r2, #PEEK1_OFFS + INTERP1]
	ldr r7, [r7]
	stmia r1!, {r4, r5, r6, r7}
.endr
2:
	cmp r1, ip
	bne 1b
	pop {r4, r5, r6, r7, pc}

// r0: Input buffer (word-aligned)
// r1: Output buffer (word-aligned)
// r2: Input size (pixels)
// r3: Left shift amount
//
// Note that only the data written to interp0 (pixel 0, 1) is leftshifted, not
// the data written to interp1 (pixel 2, 3). Otherwise we always lose MSBs, as
// the LUT offset MSB is at bit 8, so pixel 0 always requires some left shift,
// since its channel MSBs are no greater than 7.

decl_func tmds_encode_loop_8bpp_leftshift
	push {r4, r5, r6, r7, lr}
	lsls r2, #3
	add r2, r1
	mov ip, r2
	ldr r2, =(SIO_BASE + SIO_INTERP0_ACCUM0_OFFSET)
	b 2f
.align 2
1:
.rept TMDS_ENCODE_UNROLL
	ldmia  r0!, {r4}
	str r4, [r2, #ACCUM0_OFFS + INTERP1]
	lsls r4, r3
	str r4, [r2, #ACCUM0_OFFS]
	ldr r4, [r2, #PEEK0_OFFS]
	ldr r4, [r4]
	ldr r5, [r2, #PEEK1_OFFS]
	ldr r5, [r5]
	ldr r6, [r2, #PEEK0_OFFS + INTERP1]
	ldr r6, [r6]
	ldr r7, [r2, #PEEK1_OFFS + INTERP1]
	ldr r7, [r7]
	stmia r1!, {r4, r5, r6, r7}
.endr
2:
	cmp r1, ip
	bne 1b
	pop {r4, r5, r6, r7, pc}


// ----------------------------------------------------------------------------
// Other bit depths (1bpp / 2bpp) and the full-resolution / palette
// encoders that the upstream library ships with are not present here:
// the frank-hdmi-sound scanline pipeline only ever calls the four
// loops above.  Drop additional loops in here if you want to extend
// the driver to other modes.
