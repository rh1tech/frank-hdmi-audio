/*
 * hello_hdmi — minimal example for frank-hdmi-sound.
 *
 * Draws SMPTE-style colour bars in the 320x240 logical canvas and
 * emits a 440 Hz sine wave on both audio channels.
 *
 * (c) 2026 Mikhail Matveev <xtreme@rh1.tech>, https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.  See LICENSE
 * at the root of this repository, or <https://www.gnu.org/licenses/>.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"

#include "frank_hdmi.h"

#define FB_W   FRANK_HDMI_LOGICAL_WIDTH
#define FB_H   FRANK_HDMI_LOGICAL_HEIGHT
static uint8_t framebuffer[FB_W * FB_H];

/*
 * Test pattern palette.  Deliberately avoids SMPTE colour bars —
 * those are the same image most capture cards display when there is
 * no HDMI signal, so a working driver would be indistinguishable
 * from a broken one in a screenshot.
 */
enum {
    PAL_BG    = 0,
    PAL_GRID  = 1,
    PAL_FG    = 2,
    PAL_RED   = 3,
    PAL_GREEN = 4,
    PAL_BLUE  = 5,
};

static const uint32_t test_palette[6] = {
    [PAL_BG]    = 0x101820,  /* dark navy background */
    [PAL_GRID]  = 0x303848,  /* faint grid lines */
    [PAL_FG]    = 0xF0F0F0,  /* near-white foreground */
    [PAL_RED]   = 0xE05050,
    [PAL_GREEN] = 0x60D060,
    [PAL_BLUE]  = 0x6080F0,
};

/*
 * Static elements: dark navy field, 32-pixel grid, three coloured
 * solid squares stacked diagonally.  An animated white "march" line
 * is added every frame in main_loop so motion is visible.
 */
static void draw_static_pattern(void) {
    for (int y = 0; y < FB_H; ++y) {
        for (int x = 0; x < FB_W; ++x) {
            uint8_t p = PAL_BG;
            if ((x % 32) == 0 || (y % 32) == 0) p = PAL_GRID;
            framebuffer[y * FB_W + x] = p;
        }
    }
    /* Three solid 40x40 squares so the colour channels can be
     * verified at a glance. */
    for (int y = 0; y < 40; ++y) {
        for (int x = 0; x < 40; ++x) {
            framebuffer[(40 + y) * FB_W + 40 + x]   = PAL_RED;
            framebuffer[(100 + y) * FB_W + 100 + x] = PAL_GREEN;
            framebuffer[(160 + y) * FB_W + 200 + x] = PAL_BLUE;
        }
    }
}

/*
 * Erase the previous marcher position with the static-background
 * colour, then draw the new one.  Avoids redrawing the whole frame
 * every iteration — that caused the static squares to flicker (Core
 * 1 catches the framebuffer mid-redraw) and audio glitches (Core 0
 * SRAM bursts starved the TMDS DMA of bandwidth).
 */
#define MARCHER_W 16
#define MARCHER_H 12
#define MARCHER_ROW (FB_H - 24)

static void erase_marcher_at(int col) {
    for (int y = 0; y < MARCHER_H; ++y) {
        for (int x = 0; x < MARCHER_W; ++x) {
            int xx = col + x;
            int yy = MARCHER_ROW + y;
            uint8_t p = ((xx % 32) == 0 || (yy % 32) == 0) ? PAL_GRID : PAL_BG;
            framebuffer[yy * FB_W + xx] = p;
        }
    }
}

static void draw_marcher_at(int col) {
    for (int y = 0; y < MARCHER_H; ++y) {
        for (int x = 0; x < MARCHER_W; ++x) {
            framebuffer[(MARCHER_ROW + y) * FB_W + col + x] = PAL_FG;
        }
    }
}

/* ================================================================== */
/*  Audio synth                                                        */
/* ================================================================== */

