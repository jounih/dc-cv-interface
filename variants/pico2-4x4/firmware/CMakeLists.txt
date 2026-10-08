cmake_minimum_required(VERSION 3.13)

# Default target: Raspberry Pi Pico 2 (RP2350). PICO_BOARD=pico builds for RP2040
# (compiles, but the ADC rate is only approximately 48 kHz there; see README).
set(PICO_BOARD pico2 CACHE STRING "Board type")
set(CV_N_OUT 4 CACHE STRING "Number of CV outputs (4 or 8)")

include(pico_sdk_import.cmake)
project(cv_interface C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(cv_interface
    src/main.c
    src/converters.c
    src/cal.c
    src/cal_flash.c
    src/engine.c
    src/proto.c
    src/usb_desc.c
    src/quirk_os_guessing.c
)
pico_generate_pio_header(cv_interface ${CMAKE_CURRENT_LIST_DIR}/src/cv.pio)
target_include_directories(cv_interface PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_definitions(cv_interface PRIVATE CV_N_OUT=${CV_N_OUT} BOARD_TUD_RHPORT=0)
target_compile_options(cv_interface PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(cv_interface
    pico_stdlib pico_unique_id pico_flash pico_bootrom
    hardware_pio hardware_dma hardware_clocks hardware_flash hardware_watchdog
    tinyusb_device)
pico_enable_stdio_usb(cv_interface 0)
pico_enable_stdio_uart(cv_interface 0)
pico_add_extra_outputs(cv_interface)
