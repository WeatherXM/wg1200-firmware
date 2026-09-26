# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources 
    ${CMAKE_SOURCE_DIR}/main/*.cpp
    ${CMAKE_SOURCE_DIR}/main/*.c
    ${CMAKE_SOURCE_DIR}/main/*.cc
    ${CMAKE_SOURCE_DIR}/main/*.S
)

# Add private mbedtls header directories for direct mbedtls v4.x low-level API access
set(mbedtls_priv_include
    "${IDF_PATH}/components/mbedtls/mbedtls/tf-psa-crypto/drivers/builtin/include"
    "${IDF_PATH}/components/mbedtls/mbedtls/tf-psa-crypto/include"
    "${IDF_PATH}/components/mbedtls/mbedtls/tf-psa-crypto/core"
)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "include" ${mbedtls_priv_include}
    PRIV_REQUIRES ArduinoJson mbedtls nvs_flash mqtt cjson bsp bus lora esp_adc esp_mm mdns
)


target_add_binary_data(${COMPONENT_TARGET} "certs/wxm_mqtt.pem" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "certs/wxm.pem" TEXT)
