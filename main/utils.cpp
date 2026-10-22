#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS

#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/version.h"
#include "mbedtls/md.h"

#if __has_include("mbedtls/private/entropy.h")
#include "mbedtls/private/entropy.h"
#include "mbedtls/private/ctr_drbg.h"
#else
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#endif
#include "sys/time.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "math.h"
#include "time.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_task_wdt.h"
#include "esp_efuse.h"
#include "esp_mac.h"
#include "tb.h"
#include "wifi.h"
#include "esp_task_wdt.h"
#include "env.h"
#include "utils.h"
#include "app_config.h"
#include "const.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_attr.h"
#include "esp_partition.h"
#include "driver/gpio.h"
#include "freertos/semphr.h"
#include "esp_rom_sys.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include "hal/gpio_ll.h"
#include "bootloader_common.h"
#include "nvs_flash.h"
#include "unistd.h"
#include "sys/stat.h"
#ifndef CONFIG_SCREENLESS
#include "bsp_lcd.h"
#endif
#if !defined(CONFIG_IDF_TARGET_ESP32)
#include "esp_ds.h"
#endif
#include "ui/views/include/ui_view_splash.h"
#include "esp_task_wdt.h"
#include "credentials.h"
#include "ws/include/ws_data.h"

extern "C" {
    #include "esp_secure_cert_read.h"
}

namespace Utils
{
//
// Local vars
//
/** Logger tag */
static const char TAG[] = "Utils";

static const uint32_t RESTART_BREADCRUMB_MAGIC = 0x52535431; // "RST1"
static const char DIAGNOSTIC_LOG_PATH[] = "/littlefs/boot_diag.log";
static const char DIAGNOSTIC_LOG_PREVIOUS_PATH[] = "/littlefs/boot_diag.1.log";
static const long DIAGNOSTIC_LOG_MAX_BYTES = 16 * 1024;

struct RestartBreadcrumb
{
	uint32_t magic;
	uint8_t reason;
	uint8_t reason_inverse;
	uint16_t reserved;
};

static RTC_NOINIT_ATTR RestartBreadcrumb s_restart_breadcrumb;
static BootDiagnostics s_boot_diagnostics = {
	.reset_reason = ESP_RST_UNKNOWN,
	.raw_reset_reason_cpu0 = -1,
	.raw_reset_reason_cpu1 = -1,
	.controlled_restart_reason = RestartReason::UNKNOWN,
	.bootloader_factory_reset = false,
	.gpio10_level_at_app_entry = -1,
	.gpio38_level_at_app_entry = -1,
	.running_partition = "",
	.nvs_initial_result = ESP_ERR_INVALID_STATE,
	.nvs_final_result = ESP_ERR_INVALID_STATE,
	.nvs_used_entries = 0,
	.nvs_free_entries = 0,
	.nvs_total_entries = 0,
	.nvs_namespace_count = 0,
};

//
// Local functions
//
void diag_monitor_task(void *param);

const char *restart_reason_to_str(RestartReason reason)
{
	switch(reason)
	{
		case RestartReason::GENERAL:
			return "general";
		case RestartReason::CREDENTIAL_PROVISIONING:
			return "credential_provisioning";
		case RestartReason::OTA_UPDATE:
			return "ota_update";
		case RestartReason::WEB_OTA_UPDATE:
			return "web_ota_update";
		case RestartReason::BOOT_GUARD_ROLLBACK:
			return "boot_guard_rollback";
		case RestartReason::WIFI_CONFIG_REQUEST:
			return "wifi_config_request";
		case RestartReason::WIFI_CONFIG_COMPLETE:
			return "wifi_config_complete";
		case RestartReason::WIFI_CONFIG_TIMEOUT:
			return "wifi_config_timeout";
		case RestartReason::WIFI_RECOVERY:
			return "wifi_recovery";
		case RestartReason::SOFT_WATCHDOG:
			return "soft_watchdog";
		case RestartReason::SETTINGS_CHANGE:
			return "settings_change";
		case RestartReason::RECOVERY_EXIT:
			return "recovery_exit";
		case RestartReason::FACTORY_FIRMWARE_RESTORE:
			return "factory_firmware_restore";
		case RestartReason::TEST_MODE_EXIT:
			return "test_mode_exit";
		case RestartReason::NVS_INCOMPATIBLE:
			return "nvs_incompatible";
		case RestartReason::REMOTE_REBOOT:
			return "remote_reboot";
		case RestartReason::OTA_FAILED:
			return "ota_failed";
		case RestartReason::UNKNOWN:
		default:
			return "unknown";
	}
}

void append_diagnostic_event(const char *event, int value1, int value2, const char *detail)
{
	if(event == nullptr)
	{
		return;
	}

	struct stat st = {};
	if(stat(DIAGNOSTIC_LOG_PATH, &st) == 0 && st.st_size >= DIAGNOSTIC_LOG_MAX_BYTES)
	{
		unlink(DIAGNOSTIC_LOG_PREVIOUS_PATH);
		rename(DIAGNOSTIC_LOG_PATH, DIAGNOSTIC_LOG_PREVIOUS_PATH);
	}

	FILE *file = fopen(DIAGNOSTIC_LOG_PATH, "a");
	if(file == nullptr)
	{
		ESP_LOGW(TAG, "Could not append diagnostic event '%s'", event);
		return;
	}

	fprintf(file, "%lld,%s,%d,%d,%s\n",
			(long long)(esp_timer_get_time() / 1000),
			event,
			value1,
			value2,
			detail != nullptr ? detail : "");
	fflush(file);
	fsync(fileno(file));
	fclose(file);
}

void capture_early_boot_diagnostics()
{
	s_boot_diagnostics.reset_reason = esp_reset_reason();
	s_boot_diagnostics.raw_reset_reason_cpu0 = (int)esp_rom_get_reset_reason(0);
#if !CONFIG_FREERTOS_UNICORE
	s_boot_diagnostics.raw_reset_reason_cpu1 = (int)esp_rom_get_reset_reason(1);
#endif
	s_boot_diagnostics.gpio10_level_at_app_entry = gpio_get_level(GPIO_NUM_10);
	s_boot_diagnostics.gpio38_level_at_app_entry = gpio_get_level(GPIO_NUM_38);

#ifndef CONFIG_SCREENLESS
	// Controlled restarts hold GPIO10 high through the bootloader's historical
	// factory-reset check. Restore a known high output before releasing the pad
	// hold so the LCD data line cannot glitch low on release; BSP init will
	// configure it for RGB output later.
	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_direction(GPIO_NUM_10, GPIO_MODE_INPUT_OUTPUT));
	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level(GPIO_NUM_10, 1));
	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_hold_dis(GPIO_NUM_10));
#endif

	if(s_restart_breadcrumb.magic == RESTART_BREADCRUMB_MAGIC &&
	   (uint8_t)~s_restart_breadcrumb.reason == s_restart_breadcrumb.reason_inverse &&
	   s_restart_breadcrumb.reason <= (uint8_t)RestartReason::OTA_FAILED)
	{
		s_boot_diagnostics.controlled_restart_reason = (RestartReason)s_restart_breadcrumb.reason;
	}
	s_restart_breadcrumb.magic = 0;

