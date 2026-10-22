/******************************************************************************
* All wifi related routines, event handler.
******************************************************************************/

#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "string.h"
#include "wifi.h"
#include "ws/include/web_server.h"
#include "mdns_discovery.h"
#include "env.h"
#include "const.h"
#include "global_state.h"
#include "tb.h"
#include "netdb.h"
#include "lwip/dns.h"
#include "time.h"
#include "utils.h"
#include "global_events.h"
#include "log.h"
#include "datetime_rtc.h"
#include "wifi_config_mode.h"
#include "esp_attr.h"

#ifndef WIFI_REASON_ASSOC_EXPIRE
#define WIFI_REASON_ASSOC_EXPIRE WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY
#endif

namespace Wifi
{

///////////////////////////////////////////////////////////////////////////////
// Local vars
///////////////////////////////////////////////////////////////////////////////

/** Logger tag */
static const char TAG[] = "WiFi";

/** STA Netif */
esp_netif_t *_netif = NULL;

/** Manager task handle*/
static TaskHandle_t _manager_task_handle = NULL;

/** Task stack */
EXT_RAM_BSS_ATTR StackType_t _manager_task_stack[WIFI_MANAGER_WXM_TASK_STACK_SIZE] = {};

/** Task buffer */
StaticTask_t _manager_task_buff;

/* @brief Software timer to wait for ip.
 * There is a state where ESP32 is waiting for ip and nothing handles this state*/
TimerHandle_t _wifi_waiting_for_ip_timer = NULL;

/** Software timer to check for internet connectivity */
TimerHandle_t _check_internet_timer = NULL;

/** Flag to track initialization */
bool _is_wifi_sta_initialized = false;

/** Flag to track start/stop process */
bool _is_wifi_sta_started = false;

/** Flag to enable/disable auto reconnect feature */
bool _auto_reconnect = false;

/** Current status of the WiFi */
Wifi::WifiStatus _wifi_status = Wifi::WifiStatus::NONE;

/** Data of last successful connection */
uint8_t _last_disconnection_reason;

/** Data of last successful ip acquisition */
ip_event_got_ip_t _last_ip_data;

/** fallback dns server */
ip_addr_t _secondary_dns;

/** Status changed timestamp */
uint32_t _last_status_changed_ts = 0;

/** Manager notification queue (prevents single-slot xTaskNotify drops) */
static constexpr size_t MANAGER_QUEUE_LEN = 16;
static QueueHandle_t _manager_queue = NULL;
static StaticQueue_t _manager_queue_buff;
static uint8_t _manager_queue_storage[MANAGER_QUEUE_LEN * sizeof(ManagerNotification)] = {};

/** Persistent DNS resolve worker task (stack in PSRAM to avoid runtime internal DRAM exhaustion) */
static constexpr uint32_t DNS_RESOLVE_TASK_STACK_SIZE = 4096;
static TaskHandle_t _dns_resolve_task_handle = NULL;
EXT_RAM_BSS_ATTR static StackType_t _dns_resolve_task_stack[DNS_RESOLVE_TASK_STACK_SIZE] = {};
static StaticTask_t _dns_resolve_task_buff;

/** current ssid */
char _wifi_ssid[Utils::WIFI_SSID_STRING_SIZE] = "";

/** Consecutive failed internet checks in the current outage. */
uint32_t _consecutive_internet_failures = 0;

/** Number of active connection recoveries attempted in the current outage. */
uint32_t _internet_recovery_attempts = 0;

/** Timestamp of the first failed internet check in the current outage. */
uint32_t _internet_outage_start_ts = 0;

/** Timestamp of the latest active connection recovery. */
uint32_t _last_internet_recovery_ts = 0;

/** Suppress the normal disconnect callback while deliberately restarting the driver. */
bool _driver_restart_in_progress = false;

/**
 * RTC state prevents a sustained external outage from causing a reboot loop.
 * It survives esp_restart(), and is cleared only after internet connectivity
 * succeeds or power is removed.
 */
struct InternetRecoveryRtcState
{
	uint32_t magic;
	uint32_t rebooted_during_outage;
};

static const uint32_t INTERNET_RECOVERY_RTC_MAGIC = 0x57494649; // "WIFI"
static RTC_NOINIT_ATTR InternetRecoveryRtcState _internet_recovery_rtc;

static_assert(WIFI_INTERNET_FAILURES_BEFORE_RECOVERY > 0,
			  "Internet recovery must require at least one failed check");
static_assert(WIFI_INTERNET_DRIVER_RESTART_AFTER_ATTEMPT <
			  (sizeof(WIFI_INTERNET_RECOVERY_BACKOFF_MS) /
			   sizeof(WIFI_INTERNET_RECOVERY_BACKOFF_MS[0])),
			  "Driver restart escalation must have a configured backoff");
static_assert(WIFI_INTERNET_MIN_RECOVERIES_BEFORE_REBOOT >
			  WIFI_INTERNET_DRIVER_RESTART_AFTER_ATTEMPT,
			  "A device reboot must happen only after driver restart recovery");

///////////////////////////////////////////////////////////////////////////////
// Local functions
///////////////////////////////////////////////////////////////////////////////

/** Wifi event handler */
void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);

/** Manager task handler */
void manager_task(void *data);

/** Send message to manager task */
RetResult notify_manager_task(ManagerNotification notification);

/** Callaback that is called when STA state is connected but didn't get IP after X time */
void wait_ip_timeout_cb(TimerHandle_t xTimer);

/** Callaback that is called to check internet connectivity */
void check_internet_cb(TimerHandle_t xTimer);

/** Asynchronous DNS resolution */
void dns_resolve_async();

/** FreeRTOS DNS resolve task function */
static void dns_resolve_task(void *pvParameters);

/** Print common disconnect reasons */
RetResult print_disconnect_reason(uint16_t reason);

/** Reset all recovery counters after a successful internet check. */
void reset_internet_recovery_state();

