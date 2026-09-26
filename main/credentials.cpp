#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS

#include "credentials.h"

#if !defined(CONFIG_IDF_TARGET_ESP32)
#include "esp_log.h"
#include "string.h"
#include "esp_ds.h"
#include "esp_efuse.h"
#include "esp_random.h"
#include "esp_tls.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"

extern "C" {
#if __has_include("mbedtls/private/rsa.h")
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/rsa.h"
#include "mbedtls/private/sha256.h"
#include "mbedtls/private/bignum.h"
#include "mbedtls/private/entropy.h"
#include "mbedtls/private/ctr_drbg.h"
#include "mbedtls/private/pk_private.h"
#include "mbedtls/x509_crt.h"
#else
#include "mbedtls/rsa.h"
#include "mbedtls/sha256.h"
#include "mbedtls/bignum.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#endif
#include "mbedtls/platform_util.h"
#include "mbedtls/platform.h"
#include "mbedtls/error.h"
extern const mbedtls_pk_info_t mbedtls_rsa_info;
int mbedtls_rsa_write_pubkey(const mbedtls_rsa_context *rsa, unsigned char *start, unsigned char **p);
int mbedtls_rsa_write_key(const mbedtls_rsa_context *rsa, unsigned char *start, unsigned char **p);
}

extern "C" {
    #include "esp_secure_cert_read.h"
	#include "esp_secure_cert_write.h"
}

#include "lvgl/lvgl.h"
#include "ui/include/ui_common.h"
#include "ui/include/ui_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_efuse_table.h"


