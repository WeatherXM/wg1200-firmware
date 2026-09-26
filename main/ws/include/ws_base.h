#ifndef WS_BASE_H
#define WS_BASE_H

#include "ws_common.h"
#include "ws_fields.h"
#include "ws_gps.h"
#include "ws_adapter.h"
#include "credentials.h"

namespace WsBase
{

/******************************************************************************
 * @brief RF type enumeration
 ******************************************************************************/
enum class RfType
{
	NONE = 0,
	LORA,
	FSK
};

/******************************************************************************
 * @brief Provide configuration for LoRa
 ******************************************************************************/
struct ConfigLora
{
	enum {
		BW_125_KHZ = 0,
		BW_250_KHZ,
		BW_500_KHZ
	}bandwidth;

	enum {
		DR_64 = 6,
		DR_128,
		DR_256,
		DR_512,
		DR_1024,
		DR_2048,
		DR_4096
	}datarate;

	enum {
		CR_4_TO_5 = 1,
		CR_4_TO_6,
		CR_4_TO_7,
		CR_4_TO_8
	}coderate;

	uint16_t preamble_len;
	uint16_t symb_timeout;
	bool fixed_len;
	uint16_t pl_len;
	bool crc_on;
};

/******************************************************************************
 * @brief Provide configuration for FSK
 ******************************************************************************/
struct ConfigFsk
{

};


/******************************************************************************
 * @brief RF config. Provide configuration either for LoRa or FSK
 ******************************************************************************/
struct RfConfig
{
	WsBase::RfType rf_type;
	union{
		ConfigFsk config_fsk;
		ConfigLora config_lora;
	} config;	
};

struct PairData
{
	uint32_t id = 0x00;
	u8 pubkey[WS_PUB_KEY_LEN];
	u8 sn[WS_SN_LEN];
};

/******************************************************************************
 * @brief Context provided by ws_handler to station packet handler
 ******************************************************************************/
struct GwCtx
{
	PairData paired_info = {};
	bool is_paired = false;
	bool gps_needed = false;
	u8 gw_sn[Credentials::DEV_SERIAL_LENGTH];	
	bool time_is_valid = false;
	u32 cur_ts = 0;
};

/******************************************************************************
 * @brief Station context after packet is handled by station's packet handler
 ******************************************************************************/
struct StaCtx
{
	uint16_t req_tx_power = 0;
	bool reboot_detected = false;
	bool ack_loss_detected = false;
	uint16_t lost_packets = 0;
	uint16_t packet_index = 0;
	bool packet_index_valid = false;
	bool gps_req_permitted = false;
	bool gps_pkt_received = false;
	bool is_busy = false;
	PairData pair_data = {};
	bool pair_permitted = false;
};

/******************************************************************************
 * @brief Packet handler callback
 ******************************************************************************/
typedef WsCommon::RetRes (*PacketHandler)(
	GwCtx* ctx,
	WsCommon::Packet* pkt_received,
	WsCommon::Packet* pkt_response,
	StaCtx* meta);

/******************************************************************************
 * @brief Packet decoder callback
 ******************************************************************************/
typedef WsCommon::RetRes (*PacketDecoder)(
	WsCommon::Packet* pkt_received,
	WsFields::FieldPack* field_pack,
	WsGps::GpsData* gps_pack);

/******************************************************************************
 * @brief Frequency resolver callback
 ******************************************************************************/
typedef WsCommon::RetRes (*FreqResolver)(
	u32 region_base_freq_hz, 
	const PairData* pair_data,
	u32* resolved_freq_hz);

/******************************************************************************
 * @brief Configuration
 ******************************************************************************/
struct Config
{
	bool extended_pair_info = false;
	bool user_select_freq = false;
	bool user_pairable = false;
};

/******************************************************************************
 * @brief Multistation struct
 ******************************************************************************/
struct MultiStation
{
	const char* NAME = "";
	const WsType TYPE = WS_TYPE_NONE;
	const RfConfig RF_CONFIG = {};
	const Config CONFIG = {};
	const WsFields::FieldDesc** FIELDS = NULL;
	const uint8_t FIELDS_COUNT = 0;
	const PacketHandler packet_handler = NULL;
	const PacketDecoder packet_decoder = NULL;
	const FreqResolver freq_resolver = NULL;
	const uint16_t BROADCAST_INTERVAL_SEC = 0;
	const uint16_t BUSY_DURATION_SEC = 0;
	const WsAdapter::AdapterValueName** ADAPTER_TB = NULL;
	const uint8_t ADAPTER_TB_VALUES_COUNT = 0;
};

}

#endif