/** Record a failed internet check and schedule the next recovery action. */
void handle_internet_check_failure();

/** Restart the complete station driver, then reconnect if auto-reconnect is enabled. */
RetResult restart_sta_driver_sync();

/** Get the configured delay for a recovery attempt. */
uint32_t get_internet_recovery_backoff_ms(uint32_t attempt);

/******************************************************************************
 * @brief Set wifi status.
 * @param wifi_status WiFi status.
 ******************************************************************************/
void set_status(Wifi::WifiStatus wifi_status)
{
	if(wifi_status != _wifi_status)
	{
		// If for any reason disconnected or connected, capture timestamp.
		if(_wifi_status == WifiStatus::CONNECTED || wifi_status == WifiStatus::DISCONNECTED)
		{
			_last_status_changed_ts = Utils::get_tick_sec();
		}
	}
	_wifi_status = wifi_status;
}

/******************************************************************************
* Initialize WiFi
* @return
*   RET_OK on success
******************************************************************************/
RetResult init()
{
	if(_is_wifi_sta_initialized)
		return RET_INVALID_STATE;
	ESP_LOGI(TAG, "Init WiFi");
	ESP_ERROR_CHECK(esp_netif_init());
	_netif = esp_netif_create_default_wifi_sta();
	if(!_netif)
		return RET_ERROR;

	wifi_init_config_t default_config = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_init(&default_config));

	// Set events
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, 0));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, 0));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &event_handler, NULL, 0));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

	// Station mode
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

	// A lot more stable when power save is disabled, less AUTH_EXPIRE errors, less AP_NOT_FOUND errors
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

	// Create IP timeout timer
	_wifi_waiting_for_ip_timer = xTimerCreate( NULL, pdMS_TO_TICKS(WIFI_GET_IP_WAIT_MS), pdFALSE, ( void * ) 0, wait_ip_timeout_cb);
	// Create check internet timer
	_check_internet_timer = xTimerCreate( NULL, pdMS_TO_TICKS(WIFI_CHECK_INTERNET_EVERY_MS), pdTRUE, ( void * ) 0, check_internet_cb);
	if(_wifi_waiting_for_ip_timer == NULL || _check_internet_timer == NULL)
	{
		ESP_LOGE(TAG, "Could not create Wi-Fi recovery timers");
		if(_wifi_waiting_for_ip_timer != NULL)
		{
			xTimerDelete(_wifi_waiting_for_ip_timer, 0);
			_wifi_waiting_for_ip_timer = NULL;
		}
		if(_check_internet_timer != NULL)
		{
			xTimerDelete(_check_internet_timer, 0);
			_check_internet_timer = NULL;
		}
		return RET_ERROR;
	}

	// Start in state DISCONNECTED but INITIALIZED
	auto evg = GlobalState::get_event_group();
	xEventGroupSetBits(evg, GlobalState::EV_WIFI_DISCONNECTED_BIT);

	// Create manager notification queue before starting manager task
	_manager_queue = xQueueCreateStatic(MANAGER_QUEUE_LEN, sizeof(ManagerNotification),
		_manager_queue_storage, &_manager_queue_buff);
	if(_manager_queue == NULL)
	{
		ESP_LOGE(TAG, "Could not create WiFi manager queue.");
		return RET_ERROR;
	}

	// Create persistent DNS worker task (stack in PSRAM)
	_dns_resolve_task_handle = xTaskCreateStatic(dns_resolve_task, "dns_res_task",
		sizeof(_dns_resolve_task_stack), NULL, WIFI_MANAGER_WXM_TASK_PRIORITY,
		_dns_resolve_task_stack, &_dns_resolve_task_buff);
	if(_dns_resolve_task_handle == NULL)
	{
		ESP_LOGE(TAG, "Could not create DNS resolve task.");
		return RET_ERROR;
	}

	// Create manager task
	_manager_task_handle = xTaskCreateStatic(manager_task, WIFI_MANAGER_WXM_TASK_NAME, sizeof(_manager_task_stack), NULL,
		WIFI_MANAGER_WXM_TASK_PRIORITY, _manager_task_stack, &_manager_task_buff);
	if(_manager_task_handle == NULL)
	{
		ESP_LOGE(TAG, "Could not create WiFi manager task.");
		return RET_ERROR;
	}
	_wifi_status = WifiStatus::DISCONNECTED;
	_last_status_changed_ts = 0;
	_last_disconnection_reason = 0;
	_consecutive_internet_failures = 0;
	_internet_recovery_attempts = 0;
	_internet_outage_start_ts = 0;
	_last_internet_recovery_ts = 0;
	_driver_restart_in_progress = false;
	if(_internet_recovery_rtc.magic != INTERNET_RECOVERY_RTC_MAGIC)
	{
		_internet_recovery_rtc.magic = INTERNET_RECOVERY_RTC_MAGIC;
		_internet_recovery_rtc.rebooted_during_outage = 0;
	}
	_is_wifi_sta_initialized = true;
	ESP_LOGI(TAG, "WiFi initialized");
	return RET_OK;
}

char *get_ssid()
{
	return &_wifi_ssid[0];
}

bool has_ip()
{
	return (_wifi_status == WifiStatus::CONNECTED || _wifi_status == WifiStatus::CONNECTED_NO_INTERNET) &&
	       _last_ip_data.ip_info.ip.addr != 0;
}

char *get_ip_str()
{
	static char ip_str[IP4ADDR_STRLEN_MAX];
	if (has_ip())
	{
		strcpy(ip_str, ip4addr_ntoa((ip4_addr_t*)&(_last_ip_data.ip_info.ip)));
		return &ip_str[0];
	}
	return (char *)"0.0.0.0";
}


