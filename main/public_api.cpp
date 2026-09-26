#include "public_api.h"
#include "https_client.h"
#include "settings.h"
#include "ArduinoJson.h"
#include "global_events.h"
#include <ctype.h>
#include <string.h>
#include <time.h>

namespace PublicApi
{

/** Logger tag */
static const char TAG[] = "PubApi";

/** Json document */
StaticJsonDocument<PUB_API_JSON_DOC_SIZE> _json_doc;

/** Mutex object for json doc */
SemaphoreHandle_t _sem_json_doc;

/** Semaphore buffer */
StaticSemaphore_t _sem_json_doc_buff;

/******************************************************************************
 * @brief Strip administrative labels from an address, preserving the rest
 *        e.g. "Levadia Municipal Unit, GR" -> "Levadia, GR"
 * @param str String of address to strip labels from. In and out.
 ******************************************************************************/
void strip_address_admin_labels(char* str)
{
	if(!str || !*str)
	{
		return;
	}

	const char* PUB_API_ADDRESS_ADMIN_LABELS[] = {
		"Municipal Unit of", "Municipal Unit",
		"Regional Unit of",  "Regional Unit",
		"Municipality of",   "Municipality",
		"Prefecture of",     "Prefecture",
		"Region of",         "Region",
		"City of", "Town of", "Village of",
		"Locality of", "Community of", "Settlement of"
	};
	const size_t LABELS_COUNT = sizeof(PUB_API_ADDRESS_ADMIN_LABELS)/sizeof(PUB_API_ADDRESS_ADMIN_LABELS[0]);

	// Remove labels wherever they appear, case-insensitive, and swallow adjacent spaces.
	for(size_t i = 0; i < LABELS_COUNT; i++)
	{
		const char* label = PUB_API_ADDRESS_ADMIN_LABELS[i];
		size_t label_len = strlen(label);
		if(label_len == 0)
		{
			continue;
		}

		char* p = str;
		while(*p)
		{
			// Case-insensitive match at p
			size_t j = 0;
			while(j < label_len && tolower((unsigned char)p[j]) == tolower((unsigned char)label[j]))
			{
				j++;
			}

			if(j == label_len)
			{
				// Remove label and any trailing spaces
				char* after = p + label_len;
				while(*after && isspace((unsigned char)*after)) after++;

				// If there is exactly one leading space before the label, drop it to avoid double spaces.
				char* start = p;
				if(start > str && isspace((unsigned char)start[-1])) start--;

				memmove(start, after, strlen(after) + 1);
				// Continue scanning from current position (start) without advancing
				p = start;
			}
			else
			{
				p++;
			}
		}
	}

	// Normalize spaces around commas: remove spaces before ',', ensure single space after ','
	{
		char* src = str;
		char* dst = str;

		// Trim leading spaces
		while(*src && isspace((unsigned char)*src))
		{
			src++;
		}

		while(*src)
		{
			if(*src == ',')
			{
				// Remove any spaces before comma already handled by previous char logic
				*dst++ = *src++;
				// Skip spaces after comma
				while(*src && isspace((unsigned char)*src))
				{
					src++;
				}
				// Insert single space after comma if not end or another comma
				if(*src && *src != ',')
				{
					*dst++ = ' ';
				}
			}
			else
			{
				// Collapse multiple spaces to single, avoid space right after comma which we already inserted
				if(isspace((unsigned char)*src))
				{
					// Skip run of spaces
					while(*src && isspace((unsigned char)*src))
					{
						src++;
					}
					// Add a single space if next char is not comma or end and last placed char isn't space
					if(*src && *src != ',' && (dst == str || dst[-1] != ' '))
					{
						*dst++ = ' ';
					}
				}
				else
				{
					*dst++ = *src++;
				}
			}
		}
		*dst = '\0';
	}

	// Final trim
	{
		char* p = str;
		while(*p && isspace((unsigned char)*p))
		{
			p++;
		}
		if(p != str)
		{
			memmove(str, p, strlen(p) + 1);
		}

		size_t len = strlen(str);
		while(len > 0 && isspace((unsigned char)str[len - 1]))
		{
			str[--len] = '\0';
		}
	}
}

/******************************************************************************
 * @brief Rewards ready http response callback
 * @param status Status of http response
 * @param data Response body
 * @param data_len Response body length
 * @param ctx Context
 ******************************************************************************/
void on_rewards_ready(int status, const char* data, size_t data_len, void* ctx)
{
	if(status != PUB_API_STATUS_OK)
	{
		ESP_LOGE(TAG, "on_rewards_ready: error, status:%d", status);
		return;
	}
	if(data == nullptr || data_len == 0)
	{
		ESP_LOGE(TAG, "on_rewards_ready: empty body");
		return;
	}

	Rewards rm_data{};
	rm_data.ts = 0;

	xSemaphoreTake(_sem_json_doc, portMAX_DELAY);

	_json_doc.clear();
	DeserializationError deser_status = deserializeJson(_json_doc, data, data_len);
	if(deser_status != DeserializationError::Code::Ok)
	{
		ESP_LOGE(TAG, "on_rewards_ready: deserialization error: %s", deser_status.c_str());
		xSemaphoreGive(_sem_json_doc);
		return;
	}

	rm_data.total_rewards = _json_doc["total_rewards"] | 0.0f;
	rm_data.latest_total_reward = _json_doc["latest"]["total_reward"] | 0.0f;

	const char* ts_str = _json_doc["latest"]["timestamp"].as<const char*>();
	if(ts_str && *ts_str)
	{
		struct tm t = {};
    
		// Parse up to seconds; ignore fractional ms and trailing 'Z'
		if(strptime(ts_str, "%Y-%m-%dT%H:%M:%S", &t) != nullptr)
		{
			rm_data.ts = mktime(&t);
		}
		else
		{
			ESP_LOGW(TAG, "on_rewards_ready: strptime failed for ts:%s", ts_str);
		}
	}
	else
	{
		ESP_LOGW(TAG, "on_rewards_ready: missing latest.timestamp");
	}

	xSemaphoreGive(_sem_json_doc);

	ESP_LOGI(TAG, "Rewards acquired!");
	ESP_LOGI(TAG, "total_rewards=%f", rm_data.total_rewards);
	ESP_LOGI(TAG, "latest_total_reward:%f", rm_data.latest_total_reward);

	GlobalEvents::publish(GlobalEvents::EventId::PUB_API_REWARDS, &rm_data, sizeof(rm_data));
}

/******************************************************************************
 * @brief Device info ready http response callback
 * @param status Status of http response
 * @param data Response body
 * @param data_len Response body length
 * @param ctx Context
 ******************************************************************************/
void on_dev_info_ready(int status, const char* data, size_t data_len, void* ctx)
{
	if(status != PUB_API_STATUS_OK)
	{
		ESP_LOGE(TAG, "on_dev_info_ready: error, status:%d", status);
		return;
	}
	if(data == nullptr || data_len == 0)
	{
		ESP_LOGE(TAG, "on_dev_info_ready: empty body");
		return;
	}

	DevInfo dev_info{};
	dev_info.qod_score = 0;
	dev_info.pol_reason = PolReason::NONE;
	dev_info.address[0] = '\0';

	xSemaphoreTake(_sem_json_doc, portMAX_DELAY);

	_json_doc.clear();
	DeserializationError deser_status = deserializeJson(_json_doc, data, data_len);
	if(deser_status != DeserializationError::Code::Ok)
	{
		ESP_LOGE(TAG, "on_dev_info_ready: deserialization error: %s", deser_status.c_str());
		xSemaphoreGive(_sem_json_doc);
		return;
	}

	if(_json_doc["metrics"]["qod_score"].is<uint8_t>()) dev_info.qod_score = _json_doc["metrics"]["qod_score"].as<uint8_t>();
	const char* pol_str = _json_doc["metrics"]["pol_reason"].as<const char*>();
	if(pol_str)
	{
		if(strcmp(pol_str, PUB_API_POL_REASON_NO_LOCATION_DATA_STR) == 0)
		{
			dev_info.pol_reason = PolReason::NO_LOCATION_DATA;
		}
		else if(strcmp(pol_str, PUB_API_POL_REASON_NOT_VERIF_STR) == 0)
		{
			dev_info.pol_reason = PolReason::LOCATION_NOT_VERIFIED;
		}
	}

	const char* addr = _json_doc["address"] | "";
	strncpy(dev_info.address, addr, sizeof(dev_info.address) - 1);
	dev_info.address[sizeof(dev_info.address) - 1] = '\0';
	strip_address_admin_labels(dev_info.address);

	xSemaphoreGive(_sem_json_doc);

	ESP_LOGI(TAG, "Device info acquired!");
	ESP_LOGI(TAG, "qod=%d", dev_info.qod_score);
	ESP_LOGI(TAG, "pol=%d", (int)dev_info.pol_reason);
	ESP_LOGI(TAG, "address=%s", dev_info.address);

	GlobalEvents::publish(GlobalEvents::EventId::PUB_API_DEV_INFO, &dev_info, sizeof(dev_info));
}

/******************************************************************************
 * @brief Device id and cell index ready callback, to acquire rewards
 * @param dev_id Device ID
 * @param cell_index Cell index
 ******************************************************************************/
void acquire_rewards(const char* dev_id, const char* cell_index)
{
	HttpsClient::Req req_rewards{};
	req_rewards.resp_cb = on_rewards_ready;

	int n = snprintf(req_rewards.url, sizeof(req_rewards.url), PUB_API_REWARDS_URL, dev_id ? dev_id : "");
	if(n <= 0 || (size_t)n >= sizeof(req_rewards.url))
	{
		ESP_LOGW(TAG, "acquire_rewards: URL truncated");
	}

	HttpsClient::request(&req_rewards);
}

/******************************************************************************
 * @brief Device id and cell index ready callback, to acquire device information
 * @param dev_id Device ID
 * @param cell_index Cell index
 ******************************************************************************/
void acquire_dev_info(const char* dev_id, const char* cell_index)
{
	HttpsClient::Req req_dev_info{};
	req_dev_info.resp_cb = on_dev_info_ready;

	int n = snprintf(req_dev_info.url, sizeof(req_dev_info.url), PUB_API_DEV_INFO_URL, cell_index ? cell_index : "", dev_id ? dev_id : "");
	if(n <= 0 || (size_t)n >= sizeof(req_dev_info.url))
	{
		ESP_LOGW(TAG, "acquire_dev_info: URL truncated");
	}

	HttpsClient::request(&req_dev_info);
}

/******************************************************************************
 * @brief Device id ready http response callback
 * @param status Status of http response
 * @param data Response body
 * @param data_len Response body length
 * @param ctx Context
 ******************************************************************************/
void on_dev_id_ready(int status, const char* data, size_t data_len, void* ctx)
{
	if(status != PUB_API_STATUS_OK)
	{
		ESP_LOGE(TAG, "on_dev_id_ready: error, status:%d", status);
		return;
	}
	if(data == nullptr || data_len == 0)
	{
		ESP_LOGE(TAG, "on_dev_id_ready: empty body");
		return;
	}

	xSemaphoreTake(_sem_json_doc, portMAX_DELAY);

	_json_doc.clear();
	DeserializationError deser_status = deserializeJson(_json_doc, data, data_len);
	if(deser_status != DeserializationError::Code::Ok)
	{
		ESP_LOGE(TAG, "on_dev_id_ready: deserialization error: %s", deser_status.c_str());
		xSemaphoreGive(_sem_json_doc);
		return;
	}

	JsonArray devices = _json_doc["devices"].as<JsonArray>();
	if(!devices || devices.size() == 0)
	{
		ESP_LOGE(TAG, "on_dev_id_ready: no devices in JSON");
		xSemaphoreGive(_sem_json_doc);
		return;
	}

	const char* id_ptr = devices[0]["id"] | "";
	const char* cell_ptr = devices[0]["cell_index"] | "";

	char dev_id[PUB_API_DEV_ID_CHAR_SIZE] = {0};
	strncpy(dev_id, id_ptr, sizeof(dev_id) - 1);

	char cell_index[PUB_API_CELL_INDEX_CHAR_SIZE] = {0};
	strncpy(cell_index, cell_ptr, sizeof(cell_index) - 1);

	xSemaphoreGive(_sem_json_doc);

	if(ctx)
	{
		((DevSearchCb)ctx)(dev_id, cell_index);
	}
}

/******************************************************************************
 * @brief Build device id http request
 * @param req Request struct to build the device id request
 ******************************************************************************/
RetResult build_dev_id_req(HttpsClient::Req* req)
{
	const char* dev_name = Settings::get_device_name();
	if(!dev_name || strlen(dev_name) == 0)
	{
		ESP_LOGE(TAG, "build_dev_id_req: empty device name");
		return RET_ERROR;
	}

	int n = snprintf(req->url, sizeof(req->url), PUB_API_SEARCH_URL, dev_name);
	if(n <= 0 || (size_t)n >= sizeof(req->url))
	{
		ESP_LOGE(TAG, "build_dev_id_req: URL truncated");
		return RET_ERROR;
	}

	req->resp_cb = on_dev_id_ready;
	return RET_OK;
}

/******************************************************************************
 * @brief Request device info from API. Response comes with Global Event
 ******************************************************************************/
void req_dev_info_data()
{
	ESP_LOGI(TAG, "Requiring device information");
	HttpsClient::Req req_dev_id{};
	if(build_dev_id_req(&req_dev_id) != RET_OK)
	{
		ESP_LOGE(TAG, "req_dev_info_data: build_dev_id_req error");
		return;
	}

	req_dev_id.ctx = (void*) acquire_dev_info;
	HttpsClient::request(&req_dev_id);
}

/******************************************************************************
 * @brief Request rewards data from API. Response comes with Global Event
 ******************************************************************************/
void req_rewards_data()
{
	ESP_LOGI(TAG, "Requiring rewards data");
	HttpsClient::Req req_dev_id{};
	if(build_dev_id_req(&req_dev_id) != RET_OK)
	{
		ESP_LOGE(TAG, "req_rewards_data: build_dev_id_req error");
		return;
	}

	req_dev_id.ctx = (void*) acquire_rewards;
	HttpsClient::request(&req_dev_id);
}

/******************************************************************************
 * @brief Init
 ******************************************************************************/
void init()
{
	_sem_json_doc = xSemaphoreCreateMutexStatic(&_sem_json_doc_buff);
}

}
