#ifndef UTILS_H
#define UTILS_H
#include "common.h"
#include "struct.h"
#include "lvgl/lvgl.h"
#include "esp_err.h"
#include "esp_system.h"

namespace Utils
{

enum class RestartReason : uint8_t
{
	UNKNOWN = 0,
	GENERAL,
	CREDENTIAL_PROVISIONING,
	OTA_UPDATE,
	WEB_OTA_UPDATE,
	BOOT_GUARD_ROLLBACK,
	WIFI_CONFIG_REQUEST,
	WIFI_CONFIG_COMPLETE,
	WIFI_CONFIG_TIMEOUT,
	WIFI_RECOVERY,
	SOFT_WATCHDOG,
	SETTINGS_CHANGE,
	RECOVERY_EXIT,
	FACTORY_FIRMWARE_RESTORE,
	TEST_MODE_EXIT,
	NVS_INCOMPATIBLE,
	REMOTE_REBOOT,
	OTA_FAILED,
};

struct BootDiagnostics
{
	esp_reset_reason_t reset_reason;
	int raw_reset_reason_cpu0;
	int raw_reset_reason_cpu1;
	RestartReason controlled_restart_reason;
	bool bootloader_factory_reset;
	int gpio10_level_at_app_entry;
	int gpio38_level_at_app_entry;
	char running_partition[17];
	esp_err_t nvs_initial_result;
	esp_err_t nvs_final_result;
	size_t nvs_used_entries;
	size_t nvs_free_entries;
	size_t nvs_total_entries;
	size_t nvs_namespace_count;
};

void wait_ms(u32 ms);
void print_buff_hex(u8 *buff, int len);
RetResult buff_to_hex_str(uint8_t *buff, int buff_len, char *out, int out_size);
float deg_to_rad(float deg);
float rad_to_deg(float rad);

RetResult hex_str_to_byte_array(const char *hex_str, u8 *byte_array, int byte_array_len);

/** Pairing mode to lowercase string ("off"/"cloud"/"local"/"scan") */
const char *pair_mode_to_str(PairMode mode);

/** Parse pairing mode string, PAIR_MODE_NONE when unrecognized */
PairMode pair_mode_from_str(const char *str);

RetResult wifi_manager_clear_creds();

/** Extra byte is required because the ESP-IDF config fields allow 32/64 bytes
 * without a trailing NUL while the application treats credentials as strings. */
constexpr size_t WIFI_SSID_STRING_SIZE = 33;
constexpr size_t WIFI_PASSWORD_STRING_SIZE = 65;

RetResult wifi_manager_get_creds(char ssid[WIFI_SSID_STRING_SIZE],
								 char pass[WIFI_PASSWORD_STRING_SIZE]);

/** State of WiFi credentials stored in NVS */
enum class WifiCredsState
{
	PRESENT,  // Valid SSID stored
	ABSENT,   // Definitively not provisioned (key/namespace missing or SSID empty)
	ERROR     // Could not determine (NVS read error) - do NOT treat as absent
};

WifiCredsState wifi_manager_creds_state();

u32 get_tick_sec();

int64_t get_tick_ms();

void start_diag_monitor_task();

void die(const char *msg = NULL);

int version_compare(char *v_a, char *v_b);

RetResult get_efuse_blk3(char *buff, int buff_size);

RetResult get_mac_addr_str(char *buff, int buff_size);

RetResult set_idle_task_watchdog_state(bool enable);

void reverse_bytes(uint8_t *buff, size_t length);

RetResult restart(RestartReason reason = RestartReason::GENERAL);
void prepare_safe_restart_gpios();
[[noreturn]] void safe_restart(RestartReason reason = RestartReason::GENERAL);
const char *restart_reason_to_str(RestartReason reason);
void capture_early_boot_diagnostics();
void record_nvs_diagnostics(esp_err_t initial_result, esp_err_t final_result);
void append_diagnostic_event(const char *event, int value1 = 0, int value2 = 0,
							 const char *detail = nullptr);
const BootDiagnostics &get_boot_diagnostics();
const char* get_bootloader_version();
const char* get_bootloader_sha256();

void print_mem_info();

double round_num(float val, int decimals);

RetResult device_sign(uint8_t *data, size_t data_len, uint8_t *sig_buff, size_t sig_buff_size);
void rotate_screen(ScreenRotation scr_rot);
const ScreenRotationDesc *get_scr_rot_desc(char *name, ScreenRotation scr_rot);

RetResult calc_sha256_sw(const uint8_t* data, size_t length, uint8_t* hash);
RetResult calc_sha256_hw(const uint8_t* data, size_t length, uint8_t* hash);

void get_formatted_datetime(char* buff, size_t buff_size, uint32_t ts, const char* format, bool set_local_tz);

void print_boot_info();
bool normal_boot();

const MqttBroker* get_active_broker_desc();

/******************************************************************************
 * Find a descriptor in an array by provided member/value pair.
 * (array[x].member == val)
 * @param array Array of descriptor structs
 * @param size Size of array
 * @param member Struct member to match with value
 * @param val Value to match
 * @return
 * 	- Pointer to item in array if found
 * 	- nullptr if no member == value found
 ******************************************************************************/
template <typename T, typename U>
const T* lookup_desc_array(const T* array, size_t size, U T::*member, U val)
{
	for(size_t i = 0; i < size; ++i)
	{
		if(array[i].*member == val)
		{
			return &array[i];
		}
	}
	return nullptr; // Not found
}

/******************************************************************************
 * Find a descriptor in an array by provided member/value pair.
 * Overload for char arrays
 * (array[x].member == val)
 * @param array Array of descriptor structs
 * @param size Size of array
 * @param member Struct member to match with value
 * @param val Value to match
 * @return
 * 	- Pointer to item in array if found
 * 	- nullptr if no member == value found
 ******************************************************************************/
template <typename T>
const T* lookup_desc_array(const T* array, size_t size, const char* T::*member, const char* val)
{
	for(size_t i = 0; i < size; ++i)
	{
		if(strcmp(array[i].*member, val) == 0)
		{
			return &array[i];
		}
	}
	return nullptr; // Not found
}

}

#endif