#define AUDIO_RATE      FRANK_HDMI_AUDIO_RATE          /* 32000 */
#define FRAMES_PER_VID  (AUDIO_RATE / 60)              /* 533    */
#define SINE_LUT_LEN    256

static int16_t sine_lut[SINE_LUT_LEN];
static int16_t chunk_buf[FRAMES_PER_VID * 2];

static void build_sine_lut(void) {
    const float two_pi = 6.2831853f;
    for (int i = 0; i < SINE_LUT_LEN; ++i) {
        sine_lut[i] = (int16_t)(sinf(two_pi * (float)i / (float)SINE_LUT_LEN) * 8000.0f);
    }
}

/*
 * Helper: convert frequency in Hz (×1000 fixed) to a 32-bit phase
 * step.  PHASE_STEP = freq * 2^32 / AUDIO_RATE; for an integer
 * frequency that's 2^32 / AUDIO_RATE * freq, kept in 64-bit math.
 */
static inline uint32_t phase_step_hz_q10(uint32_t hz_q10) {
    /* hz_q10 is freq in millihertz × 0.001, i.e. plain Hz scaled by
     * 1024 so we can carry quarter-tones without floats.  Final step
     * = (hz_q10 * 2^32) / (AUDIO_RATE * 1024). */
    return (uint32_t)(((uint64_t)hz_q10 * (1ull << 32)) / ((uint64_t)AUDIO_RATE * 1024));
}

/* Single oscillator state. */
typedef struct {
    uint32_t phase;
    uint32_t step;
    int16_t  amp;       /* peak amplitude, int16 scale */
} osc_t;

static inline void osc_set_freq_hz(osc_t *o, uint32_t hz) {
    o->step = (uint32_t)(((uint64_t)hz * (1ull << 32)) / AUDIO_RATE);
}

static inline int16_t osc_tick(osc_t *o) {
    int16_t s = sine_lut[o->phase >> (32 - 8)];
    o->phase += o->step;
    /* amp is 0..32767; multiply then divide by 32768 to scale. */
    return (int16_t)(((int32_t)s * (int32_t)o->amp) >> 15);
}

/* Hard-clamp the int32 sum to int16 range. */
static inline int16_t clamp16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* ----------------- pure 440 Hz tone (intro) ---------------------- */

static void fill_tone(uint32_t *phase) {
    static const uint32_t TONE_STEP = (uint32_t)(((uint64_t)440 * (1ull << 32)) / AUDIO_RATE);
    uint32_t p = *phase;
    for (int i = 0; i < FRAMES_PER_VID; ++i) {
        int16_t s = sine_lut[p >> (32 - 8)];
        chunk_buf[i * 2 + 0] = s;
        chunk_buf[i * 2 + 1] = s;
        p += TONE_STEP;
    }
    *phase = p;
}

/* ----------------- silence (gap between tone and theme) ---------- */

static void fill_silence(void) {
    memset(chunk_buf, 0, sizeof chunk_buf);
}

/* ================================================================== */
/*  Terminator theme                                                   */
/* ================================================================== */

/*
 * The Terminator main-title theme by Brad Fiedel.  The famous hook
 * is a 13-sixteenth-note ostinato in C# minor (often notated as 13/8
 * or felt as 5/4 with a triplet pull).  Accents on beats 1, 4, 8 and
 * 11 give the iconic mechanical "DUN ... dun-dun-dun ... DUN ..." feel.
 *
 * Voicing:
 *   - bass:   E1/E2 octave drone, retriggered on every beat
 *   - hit:    low-octave anvil/clang on the four accents (E2)
 *   - lead:   high-octave bell line moving E5 / G5 / B5
 *   - drone: sustained C# minor power-fifth E2+B2 in the background
 *
 * The signature pattern (one loop = 13 sixteenths):
 *
 *      1  2  3   4  5  6  7   8  9 10  11 12 13
 *      *  .  .   *  .  .  .   *  .  .   *  .  .
 *
 * where '*' = accented hit, '.' = soft tick.  The lead part also
 * pulses on the accented beats, hopping E5 → G5 → E5 → B5.
 *
 * Tempo ≈ 130 BPM in 4/4, so a sixteenth ≈ 115 ms.  We pick
 * SIXTEENTH_SAMPLES = 3700 (115.6 ms at 32 kHz) which keeps the loop
 * length cleanly within an integer number of audio chunks.
 */