/******************************************************************************
* Deinit WiFi
* @return
*   RET_OK on success
******************************************************************************/
RetResult deinit()
{
	esp_netif_destroy(_netif);
	esp_netif_deinit();
	// Free tasks and queue
	if(_manager_task_handle != NULL)
		vTaskDelete(_manager_task_handle);
	_manager_task_handle = NULL;

	if(_dns_resolve_task_handle != NULL)
		vTaskDelete(_dns_resolve_task_handle);
	_dns_resolve_task_handle = NULL;

	if(_manager_queue != NULL)
		vQueueDelete(_manager_queue);
	_manager_queue = NULL;

	// Free timers
	if(_wifi_waiting_for_ip_timer)
		xTimerDelete(_wifi_waiting_for_ip_timer, 0);
	if(_check_internet_timer)
		xTimerDelete(_check_internet_timer, 0);

	auto evg = GlobalState::get_event_group();
	xEventGroupClearBits(evg, GlobalState::EV_WIFI_CONNECTED_BIT);
	//EV_WIFI_CONNECTED_BIT == EV_WIFI_DISCONNECTED_BIT basically means uknown state to the outer event world
	xEventGroupClearBits(evg, GlobalState::EV_WIFI_DISCONNECTED_BIT);
	_is_wifi_sta_initialized = false;
	set_status(Wifi::WifiStatus::NONE);
	return RET_OK;
}

/*******************************************************************************
 * Connect to network
 * @return
 *   RET_OK on success
 ******************************************************************************/
RetResult connect(char *ssid, char *pass, bool autoreconnect)
{
	ESP_LOGD(TAG, "Public connect called");
	if(!_is_wifi_sta_initialized)
	{
		ESP_LOGE(TAG, "Can't connect, wifi not initialized");
		return RET_INVALID_STATE;
	}

    // Get creds from wifi manager
	char wifi_ssid[Utils::WIFI_SSID_STRING_SIZE] = "";
	char wifi_pass[Utils::WIFI_PASSWORD_STRING_SIZE] = "";
	if(ssid != nullptr && pass != nullptr)
	{
		if(strnlen(ssid, sizeof(wifi_ssid)) >= sizeof(wifi_ssid) ||
		   strnlen(pass, sizeof(wifi_pass)) >= sizeof(wifi_pass))
		{
			ESP_LOGE(TAG, "WiFi credentials exceed the supported length");
			return RET_INVALID_PARAMS;
		}
		strncpy(wifi_ssid, ssid, sizeof(wifi_ssid) - 1);
		strncpy(wifi_pass, pass, sizeof(wifi_pass) - 1);
	}
	else
	{
		Utils::wifi_manager_get_creds(wifi_ssid, wifi_pass);
		if(wifi_ssid[0] == '\0')
		{
			ESP_LOGW(TAG, "WiFi credentials not found in NVS, falling back to compile-time default credentials.");
			strncpy(wifi_ssid, WIFI_DEFAULT_SSID, sizeof(wifi_ssid) - 1);
			strncpy(wifi_pass, WIFI_DEFAULT_PASS, sizeof(wifi_pass) - 1);
			wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
			wifi_pass[sizeof(wifi_pass) - 1] = '\0';
		}
	}

	wifi_config_t wifi_config = {};
	memcpy(wifi_config.sta.ssid, wifi_ssid, strnlen(wifi_ssid, sizeof(wifi_ssid)));
	strncpy(_wifi_ssid, wifi_ssid, sizeof(_wifi_ssid) - 1);
	memcpy(wifi_config.sta.password, wifi_pass, strnlen(wifi_pass, sizeof(wifi_pass)));

	wifi_config.sta.bssid_set = false;
	wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
	wifi_config.sta.threshold.rssi = -127;
	// Scan every channel before choosing. This is slower only while connecting,
	// but reliably handles replacement APs and multiple APs sharing an SSID.
	wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
	wifi_config.sta.pmf_cfg.capable = true;
	wifi_config.sta.pmf_cfg.required = false;
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

	if(_wifi_status == Wifi::WifiStatus::CONNECTING || _wifi_status == Wifi::WifiStatus::CONNECTED ||
		_wifi_status == Wifi::WifiStatus::CONNECTED_NO_IP || _wifi_status == Wifi::WifiStatus::CONNECTED_NO_INTERNET)
		return RET_OK;
	if(_wifi_status == Wifi::WifiStatus::DISCONNECTING)
		return RET_INVALID_STATE;
	_auto_reconnect = autoreconnect;
	return notify_manager_task(ManagerNotification::ORDER_CONNECT_STA);
}

/*******************************************************************************
 * Disconnect from network or stop connection process
 * @return
 *   RET_OK on success
 ******************************************************************************/
RetResult disconnect()
{
	ESP_LOGD(TAG, "Public disconnect called");
	if(!_is_wifi_sta_initialized)
	{
		ESP_LOGE(TAG, "Can't disconnect, wifi not initialized");
		return RET_INVALID_STATE;
	}
	if(_wifi_status == Wifi::WifiStatus::DISCONNECTING || _wifi_status == Wifi::WifiStatus::DISCONNECTED)
		return RET_OK;

	_auto_reconnect = false;
	if(_wifi_status == Wifi::WifiStatus::CONNECTING || _wifi_status == Wifi::WifiStatus::CONNECTION_FAILED)
	{
		_wifi_status = Wifi::WifiStatus::DISCONNECTING;
		return RET_OK;
	}
	return notify_manager_task(ManagerNotification::ORDER_DISCONNECT_STA);
}

/*******************************************************************************
 * Synchronous connection process. This function won't return until connection is established
 * or disconnect is called.
 * @return
 *   RET_OK on success
 ******************************************************************************/
