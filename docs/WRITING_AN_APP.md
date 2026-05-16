# Writing an app with frank-hdmi-sound

A from-scratch tutorial that builds the bundled `hello_hdmi` example
one piece at a time, with explanations for every non-obvious line.

By the end you will have:

- An animated 320x240 test pattern on HDMI.
- A short audio cycle: 3 s of 440 Hz tone, 1 s of silence, then a
  multi-voice melody on loop forever.
- A USB CDC console with a per-second heartbeat.

The point isn't to copy `hello_hdmi/main.c` verbatim. The point is to
explain the design choices section by section so you can write your
own application against this driver. Read it alongside
[BUILDING.md](BUILDING.md) (toolchain) and [LLM_GUIDE.md](LLM_GUIDE.md)
(API reference and bug catalogue).

---

## Mental model first

Two cores, two responsibilities.

**Core 1** is the driver. It owns:

- The PIO0 state machines (one per TMDS data lane).
- The PWM slice that drives the pixel clock.
- The DMA chain that streams TMDS symbols and audio packets.
- `DMA_IRQ_1`.

It runs `frank_hdmi_run_core1()`, an infinite producer/consumer loop.
Producer fills a 320-pixel RGB565 scanline by looking up the source
framebuffer through the palette LUT. Consumer drives that scanline
through the libdvi TMDS encoder. The loop runs ~14 400 times a second
(480 active scanlines × 60 Hz, vertically doubled), so anything that
delays Core 1 by more than ~30 µs causes a missed scanline (the
infamous "red flash").

**Core 0** is your application. It owns:

- The framebuffer (8-bit palette indices, max 320x240).
- The palette LUT (256 entries of RGB888).
- Audio sample generation.

The two cores share state through two narrow APIs:

| Shared state         | Producer (Core 0)               | Consumer (Core 1)                  |
|----------------------|---------------------------------|------------------------------------|
| Framebuffer pixels   | `frank_hdmi_set_buffer()`, direct writes | scanline reads via palette LUT |
| Audio ring (~64 ms)  | `frank_hdmi_audio_write()`      | libdvi data-island packetiser      |

PIO programs, DMA channels, IRQ wiring: don't touch any of it.

---

## Step 1: project skeleton

Create a directory for your app and a minimal CMakeLists. The library
itself is added with `add_subdirectory()`.

```text
my_app/
├── CMakeLists.txt
├── pico_sdk_import.cmake          (copy from pico-sdk/external/)
├── third_party/
│   └── frank-hdmi-sound/          (vendored or submodule)
└── src/
    └── main.c
```

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD pico2 CACHE STRING "Pico board type")

include(pico_sdk_import.cmake)
project(my_app C CXX ASM)
pico_sdk_init()

add_subdirectory(third_party/frank-hdmi-sound)

add_executable(my_app src/main.c)
target_link_libraries(my_app
    frank_hdmi_sound
    pico_stdlib
    pico_multicore
)

# Optional but recommended: USB CDC console for debug prints.
pico_enable_stdio_uart(my_app 0)
pico_enable_stdio_usb(my_app 1)

pico_add_extra_outputs(my_app)
```

Configure and build:

```sh
mkdir build && cd build
cmake -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2 ..
cmake --build . -j
```

You should get `my_app.uf2` from an empty source file. Now we fill it.

---

## Step 2: framebuffer and palette

The library exposes the canvas dimensions as macros so you don't hard-
code 320 and 240. Define a static framebuffer:

```c
#include <stdint.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "frank_hdmi.h"

#define FB_W  FRANK_HDMI_LOGICAL_WIDTH    /* 320 */
#define FB_H  FRANK_HDMI_LOGICAL_HEIGHT   /* 240 */

static uint8_t framebuffer[FB_W * FB_H];
```

The framebuffer holds **palette indices**, one byte per pixel. Pick a
palette before bringing up the driver. Palette entry 0 is whatever you
want the background to be:

```c
enum {
    PAL_BG    = 0,
    PAL_GRID  = 1,
    PAL_FG    = 2,
    PAL_RED   = 3,
    PAL_GREEN = 4,
    PAL_BLUE  = 5,
};

