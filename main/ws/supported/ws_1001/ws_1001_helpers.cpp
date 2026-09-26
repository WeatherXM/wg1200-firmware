#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS

#include "include/ws_1001_decoders.h"
#include "include/ws_1001_helpers.h"
#include "include/ws_1001_keys.h"
#include "ws/include/ws_handler.h"
#include "esp_log.h"
#include "log.h"
#include "string.h"
#include "mbedtls/md.h"

#if __has_include("mbedtls/private/aes.h")
#include "mbedtls/private/aes.h"
#else
#include "mbedtls/aes.h"
#endif
#include "env.h"
#include <math.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

#define NMEA_STR "GNRMC,%2d%2d%2d.%3d,%c,%[^,],%c,%[^,],%c,%f,%f,%2d%2d%2d"

namespace Ws1001Helpers
{

// Power levels
const int POWER_LEVEL_MAX = 20;
const int POWER_LEVEL_MIN = 2;
const Ws1001::FirmwareVersion SUPPORTED_FW_VERSION = (Ws1001::FirmwareVersion) 0x42; // (minor:2, patch:2)
const uint8_t SUPPORTED_STATION_CODE = 0x69;

static uint8_t _current_power_level = 0;
static uint16_t _last_packet_index = 0;
static uint16_t _last_opt_index = 0;
static bool _power_dcr_period_long_en = false;

/******************************************************************************
 * @brief Calculate CRC
 * @param buff Data
 * @param len Data length
 * @param polynomial Polynomial to use for CLC calc
 * @param init Initial remainer
 * @return Calculated CRC
 *****************************************************************************/
uint8_t calc_crc(uint8_t const buff[], int len, uint8_t polynomial, uint8_t init)
{
	uint8_t remainder = init;

	for (int i = 0; i < len; ++i)
	{
		remainder ^= buff[i];
		for (int j = 0; j < 8; ++j)
		{
			if (remainder & 0x80)
				remainder = (remainder << 1) ^ polynomial;
			else
				remainder = (remainder << 1);
		}
	}
	return remainder;
}

/******************************************************************************
 * @brief Calculate checksum
 * @param buff Data
 * @param buff Data length
 * @return Calculated checksum
 *****************************************************************************/
uint8_t calc_checksum(uint8_t const buff[], int len)
{
	uint8_t checksum = 0;
	for (int i = 0; i < len; i++)
	{
		checksum += buff[i];
	}

	return checksum;
}

/******************************************************************************
 * @brief Get packet size value from packet
 * @param pkt Packet data
 * @return Packet size
 *****************************************************************************/
uint8_t get_packet_size(WsCommon::Packet* pkt)
{
	const int PACKET_SIZE_INDEX = 0;
	if(pkt->pl_len <= PACKET_SIZE_INDEX) return 0;
	return pkt->pl[PACKET_SIZE_INDEX];
}

/******************************************************************************
 * @brief Get packet type from buffer.
 * @param pkt Packet data
 * @return Ws1001::PacketType
 ******************************************************************************/
Ws1001::PacketType get_packet_type(WsCommon::Packet* pkt)
{
	const int PACKET_TYPE_INDEX = 12;
	if(pkt->pl_len <= PACKET_TYPE_INDEX) return Ws1001::PacketType::NONE;

	uint8_t type =  pkt->pl[PACKET_TYPE_INDEX];

	switch (type)
	{
		case 0x02:
			return Ws1001::PacketType::WEATHER_SIGNED;
			break;
		case 0x04:
			return Ws1001::PacketType::WEATHER_GPS_SIGNED;
			break;
		default:
			return Ws1001::PacketType::NONE;
	}

	return Ws1001::PacketType::NONE;
}

/******************************************************************************
 * @brief Get station code from buffer
 * @param pkt Packet data
 * @return Packet station code. Should be 0x69
 ******************************************************************************/
uint8_t get_packet_station_code(WsCommon::Packet* pkt)
{
	const int PACKET_STATION_CODE_INDEX = 2;
	if(pkt->pl_len <= PACKET_STATION_CODE_INDEX) return 0;
	return pkt->pl[PACKET_STATION_CODE_INDEX];
}

/******************************************************************************
 * @brief Get pointer to GNSS data from buffer
 * @param pkt Packet data
 * @return Pointer to GNSS data
 ******************************************************************************/
uint8_t* get_packet_gps_buff_pt(WsCommon::Packet* pkt)
{
	const int PACKET_GNSS_DATA_INDEX = 26;
	if(pkt->pl_len <= PACKET_GNSS_DATA_INDEX) return NULL;
	return &pkt->pl[PACKET_GNSS_DATA_INDEX];
}

/******************************************************************************
 * @brief Get GNSS data length from buffer
 * @param pkt Packet data
 * @return Pointer to GNSS data
 ******************************************************************************/
uint8_t get_packet_gps_buff_len(WsCommon::Packet* pkt)
{
	const int PACKET_GNSS_LEN_INDEX = 25;
	if(pkt->pl_len <= PACKET_GNSS_LEN_INDEX) return 0;
	return pkt->pl[PACKET_GNSS_LEN_INDEX];
}

/******************************************************************************
 * @brief Get CRC byte from buffer
 * @param pkt Packet data
 * @return CRC
 ******************************************************************************/
uint8_t get_packet_crc(WsCommon::Packet* pkt)
{
	const int PACKET_CRC_INDEX = 25;
	if(pkt->pl_len <= PACKET_CRC_INDEX) return 0;
	uint8_t crc = pkt->pl[PACKET_CRC_INDEX];
	if(get_packet_type(pkt) == Ws1001::PacketType::WEATHER_GPS_SIGNED)
	{
		crc = pkt->pl[get_packet_size(pkt) - 2];
	}
	return  crc;
}

/******************************************************************************
 * @brief Get Checksum byte from buffer
 * @param pkt Packet data
 * @return Checksum
 ******************************************************************************/
uint8_t get_packet_checksum(WsCommon::Packet* pkt)
{
	const int PACKET_CHECKSUM_INDEX = 26;
	if(pkt->pl_len <= PACKET_CHECKSUM_INDEX) return 0;
	uint8_t checksum = pkt->pl[PACKET_CHECKSUM_INDEX];
	if(get_packet_type(pkt) == Ws1001::PacketType::WEATHER_GPS_SIGNED)
	{
		checksum = pkt->pl[get_packet_size(pkt) - 1];
	}
	return checksum;
}

/******************************************************************************
 * @brief Calculate station status metadata needed for the response packet
 * @param pkt Packet data
 * @param sta_ctx Pointer to status metadata
 * @return Void
 * @todo Separate power calc algo
 ******************************************************************************/
void calc_sta_status(WsCommon::Packet* pkt, WsBase::StaCtx* sta_ctx)
{
	// Algorithm targetting to 1 packet lost every ~3 days
	const uint16_t POWER_DECREASE_PERIOD_SHORT = 90; // 90 packet count, ~30min for 20s interval
	const uint16_t POWER_DECREASE_PERIOD_LONG = 4320; // 4320 packet count, ~1d for 20s interval

	uint16_t tx_power;
	uint16_t tx_interval;
	uint32_t packet_count;
	Ws1001Decoders::decode_meta_tx_power(pkt, &tx_power);
	Ws1001Decoders::decode_meta_pkt_count(pkt, &packet_count);
	Ws1001Decoders::decode_meta_tx_interval(pkt, &tx_interval);
	sta_ctx->packet_index = (uint16_t)packet_count;
	sta_ctx->packet_index_valid = true;

	if(_current_power_level != 0 && 
		(tx_power != _current_power_level || tx_interval != Ws1001::BROADCAST_INTERVAL_SEC))
	{
		sta_ctx->ack_loss_detected = true;
	}

	bool gps_fixing = false;
	sta_ctx->gps_req_permitted = true;
	Ws1001Decoders::decode_meta_gps_fixing(pkt, &gps_fixing);
	if(gps_fixing)
	{
		sta_ctx->is_busy = true;
		sta_ctx->gps_req_permitted = false;
	}

	if(Ws1001Helpers::is_gps_data_valid(pkt))
	{
		sta_ctx->gps_req_permitted = false;
	}
	
	if(Ws1001Helpers::get_packet_type(pkt) == Ws1001::PacketType::WEATHER_GPS_SIGNED)
	{
		sta_ctx->gps_pkt_received = true;
	}

	_current_power_level = _current_power_level == 0 ? tx_power : _current_power_level;
	_last_opt_index = _last_opt_index == 0 ? packet_count : _last_opt_index;

	// Handle duplicate packet without altering power adaptation or underflowing lost counter
	if(_last_packet_index != 0 && packet_count == _last_packet_index)
	{
		sta_ctx->lost_packets = 0;
		sta_ctx->req_tx_power = _current_power_level;
		return;
	}

	// Check reboot or wrap-around vs packet loss
	uint16_t lost_packets = 0;

	if(_last_packet_index == 0)
	{
		lost_packets = 0;
	}
	else if(packet_count > _last_packet_index)
	{
		lost_packets = packet_count - 1 - _last_packet_index;
	}
	else // packet_count < _last_packet_index
	{
		// Check 16-bit sequence roll-over
		uint16_t rollover_delta = (uint16_t)(packet_count - _last_packet_index);
		if(rollover_delta < 100)
		{
			lost_packets = (rollover_delta > 1) ? (rollover_delta - 1) : 0;
		}
		else
		{
			// Station reboot
			_last_packet_index = 0;
			_current_power_level = tx_power;
			_last_opt_index = packet_count;
			_power_dcr_period_long_en = false;
			sta_ctx->reboot_detected = true;
			lost_packets = 0;
		}
	}

	sta_ctx->lost_packets = lost_packets;

	if(lost_packets > 0 && !_power_dcr_period_long_en)
	{
		_power_dcr_period_long_en = true;
	}

	// Select power decrease period
	uint16_t power_decrease_period = _power_dcr_period_long_en ? POWER_DECREASE_PERIOD_LONG : POWER_DECREASE_PERIOD_SHORT;
	// ESP_LOGI(TAG, "power_decrease_period: %d", power_decrease_period);

	// Decrease power if no packets lost during POWER_DECREASE_PERIOD_LONG/POWER_DECREASE_PERIOD_SHORT
	// Increase power if any packet is lost during POWER_DECREASE_PERIOD_LONG/POWER_DECREASE_PERIOD_SHORT
	if(lost_packets != 0)
	{
		if(_current_power_level < POWER_LEVEL_MAX)
		{
			_current_power_level++;
		}		
		_last_opt_index = packet_count;
	}
	else if((uint16_t)(packet_count - _last_opt_index) > power_decrease_period)
	{
		if(_current_power_level > POWER_LEVEL_MIN)
		{
			_current_power_level--;
		}		
		_last_opt_index = packet_count;

		// If waited POWER_DECREASE_PERIOD_LONG, continue with POWER_DECREASE_PERIOD_SHORT
		_power_dcr_period_long_en = false;
	}
	// ESP_LOGI(TAG, "current_power_level: %d", current_power_level);

	_last_packet_index = packet_count;

	sta_ctx->req_tx_power = _current_power_level;
}

/******************************************************************************
 * @brief Generate ack packet
 * @param pkt Packet data
 * @param req_power TX power to be requested
 * @param req_gps true if GPS fix is needed
 * @param req_interval TX itnerval to be requested
 * @param req_reboot true if station reboot is needed
 * @param out Pointer to buffer holding the ack to be generated
 * @retval true if signature is needed
 * @retval false if signature is not needed.
 ******************************************************************************/
bool gen_ack_base(WsCommon::Packet* pkt, uint8_t req_power, bool req_gps, uint8_t req_interval, bool req_reboot, uint8_t *out)
{
	Ws1001::AckPacket ack = {
		.size = sizeof(Ws1001::AckPacket),
		.aux_byte_1 = 0x00,
		.aux_byte_2 = 0x00,
		.sensor_type = 0x69,
		.crc = get_packet_crc(pkt),
		.checksum = get_packet_checksum(pkt),
		.ack_crc = 0,
		.ack_checksum = 0
	};

	bool command_needed = false;
	uint16_t tx_power;
	Ws1001Decoders::decode_meta_tx_power(pkt, &tx_power);

	if(req_gps || req_power != tx_power || req_reboot)
	{
		command_needed = true;
	}

	ack.aux_byte_1.interval = req_interval / 10U;
	ack.aux_byte_1.reboot = req_reboot ? 1U : 0U;
	ack.aux_byte_1.req_gps = req_gps ? 1U : 0U;
	ack.aux_byte_2.power = req_power;
	ack.aux_byte_2.req_command = command_needed ? 1U : 0U;
	ack.ack_crc = calc_crc((uint8_t*)&ack, sizeof(ack) - 2, 0x31, 0);
	ack.ack_checksum = calc_checksum((uint8_t*)&ack, sizeof(ack) - 1);

	memcpy(out, &ack, sizeof(ack));

	// if command is needed then signature is needed
	return command_needed;
}

/******************************************************************************
 * @brief Validate received packet
 * @param pkt Packet data
 * @retval ERROR_OUT_OF_BOUNDS if packet size is not correct
 * @retval ERROR_NOT_SUPPORTED wrong packet type, station code or fw version
 * @retval ERROR_BAD_CRC CRC is wrong
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes packet_validate(WsCommon::Packet* pkt)
{
	if((pkt->pl_len - Ws1001::SIGNATURE_LEN) != get_packet_size(pkt))
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}

	uint8_t calced_crc = calc_crc(pkt->pl, get_packet_size(pkt) - 2, 0x31, 0);
	uint8_t calced_checksum = calc_checksum(pkt->pl, get_packet_size(pkt) - 1);
	if(calced_crc != get_packet_crc(pkt) || calced_checksum != get_packet_checksum(pkt))
	{
		return WsCommon::RetRes::ERROR_BAD_CRC;
	}	

	// Packet passed CRC and Checksum verification! Log warnings for unexpected header metadata but accept it.
	if(get_packet_type(pkt) == Ws1001::PacketType::NONE)
	{
		ESP_LOGW("WS_HELPERS", "WS1001 unexpected packet type 0x%02X accepted due to valid CRC",
			pkt->pl_len > 12 ? pkt->pl[12] : 0);
	}

	if(get_packet_station_code(pkt) != SUPPORTED_STATION_CODE)
	{
		ESP_LOGW("WS_HELPERS", "WS1001 unexpected station code 0x%02X accepted due to valid CRC",
			get_packet_station_code(pkt));
	}

	uint8_t val;
	Ws1001Decoders::decode_meta_fw_ver(pkt, &val);

	Ws1001::FirmwareVersion fw_ver;
	fw_ver.raw = val;

	if(fw_ver.raw == 0x41 || fw_ver.raw == 0x42)
	{
		// Production station firmware versions in the field: accepted without warnings
		WsHandler::set_station_fw_warning("");
	}
	else if(fw_ver.raw < 0x41)
	{
		uint8_t minor = fw_ver.raw >> 5;
		uint8_t patch = fw_ver.raw & 0x1F;
		char warn_msg[64];
		snprintf(warn_msg, sizeof(warn_msg), "Station FW 1.%u.%u (0x%02X) < prod 0x41/0x42", minor, patch, fw_ver.raw);
		WsHandler::set_station_fw_warning(warn_msg);

		static uint8_t last_logged_old_fw = 0;
		if(last_logged_old_fw != fw_ver.raw)
		{
			ESP_LOGW("WS_HELPERS", "WS1001 station with older firmware detected: 1.%u.%u (0x%02X). Production is 0x41 (1.2.1) or 0x42 (1.2.2).", minor, patch, fw_ver.raw);
			last_logged_old_fw = fw_ver.raw;
		}
	}
	else // fw_ver.raw > 0x42
	{
		uint8_t minor = fw_ver.raw >> 5;
		uint8_t patch = fw_ver.raw & 0x1F;
		char warn_msg[64];
		snprintf(warn_msg, sizeof(warn_msg), "Station FW 1.%u.%u (0x%02X) > prod 0x41/0x42", minor, patch, fw_ver.raw);
		WsHandler::set_station_fw_warning(warn_msg);

		static uint8_t last_logged_new_fw = 0;
		if(last_logged_new_fw != fw_ver.raw)
		{
			ESP_LOGW("WS_HELPERS", "WS1001 station with newer firmware detected: 1.%u.%u (0x%02X). Production is 0x41 (1.2.1) or 0x42 (1.2.2).", minor, patch, fw_ver.raw);
			last_logged_new_fw = fw_ver.raw;
		}
	}

	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Checks if GNSS data are valid
 * @param pkt Packet data
 * @retval true if valid
 ******************************************************************************/
bool is_gps_data_valid(WsCommon::Packet* pkt)
{

	if(get_packet_type(pkt) != Ws1001::PacketType::WEATHER_GPS_SIGNED) return false;

	// UTC time and date parts
	int time_hr = 0, time_min = 0, time_sec = 0, time_ms = 0,
		date_d = 0, date_m = 0, date_y = 0;

	// Position status A|V
	char pos_status = 0;

	// Lat/lon directions N|S / W|E
	char lat_dir = 0, lon_dir = 0;

	// Ground speed in knots
	float speed_kn = 0;

	// Direction (N reference)
	float speed_dir = 0;

	// Lat/lon position strings
	// Lat: DDmm.mm (fixed len, decimal part 7 digits)
	// Long DDDmm.mm (fixed len, decimal part 7 digits)
	char str_lat[12] = "", str_lon[12] = "";

	char *gnss_str = (char *)get_packet_gps_buff_pt(pkt);
	if(gnss_str == NULL) return false;

	int items = sscanf(gnss_str, "GNRMC,%2d%2d%2d.%3d,%c,%[^,],%c,%[^,],%c,%f,%f,%2d%2d%2d",
					   &time_hr, &time_min, &time_sec, &time_ms, &pos_status,
					   str_lat, &lat_dir, str_lon, &lon_dir,
					   &speed_kn, &speed_dir, &date_d, &date_m, &date_y);
	
	if (pos_status == 'A')
	{
		return true;
	}
	return false;
}

/******************************************************************************
 * @brief Generate signature either for packet verification or ack
 * @param pkt Packet data
 * @retval Void
 ******************************************************************************/
void generate_signature(WsCommon::Packet* pkt, uint8_t *key, uint8_t *iv,uint8_t *sig, size_t sig_len)
{
	const int AES_BLOCK_SIZE = 16;

	if(sig_len != Ws1001::SIGNATURE_LEN)
	{
		return; //error
	}

	uint8_t out[128] = {};

	// TODO: Mbedtls stuff should go to utils as independent function, not coupled to WS1001 namespace

	// To avoid reallocating a buffer for in + padding bytes, allocate a static 16b block,
	// copy remainder bytes from last block in input and pad the rest.
	// In this case encryption will take 2 steps, encrypt input without the last block and
	// then the last block separately.
	uint8_t padded_buff[16] = {};

	int encrypt_len = pkt->pl_len; // Bytes from buff to encrypt
	int remainder_len = encrypt_len % AES_BLOCK_SIZE;
	int padding_len = AES_BLOCK_SIZE - remainder_len;

	if (remainder_len > 0)
	{
		uint8_t *remainder = pkt->pl + encrypt_len - remainder_len;
		memcpy(padded_buff, remainder, remainder_len);
		memset(padded_buff + remainder_len, (uint8_t)padding_len, padding_len);
	}

	//
	// Calc cipher
	//
	mbedtls_aes_context aes_ctx;
	mbedtls_aes_init(&aes_ctx);
	mbedtls_aes_setkey_enc(&aes_ctx, key, 128);
	mbedtls_aes_crypt_cbc(&aes_ctx, MBEDTLS_AES_ENCRYPT, encrypt_len - remainder_len, iv, pkt->pl, out);

	if (remainder_len > 0)
	{
		mbedtls_aes_crypt_cbc(&aes_ctx, MBEDTLS_AES_ENCRYPT, AES_BLOCK_SIZE, iv, padded_buff, out + (encrypt_len - remainder_len));
	}
	mbedtls_aes_free(&aes_ctx);

	//
	// MD5
	//
	const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
	if (md_info != NULL)
	{
		mbedtls_md(md_info, out, encrypt_len + padding_len, sig);
	}
}

/******************************************************************************
 * @brief Get keys from packet
 * @param pkt Packet data
 * @retval Void
 ******************************************************************************/
void get_packet_keys(WsCommon::Packet* pkt, uint8_t *key, uint8_t *iv)
{
	get_ws1001_keys(pkt->pl, pkt->pl_len, NULL, 0, key, iv);
}

/******************************************************************************
 * @brief Get keys from ack
 * @param pkt Packet data
 * @retval Void
 ******************************************************************************/
void get_ack_keys(WsCommon::Packet* pkt, WsCommon::Packet* ack_pkt, uint8_t *key, uint8_t *iv)
{
	get_ws1001_keys(pkt->pl, pkt->pl_len, ack_pkt->pl, ack_pkt->pl_len, key, iv);
}

/******************************************************************************
 * @brief Generate signature for ack
 * @param pkt Packet data
 * @retval Void
 ******************************************************************************/
void gen_ack_sig(WsCommon::Packet* pkt, WsCommon::Packet* ack_pkt, uint8_t *sig, uint16_t sig_len)
{
	uint8_t key[16] = {};
	uint8_t iv[16] = {};

	get_ack_keys(pkt, ack_pkt, key, iv);	
	generate_signature(ack_pkt, key, iv, sig, sig_len);
}

/******************************************************************************
 * @brief Verify packet signature
 * @param pkt Packet data
 * @retval Void
 ******************************************************************************/
WsCommon::RetRes packet_verify(WsCommon::Packet* pkt)
{
	// Has to be at least 1 char + signature
	if (pkt->pl_len < Ws1001::SIGNATURE_LEN + 1)
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}