RetResult connect_sync()
{
	ESP_LOGI(TAG, "Connection process started");
	if(!_is_wifi_sta_initialized) return RET_INVALID_STATE;

	//Get config for printing
	wifi_config_t wifi_config = {};
	esp_wifi_get_config(WIFI_IF_STA, &wifi_config);

	uint16_t connect_tries = 0;
	//Use it carefully
	do
	{
		// WiFi start stop state is a synced process despite event_handler having an event of it
		if(!_is_wifi_sta_started)
		{
			if(esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK)
				return RET_ERROR;
			if(esp_wifi_start() != ESP_OK)
				return RET_ERROR;
			_is_wifi_sta_started = true;
			// esp_wifi_start resets power save to WIFI_PS_MIN_MODEM by default; disable modem sleep explicitly
			esp_wifi_set_ps(WIFI_PS_NONE);
		}
		_wifi_status = Wifi::WifiStatus::CONNECTING;
		ESP_LOGI(TAG, "Connecting to SSID: %s", wifi_config.sta.ssid);
		esp_err_t connect_ret = esp_wifi_connect();
		if(connect_ret != ESP_OK)
		{
			ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(connect_ret));
			_wifi_status = Wifi::WifiStatus::CONNECTION_FAILED;
		}
		uint32_t wait_start = Utils::get_tick_sec();
		while(_wifi_status == Wifi::WifiStatus::CONNECTING)
		{
			Utils::wait_ms(1000);
			if (Utils::get_tick_sec() - wait_start > 30) // 30 seconds safety timeout
			{
				ESP_LOGE(TAG, "Connecting timed out!");
				_wifi_status = Wifi::WifiStatus::CONNECTION_FAILED;
				break;
			}
		}
			
		if(_wifi_status == Wifi::WifiStatus::CONNECTED_NO_IP || 
		   _wifi_status == Wifi::WifiStatus::CONNECTED || 
		   _wifi_status == Wifi::WifiStatus::CONNECTED_NO_INTERNET)
		{
			ESP_LOGI(TAG, "Connected to SSID: %s", wifi_config.sta.ssid);
			break;
		}
		else if(_wifi_status == Wifi::WifiStatus::CONNECTION_FAILED)
		{
			ESP_LOGE(TAG, "Connection failed!");
			print_disconnect_reason(_last_disconnection_reason);
			if(_last_disconnection_reason == WIFI_REASON_AUTH_EXPIRE || _last_disconnection_reason == WIFI_REASON_ASSOC_EXPIRE)
			{
				esp_wifi_disconnect();
				if(esp_wifi_stop() == ESP_OK)
				{
					// esp_wifi_stop() is synchronous; do not depend on delivery
					// order of WIFI_EVENT_STA_STOP before the next attempt.
					_is_wifi_sta_started = false;
				}
			}
			connect_tries++;
			if(connect_tries > WIFI_RECONNECT_ATTEMPTS)
			{
#ifdef CONFIG_SCREENLESS
				ESP_LOGE(TAG, "Connection failed after %d attempts. Rebooting into Wi-Fi Config Mode...", WIFI_RECONNECT_ATTEMPTS);
				WifiConfigMode::set_connection_failed_on_next_boot();
				Utils::safe_restart(Utils::RestartReason::WIFI_CONFIG_REQUEST);
#else
				ESP_LOGW(TAG, "Waiting %d mS before new connection attempt", WIFI_CONNECT_RETRY_INT_LONG_MS);
				Log::log(Log::Code::WIFI_CONNECTION_FAILED, _last_disconnection_reason);
				Utils::wait_ms(WIFI_CONNECT_RETRY_INT_LONG_MS);
				connect_tries = 0;
#endif
			}
			else
				Utils::wait_ms(WIFI_CONNECT_RETRY_INT_SHORT_MS);
			continue;
		}
		else if(_wifi_status == Wifi::WifiStatus::DISCONNECTING || _wifi_status == Wifi::WifiStatus::DISCONNECTED)
		{
			//Someone called disconnect, abort
			ESP_LOGW(TAG, "Connection process aborting");
			break;
		}
		else
		{
			ESP_LOGE(TAG, "Connection process in wrong state");
		}
	} while(_auto_reconnect);

	ESP_LOGI(TAG, "Connection process ended");
	//In this case disconnect was called while we were trying to connect, so change state to DISCONNECTED
	if(!_auto_reconnect && _wifi_status == Wifi::WifiStatus::DISCONNECTING)
	{
		set_status(Wifi::WifiStatus::DISCONNECTED);
	}

	return RET_OK;
}

/*******************************************************************************
 * Synchronous disconnection process
 * @return
 *   RET_OK on success
 ******************************************************************************/
RetResult disconnect_sync()
{
	ESP_LOGI(TAG, "Disconnection process started");
	set_status(Wifi::WifiStatus::DISCONNECTING);
	esp_wifi_disconnect();
	esp_wifi_stop();
	_is_wifi_sta_started = false;
	uint32_t wait_start = Utils::get_tick_sec();
	while(_wifi_status == Wifi::WifiStatus::DISCONNECTING)
	{
		Utils::wait_ms(1000);
		if((Utils::get_tick_sec() - wait_start) * 1000 >= WIFI_DRIVER_STOP_WAIT_MS)
		{
			ESP_LOGW(TAG, "Timed out waiting for Wi-Fi disconnect event; driver is already stopped");
			set_status(Wifi::WifiStatus::DISCONNECTED);
			break;
		}
	}
	if(_wifi_status != Wifi::WifiStatus::DISCONNECTED)
		return RET_ERROR;
	ESP_LOGI(TAG, "Disconnection process ended");
	return RET_OK;
}

/*******************************************************************************
 * Synchronous dns resolve of TB_MQTT_BROKER_HOSTNAME @ credentials.X.h
 * @return
 *   RET_OK on success
 ******************************************************************************/
