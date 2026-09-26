#ifndef REMOTE_OBSERVATION_QUEUE_H
#define REMOTE_OBSERVATION_QUEUE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "common.h"
#include "ws/include/ws_common.h"
#include "ws/include/ws_fields.h"

namespace RemoteObservationQueue
{

constexpr size_t MAX_RAW_PAYLOAD_LEN = 128;
constexpr size_t QUEUE_CAPACITY = 16;
constexpr size_t DEDUP_HISTORY_CAPACITY = 32;

struct RemoteObservation
{
	uint32_t station_id;
	uint32_t timestamp;
	uint8_t raw_payload[MAX_RAW_PAYLOAD_LEN];
	uint8_t raw_payload_len;
	int16_t rssi;
	int8_t snr;
	bool is_signed;
	int64_t rx_uptime_ms;
	WsFields::FieldPack field_pack;
};

WsCommon::RetRes init();
WsCommon::RetRes push(const RemoteObservation &obs);
WsCommon::RetRes pop(RemoteObservation *obs);
int get_count();
void clear();

} // namespace RemoteObservationQueue

#endif // REMOTE_OBSERVATION_QUEUE_H