	uint8_t key[16] = {};
	uint8_t iv[16] = {};

	get_packet_keys(pkt, key, iv);

	//
	// Calculated signature
	//
	uint8_t sig_calculated[Ws1001::SIGNATURE_LEN] = {};
	WsCommon::Packet bare_packet = {.pl = pkt->pl, .pl_len = pkt->pl_len - Ws1001::SIGNATURE_LEN};
	generate_signature(&bare_packet, key, iv, sig_calculated, sizeof(sig_calculated));

	const uint8_t *sig_received = (pkt->pl + pkt->pl_len) - Ws1001::SIGNATURE_LEN;

#if WXM_ENV_DEBUG
	printf("[CRYPTO_DIAG] Recv sig: ");
	for(int i=0; i<16; i++) printf("%02X", sig_received[i]);
	printf("\n[CRYPTO_DIAG] Calc sig: ");
	for(int i=0; i<16; i++) printf("%02X", sig_calculated[i]);
	printf("\n");
#endif

	if(memcmp(sig_received, sig_calculated, Ws1001::SIGNATURE_LEN) == 0)
	{
		return WsCommon::RetRes::OK;
	}

	// Version 0x09 wind-zeroing workaround retry loop
	uint8_t fw_ver = pkt->pl[1];
	if (fw_ver == 0x09)
	{
		uint16_t wind_speed_raw = pkt->pl[17];
		wind_speed_raw = ((pkt->pl[15] & 0x08) << 5) | wind_speed_raw;
		uint16_t wind_gust_raw = pkt->pl[18];
		wind_gust_raw = ((pkt->pl[15] & 0x04) << 6) | wind_gust_raw;

		if (wind_speed_raw != 0 || wind_gust_raw != 0)
		{
			// FALLBACK A: Zero out wind fields, update CRC & checksum, verify
			uint8_t temp_pl[256];
			memcpy(temp_pl, pkt->pl, pkt->pl_len);
			temp_pl[17] = 0;
			temp_pl[18] = 0;
			temp_pl[15] &= ~0x0C;

			uint8_t bare_len = temp_pl[0];
			temp_pl[bare_len - 2] = calc_crc(temp_pl, bare_len - 2, 0x31, 0);
			temp_pl[bare_len - 1] = calc_checksum(temp_pl, bare_len - 1);

			uint8_t key_fallback[16] = {};
			uint8_t iv_fallback[16] = {};
			WsCommon::Packet fallback_pkt = {.pl = temp_pl, .pl_len = pkt->pl_len};
			get_packet_keys(&fallback_pkt, key_fallback, iv_fallback);

			uint8_t sig_fallback[Ws1001::SIGNATURE_LEN] = {};
			WsCommon::Packet fallback_bare = {.pl = temp_pl, .pl_len = bare_len};
			generate_signature(&fallback_bare, key_fallback, iv_fallback, sig_fallback, sizeof(sig_fallback));

			if (memcmp(sig_received, sig_fallback, Ws1001::SIGNATURE_LEN) == 0)
			{
				ESP_LOGW("WS_HELPERS", "[CRYPTO_WORKAROUND] Beta/prototype station v1.0.9: verified packet using Fallback A (updated CRC/chk)");
				return WsCommon::RetRes::OK;
			}

			// FALLBACK B: Zero out wind fields, keep original CRC & checksum, verify
			memcpy(temp_pl, pkt->pl, pkt->pl_len);
			temp_pl[17] = 0;
			temp_pl[18] = 0;
			temp_pl[15] &= ~0x0C;

			// Retain original CRC and checksum
			get_packet_keys(&fallback_pkt, key_fallback, iv_fallback);
			generate_signature(&fallback_bare, key_fallback, iv_fallback, sig_fallback, sizeof(sig_fallback));

			if (memcmp(sig_received, sig_fallback, Ws1001::SIGNATURE_LEN) == 0)
			{
				ESP_LOGW("WS_HELPERS", "[CRYPTO_WORKAROUND] Beta/prototype station v1.0.9: verified packet using Fallback B (original CRC/chk)");
				return WsCommon::RetRes::OK;
			}
		}
	}