RetResult dns_resolve_sync()
{
	struct addrinfo *address_info = NULL;
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
    ESP_LOGI(TAG, "Checking internet connectivity...");

	int res = getaddrinfo(DNS_RESOLVE_HOSTNAME, NULL, &hints, &address_info);

	// We should only apply DNS resolution results if we are currently in an active connected state.
	// If the state is CONNECTING, CONNECTED_NO_IP, or DISCONNECTED, we must abort.
	if(_wifi_status != Wifi::WifiStatus::CONNECTED && _wifi_status != Wifi::WifiStatus::CONNECTED_NO_INTERNET)
	{
		ESP_LOGW(TAG, "WiFi status is not CONNECTED/CONNECTED_NO_INTERNET (current: %d) during DNS resolution, aborting updates.", (int)_wifi_status);
		if (address_info != NULL) {
			freeaddrinfo(address_info);
		}
		return RET_ERROR;
	}

	auto evg = GlobalState::get_event_group();

	GlobalEvents::WifiStateChangedEvent wifi_event = GlobalEvents::WifiStateChangedEvent::NONE;
	
	//DNS resolve failed
	if(res != 0 || address_info == NULL)
	{
		ESP_LOGE(TAG, "No internet connection!");
		set_status(Wifi::WifiStatus::CONNECTED_NO_INTERNET);
			
		wifi_event = GlobalEvents::WifiStateChangedEvent::CONNECTED_NO_INTERNET;
		GlobalEvents::publish(GlobalEvents::EventId::WIFI_STATE_CHANGED, &wifi_event, sizeof(wifi_event));		
		
		Log::log(Log::Code::WIFI_CONNECTED_NO_INTERNET);		
		xEventGroupClearBits(evg, GlobalState::EV_WIFI_CONNECTED_BIT);
		xEventGroupSetBits(evg, GlobalState::EV_WIFI_DISCONNECTED_BIT);
		handle_internet_check_failure();
	}
	else //DNS resolve success
	{
		ESP_LOGI(TAG, "Connected to internet!");
		reset_internet_recovery_state();
		if(_wifi_status != Wifi::WifiStatus::CONNECTED)
		{
			set_status(Wifi::WifiStatus::CONNECTED);
			Log::log(Log::Code::WIFI_CONNECTED);
			if(get_sec_since_status_change() != 0) // first time connecting
			{
				ESP_LOGW(TAG, "WiFi downtime: %ds", get_sec_since_status_change());
				Log::logc(Log::Code::WIFI_DOWNTIME, get_sec_since_status_change());
			}	
			xEventGroupSetBits(evg, GlobalState::EV_WIFI_CONNECTED_BIT);
			xEventGroupClearBits(evg, GlobalState::EV_WIFI_DISCONNECTED_BIT);
			wifi_event = GlobalEvents::WifiStateChangedEvent::CONNECTED;
			GlobalEvents::publish(GlobalEvents::EventId::WIFI_STATE_CHANGED, &wifi_event, sizeof(wifi_event));
			DatetimeRtc::force_sync();
		}
	}

	if(address_info != NULL)
	{
		freeaddrinfo(address_info);
	}	

	return RET_OK;
}

/*******************************************************************************
 * Persistent FreeRTOS worker task to resolve DNS asynchronously without
 * blocking WiFi Manager task or allocating internal DRAM at runtime.
 ******************************************************************************/
static void dns_resolve_task(void *pvParameters)
{
	while(1)
	{
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		dns_resolve_sync();
	}
}

/*******************************************************************************
 * Asynchronous trigger for DNS resolution
 ******************************************************************************/
void dns_resolve_async()
{
	if (_dns_resolve_task_handle != NULL)
	{
		xTaskNotifyGive(_dns_resolve_task_handle);
	}
	else
	{
		ESP_LOGW(TAG, "DNS worker task unavailable, falling back to sync resolution");
		dns_resolve_sync();
	}
}

/*******************************************************************************
 * Main event handler for Wifi
 ******************************************************************************/
