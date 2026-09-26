#include "ws/include/remote_observation_queue.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "REMOTE_OBS_Q";

namespace RemoteObservationQueue
{

struct DedupEntry
{
	uint32_t station_id;
	uint32_t timestamp;
	uint16_t payload_crc;
};

static SemaphoreHandle_t _queue_mutex = NULL;
static RemoteObservation _queue[QUEUE_CAPACITY] = {};
static size_t _head = 0;
static size_t _tail = 0;
static size_t _count = 0;

static DedupEntry _dedup_history[DEDUP_HISTORY_CAPACITY] = {};
static size_t _dedup_idx = 0;

static uint16_t compute_crc16(const uint8_t *data, size_t len)
{
	uint16_t crc = 0xFFFF;
	for (size_t i = 0; i < len; i++) {
		crc ^= (uint16_t)data[i];
		for (int b = 0; b < 8; b++) {
			if (crc & 0x0001) {
				crc = (crc >> 1) ^ 0xA001;
			} else {
				crc = crc >> 1;
			}
		}
	}
	return crc;
}

WsCommon::RetRes init()
{
	if (_queue_mutex == NULL) {
		_queue_mutex = xSemaphoreCreateMutex();
		if (_queue_mutex == NULL) {
			ESP_LOGE(TAG, "Failed to create queue mutex");
			return WsCommon::RetRes::ERROR;
		}
	}
	clear();
	return WsCommon::RetRes::OK;
}

WsCommon::RetRes push(const RemoteObservation &obs)
{
	if (_queue_mutex == NULL) {
		init();
	}

	if (xSemaphoreTake(_queue_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
		ESP_LOGW(TAG, "Failed to acquire mutex for push");
		return WsCommon::RetRes::ERROR_TIMEOUT;
	}

	// Calculate payload hash for deduplication
	uint16_t crc = compute_crc16(obs.raw_payload, obs.raw_payload_len);

	// Check deduplication history
	for (size_t i = 0; i < DEDUP_HISTORY_CAPACITY; i++) {
		if (_dedup_history[i].station_id == obs.station_id &&
			_dedup_history[i].timestamp == obs.timestamp &&
			_dedup_history[i].payload_crc == crc)
		{
			ESP_LOGD(TAG, "Dropping duplicate remote observation for station 0x%08X (ts=%u)",
					 (unsigned int)obs.station_id, (unsigned int)obs.timestamp);
			xSemaphoreGive(_queue_mutex);
			return WsCommon::RetRes::OK;
		}
	}

	// Record in dedup history
	_dedup_history[_dedup_idx] = {
		.station_id = obs.station_id,
		.timestamp = obs.timestamp,
		.payload_crc = crc
	};
	_dedup_idx = (_dedup_idx + 1) % DEDUP_HISTORY_CAPACITY;

	// Push into ring buffer
	if (_count >= QUEUE_CAPACITY) {
		// Queue is full: overwrite oldest entry (advance tail)
		ESP_LOGW(TAG, "Queue full (%d items); dropping oldest remote observation", (int)QUEUE_CAPACITY);
		_tail = (_tail + 1) % QUEUE_CAPACITY;
		_count--;
	}

	_queue[_head] = obs;
	_head = (_head + 1) % QUEUE_CAPACITY;
	_count++;

	ESP_LOGI(TAG, "Enqueued remote observation for station 0x%08X (ts=%u, signed=%d, count=%d)",
			 (unsigned int)obs.station_id, (unsigned int)obs.timestamp, (int)obs.is_signed, (int)_count);

	xSemaphoreGive(_queue_mutex);
	return WsCommon::RetRes::OK;
}

WsCommon::RetRes pop(RemoteObservation *obs)
{
	if (obs == NULL) {
		return WsCommon::RetRes::ERROR_NULL_POINTER;
	}

	if (_queue_mutex == NULL) {
		init();
	}

	if (xSemaphoreTake(_queue_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
		return WsCommon::RetRes::ERROR_TIMEOUT;
	}

	if (_count == 0) {
		xSemaphoreGive(_queue_mutex);
		return WsCommon::RetRes::ERROR_NOT_FOUND;
	}

	*obs = _queue[_tail];
	_tail = (_tail + 1) % QUEUE_CAPACITY;
	_count--;

	xSemaphoreGive(_queue_mutex);
	return WsCommon::RetRes::OK;
}

int get_count()
{
	if (_queue_mutex == NULL) return 0;
	if (xSemaphoreTake(_queue_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
		return (int)_count;
	}
	int cnt = (int)_count;
	xSemaphoreGive(_queue_mutex);
	return cnt;
}

void clear()
{
	if (_queue_mutex != NULL) {
		xSemaphoreTake(_queue_mutex, pdMS_TO_TICKS(100));
	}
	_head = 0;
	_tail = 0;
	_count = 0;
	memset(_queue, 0, sizeof(_queue));
	memset(_dedup_history, 0, sizeof(_dedup_history));
	_dedup_idx = 0;
	if (_queue_mutex != NULL) {
		xSemaphoreGive(_queue_mutex);
	}
}

} // namespace RemoteObservationQueue
