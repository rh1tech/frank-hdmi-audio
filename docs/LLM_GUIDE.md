# LLM_GUIDE.md

A guide for AI agents adding `frank-hdmi-sound` to an existing Pico
SDK project, or modifying the driver itself.

The goal is to keep you out of the rabbit holes I went down while
building this. If you find yourself debugging "no HDMI signal" or
"audio sounds wrong", read the matching section first.

## What this driver does (one paragraph)

A 640x480p60 HDMI signal, generated entirely from RP2350 PIO and
PWM, plus 32 kHz stereo PCM packed into HDMI data-island packets.
The TMDS engine is libdvi (vendored). Core 1 owns the engine and
runs a producer/consumer loop that reads an 8-bit palette-indexed
framebuffer, converts it to RGB565 line by line, and feeds the
libdvi encoder. Core 0 stays free for application work and pushes
audio samples whenever it has them.

## How to integrate

```cmake
add_subdirectory(third_party/frank-hdmi-sound)
target_link_libraries(your_app frank_hdmi_sound pico_stdlib pico_multicore)
```

```c
set_sys_clock_khz(252000, true);     /* with DVI_SM_CLKDIV=1 */
frank_hdmi_set_palette(...);          /* before init */
frank_hdmi_init();
frank_hdmi_set_buffer(fb, w, h);
multicore_launch_core1(frank_hdmi_run_core1);
/* push audio with frank_hdmi_audio_write() at the wall-clock rate
   that matches FRANK_HDMI_AUDIO_RATE (32 kHz by default) */
```

## Hard rules

1. **`set_sys_clock_khz` must be called before `frank_hdmi_init`** and
   the value must satisfy `sys_clock = TMDS_bit_clock × DVI_SM_CLKDIV`.
   For 640x480p60 the TMDS bit clock is 252 MHz. Default in this repo
   is `DVI_SM_CLKDIV=1` so sys_clock must be 252 MHz; for an
   application that needs 504 MHz CPU headroom, override to
   `DVI_SM_CLKDIV=2` and call `set_sys_clock_khz(504000, true)`.
   Wrong clock = no signal lock at all.

2. **Core 1 belongs to the driver.** `frank_hdmi_run_core1` never
   returns. Use the second core for HDMI, or use a different driver.

3. **PIO0 belongs to the driver.** State machines 0/1/2 carry the
   three TMDS data lanes. Application audio peripherals etc. should
   go on PIO1 or PIO2.

4. **DMA_IRQ_1 belongs to the driver.** It paces the TMDS DMA chain.

5. **Frame buffer is 8-bit palette indices, max 320x240.** The libdvi
   encoder is pixel-doubling on the wire, so a 640-wide source buffer
   would have only its left half displayed (and that pixel-doubled to
   1280, far past the 640-wide canvas). Use a 320-wide buffer.

6. **Set the palette before calling `frank_hdmi_init`** (or any time
   after — both work). The driver does NOT zero the palette LUT in
   `frank_hdmi_init`; the static arrays are zero-initialised at boot,
   and any pre-init writes are preserved. (An earlier version
   helpfully zeroed them in init and silently broke this contract.)

## Audio API

`frank_hdmi_audio_write(samples_lr, num_frames)`:

- `samples_lr` is interleaved `int16_t L, R, L, R, ...`.
- `num_frames` is stereo frames (so the array is `2 * num_frames`
  shorts long).
- Returns the number of frames actually written. The rest are
  dropped silently; the driver never blocks the producer.
- The audio ring is 2048 frames (~64 ms at 32 kHz).

### Producer rate must match the wire rate

This is the single most important audio invariant.

The driver advertises a fixed rate to the HDMI receiver via the audio
info-frame and CTS/N values (32 kHz by default). The receiver is a
**pull** consumer: it drains samples at exactly that declared rate
and is not flow-controlled by the producer.

If your producer's long-term sample rate is different from the
declared rate, the receiver hears the difference as **pitch shift**,
and capture cards in particular re-clock the stream in audible
jumps. Concretely:

- Declared rate: 32 000 Hz.
- If you produce 533 samples every 17 ms (using `sleep_ms(17)` or
  `delayed_by_ms(prev, 17)`), your real rate is 533 / 0.017 ≈
  31 353 Hz, ~2 % low. That sounds wrong.
- Even worse, if the wall-clock pacing accumulates rounding error
  per chunk (as it does with `delayed_by_ms` of an integer ms), the
  drift grows without bound.

**Solution**: pace from a single anchor time and a microsecond
deadline:

```c
const uint64_t CHUNK_US = (uint64_t)FRAMES_PER_VID * 1000000ull / AUDIO_RATE;
absolute_time_t start = get_absolute_time();
uint64_t chunks = 0;
while (1) {
    fill_chunk(...);
    frank_hdmi_audio_write(buf, FRAMES_PER_VID);
    ++chunks;
    sleep_until(delayed_by_us(start, chunks * CHUNK_US));
}
```

This locks the long-term cadence to the wall clock at integer-µs
precision. Tested: produces a perfect 440 Hz tone with no glitches.

### Do not call `sinf()` per sample

`sinf()` is software floating point on the RP2350 (no hardware FP).
A naive `for (i=0..532) buf[i] = sinf(phase) * gain` per video frame
is slow enough that the audio push slips its deadline, the producer
falls behind, the receiver underruns, and the audio glitches at the
cadence of whatever else Core 0 is doing (even something as light as
a 16x12-pixel sprite update).

Use a sine LUT and a 32-bit fixed-point phase accumulator instead:

```c
static int16_t sine_lut[256];
static const uint32_t PHASE_STEP =
    (uint32_t)(((uint64_t)TONE_HZ * (1ull << 32)) / AUDIO_RATE);
/* per sample: */
int16_t s = sine_lut[phase >> 24];
phase += PHASE_STEP;
```

That's 4 instructions per sample, no FP, no clock-dependent latency.

### Stereo / multi-channel mixing

Each frame is `int16_t L, R`. To mix several voices, sum them in a
wider type and clamp:

```c
int32_t left  = voice_a.l + voice_b.l + voice_c.l;
int32_t right = voice_a.r + voice_b.r + voice_c.r;
if (left  >  32767) left  =  32767;
if (left  < -32768) left  = -32768;
if (right >  32767) right =  32767;
if (right < -32768) right = -32768;
buf[i*2 + 0] = (int16_t)left;
buf[i*2 + 1] = (int16_t)right;
```

Keep individual voices well below full scale (e.g. 8000-10000) so
the sum has headroom.

## Video tearing avoidance

Core 1 reads the framebuffer **live** every scanline. If Core 0
rewrites the same memory while Core 1 is reading it, the capture
shows torn / flickering pixels.

For a static scene with a small animated sprite, draw the static
parts once at init and only rewrite the sprite's bounding box per
frame. The example does this for the marcher block:

```c
erase_marcher_at(prev_col);   /* restore static background */
draw_marcher_at(col);
prev_col = col;
```

Calling `draw_static_pattern()` every frame caused both visible
square-flicker and audio glitches (the per-frame ~76 KB SRAM write
storm starved Core 1's DMA bandwidth long enough to register on the
HDMI side).

For larger animated scenes you'd want a real double-buffer plus an
atomic pointer swap; this driver doesn't ship one but
`frank_hdmi_set_buffer(new_fb, w, h)` is safe to call at any time
from Core 0 and is cheap.

## Capture-card sanity check

Capture cards display **vertical SMPTE-style colour bars** when
they have no HDMI signal. If your test pattern looks like that,
you can't tell working from broken. Use a pattern that's obviously
not vertical bars (the example uses a navy field with a 32-pixel
grid plus three solid R/G/B squares plus a marching white block).

## Things that look like bugs but are not

### "I see colour bars on the capture card."

That's the capture card's "no signal" indicator, not output from the
driver. Real driver output never looks like vertical SMPTE bars
unless you explicitly draw them.

### "Build fails with `Out of memory` panic at boot."

Earlier libdvi versions `malloc()`'d the TMDS buffers from the heap
(default Pico heap = 2 KB; TMDS buffers ≈ 12 KB). The driver here
uses a static buffer pool. If you replace `src/frank_dvi.c` with a
fresh upstream `dvi.c`, re-apply that patch (search for
`static_tmds_pool`).

### "Image is shifted right by half the screen."

You are feeding the encoder a 640-wide framebuffer. It only reads
the first 320 pixels per line and pixel-doubles them on the wire.
Use a 320-wide buffer (FRANK_HDMI_LOGICAL_WIDTH).

### "Image looks all black even though Core 1 heartbeat increments."

The palette LUT is zeroed. Either you forgot to call
`frank_hdmi_set_palette` for any indices, or you wrote a recent
libdvi commit that added a `memset(palette, 0)` to `frank_hdmi_init`.
The driver here is structured so palette writes before init are
preserved.

### "Red horizontal lines flash on the screen under load."

libdvi's `late_scanline_ctr` indicator. Core 1 is not keeping up
with the TMDS encoder because Core 0 traffic on the AHB fabric or
SRAM banks is starving it. Fixes already in this driver:

- `bus_ctrl.priority` bits set for PROC1, DMA_R, DMA_W.
- TMDS DMA channels marked HIGH_PRIORITY (libdvi patch).
- Palette LUT in `scratch_y`.
- `fill_scanline` in `scratch_y`.
- Pillarbox columns pre-zeroed once at init (no per-line memset).
- `DVI_N_TMDS_BUFFERS=3` for IRQ slack.

If you still see red lines, the most likely cause is a long XIP /
PSRAM stall on Core 0. Move the offending code into RAM with
`__not_in_flash_func()`. The next most likely cause is a per-frame
full-screen redraw — see "Video tearing avoidance" above.

### "No HDMI signal, but Core 1 heartbeat shows ~60 fps."

The encode loop is alive but the receiver isn't locking. Most likely
causes, in order of frequency:

1. **`invert_diffpairs` wrong for your board.** Boards that put the
   negative leg on the lower-numbered GPIO of each pair (most) need
   `invert_diffpairs=true` (the default in this driver). Boards
   wired the other way need `false`. Override with
   `-DFRANK_HDMI_INVERT_DIFFPAIRS=0`.

2. **System clock wrong for current `DVI_SM_CLKDIV`.** With CLKDIV=1,
   sys_clock must be 252 MHz. With CLKDIV=2, sys_clock must be 504
   MHz. Verify with `clock_get_hz(clk_sys)` printed over USB.

3. **Pin numbers wrong.** Check the runtime GPIO funcsel for your
   pins (4=PWM, 6=PIO0 on RP2350) — see the diagnostic snippet
   below.

### "No audio."

In rough order of likelihood:

1. The producer never calls `frank_hdmi_audio_write`. Verify
   `frank_hdmi_init` was called first.
2. The HDMI sink is muted. On macOS the capture card audio is a
   separate USB device — check **System Settings → Sound**.
3. The producer's intermediate ring (between mixer and HDMI) is
   not being drained. The HDMI driver only drains its own ring.

### "Audio plays but sounds broken: clicks or glitches mid-waveform."

You are passing `full=false` to libdvi's `get_write_size`. Upstream
has a sign-error in that branch and over-reports free space. The
driver here always uses `full=true`. Do not switch back.

### "Audio plays at the wrong pitch (~13 % high or low)."

Producer rate doesn't match the declared wire rate. See "Producer
rate must match the wire rate" above. The most common cause is
pacing with `sleep_ms(17)` instead of microsecond-precise wall-clock
deadlines.

### "Audio has clicks audible only at high volume."

Same root cause as the previous one: small but persistent producer
rate mismatch + receiver re-clocking. The fix is the same — pace off
a single anchor with `delayed_by_us(start, chunks * CHUNK_US)`.

### "Audio sounds gappy / ratchety."

`HDMI_AUDIO_RING_FRAMES` is too small. The producer is bursty (one
chunk per video frame); if your ring is smaller than one chunk you
drop a fraction of every burst. The driver here uses 2048 frames
(~64 ms at 32 kHz) which fits typical ~533-frame bursts with
plenty of margin.

## Diagnostic snippets

### GPIO funcsel dump

Confirms the PIO and PWM took the pins:

```c
#include "hardware/structs/iobank0.h"
for (int p = 12; p <= 19; ++p) {
    uint32_t fn = (iobank0_hw->io[p].ctrl
                   & IO_BANK0_GPIO0_CTRL_FUNCSEL_BITS)
                  >> IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
    printf("gpio%d funcsel=%lu\n", p, (unsigned long)fn);
}
```

Expected on the M2 layout: `gpio12,13 funcsel=4` (PWM),
`gpio14..19 funcsel=6` (PIO0).

### Core 1 heartbeat

Already exposed by the driver:

```c
extern volatile uint32_t frank_hdmi_heartbeat_lines;
extern volatile uint32_t frank_hdmi_heartbeat_frames;
```

Frames should increment ~60/sec, lines ~14400/sec.

### Audio capture analysis

```bash
# Capture 5s from the macOS audio-input "USB Digital Audio" capture
# device that the HDMI capture card exposes:
ffmpeg -y -f avfoundation -i ":USB Digital Audio" \
       -t 5 -ac 2 -ar 48000 /tmp/aud.wav

# FFT to find the fundamental:
python3 -c "
import wave, numpy as np
w = wave.open('/tmp/aud.wav','rb')
fr = w.getframerate()
a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
L = a[0::w.getnchannels()].astype(np.float32)
seg = L[len(L)//2 : len(L)//2 + fr]
spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
freqs = np.fft.rfftfreq(len(seg), 1/fr)
i = np.argmax(spec)
print(f'fundamental: {freqs[i]:.1f} Hz')
"
```