#if CONFIG_BOOTLOADER_RESERVE_RTC_MEM
	s_boot_diagnostics.bootloader_factory_reset =
		bootloader_common_get_rtc_retain_mem_factory_reset_state();
#endif

	const esp_partition_t *running = esp_ota_get_running_partition();
	if(running != nullptr)
	{
		strlcpy(s_boot_diagnostics.running_partition, running->label,
				sizeof(s_boot_diagnostics.running_partition));
	}

	char detail[96] = "";
	snprintf(detail, sizeof(detail), "restart=%s part=%s factory_reset=%d gpio10=%d gpio38=%d",
			 restart_reason_to_str(s_boot_diagnostics.controlled_restart_reason),
			 s_boot_diagnostics.running_partition,
			 s_boot_diagnostics.bootloader_factory_reset,
			 s_boot_diagnostics.gpio10_level_at_app_entry,
			 s_boot_diagnostics.gpio38_level_at_app_entry);
	append_diagnostic_event("boot",
						(int)s_boot_diagnostics.reset_reason,
						s_boot_diagnostics.raw_reset_reason_cpu0,
						detail);

	ESP_LOGW(TAG,
			 "Boot diagnostics: reset=%d raw_cpu0=%d raw_cpu1=%d controlled=%s "
			 "factory_reset=%d partition=%s gpio10=%d gpio38=%d",
			 (int)s_boot_diagnostics.reset_reason,
			 s_boot_diagnostics.raw_reset_reason_cpu0,
			 s_boot_diagnostics.raw_reset_reason_cpu1,
			 restart_reason_to_str(s_boot_diagnostics.controlled_restart_reason),
			 s_boot_diagnostics.bootloader_factory_reset,
			 s_boot_diagnostics.running_partition,
			 s_boot_diagnostics.gpio10_level_at_app_entry,
			 s_boot_diagnostics.gpio38_level_at_app_entry);
}

void record_nvs_diagnostics(esp_err_t initial_result, esp_err_t final_result)
{
	s_boot_diagnostics.nvs_initial_result = initial_result;
	s_boot_diagnostics.nvs_final_result = final_result;

	if(final_result == ESP_OK)
	{
		nvs_stats_t stats = {};
		esp_err_t stats_result = nvs_get_stats(nullptr, &stats);
		if(stats_result == ESP_OK)
		{
			s_boot_diagnostics.nvs_used_entries = stats.used_entries;
			s_boot_diagnostics.nvs_free_entries = stats.free_entries;
			s_boot_diagnostics.nvs_total_entries = stats.total_entries;
			s_boot_diagnostics.nvs_namespace_count = stats.namespace_count;
		}
		else
		{
			ESP_LOGE(TAG, "nvs_get_stats failed: %s", esp_err_to_name(stats_result));
		}
	}

	char detail[96] = "";
	snprintf(detail, sizeof(detail), "used=%u free=%u total=%u namespaces=%u",
			 (unsigned)s_boot_diagnostics.nvs_used_entries,
			 (unsigned)s_boot_diagnostics.nvs_free_entries,
			 (unsigned)s_boot_diagnostics.nvs_total_entries,
			 (unsigned)s_boot_diagnostics.nvs_namespace_count);
	append_diagnostic_event("nvs_init", initial_result, final_result, detail);

	ESP_LOGW(TAG, "NVS diagnostics: initial=%s final=%s used=%u free=%u total=%u namespaces=%u",
			 esp_err_to_name(initial_result),
			 esp_err_to_name(final_result),
			 (unsigned)s_boot_diagnostics.nvs_used_entries,
			 (unsigned)s_boot_diagnostics.nvs_free_entries,
			 (unsigned)s_boot_diagnostics.nvs_total_entries,
			 (unsigned)s_boot_diagnostics.nvs_namespace_count);
}

const BootDiagnostics &get_boot_diagnostics()
{
	return s_boot_diagnostics;
}

/******************************************************************************
 * Wait for X ms
 * @param ms ms to wait for
 *****************************************************************************/
void wait_ms(u32 ms)
{
	vTaskDelay(pdMS_TO_TICKS(ms));
}

/******************************************************************************
 * @brief Enable or disable idle task watchdog of the core the current task is
 *	is running at.
 ******************************************************************************/
RetResult set_idle_task_watchdog_state(bool enable)
{
#if CONFIG_FREERTOS_UNICORE
	TaskHandle_t handle = xTaskGetIdleTaskHandle();
	if(handle == NULL)
	{
		ESP_LOGE(TAG, "set_idle_task_watchdog_state: couldn't get handle");
		return RET_ERROR;
	}

	if(enable)
	{
		if(esp_task_wdt_add(handle)  != ESP_OK)
		{
			ESP_LOGE(TAG, "set_idle_task_watchdog_state:esp_task_wdt_add task failed");
			return RET_ERROR;
		}
	}
	else
	{		
		if(esp_task_wdt_delete(handle) != ESP_OK)
		{
			ESP_LOGE(TAG, "set_idle_task_watchdog_state:esp_task_wdt_delete task failed");
			return RET_ERROR;
		}
	}
#else
	for (int i = 0; i < 2; i++) {
		TaskHandle_t handle = xTaskGetIdleTaskHandleForCore(i);
		if (handle != NULL) {
			if (enable) {
				esp_task_wdt_add(handle);
			} else {
				esp_task_wdt_delete(handle);
			}
		}
	}
#endif
	
	return RET_OK;
}

/******************************************************************************
 * @brief Function to convert hex string to byte array
 * @param hex_str String of hex chars
 * @param byte_array Buffer to save byte array
 * @param byte_array_len Length of buffer to save byte array
 ******************************************************************************/
static inline uint8_t hex_char_to_val(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return 0;
}

RetResult hex_str_to_byte_array(const char *hex_str, u8 *byte_array, int byte_array_len)
{
	if (hex_str == nullptr)
	{
		return RetResult::RET_INVALID_PARAMS;
	}

	if (strncmp(hex_str, "0x", 2) == 0 || strncmp(hex_str, "0X", 2) == 0)
	{
		hex_str += 2;
	}

	if(strlen(hex_str) != byte_array_len * 2)
	{
		return RetResult::RET_INVALID_PARAMS;
	}

	for(int i = 0; i < byte_array_len; i++)
	{
		char high = hex_str[i * 2];
		char low = hex_str[i * 2 + 1];
		if(!isxdigit((unsigned char)high) || !isxdigit((unsigned char)low))
		{
			return RetResult::RET_INVALID_PARAMS;
		}

		byte_array[i] = (hex_char_to_val(high) << 4) | hex_char_to_val(low);
	}

	return RetResult::RET_OK;
}

/******************************************************************************
* Pairing mode to lowercase string
******************************************************************************/
const char *pair_mode_to_str(PairMode mode)
{
	switch(mode)
	{
		case PairMode::PAIR_MODE_OFF:   return "off";
		case PairMode::PAIR_MODE_CLOUD: return "cloud";
		case PairMode::PAIR_MODE_LOCAL: return "local";
		case PairMode::PAIR_MODE_SCAN:  return "scan";
		default:                        return "none";
	}
}

