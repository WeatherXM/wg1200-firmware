#ifndef WS_1300_H
#define WS_1300_H

#include "ws/include/ws_common.h"
#include "ws/include/ws_fields.h"
#include "ws/include/ws_base.h"
#include "ws_1300_decoders.h"
#include "ws/include/ws_gps.h"
#include "ws/include/ws_adapter.h"
#include "ws_1300_adapter_tb.h"
#include "const.h"
#include "struct.h"

namespace Ws1300
{

static const char* NAME = "WS1300";
const WsType TYPE = WS_TYPE_1300;
const uint16_t BROADCAST_INTERVAL_SEC = 20;
const uint16_t BUSY_DURATION_SEC = 720;
const uint16_t TIME_SYNC_INTERVAL = 6 * 60 * 60; // Every 6 hours

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
			.preamble_len = 8,
			.symb_timeout = 0,
			.fixed_len = false,
			.pl_len = 9, //?????
			.crc_on = true
		}
	}
};

const WsBase::Config CONFIG = 
{
	.extended_pair_info = true,
	.user_select_freq = false,
	.user_pairable = false
};

static const WsFields::FieldDesc* FIELDS[] =
{
	&WsFields::FIELD_INFO_WEATHER_TEMP,
	&WsFields::FIELD_INFO_WEATHER_HUM,
	&WsFields::FIELD_INFO_WEATHER_WIND_DIR,
	&WsFields::FIELD_INFO_WEATHER_WIND_SPEED,
	&WsFields::FIELD_INFO_WEATHER_WIND_GUST,
	&WsFields::FIELD_INFO_WEATHER_ILLUMINANCE,
	&WsFields::FIELD_INFO_WEATHER_UV_INDEX,
	&WsFields::FIELD_INFO_WEATHER_PRECIP_CML,
	&WsFields::FIELD_INFO_WEATHER_PRECIP_RATE,
	&WsFields::FIELD_INFO_WEATHER_PRESSURE,
	&WsFields::FIELD_INFO_WEATHER_TS,
	&WsFields::FIELD_INFO_WEATHER_SOLAR_RAD,

	&WsFields::FIELD_INFO_META_TX_INTERVAL,	
	&WsFields::FIELD_INFO_META_ID,
	&WsFields::FIELD_INFO_META_PCK_COUNT,
	&WsFields::FIELD_INFO_META_TX_POWER,
	&WsFields::FIELD_INFO_META_BAT_MV,
	&WsFields::FIELD_INFO_META_BAT_LOW,
	&WsFields::FIELD_INFO_META_RSSI,
	&WsFields::FIELD_INFO_META_FW_VER,
};

static const uint8_t FIELDS_COUNT = sizeof(Ws1300::FIELDS)/sizeof(WsFields::FieldDesc*);

static const WsAdapter::AdapterValueName* ADAPTER_TB[] =
{
	&Ws1300AdapterTb::ADAPTER_TB_WEATHER_TEMP,
	&Ws1300AdapterTb::ADAPTER_TBWEATHER_HUM,
	&Ws1300AdapterTb::ADAPTER_TB_WEATHER_WIND_DIR,
	&Ws1300AdapterTb::ADAPTER_TB_WEATHER_WIND_SPEED,
	&Ws1300AdapterTb::ADAPTER_TB_WEATHER_WIND_GUST,
	&Ws1300AdapterTb::ADAPTER_TB_WEATHER_ILLUMINANCE,
	&Ws1300AdapterTb::ADAPTER_TB_WEATHER_SOLAR_RAD,
	&Ws1300AdapterTb::ADAPTER_TB_WEATHER_UV_INDEX,
	&Ws1300AdapterTb::ADAPTER_TB_WEATHER_PRECIP_CML,
	&Ws1300AdapterTb::ADAPTER_TB_WEATHER_PRECIP_RATE,	
	&Ws1300AdapterTb::ADAPTER_TB_WEATHER_SOLAR_RAD,
	&Ws1300AdapterTb::ADAPTER_TB_WEATHER_PRESSURE,

	&Ws1300AdapterTb::ADAPTER_TB_META_TX_INTERVAL,
	&Ws1300AdapterTb::ADAPTER_TB_META_ID,
	&Ws1300AdapterTb::ADAPTER_TB_META_PCK_COUNT,
	&Ws1300AdapterTb::ADAPTER_TB_META_TX_POWER,
	&Ws1300AdapterTb::ADAPTER_TB_META_BAT_MV,
	&Ws1300AdapterTb::ADAPTER_TB_META_BAT_LOW,
	&Ws1300AdapterTb::ADAPTER_TB_META_RSSI,
	&Ws1300AdapterTb::ADAPTER_TB_META_FW_VER,
};