/* Note frequencies in Hz, C# minor / E natural-minor neighbours. */
#define HZ_E1    41
#define HZ_E2    82
#define HZ_B2   123
#define HZ_E3   165
#define HZ_E4   330
#define HZ_E5   659
#define HZ_G5   784
#define HZ_B5   988

typedef struct {
    uint16_t bass;     /* sub-bass drone (always low E) */
    uint16_t hit;      /* anvil/accent voice */
    uint16_t lead;     /* high bell line */
    uint16_t drone;    /* sustained power-fifth */
} step_t;

/*
 * 13-sixteenth-note loop with accents on positions 0, 3, 7 and 10
 * (zero-based).  This is the "DUN ... dun ... dun-dun ... DUN ..."
 * pulse the film opens with.
 */
static const step_t pattern[13] = {
    /*  0 */ { HZ_E1, HZ_E2, HZ_E5, HZ_B2 },   /* accent */
    /*  1 */ { 0,     0,     0,     0     },
    /*  2 */ { 0,     0,     0,     0     },
    /*  3 */ { HZ_E1, HZ_E2, HZ_G5, 0     },   /* accent */
    /*  4 */ { 0,     0,     0,     0     },
    /*  5 */ { 0,     0,     0,     0     },
    /*  6 */ { 0,     0,     0,     0     },
    /*  7 */ { HZ_E1, HZ_E2, HZ_E5, HZ_B2 },   /* accent */
    /*  8 */ { 0,     0,     0,     0     },
    /*  9 */ { 0,     0,     0,     0     },
    /* 10 */ { HZ_E1, HZ_E2, HZ_B5, 0     },   /* accent */
    /* 11 */ { 0,     0,     0,     0     },
    /* 12 */ { 0,     0,     0,     0     },
};

#define PATTERN_LEN         13
#define SIXTEENTH_SAMPLES   3700
#define LOOP_SAMPLES        (PATTERN_LEN * SIXTEENTH_SAMPLES)

/*
 * Each voice: oscillator + simple AD envelope.  decay_shift is
 * applied per sample as `amp -= base_amp >> decay_shift`, so larger
 * shift = slower decay (more sustain).
 */
typedef struct {
    osc_t    osc;
    int16_t  base_amp;
    uint8_t  decay_shift;
} voice_t;

static voice_t v_bass;     /* slow decay so the sub-bass sustains under hits */
static voice_t v_hit;      /* fast decay — percussive anvil */
static voice_t v_lead;     /* medium decay — bell ring */
static voice_t v_drone;    /* very slow — almost a pad */

static uint32_t theme_step_pos;
static uint32_t theme_step_index;

static void theme_init(void) {
    memset(&v_bass,  0, sizeof v_bass);
    memset(&v_hit,   0, sizeof v_hit);
    memset(&v_lead,  0, sizeof v_lead);
    memset(&v_drone, 0, sizeof v_drone);

    v_bass.base_amp    = 14000;  v_bass.decay_shift  = 14; /* very slow */
    v_hit.base_amp     = 12000;  v_hit.decay_shift   =  9; /* punchy */
    v_lead.base_amp    =  9000;  v_lead.decay_shift  = 11; /* bell */
    v_drone.base_amp   =  5000;  v_drone.decay_shift = 13; /* slow pad */

    theme_step_pos = 0;
    theme_step_index = 0;
}

static void theme_retrigger(voice_t *v, uint16_t hz) {
    if (hz == 0) {
        /* Don't kill an already-ringing voice on a "rest" step —
         * just let its envelope decay naturally. */
        return;
    }
    osc_set_freq_hz(&v->osc, hz);
    v->osc.amp = v->base_amp;
}

