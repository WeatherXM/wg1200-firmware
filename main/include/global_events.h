#ifndef GLOBAL_EVENTS_H
#define GLOBAL_EVENTS_H

#include "common.h"
#include "esp_event.h"
#include "ws/include/ws_data.h"
#include "ws/include/ws_base.h"
#include "ws/include/ws_fields.h"
#include "ws/include/ws_handler.h"
#include "public_api.h"

namespace GlobalEvents
{
ESP_EVENT_DECLARE_BASE(APP_EVENT);
const size_t QUEUE_MSG_DATA_MAX_SIZE_BYTES =  64;
const uint16_t PUBLISH_TIMEOUT_MS = 30 * 1000; // 30 seconds

//TODO State enumerations need to go to each task, not in here.

enum class WifiStateChangedEvent
{
    NONE,
	CONNECTED_NO_INTERNET,
	CONNECTED,
	DISCONNECTED
};

enum class MqttStateChangedEvent
{
    NONE,
	CONNECTED,
	DISCONNECTED
};

struct MqttDataChangedEvent
{
    void *data;
    int data_size;
};

struct MqttDataPublishedEvent
{
    int msg_id; /*!< Message id */
	bool retain; /*!< Retained flag of the message */
    int qos;     /*!< QoS of the messages*/
    bool dup;    /*!< Duplicate flag of the message*/
};

enum class GpsStateChangedEvent
{
    NONE,
    SIGNAL_FIX,
	SIGNAL_LOSS
};

enum class TimeSyncedEvent
{
    NONE,
    FIRST_SYNC,
	INTERVAL_SYNC
};

struct GpsDataChangedEvent
{
    float lat;
    float lon;
};

struct RmDataUpdatedEvent
{
    u32 run_ts = 0;
};

typedef WsHandler::ActivityStatus WsStatusChangedEvent;
typedef WsFields::FieldPack WsDataUpdatedEvent;
typedef u32 SettingsChangedEvent;

enum class UiEventType
{
    NONE,
    WS_UPDATE_CONTENT,
	CLAIM_REQUESTED,
	CLAIM_FINISHED,
    TOUCH_GENERIC,
    DRAG_GENERIC
};

struct UiEvent
{
    UiEventType ui_event_type;
    u32 tstamp;
};

struct WsInfoChangedEvent
{
	WsHandler::PairState pair_state;
	WsBase::PairData pair_data;
	const WsFreqDesc* freq_desc;
	WsBase::MultiStation* station;
};

enum class ClaimEnRespStatus
{
    NONE,
    CLAIM_ENABLE_SUCCESS,
	CLAIM_ENABLE_FAIL,
	CLAIM_ENABLE_TIMEOUT
};

struct ClaimEnRespEvent
{
    ClaimEnRespStatus type;
    u32 code;
};

typedef PublicApi::DevInfo PubApiDevInfEvent;
typedef PublicApi::Rewards PubApiRewardsEvent;

enum class EventId
{
    ANY_EVENT = ESP_EVENT_ANY_ID,
    NONE = 0,
    WIFI_STATE_CHANGED,
    MQTT_STATE_CHANGED,
    MQTT_DATA_RECEIVED,
	MQTT_DATA_PUBLISHED,
    GPS_STATE_CHANGED,
    GPS_DATA_RECEIVED,
    WS_STATUS_CHANGED,
    WS_INFO_CHANGED,
    WS_DATA_RECEIVED,
	TIME_SYNCED,
	CLAIM_ENABLE_RESPONSE,
    UI_EVENT,
	SETTINGS_CHANGED,
	RM_TS_DATA_UPDATED,
    PUB_API_DEV_INFO,
    PUB_API_REWARDS
};

struct QueueMsg
{
    EventId event_id;
    uint8_t event_data[QUEUE_MSG_DATA_MAX_SIZE_BYTES];
};

void init();
RetResult subscribe(EventId event_id, esp_event_handler_t handler, esp_event_handler_instance_t *handler_instance);
RetResult subscribe(EventId event_id, esp_event_handler_t handler, esp_event_handler_instance_t *handler_instance, void *handler_arg);
// RetResult subscribe_queue(EventId event_id, QueueHandle_t queue, esp_event_handler_instance_t *handler_instance); Needs further testing
RetResult unsubscribe(EventId event_id, esp_event_handler_instance_t *handler_instance);
RetResult wait(EventId event_id, void *event_data, int event_data_size, TickType_t xTicksToWait);
RetResult publish(EventId event_id, void *event_data, size_t event_data_size);
RetResult publish_from_isr(EventId event_id, void *event_data, size_t event_data_size, BaseType_t *task_unblocked);

}

#endif
