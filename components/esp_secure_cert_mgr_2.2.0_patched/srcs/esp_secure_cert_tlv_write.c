#if __has_include(<mbedtls/build_info.h>)
#include <mbedtls/build_info.h>
#endif

#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_partition.h"
#include "esp_crc.h"
#include "esp_efuse.h"
#include "esp_secure_cert_tlv_config.h"
#include "esp_secure_cert_tlv_private.h"
#include "esp_secure_cert_read.h"
#include "esp_secure_cert_write.h"
#include "nvs_flash.h"
#include "soc/soc_caps.h"
#include "esp_heap_caps.h"

#if __has_include(<mbedtls/gcm.h>)
#include <mbedtls/gcm.h>
#else
#if !defined(ESP_RSA_DS_DRIVER_ENABLED) && !defined(esp_rsa_ds_opaque_sign_hash_operation_t)
typedef struct {} esp_rsa_ds_opaque_sign_hash_operation_t;
#endif
#include <psa/crypto.h>
#endif


static const char *TAG = "esp_secure_cert_write";
#define MIN_ALIGNMENT_REQUIRED 16

esp_err_t esp_secure_cert_erase_partition(void)
{
    esp_partition_iterator_t it = esp_partition_find(ESP_SECURE_CERT_TLV_PARTITION_TYPE, ESP_PARTITION_SUBTYPE_ANY, ESP_SECURE_CERT_TLV_PARTITION_NAME);
    if (it == NULL) {
        ESP_LOGE(TAG, "Partition not found.");
        return ESP_FAIL;
    }

    const esp_partition_t *part = esp_partition_get(it);
    if (part == NULL) {
        ESP_LOGE(TAG, "Could not get partition.");
        return ESP_FAIL;
    }

    // Erase the complete region which shall be used by the esp_secure_cert partition
    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not erase partition.");
        return err;
    }
    ESP_LOGI(TAG, "Successfully erased esp_secure_cert partition");
    return ESP_OK;
}

/*  Write the data to flash at particular offset
 *  @params
 *  offset
 *      The offset from the start of the esp_secure_cert partition.
 *  data_buf
 *      The buffer containing the data to be flashed
 *  data_len
 *      The length of the data in bytes
 *  Note:
 *      This API cannot be used when flash encryption is enabled. When flash encryption is enabled
 *      the esp_partition_write requires a 32 byte aligned flash address and 32 byte aligned data buffer.
 *      This API does not take care of that.
 **/
static esp_err_t esp_secure_cert_write_to_flash(size_t offset, unsigned char *data_buf, size_t data_len)
{
    assert(data_buf);
    ESP_LOGI(TAG, "the data of length %d bytes shall be flashed at an offset of %d"
             " from the base address of esp_secure_cert partition", data_len, offset);

    esp_partition_iterator_t it = esp_partition_find(ESP_SECURE_CERT_TLV_PARTITION_TYPE, ESP_PARTITION_SUBTYPE_ANY, ESP_SECURE_CERT_TLV_PARTITION_NAME);
    if (it == NULL) {
        ESP_LOGI(TAG, "Could not find esp_secure_cert partition.");
        return ESP_FAIL;
    }

    const esp_partition_t *part = esp_partition_get(it);
    if (part == NULL) {
        ESP_LOGI(TAG, "Could not get esp_secure_cert partition.");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_FAIL;
    err = esp_partition_write(part, offset, data_buf, data_len);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Could not write to esp_secure_cert partition.");
        return err;
    }

    return ESP_OK;
}

/* Prepare the tlv structure of the given data
 * @params
 *  type        type of tlv
 *  value       Buffer containing the data to be written to the tlv
 *  value_len   Length of the data in bytes
 *  output_buf  The output buffer where the generated tlv shall be stored
 *  output_len  The length of the final tlv structure
 *  output_len  (input/output)
 *              input : This should contain the length of the output buffer
 *              output: This value shall be updated with the actual output length
 *  flags       The flags byte
 */