/******************************************************************************
* Parse pairing mode string ("auto" is accepted as a legacy alias of "scan")
******************************************************************************/
PairMode pair_mode_from_str(const char *str)
{
	if(str == nullptr)               return PairMode::PAIR_MODE_NONE;
	if(strcmp(str, "off") == 0)      return PairMode::PAIR_MODE_OFF;
	if(strcmp(str, "cloud") == 0)    return PairMode::PAIR_MODE_CLOUD;
	if(strcmp(str, "local") == 0)    return PairMode::PAIR_MODE_LOCAL;
	if(strcmp(str, "scan") == 0 ||
	   strcmp(str, "auto") == 0)     return PairMode::PAIR_MODE_SCAN;
	return PairMode::PAIR_MODE_NONE;
}

/******************************************************************************
* Get screen rotation descriptor by name or degrees value
* Either name or value must be set, not both
* @param name Screen rotation name as displayed in UI
* @param scr_rot Rotation value in degrees
* @return Ptr to screen rotation descriptor in provided options array
******************************************************************************/
const ScreenRotationDesc *get_scr_rot_desc(char *name, ScreenRotation scr_rot)
{
	// Only name or rotation must be set
	if(!((name == nullptr) ^ (scr_rot == ScreenRotation::SCR_ROT_DEGREES_NONE)))
	{
		return nullptr;
	}

	const int LAST_INDEX = sizeof(SCREEN_ROTATION_OPTIONS) / sizeof(SCREEN_ROTATION_OPTIONS[0]);
	int found_index = -1;

	if(name != nullptr)
	{
		for (int i = 0; i < LAST_INDEX; i++)
		{
			if(strcmp(name, SCREEN_ROTATION_OPTIONS[i].title) == 0)
			{
				found_index = i;
				break;
			}
		}
	}
	else
	{
		for (int i = 0; i < LAST_INDEX; i++)
		{
			if(SCREEN_ROTATION_OPTIONS[i].degrees == scr_rot)
			{
				found_index = i;
				break;
			}
		}
	}

	return found_index >= 0 ? &SCREEN_ROTATION_OPTIONS[found_index] : nullptr;
}

bool normal_boot()
{
#ifdef CONFIG_SCREENLESS
	return true;
#else
	static UiViewSplash::View view_splash;
	UiCommon::lock();
	view_splash.create(NULL);
	UiCommon::unlock();
	vTaskDelay(pdMS_TO_TICKS(BOOT_LOGO_DISPLAY_TIME_MS));
	UiCommon::lock();
	view_splash.disable_click();
	UiCommon::unlock();
	vTaskDelay(pdMS_TO_TICKS(500));
	return !view_splash.get_enter_recovery();
#endif
}

void print_boot_info()
{
	const char logo[] =
		"\033[1;36m __        __         _   _             __  ____  __ \n"
		"\033[0;36m \\ \\      / /__  __ _| |_| |__   ___ _ _\\ \\/ /  \\/  |\n"
		"\033[0;36m  \\ \\ /\\ / / _ \\/ _` | __| '_ \\ / _ \\ '__\\  /| |\\/| |\n"
		"\033[1;36m   \\ V  V /  __/ (_| | |_| | | |  __/ |  /  \\| |  | |\n"
		"\033[0;36m    \\_/\\_/ \\___|\\__,_|\\__|_| |_|\\___|_| /_/\\_\\_|  |_|\033[0m\n";
	printf("\n%s\n", logo);

	printf("%s", "═════════════════════════════════════════════\n");
	printf("> \033[0;36mWXM WG1200 \033[0m\n");

	// Version and build env on same line
	printf("> Version: %s ", APP_VERSION);
	#if WXM_ENV_DEBUG
	printf("\u001b[31mDEBUG BUILD\n\u001b[0m");
	#elif WXM_ENV_RELEASE
	printf("\u001b[34mRELEASE BUILD\n\u001b[0m");
	#endif

	printf("> S/N: %s\n", Credentials::get_serial_str());
	printf("> DevCert: \n%s\n", Credentials::get_dev_cert());

	// Print active broker info
	//auto broker = Utils::get_active_broker_desc();

	//printf("> Mqtt: %s:%d\n", broker->addr, broker->port);

	printf("%s", "═════════════════════════════════════════════\n\n");

	printf("Default settings\n");
	Settings::print_settings((Settings::Data*)&DEFAULT_SETTINGS);

	printf("Current settings\n");
	Settings::print_cur_settings();
}

/******************************************************************************
 * Print a buffer in hex
 * @param buff Buffer to print
 * @param len Buffer length
 *****************************************************************************/
void print_buff_hex(uint8_t *buff, int len)
{
	printf("%s", "=========================\n");
	// char line[100] = "";
	for (int i = 0; i < len; i++)
	{
		printf("%02x ", buff[i]);

		if (((i + 1) % 32 == 0) && (i != len - 1))
		{
			printf("\n");
		}
	}
	printf("%s", "\n=========================\n");
}

/******************************************************************************
* Buiild HEX string from buffer
* @param buff Input buffer
* @param buff_len Length of data to parse
* @param out Output buffer
* @param out_size Output buffer size
* @return
*	RET_INVALID_PARAMS - Output buffer too small
******************************************************************************/
RetResult buff_to_hex_str(uint8_t *buff, int buff_len, char *out, int out_size)
{
	// Output buffer too small
	if(out_size < 2 + 1 || out_size < buff_len * 2 + 1)
	{
		return RET_INVALID_PARAMS;
	}

	// Protect against uninitialized char *
	out[0] = '\0';

	for(int i = 0; i < buff_len; i++)
	{
		char hex_byte[3] = "";
		snprintf(hex_byte, sizeof(hex_byte), "%02X", buff[i]);

		strcat(out, hex_byte);
	}

	out[buff_len * 2] = '\0';

	return RET_OK;
}

/******************************************************************************
 * Convert degrees to radians
 * @param deg Degrees
 * @return Radians
 *****************************************************************************/
float deg_to_rad(float deg)
{
	return deg * M_PI / 180;
}

/******************************************************************************
 * Convert radians to degrees
 *****************************************************************************/
float rad_to_deg(float rad)
{
	return rad * (180 / M_PI);
}

