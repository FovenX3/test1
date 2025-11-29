# Add executable
add_executable(neopico_hd
    main.c
    usb_descriptors.c
)

# Generate PIO headers
pico_generate_pio_header(neopico_hd ${CMAKE_CURRENT_LIST_DIR}/mvs_sync.pio)

# Link libraries
target_link_libraries(neopico_hd
    pico_stdlib
    hardware_pio
    hardware_clocks
    hardware_dma
    hardware_irq
    tinyusb_device
    tinyusb_board
)

# Include directory for tusb_config.h
target_include_directories(neopico_hd PRIVATE ${CMAKE_CURRENT_LIST_DIR})

# Disable stdio USB (we handle USB ourselves via TinyUSB)
pico_enable_stdio_usb(neopico_hd 0)
pico_enable_stdio_uart(neopico_hd 0)

# Create map/bin/hex/uf2 files
pico_add_extra_outputs(neopico_hd)