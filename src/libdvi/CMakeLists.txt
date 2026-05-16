# SPDX-License-Identifier: BSD-3-Clause
########################################################################
# libdvi (vendored)
#
# Upstream: https://github.com/Wren6991/PicoDVI (Luke Wren) with audio
# data-island additions from
# https://github.com/shuichitakano/PicoDVI-audio (shuichitakano).
# Integration pattern follows
# https://github.com/fruit-bat/pico-zxspectrum (fruit-bat & contributors).
#
# Local patches against upstream are tagged with `PATCH
# (frank-hdmi-sound):` comments inside the affected files.
########################################################################

add_library(libdvi INTERFACE)

target_sources(libdvi INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/dvi.c
    ${CMAKE_CURRENT_LIST_DIR}/dvi_serialiser.c
    ${CMAKE_CURRENT_LIST_DIR}/dvi_timing.c
    ${CMAKE_CURRENT_LIST_DIR}/tmds_encode.c
    ${CMAKE_CURRENT_LIST_DIR}/tmds_encode.S
    ${CMAKE_CURRENT_LIST_DIR}/data_packet.c
    ${CMAKE_CURRENT_LIST_DIR}/audio_ring.c
)

target_include_directories(libdvi INTERFACE ${CMAKE_CURRENT_LIST_DIR})

# 3 TMDS buffers gives the IRQ handler enough slack that occasional
# Core 0 SRAM bursts don't underrun the FIFOs and flash a red "late
# scanline" indicator.
#
# DVI_SM_CLKDIV picks the divisor between sys_clock and the TMDS bit
# clock applied to both the serialiser SMs and the PWM pixel clock:
#   1  -> sys_clock = TMDS bit clock (252 MHz CPU, no overclock)
#   2  -> sys_clock = 2x TMDS bit clock (504 MHz CPU, recommended
#         for applications that need CPU headroom).
# Override before add_subdirectory(src/libdvi).
set(FRANK_HDMI_SM_CLKDIV "1" CACHE STRING
    "Divisor between sys_clock and TMDS bit clock (1 or 2)")
target_compile_definitions(libdvi INTERFACE
    DVI_N_TMDS_BUFFERS=3
    DVI_SM_CLKDIV=${FRANK_HDMI_SM_CLKDIV}
)

target_link_libraries(libdvi INTERFACE
    pico_base_headers
    pico_util
    hardware_dma
    hardware_interp
    hardware_pio
    hardware_pwm
)

pico_generate_pio_header(libdvi ${CMAKE_CURRENT_LIST_DIR}/dvi_serialiser.pio)
pico_generate_pio_header(libdvi ${CMAKE_CURRENT_LIST_DIR}/tmds_encode_1bpp.pio)
pico_generate_pio_header(libdvi ${CMAKE_CURRENT_LIST_DIR}/dvi_clock.pio)

# Strict warnings on the vendored sources too.  -Wno-sign-compare /
# -Wno-unused-function suppress noise from the upstream codebase that
# isn't worth re-touching in a vendored copy.
set(_libdvi_flags "-Wall -Wextra -Wshadow -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function")
foreach(_src
    ${CMAKE_CURRENT_LIST_DIR}/dvi.c
    ${CMAKE_CURRENT_LIST_DIR}/dvi_serialiser.c
    ${CMAKE_CURRENT_LIST_DIR}/dvi_timing.c
    ${CMAKE_CURRENT_LIST_DIR}/tmds_encode.c
    ${CMAKE_CURRENT_LIST_DIR}/data_packet.c
    ${CMAKE_CURRENT_LIST_DIR}/audio_ring.c)
    set_source_files_properties(${_src} PROPERTIES COMPILE_FLAGS "${_libdvi_flags}")
endforeach()
unset(_libdvi_flags)