/******************************************************************************
* Clear saved WiFi manager credentials.
* Wif Manager library doesn't come with a reset function. Once creds are saved
* you're stuck. Nvs and nvs_sync must be already initialized before calling this.
* TODO: Creaate pull request for lib
******************************************************************************/
RetResult wifi_manager_clear_creds()
{
	nvs_handle_t handle;
	esp_err_t esp_err;

	ESP_LOGW(TAG, "Clearing wifi creds");

	esp_err = nvs_open("espwifimgr", NVS_READWRITE, &handle);
	if (esp_err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS namespace 'espwifimgr' for clearing, err: %s", esp_err_to_name(esp_err));
		return RET_ERROR;
	}

	/** Erase SSID */
	esp_err = nvs_set_blob(handle, "ssid", "", 1);
	if(esp_err != ESP_OK){
		ESP_LOGE(TAG, "Failed to erase ssid blob in NVS, err: %s", esp_err_to_name(esp_err));
		nvs_close(handle);
		return RET_ERROR;
	}

	/** Erase password */
	esp_err = nvs_set_blob(handle, "password", "", 1);
	if(esp_err != ESP_OK){
		ESP_LOGE(TAG, "Failed to erase password blob in NVS, err: %s", esp_err_to_name(esp_err));
		nvs_close(handle);
		return RET_ERROR;
	}

	esp_err = nvs_commit(handle);
	if(esp_err != ESP_OK){
		ESP_LOGE(TAG, "Failed to commit erased wifi creds to NVS, err: %s", esp_err_to_name(esp_err));
		nvs_close(handle);
		return RET_ERROR;
	}

	nvs_close(handle);

	ESP_LOGI(TAG, "WiFi credentials cleared and committed successfully");
	return RET_OK;
}

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
* Sign input data with RSA using DS Storage private key in slot 0
* Result is rsa(padding(sha256(data)))
* @param data Input data
* @param data_size Input data length
* @param sig_buff 64 byte output buffer for signature
* @return
*	RET_INVALID_PARAMS
*	RET_ERROR on sign failure
******************************************************************************/
RetResult device_sign(uint8_t *data, size_t data_len, uint8_t *sig_buff, size_t sig_buff_size)
{
	if(data_len == 0) return RET_INVALID_PARAMS;
	if(sig_buff_size != DEVICE_SIG_BUFF_SIZE) return RET_INVALID_PARAMS;

	static SemaphoreHandle_t s_device_sign_mutex = NULL;
	if (s_device_sign_mutex == NULL) {
		s_device_sign_mutex = xSemaphoreCreateMutex();
	}

	struct AutoLock {
		SemaphoreHandle_t m;
		AutoLock(SemaphoreHandle_t mutex) : m(mutex) {
			if (m) xSemaphoreTake(m, portMAX_DELAY);
		}
		~AutoLock() {
			if (m) xSemaphoreGive(m);
		}
	} lock(s_device_sign_mutex);

	//ESP_LOGI(TAG, "Signing data");
	//print_buff_hex(data, data_len);

	//
	// SHA256 hash of input
	//
	u8 hash[32] = "";
	if (calc_sha256_sw(data, data_len, hash) != RET_OK)
	{
		ESP_LOGE(TAG, "Signing failed.");
		return RET_ERROR;
	}

	//
	// Sign
	//
#if !defined(CONFIG_IDF_TARGET_ESP32)
	esp_ds_data_ctx_t *ds_ctx = Credentials::get_ds_ctx();
	if (ds_ctx != NULL && ds_ctx->esp_ds_data != NULL) {
		ESP_LOGI(TAG, "device_sign: Using hardware Digital Signature (DS) context");
		size_t rsa_len_bytes = ds_ctx->rsa_length_bits / 8;
		if (rsa_len_bytes == 0) {
			rsa_len_bytes = 256; // fallback for 2048-bit key
		}

		// Prepare PKCS#1 v1.5 padding block for SHA-256
		uint8_t *em = (uint8_t *) calloc(1, rsa_len_bytes);
		if (em == NULL) {
			ESP_LOGE(TAG, "Failed to allocate memory for padded block");
			return RET_ERROR;
		}

		em[0] = 0x00;
		em[1] = 0x01;
		// Fill padding bytes with 0xFF
		size_t padding_len = rsa_len_bytes - 3 - 51; // 3 bytes structural, 51 bytes SHA-256 DigestInfo DER + hash
		memset(&em[2], 0xFF, padding_len);
		em[2 + padding_len] = 0x00;

		// Copy DER prefix for SHA-256 DigestInfo
		const uint8_t sha256_der_prefix[] = {
			0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
		};
		memcpy(&em[3 + padding_len], sha256_der_prefix, sizeof(sha256_der_prefix));

		// Copy SHA-256 hash
		memcpy(&em[3 + padding_len + sizeof(sha256_der_prefix)], hash, 32);

		// Allocate 32-bit aligned buffers for DS peripheral
		uint32_t *sig_buffer = (uint32_t *) heap_caps_malloc(rsa_len_bytes, MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL);
		if (sig_buffer == NULL) {
			ESP_LOGE(TAG, "Failed to allocate 32-bit aligned buffer");
			free(em);
			return RET_ERROR;
		}

		// Swap byte order as 32-bit words for the hardware peripheral
		unsigned int words_len = rsa_len_bytes / 4;
		const uint32_t *em_words = (const uint32_t *) em;
		for (unsigned int i = 0; i < words_len; i++) {
			sig_buffer[i] = __builtin_bswap32(em_words[words_len - (i + 1)]);
		}

		// Execute hardware Digital Signature
		esp_err_t ds_err = esp_ds_sign((const void *) sig_buffer,
		                               ds_ctx->esp_ds_data,
		                               (hmac_key_id_t) ds_ctx->efuse_key_id,
		                               (void *) sig_buffer);
		if (ds_err != ESP_OK) {
			ESP_LOGE(TAG, "esp_ds_sign failed: 0x%x", ds_err);
			heap_caps_free(sig_buffer);
			free(em);
			return RET_ERROR;
		}

		// Swap back the result words to output signature buffer
		uint32_t *out_words = (uint32_t *) sig_buff;
		for (unsigned int i = 0; i < words_len; i++) {
			out_words[i] = __builtin_bswap32(sig_buffer[words_len - (i + 1)]);
		}

		heap_caps_free(sig_buffer);
		free(em);
	} else {
		ESP_LOGI(TAG, "device_sign: DS context not available. Using software RSA-SHA256 signing fallback.");
		mbedtls_pk_context pk;
		if (Credentials::load_private_key_from_nvs(&pk) != RET_OK) {
			ESP_LOGE(TAG, "Failed to load virtual private key from NVS for signing. Mocking signature.");
			memset(sig_buff, 0xAA, sig_buff_size);
			return RET_OK;
		}

		size_t sig_len = 0;
		int mbed_ret = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, hash, 32, sig_buff, sig_buff_size, &sig_len);
		if (mbed_ret != 0) {
			ESP_LOGE(TAG, "mbedtls_pk_sign failed: -0x%x", -mbed_ret);
		} else {
			ESP_LOGI(TAG, "Successfully generated software signature (length: %zu)", sig_len);
		}

		mbedtls_pk_free(&pk);

		if (mbed_ret != 0) {
			return RET_ERROR;
		}
	}
#else
	ESP_LOGI(TAG, "device_sign: Mock signature (classic ESP32 target)");
	memset(sig_buff, 0xAA, sig_buff_size);
#endif

	// ESP_LOGI(TAG, "Signature:");
	// print_buff_hex(sig_buff, sig_buff_size);

	return RET_OK;
}

