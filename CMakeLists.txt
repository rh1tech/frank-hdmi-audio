# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.13)

# Pico 2 (RP2350) is the supported target.  RP2040 has not been
# tested.
set(PICO_BOARD pico2 CACHE STRING "Pico board type")

include(pico_sdk_import.cmake)

project(frank_hdmi_sound C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Core driver library (INTERFACE — consumers compile the sources).
add_subdirectory(src)

# Examples are built only when this is the top-level project, so that
# `add_subdirectory(frank-hdmi-sound)` from a parent project pulls in
# the library only.
if(CMAKE_PROJECT_NAME STREQUAL "frank_hdmi_sound")
    add_subdirectory(examples/hello_hdmi)
endif()
