#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS

#include "include/ws_1300_decoders.h"
#include "include/ws_1300_helpers.h"
#include "include/ws_1300_keys.h"
#include "mbedtls/md.h"
#include "mbedtls/ecp.h"
#include "mbedtls/private/ecdsa.h"
#include "mbedtls/private/sha256.h"
#include "env.h"
#include <stddef.h>

namespace Ws1300Helpers
{

static const char TAG[] = "Ws1300Helpers";

const int POWER_LEVEL_MAX = 22;
const int POWER_LEVEL_MIN = 1;

static uint8_t _current_power_level = 0;
static uint16_t _last_packet_index = 0;
static uint16_t _last_power_opt_index = 0;
static bool _power_dcr_period_long_en = false;

void build_time_sync_resp(WsCommon::Packet* resp_pkt, Ws1300::ByteBuffer::StaHeader* sta_header, u32 time)
{
	Ws1300::ByteBuffer::RespTimeSync* resp_time_sync = (Ws1300::ByteBuffer::RespTimeSync*) resp_pkt->pl;
	resp_time_sync->header.type = Ws1300::RespPacketType::TIME_SYNC;
	resp_time_sync->header.station_id = sta_header->station_id;
	resp_time_sync->header.packet_id = sta_header->packet_id;
	resp_time_sync->ts = Ws1300::ByteBuffer::le_to_be(time);
	gen_resp_hmac(resp_pkt->pl, sizeof(Ws1300::ByteBuffer::RespTimeSync) - sizeof(resp_time_sync->hmac),
									resp_time_sync->hmac, sizeof(resp_time_sync->hmac));
	resp_pkt->pl_len = sizeof(Ws1300::ByteBuffer::RespTimeSync);
}

void build_ack_resp(WsCommon::Packet* resp_pkt, Ws1300::ByteBuffer::StaHeader* sta_header)
{
	Ws1300::ByteBuffer::RespAck* resp_ack = (Ws1300::ByteBuffer::RespAck*) resp_pkt->pl;
	resp_ack->header.type = Ws1300::RespPacketType::ACK;
	resp_ack->header.station_id = sta_header->station_id;
	resp_ack->header.packet_id = sta_header->packet_id;
	resp_pkt->pl_len = sizeof(Ws1300::ByteBuffer::RespAck);
}

Ws1300::StaPacketType get_packet_type(WsCommon::Packet* pkt)
{
	Ws1300::StaPacketType packet_type = Ws1300::StaPacketType::NONE;

	if(pkt->pl_len < sizeof(Ws1300::ByteBuffer::StaHeader)) return packet_type;
	Ws1300::ByteBuffer::StaHeader* header = (Ws1300::ByteBuffer::StaHeader*) pkt->pl;
	return header->packet_type;
}

WsCommon::RetRes gen_resp_hmac(u8* data, int data_len, u8* hmac_buff, int hmac_buff_len)
{
	if(hmac_buff_len < sizeof(Ws1300::HMAC_LEN)) return WsCommon::RetRes::ERROR_BAD_PARAMS;
	if(data_len < sizeof(Ws1300::ByteBuffer::RespHeader)) return WsCommon::RetRes::ERROR_BAD_PARAMS;
	
	u8 calced_hmac[32];
	if(hmac_buff_len > sizeof(calced_hmac)) return WsCommon::RetRes::ERROR_BAD_PARAMS;

	u8 hmac_key[WS1300_HMAC_KEY_LEN];

	if(!get_ws1300_keys((Ws1300::ByteBuffer::RespHeader*) data, Credentials::get_serial(), hmac_key, WS1300_HMAC_KEY_LEN)) return WsCommon::RetRes::ERROR;

	const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	int ret = mbedtls_md_hmac(md_info, hmac_key, sizeof(hmac_key), data, data_len, calced_hmac);

	if(ret == 0)
	{
		memcpy(hmac_buff, calced_hmac, hmac_buff_len);
	}

	return ret == 0 ? WsCommon::RetRes::OK : WsCommon::RetRes::ERROR;
}

WsCommon::RetRes packet_validate(WsCommon::Packet* pkt)
{
	Ws1300::StaPacketType packet_type = Ws1300::StaPacketType::NONE;
	
	Ws1300::ByteBuffer::StaHeader* header = (Ws1300::ByteBuffer::StaHeader*) pkt->pl;
	if(header->model_id != Ws1300::MODEL_ID)
	{
		return WsCommon::RetRes::ERROR_NOT_SUPPORTED;
	}

	packet_type = get_packet_type(pkt);
	
	switch(header->packet_type)
	{
		case Ws1300::StaPacketType::ADV:
			if(pkt->pl_len != sizeof(Ws1300::ByteBuffer::StaAdv)) return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
			break;
		case Ws1300::StaPacketType::BOOT:
			if(pkt->pl_len != sizeof(Ws1300::ByteBuffer::StaBoot)) return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
			break;
		case Ws1300::StaPacketType::GNSS:
			if(pkt->pl_len != sizeof(Ws1300::ByteBuffer::StaGnss)) return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
			break;
		case Ws1300::StaPacketType::SENSORS:
			if(pkt->pl_len != sizeof(Ws1300::ByteBuffer::StaSensors)) return WsCommon::RetRes::ERROR_OUT_OF_BOUNDS;
			break;
		default:
			return WsCommon::RetRes::ERROR_NOT_SUPPORTED;
	}

	return WsCommon::RetRes::OK;
}

WsCommon::RetRes packet_verify(WsCommon::Packet* pkt, u8* pubkey, size_t pubkey_len)
{
	if(pkt == NULL || pkt->pl == NULL || pubkey == NULL ||
		pubkey_len != Ws1300::PUB_KEY_UNC_LEN)
	{
		return WsCommon::RetRes::ERROR_BAD_PARAMS;
	}

	size_t signed_len = 0;
	const uint8_t *signature = NULL;
	Ws1300::StaPacketType packet_type = get_packet_type(pkt);
	if(packet_type == Ws1300::StaPacketType::SENSORS &&
		pkt->pl_len == sizeof(Ws1300::ByteBuffer::StaSensors))
	{
		signed_len = offsetof(Ws1300::ByteBuffer::StaSensors, sig);
		signature = reinterpret_cast<const Ws1300::ByteBuffer::StaSensors*>(pkt->pl)->sig;
	}
	else if(packet_type == Ws1300::StaPacketType::GNSS &&
		pkt->pl_len == sizeof(Ws1300::ByteBuffer::StaGnss))
	{
		signed_len = offsetof(Ws1300::ByteBuffer::StaGnss, sig);
		signature = reinterpret_cast<const Ws1300::ByteBuffer::StaGnss*>(pkt->pl)->sig;
	}
	else
	{
		// ADV and BOOT do not carry a signature field. Reject a forged
		// contains_sig flag rather than interpreting packet data as a signature.
		return WsCommon::RetRes::ERROR_BAD_SIGNATURE;
	}

	uint8_t hash[32] = {};
	if(mbedtls_sha256(pkt->pl, signed_len, hash, 0) != 0)
	{
		return WsCommon::RetRes::ERROR;
	}

	// Wire format is SEC P-256: public key X || Y and signature r || s,
	// with every 32-byte integer encoded big-endian. The signature covers
	// every packet byte preceding the 64-byte signature field.
	uint8_t uncompressed_pubkey[1 + Ws1300::PUB_KEY_UNC_LEN] = {0x04};
	memcpy(&uncompressed_pubkey[1], pubkey, pubkey_len);

	mbedtls_ecp_group group;
	mbedtls_ecp_point point;
	mbedtls_mpi r;
	mbedtls_mpi s;
	mbedtls_ecp_group_init(&group);
	mbedtls_ecp_point_init(&point);
	mbedtls_mpi_init(&r);
	mbedtls_mpi_init(&s);

	int ret = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
	if(ret == 0)
	{
		ret = mbedtls_ecp_point_read_binary(&group, &point,
			uncompressed_pubkey, sizeof(uncompressed_pubkey));
	}
	if(ret == 0)
	{
		ret = mbedtls_ecp_check_pubkey(&group, &point);
	}
	if(ret == 0)
	{
		ret = mbedtls_mpi_read_binary(&r, signature, Ws1300::SIGNATURE_LEN / 2);
	}
	if(ret == 0)
	{
		ret = mbedtls_mpi_read_binary(&s,
			&signature[Ws1300::SIGNATURE_LEN / 2], Ws1300::SIGNATURE_LEN / 2);
	}
	if(ret == 0)
	{
		ret = mbedtls_ecdsa_verify(&group, hash, sizeof(hash), &point, &r, &s);
	}

	mbedtls_mpi_free(&s);
	mbedtls_mpi_free(&r);
	mbedtls_ecp_point_free(&point);
	mbedtls_ecp_group_free(&group);

	if(ret != 0)
	{
		ESP_LOGW(TAG, "WS1300 ECDSA verification failed: -0x%04X", (unsigned int)-ret);
		return WsCommon::RetRes::ERROR_BAD_SIGNATURE;
	}

	return WsCommon::RetRes::OK;
}

u16 get_tx_int_sec(Ws1300::ByteBuffer::StaHeader* sta_header)
{
	return ((u16) sta_header->tx_int) * Ws1300::BROADCAST_INTERVAL_SEC;
}

void init_sta_status(Ws1300::ByteBuffer::StaHeader* sta_header)
{
	_last_packet_index = 0;
	_current_power_level = sta_header->aux_byte_1.tx_power;
	_last_power_opt_index = Ws1300::ByteBuffer::be_to_le(sta_header->packet_id);
	_power_dcr_period_long_en = false;
}

void calc_sta_status(WsCommon::Packet* pkt, WsBase::StaCtx* sta_ctx)
{
	// Algorithm targetting to 1 packet lost every ~3 days
	const uint16_t POWER_DECREASE_PERIOD_SHORT = 90; // 90 packet count, ~30min for 20s interval
	const uint16_t POWER_DECREASE_PERIOD_LONG = 4320; // 4320 packet count, ~1d for 20s interval

	Ws1300::ByteBuffer::StaHeader* header = (Ws1300::ByteBuffer::StaHeader*) pkt->pl;
	u32 packet_id;
	Ws1300Decoders::decode_meta_pkt_count(pkt, &packet_id);

	if(_current_power_level != 0 && header->aux_byte_1.tx_power != _current_power_level)
	{
		sta_ctx->ack_loss_detected = true;
		if(header->aux_byte_1.tx_power > _current_power_level)
		{
			// The station raised its own TX power after missing our responses
			// (or our last power CMD never arrived). Adopt its level instead of
			// commanding it back down - fighting the station's self-recovery
			// keeps a marginal link broken. The periodic decrease resumes from
			// a fresh window, so the sweet-spot search continues from here.
			_current_power_level = header->aux_byte_1.tx_power;
			_last_power_opt_index = packet_id;
		}
	}

	sta_ctx->gps_req_permitted = true;

	if(header->aux_byte_2.acquiring_gps)
	{
		sta_ctx->gps_req_permitted = false;
	}

	if(header->packet_type == Ws1300::StaPacketType::GNSS)
	{
		sta_ctx->gps_req_permitted = false;
	}

	_current_power_level = _current_power_level == 0 ? header->aux_byte_1.tx_power : _current_power_level;
	_last_power_opt_index = _last_power_opt_index == 0 ? packet_id : _last_power_opt_index;

	// Handle duplicate packet without altering power adaptation or underflowing lost counter
	if(_last_packet_index != 0 && packet_id == _last_packet_index)
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
	else if(packet_id > _last_packet_index)
	{
		lost_packets = packet_id - 1 - _last_packet_index;
	}
	else // packet_id < _last_packet_index
	{
		// Check 16-bit sequence roll-over (e.g. 65535 -> 0 after ~15 days at 20s interval)
		uint16_t rollover_delta = (uint16_t)(packet_id - _last_packet_index);
		if(rollover_delta < 100)
		{
			lost_packets = (rollover_delta > 1) ? (rollover_delta - 1) : 0;
		}
		else
		{
			// Station reboot
			init_sta_status(header);
			sta_ctx->reboot_detected = true;
			lost_packets = 0;
		}
	}

	sta_ctx->lost_packets = lost_packets;

	if(lost_packets > 0 && !_power_dcr_period_long_en)
	{
		_power_dcr_period_long_en = true;
	}

	uint16_t power_decrease_period = _power_dcr_period_long_en ? POWER_DECREASE_PERIOD_LONG : POWER_DECREASE_PERIOD_SHORT;

	// Decrease power if no packets lost during POWER_DECREASE_PERIOD_LONG/POWER_DECREASE_PERIOD_SHORT
	// Increase power if any packet is lost during POWER_DECREASE_PERIOD_LONG/POWER_DECREASE_PERIOD_SHORT
	if(lost_packets != 0)
	{
		if(_current_power_level < POWER_LEVEL_MAX)
		{
			_current_power_level++;
		}		
		_last_power_opt_index = packet_id;
	}
	else if((uint16_t)(packet_id - _last_power_opt_index) > power_decrease_period)
	{
		if(_current_power_level > POWER_LEVEL_MIN)
		{
			_current_power_level--;
		}		
		_last_power_opt_index = packet_id;

		// If waited POWER_DECREASE_PERIOD_LONG, continue with POWER_DECREASE_PERIOD_SHORT
		_power_dcr_period_long_en = false;
	}
	// ESP_LOGI(TAG, "current_power_level: %d", current_power_level);

	_last_packet_index = packet_id;

	sta_ctx->req_tx_power = _current_power_level;
}

}