/******************************************************************************
* Determine the state of stored WiFi manager credentials.
* Distinguishes "definitively not provisioned" from "could not read": only the
* former may trigger automatic AP config mode, otherwise a transient NVS error
* would take a working field device offline into config mode.
******************************************************************************/
WifiCredsState wifi_manager_creds_state()
{
	nvs_handle handle;
	esp_err_t esp_err = nvs_open("espwifimgr", NVS_READONLY, &handle);
	if(esp_err == ESP_ERR_NVS_NOT_FOUND)
	{
		ESP_LOGW(TAG, "wifi_manager_creds_state: NVS namespace 'espwifimgr' does not exist.");
		return WifiCredsState::ABSENT;
	}
	if(esp_err != ESP_OK)
	{
		ESP_LOGE(TAG, "wifi_manager_creds_state: failed to open NVS namespace 'espwifimgr', err: %s", esp_err_to_name(esp_err));
		return WifiCredsState::ERROR;
	}

	size_t required_len = 0;
	esp_err = nvs_get_blob(handle, "ssid", NULL, &required_len);
	if(esp_err == ESP_ERR_NVS_NOT_FOUND)
	{
		ESP_LOGW(TAG, "wifi_manager_creds_state: 'ssid' key not found in NVS 'espwifimgr' namespace.");
		nvs_close(handle);
		return WifiCredsState::ABSENT;
	}
	if(esp_err != ESP_OK)
	{
		ESP_LOGE(TAG, "wifi_manager_creds_state: failed to query SSID length from NVS, err: %s", esp_err_to_name(esp_err));
		nvs_close(handle);
		return WifiCredsState::ERROR;
	}

	char ssid[33] = "";
	size_t len = (required_len < 32) ? required_len : 32;
	if (required_len <= 32) {
		esp_err = nvs_get_blob(handle, "ssid", ssid, &len);
	} else {
		char *temp_buf = (char *)malloc(required_len);
		if (temp_buf == NULL) {
			ESP_LOGE(TAG, "wifi_manager_creds_state: failed to allocate memory for reading long SSID");
			nvs_close(handle);
			return WifiCredsState::ERROR;
		}
		size_t temp_len = required_len;
		esp_err = nvs_get_blob(handle, "ssid", temp_buf, &temp_len);
		if (esp_err == ESP_OK) {
			memcpy(ssid, temp_buf, 32);
			len = 32;
		}
		free(temp_buf);
	}
	nvs_close(handle);

	if(esp_err != ESP_OK)
	{
		ESP_LOGE(TAG, "wifi_manager_creds_state: failed to read SSID from NVS, err: %s", esp_err_to_name(esp_err));
		return WifiCredsState::ERROR;
	}

	ssid[len] = '\0';
	ESP_LOGI(TAG, "wifi_manager_creds_state: read SSID from NVS: '%s' (required_len: %d, read_len: %d)", ssid, (int)required_len, (int)len);

	return (strlen(ssid) > 0) ? WifiCredsState::PRESENT : WifiCredsState::ABSENT;
}

/******************************************************************************
* Get save WiFi manager credentials
******************************************************************************/
RetResult wifi_manager_get_creds(char ssid[WIFI_SSID_STRING_SIZE],
								 char pass[WIFI_PASSWORD_STRING_SIZE])
{
	nvs_handle handle;
	esp_err_t esp_err;
	memset(ssid, 0, WIFI_SSID_STRING_SIZE);
	memset(pass, 0, WIFI_PASSWORD_STRING_SIZE);

	esp_err = nvs_open("espwifimgr", NVS_READONLY, &handle);
	if (esp_err != ESP_OK)
	{
		ESP_LOGE(TAG, "wifi_manager_get_creds: failed to open NVS namespace 'espwifimgr', err: %s", esp_err_to_name(esp_err));
		return RET_ERROR;
	}

	// Get SSID
	size_t required_ssid_len = 0;
	esp_err = nvs_get_blob(handle, "ssid", NULL, &required_ssid_len);
	if (esp_err != ESP_OK)
	{
		ESP_LOGE(TAG, "wifi_manager_get_creds: failed to query SSID from NVS, err: %s", esp_err_to_name(esp_err));
		nvs_close(handle);
		return RET_ERROR;
	}

	size_t ssid_read_len = (required_ssid_len < 32) ? required_ssid_len : 32;
	if (required_ssid_len <= 32) {
		esp_err = nvs_get_blob(handle, "ssid", ssid, &ssid_read_len);
	} else {
		char *temp_buf = (char *)malloc(required_ssid_len);
		if (temp_buf == NULL) {
			nvs_close(handle);
			return RET_ERROR;
		}
		size_t temp_len = required_ssid_len;
		esp_err = nvs_get_blob(handle, "ssid", temp_buf, &temp_len);
		if (esp_err == ESP_OK) {
			memcpy(ssid, temp_buf, 32);
			ssid_read_len = 32;
		}
		free(temp_buf);
	}

	if (esp_err != ESP_OK)
	{
		ESP_LOGE(TAG, "wifi_manager_get_creds: failed to read SSID from NVS, err: %s", esp_err_to_name(esp_err));
		nvs_close(handle);
		return RET_ERROR;
	}
	ssid[ssid_read_len] = '\0';
	ESP_LOGI(TAG, "wifi_manager_get_creds: read SSID blob of length %d", (int)ssid_read_len);

	// Get pass
	size_t required_pass_len = 0;
	esp_err = nvs_get_blob(handle, "password", NULL, &required_pass_len);
	if (esp_err != ESP_OK)
	{
		ESP_LOGW(TAG, "wifi_manager_get_creds: failed to query password from NVS, err: %s. Defaulting to empty.", esp_err_to_name(esp_err));
		memset(pass, 0, WIFI_PASSWORD_STRING_SIZE);
	}
	else
	{
		size_t pass_read_len = (required_pass_len < 64) ? required_pass_len : 64;
		if (required_pass_len <= 64) {
			esp_err = nvs_get_blob(handle, "password", pass, &pass_read_len);
		} else {
			char *temp_buf = (char *)malloc(required_pass_len);
			if (temp_buf != NULL) {
				size_t temp_len = required_pass_len;
				esp_err = nvs_get_blob(handle, "password", temp_buf, &temp_len);
				if (esp_err == ESP_OK) {
					memcpy(pass, temp_buf, 64);
					pass_read_len = 64;
				}
				free(temp_buf);
			} else {
				esp_err = ESP_ERR_NO_MEM;
			}
		}

		if (esp_err != ESP_OK)
		{
			ESP_LOGW(TAG, "wifi_manager_get_creds: failed to read password from NVS, err: %s. Defaulting to empty.", esp_err_to_name(esp_err));
			memset(pass, 0, WIFI_PASSWORD_STRING_SIZE);
		}
		else
		{
			pass[pass_read_len] = '\0';
			ESP_LOGI(TAG, "wifi_manager_get_creds: read password blob of length %d", (int)pass_read_len);
		}
	}

	ESP_LOGI(TAG, "wifi_manager_get_creds: SSID is '%s', password length: %d", ssid, (int)strlen(pass));
	nvs_close(handle);

	return RET_OK;
}

