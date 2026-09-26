#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "nvs_sync.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "ui/include/ui_common.h"
#include "ui/views/include/ui_view_wifi_config.h"
#include "utils.h"

// Conflicts with AWS, get own namespace
namespace WifiManager
{
	#include "wifi_manager.h"
}

#include "wifi_config_mode.h"

#ifndef CONFIG_FORCE_AP_MODE
#define CONFIG_FORCE_AP_MODE 0  /* Force AP mode by default for dev purposes */
#endif

namespace WifiConfigMode
{
	//
	// Local vars
	//
	/** Logger tag */
	static const char TAG[] = "WifiConfigMode";

	/** If set, Wifi config mode is started on boot. Persists in NVS between resets */
	static bool _start_on_boot = false;

	/** If set, Wifi config mode is started on boot due to a connection failure. Preserves credentials. */
	static bool _connection_failed = false;

	UiViewWifiConfig::View _ui_view_wifi_config;

	//
	// Local functions
	//
	void ui_init();
	void on_sta_got_ip_handler(void *param);
	void on_ap_stop_handler(void *param);

#ifdef CONFIG_SCREENLESS
	extern "C" void set_led_status_ap_mode();
#endif

	/******************************************************************************
	* Init
	******************************************************************************/
	RetResult init()
	{
		ESP_ERROR_CHECK(nvs_sync_create());

		_start_on_boot = false;
		_connection_failed = false;

		nvs_handle_t handle;
		esp_err_t nvs_err = nvs_open("wifi_cfg", NVS_READONLY, &handle);
		if (nvs_err == ESP_OK)
		{
			uint8_t val = 0;
			if (nvs_get_u8(handle, "start_on_boot", &val) == ESP_OK)
			{
				_start_on_boot = (val != 0);
			}
			val = 0;
			if (nvs_get_u8(handle, "conn_failed", &val) == ESP_OK)
			{
				_connection_failed = (val != 0);
			}
			nvs_close(handle);
		}
		else if(nvs_err != ESP_ERR_NVS_NOT_FOUND)
		{
			ESP_LOGE(TAG, "Could not read Wi-Fi config boot flags: %s", esp_err_to_name(nvs_err));
		}

		ESP_LOGI(TAG, "wifi_config_mode init: _start_on_boot=%d, _connection_failed=%d", _start_on_boot, _connection_failed);

		return RET_OK;
	}

	void exit_btn_cb(lv_event_t *event)
	{
		lv_event_code_t  event_code = lv_event_get_code(event);		
		if(event_code == LV_EVENT_CLICKED)
		{
			ESP_LOGI(TAG, "Exit button clicked. Clearing config boot flags, setting bypass_wifi and restarting...");
			clear_boot_flags();
			nvs_handle_t handle;
			if (nvs_open("wifi_cfg", NVS_READWRITE, &handle) == ESP_OK)
			{
				nvs_set_u8(handle, "bypass_wifi", 1);
				nvs_commit(handle);
				nvs_close(handle);
			}
			vTaskDelay(pdMS_TO_TICKS(1000));
			Utils::safe_restart(Utils::RestartReason::WIFI_CONFIG_COMPLETE);
		}		
	}

	/******************************************************************************
	* Enter config mode if device was supposed to enter config mode on boot
	******************************************************************************/
	void handle()
	{
		bool is_conn_failed = false;
#if CONFIG_FORCE_AP_MODE
		ESP_LOGW(TAG, "CONFIG_FORCE_AP_MODE is enabled. Forcing AP mode on boot!");
		_start_on_boot = true;
#endif
		if(_start_on_boot == true)
		{
			ESP_LOGW(TAG, "Entering WIFI config mode.");
			is_conn_failed = _connection_failed;
			clear_boot_flags();
		}
		else
			return;

#ifdef CONFIG_SCREENLESS
		set_led_status_ap_mode();
#endif

		ESP_LOGW(TAG, "==================================================");
		ESP_LOGW(TAG, "ESP32 CONFIRMED ENTERING AP MODE! SSID: %s", CONFIG_DEFAULT_AP_SSID);
		ESP_LOGW(TAG, "==================================================");

#ifndef CONFIG_SCREENLESS
		ui_init();
#endif

		// Before starting wifi manager, always use pure AP mode so we preserve existing credentials
		// and allow exiting or configuring on demand. Configuration mode intentionally uses an
		// open AP: physical/user action already opted into provisioning, and an old stored AP
		// password must never prevent access to the captive portal.
		ESP_LOGW(TAG, "Entering Wi-Fi AP configuration mode. Preserving existing Wi-Fi credentials and using an open provisioning AP.");
		WifiManager::wifi_manager_pure_ap_mode = true;
		WifiManager::wifi_settings.ap_pwd[0] = '\0';

		WifiManager::wifi_manager_start();

		wifi_manager_set_callback(WifiManager::WM_EVENT_STA_GOT_IP, &on_sta_got_ip_handler);
		wifi_manager_set_callback(WifiManager::WM_ORDER_STOP_AP, &on_ap_stop_handler);
		

		// Prevent entering normal flow, but retry connecting every 5 minutes if we entered due to connection failure
		uint32_t seconds_waited = 0;
		while(1)
		{
			Utils::wait_ms(1000);
			if (is_conn_failed)
			{
				seconds_waited++;
				if (seconds_waited >= 300) // 5 minutes = 300 seconds
				{
					ESP_LOGW(TAG, "5 minutes elapsed in Auto-AP mode without configuration. Rebooting to retry connection with existing credentials...");
					clear_boot_flags();
					vTaskDelay(pdMS_TO_TICKS(1000));
					Utils::safe_restart(Utils::RestartReason::WIFI_CONFIG_TIMEOUT);
				}
			}
		}
	}