static const uint32_t test_palette[6] = {
    [PAL_BG]    = 0x101820,   /* dark navy */
    [PAL_GRID]  = 0x303848,
    [PAL_FG]    = 0xF0F0F0,
    [PAL_RED]   = 0xE05050,
    [PAL_GREEN] = 0x60D060,
    [PAL_BLUE]  = 0x6080F0,
};
```

> Why not SMPTE colour bars? Most capture cards display vertical SMPTE
> bars when they have no HDMI signal. If your test pattern looks like
> SMPTE bars, you can't tell working from broken at a glance.

---

## Step 3: drawing a static pattern

Direct byte writes into `framebuffer[y * FB_W + x]`. Let's draw a 32-px
grid plus three coloured solid squares:

```c
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
            framebuffer[(40  + y) * FB_W + 40  + x] = PAL_RED;
            framebuffer[(100 + y) * FB_W + 100 + x] = PAL_GREEN;
            framebuffer[(160 + y) * FB_W + 200 + x] = PAL_BLUE;
        }
    }
}
```

Call this once at init, not every frame. The next step explains why.

---

## Step 4: animated sprite (do this carefully)

Core 1 reads the framebuffer live every scanline. If Core 0 rewrites
the framebuffer while Core 1 is mid-read, the output tears. Worse, a
per-frame full-screen redraw is a ~76 KB SRAM write storm that starves
Core 1's DMA bandwidth long enough to register on the HDMI side as red
"late scanline" flashes and audio glitches.

So: rewrite only the pixels that change.

```c
#define MARCHER_W  16
#define MARCHER_H  12
#define MARCHER_ROW (FB_H - 24)

