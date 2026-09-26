#include "claiming.h"
#include "global_events.h"
#include "tb.h"
#include "ArduinoJson.h"
#include "utils.h"
#include "esp_log.h"
#include "credentials.h"
#include "esp_random.h"
#include "math.h"

namespace Claiming
{

static const char TAG[] = "Claiming";

const char CLAIM_ENABLE_REQ_TASK_NAME[] = "enable_claiming_task";
const int CLAIM_ENABLE_REQ_TASK_STACK_SIZE = 8*1024;
const int CLAIM_ENABLE_REQ_TASK_PRIORITY = 2;
EXT_RAM_BSS_ATTR StackType_t _claim_enable_task_stack[CLAIM_ENABLE_REQ_TASK_STACK_SIZE] = {};
StaticTask_t _claim_enable_task_buff;
static TaskHandle_t _claim_enable_task_handle = NULL;
static volatile bool _stop_claiming = false;

static bool delay_or_stop(uint32_t delay_ms)
{
	uint32_t ntf_val = 0;
	if(xTaskNotifyWait(0, ULONG_MAX, &ntf_val, pdMS_TO_TICKS(delay_ms)) == pdTRUE)
	{
		return true;
	}
	return false;
}

RetResult publish_sync(uint16_t timeout_ms, uint32_t code)
{
	if(_stop_claiming)
	{
		return RET_TIMEOUT;
	}

	StaticJsonDocument<TB_JSON_DOC_SIZE> json_doc;
	char json_buff[TB_JSON_BUFF_SIZE] = "";

	JsonObject root_obj = json_doc.template to<JsonObject>();
	char key_str_buff[7] = "";
	snprintf(key_str_buff, sizeof(key_str_buff), "%06u", (unsigned int)code);
	root_obj["secretKey"] = key_str_buff;
	root_obj["durationMs"] = TB_CLAIM_ENABLE_DURATION_MS;
	serializeJson(json_doc, json_buff, sizeof(json_buff));

	ESP_LOGI(TAG, "Claim enable request:\n%s", json_buff);
	GlobalEvents::MqttDataPublishedEvent wxm_pub_event = {};
	int msg_id = Tb::publish(TB_CLAIM_PUBLISH_TOPIC, json_buff, 1);
	if(msg_id <= 0)
	{
		return RET_ERROR;
	}
	uint32_t publish_ts = Utils::get_tick_sec();
	while(Utils::get_tick_sec() - publish_ts < timeout_ms)
	{
		if(_stop_claiming)
		{
			return RET_TIMEOUT;
		}
		if(GlobalEvents::wait(GlobalEvents::EventId::MQTT_DATA_PUBLISHED, &wxm_pub_event, sizeof(wxm_pub_event), pdMS_TO_TICKS(timeout_ms)) != RET_OK)
		{
			return RET_TIMEOUT;
		}
		if(wxm_pub_event.msg_id == msg_id)
		{
			return RET_OK;
		}
	}
	return RET_TIMEOUT;
}

void enable_claiming_task(void *data)
{
	int64_t claim_en_req_init_time = Utils::get_tick_ms();
	// Give spinner some time to spin the hell out of it
	if(delay_or_stop(2000) || _stop_claiming)
	{
		_claim_enable_task_handle = NULL;
		vTaskDelete(NULL);
		return;
	}

	int fail_streak_count = 0;
	uint32_t random_number = esp_random() % 1000000;

	while(!_stop_claiming)
	{
		GlobalEvents::ClaimEnRespEvent resp_event = {
			.type =  GlobalEvents::ClaimEnRespStatus::CLAIM_ENABLE_FAIL, 
			.code = random_number
		};
		uint32_t wait_for_reenable_ms = TB_CLAIM_ENABLE_REFRESH_MS;

		if(Utils::get_tick_ms() - claim_en_req_init_time > TB_CLAIM_ENABLE_TIMEOUT_MS || fail_streak_count >= TB_CLAIM_ENABLE_MAX_FAILS)
		{
			ESP_LOGW(TAG, "Stopping claim enable task");
			if(Utils::get_tick_ms() - claim_en_req_init_time > TB_CLAIM_ENABLE_TIMEOUT_MS)
			{
				ESP_LOGW(TAG, "Claim enable timeout");
				resp_event.type = GlobalEvents::ClaimEnRespStatus::CLAIM_ENABLE_TIMEOUT;
				GlobalEvents::publish(GlobalEvents::EventId::CLAIM_ENABLE_RESPONSE, &resp_event, sizeof(resp_event));
			}			
			break;
		}

		if(publish_sync(TB_CLAIM_ENABLE_PUBLISH_TIMEOUT_MS, random_number) == RET_OK)
		{
			fail_streak_count = 0;
			resp_event.type = GlobalEvents::ClaimEnRespStatus::CLAIM_ENABLE_SUCCESS;
		}
		else
		{
			ESP_LOGE(TAG, "Claim publish timeout");
			wait_for_reenable_ms = TB_CLAIM_ENABLE_RETRY_MS;
			resp_event.type = GlobalEvents::ClaimEnRespStatus::CLAIM_ENABLE_FAIL;
			fail_streak_count++;
		}

		if(_stop_claiming)
		{
			break;
		}

		GlobalEvents::publish(GlobalEvents::EventId::CLAIM_ENABLE_RESPONSE, &resp_event, sizeof(resp_event));

		if(delay_or_stop(wait_for_reenable_ms) || _stop_claiming)
		{
			break;
		}
	}	

	ESP_LOGI(TAG, "Claim enable task finished");
	_claim_enable_task_handle = NULL;
	vTaskDelete(NULL);
}

void event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
	if(NULL == event_data)
	{
		return;
	}

	if(((GlobalEvents::UiEvent *)event_data)->ui_event_type == GlobalEvents::UiEventType::CLAIM_REQUESTED)
	{
		int wait_ms = 0;
		while(_claim_enable_task_handle != NULL && wait_ms < 500)
		{
			vTaskDelay(pdMS_TO_TICKS(10));
			wait_ms += 10;
		}

		if(_claim_enable_task_handle == NULL)
		{
			_stop_claiming = false;
			_claim_enable_task_handle = xTaskCreateStaticPinnedToCore(enable_claiming_task, CLAIM_ENABLE_REQ_TASK_NAME, sizeof(_claim_enable_task_stack), NULL,
											CLAIM_ENABLE_REQ_TASK_PRIORITY, _claim_enable_task_stack, &_claim_enable_task_buff, 1);
		}			
	}
	else if(((GlobalEvents::UiEvent *)event_data)->ui_event_type == GlobalEvents::UiEventType::CLAIM_FINISHED)
	{		
		if(_claim_enable_task_handle != NULL)
		{
			ESP_LOGI(TAG, "Claim ended by user");
			_stop_claiming = true;
			xTaskNotify(_claim_enable_task_handle, 1, eSetValueWithOverwrite);
		}
	}
}



void init()
{
	GlobalEvents::subscribe(GlobalEvents::EventId::UI_EVENT, event_handler, NULL);
}

}