static esp_err_t esp_secure_cert_prepare_tlv(esp_secure_cert_tlv_type_t type, const unsigned char *value, size_t value_len, unsigned char *output_buf, size_t *output_len, uint8_t flags)
{
    size_t data_len = 0;
    // magic 32 is added because we pad the data to the nearest multiple of 32
    size_t required_buf_len = sizeof(esp_secure_cert_tlv_header_t) + sizeof(esp_secure_cert_tlv_footer_t) + value_len + 16;
    if (*output_len < data_len) {
        ESP_LOGE(TAG, "buffer len = %d is smaller than the required size = %d", *output_len, required_buf_len);
        return ESP_ERR_INVALID_ARG;
    }
    esp_secure_cert_tlv_header_t tlv_header;
    memset((void *)&tlv_header, 0, sizeof(esp_secure_cert_tlv_header_t));
    tlv_header.type = (uint8_t) type;
    tlv_header.flags = flags;
    tlv_header.magic = ESP_SECURE_CERT_TLV_MAGIC;
    tlv_header.length = value_len;
    memset(output_buf, 0, *output_len);
    memcpy(output_buf, &tlv_header, sizeof(esp_secure_cert_tlv_header_t));
    data_len = data_len + sizeof(esp_secure_cert_tlv_header_t);
    memcpy(output_buf + data_len, value, value_len);
    data_len = data_len + value_len;

    uint8_t padding_length = MIN_ALIGNMENT_REQUIRED - (value_len % MIN_ALIGNMENT_REQUIRED);
        padding_length = (padding_length == MIN_ALIGNMENT_REQUIRED) ? 0 : padding_length;

    data_len = data_len + padding_length;
    uint32_t crc_data = 0;
    crc_data = esp_crc32_le(UINT32_MAX, (const uint8_t * )output_buf, data_len);
    ESP_LOGD(TAG, "Total data length (header + data) = %d, crc = %"PRIu32"", data_len, crc_data);
    esp_secure_cert_tlv_footer_t tlv_footer;
    tlv_footer.crc = crc_data;
    memcpy(output_buf + data_len, &tlv_footer, sizeof(esp_secure_cert_tlv_footer_t));
    data_len = data_len + sizeof(esp_secure_cert_tlv_footer_t);
    *output_len = data_len;
    return ESP_OK;
}

esp_err_t esp_secure_cert_append_tlv(esp_secure_cert_tlv_cfg_t *tlv_cfg)
{
    if (tlv_cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err;
    const void *esp_secure_cert_addr = esp_secure_cert_get_mapped_addr();
    /* This variable shall store the end address of the last tlv
     * in esp_secure_cert where the next data shall be written */
    static int tlv_end_offset;
    void *tlv_address = NULL;
    if (tlv_end_offset == 0) {
        err = esp_secure_cert_find_tlv(esp_secure_cert_addr, ESP_SECURE_CERT_TLV_END, &tlv_address);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error in getting the tlv end address");
        }
    }

    err = esp_secure_cert_find_tlv(esp_secure_cert_addr, tlv_cfg->type, &tlv_address);
    if (err == ESP_OK) {
        ESP_LOGE(TAG, "tlv of type %d already exists. There can only be one tlv of any given type in esp_secure_cert partiton", tlv_cfg->type);
        return ESP_FAIL;
    }

    // magic 32 is added because we pad the data to the nearest multupiple of 32
    size_t required_buf_len = tlv_cfg->data_len + sizeof(esp_secure_cert_tlv_header_t) + sizeof(esp_secure_cert_tlv_footer_t) + 32;
    uint8_t *output_buf;
    output_buf = (uint8_t *) calloc(1, (required_buf_len + 1) * sizeof(uint8_t));
    if (output_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for internal buffer");
        return ESP_ERR_NO_MEM;
    }

    size_t output_len = required_buf_len;
    esp_secure_cert_prepare_tlv(tlv_cfg->type, tlv_cfg->data, tlv_cfg->data_len, output_buf, &output_len, tlv_cfg->flags);
    // output_buf should now contain the required tlv structrure that needs to be written to the flash
    // This buffer shall now be written at tlv_end_offset
    ESP_LOGI(TAG, "Writing data at %d offset from base of esp_secure_cert partition, (end of last tlv)", tlv_end_offset);
    err = esp_secure_cert_write_to_flash(tlv_end_offset, output_buf, output_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write the tlv of type %d to flash, returned %04X", tlv_cfg->type, err);
        free(output_buf);
        return ESP_FAIL;
    }
    // Update the tlv_end_offset variable to directly start flashing at this offset at the next flash write operation
    tlv_end_offset = tlv_end_offset + output_len;
    free(output_buf);
    return ESP_OK;
}