/******************************************************************************
* Get system tick in mS. Provided by ESP32 timer, not FreeRTOS
* @return
******************************************************************************/
int64_t get_tick_ms()
{
	return esp_timer_get_time() / 1000;
}

/******************************************************************************
 * @brief Format a UNIX timestamp into a date/time string
 * @param buff Buffer to store the formatted date/time string
 * @param buff_size Size of the output buffer
 * @param ts Input timestamp (seconds since epoch)
 * @param format strftime-compatible format string
 * @param set_local_tz If true, apply local timezone offset from Settings::get_tz_offset()
 ******************************************************************************/
void get_formatted_datetime(char* buff, size_t buff_size, uint32_t ts, const char* format, bool set_local_tz)
{
	struct tm tmstruct = {};
	time_t now = ts;
	if(set_local_tz) now += Settings::get_tz_offset() * 3600;
	localtime_r(&now, &tmstruct);
	strftime(buff, buff_size, format, &tmstruct);
}

/******************************************************************************
* Get system tick in seconds. Provided by ESP32 timer, not FreeRTOS
* @return
******************************************************************************/
u32 get_tick_sec()
{
	return get_tick_ms() / 1000;
}


/******************************************************************************
* Compare version strings. Version strings must be formatted as:
* major.minor.build
* @param v_a First version string
* @param v_b Second version string
* @return
*	-1 if version a < b
	*	+1 if version a > b
*	0  if version a == b
*	-2 on parse error (eg. invalid format)
******************************************************************************/
int version_compare(char *v_a, char *v_b)
{
	int parts_v1[3] = {};
	int parts_v2[3] = {};

	if(sscanf(v_a, "%d.%d.%d", &parts_v1[2], &parts_v1[1], &parts_v1[0]) != 3 ||
	   sscanf(v_b, "%d.%d.%d", &parts_v2[2], &parts_v2[1], &parts_v2[0]) != 3)
	{
		// Failed
		return -2;
	}

	for (int i = 2; i >= 0; --i)
	{
		// V1 older
		if(parts_v1[i] < parts_v2[i])
			return -1;
		// V1 newer
		else if(parts_v1[i] > parts_v2[i])
			return 1;
	}

	// Equal
	return 0;
}

/******************************************************************************
* Draw BSOD with message before leaving this world
******************************************************************************/
void die(const char *msg)
{
	ESP_LOGE(TAG, "Critical Error, BSOD placeholder");
	// xSemaphoreTake(Drivers::display_get_mutex(), portMAX_DELAY);

	// lv_obj_clean(lv_scr_act());

	// // BSOD screen
	// auto bsod = lv_scr_act();

	// // Style: White, 20
	// static lv_style_t medium_text;
	// lv_style_init(&medium_text);
	// lv_style_set_text_font(&medium_text, LV_STATE_DEFAULT, &lv_font_montserrat_20);
	// lv_style_set_text_color(&medium_text, LV_STATE_DEFAULT, LV_COLOR_WHITE);

	// // Style: White, 16
	// static lv_style_t small_text;
	// lv_style_init(&small_text);
	// lv_style_set_text_font(&small_text, LV_STATE_DEFAULT, &lv_font_montserrat_16);
	// lv_style_set_text_color(&small_text, LV_STATE_DEFAULT, LV_COLOR_WHITE);

	// // Sad face
	// lv_obj_t *lbl_sad = lv_label_create(bsod, NULL);
	// lv_label_set_text_static(lbl_sad, ":-(");
	// lv_label_set_align(lbl_sad, LV_LABEL_ALIGN_CENTER);
	// lv_obj_align(lbl_sad, bsod, LV_ALIGN_IN_TOP_LEFT, 30, 25);

	// // White, large
	// static lv_style_t face_style;
	// lv_style_init(&face_style);
	// lv_style_set_text_font(&face_style, LV_STATE_DEFAULT, &lv_font_montserrat_48);
	// lv_style_set_text_color(&face_style, LV_STATE_DEFAULT, LV_COLOR_WHITE);
	// lv_obj_add_style(lbl_sad, LV_OBJ_PART_MAIN, &face_style);

	// // Something went wrong
	// lv_obj_t *lbl_wrong = lv_label_create(bsod, NULL);
	// lv_label_set_text_static(lbl_wrong, "Someone's under\nthe weather");
	// lv_label_set_align(lbl_wrong, LV_LABEL_ALIGN_LEFT);
	// lv_obj_align(lbl_wrong, lbl_sad, LV_ALIGN_OUT_RIGHT_MID, 35, 0);
	// lv_obj_add_style(lbl_wrong, LV_OBJ_PART_MAIN, &medium_text);

	// // A problem has been detected
	// lv_obj_t *lbl_descr = lv_label_create(bsod, NULL);
	// lv_label_set_text_static(lbl_descr, "A problem has been detected");
	// lv_label_set_align(lbl_descr, LV_LABEL_ALIGN_CENTER);
	// lv_obj_align(lbl_descr, bsod, LV_ALIGN_CENTER, 0, 0);
	// lv_obj_add_style(lbl_descr, LV_OBJ_PART_MAIN, &small_text);

	// // Msg
	// if(msg != NULL)
	// {
	// 	lv_obj_t *lbl_msg = lv_label_create(bsod, NULL);
	// 	lv_label_set_long_mode(lbl_msg, LV_LABEL_LONG_BREAK);
	// 	lv_obj_set_size(lbl_msg, 250, 10);
	// 	lv_label_set_text_static(lbl_msg, msg);
	// 	lv_label_set_align(lbl_msg, LV_LABEL_ALIGN_LEFT);
	// 	lv_obj_align(lbl_msg, lbl_descr, LV_ALIGN_IN_TOP_LEFT, 0, 50);
	// 	lv_obj_add_style(lbl_msg, LV_OBJ_PART_MAIN, &small_text);
	// }

	// // Set bkg color last
	// static lv_style_t bg_style;
	// lv_style_init(&bg_style);

	// lv_style_set_bg_color(&bg_style, LV_STATE_DEFAULT, lv_color_hex(0x82));  // Original BSOD
	// // lv_style_set_bg_color(&bg_style, LV_STATE_DEFAULT, lv_color_hex(0x79D8));   // Zoomer BSOD
	// lv_obj_add_style(bsod, LV_OBJ_PART_MAIN, &bg_style);

	// xSemaphoreGive(Drivers::display_get_mutex());

	// vTaskSuspendAll();
	// while(1){
	// 	// vTaskDelay(pdMS_TO_TICKS(1000))
	// }
}


