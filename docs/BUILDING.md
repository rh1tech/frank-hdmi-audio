# Building a sample app with frank-hdmi-sound

A walkthrough of building, flashing, and verifying the bundled
`hello_hdmi` example on a Raspberry Pi RP2350 board, plus a record of
every bug that came up while bringing the driver up so you can match
symptoms quickly.

The instructions below assume macOS or Linux. Windows works under WSL
with the same commands. If you are starting from a fresh checkout and
just want a UF2, skip to "1-shot build" at the end.

---

## 1. Prerequisites

Install once.

### Toolchain

```sh
# macOS (Homebrew)
brew install --cask gcc-arm-embedded
brew install cmake ninja picotool

# Debian/Ubuntu
sudo apt install gcc-arm-none-eabi cmake ninja-build
# picotool: build from source (instructions below) or apt on 24.04+
```

Verify:

```sh
arm-none-eabi-gcc --version          # 13.x or newer
cmake --version                      # 3.13 or newer
picotool version                     # 2.0 or newer
```

### Pico SDK

The driver targets Pico SDK 2.0+. Earlier 1.5.x SDKs predate RP2350
support and will not build.

```sh
git clone -b 2.1.1 --depth 1 https://github.com/raspberrypi/pico-sdk
cd pico-sdk
git submodule update --init --recursive
cd ..
export PICO_SDK_PATH="$PWD/pico-sdk"
```

Persist `PICO_SDK_PATH` in your shell rc so future builds find it.

### Hardware

- Raspberry Pi Pico 2 (RP2350). RP2040 is not supported.
- An HDMI sink that locks on raw TMDS without HPD or EDID handshake.
  Most TVs, monitors, and capture cards do.
- Differential pair wiring on 8 GPIOs. The default pin map matches the
  FRANK / MURMULATOR-2 board (CLK on GPIO 12/13, data on 14/15, 16/17,
  18/19). Override with `FRANK_HDMI_PIN_*` if your board differs.

---

## 2. Clone

```sh
git clone https://github.com/rh1tech/frank-hdmi-audio
cd frank-hdmi-audio
```

The repo layout:

```text
src/                    driver sources (libdvi-derived + frank glue)
examples/hello_hdmi/    sample app
docs/                   this file + LLM_GUIDE
release/                pre-built UF2 + ELF for the M2 board
scripts/                helper utilities
```

---

## 3. Configure and build the example

```sh
mkdir build
cd build
cmake -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2 ..
cmake --build . -j
```

Expected output ends with:

```text
[100%] Linking CXX executable hello_hdmi.elf
[100%] Built target hello_hdmi
```

Artifacts land in `build/examples/hello_hdmi/`:

| File              | Purpose                                  |
|-------------------|------------------------------------------|
| `hello_hdmi.elf`  | Debug symbols, use with `picotool`/GDB.  |
| `hello_hdmi.uf2`  | Drag-and-drop firmware image.            |
| `hello_hdmi.bin`  | Raw flash image.                         |
| `hello_hdmi.dis`  | Disassembly listing.                     |

If your board uses a different pin layout, configure with one of:

```sh
# M1 board preset
cmake -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2 \
      -DFRANK_HDMI_BOARD_M1=1 ..

# Custom pin layout
cmake -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2 \
      -DFRANK_HDMI_PIN_CLK=10 \
      -DFRANK_HDMI_PIN_D0=12 \
      -DFRANK_HDMI_PIN_D1=14 \
      -DFRANK_HDMI_PIN_D2=16 ..
```

If the receiver fails to lock and the picture stays black, the board's
differential pairs are probably wired with the polarity inverted from
the default. Add `-DFRANK_HDMI_INVERT_DIFFPAIRS=0` to flip.

---

## 4. Flash

Boot the board into BOOTSEL mode (hold BOOTSEL while plugging in USB)
and run:

```sh
picotool load -fx examples/hello_hdmi/hello_hdmi.elf
```

`-f` writes flash; `-x` resets and runs the new image. The board reboots
and starts driving HDMI immediately.

Alternatively, drag `hello_hdmi.uf2` onto the `RP2350` mass-storage
device that appears in BOOTSEL mode. The board reboots when the file
finishes copying.

---

## 5. Verify

You should see:

1. The HDMI sink locks within ~1 second of boot.
2. A dark navy 320x240 canvas with a faint 32-pixel grid, three solid
   coloured squares (red, green, blue) stacked diagonally, and a small
   white block marching across the bottom row.
3. Audio:
   - 3 seconds of pure 440 Hz sine.
   - 1 second of silence.
   - A multi-voice test melody (synth lead, bass, kick, snare) looping
     forever.

If audio is silent, check that the HDMI sink is not muted and that your
capture card's audio is exposed as a separate input device (on macOS
this is "USB Digital Audio" under System Settings, Sound).

The example also opens a USB CDC console. Open it with any serial
terminal at 115200 baud:

```sh
# macOS
ls /dev/cu.usbmodem*
screen /dev/cu.usbmodem1101 115200

# Linux
ls /dev/ttyACM*
screen /dev/ttyACM0 115200
```

There is a 1500 ms boot delay so the host has time to attach. Set
`HELLO_HDMI_BOOT_DELAY_MS=0` in the example's CMakeLists for production.

Heartbeat counters are exposed by the driver. Frames should increment at
~60/sec and lines at ~14400/sec:

```c
extern volatile uint32_t frank_hdmi_heartbeat_lines;
extern volatile uint32_t frank_hdmi_heartbeat_frames;
```

---

## 6. Integrate into your own app

Drop the directory into your project tree (vendored copy, git submodule,
or `FetchContent`) and wire it up in your top-level CMake:

```cmake
add_subdirectory(third_party/frank-hdmi-sound)
target_link_libraries(your_app
    frank_hdmi_sound
    pico_stdlib
    pico_multicore
)
```

Bring up the driver from `main()`:

```c
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "frank_hdmi.h"

static uint8_t fb[FRANK_HDMI_LOGICAL_WIDTH * FRANK_HDMI_LOGICAL_HEIGHT];

int main(void) {
    set_sys_clock_khz(252000, true);   /* with default DVI_SM_CLKDIV=1 */

    frank_hdmi_set_palette(0, 0x000000);
    frank_hdmi_set_palette(1, 0xFFFFFF);
    /* ...up to 256 entries... */

    frank_hdmi_init();
    frank_hdmi_set_buffer(fb, FRANK_HDMI_LOGICAL_WIDTH,
                              FRANK_HDMI_LOGICAL_HEIGHT);
    multicore_launch_core1(frank_hdmi_run_core1);

    while (1) {
        /* draw into fb on Core 0; push audio with
         * frank_hdmi_audio_write() */
    }
}
```

Push audio in chunks at the wall-clock rate:

```c
int16_t buf[2 * 533];                  /* one video frame at 32 kHz */
frank_hdmi_audio_write(buf, 533);
```

For why "wall clock" matters and how to do it correctly, see "Audio
pitch is wrong" below.

### CPU clock options

`DVI_SM_CLKDIV` decides how the TMDS bit clock relates to `sys_clock`:

| `DVI_SM_CLKDIV` | `sys_clock` to call | TMDS bit clock |
|-----------------|---------------------|----------------|
| 1 (default)     | 252 MHz             | 252 MHz        |
| 2               | 504 MHz             | 252 MHz        |

Use 2 if your application needs CPU headroom (emulation, decoding, etc).
Set it before `add_subdirectory()`:

```cmake
set(FRANK_HDMI_SM_CLKDIV 2 CACHE STRING "" FORCE)
add_subdirectory(third_party/frank-hdmi-sound)
```

---

## 7. Bugs and findings

Each entry below is a symptom that came up during bring-up, and what
fixed it. If you hit one of these in your own integration, the same
fix probably applies.

### "No HDMI signal at all (capture card shows colour bars)"

Capture cards display vertical SMPTE colour bars when there is no HDMI
signal. That is the capture card's "no signal" indicator, not driver
output. Driver output never looks like vertical bars unless you draw
them. Pick a test pattern that is obviously not vertical bars (the
example uses a navy field with a 32-pixel grid plus three solid R/G/B
squares plus a marching white block).

### "No HDMI signal but Core 1 heartbeat shows ~60 fps"

The encode loop is alive but the receiver is not locking. In rough
order of frequency:

1. `invert_diffpairs` is wrong for your board. Boards that put the
   negative leg on the lower-numbered GPIO of each pair (most) need
   `invert_diffpairs=true`, which is the default. Boards wired the
   other way need `false`. Override with
   `-DFRANK_HDMI_INVERT_DIFFPAIRS=0`.
2. System clock is wrong for the current `DVI_SM_CLKDIV`. With
   CLKDIV=1, `sys_clock` must be 252 MHz. With CLKDIV=2, `sys_clock`
   must be 504 MHz. Verify with `clock_get_hz(clk_sys)` printed over
   USB.
3. Pin numbers are wrong. Check the runtime GPIO funcsel: 4 = PWM,
   6 = PIO0 on RP2350.

```c
#include "hardware/structs/iobank0.h"
for (int p = 12; p <= 19; ++p) {
    uint32_t fn = (iobank0_hw->io[p].ctrl
                   & IO_BANK0_GPIO0_CTRL_FUNCSEL_BITS)
                  >> IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
    printf("gpio%d funcsel=%lu\n", p, (unsigned long)fn);
}
```

Expected on the M2 layout: `gpio12,13 funcsel=4` (PWM), `gpio14..19
funcsel=6` (PIO0).

### "Out of memory panic at boot"

Earlier libdvi versions called `malloc()` for the TMDS buffers and the
default Pico heap (2 KB) is too small for them (~12 KB). The driver
here uses a static buffer pool. If you replace `src/frank_dvi.c` with a
fresh upstream `dvi.c`, re-apply that patch (search for
`static_tmds_pool`).

### "Image is shifted right by half the screen"

The libdvi 16bpp encoder is pixel-doubling on the wire. A 640-wide
source buffer would have only its left half displayed (and that
pixel-doubled to 1280, far past the 640-wide canvas). Use a 320-wide
buffer (`FRANK_HDMI_LOGICAL_WIDTH`).

### "Image is all black even though Core 1 heartbeat increments"

The palette LUT is zeroed. Either you forgot to call
`frank_hdmi_set_palette` for any indices, or you wrote a recent libdvi
commit that added a `memset(palette, 0)` to `frank_hdmi_init`. The
driver here is structured so palette writes before init are preserved.

### "Red horizontal lines flash on the screen under load"

libdvi's `late_scanline_ctr` indicator. Core 1 is not keeping up with
the TMDS encoder because Core 0 traffic on the AHB fabric or SRAM banks
is starving it. The driver already does:

- `bus_ctrl.priority` bits set for PROC1, DMA_R, DMA_W.
- TMDS DMA channels marked `HIGH_PRIORITY` (libdvi patch).
- Palette LUT pinned in `scratch_y`.
- `fill_scanline` pinned in `scratch_y`.
- Pillarbox columns pre-zeroed once at init (no per-line memset).
- `DVI_N_TMDS_BUFFERS=3` for IRQ slack.

If red lines persist, the most likely cause is a long XIP or PSRAM
stall on Core 0. Move the offending code into RAM with
`__not_in_flash_func()`. The next most likely cause is a per-frame
full-screen redraw, see "Tearing or flicker on a static scene" below.

### "Tearing or flicker on a static scene"

Core 1 reads the framebuffer **live** every scanline. If Core 0
rewrites the same memory while Core 1 is reading it, the capture shows
torn pixels.

For a static scene with a small animated sprite, draw the static parts
once at init and only rewrite the sprite's bounding box per frame:

```c
erase_marcher_at(prev_col);   /* restore static background */
draw_marcher_at(col);
prev_col = col;
```

Calling `draw_static_pattern()` every frame caused both visible
square-flicker and audio glitches; the per-frame ~76 KB SRAM write
storm starved Core 1's DMA bandwidth long enough to register on the
HDMI side.

For larger animated scenes, allocate two framebuffers and call
`frank_hdmi_set_buffer(new_fb, w, h)` once you finish drawing into the
back buffer. The function is cheap and safe to call from Core 0 at
any time.

### "No audio"

In rough order of likelihood:

1. The producer never calls `frank_hdmi_audio_write`. Verify
   `frank_hdmi_init` was called first.
2. The HDMI sink is muted. On macOS the capture card audio is a
   separate USB device, check System Settings > Sound.
3. The producer's intermediate ring (between mixer and HDMI) is not
   being drained. The HDMI driver only drains its own ring.

