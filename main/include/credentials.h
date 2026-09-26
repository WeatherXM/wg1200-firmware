#ifndef PROVISIONING_H
#define PROVISIONING_H

#include "struct.h"
#include "utils.h"
#include "mbedtls/pk.h"

#if !defined(CONFIG_IDF_TARGET_ESP32)
extern "C" {
    #include "esp_secure_cert_read.h"
}
#else
typedef void esp_ds_data_ctx_t;
#endif

namespace Credentials {

	/** Size of the RSA key pair*/
	static const int RSA_KEY_SIZE = 2048;

	/** Serial number length*/
	static const size_t DEV_SERIAL_LENGTH = 10;
	static const size_t DEV_SERIAL_STR_LENGTH = ((2*DEV_SERIAL_LENGTH) + (DEV_SERIAL_LENGTH - 1) + 1); // "XX"digits + ":"count + "\0"

	RetResult init();
	char* get_serial_str();
	size_t get_serial_str_length();
	uint8_t* get_serial();
	size_t get_serial_length();
	char* get_dev_cert();
	uint32_t get_dev_cert_length();
	RetResult get_dev_cert_fingerprint(uint8_t *out_32);
	esp_ds_data_ctx_t* get_ds_ctx();
	RetResult load_private_key_from_nvs(mbedtls_pk_context *pk);
	RetResult load_private_key_raw_from_nvs(uint8_t **out_buf, size_t *out_size);
}

#endif