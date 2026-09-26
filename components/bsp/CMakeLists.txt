message(STATUS "=== CMAKE DIAGNOSTICS ===")
message(STATUS "PIO_ENV_NAME from ENV: $ENV{PIO_ENV_NAME}")
message(STATUS "CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")
message(STATUS "PROJECT_NAME: ${PROJECT_NAME}")
message(STATUS "IDF_TARGET: ${IDF_TARGET}")

if(CONFIG_SCREENLESS)
    set(SCREENLESS TRUE)
else()
    set(SCREENLESS FALSE)
endif()

message(STATUS "SCREENLESS decided as: ${SCREENLESS}")
message(STATUS "=========================")

if(NOT SCREENLESS)
    idf_component_register(
        SRCS
            "src/boards/bsp_board.c"
            "src/boards/lcd_panel_config.c"
            "src/boards/sensecap_indicator_board.c"
            "src/calibration/param_save.c"
            "src/calibration/touch_calibration.c"
            "src/indev/esp_hid_gap.c"
            "src/indev/indev.c"
            "src/indev/indev_hid.c"
            "src/indev/indev_tp.c"
            "src/indev/xpt2046.c"
            "src/peripherals/bsp_btn.c"
            "src/peripherals/bsp_codec.c"
            "src/peripherals/bsp_i2c.c"
            "src/peripherals/bsp_i2s.c"
            "src/peripherals/bsp_lcd.c"
            "src/peripherals/lcd_panel_st7789.c"
            "src/peripherals/lcd_panel_st7796.c"
            "src/peripherals/virtual_mic.c"
            "src/storage/bsp_sdcard.c"
            "src/storage/bsp_spiffs.c"
        INCLUDE_DIRS "include" "src" "src/calibration" "src/indev" "src/peripherals" "src/storage"
        REQUIRES driver bus esp_adc esp_lcd nvs_flash iot_button i2c_devices lora esp_driver_ledc esp_driver_spi esp_driver_gpio spiffs fatfs
    )
else()
    idf_component_register(
        SRCS ""
        INCLUDE_DIRS "include"
        REQUIRES driver bus
    )
endif()