#if SOC_HMAC_SUPPORTED
esp_err_t esp_secure_cert_append_tlv_with_hmac_encryption(esp_secure_cert_tlv_cfg_t *tlv_cfg)
{
    if (tlv_cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_secure_cert_tlv_cfg_t *tlv_cfg_hmac_enc = NULL;
    esp_err_t esp_ret = ESP_FAIL;
    int ret = -1;
    esp_efuse_block_t efuse_block = EFUSE_BLK_MAX;
    unsigned char *encrypted_data = NULL;
    ESP_LOGI(TAG, "Encrypting the data with HMAC base encryption");

    if (!esp_efuse_find_purpose(ESP_EFUSE_KEY_PURPOSE_HMAC_UP, &efuse_block)) {
        ESP_LOGE(TAG, "Failed to get the block with purpose set to HMAC_UP");
        return ESP_FAIL;;
    } else {
        ESP_LOGI(TAG , "eFuse block %d found with key purpose set to HMAC_UP", efuse_block);
    }

    uint8_t aes_gcm_key[HMAC_ENCRYPTION_AES_GCM_KEY_LEN];
    uint8_t iv[HMAC_ENCRYPTION_IV_LEN];

    esp_ret = esp_secure_cert_calculate_hmac_encryption_iv(iv);
    if (esp_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to calculate hmac encryption iv");
        return esp_ret;
    }

    esp_ret = esp_secure_cert_calculate_hmac_encryption_key(aes_gcm_key);
    if (esp_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to calculate hmac encryption key");
        return esp_ret;
    }

#if !__has_include(<mbedtls/gcm.h>)
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Failed to initialize PSA Crypto: %d", status);
        return ESP_FAIL;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, HMAC_ENCRYPTION_AES_GCM_KEY_LEN * 8);

    psa_key_id_t key_id = 0;
    status = psa_import_key(&attributes, aes_gcm_key, HMAC_ENCRYPTION_AES_GCM_KEY_LEN, &key_id);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Failed to import key to PSA: %d", status);
        return ESP_FAIL;
    }

    // Allocate buffer for to write the encrypted data
    encrypted_data = (unsigned char*) heap_caps_calloc(1, sizeof(uint8_t) * (tlv_cfg->data_len + HMAC_ENCRYPTION_TAG_LEN) , MALLOC_CAP_INTERNAL);
    if (encrypted_data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate data encryption buffer");
        psa_destroy_key(key_id);
        return ESP_ERR_INVALID_ARG;
    }

    size_t out_len = 0;
    status = psa_aead_encrypt(key_id, PSA_ALG_GCM,
                              iv, HMAC_ENCRYPTION_IV_LEN,
                              NULL, 0,
                              tlv_cfg->data, tlv_cfg->data_len,
                              encrypted_data, tlv_cfg->data_len + HMAC_ENCRYPTION_TAG_LEN,
                              &out_len);

    psa_destroy_key(key_id);

    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "PSA encrypt failed: %d", status);
        free(encrypted_data);
        return ESP_FAIL;
    }
#else
    mbedtls_gcm_context gcm_ctx;
    mbedtls_gcm_init(&gcm_ctx);
    ret = mbedtls_gcm_setkey(&gcm_ctx, MBEDTLS_CIPHER_ID_AES, (unsigned char *)aes_gcm_key, HMAC_ENCRYPTION_AES_GCM_KEY_LEN * 8);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failure at mbedtls_gcm_setkey_enc with error code : -0x%04X", -ret);
        esp_ret = ESP_FAIL;
        goto cleanup;
    }

    // Allocate buffer for to write the encrypted data
    // We shall append the tag to the encrypted data before writing
    // it as a TLV
    encrypted_data = (unsigned char*) heap_caps_calloc(1, sizeof(uint8_t) * (tlv_cfg->data_len + HMAC_ENCRYPTION_TAG_LEN) , MALLOC_CAP_INTERNAL);
    if (encrypted_data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate data encryption buffer");
        esp_ret = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    ret = mbedtls_gcm_crypt_and_tag(&gcm_ctx, MBEDTLS_GCM_ENCRYPT, tlv_cfg->data_len, iv,
                                        HMAC_ENCRYPTION_IV_LEN, NULL, 0, tlv_cfg->data,
                                        encrypted_data, HMAC_ENCRYPTION_TAG_LEN, encrypted_data + tlv_cfg->data_len);

    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to encrypt the data, mbedtls_gcm_crypt_and_tag returned %02X", ret);
        ret = ESP_FAIL;
        goto cleanup;
    }
#endif

    tlv_cfg_hmac_enc = (esp_secure_cert_tlv_cfg_t *) heap_caps_calloc(1, sizeof(esp_secure_cert_tlv_cfg_t), MALLOC_CAP_INTERNAL);
    if (tlv_cfg_hmac_enc == NULL) {
        return ESP_ERR_NO_MEM;
    }

    tlv_cfg_hmac_enc->type = tlv_cfg->type;
    tlv_cfg_hmac_enc->data = encrypted_data;
    tlv_cfg_hmac_enc->data_len = tlv_cfg->data_len + HMAC_ENCRYPTION_TAG_LEN;
    tlv_cfg_hmac_enc->flags = tlv_cfg->flags | ESP_SECURE_CERT_TLV_FLAG_HMAC_ENCRYPTION;

    esp_ret = esp_secure_cert_append_tlv(tlv_cfg_hmac_enc);
    if (esp_ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not write the tlv, returned %04X", esp_ret);
        goto cleanup;
    }

cleanup:
    free(tlv_cfg_hmac_enc);
#if __has_include(<mbedtls/gcm.h>)
    mbedtls_gcm_free(&gcm_ctx);
#endif
    free(encrypted_data);
    return esp_ret;
}
#endif
