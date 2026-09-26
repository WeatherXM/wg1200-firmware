/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "esp_secure_cert_read.h"
#include "soc/soc_caps.h"
#include "esp_secure_cert_tlv_type.h"

typedef struct esp_secure_cert_tlv_cfg {
    esp_secure_cert_tlv_type_t type; // Type of tlv structure
    const unsigned char *data; // pointer to the buffer containint data
    uint16_t data_len; // Length of the data field
    uint8_t flags; // flags field (Should be set to zero by default)
} esp_secure_cert_tlv_cfg_t;

/* @info
 *
 * Erase the esp_secure_cert partition
 * @return
 *      - ESP_OK on success
 *      - ESP_FAIL/other relevant esp error code on failure
 */
esp_err_t esp_secure_cert_erase_partition(void);

/* @info
 * Write the tlv in the flash
 * The API would fail if the tlv of given type already exists in the esp_secure_cert partition
 * @params
 *
 * tlv_cfg          The configuration structure for the TLV
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG when invalid arguments are provided
 *      - ESP_FAIL/other relevant esp error code on failure
 */
esp_err_t esp_secure_cert_append_tlv(esp_secure_cert_tlv_cfg_t *tlv_cfg);

#if SOC_HMAC_SUPPORTED
/* @info
 * Write the tlv in the flash with hmac based encryption scheme
 * This API assumes that a key has been burned in one of the eFuse key blocks with key purpose set to HMAC_UP. Oterwise it returns failure.
 * In this case the flag for HMAC encryption shall be applied to the TLV flags field.
 *
 * The API will do the following
 * 1) Find the first key block with purpose set to HMAC_UP
 * 2) Calculate the HMAC value by passing a pre-defined constant message
 * 3) Use the calculated hmac as key for aes-gcm encryption
 * 4) Write the encrypted data in TLV format
 *
 * @params
 *
 * tlv_cfg           The configuration structre for the tlv.
 * @return
 *      - ESP_OK on success
 *      - ESP_FAIL/other relevant esp error code on failure
 */
esp_err_t esp_secure_cert_append_tlv_with_hmac_encryption(esp_secure_cert_tlv_cfg_t *tlv_cfg);
#endif