### "Audio plays but has clicks or glitches mid-waveform"

You are passing `full=false` to libdvi's `get_write_size`. Upstream has
a sign-error in that branch and over-reports free space, so the
producer overwrites samples the consumer hasn't read. The driver here
always uses `full=true`. Do not switch back.

### "Audio plays at the wrong pitch (~13 % high or low)"

The producer's actual rate doesn't match the rate the driver
advertises.

The driver tells the HDMI receiver an audio rate via the info-frame
and CTS/N values (32 kHz by default). The receiver is a pull consumer:
it drains samples at that declared rate, regardless of how fast the
producer is feeding them in. If your producer's long-term sample rate
differs from the declared rate, the receiver hears the difference as
pitch shift, and capture cards in particular re-clock the stream in
audible jumps.

Concrete failure: producing 533 samples every 17 ms (using
`sleep_ms(17)` or `delayed_by_ms(prev, 17)`) gives 533 / 0.017 ≈
31 353 Hz, ~2 % low. That sounds wrong, and the rounding error per
chunk (with `delayed_by_ms` of an integer ms) accumulates without bound.

Fix: pace from a single anchor time and a microsecond deadline.

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
precision. Tested: a 440 Hz tone comes out at 440 Hz, FFT-verified.

### "Audio has clicks audible only at high volume"

Same root cause as the previous symptom: a small but persistent
producer-rate mismatch the receiver re-clocks. Anchor every chunk's
deadline to a single `start` time with
`delayed_by_us(start, chunks * CHUNK_US)`, as shown above.

A second cause: `sinf()` called per sample. `sinf()` is software
floating point on the RP2350 (no hardware FP). A naive
`for (i=0..532) buf[i] = sinf(phase) * gain` per video frame is slow
enough that the audio push slips its deadline, the producer falls
behind, and the receiver underruns. Use a sine LUT and a 32-bit
fixed-point phase accumulator instead:

```c
static int16_t sine_lut[256];
static const uint32_t PHASE_STEP =
    (uint32_t)(((uint64_t)TONE_HZ * (1ull << 32)) / AUDIO_RATE);
/* per sample */
int16_t s = sine_lut[phase >> 24];
phase += PHASE_STEP;
```

Four instructions per sample, no FP, no clock-dependent latency.

### "Audio sounds gappy or ratchety"

The audio ring is too small. The producer is bursty (one chunk per
video frame); if the ring is smaller than one chunk you drop a fraction
of every burst. The driver here uses 2048 frames (~64 ms at 32 kHz)
which fits typical ~533-frame bursts with plenty of margin.

### Audio waveform analysis

To check the actual audio rate against the declared rate, capture a
clip and run an FFT.

```sh
# macOS, capture 5s from the HDMI capture card's audio input
ffmpeg -y -f avfoundation -i ":USB Digital Audio" \
       -t 5 -ac 2 -ar 48000 /tmp/aud.wav

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

For a 440 Hz reference tone this should print ~440 Hz to within about
1 Hz. Anything more than ±5 Hz is a producer-rate problem.

---

## 8. Hard rules (recap)

1. `set_sys_clock_khz` must be called before `frank_hdmi_init` and the
   value must satisfy `sys_clock = TMDS_bit_clock × DVI_SM_CLKDIV`.
   Wrong clock = no signal lock at all.
2. Core 1 belongs to the driver. `frank_hdmi_run_core1` never returns.
3. PIO0 belongs to the driver. State machines 0/1/2 carry the three
   TMDS data lanes. Use PIO1/PIO2 for application peripherals.
4. DMA_IRQ_1 belongs to the driver.
5. Frame buffer is 8-bit palette indices, max 320x240.
6. Set the palette before or after `frank_hdmi_init`. Both work.

---

## 9. 1-shot build

If you just want a UF2:

```sh
git clone https://github.com/rh1tech/frank-hdmi-audio
cd frank-hdmi-audio && mkdir build && cd build
cmake -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2 ..
cmake --build . -j
picotool load -fx examples/hello_hdmi/hello_hdmi.elf
```

Or drag-and-drop `release/hello_hdmi_m2.uf2` onto a board in BOOTSEL
mode for the M2 pin layout. No build required.