void event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
	auto evg = GlobalState::get_event_group();
	if(base == WIFI_EVENT)
	{
		if(id == WIFI_EVENT_STA_CONNECTED)
		{
			const wifi_event_sta_connected_t *connected =
				(const wifi_event_sta_connected_t *)event_data;
			ESP_LOGI(TAG, "Associated with BSSID %02x:%02x:%02x:%02x:%02x:%02x, "
					 "channel %u, auth mode %d",
					 connected->bssid[0], connected->bssid[1], connected->bssid[2],
					 connected->bssid[3], connected->bssid[4], connected->bssid[5],
					 connected->channel, (int)connected->authmode);
			set_status(Wifi::WifiStatus::CONNECTED_NO_IP);
			// TODO: Buff lock in log may introduce waiting in default loop
			Log::log(Log::Code::WIFI_CONNECTED_NO_IP);
			if(xTimerStart(_wifi_waiting_for_ip_timer, (TickType_t)0) != pdPASS)
			{
				ESP_LOGE(TAG, "Failed to start DHCP acquisition timer");
			}
		}
		else if(id == WIFI_EVENT_STA_DISCONNECTED)
		{
			xEventGroupClearBits(evg, GlobalState::EV_WIFI_CONNECTED_BIT);
			xEventGroupSetBits(evg, GlobalState::EV_WIFI_DISCONNECTED_BIT);
			_last_disconnection_reason =  ((wifi_event_sta_disconnected_t*) event_data)->reason;

			if(_wifi_status != Wifi::WifiStatus::CONNECTING)
			{
				set_status(Wifi::WifiStatus::DISCONNECTED);
				// TODO: Buff lock in log may introduce waiting in default loop
				Log::log(Log::Code::WIFI_DISCONNECTED, _last_disconnection_reason);
				xTimerStop(_check_internet_timer, (TickType_t) 0);
				xTimerStop(_wifi_waiting_for_ip_timer, (TickType_t) 0);
				if(!_driver_restart_in_progress &&
				   notify_manager_task(ManagerNotification::EVENT_STA_DISCONNECTED) != RET_OK)
					ESP_LOGE(TAG, "Failed to notify Manager Task about EVENT_STA_DISCONNECTED");
			}
			else
			{
				set_status(Wifi::WifiStatus::CONNECTION_FAILED);
			}
		}
		else if(id == WIFI_EVENT_STA_START)
		{
			_is_wifi_sta_started = true;
			esp_wifi_set_ps(WIFI_PS_NONE);
		}
		else if(id == WIFI_EVENT_STA_STOP)
			_is_wifi_sta_started = false;
		else
			ESP_LOGW(TAG, "WIFI_EVENT: %ld not handled.", id);
	}
	else if(base == IP_EVENT)
	{
		if(id == IP_EVENT_STA_GOT_IP)
		{
			esp_wifi_set_ps(WIFI_PS_NONE);
			xTimerStop(_wifi_waiting_for_ip_timer, (TickType_t) 0);
			if(xTimerStart(_check_internet_timer, (TickType_t)0) != pdPASS)
			{
				ESP_LOGE(TAG, "Failed to start internet check timer");
			}
			memcpy (&_last_ip_data, (ip_event_got_ip_t*) event_data, sizeof (_last_ip_data));
			if(notify_manager_task(ManagerNotification::EVENT_STA_GOT_IP) != RET_OK)
				ESP_LOGE(TAG, "Failed to notify Manager Task about EVENT_STA_GOT_IP");
		}
		else if(id == IP_EVENT_STA_LOST_IP)
		{
			if(_wifi_status == Wifi::WifiStatus::CONNECTED ||
			   _wifi_status == Wifi::WifiStatus::CONNECTED_NO_INTERNET)
			{
				ESP_LOGW(TAG, "Station lost its IP address; allowing DHCP %lu ms to recover",
						 WIFI_GET_IP_WAIT_MS);
				set_status(Wifi::WifiStatus::CONNECTED_NO_IP);
				Log::log(Log::Code::WIFI_CONNECTED_NO_IP);
				xEventGroupClearBits(evg, GlobalState::EV_WIFI_CONNECTED_BIT);
				xEventGroupSetBits(evg, GlobalState::EV_WIFI_DISCONNECTED_BIT);
				xTimerStop(_check_internet_timer, (TickType_t)0);
				if(xTimerReset(_wifi_waiting_for_ip_timer, (TickType_t)0) != pdPASS)
				{
					ESP_LOGE(TAG, "Failed to start lost-IP recovery timer; disconnecting now");
					if(notify_manager_task(ManagerNotification::EVENT_WAITING_IP_TIMEOUT) != RET_OK)
					{
						ESP_LOGE(TAG, "Failed to schedule immediate lost-IP recovery");
					}
				}

				if(notify_manager_task(ManagerNotification::EVENT_STA_LOST_IP) != RET_OK)
				{
					ESP_LOGE(TAG, "Failed to notify Manager Task about EVENT_STA_LOST_IP");
				}
			}
		}
	}
}

/******************************************************************************
* Reset internet recovery state after confirmed connectivity.
******************************************************************************/
void reset_internet_recovery_state()
{
	if(_consecutive_internet_failures > 0 || _internet_recovery_attempts > 0)
	{
		ESP_LOGI(TAG, "Internet recovered after %lu failed checks and %lu recovery actions",
				 _consecutive_internet_failures, _internet_recovery_attempts);
	}

	_consecutive_internet_failures = 0;
	_internet_recovery_attempts = 0;
	_internet_outage_start_ts = 0;
	_last_internet_recovery_ts = 0;
	_internet_recovery_rtc.magic = INTERNET_RECOVERY_RTC_MAGIC;
	_internet_recovery_rtc.rebooted_during_outage = 0;
}

/******************************************************************************
* Return the configured backoff for a recovery attempt.
******************************************************************************/
uint32_t get_internet_recovery_backoff_ms(uint32_t attempt)
{
	const size_t backoff_count =
		sizeof(WIFI_INTERNET_RECOVERY_BACKOFF_MS) /
		sizeof(WIFI_INTERNET_RECOVERY_BACKOFF_MS[0]);
	size_t index = attempt < backoff_count ? attempt : backoff_count - 1;
	return WIFI_INTERNET_RECOVERY_BACKOFF_MS[index];
}

/******************************************************************************
* Escalate a sustained internet outage without creating a reboot loop.
******************************************************************************/
void handle_internet_check_failure()
{
	uint32_t now = Utils::get_tick_sec();
	if(_consecutive_internet_failures == 0)
	{
		_internet_outage_start_ts = now;
	}
	_consecutive_internet_failures++;

	ESP_LOGW(TAG, "Internet check failure %lu (recovery actions: %lu)",
			 _consecutive_internet_failures, _internet_recovery_attempts);

	if(_consecutive_internet_failures < WIFI_INTERNET_FAILURES_BEFORE_RECOVERY)
	{
		return;
	}

	uint32_t outage_ms = (now - _internet_outage_start_ts) * 1000;
	if(outage_ms >= WIFI_INTERNET_REBOOT_AFTER_MS &&
	   _internet_recovery_attempts >= WIFI_INTERNET_MIN_RECOVERIES_BEFORE_REBOOT &&
	   _internet_recovery_rtc.rebooted_during_outage == 0)
	{
		if(notify_manager_task(ManagerNotification::ORDER_REBOOT_STA_RECOVERY) == RET_OK)
		{
			return;
		}
	}

	uint32_t backoff_ms = get_internet_recovery_backoff_ms(_internet_recovery_attempts);
	uint32_t since_recovery_ms = (now - _last_internet_recovery_ts) * 1000;
	if(_last_internet_recovery_ts != 0 && since_recovery_ms < backoff_ms)
	{
		ESP_LOGI(TAG, "Next internet recovery is backed off for another %lu ms",
				 backoff_ms - since_recovery_ms);
		return;
	}

	ManagerNotification action =
		_internet_recovery_attempts >= WIFI_INTERNET_DRIVER_RESTART_AFTER_ATTEMPT
			? ManagerNotification::ORDER_RESTART_STA
			: ManagerNotification::ORDER_RECOVER_STA;

	if(notify_manager_task(action) == RET_OK)
	{
		_last_internet_recovery_ts = now;
		_internet_recovery_attempts++;
	}
}