/******************************************************************************
* Print memory info (for debugging)
******************************************************************************/
void print_mem_info()
{
	printf("\033[%dm", 34);
	printf("Memory info------\n");
	printf("Heap free: %.2f\n", (float)esp_get_free_heap_size() / 1024);
	printf("Heap min: %.2f\n", (float)esp_get_minimum_free_heap_size() / 1024);
	printf("Int. heap free: %.2f\n", (float)esp_get_free_internal_heap_size() / 1024);
	printf("Int. heap min: %.2f\n", (float)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024);
	printf("Free heap 8bit: %.2f\n", (float)heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024);

	printf("PSRAM size: %.2f\n", (float)heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024);
	printf("PSRAM free: %.2f\n", (float)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
	printf("--------------------\n");

	printf("\033[%dm", 0);
}

/********************************************************************************
 * Get ESP32 mac address string
 * @param out Output buffer. Should be at least 18 bytes long
 * @param size Buff size
 * @return
 *  RET_OK - success
 *  RET_ERROR - failure
 ********************************************************************************/
RetResult get_mac_addr_str(char *buff, int buff_size)
{
	if(buff == NULL || buff_size < 18)
	{
		return RET_ERROR;
	}

	uint8_t mac[6];

	if(esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK)
		return RET_ERROR;

	snprintf(buff, buff_size,  "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	return RET_OK;
}

/******************************************************************************
* Start the diagnostics task
******************************************************************************/
void start_diag_monitor_task()
{
	#ifdef WXM_ENV_DEBUG

	EXT_RAM_BSS_ATTR static StackType_t task_stack[DIAG_MONITOR_TASK_STACK_SIZE] = {};
	static StaticTask_t task_buff;

	xTaskCreateStaticPinnedToCore(diag_monitor_task, DIAG_MONITOR_TASK_NAME, sizeof(task_stack), NULL,
		DIAG_MONITOR_TASK_PRIORITY, task_stack, &task_buff, 1);

	#endif
}

/******************************************************************************
* Prints diagnostics on predefined intervals
* For debugging use only
******************************************************************************/
void diag_monitor_task(void *param)
{
	while(1)
	{
		Utils::print_mem_info();
		wait_ms(5000);
	}
}

/******************************************************************************
* Gracefully restart ESP32 after stopping TB and Wifi
******************************************************************************/
RetResult restart(RestartReason reason)
{
	ESP_LOGW(TAG, "Restarting device: reason=%s", restart_reason_to_str(reason));

	ESP_LOGW(TAG, "Stopping TB");
	Tb::stop();
	Tb::wait_disconnected(3000);

	ESP_LOGW(TAG, "Stopping Wifi");
	Wifi::disconnect();
	Wifi::wait_disconnected(2000);

	Utils::wait_ms(2000);

	safe_restart(reason);
}

static void wait_for_factory_reset_button_release()
{
#ifndef CONFIG_SCREENLESS
	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_reset_pin(GPIO_NUM_38));
	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_direction(GPIO_NUM_38, GPIO_MODE_INPUT));
	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_pull_mode(GPIO_NUM_38, GPIO_PULLUP_ONLY));

	uint32_t low_wait_ms = 0;
	uint32_t stable_high_ms = 0;
	while(stable_high_ms < 100)
	{
		if(gpio_get_level(GPIO_NUM_38) == 0)
		{
			stable_high_ms = 0;
			if(low_wait_ms == 0 || (low_wait_ms % 5000) == 0)
			{
				ESP_LOGE(TAG,
						 "GPIO38 is LOW; deferring restart to prevent bootloader factory reset");
			}
			low_wait_ms += 10;
		}
		else
		{
			stable_high_ms += 10;
		}
		vTaskDelay(pdMS_TO_TICKS(10));
	}
#endif
}

[[noreturn]] void safe_restart(RestartReason reason)
{
	ESP_LOGW(TAG, "Safe restart requested: reason=%s", restart_reason_to_str(reason));

	append_diagnostic_event("restart", (int)reason, (int)esp_reset_reason(),
							restart_reason_to_str(reason));

	// GPIO38 is an active-low physical button on WG1200 and is also used by
	// newer bootloaders for factory reset. Never restart while it is grounded.
	wait_for_factory_reset_button_release();

	s_restart_breadcrumb.magic = RESTART_BREADCRUMB_MAGIC;
	s_restart_breadcrumb.reason = (uint8_t)reason;
	s_restart_breadcrumb.reason_inverse = (uint8_t)~s_restart_breadcrumb.reason;
	s_restart_breadcrumb.reserved = 0;

	prepare_safe_restart_gpios();
	esp_restart();

	// esp_restart() should not return. Preserve the watchdog fallback while
	// keeping GPIO10 deasserted and avoiding output contention on GPIO38.
	esp_task_wdt_config_t wdt_config = {
		.timeout_ms = 1000,
		.idle_core_mask = 1,
		.trigger_panic = true
	};
	esp_task_wdt_init(&wdt_config);
	esp_task_wdt_add(NULL);
	while(true)
	{
		gpio_set_level(GPIO_NUM_10, 1);
		gpio_set_direction(GPIO_NUM_38, GPIO_MODE_INPUT);
		gpio_set_pull_mode(GPIO_NUM_38, GPIO_PULLUP_ONLY);
		esp_rom_delay_us(1000);
	}
}

/******************************************************************************
* Round number to X decimals
* @param val Value to round
* @param decimals Number of decimals to round to
* @return rounded value - use double to avoid serializeJson() bug
******************************************************************************/
double round_num(float val, int decimals)
{
	// Pow is already used elsewhere in the code so use it again
	double mul = pow(10, decimals);
	return (int)(val * mul + 0.5) / mul;
}

/******************************************************************************
* Generate the SHA256 digest of input data using the mbedTLS crypto lib
* Improved performance compared to calc_sha256_hw()
* @param data Input data
* @param length Length of input data
* @param hash Digest output
******************************************************************************/
RetResult calc_sha256_sw(const uint8_t* data, size_t length, uint8_t* hash)
{
	const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	if (md_info == NULL)
	{
		ESP_LOGE(TAG, "Failed to get MD info for SHA256");
		return RET_ERROR;
	}
	if (mbedtls_md(md_info, data, length, hash) != 0)
	{
		ESP_LOGE(TAG, "Hashing failed.");
		return RET_ERROR;
	}
	return RET_OK;
}

/******************************************************************************
* Get currently active MQTT broker.
* Release and debug builds both use the production broker. The debug profile
* changes diagnostics, not cloud routing.
* Must ALWAYS return a broker
* @return Pointer to the broker descriptor
******************************************************************************/
const MqttBroker* get_active_broker_desc()
{
	// Index 0 is required to be the production broker in every build profile.
	return &TB_MQTT_BROKERS[0];
}

void prepare_safe_restart_gpios()
{
	ESP_LOGI(TAG, "Preparing GPIO 10 and 38 for safe restart...");
	// GPIO10 is LCD RGB data G0 and the factory-reset input on historical
	// bootloaders. Disconnect it from the RGB matrix, force it high with input
	// sampling enabled, then hold the pad through the system reset. The input
	// path lets the bootloader sample the physical high level even though its
	// own pad reconfiguration is blocked by the hold.
	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_reset_pin(GPIO_NUM_10));
	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_direction(GPIO_NUM_10, GPIO_MODE_INPUT_OUTPUT));
	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level(GPIO_NUM_10, 1));
#ifndef CONFIG_SCREENLESS
	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_hold_en(GPIO_NUM_10));