	/******************************************************************************
	* Init ui
	******************************************************************************/
	void ui_init()
	{
#ifndef CONFIG_SCREENLESS
		UiCommon::lock();
		_ui_view_wifi_config.create(NULL);
		_ui_view_wifi_config._btn_exit.set_cb(exit_btn_cb, NULL);
		UiCommon::unlock();
#endif
	}

	/******************************************************************************
	* Successfully connected to WiFi and got IP
	******************************************************************************/
	void on_sta_got_ip_handler(void *param)
	{
		ESP_LOGI(TAG, "Got IP");
		clear_boot_flags();
#ifndef CONFIG_SCREENLESS
		UiCommon::lock();
		_ui_view_wifi_config.update_status_text("Connected!\nRebooting...", lv_color_hex(0x0fd65f));
		UiCommon::unlock();
#endif

		wifi_mode_t mode;
		if (esp_wifi_get_mode(&mode) == ESP_OK)
		{
			if (mode == WIFI_MODE_STA)
			{
				ESP_LOGI(TAG, "SoftAP is not active. Rebooting into normal mode immediately...");
				vTaskDelay(pdMS_TO_TICKS(1000));
				Utils::safe_restart(Utils::RestartReason::WIFI_CONFIG_COMPLETE);
			}
			else
			{
				ESP_LOGI(TAG, "SoftAP is active. Waiting for background shutdown timer to stop AP and reboot...");
			}
		}
		else
		{
			ESP_LOGE(TAG, "Failed to get Wi-Fi mode. Rebooting as safeguard...");
			vTaskDelay(pdMS_TO_TICKS(1000));
			Utils::safe_restart(Utils::RestartReason::WIFI_CONFIG_COMPLETE);
		}
	}

	/******************************************************************************
	* Provisioning done
	******************************************************************************/
	void on_ap_stop_handler(void *param)
	{
		
		ESP_LOGI(TAG, "Restarting in normal mode");
		clear_boot_flags();
		vTaskDelay(pdMS_TO_TICKS(1000));
		Utils::safe_restart(Utils::RestartReason::WIFI_CONFIG_COMPLETE);
	}

	/******************************************************************************
	* Set to enter on next boot
	******************************************************************************/
	void set_on_next_boot()
	{
		_start_on_boot = true;
		_connection_failed = false;

		nvs_handle_t handle;
		esp_err_t nvs_err = nvs_open("wifi_cfg", NVS_READWRITE, &handle);
		if (nvs_err == ESP_OK)
		{
			nvs_err = nvs_set_u8(handle, "start_on_boot", 1);
			if(nvs_err == ESP_OK) nvs_err = nvs_set_u8(handle, "conn_failed", 0);
			if(nvs_err == ESP_OK) nvs_err = nvs_commit(handle);
			nvs_close(handle);
		}
		if(nvs_err != ESP_OK) ESP_LOGE(TAG, "Failed to persist Wi-Fi config-mode boot flag: %s", esp_err_to_name(nvs_err));
	}

	/******************************************************************************
	* Set to enter on next boot due to connection failure
	******************************************************************************/
	void set_connection_failed_on_next_boot()
	{
		_start_on_boot = true;
		_connection_failed = true;

		nvs_handle_t handle;
		esp_err_t nvs_err = nvs_open("wifi_cfg", NVS_READWRITE, &handle);
		if (nvs_err == ESP_OK)
		{
			nvs_err = nvs_set_u8(handle, "start_on_boot", 1);
			if(nvs_err == ESP_OK) nvs_err = nvs_set_u8(handle, "conn_failed", 1);
			if(nvs_err == ESP_OK) nvs_err = nvs_commit(handle);
			nvs_close(handle);
		}
		if(nvs_err != ESP_OK) ESP_LOGE(TAG, "Failed to persist connection-failed boot flag: %s", esp_err_to_name(nvs_err));
	}

	/******************************************************************************
	* Clear enter-on-boot flags to prevent wifi config from start on boot
	******************************************************************************/
	void clear_boot_flags()
	{
		_start_on_boot = false;
		_connection_failed = false;

		nvs_handle_t handle;
		esp_err_t nvs_err = nvs_open("wifi_cfg", NVS_READWRITE, &handle);
		if (nvs_err == ESP_OK)
		{
			nvs_err = nvs_erase_key(handle, "start_on_boot");
			if(nvs_err == ESP_ERR_NVS_NOT_FOUND) nvs_err = ESP_OK;
			esp_err_t conn_err = nvs_erase_key(handle, "conn_failed");
			if(conn_err != ESP_OK && conn_err != ESP_ERR_NVS_NOT_FOUND && nvs_err == ESP_OK) nvs_err = conn_err;
			if(nvs_err == ESP_OK) nvs_err = nvs_commit(handle);
			nvs_close(handle);
		}
		if(nvs_err != ESP_OK) ESP_LOGE(TAG, "Failed to clear Wi-Fi config-mode boot flags: %s", esp_err_to_name(nvs_err));
	}
}
