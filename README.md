# frank-hdmi-sound

FRANK HDMI Sound is a small HDMI video and audio driver for the 
Raspberry Pi RP2350, packaged as a Pico SDK library. It outputs 
640x480p60 video from a 320x240 palette-indexed framebuffer and embeds 
32 kHz stereo PCM in the HDMI data-island stream. No external DAC, 
no separate audio path.

The TMDS encoding core is based on
[PicoDVI](https://github.com/Wren6991/PicoDVI) by Wren6991, via shuichitakano's
[PicoDVI-audio](https://github.com/shuichitakano/PicoDVI-audio) fork,
which adds HDMI data-island audio support. The libdvi integration
pattern follows
[pico-zxspectrum](https://github.com/fruit-bat/pico-zxspectrum). This
library extracts and generalises the HDMI path originally written for
[frank-snes](https://github.com/rh1tech/frank-snes).

![hello_hdmi test pattern: navy field with a 32-pixel grid, three coloured squares and a marching white block](screenshots/screen1.png)

Captured from the bundled `hello_hdmi` example over an HDMI capture
card. Audio (440 Hz tone, then a multi-voice melody) is carried in the
same HDMI stream.

## What you get

- 640x480p60 HDMI video on a single PIO block.
- 320x240 logical canvas. The libdvi 16bpp encoder is pixel-doubling,
  so the source framebuffer is 320 wide on the wire.
- 8-bit palette-indexed framebuffer with a 256-entry RGB888 LUT.
- 32 kHz stereo PCM audio in the HDMI data-island stream.
- Producer/consumer split across the two RP2350 cores: Core 1 owns
  the DVI engine, Core 0 stays free for application work.
- CPU at 504 MHz. libdvi divides its serialiser SMs and the PWM
  pixel clock by 2 so the 252 MHz TMDS bit clock stays at spec.

## What you do not get

- Frame buffers larger than 320x240. Smaller buffers are centred with
  a black pillarbox and letterbox; larger buffers are clipped.
- 50 Hz, 720x576, or 1280x720 modes. The vendored libdvi build can
  do them; this driver only wires up 640x480p60.
- A per-line scanline callback. Application work has to live on
  Core 0.
- HDMI hot-plug detect, EDID, or HDCP. The driver assumes a passive
  HDMI sink (TV, capture card, monitor) that locks on the TMDS
  stream unconditionally.

## Pin layout

The driver writes a TMDS clock pair on `(CLK_PIN, CLK_PIN+1)` and three
TMDS data pairs on `(D0_PIN, D0_PIN+1)`, `(D1_PIN, D1_PIN+1)`,
`(D2_PIN, D2_PIN+1)`. `CLK_PIN` must be even (PWM slice constraint).

Default layout matches the FRANK / M2 board:

| Signal | GPIO  |
|--------|-------|
| CLK-   | 12    |
| CLK+   | 13    |
| D0-    | 14    |
| D0+    | 15    |
| D1-    | 16    |
| D1+    | 17    |
| D2-    | 18    |
| D2+    | 19    |

Override at build time:

```cmake
target_compile_definitions(your_target PRIVATE
    FRANK_HDMI_PIN_CLK=10
    FRANK_HDMI_PIN_D0=12
    FRANK_HDMI_PIN_D1=14
    FRANK_HDMI_PIN_D2=16
)
```

If your board has the differential pair wired with the positive leg on
the lower-numbered GPIO of each pair, also set
`FRANK_HDMI_INVERT_DIFFPAIRS=1`.

## Prerequisites

- Pico SDK 2.0 or later. Set `PICO_SDK_PATH` in your environment.
- Pico 2 (RP2350) board. RP2040 is not supported.
- ARM GNU toolchain (`arm-none-eabi-gcc`).
- `picotool` on `PATH` for flashing.

## Using the library in your project

1. Drop the directory into your project tree. A vendored copy, a git
   submodule, or `FetchContent` all work.

2. Add it to your top-level CMake:

   ```cmake
   add_subdirectory(third_party/frank-hdmi-sound)

   target_link_libraries(your_app
       frank_hdmi_sound
       pico_stdlib
       pico_multicore
   )
   ```

3. Bring up the driver from your `main()`:

   ```c
   #include "pico/stdlib.h"
   #include "pico/multicore.h"
   #include "hardware/clocks.h"
   #include "frank_hdmi.h"

   static uint8_t fb[320 * 240];

   int main(void) {
       set_sys_clock_khz(504000, true);

       /* Fill some palette entries. */
       frank_hdmi_set_palette(0, 0x000000);
       frank_hdmi_set_palette(1, 0xFFFFFF);

       frank_hdmi_init();
       frank_hdmi_set_buffer(fb, 320, 240);
       multicore_launch_core1(frank_hdmi_run_core1);

       /* Draw into fb at any time.  Core 1 picks up the changes on
        * the next scanline read. */
       while (1) tight_loop_contents();
   }
   ```

4. Push audio whenever you have samples ready:

   ```c
   int16_t buf[2 * 533];   /* one video frame's worth at 32 kHz */
   /* fill buf with stereo int16 samples */
   frank_hdmi_audio_write(buf, 533);
   ```

   The audio ring is 2048 frames, which is about 64 ms at 32 kHz.
   The driver drops samples silently on overflow; it never blocks
   the producer.

## Building the example

```sh
git clone https://github.com/rh1tech/frank-hdmi-audio
cd frank-hdmi-audio
mkdir build && cd build
cmake -DPICO_PLATFORM=rp2350 ..
make -j
picotool load -fx examples/hello_hdmi/hello_hdmi.elf
```

The example draws a navy-and-grid test pattern with three R/G/B
squares and a marching white block, then plays:

1. A pure 440 Hz tone for 3 seconds.
2. 1 second of silence.
3. A multi-voice test melody (synth lead, bass, kick, snare) on
   loop.

A pre-built UF2 for the M2 board is also checked in at
`release/hello_hdmi_m2.uf2`.

For a step-by-step walkthrough (toolchain install, configure, flash,
verify, integrate, plus a debugging reference covering every bug the
driver hit during bring-up), see [docs/BUILDING.md](docs/BUILDING.md).

For a from-scratch tutorial that builds an app like `hello_hdmi` one
piece at a time with explanations for every non-obvious line, see
[docs/WRITING_AN_APP.md](docs/WRITING_AN_APP.md).

## System clock

The driver does not change the CPU clock. The vendored libdvi build is
configured with `DVI_SM_CLKDIV=2`, so the TMDS bit clock is sys_clock
divided by 2. Concretely:

| `set_sys_clock_khz` | TMDS bit clock | Notes |
|---------------------|----------------|-------|
| 504 000             | 252 MHz        | Recommended. |
| 252 000             | 126 MHz        | Fails: too slow for 640x480p60. |

If you need to run the CPU at a different speed, set `DVI_SM_CLKDIV`
accordingly when adding the subdirectory:

```cmake
set(DVI_SM_CLKDIV 1 CACHE STRING "" FORCE)   # CPU = TMDS = 252 MHz
add_subdirectory(third_party/frank-hdmi-sound)
```

## Memory footprint

| Section          | Bytes      | Where |
|------------------|------------|-------|
| TMDS DMA buffers | ~12 KB (3 × ~3.8 KB) | main SRAM |
| Audio ring       | 8 KB       | main SRAM |
| Scanline buffers | 1.25 KB (2 × 640 B) | main SRAM |
| Palette LUT      | 512 B      | scratch_y |
| `fill_scanline`  | ~200 B     | scratch_y |

Total: ~22 KB main SRAM, ~700 B scratch_y. That leaves plenty of
room for an application working set on the RP2350's 512 KB.

## Acknowledgements

- [Luke Wren](https://github.com/Wren6991) for PicoDVI.
- [shuichitakano](https://github.com/shuichitakano) for the
  PicoDVI-audio fork that adds HDMI data-island audio.
- [fruit-bat](https://github.com/fruit-bat) and contributors for
  pico-zxspectrum, whose libdvi integration pattern this driver
  follows.

## Licence

The driver source (`src/frank_hdmi.{c,h}`, the example, build system,
docs) is GPL-3.0-or-later, Copyright (c) 2026 Mikhail Matveev.

The driver internals (`src/frank_dvi*.{c,h}`,
`src/frank_serialiser*`, `src/frank_audio_ring*`,
`src/frank_data_packet*`, `src/frank_tmds*`,
`src/frank_queue_inline.h`) are based on libdvi by Luke Wren and
shuichitakano. BSD-3-Clause, Copyright (c) 2021 Luke Wren and
contributors. Each file carries the BSD notice; local changes are
tagged `PATCH (frank-hdmi-sound):` so they can be lifted back
upstream if anyone wants to.

## Author

Mikhail Matveev <[xtreme@rh1.tech](mailto:xtreme@rh1.tech)>

[https://rh1.tech](https://rh1.tech) | [GitHub](https://github.com/rh1tech/frank-hdmi-audio)
