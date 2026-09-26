/**
 * @file tb.cpp
 * @brief Manages all TB operations and MQTT connectivity.
 * Not thread safe.
 * 
 */
#include "esp_log.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "global_events.h"
#include "global_state.h"
#include "const.h"
#include "mqtt_client.h"
#include "env.h"
#include "wifi.h"
#include "tb.h"
#include "esp_tls.h"
#include "log.h"
#include "utils.h"
#include "credentials.h"
#include "boot_guard.h"
#include <string_view>

#ifdef CONFIG_CELLULAR_BACKUP
#include "cellular.h"
#endif

extern uint32_t g_tb_success_count;
extern uint32_t g_tb_fail_count;

namespace Tb
{

/** Weatherxm.com self signed cert */
extern const uint8_t wxm_mqtt_pem_start[] asm("_binary_wxm_mqtt_pem_start");
extern const uint8_t wxm_mqtt_pem_end[] asm("_binary_wxm_mqtt_pem_end");

//
// Structs
//
enum ResponseState
{
	RESP_STATE_IDLE = 0,
	RESP_STATE_WAITING,
	RESP_STATE_RECEIVED
} _resp_state;

/** Buffer for responses received from MQTT.
 * Used for synchronus publish/receive with wait_response */
EXT_RAM_BSS_ATTR MqttResponse _mqtt_resp = {};

//
// Constants
//
static const char TAG[] = "TB";
/** Logger tag */

///////////////////////////////////////////////////////////////////////////////
// Local vars
///////////////////////////////////////////////////////////////////////////////

/** TB MQTT client handle */
static esp_mqtt_client_handle_t _mqtt_client = NULL;

/** Response message semaphore */
SemaphoreHandle_t _sem_resp_msg;
/** Static buffer for response message semaphore */
StaticSemaphore_t _sem_resp_msg_buff;