namespace Credentials {

///////////////////////////////////////////////////////////////////////////////
// Local constants
///////////////////////////////////////////////////////////////////////////////
const char TAG[] = "Credentials";

/** This should be false after credentials development is finished.*/
static const bool ON_DEVELOPMENT = false;

/** DS parameters settings*/
static const int DS_RSA_LENGTH = (RSA_KEY_SIZE / 32) - 1; // ESP_DS_RSA_X
static const size_t DS_HMAC_LENGTH = 32;
static const size_t DS_IV_LENGTH = 16;

/** Fuse block/key used for device serial number.*/
static const esp_efuse_block_t FUSE_BLOCK_SERIAL = EFUSE_BLK_KEY2;
static const uint16_t KEY_BLOCK_SERIAL = (uint16_t) FUSE_BLOCK_SERIAL - EFUSE_BLK_KEY0;
static const size_t KEY_BLOCK_SERIAL_LENGTH = DEV_SERIAL_LENGTH;
static const esp_efuse_purpose_t KEY_BLOCK_SERIAL_PURP = ESP_EFUSE_KEY_PURPOSE_USER;

/** Fuse block/key used for DS data decryption. This is where the hmac will be stored*/
static const esp_efuse_block_t FUSE_BLOCK_HMAC = EFUSE_BLK_KEY1;
static const uint16_t KEY_BLOCK_HMAC = (uint16_t) FUSE_BLOCK_HMAC - EFUSE_BLK_KEY0;
static const size_t KEY_BLOCK_HMAC_LENGTH = DS_HMAC_LENGTH;
static const esp_efuse_purpose_t KEY_BLOCK_HMAC_PURP = ESP_EFUSE_KEY_PURPOSE_HMAC_DOWN_DIGITAL_SIGNATURE;

/** Size for public key buffer. TODO: Can be optimized*/
static const size_t DEV_PUB_KEY_BUFFER_SIZE = RSA_KEY_SIZE;

/** Size for device certificate buffer. TODO: Can be optimized*/
static const size_t DEV_CERT_BUFFER_SIZE = RSA_KEY_SIZE;

/** Certificate parameters*/
const char DEV_CERT_ISSUER_NAME_FORMAT[] = "C=GR,O=WeatherXM Network,CN=%s,OU=https://weatherxm.network"; // Device serial goes in CN=%s
const char DEV_CERT_VALID_NOT_BEFORE[] = "20171212121212";
const char DEV_CERT_VALID_NOT_AFTER[] = "20521212121212";
const int DEV_CERT_VERSION = MBEDTLS_X509_CRT_VERSION_3;
const mbedtls_md_type_t DEV_CERT_MD_ALG = MBEDTLS_MD_SHA256;

///////////////////////////////////////////////////////////////////////////////
// Local types
///////////////////////////////////////////////////////////////////////////////

/** Struct to make 4096bit calculations*/
typedef struct __attribute__((__packed__))
{
	uint8_t n_4096_bit[512];
} n_4096_bit_t;

/** Struct to check provision status*/
enum class ProvisionStatus
{
	NONE = 0,
	PROVISIONED, 			// Both serial and hmac fuses burned
	UNPROVISIONED,			// No fuse burned. Neither hmac nor serial
	FRAGMENTED				// One of two burned
};

///////////////////////////////////////////////////////////////////////////////
// Local variables
///////////////////////////////////////////////////////////////////////////////

/** Pointer for device certificate loading.
 * Because secure_cert_mgr does not free cert pointer (-.-),
 * we will hold it here and everyone gets it from here.
*/
char *_dev_cert = NULL;

/** Length of loaded device certificate*/
uint32_t _dev_cert_length = 0;

/** Load DS context here once for everyone*/
esp_ds_data_ctx_t *_ds_ctx = NULL;

/** This is where the device serial gets generated/loaded*/
uint8_t EXT_RAM_BSS_ATTR _dev_serial[DEV_SERIAL_LENGTH];

/** This is where the device serial string gets generated/loaded*/
char EXT_RAM_BSS_ATTR _dev_serial_str[DEV_SERIAL_STR_LENGTH] = {'\0'};

///////////////////////////////////////////////////////////////////////////////
// Local functions
///////////////////////////////////////////////////////////////////////////////

void print_blk_keys_status();
ProvisionStatus get_provision_status();
RetResult provision_device();
RetResult  load_dev_credentials();
RetResult gen_rsa_keypair(mbedtls_rsa_context* rsa);
RetResult  gen_dev_serial(mbedtls_pk_context *pk);
RetResult  gen_dev_cert(mbedtls_pk_context* pk, uint8_t *out, size_t size);
void print_mpi(const mbedtls_mpi* mpi, const char* name);
void print_dev_serial_str();
void print_dev_cert(char *dev_cert, uint32_t dev_cert_length);
void print_n4096(char *text, const n_4096_bit_t *n4096);
RetResult calc_rsa_rinv_mprime(const mbedtls_mpi* N, mbedtls_mpi* rinv, uint32_t* mprime);
void reverse_bytes(uint8_t *buff, size_t length);
RetResult config_ds_partition(char *dev_cert, uint32_t dev_cert_length, esp_ds_data_t* ds_data);
RetResult config_enc_ds_params(mbedtls_rsa_context* rsa, esp_ds_data_t* esp_ds_data, uint8_t *hmac);
RetResult conv_rinv_mprime_to_ds_params(const mbedtls_mpi* D, const mbedtls_mpi* N, const mbedtls_mpi* rinv,
                                         uint32_t mprime, esp_ds_p_data_t* params);
template<typename... func_args, typename... param_args>
int try_mbedtls(bool *prev_cur_failed, const char* func_name, int (*func)(func_args ...),param_args&& ... args);
template<typename... func_args, typename... param_args>
RetResult try_retres(bool *prev_cur_failed, const char* func_name, RetResult (*func)(func_args ...),param_args&& ... args);
bool try_ptr(bool *prev_cur_failed, void *ptr, const char* msg);
template<typename... func_args, typename... param_args>
esp_err_t try_espret(bool *prev_cur_failed, const char* func_name, esp_err_t (*func)(func_args ...),param_args&& ... args);
RetResult burn_efuse_key(esp_efuse_block_t block, esp_efuse_purpose_t purpose, uint8_t *data, size_t length, bool dis_read);
RetResult load_dev_cert();
RetResult load_dev_serial();
RetResult load_ds_ctx();
bool is_efuse_key_block_unused(esp_efuse_block_t block);
bool is_virtual_key_block_unused(const char *key_name);
RetResult save_private_key_to_nvs(mbedtls_pk_context *pk);

/******************************************************************************
 * @brief Initialize credentials and provision device if it is needed.
 * @note WARNING! Fuses will be burned if not already provisioned. If you want to force reprovision
 * change block key first.
 ******************************************************************************/
struct ProvisionTaskParam {
	RetResult ret;
	SemaphoreHandle_t sem;
};

static void provision_task(void *pvParameters)
{
	ProvisionTaskParam *param = (ProvisionTaskParam *)pvParameters;
	param->ret = provision_device();
	if (param->sem != NULL) {
		xSemaphoreGive(param->sem);
	}
	vTaskDelete(NULL);
}

static RetResult run_provision_device_in_task()
{
	ProvisionTaskParam param;
	param.ret = RET_ERROR;
	param.sem = xSemaphoreCreateBinary();
	if (param.sem == NULL) {
		ESP_LOGE(TAG, "Failed to create binary semaphore for provisioning task");
		return RET_ERROR;
	}

	// Create provisioning task on App Core (Core 1) with 24KB stack
	BaseType_t res = xTaskCreatePinnedToCore(
		provision_task,
		"prov_task",
		24576, // 24KB stack size
		&param,
		5,     // priority
		NULL,
		1      // core ID
	);

	if (res != pdPASS) {
		ESP_LOGE(TAG, "Failed to create provisioning task");
		vSemaphoreDelete(param.sem);
		return RET_ERROR;
	}

	// Block current task until provisioning completes
	xSemaphoreTake(param.sem, portMAX_DELAY);
	vSemaphoreDelete(param.sem);

	return param.ret;
}

struct KeygenTaskParams {
	SemaphoreHandle_t done_sem;
	bool success;
};

static void generate_credentials_task(void *pvParameters)
{
	ESP_LOGI(TAG, "Entering generate_credentials_task with ample stack...");
	KeygenTaskParams *params = (KeygenTaskParams *)pvParameters;
	bool failed = false;

	mbedtls_pk_context pk;
	mbedtls_pk_init(&pk);

	mbedtls_rsa_context *rsa_ctx = (mbedtls_rsa_context *) heap_caps_malloc(sizeof(mbedtls_rsa_context), MALLOC_CAP_SPIRAM);
	if (rsa_ctx == nullptr) {
		ESP_LOGE(TAG, "Failed to allocate memory for rsa_ctx");
		failed = true;
	} else {
		mbedtls_rsa_init(rsa_ctx);
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
		mbedtls_rsa_set_padding(rsa_ctx, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
#endif
	}

	if (!failed) {
		failed = (gen_rsa_keypair(rsa_ctx) != RET_OK);
	}

	unsigned char *der_buf = (unsigned char *) heap_caps_malloc(1600, MALLOC_CAP_8BIT);
	unsigned char *p = nullptr;
	int der_len = 0;
	if (der_buf == nullptr) {
		ESP_LOGE(TAG, "Failed to allocate memory for der_buf");
		failed = true;
	} else {
		p = der_buf + 1600;
	}

	if (!failed) {
		der_len = mbedtls_rsa_write_key(rsa_ctx, der_buf, &p);
		if (der_len < 0) {
			ESP_LOGE(TAG, "Failed to write legacy RSA key to DER: -0x%x", -der_len);
			failed = true;
		} else {
			ESP_LOGI(TAG, "Successfully wrote legacy RSA key to DER: length = %d", der_len);
		}
	}

	if (!failed) {
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
		int parse_res = mbedtls_pk_parse_key(&pk, p, der_len, NULL, 0);
#else
		int parse_res = mbedtls_pk_parse_key(&pk, p, der_len, NULL, 0, NULL, NULL);
#endif
		if (parse_res != 0) {
			ESP_LOGE(TAG, "Failed to parse RSA key into pk_context: -0x%x", -parse_res);
			failed = true;
		}
	}

	if (der_buf != nullptr) {
		heap_caps_free(der_buf);
	}

	uint8_t *new_dev_cert = (uint8_t*) heap_caps_malloc(DEV_CERT_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
	uint32_t new_dev_cert_length = 0;
	if (new_dev_cert == nullptr) {
		failed = true;
	} else {
		memset(new_dev_cert, '\0', DEV_CERT_BUFFER_SIZE);
	}

	if (!failed) {
		failed = (gen_dev_cert(&pk, new_dev_cert, DEV_CERT_BUFFER_SIZE) != RET_OK);
	}

	if (!failed) {
		new_dev_cert_length = strnlen((char*) new_dev_cert, DEV_CERT_BUFFER_SIZE) + 1;
		failed = (save_private_key_to_nvs(&pk) != RET_OK);

		if (!failed) {
			nvs_handle_t my_handle;
			if (nvs_open("wxm_virt", NVS_READWRITE, &my_handle) == ESP_OK) {
				if (nvs_set_blob(my_handle, "virt_dev_cert", new_dev_cert, new_dev_cert_length) == ESP_OK) {
					nvs_commit(my_handle);
					ESP_LOGI(TAG, "Successfully saved virtual certificate to NVS in keygen task.");
				} else {
					failed = true;
				}
				nvs_close(my_handle);
			} else {
				failed = true;
			}
		}
	}

	if (new_dev_cert != nullptr) {
		heap_caps_free(new_dev_cert);
	}

	if (rsa_ctx != nullptr) {
		mbedtls_rsa_free(rsa_ctx);
		heap_caps_free(rsa_ctx);
	}
	mbedtls_pk_free(&pk);

	params->success = !failed;
	ESP_LOGI(TAG, "Keygen task finishing. Success: %s", params->success ? "true" : "false");

	if (params->done_sem != NULL) {
		xSemaphoreGive(params->done_sem);
	}
	vTaskDelete(NULL);
}

RetResult init()
{
	RetResult ret = RET_ERROR;

	print_blk_keys_status();

	ProvisionStatus prov_status = get_provision_status();

	if(prov_status == ProvisionStatus::UNPROVISIONED)
	{
		ESP_LOGI(TAG, "Device is not provisioned");		
		ret = run_provision_device_in_task();
		if(ret != RET_OK)
		{
			ESP_LOGE(TAG, "Provisioning failed! Falling back to mock credentials for testing...");
			_dev_serial[0] = 0x00; _dev_serial[1] = 0x11; _dev_serial[2] = 0x22;
			_dev_serial[3] = 0x33; _dev_serial[4] = 0x44; _dev_serial[5] = 0x55;
			_dev_serial[6] = 0x66; _dev_serial[7] = 0x77; _dev_serial[8] = 0x88;
			_dev_serial[9] = 0x99;
			strcpy(_dev_serial_str, "00:11:22:33:44:55:66:77:88:99");
			_dev_cert = (char *)"";
			_dev_cert_length = 0;
			_ds_ctx = NULL;
			ret = RET_OK;
		}
		else
		{
			ESP_LOGW(TAG, "Provisioning finished. Restarting...");
			Utils::restart(Utils::RestartReason::CREDENTIAL_PROVISIONING);
		}
	}
	else if(prov_status == ProvisionStatus::PROVISIONED)
	{
		ESP_LOGI(TAG, "Device is already provisioned");
		ret = load_dev_credentials();
		if (ret != RET_OK)
		{
#ifndef CONFIG_WXM_PRODUCTION_BURN_EFUSES
			ESP_LOGW(TAG, "Loading credentials failed in Development. Erasing virtual keys and secure cert partition to force reprovisioning...");
			nvs_handle_t my_handle;
			if (nvs_open("wxm_virt", NVS_READWRITE, &my_handle) == ESP_OK) {
				nvs_erase_key(my_handle, "virt_serial");
				nvs_erase_key(my_handle, "virt_hmac");
				nvs_commit(my_handle);
				nvs_close(my_handle);
			}
			esp_secure_cert_erase_partition();
			ESP_LOGW(TAG, "Erase complete. Restarting device to trigger fresh provisioning...");
			vTaskDelay(pdMS_TO_TICKS(1000));
			Utils::restart(Utils::RestartReason::CREDENTIAL_PROVISIONING);
#else
			ESP_LOGE(TAG, "Loading credentials failed! Falling back to mock credentials for testing...");
			_dev_serial[0] = 0x00; _dev_serial[1] = 0x11; _dev_serial[2] = 0x22;
			_dev_serial[3] = 0x33; _dev_serial[4] = 0x44; _dev_serial[5] = 0x55;
			_dev_serial[6] = 0x66; _dev_serial[7] = 0x77; _dev_serial[8] = 0x88;
			_dev_serial[9] = 0x99;
			strcpy(_dev_serial_str, "00:11:22:33:44:55:66:77:88:99");
			_dev_cert = (char *)"";
			_dev_cert_length = 0;
			_ds_ctx = NULL;
			ret = RET_OK;
#endif
		}
	}
	else if(prov_status == ProvisionStatus::FRAGMENTED)
	{
		ESP_LOGE(TAG, "Provisioning is fragmented!");
		if(ON_DEVELOPMENT)
		{
			ret = run_provision_device_in_task(); // Try to recover
		}
		
		if (ret != RET_OK)
		{
			ESP_LOGW(TAG, "Bypassing fragmented provisioning. Using mock credentials for testing...");
			_dev_serial[0] = 0x00; _dev_serial[1] = 0x11; _dev_serial[2] = 0x22;
			_dev_serial[3] = 0x33; _dev_serial[4] = 0x44; _dev_serial[5] = 0x55;
			_dev_serial[6] = 0x66; _dev_serial[7] = 0x77; _dev_serial[8] = 0x88;
			_dev_serial[9] = 0x99;
			strcpy(_dev_serial_str, "00:11:22:33:44:55:66:77:88:99");
			_dev_cert = (char *)"";
			_dev_cert_length = 0;
			_ds_ctx = NULL;
			ret = RET_OK;
		}
	}

	return ret;
}

/******************************************************************************
 * @brief Provision device. Geneate RSA keys, certificate, serial, DS params and burn efuses.
 ******************************************************************************/
RetResult provision_device()
{
	ESP_LOGI(TAG, "Provisioning device");

#ifndef CONFIG_SCREENLESS
	UiCommon::lock();

	lv_obj_t *old_scr = lv_scr_act();
	lv_obj_t *base_obj = lv_obj_create(NULL);
	lv_scr_load(base_obj);
	if(old_scr != NULL && old_scr != base_obj)
	{
		lv_obj_del_async(old_scr);
	}

	UiTypes::LabelComponent text_lbl;

	text_lbl.create(base_obj)
		.font(&lv_font_montserrat_20)
		.color(UI_TEXT_COLOR_LIGHT)
		.text("Provisioning device, please wait...")
		.align(LV_ALIGN_TOP_LEFT, 20, 20);

	UiCommon::unlock();
#endif

	bool failed = false;
	int ret = 0;	
    
    esp_ds_data_t* ds_encrypted = NULL;   

	// 1. Manually allocate and initialize a software-based RSA context
	mbedtls_rsa_context *rsa_ctx = (mbedtls_rsa_context *) heap_caps_malloc(sizeof(mbedtls_rsa_context), MALLOC_CAP_SPIRAM);
	if (rsa_ctx == nullptr) {
		ESP_LOGE(TAG, "Failed to allocate memory for rsa_ctx");
		failed = true;
	} else {
		mbedtls_rsa_init(rsa_ctx);
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
		mbedtls_rsa_set_padding(rsa_ctx, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
#endif
	}

	// 2. Generate RSA key-pair inside the software context
	if (!failed) {
		try_retres(&failed, "gen_rsa_keypair", gen_rsa_keypair, rsa_ctx);
	}

	// 3. Write the generated RSA key-pair to a legacy DER private key buffer
	unsigned char *der_buf = (unsigned char *) heap_caps_malloc(1600, MALLOC_CAP_8BIT);
	unsigned char *p = nullptr;
	int der_len = 0;
	if (der_buf == nullptr) {
		ESP_LOGE(TAG, "Failed to allocate memory for der_buf");
		failed = true;
	} else {
		p = der_buf + 1600;
	}

	if (!failed) {
		der_len = mbedtls_rsa_write_key(rsa_ctx, der_buf, &p);
		if (der_len < 0) {
			ESP_LOGE(TAG, "Failed to write legacy RSA key to DER: -0x%x", -der_len);
			failed = true;
		} else {
			ESP_LOGI(TAG, "Successfully wrote legacy RSA key to DER: length = %d", der_len);
		}
	}

	// 4. Initialize and parse into mbedtls_pk_context (which will import it into PSA)
	mbedtls_pk_context pk;
	mbedtls_pk_init(&pk);
	if (!failed) {
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
		int parse_res = mbedtls_pk_parse_key(&pk, p, der_len, NULL, 0);
#else
		int parse_res = mbedtls_pk_parse_key(&pk, p, der_len, NULL, 0, NULL, NULL);
#endif
		if (parse_res != 0) {
			ESP_LOGE(TAG, "Failed to parse/import RSA key into pk_context: -0x%x", -parse_res);
			failed = true;
		} else {
			ESP_LOGI(TAG, "Successfully imported generated RSA key into PSA-backed pk_context");
		}
	}

	// Free der_buf as it is no longer needed after parsing/importing into pk_context
	if (der_buf != nullptr) {
		heap_caps_free(der_buf);
		der_buf = nullptr;
	}

	// Generate device serial number. Serial number = bytes(sha256(pub_key_der))[0...10]
	try_retres(&failed, "gen_dev_serial", gen_dev_serial, &pk);
	if(!failed && ON_DEVELOPMENT){
		print_dev_serial_str();
	}

	// Generate device certificate
	uint8_t *dev_cert = (uint8_t*) heap_caps_malloc(DEV_CERT_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
	uint32_t dev_cert_length = 0;
	try_ptr(&failed, dev_cert, "dev_cert_allocation");
	if(!failed)
	{
		memset(dev_cert, '\0', DEV_CERT_BUFFER_SIZE);
	}
	try_retres(&failed, "gen_dev_cert",  gen_dev_cert, &pk, (uint8_t*)dev_cert, DEV_CERT_BUFFER_SIZE);
	if(!failed){
		// Include null terminator in the length so mbedtls can parse the PEM cert on boot
		dev_cert_length = strnlen((char*) dev_cert, DEV_CERT_BUFFER_SIZE) + 1;
		if(ON_DEVELOPMENT)
		{
			print_dev_cert((char *) dev_cert, dev_cert_length);
		}
	}

	// Configure DS parameters from private key and encrypt them using HMAC
	uint8_t hmac[DS_HMAC_LENGTH];
	ds_encrypted = (esp_ds_data_t*) heap_caps_malloc(sizeof(esp_ds_data_t), MALLOC_CAP_SPIRAM);
	try_ptr(&failed, ds_encrypted, "ds_encrypted_allocation");	
	if (!failed) {
		try_retres(&failed, "config_enc_ds_params", config_enc_ds_params, rsa_ctx, ds_encrypted, hmac);
	}

	// Configure DS partition. Generate TLVs of DEV_CERT, DS_PARAMS, DS_CTX and write them to DS partition.
	try_retres(&failed, "config_ds_partition", config_ds_partition, (char *) dev_cert, dev_cert_length, ds_encrypted);

	// Free rsa_ctx as it is no longer needed
	if (rsa_ctx != nullptr) {
		mbedtls_rsa_free(rsa_ctx);
		heap_caps_free(rsa_ctx);
		rsa_ctx = nullptr;
	}

#ifndef CONFIG_WXM_PRODUCTION_BURN_EFUSES
	// In Safe Mock-eFuse Mode, save generated private key to NVS for software-based signing fallback
	if (!failed) {
		try_retres(&failed, "save_private_key_to_nvs", save_private_key_to_nvs, &pk);
	}
#endif

	// In order to end the provisioning at the same state of just loading the credentials,
	// we will load the dev cert and DS ctx. Device will be restarted after provioning but anyway...
	try_retres(&failed, "load_dev_cert", load_dev_cert);
	if(!failed && ON_DEVELOPMENT)
	{
		// Just to check that generated device certificate and ds_storage written certificate match.
		print_dev_cert(_dev_cert, _dev_cert_length);
	}
	try_retres(&failed, "load_ds_ctx", load_ds_ctx);

	// Burn device serial saved in _dev_serial byte array to key FUSE_BLOCK_SERIAL.
	try_retres(&failed, "burn_serial_key", burn_efuse_key, 
					FUSE_BLOCK_SERIAL, KEY_BLOCK_SERIAL_PURP, _dev_serial, KEY_BLOCK_SERIAL_LENGTH, false);

	// Burn HMAC to key FUSE_BLOCK_HMAC. Read protect is automatically enabled for HMAC_SIG type keys.
	try_retres(&failed, "burn_hmac_key", burn_efuse_key, 
					FUSE_BLOCK_HMAC, KEY_BLOCK_HMAC_PURP, hmac, KEY_BLOCK_HMAC_LENGTH, false);

	// Disable SECURE_BOOT_V2_ALLOW_EFUSE_RD_DIS which allowed us to disable_read in hmac_key.
	try_espret(&failed, "efuse_dis_rd_dis", esp_efuse_write_field_bit, ESP_EFUSE_WR_DIS_RD_DIS);	

    mbedtls_pk_free(&pk);
    heap_caps_free(ds_encrypted);
	heap_caps_free(dev_cert);

#ifndef CONFIG_SCREENLESS
	UiCommon::lock();
	if(!failed)
	{
		text_lbl.text("Provisioning device SUCCESS!");
	}
	else
	{
		text_lbl.text("Provisioning device FAILED!");
	}
	UiCommon::unlock();
	vTaskDelay(pdMS_TO_TICKS(2000));
#endif

	return failed ? RET_ERROR : RET_OK;
}

/******************************************************************************
 * @brief Check if device is already provisioned by looking if the HMAC keyblock is used.
 ******************************************************************************/
ProvisionStatus get_provision_status()
{	
	ProvisionStatus status = ProvisionStatus::NONE;
	if(is_efuse_key_block_unused(FUSE_BLOCK_HMAC) && is_efuse_key_block_unused(FUSE_BLOCK_SERIAL))
	{
		status = ProvisionStatus::UNPROVISIONED;
	}
	else if(is_efuse_key_block_unused(FUSE_BLOCK_HMAC) != is_efuse_key_block_unused(FUSE_BLOCK_SERIAL))
	{
		status = ProvisionStatus::FRAGMENTED;
	}
	else
	{
		status = ProvisionStatus::PROVISIONED;
	}
	return status;
}

/******************************************************************************
 * @brief Burn efuse key.
 * @note WARNING! If dis_read is true, key won't be readable.
 ******************************************************************************/
RetResult burn_efuse_key(esp_efuse_block_t block, esp_efuse_purpose_t purpose, uint8_t *data, size_t length, bool dis_read)
{
	bool failed = false;

	if(!is_efuse_key_block_unused(block))
	{
		ESP_LOGE(TAG, "Error, BLK_KEY%d is already used!", (uint16_t)(block - EFUSE_BLK_KEY0));
		return RET_ERROR;
	}

#ifdef CONFIG_WXM_PRODUCTION_BURN_EFUSES
	try_espret(&failed, "burn_efuse_key", esp_efuse_write_key, block, purpose, data, length);

	if(dis_read)
	{
		try_espret(&failed, "disable_key_read", esp_efuse_set_key_dis_read, block);
	}
#else
	ESP_LOGI(TAG, "Bypassing hardware eFuse burn (Safe Mock-eFuse Mode active)");
	nvs_handle_t my_handle;
	esp_err_t err = nvs_open("wxm_virt", NVS_READWRITE, &my_handle);
	if (err != ESP_OK) {
		failed = true;
	} else {
		const char *key_name = (block == FUSE_BLOCK_SERIAL) ? "virt_serial" : "virt_hmac";
		err = nvs_set_blob(my_handle, key_name, data, length);
		if (err == ESP_OK) {
			err = nvs_commit(my_handle);
		}
		nvs_close(my_handle);
		if (err != ESP_OK) {
			failed = true;
		}
	}
#endif

	return failed ? RET_ERROR : RET_OK;
}

/******************************************************************************
 * @brief Generate device serial number.
 * @param pk Pointer to mbedtls_pk_context.
 ******************************************************************************/
RetResult  gen_dev_serial(mbedtls_pk_context *pk)
{
	bool failed = false;

	mbedtls_rsa_context *rsa_ctx = mbedtls_pk_rsa(*pk);
	if (rsa_ctx != nullptr) {
		ESP_LOGI(TAG, "Diagnostic: RSA Modulus N bits: %d, Exponent E bits: %d", 
				 (int)mbedtls_mpi_bitlen(&rsa_ctx->private_N), 
				 (int)mbedtls_mpi_bitlen(&rsa_ctx->private_E));
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
		// For mbedTLS v3 under PSA, we must populate private_pub_raw and private_pub_raw_len 
		// so that pk_write_rsa_pubkey called by mbedtls_pk_write_pubkey_der has a valid key_ptr and key_len.
		unsigned char *p = pk->private_pub_raw + sizeof(pk->private_pub_raw);
		int rsa_pub_len = mbedtls_rsa_write_pubkey(rsa_ctx, pk->private_pub_raw, &p);
		if (rsa_pub_len > 0) {
			memmove(pk->private_pub_raw, p, rsa_pub_len);
			pk->private_pub_raw_len = rsa_pub_len;
			ESP_LOGI(TAG, "Diagnostic: Populated private_pub_raw with length %d", rsa_pub_len);
		} else {
			ESP_LOGE(TAG, "Diagnostic: mbedtls_rsa_write_pubkey returned error %d", rsa_pub_len);
			failed = true;
		}
#endif
	} else {
		ESP_LOGI(TAG, "Diagnostic: RSA context is NULL in gen_dev_serial (using standard PSA-backed pk_context).");
	}

	uint8_t *pub_key_der = (uint8_t*) heap_caps_malloc(DEV_PUB_KEY_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	try_ptr(&failed, pub_key_der, "PUB_KEY_allocation");
	if(!failed)
	{
		memset(pub_key_der, 0, DEV_PUB_KEY_BUFFER_SIZE);
	}

	size_t pub_key_length = try_mbedtls(&failed, "mbedtls_pk_write_pubkey_der", mbedtls_pk_write_pubkey_der, pk, pub_key_der, DEV_PUB_KEY_BUFFER_SIZE);

	// Pub_key is written to the bottom of the buff.
	uint8_t *pub_key_start = &pub_key_der[0];
	uint8_t *pub_key_end = &pub_key_der[DEV_PUB_KEY_BUFFER_SIZE - 1];
	if(!failed)
	{
		pub_key_start = &pub_key_der[DEV_PUB_KEY_BUFFER_SIZE - pub_key_length];
		// printf("Device Pub Key DER, length:%d:\n", pub_key_length);
		// Utils::print_buff_hex(pub_key_start, pub_key_length);
	}

	uint8_t hash[32] = "";
	try_mbedtls(&failed, "pub_key_hash", mbedtls_sha256, pub_key_start, pub_key_length, hash, 0);
	if(!failed)
	{
		// printf("Device Pub Key sha256 hash:\n");
		// Utils::print_buff_hex(hash, 32);

		memcpy(_dev_serial, hash, sizeof(_dev_serial));
		// printf("Device serial number bytes:\n");
		// Utils::print_buff_hex(_dev_serial, sizeof(_dev_serial));

		uint8_t append_colon = 1;
		// TODO: Check for overflow
		for(int i = 0; i < sizeof(_dev_serial); i++)
		{
			if(i == sizeof(_dev_serial) - 1) append_colon = 0;
			sprintf(_dev_serial_str + i * 3,"%02X%.*s", _dev_serial[i], append_colon, ":");
		}
		
		_dev_serial_str[sizeof(_dev_serial_str) - 1]= '\0';// Just to be sure
	}
	
	heap_caps_free(pub_key_der);

	return failed ? RET_ERROR : RET_OK;
}

/******************************************************************************
 * @brief Load device certificate from DS storage.
 ******************************************************************************/
RetResult load_dev_cert()
{
	bool failed = false;
	try_espret(&failed, "esp_secure_cert_get_device_cert", esp_secure_cert_get_device_cert, &_dev_cert, &_dev_cert_length);
	if (!failed) {
		if (_dev_cert_length == 0 || _dev_cert == NULL) {
			ESP_LOGE(TAG, "load_dev_cert: certificate is empty or NULL");
			return RET_ERROR;
		}
		// Validate that the certificate is null-terminated.
		// Mbed TLS requires PEM certificate string length parameter to include the null terminator
		// and the buffer itself to contain a null-terminated string.
		if (_dev_cert[_dev_cert_length - 1] != '\0') {
			if (_dev_cert[_dev_cert_length] == '\0') {
				ESP_LOGI(TAG, "load_dev_cert: Certificate has implicit null terminator at index %u, adjusting length.", (unsigned int)_dev_cert_length);
				_dev_cert_length++;
			} else {
				ESP_LOGW(TAG, "load_dev_cert: Stored certificate is NOT null-terminated! Stored length: %u", (unsigned int)_dev_cert_length);
				return RET_ERROR;
			}
		}
	}
	return failed ? RET_ERROR : RET_OK;
}

/******************************************************************************
 * @brief Load device serial from efuse.
 ******************************************************************************/
RetResult load_dev_serial()
{
	bool failed = false;

	if(is_efuse_key_block_unused(FUSE_BLOCK_SERIAL))
	{
		ESP_LOGE(TAG, "Tried to load serial but key is empty.");
		return RET_ERROR;
	}

	if(esp_efuse_get_key_purpose(FUSE_BLOCK_SERIAL) != KEY_BLOCK_SERIAL_PURP)
	{
#ifdef CONFIG_WXM_PRODUCTION_BURN_EFUSES
		ESP_LOGE(TAG, "Tried to load serial but key purpose doesn't match.");
		return RET_ERROR;
#else
		ESP_LOGW(TAG, "Key purpose mismatch bypassed in Safe Mock-eFuse Mode.");
#endif
	}

	if(esp_efuse_get_key_dis_read(FUSE_BLOCK_SERIAL))
	{
#ifdef CONFIG_WXM_PRODUCTION_BURN_EFUSES
		ESP_LOGE(TAG, "Tried to load serial but but read is disabled.");
		return RET_INVALID_STATE;
#else
		ESP_LOGW(TAG, "Key read-protection bypass active in Safe Mock-eFuse Mode.");
#endif
	}

#ifdef CONFIG_WXM_PRODUCTION_BURN_EFUSES
	try_espret(&failed, "retrieve_serial_blob", esp_efuse_read_field_blob, esp_efuse_get_key(FUSE_BLOCK_SERIAL), _dev_serial, DEV_SERIAL_LENGTH * 8);
#else
	nvs_handle_t my_handle;
	esp_err_t err = nvs_open("wxm_virt", NVS_READONLY, &my_handle);
	if (err != ESP_OK) {
		failed = true;
	} else {
		size_t len = DEV_SERIAL_LENGTH;
		err = nvs_get_blob(my_handle, "virt_serial", _dev_serial, &len);
		nvs_close(my_handle);
		if (err != ESP_OK || len != DEV_SERIAL_LENGTH) {
			failed = true;
		}
	}
#endif

	if(!failed)
	{
		uint8_t append_colon = 1;
		// TODO: Check for overflow
		for(int i = 0; i < sizeof(_dev_serial); i++)
		{
			if(i == sizeof(_dev_serial) - 1) append_colon = 0;
			sprintf(_dev_serial_str + i * 3,"%02X%.*s", _dev_serial[i], append_colon, ":");
		}
		
		_dev_serial_str[sizeof(_dev_serial_str) - 1]= '\0';// Just to be sure
	}

	return failed ? RET_ERROR : RET_OK;
}

/******************************************************************************
 * @brief Load device DS context here for everyone that wants to use it.
 * @note This context won't be freed. Everyone use it from here so fragmentation is minimized.
 ******************************************************************************/
RetResult load_ds_ctx()
{
	_ds_ctx = esp_secure_cert_get_ds_ctx();
    if (_ds_ctx == NULL) {
        return RET_ERROR;
    }
	return RET_OK;
}

/******************************************************************************
 * @brief Load device serial number from FUSE_BLOCK_SERIAL and certificate from DS storage.
 ******************************************************************************/
RetResult load_dev_credentials()
{
	bool failed = false;

	try_retres(&failed, "load_dev_serial", load_dev_serial);
	if (failed) {
		ESP_LOGE(TAG, "Failed to load device serial");
		return RET_ERROR;
	}

	RetResult cert_res = load_dev_cert();
	RetResult ds_res = load_ds_ctx();

	if (cert_res != RET_OK || ds_res != RET_OK) {
		ESP_LOGW(TAG, "Hardware secure cert loading failed (Cert: %d, DS: %d). Falling back to software virtual credentials...", cert_res, ds_res);

		_ds_ctx = NULL; // No DS context for software fallback

		nvs_handle_t my_handle;
		esp_err_t err = nvs_open("wxm_virt", NVS_READWRITE, &my_handle);
		if (err == ESP_OK) {
			size_t cert_len = 0;
			size_t key_len = 0;
			err = nvs_get_blob(my_handle, "virt_dev_cert", NULL, &cert_len);
			esp_err_t key_err = nvs_get_blob(my_handle, "virt_priv_key", NULL, &key_len);
			if (err == ESP_OK && cert_len > 0 && key_err == ESP_OK && key_len > 0) {
				char *buf = (char *)malloc(cert_len);
				if (buf && nvs_get_blob(my_handle, "virt_dev_cert", buf, &cert_len) == ESP_OK) {
					_dev_cert = buf;
					_dev_cert_length = cert_len;
					ESP_LOGI(TAG, "Successfully loaded virtual cert from NVS (length: %d)", (int)_dev_cert_length);
				} else {
					if (buf) free(buf);
					failed = true;
				}
				nvs_close(my_handle);
			} else {
				// Virtual credentials do not exist, generate them!
				nvs_close(my_handle);
				ESP_LOGI(TAG, "Virtual credentials not found in NVS. Spawning separate task to generate brand-new software keypair and cert...");

				SemaphoreHandle_t keygen_sem = xSemaphoreCreateBinary();
				if (keygen_sem == NULL) {
					ESP_LOGE(TAG, "Failed to create semaphore for keygen task");
					return RET_ERROR;
				}

				KeygenTaskParams params = {
					.done_sem = keygen_sem,
					.success = false
				};

				// Create the keygen task with 24KB stack (same as provisioning task) on Core 1 (App core) to be absolutely safe
				BaseType_t task_created = xTaskCreatePinnedToCore(
					generate_credentials_task,
					"wxm_keygen",
					24576,
					&params,
					5,
					NULL,
					1
				);

				if (task_created != pdPASS) {
					ESP_LOGE(TAG, "Failed to create wxm_keygen task");
					vSemaphoreDelete(keygen_sem);
					return RET_ERROR;
				}

				// Wait for the keygen task to signal completion
				xSemaphoreTake(keygen_sem, portMAX_DELAY);
				vSemaphoreDelete(keygen_sem);

				if (!params.success) {
					ESP_LOGE(TAG, "Keygen task failed to generate virtual credentials");
					return RET_ERROR;
				}

				// Keygen task successfully generated credentials and saved them to NVS!
				// Now let's open NVS again and load them.
				err = nvs_open("wxm_virt", NVS_READWRITE, &my_handle);
				if (err == ESP_OK) {
					err = nvs_get_blob(my_handle, "virt_dev_cert", NULL, &cert_len);
					if (err == ESP_OK && cert_len > 0) {
						char *buf = (char *)malloc(cert_len);
						if (buf && nvs_get_blob(my_handle, "virt_dev_cert", buf, &cert_len) == ESP_OK) {
							_dev_cert = buf;
							_dev_cert_length = cert_len;
							ESP_LOGI(TAG, "Successfully loaded freshly generated virtual cert from NVS (length: %d)", (int)_dev_cert_length);
						} else {
							if (buf) free(buf);
							failed = true;
						}
					} else {
						failed = true;
					}
					nvs_close(my_handle);
				} else {
					failed = true;
				}
			}
		} else {
			failed = true;
		}
	}

	return failed ? RET_ERROR : RET_OK;
}

static int custom_entropy_func(void *data, unsigned char *output, size_t len)
{
	esp_fill_random(output, len);
	return 0;
}

/******************************************************************************
 * @brief Generate device certificate.
 * @param pk Pointer to mbedtls_pk_context.
 * @param out Pointer to out buffer.
 * @param size Buffer size.
 ******************************************************************************/
RetResult  gen_dev_cert(mbedtls_pk_context* pk, uint8_t *out, size_t size)
{
	bool failed = false;
	int fail_ret = 0;
	ESP_LOGI(TAG, "Generating Device Certificate, please wait...");

	// the personalize string just adds a litte
	// bit of "guarenteed" entropy, as a backup.
	const char *personalize = "WXM_WG1200";

	// Disable Idle Task WDT
	// keygen takes a long time.
	if(Utils::set_idle_task_watchdog_state(false) != RET_OK)
	{
		ESP_LOGE(TAG, "Cannot disable idle task wdt");
		return RET_ERROR;
	}

	mbedtls_ctr_drbg_context *ctr_drbg = (mbedtls_ctr_drbg_context *) heap_caps_malloc(sizeof(mbedtls_ctr_drbg_context), MALLOC_CAP_8BIT);
	mbedtls_x509write_cert *crt = (mbedtls_x509write_cert *) heap_caps_malloc(sizeof(mbedtls_x509write_cert), MALLOC_CAP_8BIT);

	if (ctr_drbg == nullptr || crt == nullptr) {
		ESP_LOGE(TAG, "Failed to allocate memory for ctr_drbg or crt");
		if (ctr_drbg != nullptr) heap_caps_free(ctr_drbg);
		if (crt != nullptr) heap_caps_free(crt);
		return RET_ERROR;
	}

	mbedtls_ctr_drbg_init(ctr_drbg);
	mbedtls_x509write_crt_init(crt);

	try_mbedtls(&failed, "mbedtls_ctr_drbg_seed", mbedtls_ctr_drbg_seed, ctr_drbg, custom_entropy_func, nullptr,
							(uint8_t*) personalize, strlen(personalize));

	mbedtls_x509write_crt_set_subject_key(crt, pk);
	mbedtls_x509write_crt_set_issuer_key(crt, pk);
	char buff[100] = {'\0'};
	snprintf(buff, sizeof(buff), DEV_CERT_ISSUER_NAME_FORMAT, _dev_serial_str);
	try_mbedtls(&failed, "mbedtls_x509write_crt_set_subject_name", mbedtls_x509write_crt_set_subject_name, 
				crt, buff);
	try_mbedtls(&failed, "mbedtls_x509write_crt_set_issuer_name", mbedtls_x509write_crt_set_issuer_name, 
				crt, buff);
	mbedtls_x509write_crt_set_version(crt, DEV_CERT_VERSION);	
	mbedtls_x509write_crt_set_md_alg(crt, DEV_CERT_MD_ALG);
	// TODO: Needs char or byte?
	uint8_t buff_cert_serial[20];
	esp_fill_random(buff_cert_serial, sizeof(buff_cert_serial));
	// printf("Cert Serial:\n");
	// Utils::print_buff_hex(buff_cert_serial, sizeof(buff_cert_serial));
	try_mbedtls(&failed, "mbedtls_x509write_crt_set_serial_raw", mbedtls_x509write_crt_set_serial_raw, 
				crt, buff_cert_serial, sizeof(buff_cert_serial));
	try_mbedtls(&failed, "mbedtls_x509write_crt_set_validity", mbedtls_x509write_crt_set_validity, 
				crt, DEV_CERT_VALID_NOT_BEFORE, DEV_CERT_VALID_NOT_AFTER);
	try_mbedtls(&failed, "mbedtls_x509write_crt_set_basic_constraints", mbedtls_x509write_crt_set_basic_constraints, 
				crt, 0, -1);
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
	try_mbedtls(&failed, "mbedtls_x509write_crt_pem", mbedtls_x509write_crt_pem, 
				crt, out, size);	
#else
	try_mbedtls(&failed, "mbedtls_x509write_crt_pem", mbedtls_x509write_crt_pem, 
				crt, out, size, mbedtls_ctr_drbg_random, ctr_drbg);	
#endif

	if(Utils::set_idle_task_watchdog_state(true) != RET_OK)
	{
		ESP_LOGE(TAG, "Cannot re-enable idle task wdt");
		failed = true;
	}

	mbedtls_x509write_crt_free(crt);
	mbedtls_ctr_drbg_free(ctr_drbg);
	heap_caps_free(crt);
	heap_caps_free(ctr_drbg);
	return failed ? RET_ERROR : RET_OK;
}

/******************************************************************************
 * @brief Generate RSA keypair length of RSA_KEY_SIZE.
 * @param rsa Pointer to mbedtls_rsa_context.
 ******************************************************************************/
RetResult gen_rsa_keypair(mbedtls_rsa_context* rsa)
{
	if (rsa == nullptr) {
		ESP_LOGE(TAG, "gen_rsa_keypair received a NULL rsa context!");
		return RET_ERROR;
	}
	bool failed = false;
	ESP_LOGI(TAG, "Generating RSA key-pair, please wait...");

	// Disable Idle Task WDT because keygen takes a long time.
	if(Utils::set_idle_task_watchdog_state(false) != RET_OK)
	{
		ESP_LOGE(TAG, "Cannot disable idle task wdt");
		return RET_ERROR;
	}

	mbedtls_ctr_drbg_context ctrDrbg;
	mbedtls_ctr_drbg_init(&ctrDrbg);

	const char *personalize = "WXM_WG1200";
	try_mbedtls(&failed, "mbedtls_ctr_drbg_seed", mbedtls_ctr_drbg_seed, 
				&ctrDrbg, custom_entropy_func, nullptr, (uint8_t*) personalize, strlen(personalize));
	try_mbedtls(&failed, "mbedtls_rsa_gen_key", mbedtls_rsa_gen_key, 
				rsa, mbedtls_ctr_drbg_random, &ctrDrbg, RSA_KEY_SIZE, 65537);
	try_mbedtls(&failed, "mbedtls_rsa_complete", mbedtls_rsa_complete, rsa);
	if(!failed)
	{
		ESP_LOGI(TAG, "Generating RSA key-pair completed");
	}

	mbedtls_ctr_drbg_free(&ctrDrbg);

	// Re-enable Idle Task WDT.
	if(Utils::set_idle_task_watchdog_state(true) != RET_OK)
	{
		ESP_LOGE(TAG, "Cannot re-enable idle task wdt");
		return RET_ERROR;
	}

	return failed ? RET_ERROR : RET_OK;
}

/******************************************************************************
 * @brief Calculate rinv and mprime parameters.
 * @param N Input - Rsa private_N parameter.
 * @param rivv Output - R inverted parameter.
 * @param mprime Output - M prime parameter.
 ******************************************************************************/
RetResult calc_rsa_rinv_mprime(const mbedtls_mpi* N, mbedtls_mpi* rinv, uint32_t* mprime)
{
	bool failed = false;
	mbedtls_mpi rr, ls32, a;
	mbedtls_mpi_init(&rr);
	mbedtls_mpi_init(&ls32);
	mbedtls_mpi_init(&a);
	
	ESP_LOGI(TAG, "Calculate rinv, mprime");

	try_mbedtls(&failed, "mbedtls_mpi_lset_rr", mbedtls_mpi_lset, &rr, 1);
	try_mbedtls(&failed, "mbedtls_mpi_shift_l_rr", mbedtls_mpi_shift_l, &rr, RSA_KEY_SIZE * 2);
	try_mbedtls(&failed, "mbedtls_mpi_mod_mpi_rinv", mbedtls_mpi_mod_mpi, rinv, &rr, N); // rinv = rr % rsa.N 
	try_mbedtls(&failed, "mbedtls_mpi_lset_ls32", mbedtls_mpi_lset, &ls32, 1); // ls32 = 1 << 32
	try_mbedtls(&failed, "mbedtls_mpi_shift_l_ls32", mbedtls_mpi_shift_l, &ls32, 32);
	
	// Modulo reduction of N before inv_mod in mbedTLS v3 to avoid constraint check error (A must be < N)
	mbedtls_mpi temp;
	mbedtls_mpi_init(&temp);
	try_mbedtls(&failed, "mbedtls_mpi_mod_mpi_temp", mbedtls_mpi_mod_mpi, &temp, N, &ls32);
	try_mbedtls(&failed, "mbedtls_mpi_inv_mod_a", mbedtls_mpi_inv_mod, &a, &temp, &ls32); // a = rsa._modinv(N, 1 << 32)
	mbedtls_mpi_free(&temp);

	uint32_t a32 = 0; // a32 = a 
	try_mbedtls(&failed, "mbedtls_mpi_write_binary_le", mbedtls_mpi_write_binary_le, &a, (uint8_t*) &a32, sizeof(uint32_t)); // a = rsa._modinv(N, 1 << 32)

	*mprime = ((int32_t) a32 * -1) & 0xFFFFFFFF;

	if(!failed)
	{
		if(ON_DEVELOPMENT)
		{
			// ESP_LOGI(TAG, "Calculate rinv, mprime result:");
			// print_mpi(N, "N");
			// print_mpi(rinv, "RInv");
			// printf("mprime: 0x%x\n", *mprime);
		}		
	}
	else
	{
		ESP_LOGE(TAG, "Calculate rinv, mprime failed");
	}

	mbedtls_mpi_free(&rr);
	mbedtls_mpi_free(&ls32);
	mbedtls_mpi_free(&a);

	return failed ? RET_ERROR : RET_OK;
}

/******************************************************************************
 * @brief Convert rinv and mprime to ds parameters.
 * @param d Input - RSA private D.
 * @param N Input - RSA private N.
 * @param rinv Input - R inverted.
 * @param mprime Input - M prime.
 * @param ds_p_data Output - Pointer to ds_p_data.
 ******************************************************************************/
RetResult conv_rinv_mprime_to_ds_params(const mbedtls_mpi* D, const mbedtls_mpi* N, const mbedtls_mpi* rinv,
                                         uint32_t mprime, esp_ds_p_data_t* ds_p_data)
{
	bool failed = false;

	try_mbedtls(&failed, "calc:Y", mbedtls_mpi_write_binary, D, (uint8_t*) ds_p_data->Y, sizeof(ds_p_data->Y));
	try_mbedtls(&failed, "calc:M", mbedtls_mpi_write_binary, N, (uint8_t*) ds_p_data->M, sizeof(ds_p_data->M));
	try_mbedtls(&failed, "calc:Rb", mbedtls_mpi_write_binary, rinv, (uint8_t*) ds_p_data->Rb, sizeof(ds_p_data->Rb));
	
	reverse_bytes((uint8_t*) ds_p_data->Y, sizeof(n_4096_bit_t)); // big to little endian
	reverse_bytes((uint8_t*) ds_p_data->M, sizeof(n_4096_bit_t)); // big to little endian
	reverse_bytes((uint8_t*) ds_p_data->Rb, sizeof(n_4096_bit_t));// big to little endian
	ds_p_data->M_prime = mprime;
	ds_p_data->length = DS_RSA_LENGTH;
    
    return failed ? RET_ERROR : RET_OK;
}

/******************************************************************************
 * @brief Retrieve burned hmac key.
 * @note Development usage only. The key will be read/write protected.
 ******************************************************************************/
RetResult retrieve_key(uint8_t *hmac)
{
	bool failed = false;

	if(is_efuse_key_block_unused(FUSE_BLOCK_HMAC))
	{
		ESP_LOGI(TAG, "Tried to retrieve hmac but key is empty.");
		return RET_ERROR;
	}

	if(esp_efuse_get_key_purpose(FUSE_BLOCK_HMAC) != ESP_EFUSE_KEY_PURPOSE_HMAC_DOWN_DIGITAL_SIGNATURE)
	{
#ifdef CONFIG_WXM_PRODUCTION_BURN_EFUSES
		ESP_LOGE(TAG, "Tried to retrieve hmac but purpose doesn't match.");
		return RET_ERROR;
#else
		ESP_LOGW(TAG, "HMAC purpose mismatch bypassed in Safe Mock-eFuse Mode.");
#endif
	}

	if(esp_efuse_get_key_dis_read(FUSE_BLOCK_HMAC))
	{
#ifdef CONFIG_WXM_PRODUCTION_BURN_EFUSES
		ESP_LOGE(TAG, "Tried to retrieve hmac but read is disabled.");
		return RET_INVALID_STATE;
#else
		ESP_LOGW(TAG, "HMAC read-protection bypass active in Safe Mock-eFuse Mode.");
#endif
	}

#ifdef CONFIG_WXM_PRODUCTION_BURN_EFUSES
	try_espret(&failed, "retrieve_hmac_blob", esp_efuse_read_field_blob, esp_efuse_get_key(FUSE_BLOCK_HMAC), hmac, DS_HMAC_LENGTH * 8);
#else
	nvs_handle_t my_handle;
	esp_err_t err = nvs_open("wxm_virt", NVS_READONLY, &my_handle);
	if (err != ESP_OK) {
		failed = true;
	} else {
		size_t len = DS_HMAC_LENGTH;
		err = nvs_get_blob(my_handle, "virt_hmac", hmac, &len);
		nvs_close(my_handle);
		if (err != ESP_OK || len != DS_HMAC_LENGTH) {
			failed = true;
		}
	}
#endif

	return failed ? RET_ERROR : RET_OK;
}

/******************************************************************************
 * @brief Config DS storage parameters and encrypt them.
 ******************************************************************************/
RetResult config_enc_ds_params(mbedtls_rsa_context* rsa, esp_ds_data_t* esp_ds_data, uint8_t *hmac)
{
	bool failed = false;
	int ret = 0;

	esp_ds_p_data_t* params = NULL;
	mbedtls_mpi rinv;

	mbedtls_mpi_init(&rinv);
	uint32_t mprime = 0;

	try_retres(&failed, "calc_rsa_rinv_mprime", calc_rsa_rinv_mprime, &rsa->private_N, &rinv, &mprime);

	params = (esp_ds_p_data_t*) heap_caps_malloc(sizeof(esp_ds_p_data_t), MALLOC_CAP_SPIRAM);
	try_ptr(&failed, params, "esp_ds_p_data_t_allocation");

	try_retres(&failed, "conv_rinv_mprime_to_ds_params", conv_rinv_mprime_to_ds_params,
				&rsa->private_D, &rsa->private_N, &rinv, mprime, params);

	if(!failed && ON_DEVELOPMENT)
	{
		// print_n4096("Y:\n", (const n_4096_bit_t *) params->Y);
		// print_n4096("M:\n", (const n_4096_bit_t *) params->M);
		// print_n4096("Rb:\n", (const n_4096_bit_t *) params->Rb);
		// printf("mprime: 0x%x\n", params->M_prime);
	}

	uint8_t iv[DS_IV_LENGTH];
	esp_fill_random(iv, DS_IV_LENGTH);
	if(ON_DEVELOPMENT)
	{		
		if(retrieve_key(hmac) == RET_OK)
		{
			ESP_LOGW(TAG, "Using existing READABLE hmac");
		}
		else
		{
			ESP_LOGW(TAG, "Using random HMAC");
			esp_fill_random(hmac, DS_HMAC_LENGTH);
		}		
	}
	else
	{
		esp_fill_random(hmac, DS_HMAC_LENGTH);
	}

	// aes encrypt RSA params with IV & HMAC keys
	try_espret(&failed, "esp_ds_encrypt_params", esp_ds_encrypt_params,
				esp_ds_data, iv, params, hmac);

	mbedtls_mpi_free(&rinv);
	heap_caps_free(params);

	return failed ? RET_ERROR : RET_OK;
}

/******************************************************************************
 * @brief Config DS storage partition.
 * @param pk Pointer to mbedtls_pk_context.
 * @param ds_data Pointer to ds encrypted data.
 * @param fuse_block Fuse block holding hmac key.
 ******************************************************************************/
RetResult config_ds_partition(char *dev_cert, uint32_t dev_cert_length, esp_ds_data_t* ds_data)
{
	bool failed = false;

	try_espret(&failed, "erase_ds_part", esp_secure_cert_erase_partition);

	esp_secure_cert_tlv_cfg cfg = {};
	cfg.type = ESP_SECURE_CERT_DEV_CERT_TLV;
	cfg.data = (uint8_t *) dev_cert;
	cfg.data_len = dev_cert_length;
	try_espret(&failed, "append_dev_cert_tlv", esp_secure_cert_append_tlv, &cfg);

	cfg.type = ESP_SECURE_CERT_DS_DATA_TLV;
	cfg.data = (const unsigned char*) ds_data;
	cfg.data_len = sizeof(esp_ds_data_t);
	try_espret(&failed, "append_ds_data_tlv", esp_secure_cert_append_tlv, &cfg);

	esp_ds_data_ctx_t ds_ctx = {};
	ds_ctx.efuse_key_id = KEY_BLOCK_HMAC;
	ds_ctx.rsa_length_bits = RSA_KEY_SIZE;
	cfg.type = ESP_SECURE_CERT_DS_CONTEXT_TLV;
	cfg.data = (const unsigned char*) &ds_ctx;
	cfg.data_len = sizeof(esp_ds_data_ctx_t);
	try_espret(&failed, "append_ds_ctx_tlv", esp_secure_cert_append_tlv, &cfg);

	return failed ? RET_ERROR : RET_OK;
}

/******************************************************************************
 * @brief Recerse bytes from/to Little/Big Endian
 * @param buff Pointer to byte buffer.
 * @param length Buffer length.
 ******************************************************************************/
void reverse_bytes(uint8_t *buff, size_t length)
{
	for(int i = 0; i < length/2; i++)
	{
		uint8_t temp_byte = buff[i];
		buff[i] = buff[length - i - 1];
		buff[length - i - 1] = temp_byte;
	}
}

/******************************************************************************
 * @brief Try chain for mbedtls.
 ******************************************************************************/
template<typename... func_args, typename... param_args>
int try_mbedtls(bool *prev_cur_failed, const char* func_name, int (*func)(func_args ...),param_args&& ... args)
{
	if(*prev_cur_failed)
	{
		return -1;
	}

	int ret = func(static_cast<param_args&&>(args) ...);

	if(ret < 0)
	{
		*prev_cur_failed = true;
		ESP_LOGE(TAG, "%s:failed with error -0x%x (%d)", func_name, -ret, ret);
	}

	return ret;
}

/******************************************************************************
 * @brief Try chain for RetResult.
 ******************************************************************************/
template<typename... func_args, typename... param_args>
RetResult try_retres(bool *prev_cur_failed, const char* func_name, RetResult (*func)(func_args ...),param_args&& ... args)
{
	if(*prev_cur_failed)
	{
		return RET_ERROR;
	}

	RetResult ret = func(static_cast<param_args&&>(args) ...);

	if(ret != RET_OK)
	{
		*prev_cur_failed = true;
		ESP_LOGE(TAG, "%s:failed", func_name);
	}

	return ret;
}

/******************************************************************************
 * @brief Try chain for esp_err_t.
 ******************************************************************************/
template<typename... func_args, typename... param_args>
esp_err_t try_espret(bool *prev_cur_failed, const char* func_name, esp_err_t (*func)(func_args ...),param_args&& ... args)
{
	if(*prev_cur_failed)
	{
		return -1;
	}

	esp_err_t ret = func(static_cast<param_args&&>(args) ...);

	if(ret != ESP_OK)
	{
		*prev_cur_failed = true;
		ESP_LOGE(TAG, "%s:failed", func_name);
	}

	return ret;
}

/******************************************************************************
 * @brief Try chain for pointers.
 ******************************************************************************/
bool try_ptr(bool *prev_cur_failed, void *ptr, const char* msg)
{
	if(*prev_cur_failed)
	{
		return false;
	}

	if(ptr)
	{
		return true;
	}
	
	*prev_cur_failed = false;
	ESP_LOGE(TAG, "%s:failed", msg);

	return false;
}

/******************************************************************************
 * @brief Print status of block keys in fuses.
 ******************************************************************************/
void print_blk_keys_status()
{
	printf("BLK KEYS Status:\n");
	printf("%s", "=========================\n");
	for(int i = EFUSE_BLK_KEY0; i < EFUSE_BLK_KEY_MAX; i++)
	{
		if(esp_efuse_key_block_unused((esp_efuse_block_t) i))
		{
			printf("BLK_KEY%d is NOT used\n", (i - EFUSE_BLK_KEY0));
		}
		else
		{
			printf("BLK_KEY%d is USED, %s, %s\n", (i - EFUSE_BLK_KEY0), 
			esp_efuse_get_key_dis_read((esp_efuse_block_t) i) ? "Unreadable" : "Readable", 
			esp_efuse_get_key_dis_write((esp_efuse_block_t) i) ? "Unwritable" : "Writable");
		}
	}
	printf("%s", "=========================\n");
}

/******************************************************************************
 * @brief Print 4096bit number.
 ******************************************************************************/
void print_n4096(char *text, const n_4096_bit_t *n4096)
{
	printf(text);
	Utils::print_buff_hex((u8*)n4096->n_4096_bit, sizeof(n4096->n_4096_bit));
}

/******************************************************************************
 * @brief Print Multiple Precision Integer
 ******************************************************************************/
void print_mpi(const mbedtls_mpi* mpi, const char* name)
{
	size_t olen = 0;
	uint32_t len = MBEDTLS_MPI_MAX_SIZE * 4 + 1;
	char* pp = (char*)calloc(1, len);
	mbedtls_mpi_write_string(mpi, 16, pp, len, &olen);
	printf("%s: %s\n", name, pp);
	free(pp);
}

/******************************************************************************
 * @brief Print device PEM certificate string.
 ******************************************************************************/
void print_dev_cert(char *dev_cert, uint32_t dev_cert_length)
{
	printf("Device Certificate PEM, length %d:\n", (int)dev_cert_length);
	printf("%s", "=========================\n");
	for(int i = 0; i < dev_cert_length; i++)
	{
		if(dev_cert[i] == '\0')
		{
			break;
		}
		printf("%c", (char)dev_cert[i]);
	}
	printf("%s", "=========================\n");
}

/******************************************************************************
 * @brief Print device serial number string.
 ******************************************************************************/
void print_dev_serial_str()
{
	printf("Device serial number: %s\n", _dev_serial_str);
}

/******************************************************************************
 * @brief Get pointer to loaded serial number string.
 ******************************************************************************/
char* get_serial_str()
{
	return _dev_serial_str;
}

/******************************************************************************
 * @brief Get serial number string length.
 ******************************************************************************/
size_t get_serial_str_length()
{
	return sizeof(_dev_serial_str);
}

/******************************************************************************
 * @brief Get pointer to loaded serial number.
 ******************************************************************************/
uint8_t* get_serial()
{
	return _dev_serial;
}

/******************************************************************************
 * @brief Get serial number byte length.
 ******************************************************************************/
size_t get_serial_length()
{
	return sizeof(_dev_serial);
}

/******************************************************************************
 * @brief Get pointer to loaded device certificate.
 ******************************************************************************/
char* get_dev_cert()
{
	return _dev_cert;
}

/******************************************************************************
 * @brief Get device certificate length.
 ******************************************************************************/
uint32_t get_dev_cert_length()
{
	return _dev_cert_length;
}

/******************************************************************************
 * @brief Get 32-byte SHA-256 fingerprint of device certificate or serial.
 ******************************************************************************/
RetResult get_dev_cert_fingerprint(uint8_t *out_32)
{
	if (out_32 == nullptr) {
		return RET_INVALID_PARAMS;
	}
	if (_dev_cert_length > 0 && _dev_cert != nullptr) {
		return Utils::calc_sha256_sw((const uint8_t *)_dev_cert, _dev_cert_length, out_32);
	}
	if (sizeof(_dev_serial) > 0) {
		return Utils::calc_sha256_sw((const uint8_t *)_dev_serial, sizeof(_dev_serial), out_32);
	}
	return RET_ERROR;
}

/******************************************************************************
 * @brief Get DS context.
 ******************************************************************************/
esp_ds_data_ctx_t* get_ds_ctx()
{
	return _ds_ctx;
}

bool is_virtual_key_block_unused(const char *key_name)
{
	nvs_handle_t my_handle;
	if (nvs_open("wxm_virt", NVS_READONLY, &my_handle) != ESP_OK)
	{
		return true; // Unopened or not found means unused
	}
	size_t size = 0;
	esp_err_t err = nvs_get_blob(my_handle, key_name, NULL, &size);
	nvs_close(my_handle);
	return (err != ESP_OK || size == 0);
}

bool is_efuse_key_block_unused(esp_efuse_block_t block)
{
#ifdef CONFIG_WXM_PRODUCTION_BURN_EFUSES
	return esp_efuse_key_block_unused(block);
#else
	if (block == FUSE_BLOCK_SERIAL)
	{
		return is_virtual_key_block_unused("virt_serial");
	}
	else if (block == FUSE_BLOCK_HMAC)
	{
		return is_virtual_key_block_unused("virt_hmac");
	}
	return true;
#endif
}

RetResult save_private_key_to_nvs(mbedtls_pk_context *pk)
{
	uint8_t *buf = (uint8_t *)malloc(1600);
	if (!buf)
	{
		ESP_LOGE(TAG, "save_private_key_to_nvs: memory allocation failed");
		return RET_ERROR;
	}
	int len = mbedtls_pk_write_key_der(pk, buf, 1600);
	if (len < 0)
	{
		ESP_LOGE(TAG, "save_private_key_to_nvs: mbedtls_pk_write_key_der failed (%d)", len);
		free(buf);
		return RET_ERROR;
	}
	uint8_t *key_start = buf + 1600 - len;

	nvs_handle_t my_handle;
	esp_err_t err = nvs_open("wxm_virt", NVS_READWRITE, &my_handle);
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "save_private_key_to_nvs: nvs_open failed (%d)", err);
		free(buf);
		return RET_ERROR;
	}
	err = nvs_set_blob(my_handle, "virt_priv_key", key_start, len);
	if (err == ESP_OK)
	{
		err = nvs_commit(my_handle);
	}
	nvs_close(my_handle);
	free(buf);

	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "save_private_key_to_nvs: nvs set/commit failed (%d)", err);
		return RET_ERROR;
	}

	ESP_LOGI(TAG, "Virtual private key successfully saved to NVS.");
	return RET_OK;
}

RetResult load_private_key_from_nvs(mbedtls_pk_context *pk)
{
	nvs_handle_t my_handle;
	esp_err_t err = nvs_open("wxm_virt", NVS_READONLY, &my_handle);
	if (err != ESP_OK)
	{
		return RET_ERROR;
	}

	size_t required_size = 0;
	err = nvs_get_blob(my_handle, "virt_priv_key", NULL, &required_size);
	if (err != ESP_OK || required_size == 0)
	{
		nvs_close(my_handle);
		return RET_ERROR;
	}

	uint8_t *buf = (uint8_t *)malloc(required_size);
	if (!buf)
	{
		nvs_close(my_handle);
		return RET_ERROR;
	}

	err = nvs_get_blob(my_handle, "virt_priv_key", buf, &required_size);
	nvs_close(my_handle);

	if (err != ESP_OK)
	{
		free(buf);
		return RET_ERROR;
	}

	mbedtls_pk_init(pk);
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
	int mbed_ret = mbedtls_pk_parse_key(pk, buf, required_size, NULL, 0);
#else
	int mbed_ret = mbedtls_pk_parse_key(pk, buf, required_size, NULL, 0, NULL, NULL);
#endif
	free(buf);

	if (mbed_ret != 0)
	{
		ESP_LOGE(TAG, "load_private_key_from_nvs: mbedtls_pk_parse_key failed (%d)", mbed_ret);
		return RET_ERROR;
	}

	return RET_OK;
}

RetResult load_private_key_raw_from_nvs(uint8_t **out_buf, size_t *out_size)
{
	nvs_handle_t my_handle;
	esp_err_t err = nvs_open("wxm_virt", NVS_READONLY, &my_handle);
	if (err != ESP_OK)
	{
		return RET_ERROR;
	}

	size_t required_size = 0;
	err = nvs_get_blob(my_handle, "virt_priv_key", NULL, &required_size);
	if (err != ESP_OK || required_size == 0)
	{
		nvs_close(my_handle);
		return RET_ERROR;
	}

	uint8_t *buf = (uint8_t *)malloc(required_size);
	if (!buf)
	{
		nvs_close(my_handle);
		return RET_ERROR;
	}

	err = nvs_get_blob(my_handle, "virt_priv_key", buf, &required_size);
	nvs_close(my_handle);

	if (err != ESP_OK)
	{
		free(buf);
		return RET_ERROR;
	}

	*out_buf = buf;
	*out_size = required_size;
	return RET_OK;
}

}
#else
#include "esp_log.h"
#include <string.h>

namespace Credentials {

static char _dev_serial_str[DEV_SERIAL_STR_LENGTH] = "00:11:22:33:44:55:66:77:88:99";
static uint8_t _dev_serial[DEV_SERIAL_LENGTH] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
static char _dev_cert[1] = {0};
static uint32_t _dev_cert_length = 0;
static esp_ds_data_ctx_t* _ds_ctx = NULL;

RetResult init() {
    ESP_LOGI("Credentials", "Mock Credentials initialization (classic ESP32 target)");
    return RET_OK;
}
char* get_serial_str() {
    return _dev_serial_str;
}
size_t get_serial_str_length() {
    return sizeof(_dev_serial_str);
}
uint8_t* get_serial() {
    return _dev_serial;
}
size_t get_serial_length() {
    return sizeof(_dev_serial);
}
char* get_dev_cert() {
    return _dev_cert;
}
uint32_t get_dev_cert_length() {
    return _dev_cert_length;
}
RetResult get_dev_cert_fingerprint(uint8_t *out_32) {
    if (out_32 == nullptr) return RET_INVALID_PARAMS;
    if (_dev_cert_length > 0 && _dev_cert != nullptr) {
        return Utils::calc_sha256_sw((const uint8_t *)_dev_cert, _dev_cert_length, out_32);
    }
    return Utils::calc_sha256_sw((const uint8_t *)_dev_serial, sizeof(_dev_serial), out_32);
}
esp_ds_data_ctx_t* get_ds_ctx() {
    return _ds_ctx;
}
RetResult load_private_key_raw_from_nvs(uint8_t **out_buf, size_t *out_size) {
    return RET_ERROR;
}

} // namespace Credentials
#endif