static inline void voice_envelope_tick(voice_t *v) {
    if (v->osc.amp > 0) {
        int32_t dec = (int32_t)v->base_amp >> v->decay_shift;
        if (dec < 1) dec = 1;
        int32_t a = (int32_t)v->osc.amp - dec;
        v->osc.amp = (int16_t)(a < 0 ? 0 : a);
    }
}

/* Generate one chunk (FRAMES_PER_VID samples) of the theme. */
static void fill_theme(void) {
    for (int i = 0; i < FRAMES_PER_VID; ++i) {
        if (theme_step_pos == 0) {
            const step_t *s = &pattern[theme_step_index];
            theme_retrigger(&v_bass,  s->bass);
            theme_retrigger(&v_hit,   s->hit);
            theme_retrigger(&v_lead,  s->lead);
            theme_retrigger(&v_drone, s->drone);
        }

        int32_t s_bass  = osc_tick(&v_bass.osc);
        int32_t s_hit   = osc_tick(&v_hit.osc);
        int32_t s_lead  = osc_tick(&v_lead.osc);
        int32_t s_drone = osc_tick(&v_drone.osc);

        voice_envelope_tick(&v_bass);
        voice_envelope_tick(&v_hit);
        voice_envelope_tick(&v_lead);
        voice_envelope_tick(&v_drone);

        /* Stereo image: bass and drone centred (mono); the hit lands
         * slightly left-of-centre, the bell-lead slightly right. */
        int32_t centre = s_bass + s_drone;
        int32_t left   = centre + s_hit + (s_lead >> 1);
        int32_t right  = centre + (s_hit >> 1) + s_lead;

        chunk_buf[i * 2 + 0] = clamp16(left);
        chunk_buf[i * 2 + 1] = clamp16(right);

        if (++theme_step_pos >= SIXTEENTH_SAMPLES) {
            theme_step_pos = 0;
            theme_step_index = (theme_step_index + 1) % PATTERN_LEN;
        }
    }
}

