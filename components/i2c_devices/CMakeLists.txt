
if(IDF_TARGET STREQUAL "esp32s3")
idf_component_register(
    SRC_DIRS
        "touch_panel"
        "io_expander"
        "sensor/bmp3xx"
    INCLUDE_DIRS
        "touch_panel/include"
        "io_expander"
        "sensor/bmp3xx"
    REQUIRES
        bsp)
else()
idf_component_register()
endif()