/******************************************************************************
* Stop and restart the full Wi-Fi station driver.
******************************************************************************/
RetResult restart_sta_driver_sync()
{
	bool reconnect = _auto_reconnect;
	ESP_LOGW(TAG, "Restarting complete Wi-Fi station driver");

	WebServer::stop();
	MdnsDiscovery::stop();
	GlobalEvents::WifiStateChangedEvent wifi_event =
		GlobalEvents::WifiStateChangedEvent::DISCONNECTED;
	GlobalEvents::publish(GlobalEvents::EventId::WIFI_STATE_CHANGED,
						  &wifi_event, sizeof(wifi_event));

	_driver_restart_in_progress = true;
	RetResult ret = disconnect_sync();
	_driver_restart_in_progress = false;

	if(ret != RET_OK)
	{
		ESP_LOGE(TAG, "Wi-Fi driver stop failed");
	}

	if(reconnect)
	{
		set_status(Wifi::WifiStatus::DISCONNECTED);
		return connect_sync();
	}
	return ret;
}

/******************************************************************************
* Callback for IP timer timeout. If it is called, waiting for IP in STA_CONNECTED state timed out.
******************************************************************************/
void wait_ip_timeout_cb( TimerHandle_t xTimer )
{
	/* stop the timer */
	//xTimerStop( xTimer, (TickType_t) 0 );
	/* message task */
	if(notify_manager_task(ManagerNotification::EVENT_WAITING_IP_TIMEOUT) != RET_OK)
		ESP_LOGE(TAG, "Failed to notify Manager Task about EVENT_WAITING_IP_TIMEOUT");
}

/******************************************************************************
* Callback to check internet connectivity
******************************************************************************/
void check_internet_cb(TimerHandle_t xTimer)
{
	if(notify_manager_task(ManagerNotification::ORDER_DNS_RESOLVE) != RET_OK)
		ESP_LOGE(TAG, "Failed to notify Manager Task about ORDER_DNS_RESOLVE");
}

/******************************************************************************
* Wait on wifi connected event bit
* @return
*   RET_OK on success
*   RET_TIMEOUT on wait timeout
******************************************************************************/
RetResult wait_connected(u32 timeout)
{
	EventBits_t ret = xEventGroupWaitBits(GlobalState::get_event_group(), GlobalState::EV_WIFI_CONNECTED_BIT, false, false, timeout);
	return ret & GlobalState::EV_WIFI_CONNECTED_BIT ? RET_OK : RET_TIMEOUT;
}

/******************************************************************************
* Wait on wifi disconnect event bit
* @return
*   RET_OK on success
*   RET_TIMEOUT on wait timeout
******************************************************************************/
RetResult wait_disconnected(u32 timeout)
{
	EventBits_t ret = xEventGroupWaitBits(GlobalState::get_event_group(), GlobalState::EV_WIFI_DISCONNECTED_BIT, false, false, timeout);
	return ret & GlobalState::EV_WIFI_DISCONNECTED_BIT ? RET_OK : RET_TIMEOUT;
}

/******************************************************************************
* Wifi connection status
* @return
*   true if connected to wifi, has IP and is connected to internet
******************************************************************************/
bool is_connected()
{
	auto evg = GlobalState::get_event_group();
	return evg != NULL && xEventGroupGetBits(evg) & GlobalState::EV_WIFI_CONNECTED_BIT ? true : false;
}

/*******************************************************************************
* Notify manager task. It could be an event or an order.
* @return
*   RET_OK if notification sent OK
******************************************************************************/
RetResult notify_manager_task(ManagerNotification notification)
{
	if(!_manager_queue)
	{
		ESP_LOGE(TAG, "manager_queue is NULL");
		return RET_ERROR;
	}
	if(xQueueSend(_manager_queue, &notification, 0) != pdTRUE)
	{
		ESP_LOGE(TAG, "Manager notification queue full (dropped %d)", (int)notification);
		return RET_INVALID_STATE;
	}
	return RET_OK;
}

/*******************************************************************************
* Get the status of the wifi
* @return
*   WifiStatus Status of the wifi type
******************************************************************************/
Wifi::WifiStatus get_status()
{
	return _wifi_status;
}

/*******************************************************************************
 * @brief Get how many seconds have passed since last status change.
 * @return
 *   uint32_t Seconds since last status change
 ******************************************************************************/
uint32_t get_sec_since_status_change()
{
	if(_last_status_changed_ts == 0)
	{
		return 0;
	}

	return Utils::get_tick_sec() - _last_status_changed_ts;
}

