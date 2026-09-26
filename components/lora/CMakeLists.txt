if(IDF_TARGET STREQUAL "esp32s3")
set(srcs 
    "sx126x_sensecap_board.c"
    "radio.c"
    "sx126x.c"
    "timer.c")

idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS .
                    REQUIRES bus i2c_devices driver esp_driver_spi esp_driver_gpio)

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
else()
idf_component_register()
endif()