 /** Task waiting and needs to be notified on response */
TaskHandle_t _resp_waiting_task = NULL;

/** Keep alive task */
static TaskHandle_t _keep_alive_task_handle = NULL;

/** Task stack */
EXT_RAM_BSS_ATTR StackType_t _keep_alive_task_stack[TB_KEEPALIVE_TASK_SIZE] = {};

/** Task buffer */
StaticTask_t _keep_alive_task_buff;

/** Status of the mqtt*/
Tb::MqttStatus _mqtt_status = MqttStatus::NONE;

/** Status changed timestamp */
uint32_t _last_status_changed_ts = 0;

///////////////////////////////////////////////////////////////////////////////
// Local functions
///////////////////////////////////////////////////////////////////////////////

/** MQTT event handler */
void mqtt_ev_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

/** Keep alive task handler */
void keep_alive_task(void *data);

void set_status(MqttStatus status);
uint32_t get_sec_since_status_change();

/******************************************************************************
* Init
******************************************************************************/
RetResult init()
{
	esp_mqtt_client_config_t cfg = {};

	const auto broker = Utils::get_active_broker_desc();
	cfg.broker.address.uri = broker->addr;
	cfg.broker.address.port = broker->port;

    ESP_LOGI(TAG, "Host: %s:%d", cfg.broker.address.uri , cfg.broker.address.port);
	
	static char *cert_buf = NULL;
	if (cert_buf == NULL) {
		size_t cert_len = wxm_mqtt_pem_end - wxm_mqtt_pem_start;
		cert_buf = (char *)malloc(cert_len + 1);
		if (cert_buf == NULL) {
			ESP_LOGE(TAG, "Failed to allocate memory for MQTT broker certificate");
			return RET_ERROR;
		}
		memcpy(cert_buf, wxm_mqtt_pem_start, cert_len);
		cert_buf[cert_len] = '\0';
	}
	cfg.broker.verification.certificate = cert_buf;
	cfg.broker.verification.certificate_len = strlen(cert_buf) + 1;

	cfg.task.stack_size = TB_MQTT_TASK_SIZE;
	cfg.task.priority = TB_MQTT_TASK_PRIORITY;
	cfg.buffer.size = TB_MQTT_BUFFER_SIZE;
	cfg.buffer.out_size = TB_MQTT_BUFFER_SIZE;

	if (Credentials::get_dev_cert_length() == 0) {
		cfg.credentials.authentication.certificate = NULL;
		cfg.credentials.authentication.certificate_len = 0;
	} else {
		cfg.credentials.authentication.certificate = (const char*) Credentials::get_dev_cert();
		cfg.credentials.authentication.certificate_len = Credentials::get_dev_cert_length();
	}
	ESP_LOGI(TAG, "DS ctx: %p, Cert length: %lu", 
		Credentials::get_ds_ctx(), 
		(unsigned long)Credentials::get_dev_cert_length());
	if (Credentials::get_ds_ctx() == NULL) {
		static uint8_t *priv_key_buf = NULL;
		static size_t priv_key_len = 0;
		if (priv_key_buf == NULL) {
			if (Credentials::load_private_key_raw_from_nvs(&priv_key_buf, &priv_key_len) != RET_OK) {
				ESP_LOGE(TAG, "Failed to load virtual private key from NVS");
			}
		}
		if (priv_key_buf != NULL) {
			cfg.credentials.authentication.key = (const char *) priv_key_buf;
			cfg.credentials.authentication.key_len = priv_key_len;
			cfg.credentials.authentication.ds_data = NULL;
			ESP_LOGI(TAG, "Using software private key from NVS (length: %zu)", priv_key_len);
		} else {
			cfg.credentials.authentication.key = NULL;
			cfg.credentials.authentication.ds_data = NULL;
			// Safe fallback to prevent ESP_ERR_INVALID_STATE (0x0103) assertion crash in esp-tls
			cfg.credentials.authentication.certificate = NULL;
			cfg.credentials.authentication.certificate_len = 0;
			ESP_LOGE(TAG, "No software private key or DS context available! Mutual auth disabled to avoid ESP_ERR_INVALID_STATE.");
		}
	} else {
		cfg.credentials.authentication.key = NULL;
		cfg.credentials.authentication.ds_data = (void *) Credentials::get_ds_ctx();
		ESP_LOGI(TAG, "Using hardware Digital Signature (DS) context");
	}
	cfg.network.reconnect_timeout_ms = 10000;

	_mqtt_client = esp_mqtt_client_init(&cfg);
	if(_mqtt_client == NULL)
	{
		ESP_LOGE(TAG, "Could not init MQTT");
		return RET_ERROR;
	}

	/* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
	esp_mqtt_client_register_event(_mqtt_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_ev_handler, NULL);

	//
	// Create mutex for receive buffer
	//
	// Create mutex to lock buffers
	_sem_resp_msg = xSemaphoreCreateMutexStatic(&_sem_resp_msg_buff);
	if(_sem_resp_msg == NULL)
	{
		//Utils::die("Response message semaphore init failed.");
		return RET_ERROR;
	}

	//
	// Create keep alive task
	//
	_keep_alive_task_handle = xTaskCreateStaticPinnedToCore(keep_alive_task, TB_KEEPALIVE_TASK_NAME, sizeof(_keep_alive_task_stack), NULL,
		TB_KEEPALIVE_TASK_PRIORITY, _keep_alive_task_stack, &_keep_alive_task_buff, 1);

	if(_keep_alive_task_handle == NULL)
	{
		ESP_LOGE(TAG, "Could not create TB keepalive task.");
	}
	else
	{
		// Start suspended
		vTaskSuspend(_keep_alive_task_handle);
	}
	_mqtt_status = MqttStatus::DISCONNECTED;
	_last_status_changed_ts = 0;
	return RET_OK;
}

/******************************************************************************
* Stop MQTT
******************************************************************************/
RetResult mqtt_stop()
{
	if(!_mqtt_client)
	{
		ESP_LOGE(TAG, "Client was not created.");
		return RET_ERROR;
	}

	// If connecting, wait for it to settle (connected or failed/disconnected) before stopping
	int wait_limit = 50; // 5 seconds max
	while(get_status() == MqttStatus::CONNECTING && wait_limit-- > 0)
	{
		ESP_LOGI(TAG, "MQTT is connecting, waiting for state to settle... (rem: %d)", wait_limit);
		vTaskDelay(pdMS_TO_TICKS(100));
	}

	set_status(MqttStatus::DISCONNECTING);
	while(is_connected())
	{
		esp_mqtt_client_disconnect(_mqtt_client);
		vTaskDelay(pdMS_TO_TICKS(1000));
		//Utils::wait_ms(1000);
	}
	set_status(MqttStatus::DISCONNECTED);
	if(esp_mqtt_client_stop(_mqtt_client) != ESP_OK)
	{
		return RET_ERROR;
	}
	return RET_OK;
}

/******************************************************************************
* Start TB MQTT
******************************************************************************/
RetResult start()
{
	ESP_LOGI(TAG, "Starting TB"); 

	if (Settings::get_mqtt_disabled())
	{
		ESP_LOGI(TAG, "TB is disabled by settings, not starting");
		return RET_OK;
	}

	if(!_keep_alive_task_handle)
	{
		ESP_LOGE(TAG, "Task was not created.");
		return RET_ERROR;
	}
	vTaskResume(_keep_alive_task_handle);
	xTaskNotify(_keep_alive_task_handle, 1, eSetValueWithOverwrite);

	return RET_OK;
}

/******************************************************************************
* Stop TB MQTT
******************************************************************************/
RetResult stop()
{
	ESP_LOGI(TAG, "Stopping TB");

	if(!_keep_alive_task_handle)
	{
		ESP_LOGE(TAG, "Task was not created.");
		return RET_ERROR;
	}

	xTaskNotify(_keep_alive_task_handle, 2, eSetValueWithOverwrite);

	return RET_OK;
}

RetResult publish_telemetry(char *payload)
{
	if (Settings::get_mqtt_disabled())
	{
		ESP_LOGW(TAG, "MQTT disabled, ignoring telemetry publish");
		return RET_OK;
	}

	int ret = publish(TB_TELEMETRY_TOPIC, payload, TB_MQTT_TELEMETRY_QOS);
	if (ret < 0) {
		g_tb_fail_count++;
		return RET_ERROR;
	} else {
		g_tb_success_count++;
		return RET_OK;
	}
}

/******************************************************************************
* Wait for a response on a specific topic.
* ACHTUNG Not thread safe. Uses freeRTOS direct to task notifs which are only
* single notifier single receiver.
* @param resp Response output struct. Responses will be waited for only on topics
*   that -start- with the value in the topic of this struct, otherwise ignored.
*   On data receipt the topic will contain the actual topic on which the data 
*   was received.
* @param timeout Max time to wait before failing
* @return
*   RET_OK whne message received
*   RET_TIMEOUT timeout
*   RET_ERROR other errors
*   RET_INVALID_STATE already waiting for response
******************************************************************************/
/******************************************************************************
* Arm a response wait WITHOUT blocking.
*
* Split out of wait_response() so a caller can arm the wait BEFORE publishing
* the request it expects a reply to. wait_response() arms only after the caller
* has already published, which leaves a window where a fast reply arrives while
* the handler is still IDLE and is therefore dropped - costing a full response
* timeout and a retry.
*
* Pair with await_response(), or discard with cancel_wait_response().
* wait_response() is left untouched so existing callers are unaffected.
******************************************************************************/
RetResult start_wait_response(const char *topic, u32 timeout)
{
	if(_sem_resp_msg == NULL)
	{
		return RET_ERROR;
	}

	if(xSemaphoreTake(_sem_resp_msg, pdMS_TO_TICKS(timeout)) == pdFALSE)
	{
		return RET_TIMEOUT;
	}

	strncpy(_mqtt_resp.topic, topic, sizeof(_mqtt_resp.topic));
	_mqtt_resp.topic[sizeof(_mqtt_resp.topic) - 1] = '\0';
	_mqtt_resp.data_len = 0;
	_resp_waiting_task = xTaskGetCurrentTaskHandle();
	_resp_state = ResponseState::RESP_STATE_WAITING;

	// Drop any stale notification left over by a previously timed-out wait,
	// which would otherwise satisfy this wait instantly with no data.
	xTaskNotifyWait(0, ULONG_MAX, NULL, 0);

	xSemaphoreGive(_sem_resp_msg);

	return RET_OK;
}

/******************************************************************************
* Abandon a wait armed by start_wait_response()
******************************************************************************/
void cancel_wait_response()
{
	if(_sem_resp_msg == NULL)
	{
		return;
	}

	if(xSemaphoreTake(_sem_resp_msg, pdMS_TO_TICKS(500)) == pdTRUE)
	{
		_resp_state = ResponseState::RESP_STATE_IDLE;
		_mqtt_resp.data_len = 0;
		xSemaphoreGive(_sem_resp_msg);
	}
	else
	{
		_resp_state = ResponseState::RESP_STATE_IDLE;
		_mqtt_resp.data_len = 0;
	}
}

/******************************************************************************
* Block until the wait armed by start_wait_response() completes
******************************************************************************/
RetResult await_response(MqttResponse *out, u32 timeout)
{
	if(_sem_resp_msg == NULL)
	{
		return RET_ERROR;
	}

	ESP_LOGW(TAG, "Waiting on %s", _mqtt_resp.topic);

	u32 notifyValue = 0;
	BaseType_t ret = xTaskNotifyWait(0, ULONG_MAX, &notifyValue, pdMS_TO_TICKS(timeout));
	_resp_state = ResponseState::RESP_STATE_IDLE;

	if(ret != pdTRUE)
	{
		ESP_LOGE(TAG, "await_response timeout");
		return RET_TIMEOUT;
	}

	if(xSemaphoreTake(_sem_resp_msg, pdMS_TO_TICKS(timeout)) != pdTRUE)
	{
		return RET_ERROR;
	}

	RetResult res = RET_ERROR;
	if(_mqtt_resp.data_len > 0)
	{
		if(out != NULL)
		{
			memcpy(out, &_mqtt_resp, sizeof(_mqtt_resp));
		}
		res = RET_OK;
	}

	xSemaphoreGive(_sem_resp_msg);

	return res;
}

RetResult wait_response(const char *topic, MqttResponse *out, u32 timeout)
{
	if(_sem_resp_msg == NULL)
	{
		return RET_ERROR;
	}

	// Already waiting
	// if(_resp_state == ResponseState::RESP_STATE_IDLE)
	// {
	//		return RET_INVALID_STATE;
	// }

	if(xSemaphoreTake(_sem_resp_msg, pdMS_TO_TICKS(timeout)) == pdFALSE)
	{
		return RET_TIMEOUT;
	}
	
	strncpy(_mqtt_resp.topic, topic, sizeof(_mqtt_resp.topic));
	_mqtt_resp.topic[sizeof(_mqtt_resp.topic) - 1] = '\0';
	_mqtt_resp.data_len = 0;
	_resp_waiting_task = xTaskGetCurrentTaskHandle();
	_resp_state = ResponseState::RESP_STATE_WAITING;

	// Drop any stale notification left over by a previously timed-out wait
	xTaskNotifyWait(0, ULONG_MAX, NULL, 0);

	xSemaphoreGive(_sem_resp_msg);

	// Wait for message to arrive
	// Could possibly take timeout + timouet if mutex took too long to be acquired
	ESP_LOGW(TAG, "Waiting on %s", topic);
	u32 notifyValue = 0;
	BaseType_t ret = xTaskNotifyWait(0, ULONG_MAX, &notifyValue, pdMS_TO_TICKS(timeout));
	_resp_state = ResponseState::RESP_STATE_IDLE;
	if(ret == pdTRUE)
	{
		if(xSemaphoreTake(_sem_resp_msg, pdMS_TO_TICKS(timeout)) == pdTRUE && _mqtt_resp.data_len > 0)
		{
			memcpy(out, &_mqtt_resp, sizeof(_mqtt_resp));
			xSemaphoreGive(_sem_resp_msg);
		}
		else
		{
			xSemaphoreGive(_sem_resp_msg);
			return RET_ERROR;
		}
	}
	else
	{
		ESP_LOGE(TAG, "wait_response timeout");
	}

	return ret == pdFAIL ? RET_TIMEOUT : RET_OK;
}

/******************************************************************************
* Wrapper for esp's mqtt publish
* Helps with debug
******************************************************************************/
int publish(const char *topic, char *data, int qos)
{
	if(_mqtt_client == NULL)
		return -1;

	ESP_LOGI(TAG, "Publishing to %s", topic);;
	return esp_mqtt_client_publish(_mqtt_client, topic, data, strlen(data), qos, 0);
}

/******************************************************************************
* Wrapper for esp's mqtt subscribe
******************************************************************************/
int subscribe(const char *topic, int qos)
{
	if(!_mqtt_client)
		return -1;
	return esp_mqtt_client_subscribe(_mqtt_client, topic, qos);
}

/******************************************************************************
* Wrapper for esp's mqtt subscribe
******************************************************************************/
int unsubscribe(const char *topic)
{
	if(!_mqtt_client)
		return -1;

	return esp_mqtt_client_unsubscribe(_mqtt_client, topic);
}

/******************************************************************************
* Notify about connection result
******************************************************************************/
void notify_conn_result(esp_mqtt_event_id_t event){
	if (_keep_alive_task_handle != NULL) {
		xTaskNotify(_keep_alive_task_handle, (uint32_t)event, eSetValueWithOverwrite);
	}
}

/******************************************************************************
* Handle MQTT Events
******************************************************************************/
void mqtt_ev_handler(void *args, esp_event_base_t base, int32_t event_id, void *event_data)
{
	auto evg = GlobalState::get_event_group();
	esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
	GlobalEvents::MqttStateChangedEvent wxm_event;
	switch ((esp_mqtt_event_id_t)event_id)
	{
	case MQTT_EVENT_BEFORE_CONNECT:
		set_status(MqttStatus::CONNECTING);
		ESP_LOGI(TAG, "MQTT state before connect");
		break;
	case MQTT_EVENT_CONNECTED:
		ESP_LOGW(TAG, "Connection SUCCESS");
		// Reaching Thingsboard proves the firmware is functional: clear the
		// boot guard crash counter and report any pending OTA rollback record
		BootGuard::mark_healthy();
		// TODO: Buff lock in log may introduce waiting in default loop
		Log::log(Log::Code::TB_CONNECTED);
		if(get_sec_since_status_change() != 0) // first time connecting
		{
			Log::logc(Log::Code::TB_DOWNTIME, get_sec_since_status_change());
			ESP_LOGW(TAG, "TB downtime: %ds", get_sec_since_status_change());
		}
		xEventGroupClearBits(evg, GlobalState::EV_TB_DISCONNECTED_BIT);
		xEventGroupSetBits(evg, GlobalState::EV_TB_CONNECTED_BIT);
		set_status(MqttStatus::CONNECTED);
		if (_keep_alive_task_handle != NULL) {
			xTaskNotify(_keep_alive_task_handle, (uint32_t)MQTT_EVENT_CONNECTED, eSetValueWithOverwrite);
		}
		wxm_event = GlobalEvents::MqttStateChangedEvent::CONNECTED;
		GlobalEvents::publish(GlobalEvents::EventId::MQTT_STATE_CHANGED, &wxm_event, sizeof(wxm_event));
		break;
	case MQTT_EVENT_DISCONNECTED:
		if(get_status() == MqttStatus::CONNECTING) 
		{
			ESP_LOGW(TAG, "Connection FAILED");
			set_status(MqttStatus::CONNECTION_FAILED);
		}
		else 
		{
			ESP_LOGW(TAG, "MQTT disconnected");
			set_status(MqttStatus::DISCONNECTED);
		}
		// TODO: Buff lock in log may introduce waiting in default loop
		Log::log(Log::Code::TB_DISCONNECTED);
		xEventGroupClearBits(evg, GlobalState::EV_TB_CONNECTED_BIT);
		xEventGroupSetBits(evg, GlobalState::EV_TB_DISCONNECTED_BIT);
		if(get_status() == MqttStatus::CONNECTION_FAILED)
		{
			if (_keep_alive_task_handle != NULL) {
				xTaskNotify(_keep_alive_task_handle, (uint32_t)MQTT_EVENT_DISCONNECTED, eSetValueWithOverwrite);
			}
			set_status(MqttStatus::DISCONNECTED);
		}
		wxm_event = GlobalEvents::MqttStateChangedEvent::DISCONNECTED;
		GlobalEvents::publish(GlobalEvents::EventId::MQTT_STATE_CHANGED, &wxm_event, sizeof(wxm_event));
		break;
	case MQTT_EVENT_SUBSCRIBED:
		ESP_LOGI(TAG, "MQTT Subscribed (id %d)", event->msg_id);
		break;
	case MQTT_EVENT_UNSUBSCRIBED:
		ESP_LOGI(TAG, "MQTT Unsubscribed (id %d)", event->msg_id);
		break;
	case MQTT_EVENT_PUBLISHED:
	{
		GlobalEvents::MqttDataPublishedEvent wxm_pub_event = {
			.msg_id = event->msg_id,
			.retain = event->retain,
			.qos = event->qos,
			.dup = event->dup
		};
		GlobalEvents::publish(GlobalEvents::EventId::MQTT_DATA_PUBLISHED, &wxm_pub_event, sizeof(wxm_pub_event));
		ESP_LOGI(TAG, "MQTT Published (id %d)", event->msg_id);
		break;
	}
	case MQTT_EVENT_DATA:
		if (event->topic != NULL && event->topic_len > 0)
		{
			ESP_LOGI(TAG, "MQTT Data received on topic (id %d): %.*s [len: %d, total: %d, offset: %d]",
			         event->msg_id, event->topic_len, event->topic,
			         event->data_len, event->total_data_len, event->current_data_offset);
		}
		else
		{
			ESP_LOGI(TAG, "MQTT Data fragment (id %d) [len: %d, total: %d, offset: %d]",
			         event->msg_id, event->data_len, event->total_data_len, event->current_data_offset);
		}

		// Handle any pending wait_response / await_response calls
		if(xSemaphoreTake(_sem_resp_msg, pdMS_TO_TICKS(500)) == pdTRUE)
		{
			if(_resp_state == ResponseState::RESP_STATE_WAITING)
			{
				bool match = false;
				if(event->current_data_offset == 0)
				{
					// First fragment or complete unfragmented message: verify topic prefix match
					if(event->topic != NULL && event->topic_len > 0)
					{
						std::string_view received_topic(event->topic, event->topic_len);
						if(received_topic.find(_mqtt_resp.topic) != std::string_view::npos)
						{
							match = true;
							_mqtt_resp.msg_id = event->msg_id;
							_mqtt_resp.data_len = 0;
						}
						else
						{
							ESP_LOGE(TAG, "No topic match (waiting for %s)", _mqtt_resp.topic);
						}
					}
					else
					{
						ESP_LOGW(TAG, "MQTT data offset 0 with NULL topic");
					}
				}
				else
				{
					// Continuation fragment of an in-progress response:
					// In esp-mqtt, continuation fragments (offset > 0) arrive with event->topic = NULL.
					// Verify that this fragment is contiguous with data accumulated so far.
					if(event->current_data_offset == _mqtt_resp.data_len && _mqtt_resp.data_len > 0)
					{
						match = true;
					}
					else
					{
						ESP_LOGE(TAG, "MQTT fragment offset mismatch: expected %d, got %d",
						         _mqtt_resp.data_len, event->current_data_offset);
					}
				}

				if(match)
				{
					// Accumulate data into response buffer with overflow protection
					int max_copy = (int)sizeof(_mqtt_resp.data) - event->current_data_offset;
					if(max_copy > 0)
					{
						int copy_bytes = event->data_len < max_copy ? event->data_len : max_copy;
						memcpy(_mqtt_resp.data + event->current_data_offset, event->data, copy_bytes);
						_mqtt_resp.data_len = event->current_data_offset + copy_bytes;
					}
					else
					{
						ESP_LOGE(TAG, "MQTT response buffer overflow! offset=%d, buf_size=%zu",
						         event->current_data_offset, sizeof(_mqtt_resp.data));
					}

					// Only notify the waiting task when all fragments have been fully reassembled
					if(event->current_data_offset + event->data_len >= event->total_data_len)
					{
						_resp_state = ResponseState::RESP_STATE_RECEIVED;
						if(event->data_len != event->total_data_len)
						{
							ESP_LOGI(TAG, "MQTT reassembled %d bytes across fragments (expected %d)",
							         _mqtt_resp.data_len, event->total_data_len);
						}

						// Notify any tasks waiting on wait_response / await_response
						xTaskNotify(_resp_waiting_task, 0, eSetValueWithOverwrite);
					}
					else
					{
						ESP_LOGD(TAG, "MQTT fragment buffered: offset=%d, len=%d, accumulated=%d/%d",
						         event->current_data_offset, event->data_len, _mqtt_resp.data_len, event->total_data_len);
					}
				}
			}
			else
			{
				ESP_LOGW(TAG, "Noone is awaiting those data (state: %d)", (int)_resp_state);
			}

			xSemaphoreGive(_sem_resp_msg);
		}
		else
		{
			ESP_LOGE(TAG, "Handler timed out waiting for _sem_resp_msg");
		}

		break;
	case MQTT_EVENT_ERROR:
		ESP_LOGE(TAG, "MQTT Error");
		if(event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
		{
			ESP_LOGI(TAG, "esp-tls: %d", event->error_handle->esp_tls_last_esp_err);
			ESP_LOGI(TAG, "TLS stack: %d ", event->error_handle->esp_tls_stack_err);
			ESP_LOGI(TAG, "Transport socket errno: %d", event->error_handle->esp_transport_sock_errno);
			ESP_LOGI(TAG, "Last error: %s", strerror(event->error_handle->esp_transport_sock_errno));
		}
		break;
	default:
		ESP_LOGI(TAG, "MQTT other event id: %d", event->event_id);
		break;
	}
}

/******************************************************************************
* Return current TB status
******************************************************************************/
MqttStatus get_status()
{
	return _mqtt_status;
}

/*******************************************************************************
 * @brief Set current TB status.
 ******************************************************************************/
void set_status(MqttStatus status)
{
	if(status != _mqtt_status)
	{
		// If for any reason disconnected or connected, capture timestamp.
		if(_mqtt_status == MqttStatus::CONNECTED || status == MqttStatus::DISCONNECTED)
		{
			_last_status_changed_ts = Utils::get_tick_sec();
		}
	}
	_mqtt_status = status;
}

/*******************************************************************************
 * @brief Get how many seconds have passed since last status change.
 * @note Only captures duration between CONNECTED and DISCONNECTED statuses.
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
* TB keep alive task
* Keep TB alive when wifi is ON.
******************************************************************************/
void keep_alive_task(void *data)
{
	ESP_LOGI(TAG, "Keepalive task started");
	bool was_running = false;
	
	while(1)
	{
		bool wifi_ok = Wifi::is_connected();
		bool mqtt_ok = !Settings::get_mqtt_disabled();
		
		if (wifi_ok && mqtt_ok) {
			if (!was_running) {
				// Defer starting the MQTT client by a short delay after initial Wi-Fi connect
				// to allow the Web Server to safely warm up and respond to initial status checks
				// without blocking LwIP DNS lookups or starving tasks with high-CPU TLS handshakes.
				uint32_t delay_ms = 5000; // default 5 seconds
				if (Utils::get_tick_sec() < 45) {
					// Extra buffer on initial cold boot
					delay_ms = 10000; // 10 seconds
				}
				ESP_LOGI(TAG, "Delaying MQTT client start by %lu ms to let network services stabilize...", (unsigned long)delay_ms);
				vTaskDelay(pdMS_TO_TICKS(delay_ms));

				// Re-verify conditions in case Wi-Fi disconnected or MQTT was disabled during the delay
				if (Wifi::is_connected() && !Settings::get_mqtt_disabled()) {
					ESP_LOGI(TAG, "Starting MQTT client from keepalive task");
					esp_err_t ret = esp_mqtt_client_start(_mqtt_client);
					if (ret == ESP_OK) {
						was_running = true;
					} else {
						ESP_LOGE(TAG, "Error starting mqtt client: %s", esp_err_to_name(ret));
						vTaskDelay(pdMS_TO_TICKS(3000));
						continue;
					}
				}
			}
			
			// Wait for a notification (e.g. settings change) or timeout
			uint32_t notif = 0;
			xTaskNotifyWait(0, ULONG_MAX, &notif, pdMS_TO_TICKS(5000));
			
			// If wifi gets disconnected, we must stop the client
			if (!Wifi::is_connected()) {
				ESP_LOGI(TAG, "Wi-Fi disconnected, stopping MQTT");
				mqtt_stop();
				was_running = false;
				vTaskDelay(pdMS_TO_TICKS(TB_MQTT_RECONNECT_INTERVAL_MS));
			}
		} else {
			if (was_running) {
				ESP_LOGI(TAG, "Stopping MQTT client from keepalive task (wifi_ok=%d, mqtt_ok=%d)", wifi_ok, mqtt_ok);
				mqtt_stop();
				was_running = false;
			}
			
			// Wait for a notification or 1 second timeout before checking again
			uint32_t notif = 0;
			xTaskNotifyWait(0, ULONG_MAX, &notif, pdMS_TO_TICKS(1000));
		}
	}
}

/******************************************************************************
* Wait on MQTT connected event bit
* @return 
*   RET_OK on success
*   RET_TIMEOUT on wait timeout
******************************************************************************/
RetResult wait_connected(u32 timeout)
{
	EventBits_t ret = xEventGroupWaitBits(GlobalState::get_event_group(), GlobalState::EV_TB_CONNECTED_BIT, false, false, pdMS_TO_TICKS(timeout));

	return ret & GlobalState::EV_TB_CONNECTED_BIT ? RET_OK : RET_TIMEOUT;
}

/******************************************************************************
* Wait on MQTT disconnect event bit
* @return 
*   RET_OK on success
*   RET_TIMEOUT on wait timeout
******************************************************************************/
RetResult wait_disconnected(u32 timeout)
{
	EventBits_t ret = xEventGroupWaitBits(GlobalState::get_event_group(), GlobalState::EV_TB_DISCONNECTED_BIT, false, false, pdMS_TO_TICKS(timeout));

	return ret & GlobalState::EV_TB_DISCONNECTED_BIT ? RET_OK : RET_TIMEOUT;
}

/******************************************************************************
* MQTT evg connection status
* @return
*   true if connected
******************************************************************************/
bool is_connected()
{
	auto evg = GlobalState::get_event_group();

	return evg != NULL && xEventGroupGetBits(evg) & GlobalState::EV_TB_CONNECTED_BIT ? true : false;
}

/******************************************************************************
* Get client handle
******************************************************************************/
esp_mqtt_client_handle_t get_client()
{
	return _mqtt_client;
}

}