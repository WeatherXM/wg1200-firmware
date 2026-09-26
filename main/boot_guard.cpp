#include "boot_guard.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "nvs.h"
#include "log.h"
#include "utils.h"

/**
 * Boot guard: app-level OTA failure recovery.
 *
 * Field devices run bootloaders without CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
 * and bootloaders cannot be updated over OTA, so rollback after a bad OTA has
 * to be done by the app itself:
 *
 *  - Every crash-caused boot (panic, HW watchdog, soft watchdog) increments a
 *    counter in NVS. A power cycle resets it, reaching HEALTHY_UPTIME_SEC of
 *    uptime or connecting to Thingsboard clears it.
 *  - After MAX_CRASH_BOOTS consecutive crash boots the guard switches the boot
 *    partition back to the previous app slot (the firmware that ran before the
 *    OTA) and reboots. A record of the rollback is kept in NVS.
 *  - The record cannot be uploaded at rollback time (the device is crash
 *    looping), and the old firmware knows nothing about it. It is reported to
 *    Thingsboard (Log::Code::OTA_ROLLBACK) by the next firmware that boots
 *    healthy and calls mark_healthy(). Until then, the visible fleet signal is
 *    the device's firmware version attribute reverting on Thingsboard.
 */
namespace BootGuard
{

static const char TAG[] = "BootGuard";

/** Consecutive crash boots before rolling back to the previous app */
static const uint8_t MAX_CRASH_BOOTS = 3;

/** Uptime after which the current boot is considered healthy */
static const int HEALTHY_UPTIME_SEC = 600;

static const char NVS_NAMESPACE[] = "bootguard";
static const char NVS_KEY_CRASH_CNT[] = "crash_cnt";
static const char NVS_KEY_SWD_FLAG[] = "swd_flag";
static const char NVS_KEY_RB_CNT[] = "rb_cnt";
static const char NVS_KEY_RB_REASON[] = "rb_reason";
static const char NVS_KEY_RB_POSSIBLE[] = "rb_possible";

static esp_timer_handle_t _healthy_timer = NULL;

/******************************************************************************
* @brief Decide whether the previous boot ended in a crash
******************************************************************************/
static bool is_crash_reset(esp_reset_reason_t reason, bool swd_flag)
{
	if(swd_flag)
	{
		return true;
	}

	switch(reason)
	{
		case ESP_RST_PANIC:
		case ESP_RST_INT_WDT:
		case ESP_RST_TASK_WDT:
		case ESP_RST_WDT:
			return true;
		default:
			return false;
	}
}

/******************************************************************************
* @brief Store a rollback record to be reported once a healthy firmware runs
******************************************************************************/
static void store_rollback_record(nvs_handle_t handle, uint8_t crash_cnt,
								  esp_reset_reason_t reason, bool rollback_possible)
{
	nvs_set_u8(handle, NVS_KEY_RB_CNT, crash_cnt);
	nvs_set_u8(handle, NVS_KEY_RB_REASON, (uint8_t)reason);
	nvs_set_u8(handle, NVS_KEY_RB_POSSIBLE, rollback_possible ? 1 : 0);
	nvs_commit(handle);
}

/******************************************************************************
* @brief Roll back to the previous app partition if one with a valid image exists
* @retval true Rollback initiated (device will restart, does not return)
* @retval false No rollback possible
******************************************************************************/
static bool try_rollback(nvs_handle_t handle, uint8_t crash_cnt, esp_reset_reason_t reason)
{
	const esp_partition_t *running = esp_ota_get_running_partition();
	if(running == NULL ||
	   (running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
		running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1))
	{
		// Running from factory: nothing older to fall back to
		ESP_LOGE(TAG, "Crash loop detected but running from factory partition, cannot roll back.");
		return false;
	}

	const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
	esp_app_desc_t target_desc = {};
	if(target == NULL || esp_ota_get_partition_description(target, &target_desc) != ESP_OK)
	{
		ESP_LOGE(TAG, "Crash loop detected but no valid app in the other OTA slot, cannot roll back.");
		return false;
	}

	ESP_LOGE(TAG, "Crash loop detected (%d crash boots). Rolling back to previous firmware %s at %s.",
			 crash_cnt, target_desc.version, target->label);

	store_rollback_record(handle, crash_cnt, reason, true);
	nvs_set_u8(handle, NVS_KEY_CRASH_CNT, 0);
	nvs_commit(handle);

	// With secure boot enabled this also verifies the target image signature
	esp_err_t err = esp_ota_set_boot_partition(target);
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
		return false;
	}

	nvs_close(handle);
	Utils::safe_restart(Utils::RestartReason::BOOT_GUARD_ROLLBACK);
	return true; // Not reached
}

/******************************************************************************
* @brief Healthy-uptime timer callback
******************************************************************************/
static void healthy_timer_cb(void *arg)
{
	mark_healthy();
}