/* Restore the static background under the marcher's previous box. */
static void erase_marcher_at(int col) {
    for (int y = 0; y < MARCHER_H; ++y) {
        for (int x = 0; x < MARCHER_W; ++x) {
            int xx = col + x;
            int yy = MARCHER_ROW + y;
            uint8_t p = ((xx % 32) == 0 || (yy % 32) == 0)
                      ? PAL_GRID : PAL_BG;
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
```

For larger animated scenes, allocate two framebuffers and call
`frank_hdmi_set_buffer(new_fb, w, h)` once you finish drawing into the
back buffer. The function is cheap and safe to call any time from
Core 0.

---

## Step 5: bring up the driver

The order matters:

```c
int main(void) {
    /* 1. Set sys_clock to a multiple of the TMDS bit clock.  With the
     *    default DVI_SM_CLKDIV=1, that means 252 MHz.  Wrong clock =
     *    no signal lock at all. */
    set_sys_clock_khz(252000, true);

    /* 2. USB stdio.  Optional, but very useful for debug. */
    stdio_init_all();

    /* 3. Palette: write whatever entries you need.  Safe to do this
     *    before *or* after frank_hdmi_init(); the driver does not
     *    zero the LUT. */
    for (int i = 0; i < 6; ++i) {
        frank_hdmi_set_palette((uint8_t)i, test_palette[i]);
    }

    /* 4. Draw your static pattern once. */
    draw_static_pattern();

    /* 5. Init the driver and hand it the framebuffer. */
    frank_hdmi_init();
    frank_hdmi_set_buffer(framebuffer, FB_W, FB_H);

    /* 6. Hand Core 1 over to the driver.  Never returns. */
    multicore_launch_core1(frank_hdmi_run_core1);

    /* 7. Your application loop runs on Core 0 from here. */
    while (1) {
        /* ...draw, push audio, sleep... */
    }
}
```

Flash it. You should see a stable HDMI signal with the static pattern.
No audio yet.

---

## Step 6: 440 Hz reference tone

Start with the simplest audio there is: a sine wave at a known
frequency. The target sample rate is `FRANK_HDMI_AUDIO_RATE`
(32 kHz). The driver expects packed `int16_t L, R, L, R, ...` in a
contiguous buffer.

Push one video frame's worth of audio per main-loop iteration. At
32 kHz / 60 Hz that's 533 stereo frames per chunk:

```c
#define AUDIO_RATE      FRANK_HDMI_AUDIO_RATE   /* 32000 */
#define FRAMES_PER_VID  (AUDIO_RATE / 60)       /* 533   */

static int16_t chunk_buf[FRAMES_PER_VID * 2];
```

### Sine LUT, not sinf()

Don't call `sinf()` per sample. The RP2350 has no hardware FP, so
`sinf()` is software. A naive `for (i=0..532) buf[i] = sinf(phase) *
gain` per video frame is slow enough that the audio push slips its
deadline and the receiver underruns, audible as clicks at high volume.

Pre-compute a 256-entry table at boot:

```c
#include <math.h>
#define SINE_LUT_LEN 256
static int16_t sine_lut[SINE_LUT_LEN];

static void build_sine_lut(void) {
    const float two_pi = 6.2831853f;
    for (int i = 0; i < SINE_LUT_LEN; ++i) {
        sine_lut[i] = (int16_t)(
            sinf(two_pi * (float)i / (float)SINE_LUT_LEN) * 8000.0f);
    }
}
```

We use 8000 instead of 32767 to leave headroom for mixing later.

### 32-bit fixed-point phase accumulator

Generate the tone with integer math: a 32-bit phase counter, the top
8 bits used as a LUT index. The wrap is automatic at 2^32.

```c
static void fill_tone(uint32_t *phase) {
    static const uint32_t TONE_STEP =
        (uint32_t)(((uint64_t)440 * (1ull << 32)) / AUDIO_RATE);
    uint32_t p = *phase;
    for (int i = 0; i < FRAMES_PER_VID; ++i) {
        int16_t s = sine_lut[p >> (32 - 8)];
        chunk_buf[i * 2 + 0] = s;   /* left  */
        chunk_buf[i * 2 + 1] = s;   /* right */
        p += TONE_STEP;
    }
    *phase = p;
}
```

Four instructions per sample, no FP, no clock-dependent latency.

---

## Step 7: producer rate must match the wire rate

This is the audio invariant most likely to bite you. Get it wrong and
the tone plays at a noticeably different pitch.

The driver advertises an audio rate to the HDMI receiver via the
info-frame and CTS/N values (32 kHz). The receiver is a pull consumer:
it drains samples at that declared rate, regardless of how fast the
producer feeds them in. Any mismatch shows up as pitch shift, and
capture cards in particular re-clock the stream in audible jumps.

### What goes wrong with `sleep_ms(17)`

Producing 533 samples every 17 ms gives 533 / 0.017 ≈ 31 353 Hz, ~2 %
low. Sounds wrong. Worse, `delayed_by_ms(prev, 17)` rounds each chunk
deadline to 17 000 µs, so the rounding error accumulates without bound.

### What works: anchor + microsecond deadlines

Pick an anchor time once at start, and compute every chunk's deadline
relative to it in microseconds:

```c
const uint64_t CHUNK_US = (uint64_t)FRAMES_PER_VID * 1000000ull / AUDIO_RATE;
absolute_time_t start = get_absolute_time();
uint64_t chunks_pushed = 0;

while (1) {
    fill_tone(&phase);
    frank_hdmi_audio_write(chunk_buf, FRAMES_PER_VID);
    ++chunks_pushed;
    sleep_until(delayed_by_us(start, chunks_pushed * CHUNK_US));
}
```

`CHUNK_US` is `533 * 1 000 000 / 32 000 = 16 656 µs`. The single anchor
keeps long-term cadence locked to the wall clock at integer-µs
precision; tone comes out at exactly 440 Hz, FFT-verified.

---

## Step 8: a state machine for tone → silence → melody

Now layer up: 3 seconds of tone, 1 second of silence, then the melody
on loop forever. Implement it as a phase counter.

```c
enum { PHASE_TONE, PHASE_SILENCE, PHASE_MELODY };

const uint64_t TONE_CHUNKS    = 3 * AUDIO_RATE / FRAMES_PER_VID;  /* ~180 */
const uint64_t SILENCE_CHUNKS = 1 * AUDIO_RATE / FRAMES_PER_VID;  /* ~60  */

int      phase_state  = PHASE_TONE;
uint64_t phase_chunks = 0;

while (1) {
    switch (phase_state) {
    case PHASE_TONE:
        fill_tone(&tone_phase);
        if (++phase_chunks >= TONE_CHUNKS) {
            phase_state  = PHASE_SILENCE;
            phase_chunks = 0;
        }
        break;
    case PHASE_SILENCE:
        memset(chunk_buf, 0, sizeof chunk_buf);
        if (++phase_chunks >= SILENCE_CHUNKS) {
            phase_state  = PHASE_MELODY;
            phase_chunks = 0;
            melody_init();
        }
        break;
    case PHASE_MELODY:
        fill_melody();
        ++phase_chunks;
        break;
    }
    frank_hdmi_audio_write(chunk_buf, FRAMES_PER_VID);
    sleep_until(delayed_by_us(start, ++chunks_pushed * CHUNK_US));
}
```

---

## Step 9: a multi-voice melody (optional)

If you just want a tone-and-silence test, skip this section.

The melody is a 4-bar synth riff (F#5 F#5 D5 B4 B4 E5 E5 G#5) with
bass on every quarter and kick/snare on alternating beats, in A-major
over an I-IV-V-I chord progression. ~169 BPM.

The point of doing this is to confirm the audio path can carry several
summed voices without clipping, hold a stereo image, and pass short
percussive transients (kick, snare) cleanly.

### Voices: oscillator + envelope

A "voice" is a sine oscillator and a peak amplitude that decays each
sample:

```c
typedef struct {
    uint32_t phase;
    uint32_t step;
    int16_t  amp;
} osc_t;

typedef struct {
    osc_t   osc;
    int16_t base_amp;
    uint8_t decay_shift;
} voice_t;

static inline void osc_set_freq_hz(osc_t *o, uint32_t hz) {
    o->step = (uint32_t)(((uint64_t)hz * (1ull << 32)) / AUDIO_RATE);
}

static inline int16_t osc_tick(osc_t *o) {
    int16_t s = sine_lut[o->phase >> (32 - 8)];
    o->phase += o->step;
    return (int16_t)(((int32_t)s * (int32_t)o->amp) >> 15);
}

static inline void voice_envelope_tick(voice_t *v) {
    if (v->osc.amp > 0) {
        int32_t dec = (int32_t)v->base_amp >> v->decay_shift;
        if (dec < 1) dec = 1;
        int32_t a = (int32_t)v->osc.amp - dec;
        v->osc.amp = (int16_t)(a < 0 ? 0 : a);
    }
}
```

A larger `decay_shift` decays slower (more sustain). Kick: `8`. Snare:
`7`. Bass: `11`. Lead: `12`.

### Pattern table

A pattern is an array of `step_t` (lead Hz, bass Hz, kick flag,
snare flag) at a sixteenth-note grid. 64 steps = 4 bars in 4/4. The
loop walks the pattern:

```c
typedef struct {
    uint16_t lead;
    uint16_t bass;
    uint8_t  kick;
    uint8_t  snare;
} step_t;

#define SIXTEENTH_SAMPLES 2840   /* 32000 * 60 / (169 * 4) ≈ 2841 */

static uint32_t step_pos;
static uint32_t step_index;

static void fill_melody(void) {
    for (int i = 0; i < FRAMES_PER_VID; ++i) {
        if (step_pos == 0) {
            const step_t *s = &pattern[step_index];
            if (s->lead) { osc_set_freq_hz(&v_lead.osc, s->lead);
                           v_lead.osc.amp = v_lead.base_amp; }
            if (s->bass) { osc_set_freq_hz(&v_bass.osc, s->bass);
                           v_bass.osc.amp = v_bass.base_amp; }
            if (s->kick) { osc_set_freq_hz(&v_kick.osc, 60);
                           v_kick.osc.amp = v_kick.base_amp; }
            if (s->snare){ osc_set_freq_hz(&v_snare.osc, 220);
                           v_snare.osc.amp = v_snare.base_amp; }
        }
        /* tick all voices, mix, clamp, write to chunk_buf... */
        if (++step_pos >= SIXTEENTH_SAMPLES) {
            step_pos = 0;
            step_index = (step_index + 1) % PATTERN_LEN;
        }
    }
}
```

### Mixing and clipping

Sum voices in `int32_t`, clamp to `int16_t`:

```c
static inline int16_t clamp16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

int32_t centre = s_bass + s_kick;                /* low energy = centre */
int32_t left   = centre + s_snare + (s_lead >> 1);
int32_t right  = centre + (s_snare >> 1) + s_lead;
chunk_buf[i * 2 + 0] = clamp16(left);
chunk_buf[i * 2 + 1] = clamp16(right);
```

Keep individual voices well under full scale (~10 000 base_amp, not
30 000) so the sum has headroom for mixing without persistent clipping.

### Snare needs noise

A pure sine snare sounds like a sine ping. Mix in pseudo-noise from a
cheap LFSR, scaled by the snare envelope so it disappears when the
drum decays:

```c
static uint32_t noise_state = 0xACE1u;
static inline int16_t noise_tick(void) {
    uint32_t bit = ((noise_state >> 0)  ^ (noise_state >> 1)
                  ^ (noise_state >> 21) ^ (noise_state >> 31)) & 1u;
    noise_state = (noise_state >> 1) | (bit << 31);
    return (int16_t)(noise_state & 0xffff) - 16384;
}
/* ... */
int32_t s_snare = osc_tick(&v_snare.osc)
              + ((int32_t)noise_tick() * (int32_t)v_snare.osc.amp >> 16);
```

---

## Step 10: video animation in the same loop

The marcher needs to march. Slot it into the same main-loop tick that
pushes audio, before the audio fill, so the framebuffer write happens
early in the chunk and Core 1 has the rest of the chunk to read it:

```c
int prev_col = 0;
uint32_t frame_no = 0;

while (1) {
    int col = (int)(frame_no % (FB_W - MARCHER_W));
    erase_marcher_at(prev_col);
    draw_marcher_at(col);
    prev_col = col;
    ++frame_no;

    /* audio fill + push + sleep_until() as before */
}
```

A 16x12 sprite update is small enough that it doesn't measurably
disturb Core 1's bandwidth.

---

## Step 11: heartbeat

Worth wiring up early; saves time the first time something silently
wedges. The driver exposes two counters. Print them once a second:

```c
extern volatile uint32_t frank_hdmi_heartbeat_lines;
extern volatile uint32_t frank_hdmi_heartbeat_frames;

uint32_t last_log_ms = 0;
while (1) {
    /* ...your loop... */
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (now_ms - last_log_ms >= 1000) {
        last_log_ms = now_ms;
        printf("[hb] core1_frames=%lu chunks=%llu\n",
               (unsigned long)frank_hdmi_heartbeat_frames,
               (unsigned long long)chunks_pushed);
    }
}
```

Frames should increment ~60/sec. If it stalls, Core 1 is stuck;
usually a misuse of PIO0 or DMA_IRQ_1 elsewhere in the application.

---

## Step 12: full picture

The complete `main()`:

```c
int main(void) {
    set_sys_clock_khz(252000, true);
    stdio_init_all();
    sleep_ms(1500);   /* let host CDC console attach */

    for (int i = 0; i < 6; ++i)
        frank_hdmi_set_palette((uint8_t)i, test_palette[i]);

    build_sine_lut();
    draw_static_pattern();
    melody_init();

    frank_hdmi_init();
    frank_hdmi_set_buffer(framebuffer, FB_W, FB_H);
    multicore_launch_core1(frank_hdmi_run_core1);

    enum { PHASE_TONE, PHASE_SILENCE, PHASE_MELODY };
    int      phase = PHASE_TONE;
    uint64_t phase_chunks = 0;
    uint32_t tone_phase = 0;
    uint32_t frame_no = 0;
    int      prev_col = 0;
    uint64_t chunks_pushed = 0;
    const uint64_t CHUNK_US = (uint64_t)FRAMES_PER_VID * 1000000ull / AUDIO_RATE;
    const uint64_t TONE_CHUNKS    = 3 * AUDIO_RATE / FRAMES_PER_VID;
    const uint64_t SILENCE_CHUNKS = 1 * AUDIO_RATE / FRAMES_PER_VID;
    absolute_time_t start = get_absolute_time();

    while (1) {
        int col = (int)(frame_no % (FB_W - MARCHER_W));
        erase_marcher_at(prev_col);
        draw_marcher_at(col);
        prev_col = col;
        ++frame_no;

        switch (phase) {
        case PHASE_TONE:
            fill_tone(&tone_phase);
            if (++phase_chunks >= TONE_CHUNKS) { phase = PHASE_SILENCE; phase_chunks = 0; }
            break;
        case PHASE_SILENCE:
            memset(chunk_buf, 0, sizeof chunk_buf);
            if (++phase_chunks >= SILENCE_CHUNKS) { phase = PHASE_MELODY; phase_chunks = 0; melody_init(); }
            break;
        case PHASE_MELODY:
            fill_melody();
            ++phase_chunks;
            break;
        }
        frank_hdmi_audio_write(chunk_buf, FRAMES_PER_VID);
        sleep_until(delayed_by_us(start, ++chunks_pushed * CHUNK_US));
    }
}
```

Build, flash, and the board now plays the same content as the bundled
`hello_hdmi`.

---

## Common adaptations

### Run the CPU at 504 MHz

If your application needs more headroom (emulation, decoding):

In your top-level CMake, before `add_subdirectory()`:

```cmake
set(FRANK_HDMI_SM_CLKDIV 2 CACHE STRING "" FORCE)
add_subdirectory(third_party/frank-hdmi-sound)
```

In `main()`:

```c
set_sys_clock_khz(504000, true);
```

The TMDS bit clock stays at 252 MHz because libdvi divides its
serialiser SMs and the PWM pixel clock by 2.

### Different pin layout

Either pick a board preset (`-DFRANK_HDMI_BOARD_M1=1` or
`-DFRANK_HDMI_BOARD_M2=1`) or set individual pins:

```cmake
target_compile_definitions(my_app PRIVATE
    FRANK_HDMI_PIN_CLK=10
    FRANK_HDMI_PIN_D0=12
    FRANK_HDMI_PIN_D1=14
    FRANK_HDMI_PIN_D2=16
)
```

If the board's differential pairs are wired with the positive leg on
the lower-numbered GPIO of each pair, also set
`FRANK_HDMI_INVERT_DIFFPAIRS=0`.

### Larger framebuffers

You can't, on the wire. The libdvi 16bpp encoder is pixel-doubling
and the canvas is fixed at 320x240. For smaller content (e.g. a
256x224 SNES frame), pass it to `frank_hdmi_set_buffer(fb, 256, 224)`
and the driver centres it with a black pillarbox/letterbox.

### Different audio rate

Don't. `FRANK_HDMI_AUDIO_RATE` is wired into the CTS/N values the
receiver uses to recover the audio clock; changing it without
adjusting the rest of the driver causes pitch shift.

### Skipping the driver's audio entirely (video-only)

Just don't call `frank_hdmi_audio_write()`. The data-island stream
will carry silence frames automatically.

---

## When something goes wrong

For a symptom-by-symptom catalogue (no signal, red flashes, tearing,
audio pitch wrong, clicks, gappy audio, etc.), see
[BUILDING.md > Bugs and findings](BUILDING.md#7-bugs-and-findings).

For the full driver API surface and a more terse cheat-sheet, see
[LLM_GUIDE.md](LLM_GUIDE.md).

---

## Hard rules (recap)

1. `set_sys_clock_khz` must be called before `frank_hdmi_init`.
2. Core 1, PIO0 (SMs 0/1/2), DMA_IRQ_1: the driver owns them.
3. Framebuffer is 8-bit palette indices, max 320x240.
4. Pace audio off a single wall-clock anchor with microsecond
   deadlines, not `sleep_ms()`.
5. Never call `sinf()` per sample. Use a LUT.
6. Don't redraw the full framebuffer every frame; touch only the
   pixels that change.