For a 440 Hz reference tone this should print ~440 Hz to within
about 1 Hz. Anything more than ±5 Hz is a producer-rate problem.

## Things that are still bugs

If you see any of these, file an issue:

- HDMI loses lock after running for hours (slow clock drift).
- Audio drops out periodically with clean silence (not noise) — could
  indicate an overflow that the `full=true` fix didn't catch.
- Compile fails with a fresh Pico SDK release. The `pico.h`
  prerequisite shim in `tmds_encode.S` may need updating.

## Debugging history (chronological)

The driver went through these steps in order. Each one was needed
to fix a real symptom.

1. **Build green at -DHDMI_DRIVER=ALT.** Required vendoring libdvi
   under `src/libdvi/` and patching `tmds_encode.S` to include
   `pico.h` first (otherwise the assembler chokes on board-header
   `pico_board_cmake_set` tokens).

2. **Replace libdvi's `malloc()` for TMDS buffers with a static pool.**
   Default Pico heap is too small.

3. **Get HDMI signal lock.** Required priming `q_tmds_valid` before
   `dvi_start`, switching to `DMA_IRQ_1` (`DMA_IRQ_0` was already
   used by Core 0 audio in frank-snes), and `DVI_N_TMDS_BUFFERS=3`
   for slack.

4. **Fix geometry.** The 16bpp encoder is pixel-doubling, so scanline
   buffers are 320 wide, not 640.

5. **Eliminate red "late scanline" flashes under heavy Core 0 work.**
   `bus_ctrl` priority bits, `HIGH_PRIORITY` on TMDS DMA channels,
   palette LUT in scratch_y, `fill_scanline` in scratch_y, pre-zero
   pillarbox columns, `DVI_N_TMDS_BUFFERS=3`.

6. **Re-enable HDMI audio data islands.** In frank-snes that required
   draining a Core 0 SRAM ring after each push. The standalone
   driver here uses `dvi0.audio_ring` directly, so no intermediate
   queue.

7. **Half-pre-fill the audio ring at init.** Without it the rate-
   matched producer/consumer pair runs at the boundary of underrun
   and audio has short gaps.

8. **Use `full=true` on `get_write_size`.** The `full=false` branch
   in libdvi's audio_ring has a sign-error that over-reports free
   space; the producer overwrites samples the consumer hasn't read,
   audible as mid-waveform discontinuities.

9. **Decouple TMDS bit rate from CPU clock.** Added the libdvi knob
   `DVI_SM_CLKDIV` to divide the serialiser SMs and PWM pixel clock
   so the CPU can stay at 504 MHz while TMDS stays at 252 MHz spec.

10. **Generalise pin config and board presets.** Replaced the
    frank-snes-specific `board_config.h` import with build-time
    `FRANK_HDMI_PIN_CLK / D0 / D1 / D2` overrides and the
    `FRANK_HDMI_BOARD_M1` / `FRANK_HDMI_BOARD_M2` presets.

11. **Bring up the standalone example: HDMI signal locked.** Found
    that `invert_diffpairs` defaulted to `false` in my generalised
    code but the FRANK board needs `true`. Bumped the default and
    documented the override.

12. **Black-screen-after-lock.** `frank_hdmi_init` zeroed the palette
    LUT, silently overwriting any pre-init `frank_hdmi_set_palette`
    calls. Removed the memset (C zero-initialisation already handles
    the "no palette set" case correctly).

13. **Flicker + audio glitches synced to sprite movement.** Caused
    by `draw_static_pattern()` being called every frame on Core 0
    while Core 1 was reading the framebuffer live. Switched the
    example to "redraw only the marcher's bounding box" and both
    issues vanished.

14. **Tone played at the wrong pitch (~500 Hz instead of 440 Hz).**
    Wall-clock pacing was using `delayed_by_ms(prev, 17)`, which
    rounded the per-chunk interval up to 17 ms exactly. That made
    the long-term producer rate ~31 353 Hz instead of 32 000 Hz, a
    2 % rate mismatch the receiver re-clocked into a much larger
    audible pitch shift. Fixed by anchoring all chunk deadlines to
    a single `start` time and computing
    `next = delayed_by_us(start, chunks * CHUNK_US)`.

15. **Audible clicks at high volume in an otherwise stable tone.**
    `sinf()` was being called 533 times per video frame on the RP2350
    (no hardware FP), occasionally slow enough to delay the audio
    push past its deadline. Replaced with a 256-entry int16 sine
    LUT and 32-bit fixed-point phase accumulator. Tone became
    perfectly clean.

The current driver and example are the result of all 15 steps. If
you remove any of them, expect the corresponding symptom to come
back.