static const uint8_t ADAPTER_TB_VALUES_COUNT = sizeof(Ws1300::ADAPTER_TB)/sizeof(WsAdapter::AdapterValueName*);

WsCommon::RetRes packet_handler(WsBase::GwCtx* ctx, WsCommon::Packet* pkt_received, 
	WsCommon::Packet* pkt_response, WsBase::StaCtx* meta);
WsCommon::RetRes packet_decoder(WsCommon::Packet* pkt_received,	WsFields::FieldPack* field_pack, WsGps::GpsData* gps_pack);
WsCommon::RetRes freq_resolver(u32 region_base_freq_hz, const WsBase::PairData* pair_data, u32* resolved_freq_hz);

/** Length of hmac */
const int HMAC_LEN = 16;

/** Length of gateway's serial number */
const int GW_SERIAL_NUMBER_LEN = 10;

const int PUB_KEY_UNC_LEN = 64;

const int SIGNATURE_LEN = 64;

const int SN_LEN = 10;

const int MODEL_ID = 0x01;

const uint16_t FREQ_CHANNEL_GAP_KHZ = 200;

const uint16_t FREQ_BASE_OFFSET_KHZ = 100;

//
// Types
//

/**
 * Available STA packet types
 */
enum class StaPacketType : u8
{
	NONE = 0,
	BOOT = 0x01,
	ADV = 0x02,
	SENSORS = 0x03,
	GNSS = 0x04
};

/**
* Available Resp packet types
*/
enum class RespPacketType : u8
{
	NONE = 0,
	ACK = 0x01,
	CMD = 0x02,
	PAIR = 0x03,
	TIME_SYNC = 0x04
};

namespace ByteBuffer
{

	template <typename T>
	struct BigEndian{
		union{
			u8 bytes[sizeof(T)];
			T le_cast;
		};
	};

	template <typename T>
	static inline T be_to_le(BigEndian<T> be_val)
	{
		Utils::reverse_bytes(be_val.bytes, sizeof(T));
		return be_val.le_cast;
	}

	template <typename T>
	static inline BigEndian<T> le_to_be(T le_val)
	{
		BigEndian<T> be_val;
		be_val.le_cast = le_val;
		Utils::reverse_bytes(be_val.bytes, sizeof(be_val.bytes));
		return be_val;
	}

	struct StaHeader
	{
		u8 model_id = 0x01; // TODO: Handle const where?
		u8 fw_ver;
		u8 hw_ver;
		StaPacketType packet_type;
		BigEndian<u32> station_id;
		BigEndian<u16> packet_id;
		u8 last_resp_rssi;
		union
		{
			struct {
				u8 tx_power: 5;			
				u8 unused: 3;			
			} __attribute__((packed)); 
			u8 raw;
		} aux_byte_1;
		u8 tx_int;
		u8 batt_mv;
		union
		{
			struct {
				bool contains_sig: 1;
				bool acquiring_gps: 1;
				bool low_batt: 1;
				bool int_batt: 1;
				bool ext_batt: 1;
				u8 unused: 3;
			} __attribute__((packed));
			uint8_t raw;
		} aux_byte_2;
	} __attribute__((packed));
	static_assert(sizeof(StaHeader) == 15);

	struct StaBoot
	{
		StaHeader header;
		u8 fo_ver;
		u8 boot_reason;
		u8 pub_key[PUB_KEY_UNC_LEN];
		u8 sn[SN_LEN];
	} __attribute__((packed));
	static_assert(sizeof(StaBoot) == sizeof(StaHeader) + 76);

	struct StaAdv
	{
		StaHeader header;
		u8 pub_key[PUB_KEY_UNC_LEN];
		u8 sn[SN_LEN];
	} __attribute__((packed));
	static_assert(sizeof(StaAdv) == sizeof(StaHeader) + 74);

	struct StaSensors
	{
		StaHeader header;
		BigEndian<u32> ts;
		u8 sample_cnt;
		BigEndian<i16> temp;
		u8 hum;
		BigEndian<u16> wind_speed;
		BigEndian<u16> wind_gust;
		BigEndian<u16> wind_dir;
		BigEndian<float> precip_cml;
		BigEndian<u16> precip_rate;
		u8 uv_index;
		BigEndian<u24> illum;
		BigEndian<u16> press;
		u8 sig[SIGNATURE_LEN];
	} __attribute__((packed));
	static_assert(sizeof(StaSensors) == sizeof(StaHeader) + 90);