	return WsCommon::RetRes::ERROR_BAD_SIGNATURE;
}

/******************************************************************************
 * @brief Convert NMEA location string to decimal lat/lon
 * @param loc Location lat or lon string
 * @param dir Direction/quadrant (N|E|S|W)
 * @retval Success - Converted position
 * @retval Failure - NAN
 ******************************************************************************/
float location_str_deg_to_dec(char *loc, char dir)
{
	dir = (char)toupper(dir);
	if (dir != 'N' && dir != 'E' && dir != 'S' && dir != 'W')
		return NAN;

	char *pos = strchr(loc, '.');
	if (pos == NULL)
	{
		return NAN;
	}

	int int_len = (int)(pos - loc);

	int deg = 0;
	float min = 0;

	const char *format = nullptr;

	if (int_len == 4)
	{
		format = "%2d%f";
	}
	else if (int_len == 5)
	{
		format = "%3d%f";
	}
	else
	{
		// Unknown format
		return NAN;
	}
	if (sscanf(loc, format, &deg, &min) != 2)
	{
		return NAN;
	}

	float out = (float)deg + (min / 60.0f);
	if (dir == 'W' || dir == 'S')
		out = -out;

	return out;
}

/******************************************************************************
 * @brief Get gps parameters from packet
 * @param pkt Packet data
 * @param gps_lat Latitude
 * @param gps_lon Longitude
 * @param gps_valid Data has fix
 * @param gps_ts Timestamp produced by GPS
 * @param gps_speed_kn Speed in knots
 * @param gps_dir Latitude
 * @retval ERROR_NOT_SUPPORTED if packet type is not WEATHER_GPS_SIGNED
 * @retval ERROR_OUT_OF_BOUNDS if packet length is incorrect
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes get_gps_parameters(WsCommon::Packet* pkt, 
								float* gps_lat,
								float* gps_lon,
								bool* gps_valid,
								uint32_t* gps_ts,
								float* gps_speed_kn,
								float* gps_dir)
{

	//
	// Parse NMEA data
	//
	//
	// buff = "GNRMC,101554.000,A,3758.84244,N,02342.65014,E,0.00,0.00,140922,,,A";

	if(get_packet_type(pkt) != Ws1001::PacketType::WEATHER_GPS_SIGNED)
	{
		return WsCommon::RetRes::ERROR_NOT_SUPPORTED;
	}

	// UTC time and date parts
	int time_hr = 0, time_min = 0, time_sec = 0, time_ms = 0,
		date_d = 0, date_m = 0, date_y = 0;

	// Position status A|V
	char pos_status = 0;

	// Lat/lon directions N|S / W|E
	char lat_dir = 0, lon_dir = 0;

	// Lat/lon position strings
	// Lat: DDmm.mm (fixed len, decimal part 7 digits)
	// Long DDDmm.mm (fixed len, decimal part 7 digits)
	char str_lat[12] = "", str_lon[12] = "";

	char *gnss_str = (char *)get_packet_gps_buff_pt(pkt);

	float speed_kn;
	float dir;

	// const char NMEA_STR[] = "GNRMC,%2d%2d%2d.%3d,%c,%[^,],%c,%[^,],%c,%f,%f,%2d%2d%2d";

	if(get_packet_gps_buff_len(pkt) < sizeof(NMEA_STR))
	{
		return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
	}

	int items = sscanf(gnss_str, NMEA_STR,
					   &time_hr, &time_min, &time_sec, &time_ms, &pos_status,
					   str_lat, &lat_dir, str_lon, &lon_dir,
					   &speed_kn, &dir, &date_d, &date_m, &date_y);
	// printf("NMEA string: %.*s\n", get_packet_gps_buff_len(pkt), gnss_str);

	if(gps_dir != NULL)
	{
		*gps_dir = dir;
	}

	if(gps_speed_kn != NULL)
	{
		*gps_speed_kn = speed_kn;
	}

	// All items must exist
	// Note: Is there a scenario where the weather station returns a RMC string with
	// fields missing?
	if (items != 14)
	{
		// printf("Not all 14 expected items found in GPS payload.\n");
		// return RET_ERROR;
	}


	if(gps_valid != NULL)
	{
		// A: Data valid, V: Data invalid
		if (pos_status != 'A')
		{
			// printf("GNSS marked data as invalid.\n");
			*gps_valid = false;
			// return RET_ERROR;
		} else {
			*gps_valid = true;
		}
	}
	
	// Serial.printf("Items: %d\n", items);
	// Serial.printf("Time: %d:%d:%d.%d\n", time_hr, time_min, time_sec, time_ms);
	// Serial.printf("Date: %d/%d/%d\n", date_d, date_m, date_y);
	// Serial.printf("Pos status: %c\n", pos_status);
	// Serial.printf("Lat: %s %c\n", str_lat, lat_dir);
	// Serial.printf("Lon: %s %c\n", str_lon, lon_dir);
	// Serial.printf("Speed/Dir: %f %3.1f\n", speed_kn, speed_dir);

	if(gps_lat != NULL)
	{
		*gps_lat = location_str_deg_to_dec(str_lat, lat_dir);
	}
	
	if(gps_lon != NULL)
	{
		*gps_lon = location_str_deg_to_dec(str_lon, lon_dir);
	}

	if (*gps_lat == NAN || *gps_lon == NAN)
	{
		// printf("Invalid lat/lon received.\n");
		// return RET_ERROR;
	}

	//
	// Build fix tstamp
	//
	tm t = {};
	t.tm_hour = time_hr;
	t.tm_min = time_min;
	t.tm_sec = time_sec;
	t.tm_mday = date_d;
	t.tm_mon = date_m - 1;
	t.tm_year = date_y + 100;

	if(gps_ts != NULL)
	{
		time_t time_ts = mktime(&t);

		if (time_ts == -1)
		{
			*gps_ts = (uint32_t) 0;
		}
		else
		{
			*gps_ts = (uint32_t) time_ts;
		}
	}

	return WsCommon::RetRes::OK;
}

}