/******************************************************************************
* Get WiFi RSSI
******************************************************************************/
int get_rssi()
{
	if(WifiStatus::CONNECTED == _wifi_status || WifiStatus::CONNECTED_NO_INTERNET == _wifi_status || WifiStatus::CONNECTED_NO_IP == _wifi_status)
	{
		wifi_ap_record_t ap_info = {};
		if(esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
		{
			return ap_info.rssi;
		}
	}	

	return 0;
}

/*******************************************************************************
* Print reason of disconnection or connection fail at ESPLOG
* ESP-IDF https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi.html#wi-fi-reason-code
* 802.11 https://blogs.arubanetworks.com/industries/802-11-reason-codes-and-status-codes/
* @return
*   RET_ERROR string for this reason is not saved
*   RET_OK if reason string is found and printed
******************************************************************************/
RetResult print_disconnect_reason(uint16_t reason)
{
	const char * reason_ch;
	switch(reason)
	{
		case 201:
		{
			reason_ch = "AP_NOT_FOUND";
			break;
		}
		case 2:
		{
			reason_ch = "AUTH_EXPIRE";
			break;
		}
		case 4:
		{
			reason_ch = "ASSOC_EXPIRE";
			break;
		}
		case 6:
		{
			reason_ch = "NOT_AUTHED";
			break;
		}
		case 15:
		{
			reason_ch = "4WAY_HANDSHAKE_TIMEOUT";
			break;
		}
		case 202:
		{
			reason_ch = "AUTH_FAIL";
			break;
		}
		default:
		{
			ESP_LOGW(TAG, "Reason: %d", reason);
			return RET_OK;
		}
	}

	ESP_LOGW(TAG, "Reason: %s", reason_ch);
	return RET_OK;
}

/******************************************************************************
* WiFi manager task
* This task handles blocking processes and keeps alive the wifi connection.
* You can send notifications to this task using notify_manager_task.
* Have in mind that if manager task is blocked due to a process,
* notify can hold only one message until it is read by xTaskNotifyWait.
******************************************************************************/
void manager_task(void *data)
{
	ESP_LOGI(TAG, "Manager task started");
	ManagerNotification manager_notification;
	while(1)
	{
		BaseType_t ret = xQueueReceive(_manager_queue, &manager_notification, portMAX_DELAY);
		if(ret == pdFALSE)
		{
			ESP_LOGI(TAG, "xQueueReceive timed out");
			continue;
		}
		//ESP_LOGI(TAG, "Manager task got notification");
		switch(manager_notification)
		{
			case ManagerNotification::ORDER_CONNECT_STA:
			{
				if(_wifi_status != Wifi::WifiStatus::DISCONNECTED)
					break;
				connect_sync();
				break;
			}
			case ManagerNotification::ORDER_DISCONNECT_STA:
			{
				if(_wifi_status == Wifi::WifiStatus::DISCONNECTED)
					break;
				xTimerStop(_check_internet_timer, (TickType_t) 0);
				xTimerStop(_wifi_waiting_for_ip_timer, (TickType_t) 0);
				disconnect_sync();
				break;
			}
			case ManagerNotification::ORDER_DNS_RESOLVE:
			{
				dns_resolve_async();
				break;
			}
			case ManagerNotification::ORDER_RECOVER_STA:
			{
				if(_wifi_status != Wifi::WifiStatus::CONNECTED_NO_INTERNET)
					break;
				ESP_LOGW(TAG, "Refreshing Wi-Fi association and DHCP lease after internet outage");
				esp_err_t err = esp_wifi_disconnect();
				if(err != ESP_OK)
				{
					ESP_LOGE(TAG, "Recovery disconnect failed: %s", esp_err_to_name(err));
				}
				break;
			}
			case ManagerNotification::ORDER_RESTART_STA:
			{
				if(_wifi_status != Wifi::WifiStatus::CONNECTED_NO_INTERNET)
					break;
				restart_sta_driver_sync();
				break;
			}
			case ManagerNotification::ORDER_REBOOT_STA_RECOVERY:
			{
				if(_wifi_status != Wifi::WifiStatus::CONNECTED_NO_INTERNET)
					break;
				ESP_LOGE(TAG, "Internet unavailable for %lu ms after %lu recoveries; "
						 "performing the one guarded device restart for this outage",
						 (Utils::get_tick_sec() - _internet_outage_start_ts) * 1000,
						 _internet_recovery_attempts);
				_internet_recovery_rtc.magic = INTERNET_RECOVERY_RTC_MAGIC;
				_internet_recovery_rtc.rebooted_during_outage = 1;
				Utils::safe_restart(Utils::RestartReason::WIFI_RECOVERY);
				break;
			}
			case ManagerNotification::EVENT_STA_DISCONNECTED:
			{
				//print stuff
				ESP_LOGE(TAG, "Disconnected");
				memset(&_last_ip_data, 0, sizeof(_last_ip_data));
				WebServer::stop();
				MdnsDiscovery::stop();
				GlobalEvents::WifiStateChangedEvent wifi_event = GlobalEvents::WifiStateChangedEvent::DISCONNECTED;
				GlobalEvents::publish(GlobalEvents::EventId::WIFI_STATE_CHANGED, &wifi_event, sizeof(wifi_event));

				if(_auto_reconnect)
				{
					ESP_LOGW(TAG, "Trying to reconnect");
					connect_sync();
				}
				break;
			}
			case ManagerNotification::EVENT_STA_GOT_IP:
			{
				set_status(Wifi::WifiStatus::CONNECTED_NO_INTERNET);
				Log::log(Log::Code::WIFI_CONNECTED_NO_INTERNET);

				char ip_str[IP4ADDR_STRLEN_MAX];
				strcpy(ip_str, ip4addr_ntoa((ip4_addr_t*)&(_last_ip_data.ip_info.ip)));
				ESP_LOGI(TAG, "Got IP: %s", ip_str);
				printf("\n==================================\n");
				printf("  Wi-Fi CONNECTED SUCCESSFULLY!   \n");
				printf("  IP Address: %s\n", ip_str);
				printf("==================================\n\n");
				WebServer::start();
				MdnsDiscovery::start();
				// if DNS_FALLBACK_ADDRESS is not empty, use it as secondary dns
				if(strcmp(DNS_FALLBACK_ADDRESS, "") != 0)
				{
					inet_pton(AF_INET, DNS_FALLBACK_ADDRESS, &_secondary_dns);
					dns_setserver(1, &_secondary_dns);
				}
				//check_internet_timer has started but check right now, or we'll wait
				dns_resolve_async();
				break;
			}
			case ManagerNotification::EVENT_STA_LOST_IP:
			{
				WebServer::stop();
				MdnsDiscovery::stop();
				GlobalEvents::WifiStateChangedEvent wifi_event =
					GlobalEvents::WifiStateChangedEvent::DISCONNECTED;
				GlobalEvents::publish(GlobalEvents::EventId::WIFI_STATE_CHANGED,
									  &wifi_event, sizeof(wifi_event));
				break;
			}
			case ManagerNotification::EVENT_WAITING_IP_TIMEOUT:
			{
				ESP_LOGW(TAG, "Waiting for IP timed out. %lu ms passed after connection", WIFI_GET_IP_WAIT_MS);
				esp_wifi_disconnect();
				break;
			}
			default:
			{
				break;
			}
		}
	}
}

}