	struct StaGnss
	{
		StaHeader header;
		BigEndian<u32> ts;
		BigEndian<i32> lat;
		BigEndian<i32> lon;
		BigEndian<i32> height;
		BigEndian<i32> height_orth;
		u8 sats;
		BigEndian<u32> accuracy_hor;
		BigEndian<u32> accuracy_vert;
		BigEndian<u16> dop;
		union
		{
			struct {
				bool fix_ok: 1;
				bool llh_invalid: 1;
				u8 fix_type: 3;
				u8 unused: 3;
			} __attribute__((packed));
			uint8_t raw;
		} aux_byte;
		u8 sig[SIGNATURE_LEN];
	} __attribute__((packed));
	static_assert(sizeof(StaGnss) == sizeof(StaHeader) + 96);

	struct StaLog
	{
		StaHeader header;
		BigEndian<u32> ts;
		BigEndian<u16> code;
		BigEndian<u32> meta;
	} __attribute__((packed));
	static_assert(sizeof(StaLog) == sizeof(StaHeader) + 10);

	struct RespHeader
	{
		RespPacketType type;
		BigEndian<u32> station_id;
		BigEndian<u16> packet_id;
	} __attribute__((packed));
	static_assert(sizeof(RespHeader) == 7);

	struct RespAck
	{
		RespHeader header;
	} __attribute__((packed));
	static_assert(sizeof(RespAck) == sizeof(RespHeader));

	struct RespCmdV1
	{
		RespHeader header;
		bool acquire_gps: 1;
		bool reboot: 1;
		bool enable_logs: 1;
		u8 unused_1: 5;
		u8 tx_pwr: 5;
		u8 unused_2: 3;
		u8 tx_interval: 6;
		u8 unused_3: 2;
		u8 hmac[HMAC_LEN];
	} __attribute__((packed));
	static_assert(sizeof(RespCmdV1) == sizeof(RespHeader) + 19);

	struct RespCmdV2
	{
		RespHeader header;
		bool acquire_gps: 1;
		bool reboot: 1;
		bool enable_logs: 1;
		u8 unused_1: 5;
		u8 tx_pwr: 5;
		u8 unused_2: 3;
		u8 tx_interval: 6;
		u8 unused_3: 2;
		u8 meshtastic_interval_min;
		u8 hmac[HMAC_LEN];
	} __attribute__((packed));
	static_assert(sizeof(RespCmdV2) == sizeof(RespHeader) + 20);

	static const u8 RESP_CMD_V3_FORMAT_VER = 0x03;

	enum class CmdTlvTag : u8
	{
		FLAGS = 0x01,                   // len 1: [b0: acquire_gps, b1: reboot, b2: enable_logs]
		TX_POWER = 0x02,                // len 1: [tx_power (dBm)]
		TX_INTERVAL = 0x03,             // len 1: [tx_interval (seconds / 20)]
		MESHTASTIC_CONFIG = 0x10,       // len 2: [interval_min, hop_limit]
		MESHTASTIC_REGION_PRESET = 0x11,// len 2: [region_code, modem_preset]
		MESHTASTIC_CUSTOM_RF = 0x12,    // len 9: [freq_hz (4B BE), bw_khz (2B BE), sf (1B), cr (1B), tx_power_dbm (1B)]
		MESHCORE_CONFIG = 0x20,         // len 2: [interval_min, profile_idx]
		MESHCORE_CUSTOM_RF = 0x21,      // len 9: [freq_hz (4B BE), bw_khz (2B BE), sf (1B), cr (1B), tx_power_dbm (1B)]
		WEATHER_POLICY = 0x30           // len 31: AdaptivePublish::WeatherPolicyWire
	};

	struct RespCmdV3Header
	{
		RespHeader header;
		u8 format_ver;
		u8 cmd_seq;
		u8 tlv_len;
	} __attribute__((packed));
	static_assert(sizeof(RespCmdV3Header) == sizeof(RespHeader) + 3);

	typedef RespCmdV1 RespCmd;

	struct RespPair
	{
		RespHeader header;
		u8 sn[GW_SERIAL_NUMBER_LEN];
		u8 hmac[HMAC_LEN];
	} __attribute__((packed));
	static_assert(sizeof(RespPair) == sizeof(RespHeader) + 26);

	struct RespTimeSync
	{
		RespHeader header;
		BigEndian<u32> ts;
		u8 hmac[HMAC_LEN];
	} __attribute__((packed));
	static_assert(sizeof(RespTimeSync) == sizeof(RespHeader) + 20);

}

}

#endif