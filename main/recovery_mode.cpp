#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl/lvgl.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "ui/include/ui_common.h"
#include "ui/views/include/ui_view_recovery.h"
#include "recovery_mode.h"
#include "test_mode.h"
#include "utils.h"
#include "const.h"

namespace RecoveryMode
{

/******************************************************************************
* Local vars
******************************************************************************/
const char TAG[] = "Recovery mode";

/** Recovery mode events */
EventGroupHandle_t _evg = 0;

UiViewRecovery::View _view_recovery;

/******************************************************************************
* Local functions
******************************************************************************/
lv_obj_t* add_button(const char *title, void (*cb)(lv_obj_t* obj, lv_event_t event));
void on_btn_restore_fw(lv_event_t *event);
void set_boot_factory_partition();

// ============================================================================

/******************************************************************************
* Enter recovery mode
******************************************************************************/
void enter()
{
	ESP_LOGW(TAG, "Entering recovery mode");

	_evg = xEventGroupCreate();

	//
	// Prepare UI
	//
	UiCommon::lock();
	_view_recovery.create(NULL);

	_view_recovery._btn_restore_fw.set_cb(on_btn_restore_fw, NULL);

	_view_recovery._btn_enter_diagnostics.set_cb([](lv_event_t *event)
	{
		if (lv_event_get_code(event) == LV_EVENT_CLICKED)
		{
			xEventGroupSetBits(_evg, EV_ENTER_TEST_MODE_BIT);
		}
	}, NULL);

	_view_recovery._btn_exit.set_cb([](lv_event_t *event)
	{
		if (lv_event_get_code(event) == LV_EVENT_CLICKED)
		{
			Utils::safe_restart(Utils::RestartReason::RECOVERY_EXIT);
		}
	}, NULL);

	UiCommon::unlock();

	auto bits = xEventGroupWaitBits(_evg, EV_ENTER_TEST_MODE_BIT | EV_RESTORE_FW_BIT | EV_RECOVERY_MODE_FINISHED, 1, 0, portMAX_DELAY);
	if(bits & EV_ENTER_TEST_MODE_BIT)
	{
		ESP_LOGW(TAG, "Entering diagnostics mode");
		TestMode::enter();
	}
	else if(bits & EV_RESTORE_FW_BIT)
	{
		ESP_LOGW(TAG, "Restoring FW");
		set_boot_factory_partition();
	}

}


/******************************************************************************
* Btn click: Restore factory FWs
******************************************************************************/
void on_btn_restore_fw(lv_event_t *event)
{
	lv_event_code_t  event_code = lv_event_get_code(event);

	if(event_code != LV_EVENT_CLICKED)
	{
		return;
	}

	static const char *btns[] = {"Cancel", "Continue", ""};
	lv_obj_t * msgbox = lv_msgbox_create(NULL,
							"WARNING!",
							"This action will restore your firmware to factory version. \n\nContinue to this action?"							
							, btns, false);

	lv_obj_set_width(msgbox, UI_DEFAULT_MSGBOX_W);
	lv_obj_add_event_cb(msgbox, [](lv_event_t *event) {
		lv_obj_t *obj = lv_event_get_current_target(event);
		auto active_btn = lv_msgbox_get_active_btn(obj);

		if(active_btn == 0)
		{
			lv_msgbox_close_async(obj);
		}
		else
		{
			lv_msgbox_close_async(obj);
			xEventGroupSetBits(_evg, EV_RESTORE_FW_BIT);
		}
	}, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_align(msgbox, LV_ALIGN_CENTER, 0, 0);
}


/******************************************************************************
* Set factory partition as boot partition
******************************************************************************/
void set_boot_factory_partition()
{
	ESP_LOGW(TAG, "Restoring factory partition");
	const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
	if(!partition)
	{
		ESP_LOGE(TAG, "Factory partition not found.");
		return;
	}

	esp_err_t esp_ret = esp_ota_set_boot_partition(partition);
	if(esp_ret != ESP_OK)
	{
		ESP_LOGE(TAG, "Could not set factory as boot partition: %d", esp_ret);
		return;
	}

	ESP_LOGW(TAG, "Restarting");
	Utils::safe_restart(Utils::RestartReason::FACTORY_FIRMWARE_RESTORE);
}

}
