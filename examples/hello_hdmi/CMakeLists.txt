# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(hello_hdmi
    main.c
)

target_link_libraries(hello_hdmi
    frank_hdmi_sound
    pico_stdlib
    pico_multicore
)

# Pause briefly at boot so a host CDC terminal can attach in time to
# see the first log line.  Set to 0 in production.
target_compile_definitions(hello_hdmi PRIVATE HELLO_HDMI_BOOT_DELAY_MS=1500)


# USB CDC stdio so the board can be debugged over its programming USB
# port (no UART hookup needed).  UART stays off; the example does not
# wire any default UART pins.
pico_enable_stdio_uart(hello_hdmi 0)
pico_enable_stdio_usb(hello_hdmi 1)

pico_add_extra_outputs(hello_hdmi)
