# SPDX-License-Identifier: GPL-3.0-or-later
########################################################################
# frank-hdmi-sound — driver library target.
#
# All source lives flat under src/ now.  Consumers add this directory
# via add_subdirectory() and link against the `frank_hdmi_sound`
# INTERFACE target — everything else is wired in transitively.
########################################################################

add_library(frank_hdmi_sound INTERFACE)

target_sources(frank_hdmi_sound INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/frank_hdmi.c
    ${CMAKE_CURRENT_LIST_DIR}/frank_dvi.c
    ${CMAKE_CURRENT_LIST_DIR}/frank_dvi_timing.c
    ${CMAKE_CURRENT_LIST_DIR}/frank_serialiser.c
    ${CMAKE_CURRENT_LIST_DIR}/frank_audio_ring.c
    ${CMAKE_CURRENT_LIST_DIR}/frank_data_packet.c
    ${CMAKE_CURRENT_LIST_DIR}/frank_tmds.c
    ${CMAKE_CURRENT_LIST_DIR}/frank_tmds.S
)

target_include_directories(frank_hdmi_sound INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(frank_hdmi_sound INTERFACE
    pico_stdlib
    pico_multicore
    pico_util
    hardware_clocks
    hardware_dma
    hardware_interp
    hardware_pio
    hardware_pwm
)

# DVI_SM_CLKDIV picks the divisor between sys_clock and the TMDS bit
# clock for both the serialiser SMs and the PWM pixel clock:
#   1  -> sys_clock = TMDS bit clock (252 MHz CPU, no overclock)
#   2  -> sys_clock = 2x TMDS bit clock (504 MHz CPU, recommended
#         for applications that need CPU headroom).
# Override via -DFRANK_HDMI_SM_CLKDIV=N before add_subdirectory().
set(FRANK_HDMI_SM_CLKDIV "1" CACHE STRING
    "Divisor between sys_clock and TMDS bit clock (1 or 2)")

# DVI_N_TMDS_BUFFERS=3 gives the IRQ handler enough slack that
# occasional Core 0 SRAM bursts don't underrun the TMDS FIFOs and
# flash a red 'late scanline' indicator.
target_compile_definitions(frank_hdmi_sound INTERFACE
    DVI_N_TMDS_BUFFERS=3
    DVI_SM_CLKDIV=${FRANK_HDMI_SM_CLKDIV}
)

pico_generate_pio_header(frank_hdmi_sound
    ${CMAKE_CURRENT_LIST_DIR}/frank_serialiser.pio)
pico_generate_pio_header(frank_hdmi_sound
    ${CMAKE_CURRENT_LIST_DIR}/frank_clock.pio)

# Strict warnings on our sources.  -Wno-sign-compare and
# -Wno-unused-function silence noise from the upstream-derived files
# that isn't worth re-touching in this fork.
set(_frank_strict "-Wall -Wextra -Wshadow -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function")
foreach(_src
    ${CMAKE_CURRENT_LIST_DIR}/frank_hdmi.c
    ${CMAKE_CURRENT_LIST_DIR}/frank_dvi.c
    ${CMAKE_CURRENT_LIST_DIR}/frank_dvi_timing.c
    ${CMAKE_CURRENT_LIST_DIR}/frank_serialiser.c
    ${CMAKE_CURRENT_LIST_DIR}/frank_audio_ring.c
    ${CMAKE_CURRENT_LIST_DIR}/frank_data_packet.c
    ${CMAKE_CURRENT_LIST_DIR}/frank_tmds.c)
    set_source_files_properties(${_src} PROPERTIES COMPILE_FLAGS "${_frank_strict}")
endforeach()
unset(_frank_strict)