int main(void) {
    /*
     * Pick the system clock.  libdvi is configured here with
     * DVI_SM_CLKDIV=1 so sys_clock = TMDS bit clock = 252 MHz.  No
     * core-voltage bump and no QMI flash retiming needed at this
     * speed — keeps the example minimal.
     *
     * If you want the CPU at 504 MHz instead (e.g. to leave room
     * for an emulator on Core 0), override DVI_SM_CLKDIV=2 in your
     * top-level CMake, bump the voltage with vreg_set_voltage, and
     * call set_flash_timings before set_sys_clock_khz.  See
     * frank-snes for a full example.
     */
    set_sys_clock_khz(252000, true);

    stdio_init_all();
    /*
     * Optional pause before printing the first boot line, so a host
     * CDC terminal has time to attach and capture the log.  0 by
     * default (start instantly when running standalone); pass
     * -DHELLO_HDMI_BOOT_DELAY_MS=5000 to give a host five seconds to
     * connect before any log appears.
     */
#ifndef HELLO_HDMI_BOOT_DELAY_MS
#define HELLO_HDMI_BOOT_DELAY_MS 0
#endif
#if HELLO_HDMI_BOOT_DELAY_MS > 0
    sleep_ms(HELLO_HDMI_BOOT_DELAY_MS);
#endif

    printf("\n[hello_hdmi] boot, sys_clk=%lu kHz, audio=%d Hz\n",
           (unsigned long)(clock_get_hz(clk_sys) / 1000),
           AUDIO_RATE);

    /* Load the test palette. */
    for (int i = 0; i < (int)(sizeof test_palette / sizeof test_palette[0]); ++i) {
        frank_hdmi_set_palette((uint8_t)i, test_palette[i]);
    }

    build_sine_lut();
    draw_static_pattern();

    printf("[hello_hdmi] frank_hdmi_init\n");
    frank_hdmi_init();
    frank_hdmi_set_buffer(framebuffer, FB_W, FB_H);
    printf("[hello_hdmi] launching Core 1\n");
    multicore_launch_core1(frank_hdmi_run_core1);
    printf("[hello_hdmi] Core 1 launched, entering audio loop\n");

    /*
     * Each iteration: redraw the marching block over the static
     * pattern, push one video frame's worth of audio, and sleep
     * until the next ~16.67 ms tick.
     */
    /*
     * Producer loop.  Each chunk is FRAMES_PER_VID samples.  We pace
     * off a single wall-clock anchor so the long-term sample rate is
     * exactly AUDIO_RATE and the receiver's CTS/N reconstruction
     * matches the producer to integer-µs precision.  Per-chunk
     * `delayed_by_ms(prev, 17)` accumulates ~0.6 ms drift per second
     * and shows up as audible pitch shift.
     *
     * Playback state machine:
     *   PHASE_TONE     — 3 s of pure 440 Hz tone
     *   PHASE_SILENCE  — 1 s of silence
     *   PHASE_THEME    — Terminator main-title hook on loop
     *
     * Number of chunks per phase = duration_secs * AUDIO_RATE /
     * FRAMES_PER_VID.  At 32 kHz / 533 = 60.04 chunks/sec, so 3 s
     * ≈ 180 chunks, 1 s ≈ 60 chunks.
     */
    enum { PHASE_TONE, PHASE_SILENCE, PHASE_THEME };
    int    phase_state    = PHASE_TONE;
    uint64_t phase_chunks = 0;
    const uint64_t TONE_CHUNKS    = 3 * AUDIO_RATE / FRAMES_PER_VID;
    const uint64_t SILENCE_CHUNKS = 1 * AUDIO_RATE / FRAMES_PER_VID;

    const uint64_t CHUNK_US = (uint64_t)FRAMES_PER_VID * 1000000ull / AUDIO_RATE;
    uint32_t tone_phase = 0;
    uint32_t frame_no = 0;
    uint32_t last_log_ms = 0;
    int prev_col = 0;
    uint64_t chunks_pushed = 0;
    absolute_time_t start = get_absolute_time();

    theme_init();

    while (1) {
        /* Video: only the marcher's bounding box is rewritten per
         * frame, so Core 1 never catches the rest of the buffer
         * mid-redraw. */
        int col = (int)(frame_no % (FB_W - MARCHER_W));
        erase_marcher_at(prev_col);
        draw_marcher_at(col);
        prev_col = col;
        ++frame_no;

        /* Audio: phase machine. */
        switch (phase_state) {
        case PHASE_TONE:
            fill_tone(&tone_phase);
            if (++phase_chunks >= TONE_CHUNKS) {
                phase_state = PHASE_SILENCE;
                phase_chunks = 0;
            }
            break;
        case PHASE_SILENCE:
            fill_silence();
            if (++phase_chunks >= SILENCE_CHUNKS) {
                phase_state = PHASE_THEME;
                phase_chunks = 0;
                theme_init();   /* restart the theme cleanly */
            }
            break;
        case PHASE_THEME:
            fill_theme();
            ++phase_chunks;
            break;
        }
        frank_hdmi_audio_write(chunk_buf, FRAMES_PER_VID);
        ++chunks_pushed;

        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (now_ms - last_log_ms >= 1000) {
            last_log_ms = now_ms;
            const char *p = phase_state == PHASE_TONE    ? "tone"
                          : phase_state == PHASE_SILENCE ? "silence"
                          :                                "theme";
            printf("[hb] phase=%s core1_frames=%lu cpu_loop=%lu chunks=%llu\n",
                   p,
                   (unsigned long)frank_hdmi_heartbeat_frames,
                   (unsigned long)frame_no,
                   (unsigned long long)chunks_pushed);
        }

        sleep_until(delayed_by_us(start, chunks_pushed * CHUNK_US));
    }
}
