set(COMMON ${CMAKE_CURRENT_LIST_DIR}/../../common)
# CV_TIER_A=1: breadboard build with MCP4728/ADS1115 modules instead of the PCB converters.
if(CV_TIER_A)
    set(CONVERTERS "tier_a.c")
else()
    set(CONVERTERS "precision.c" "codec.c")
endif()
idf_component_register(
    SRCS "main.c" "storage.c" ${CONVERTERS} "usb.c" "midi_router.c" "power_mgr.c"
         "display.c" "ble_midi_nimble.c"
         "${COMMON}/cal.c" "${COMMON}/engine.c" "${COMMON}/proto.c" "${COMMON}/usb_desc.c"
         "${COMMON}/midi_cv.c" "${COMMON}/ble_midi.c" "${COMMON}/power.c"
    INCLUDE_DIRS "." "${COMMON}"
    REQUIRES espressif__tinyusb usb esp_driver_i2s esp_driver_spi esp_driver_i2c esp_driver_ledc esp_driver_gptimer
             esp_driver_gpio esp_adc esp_lcd nvs_flash bt esp_timer)
# TinyUSB reads its configuration from main/tusb_config.h.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC "${CMAKE_CURRENT_LIST_DIR}" "${COMMON}")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
if(CV_TIER_A)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CV_TIER_A=1)
endif()
