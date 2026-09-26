#ifndef WS_1001_H
#define WS_1001_H

#include "ws/include/ws_common.h"
#include "ws/include/ws_fields.h"
#include "ws/include/ws_base.h"
#include "ws_1001_decoders.h"
#include "ws/include/ws_gps.h"
#include "ws/include/ws_adapter.h"
#include "ws_1001_adapter_tb.h"
#include "const.h"
#include "struct.h"

namespace Ws1001
{

static const char* NAME = "WS1001";
const WsType TYPE = WS_TYPE_1001;
const uint16_t BROADCAST_INTERVAL_SEC = 20;
const uint16_t BUSY_DURATION_SEC = 720; // 12 minutes is the maximum timeout for ws1001 gps fixing

const WsBase::RfConfig RF_CONFIG = 
{
	.rf_type = WsBase::RfType::LORA, 
	.config = 
	{
		.config_lora = 
		{
			.bandwidth = WsBase::ConfigLora::BW_125_KHZ,
			.datarate = WsBase::ConfigLora::DR_128,
			.coderate = WsBase::ConfigLora::CR_4_TO_5,
			.preamble_len = 12,
			.symb_timeout = 0,
			.fixed_len = false,
			.pl_len = 9,
			.crc_on = false			
		}
	}
};

const WsBase::Config CONFIG = 
{
	.extended_pair_info = false,
	.user_select_freq = true,
	.user_pairable = true
};

// These fields can be extended per station without extending the type enum in WsFields.
// The extended fields can be on searched by name though.
static const WsFields::FieldDesc* FIELDS[] =
{
	&WsFields::FIELD_INFO_WEATHER_TEMP,
	&WsFields::FIELD_INFO_WEATHER_HUM,
	&WsFields::FIELD_INFO_WEATHER_WIND_DIR,
	&WsFields::FIELD_INFO_WEATHER_WIND_SPEED,
	&WsFields::FIELD_INFO_WEATHER_WIND_GUST,
	&WsFields::FIELD_INFO_WEATHER_ILLUMINANCE,
	&WsFields::FIELD_INFO_WEATHER_SOLAR_RAD,
	&WsFields::FIELD_INFO_WEATHER_UV_INDEX,
	&WsFields::FIELD_INFO_WEATHER_PRECIP_CML,

	&WsFields::FIELD_INFO_META_ID,
	&WsFields::FIELD_INFO_META_PCK_COUNT,
	&WsFields::FIELD_INFO_META_TX_POWER,
	&WsFields::FIELD_INFO_META_BAT_MV,
	&WsFields::FIELD_INFO_META_BAT_LOW,
	&WsFields::FIELD_INFO_META_RSSI,
	&WsFields::FIELD_INFO_META_FW_VER,
	&WsFields::FIELD_INFO_META_TX_INTERVAL,
};

static const uint8_t FIELDS_COUNT = sizeof(Ws1001::FIELDS)/sizeof(WsFields::FieldDesc*);

static const WsAdapter::AdapterValueName* ADAPTER_TB[] =
{
	&Ws1001AdapterTb::ADAPTER_TB_WEATHER_TEMP,
	&Ws1001AdapterTb::ADAPTER_TBWEATHER_HUM,
	&Ws1001AdapterTb::ADAPTER_TB_WEATHER_WIND_DIR,
	&Ws1001AdapterTb::ADAPTER_TB_WEATHER_WIND_SPEED,
	&Ws1001AdapterTb::ADAPTER_TB_WEATHER_WIND_GUST,
	&Ws1001AdapterTb::ADAPTER_TB_WEATHER_ILLUMINANCE,
	&Ws1001AdapterTb::ADAPTER_TB_WEATHER_SOLAR_RAD,
	&Ws1001AdapterTb::ADAPTER_TB_WEATHER_UV_INDEX,
	&Ws1001AdapterTb::ADAPTER_TB_WEATHER_PRECIP,

	&Ws1001AdapterTb::ADAPTER_TB_META_ID,
	&Ws1001AdapterTb::ADAPTER_TB_META_PCK_COUNT,
	&Ws1001AdapterTb::ADAPTER_TB_META_TX_POWER,
	&Ws1001AdapterTb::ADAPTER_TB_META_BAT_MV,
	&Ws1001AdapterTb::ADAPTER_TB_META_BAT_LOW,
	&Ws1001AdapterTb::ADAPTER_TB_META_RSSI,
	&Ws1001AdapterTb::ADAPTER_TB_META_FW_VER,
	&Ws1001AdapterTb::ADAPTER_TB_META_TX_INTERVAL,
};

static const uint8_t ADAPTER_TB_VALUES_COUNT = sizeof(Ws1001::ADAPTER_TB)/sizeof(WsAdapter::AdapterValueName*);

WsCommon::RetRes packet_handler(WsBase::GwCtx* ctx, WsCommon::Packet* pkt_received, 
	WsCommon::Packet* pkt_response, WsBase::StaCtx* meta);
WsCommon::RetRes packet_decoder(WsCommon::Packet* pkt_received,	WsFields::FieldPack* field_pack, WsGps::GpsData* gps_pack);
WsCommon::RetRes freq_resolver(u32 region_base_freq_hz, const WsBase::PairData* pair_data, u32* resolved_freq_hz);

/******************************************************************************
 * @brief Packet response to station
 ******************************************************************************/
struct AckPacket
{
	uint8_t size;
	union
	{
		struct {
			uint8_t interval: 6; // Range 1-30
			uint8_t reboot: 1; // True/False
			uint8_t req_gps: 1; // True/False
		};
		uint8_t raw;
	} aux_byte_1;
	union
	{
		struct {
			uint8_t power: 7; // Range 2-20
			uint8_t req_command: 1; // True/False
		};
		uint8_t raw;
	} aux_byte_2;
	const uint8_t sensor_type;
	uint8_t crc;
	uint8_t checksum;
	uint8_t ack_crc;
	uint8_t ack_checksum;
} __attribute__((packed));

/******************************************************************************
 * @brief Station packet type
 ******************************************************************************/
enum class PacketType
{
	NONE = 0,
	WEATHER_SIGNED,
	WEATHER_GPS_SIGNED
};

/******************************************************************************
 * @brief Firmware byte decoding struct
 ******************************************************************************/
typedef union
{
	struct {
		uint8_t patch: 5;
		uint8_t minor: 3;
	};
	uint8_t raw;
} FirmwareVersion;

const int SIGNATURE_LEN = 16;

}

#endif