/******************************************************************************
* Init
******************************************************************************/
RetResult init()
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "Could not open NVS: %s", esp_err_to_name(err));
		return RET_ERROR;
	}

	// Soft watchdog resets look like plain SW resets, so the soft watchdog
	// leaves a flag in NVS before restarting
	uint8_t swd_flag = 0;
	nvs_get_u8(handle, NVS_KEY_SWD_FLAG, &swd_flag);
	if(swd_flag)
	{
		nvs_erase_key(handle, NVS_KEY_SWD_FLAG);
	}

	uint8_t crash_cnt = 0;
	nvs_get_u8(handle, NVS_KEY_CRASH_CNT, &crash_cnt);

	esp_reset_reason_t reason = esp_reset_reason();

	if(reason == ESP_RST_POWERON)
	{
		// A power cycle breaks the crash chain
		crash_cnt = 0;
	}
	else if(is_crash_reset(reason, swd_flag != 0))
	{
		crash_cnt++;
		ESP_LOGW(TAG, "Crash boot detected (reset reason: %d, soft WD: %d). Consecutive crash boots: %d/%d",
				 (int)reason, (int)swd_flag, crash_cnt, MAX_CRASH_BOOTS);
	}

	nvs_set_u8(handle, NVS_KEY_CRASH_CNT, crash_cnt);
	nvs_commit(handle);

	if(crash_cnt >= MAX_CRASH_BOOTS)
	{
		if(!try_rollback(handle, crash_cnt, reason))
		{
			// Record the event and clear the counter so the device keeps trying
			// to boot instead of counting forever
			store_rollback_record(handle, crash_cnt, reason, false);
			nvs_set_u8(handle, NVS_KEY_CRASH_CNT, 0);
			nvs_commit(handle);
		}
	}

	nvs_close(handle);

	// Consider the boot healthy after HEALTHY_UPTIME_SEC of uptime. Crash loops
	// die within seconds/minutes, so surviving this long clears the counter.
	const esp_timer_create_args_t timer_args = {
		.callback = healthy_timer_cb,
		.arg = NULL,
		.dispatch_method = ESP_TIMER_TASK,
		.name = "boot_guard",
		.skip_unhandled_events = true
	};
	if(esp_timer_create(&timer_args, &_healthy_timer) == ESP_OK)
	{
		esp_timer_start_once(_healthy_timer, (uint64_t)HEALTHY_UPTIME_SEC * 1000000ULL);
	}

	return RET_OK;
}

/******************************************************************************
* Mark current boot healthy and report any pending rollback record
******************************************************************************/
void mark_healthy()
{
	nvs_handle_t handle;
	if(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
	{
		return;
	}

	uint8_t crash_cnt = 0;
	nvs_get_u8(handle, NVS_KEY_CRASH_CNT, &crash_cnt);
	if(crash_cnt != 0)
	{
		ESP_LOGI(TAG, "Boot marked healthy, clearing crash counter (was %d).", crash_cnt);
		nvs_set_u8(handle, NVS_KEY_CRASH_CNT, 0);
		nvs_commit(handle);
	}

	// Report a pending rollback record. Kept until Log accepts it (needs valid
	// RTC time), then erased so it is reported exactly once.
	uint8_t rb_cnt = 0;
	if(nvs_get_u8(handle, NVS_KEY_RB_CNT, &rb_cnt) == ESP_OK)
	{
		uint8_t rb_reason = 0, rb_possible = 0;
		nvs_get_u8(handle, NVS_KEY_RB_REASON, &rb_reason);
		nvs_get_u8(handle, NVS_KEY_RB_POSSIBLE, &rb_possible);

		Log::Code code = rb_possible ? Log::Code::OTA_ROLLBACK : Log::Code::OTA_ROLLBACK_IMPOSSIBLE;
		ESP_LOGW(TAG, "Reporting pending rollback record (crash boots: %d, reset reason: %d, rolled back: %d)",
				 rb_cnt, rb_reason, rb_possible);

		if(Log::logc(code, rb_cnt, rb_reason) == RET_OK)
		{
			nvs_erase_key(handle, NVS_KEY_RB_CNT);
			nvs_erase_key(handle, NVS_KEY_RB_REASON);
			nvs_erase_key(handle, NVS_KEY_RB_POSSIBLE);
			nvs_commit(handle);
		}
	}

	nvs_close(handle);
}

/******************************************************************************
* Note a soft watchdog reset so the next boot counts as a crash boot
******************************************************************************/
void note_soft_wd_reset()
{
	nvs_handle_t handle;
	if(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
	{
		return;
	}
	nvs_set_u8(handle, NVS_KEY_SWD_FLAG, 1);
	nvs_commit(handle);
	nvs_close(handle);
}

}