#endif

	// GPIO38 is an active-low physical button and the factory-reset input on
	// newer bootloaders. Do not drive push-pull high against a pressed switch.
	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_reset_pin(GPIO_NUM_38));
	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_direction(GPIO_NUM_38, GPIO_MODE_INPUT));
	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_pull_mode(GPIO_NUM_38, GPIO_PULLUP_ONLY));

	// Give GPIO10 and the internal pull-up time to settle before reset.
	esp_rom_delay_us(100000);
}

extern "C" void __real_esp_panic_handler(void *info);
extern "C" void IRAM_ATTR __wrap_esp_panic_handler(void *info)
{
#ifndef CONFIG_SCREENLESS
	// CRITICAL DEFENSE-IN-DEPTH:
	// If a panic occurs while the ST7701 LCD interface is active, GPIO 10 (G0)
	// may be driven LOW by the LCD controller. Historical bootloaders (e.g. v0.1.9)
	// sample GPIO 10 on boot and trigger a factory reset (NVS wipe) if held LOW for 5s.
	// We disconnect GPIO 10 from the LCD peripheral, force it HIGH, and engage
	// hardware pad hold across the reset so the bootloader sees a release level.
	esp_rom_gpio_connect_out_signal(GPIO_NUM_10, SIG_GPIO_OUT_IDX, false, false);
	gpio_ll_input_enable(&GPIO, GPIO_NUM_10);
	gpio_ll_output_enable(&GPIO, GPIO_NUM_10);
	gpio_ll_set_level(&GPIO, GPIO_NUM_10, 1);
	gpio_ll_hold_en(&GPIO, GPIO_NUM_10);
#endif
	__real_esp_panic_handler(info);
}

static char s_bootloader_version[128] = "Unknown";
static char s_bootloader_sha256[65] = "unknown";
static bool s_bootloader_version_detected = false;

struct KnownBootloader
{
	const char *sha256;
	const char *label;
};

static const KnownBootloader KNOWN_BOOTLOADERS[] = {
	// Historical v0.1.9 image recovered from a field device.
	{"31c3a3d743ce824c3fa114c1e05e289465d5500e8cea62eac2631c1c6b6e57ee",
	 "v0.1.9 / GPIO10"},
	// Known GPIO38 field images from the bootloader recovery archive.
	{"bc0f49810f8e333a7e6712caf6224698d9dbaf8bf569bdeb6b4400a7c5aef654",
	 "known / GPIO38"},
	{"64ba89f6b26a7f52450662789d0560c75582a0af275c7d3555e2a92d19252c94",
	 "known / GPIO38"},
	// Phase 1 signed wg1200_release bootloader, including erased flash padding
	// through the 0xC000 partition-table boundary.
	{"bac3b27142904850f4be0598254a819a50be1c0a1b46a3b8d0ed7b594d45db23",
	 "phase1 release / GPIO38"},
	{"a882989b968f695e67b95ec71bd36eda1dff431bbb238d7acb4b6a7ecb5044c6",
	 "phase1 debug / GPIO38"},
};

static void detect_bootloader()
{
	if(s_bootloader_version_detected)
	{
		return;
	}

	// Hash the complete region before the partition table at 0xC000. Unlike
	// compile-date guessing, a hash only classifies binaries we have verified.
	const size_t bootloader_region_size = 0xC000;
	const size_t chunk_size = 2048;
	uint8_t *chunk = (uint8_t *)malloc(chunk_size);
	if (!chunk)
	{
		s_bootloader_version_detected = true;
		return;
	}

	const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	char compile_date[12] = "Unknown";
	bool date_found = false;
	bool read_ok = true;

	const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	mbedtls_md_context_t md_ctx;
	mbedtls_md_init(&md_ctx);
	bool hash_started = md_info != nullptr &&
		mbedtls_md_setup(&md_ctx, md_info, 0) == 0 &&
		mbedtls_md_starts(&md_ctx) == 0;

	for(size_t offset = 0; offset < bootloader_region_size; offset += chunk_size)
	{
		size_t bytes_to_read = std::min(chunk_size, bootloader_region_size - offset);
		esp_err_t err = esp_flash_read(nullptr, chunk, offset, bytes_to_read);
		if (err != ESP_OK)
		{
			read_ok = false;
			break;
		}

		if(hash_started && mbedtls_md_update(&md_ctx, chunk, bytes_to_read) != 0)
		{
			hash_started = false;
		}

		// Retain the compile date as human-readable supporting information.
		for(size_t i = 0; !date_found && i + 11 < bytes_to_read; i++)
		{
			bool month_match = false;
			for (int m = 0; m < 12; m++)
			{
				if (strncmp((char *)&chunk[i], months[m], 3) == 0)
				{
					month_match = true;
					break;
				}
			}

			if (month_match)
			{
				// Match format: "Mmm[ ][ |D]D[ ]20YY" -> e.g. "Dec  5 2024" or "Jul 19 2026"
				if (chunk[i+3] == ' ' && 
					(chunk[i+4] == ' ' || (chunk[i+4] >= '0' && chunk[i+4] <= '9')) &&
					(chunk[i+5] >= '0' && chunk[i+5] <= '9') &&
					chunk[i+6] == ' ' &&
					chunk[i+7] == '2' &&
					chunk[i+8] == '0' &&
					(chunk[i+9] >= '0' && chunk[i+9] <= '9') &&
					(chunk[i+10] >= '0' && chunk[i+10] <= '9'))
				{
					snprintf(compile_date, sizeof(compile_date), "%.11s", (char *)&chunk[i]);
					date_found = true;
					break;
				}
			}
		}
	}

	uint8_t digest[32] = {};
	if(read_ok && hash_started && mbedtls_md_finish(&md_ctx, digest) == 0)
	{
		for(size_t i = 0; i < sizeof(digest); i++)
		{
			snprintf(&s_bootloader_sha256[i * 2], 3, "%02x", digest[i]);
		}
		s_bootloader_sha256[64] = '\0';
	}
	mbedtls_md_free(&md_ctx);
	free(chunk);

	const char *known_label = nullptr;
	for(const KnownBootloader &known : KNOWN_BOOTLOADERS)
	{
		if(strcmp(s_bootloader_sha256, known.sha256) == 0)
		{
			known_label = known.label;
			break;
		}
	}

	if(known_label != nullptr)
	{
		snprintf(s_bootloader_version, sizeof(s_bootloader_version),
				 "%s (%s, sha=%.8s)", known_label, compile_date, s_bootloader_sha256);
	}
	else
	{
		snprintf(s_bootloader_version, sizeof(s_bootloader_version),
				 "unknown pin (%s, sha=%.8s)", compile_date, s_bootloader_sha256);
	}

	s_bootloader_version_detected = true;
	ESP_LOGW(TAG, "Bootloader inventory: %s", s_bootloader_version);
}

const char* get_bootloader_version()
{
	detect_bootloader();
	return s_bootloader_version;
}

const char* get_bootloader_sha256()
{
	detect_bootloader();
	return s_bootloader_sha256;
}

}
