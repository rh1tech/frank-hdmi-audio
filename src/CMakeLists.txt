# SPDX-License-Identifier: GPL-3.0-or-later
#
# frank-hdmi-sound — driver library target.
# Consumers link `frank_hdmi_sound`; everything else (libdvi included)
# is wired in transitively.

add_subdirectory(libdvi)

add_library(frank_hdmi_sound INTERFACE)

target_sources(frank_hdmi_sound INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/frank_hdmi.c
)

target_include_directories(frank_hdmi_sound INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(frank_hdmi_sound INTERFACE
    libdvi
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_dma
    hardware_pwm
    hardware_pio
    hardware_interp
)

# Apply strict warnings to our source files only — the Pico SDK has
# its own warning policy and we don't want to police it.
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/frank_hdmi.c
    PROPERTIES COMPILE_FLAGS
    "-Wall -Wextra -Wshadow -Wno-unused-parameter")
