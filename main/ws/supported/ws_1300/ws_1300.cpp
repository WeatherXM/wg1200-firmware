#include "include/ws_1300.h"
#include "include/ws_1300_helpers.h"
#include "include/ws_1300_decoders.h"
#include "ws/include/ws_handler.h"
#include "ws/include/meshtastic_regions.h"
#include "ws/include/adaptive_publish.h"
#include "settings.h"
#include <time.h>

namespace Ws1300
{

static const char TAG[] = "Ws1300";
static uint32_t _last_resp_time_sync_send_ts = 0;

WsCommon::RetRes packet_handler(WsBase::GwCtx* gw_ctx, WsCommon::Packet* pkt_received, 
	WsCommon::Packet* pkt_response, WsBase::StaCtx* sta_ctx)
{
	WsCommon::RetRes ret = Ws1300Helpers::packet_validate(pkt_received);
	if(ret != WsCommon::RetRes::OK) return ret;

	ByteBuffer::StaHeader* sta_header = (ByteBuffer::StaHeader*) pkt_received->pl;
	sta_ctx->packet_index = ByteBuffer::be_to_le(sta_header->packet_id);
	sta_ctx->packet_index_valid = true;
	Ws1300::StaPacketType pkt_type = Ws1300Helpers::get_packet_type(pkt_received);
	bool signature_required = pkt_type == Ws1300::StaPacketType::SENSORS ||
		pkt_type == Ws1300::StaPacketType::GNSS;
	if(signature_required && !sta_header->aux_byte_2.contains_sig)
	{
		// The signature-present bit is itself covered by the signature. Do not
		// let clearing it turn a signed telemetry packet into an accepted
		// unsigned packet.
		return WsCommon::RetRes::ERROR_BAD_SIGNATURE;
	}
	u32 station_id;
	Ws1300Decoders::decode_meta_id(pkt_received, &station_id);

	if(sta_header->aux_byte_2.contains_sig)
	{
		bool pubkey_valid = false;
		for(size_t i = 0; i < sizeof(gw_ctx->paired_info.pubkey); i++)
		{
			if(gw_ctx->paired_info.pubkey[i] != 0 && gw_ctx->paired_info.pubkey[i] != 0xFF)
			{
				pubkey_valid = true;
				break;
			}
		}

		if(pubkey_valid)
		{
			if(Ws1300Helpers::packet_verify(pkt_received, gw_ctx->paired_info.pubkey, sizeof(gw_ctx->paired_info.pubkey)) != WsCommon::RetRes::OK)
			{
				return WsCommon::RetRes::ERROR_BAD_SIGNATURE;
			}
		}
		else
		{
			ESP_LOGW(TAG, "WS1300: No valid pubkey in pairing profile for 0x%08X; skipping ECDSA verification",
				(unsigned int)station_id);
		}
	}

	if(pkt_type != Ws1300::StaPacketType::ADV)
	{
		if(gw_ctx->is_paired)
		{
			if(gw_ctx->paired_info.id != station_id)
			{
				return WsCommon::RetRes::ERROR_BAD_ID;
			}
		}
		else
		{
			return WsCommon::RetRes::ERROR_NOT_SUPPORTED;
		}
	}

	sta_ctx->pair_data.id = station_id;

	if(pkt_type == Ws1300::StaPacketType::ADV)
	{
		ByteBuffer::StaAdv* sta_adv = (ByteBuffer::StaAdv*) pkt_received->pl;
		if(gw_ctx->is_paired)
		{
			if(station_id != gw_ctx->paired_info.id)
			{
				return WsCommon::RetRes::ERROR_BAD_ID;
			}

			if(memcmp(gw_ctx->paired_info.sn, sta_adv->sn, sizeof(gw_ctx->paired_info.sn)) != 0)
			{
				// The 4-byte station_id matched but the full serial differs.
				// Adopt the ADV's serial if ours was never set (all 0xFF or all
				// 0x00, e.g. cloud pairing provided just the id) or was a
				// synthesized channel-hint serial (AA55AA55) - otherwise this
				// is a foreign station whose id collides with ours and it must
				// not overwrite the stored credentials.
				bool sn_all_ff = true;
				bool sn_all_zero = true;
				for(size_t i = 0; i < sizeof(gw_ctx->paired_info.sn); i++)
				{
					if(gw_ctx->paired_info.sn[i] != 0xFF) sn_all_ff = false;
					if(gw_ctx->paired_info.sn[i] != 0x00) sn_all_zero = false;
				}
				bool sn_synthesized = (sizeof(gw_ctx->paired_info.sn) >= 8 &&
					gw_ctx->paired_info.sn[4] == 0xAA &&
					gw_ctx->paired_info.sn[5] == 0x55 &&
					gw_ctx->paired_info.sn[6] == 0xAA &&
					gw_ctx->paired_info.sn[7] == 0x55);
				bool sn_was_blank = sn_all_ff || sn_all_zero || sn_synthesized;
				if(!sn_was_blank)
				{
					return WsCommon::RetRes::ERROR_BAD_ID;
				}
				ESP_LOGW(TAG, "Paired station SN was placeholder/blank. Adopting authentic SN from ADV.");
			}

			// A changed public key with a matching full serial means the
			// station was reset/reflashed: permit updating the stored key.
			if(memcmp(gw_ctx->paired_info.pubkey, sta_adv->pub_key, sizeof(gw_ctx->paired_info.pubkey)) != 0)
			{
				ESP_LOGW(TAG, "Paired station public key changed (station reset/reflash). Allowing seamless re-pairing.");
			}
		}
		else
		{
			// Not supporting dynamic pair. Pair data should be setted by TB
			return WsCommon::RetRes::ERROR_NOT_SUPPORTED;
		}

		Ws1300::ByteBuffer::RespPair* resp_pair = (Ws1300::ByteBuffer::RespPair*) pkt_response->pl;
		resp_pair->header.type = Ws1300::RespPacketType::PAIR;
		resp_pair->header.packet_id = sta_header->packet_id;
		resp_pair->header.station_id = sta_header->station_id;
		memcpy(resp_pair->sn, gw_ctx->gw_sn, sizeof(resp_pair->sn));
		Ws1300Helpers::gen_resp_hmac(pkt_response->pl, sizeof(Ws1300::ByteBuffer::RespPair) - sizeof(resp_pair->hmac),
									resp_pair->hmac, sizeof(resp_pair->hmac));
		pkt_response->pl_len = sizeof(Ws1300::ByteBuffer::RespPair);
		Ws1300Helpers::init_sta_status(sta_header);
		memcpy(sta_ctx->pair_data.pubkey, sta_adv->pub_key, sizeof(sta_ctx->pair_data.pubkey));
		memcpy(sta_ctx->pair_data.sn, sta_adv->sn, sizeof(sta_ctx->pair_data.sn));
		sta_ctx->pair_permitted = true; // Flag that pair state update is allowed
		return WsCommon::RetRes::OK;
	}
	else if(pkt_type == Ws1300::StaPacketType::BOOT)
	{
		sta_ctx->reboot_detected = true;
		Ws1300Helpers::init_sta_status(sta_header);
		if(gw_ctx->time_is_valid)
		{
			Ws1300Helpers::build_time_sync_resp(pkt_response, sta_header, gw_ctx->cur_ts);
		}
		return WsCommon::RetRes::OK;
	}
	bool needs_time_sync = false;
	if(pkt_type == Ws1300::StaPacketType::SENSORS)
	{
		u32 ts_val;
		if(Ws1300Decoders::decode_weather_ts(pkt_received, &ts_val) != WsCommon::RetRes::OK &&
					gw_ctx->time_is_valid)
		{
			// Station has no GPS-derived timestamp (e.g. no GNSS fix yet).
			// Don't return immediately: a pending CMD (reboot/GPS request/TX
			// power/TX interval) must still take priority, or a station stuck
			// without a fix would never receive any command at all. Time sync
			// is sent below only if nothing else needs to go out this cycle.
			needs_time_sync = true;
		}
	}
	else if(pkt_type == Ws1300::StaPacketType::GNSS)
	{
		// TODO: Check signature, but maybe on decode because it would take time.
		sta_ctx->gps_pkt_received = true;
	}
	else return WsCommon::RetRes::ERROR_NOT_SUPPORTED;

	Ws1300Helpers::calc_sta_status(pkt_received, sta_ctx);

	uint8_t sta_fw_ver = sta_header->fw_ver;
	WsHandler::g_ws1300_station_fw_ver = sta_fw_ver;

	bool reboot_requested = WsHandler::g_ws1300_reboot_requested;
	bool req_gps = gw_ctx->gps_needed && sta_ctx->gps_req_permitted;

	// Manual TX power override (web diagnostics) falls back to the gateway's
	// automatic ack-loss-based adaptive level (sta_ctx->req_tx_power) when 0
	uint16_t target_tx_power = (WsHandler::g_ws1300_tx_power_override > 0)
		? (uint16_t)WsHandler::g_ws1300_tx_power_override : sta_ctx->req_tx_power;
	bool change_tx_power = target_tx_power != sta_header->aux_byte_1.tx_power;

	// Manual TX interval override (web diagnostics) falls back to the fixed
	// BROADCAST_INTERVAL_SEC when 0
	uint16_t target_tx_int_sec = (WsHandler::g_ws1300_tx_interval_override_sec > 0)
		? (uint16_t)WsHandler::g_ws1300_tx_interval_override_sec : BROADCAST_INTERVAL_SEC;
	bool change_tx_int = Ws1300Helpers::get_tx_int_sec(sta_header) != target_tx_int_sec;
	bool change_meshtastic = WsHandler::g_ws1300_meshtastic_changed;
	bool change_meshcore = WsHandler::g_ws1300_meshcore_changed;
	bool change_policy = WsHandler::g_ws1300_weather_policy_changed;

	Ws1300::ByteBuffer::RespHeader* resp_header = (Ws1300::ByteBuffer::RespHeader*) pkt_response->pl;
	resp_header->packet_id = sta_header->packet_id;
	resp_header->station_id = sta_header->station_id;

	if(reboot_requested || req_gps || change_tx_int || change_tx_power || change_meshtastic || change_meshcore || change_policy)
	{
		if (sta_fw_ver >= 16)
		{
			static uint8_t s_cmd_seq = 0;
			s_cmd_seq++;

			Ws1300::ByteBuffer::RespCmdV3Header* v3_hdr = (Ws1300::ByteBuffer::RespCmdV3Header*) pkt_response->pl;
			memset(v3_hdr, 0x00, sizeof(*v3_hdr));
			v3_hdr->header.packet_id = sta_header->packet_id;
			v3_hdr->header.station_id = sta_header->station_id;
			v3_hdr->header.type = Ws1300::RespPacketType::CMD;
			v3_hdr->format_ver = Ws1300::ByteBuffer::RESP_CMD_V3_FORMAT_VER;
			v3_hdr->cmd_seq = s_cmd_seq;

			uint8_t* tlv_buf = &pkt_response->pl[sizeof(Ws1300::ByteBuffer::RespCmdV3Header)];
			uint8_t tlv_len = 0;
			// Limit TLV payload to 90 bytes so entire response (116 bytes max with headers/HMAC)
			// transmits in ~180ms @ SF7/BW125, fitting safely within the station's 300ms RX window.
			constexpr size_t MAX_TLV_BUF_LEN = 90;

			if(reboot_requested)
			{
				if (tlv_len + 3 <= MAX_TLV_BUF_LEN)
				{
					tlv_buf[tlv_len++] = (uint8_t)Ws1300::ByteBuffer::CmdTlvTag::FLAGS;
					tlv_buf[tlv_len++] = 1;
					tlv_buf[tlv_len++] = 0x02; // reboot flag
					WsHandler::g_ws1300_reboot_requested = false; // one-shot
					ESP_LOGW(TAG, "Sending remote reboot command (V3 TLV) to station 0x%08lX", (unsigned long)station_id);
				}
			}
			else
			{
				uint8_t flags = 0;
				if (req_gps) flags |= 0x01;
				if (flags != 0 && tlv_len + 3 <= MAX_TLV_BUF_LEN)
				{
					tlv_buf[tlv_len++] = (uint8_t)Ws1300::ByteBuffer::CmdTlvTag::FLAGS;
					tlv_buf[tlv_len++] = 1;
					tlv_buf[tlv_len++] = flags;
				}

				if (change_tx_int && tlv_len + 3 <= MAX_TLV_BUF_LEN)
				{
					tlv_buf[tlv_len++] = (uint8_t)Ws1300::ByteBuffer::CmdTlvTag::TX_INTERVAL;
					tlv_buf[tlv_len++] = 1;
					tlv_buf[tlv_len++] = (uint8_t)(target_tx_int_sec / BROADCAST_INTERVAL_SEC);
				}

				if (change_tx_power && tlv_len + 3 <= MAX_TLV_BUF_LEN)
				{
					tlv_buf[tlv_len++] = (uint8_t)Ws1300::ByteBuffer::CmdTlvTag::TX_POWER;
					tlv_buf[tlv_len++] = 1;
					tlv_buf[tlv_len++] = (uint8_t)target_tx_power;
				}

				if (change_meshtastic)
				{
					if (tlv_len + 4 <= MAX_TLV_BUF_LEN)
					{
						tlv_buf[tlv_len++] = (uint8_t)Ws1300::ByteBuffer::CmdTlvTag::MESHTASTIC_CONFIG;
						tlv_buf[tlv_len++] = 2;
						tlv_buf[tlv_len++] = WsHandler::g_ws1300_meshtastic_interval_min;
						tlv_buf[tlv_len++] = Settings::get_mesh_env_telemetry_hop_limit() ? Settings::get_mesh_env_telemetry_hop_limit() : 3;
					}

					if (tlv_len + 4 <= MAX_TLV_BUF_LEN)
					{
						tlv_buf[tlv_len++] = (uint8_t)Ws1300::ByteBuffer::CmdTlvTag::MESHTASTIC_REGION_PRESET;
						tlv_buf[tlv_len++] = 2;
						tlv_buf[tlv_len++] = (uint8_t)Meshtastic::get_region_code_for_freq_region(Settings::get_freq_region());
						tlv_buf[tlv_len++] = Settings::get_mesh_modem_preset();
					}

					if (Settings::get_mesh_modem_preset() == 255 &&
					    tlv_len + 11 <= MAX_TLV_BUF_LEN)
					{
						tlv_buf[tlv_len++] = (uint8_t)Ws1300::ByteBuffer::CmdTlvTag::MESHTASTIC_CUSTOM_RF;
						tlv_buf[tlv_len++] = 9;
						Ws1300::ByteBuffer::BigEndian<u32> be_fq = Ws1300::ByteBuffer::le_to_be(Settings::get_mesh_freq_hz());
						memcpy(&tlv_buf[tlv_len], be_fq.bytes, 4);
						tlv_len += 4;
						Ws1300::ByteBuffer::BigEndian<u16> be_bw = Ws1300::ByteBuffer::le_to_be(Settings::get_mesh_bw_khz());
						memcpy(&tlv_buf[tlv_len], be_bw.bytes, 2);
						tlv_len += 2;
						tlv_buf[tlv_len++] = Settings::get_mesh_sf();
						tlv_buf[tlv_len++] = Settings::get_mesh_cr();
						tlv_buf[tlv_len++] = (uint8_t)Settings::get_mesh_tx_power_dbm();
					}
				}

				if (change_meshcore)
				{
					if (tlv_len + 4 <= MAX_TLV_BUF_LEN)
					{
						tlv_buf[tlv_len++] = (uint8_t)Ws1300::ByteBuffer::CmdTlvTag::MESHCORE_CONFIG;
						tlv_buf[tlv_len++] = 2;
						tlv_buf[tlv_len++] = WsHandler::g_ws1300_meshcore_interval_min;
						tlv_buf[tlv_len++] = Settings::get_meshcore_gw_profile_idx();
					}

					if (Settings::get_meshcore_gw_profile_idx() == 255 &&
					    tlv_len + 11 <= MAX_TLV_BUF_LEN)
					{
						tlv_buf[tlv_len++] = (uint8_t)Ws1300::ByteBuffer::CmdTlvTag::MESHCORE_CUSTOM_RF;
						tlv_buf[tlv_len++] = 9;
						Ws1300::ByteBuffer::BigEndian<u32> be_fq = Ws1300::ByteBuffer::le_to_be(Settings::get_meshcore_gw_freq_hz());
						memcpy(&tlv_buf[tlv_len], be_fq.bytes, 4);
						tlv_len += 4;
						Ws1300::ByteBuffer::BigEndian<u16> be_bw = Ws1300::ByteBuffer::le_to_be(Settings::get_meshcore_gw_bw_khz());
						memcpy(&tlv_buf[tlv_len], be_bw.bytes, 2);
						tlv_len += 2;
						tlv_buf[tlv_len++] = Settings::get_meshcore_gw_sf();
						tlv_buf[tlv_len++] = Settings::get_meshcore_gw_cr();
						tlv_buf[tlv_len++] = (uint8_t)Settings::get_meshcore_gw_tx_power_dbm();
					}
				}

				if (change_policy)
				{
					if (tlv_len + 2 + sizeof(AdaptivePublish::WeatherPolicyWire) <= MAX_TLV_BUF_LEN)
					{
						tlv_buf[tlv_len++] = (uint8_t)Ws1300::ByteBuffer::CmdTlvTag::WEATHER_POLICY;
						tlv_buf[tlv_len++] = (uint8_t)sizeof(AdaptivePublish::WeatherPolicyWire);
						AdaptivePublish::WeatherChangePolicy policy = Settings::get_weather_policy();
						AdaptivePublish::WeatherPolicyWire wire;
						AdaptivePublish::encode_wire(policy, &wire);
						memcpy(&tlv_buf[tlv_len], &wire, sizeof(wire));
						tlv_len += sizeof(wire);
					}
				}

				ESP_LOGW(TAG, "Sending remote command (V3 TLV, %u bytes TLV) to station 0x%08lX (int=%u, pwr=%u, mesh_chg=%d, core_chg=%d, pol_chg=%d)",
				         (unsigned int)tlv_len,
				         (unsigned long)station_id,
				         (unsigned int)target_tx_int_sec,
				         (unsigned int)target_tx_power,
				         (int)change_meshtastic,
				         (int)change_meshcore,
				         (int)change_policy);
			}

			if (tlv_len > 0)
			{
				v3_hdr->tlv_len = tlv_len;
				WsHandler::g_ws1300_meshtastic_changed = false;
				WsHandler::g_ws1300_meshcore_changed = false;
				WsHandler::g_ws1300_weather_policy_changed = false;

				size_t payload_len_before_hmac = sizeof(Ws1300::ByteBuffer::RespCmdV3Header) + tlv_len;
				uint8_t* hmac_target = &pkt_response->pl[payload_len_before_hmac];
				Ws1300Helpers::gen_resp_hmac(pkt_response->pl, payload_len_before_hmac,
											hmac_target, Ws1300::HMAC_LEN);
				pkt_response->pl_len = payload_len_before_hmac + Ws1300::HMAC_LEN;
			}
			else
			{
				Ws1300Helpers::build_ack_resp(pkt_response, sta_header);
			}
		}
		else if (sta_fw_ver >= 15)
		{
			Ws1300::ByteBuffer::RespCmdV2* resp_cmd = (Ws1300::ByteBuffer::RespCmdV2*) pkt_response->pl;
			memset(resp_cmd, 0x00, sizeof(*resp_cmd));
			resp_cmd->header.packet_id = sta_header->packet_id;
			resp_cmd->header.station_id = sta_header->station_id;
			resp_cmd->header.type = Ws1300::RespPacketType::CMD;

			if(reboot_requested)
			{
				resp_cmd->reboot = true;
				resp_cmd->acquire_gps = false;
				resp_cmd->enable_logs = false;
				resp_cmd->tx_interval = 0;
				resp_cmd->tx_pwr = 0;
				resp_cmd->meshtastic_interval_min = 0;
				WsHandler::g_ws1300_reboot_requested = false; // one-shot
				ESP_LOGW(TAG, "Sending remote reboot command (V2) to station 0x%08lX", (unsigned long)station_id);
			}
			else
			{
				resp_cmd->reboot = false;
				resp_cmd->acquire_gps = req_gps;
				resp_cmd->enable_logs = false;
				resp_cmd->tx_interval = change_tx_int ? (target_tx_int_sec / BROADCAST_INTERVAL_SEC) : 0;
				resp_cmd->tx_pwr = change_tx_power ? target_tx_power : 0;
				resp_cmd->meshtastic_interval_min = WsHandler::g_ws1300_meshtastic_interval_min;
				ESP_LOGW(TAG, "Sending remote command (V2) to station 0x%08lX: meshtastic_interval=%u min",
				         (unsigned long)station_id, (unsigned int)resp_cmd->meshtastic_interval_min);
			}
			WsHandler::g_ws1300_meshtastic_changed = false;
			pkt_response->pl_len = sizeof(Ws1300::ByteBuffer::RespCmdV2);
			Ws1300Helpers::gen_resp_hmac(pkt_response->pl, sizeof(Ws1300::ByteBuffer::RespCmdV2) - sizeof(resp_cmd->hmac),
										resp_cmd->hmac, sizeof(resp_cmd->hmac));
		}
		else
		{
			Ws1300::ByteBuffer::RespCmdV1* resp_cmd = (Ws1300::ByteBuffer::RespCmdV1*) pkt_response->pl;
			memset(resp_cmd, 0x00, sizeof(*resp_cmd));
			resp_cmd->header.packet_id = sta_header->packet_id;
			resp_cmd->header.station_id = sta_header->station_id;
			resp_cmd->header.type = Ws1300::RespPacketType::CMD;

			if(reboot_requested)
			{
				resp_cmd->reboot = true;
				resp_cmd->acquire_gps = false;
				resp_cmd->enable_logs = false;
				resp_cmd->tx_interval = 0;
				resp_cmd->tx_pwr = 0;
				WsHandler::g_ws1300_reboot_requested = false; // one-shot
				ESP_LOGW(TAG, "Sending remote reboot command (V1) to station 0x%08lX", (unsigned long)station_id);
			}
			else
			{
				resp_cmd->reboot = false;
				resp_cmd->acquire_gps = req_gps;
				resp_cmd->enable_logs = false;
				resp_cmd->tx_interval = change_tx_int ? (target_tx_int_sec / BROADCAST_INTERVAL_SEC) : 0;
				resp_cmd->tx_pwr = change_tx_power ? target_tx_power : 0;
				ESP_LOGW(TAG, "Sending remote command (V1) to station 0x%08lX", (unsigned long)station_id);
			}
			WsHandler::g_ws1300_meshtastic_changed = false;
			pkt_response->pl_len = sizeof(Ws1300::ByteBuffer::RespCmdV1);
			Ws1300Helpers::gen_resp_hmac(pkt_response->pl, sizeof(Ws1300::ByteBuffer::RespCmdV1) - sizeof(resp_cmd->hmac),
										resp_cmd->hmac, sizeof(resp_cmd->hmac));
		}
	}
	else if(needs_time_sync ||
		(gw_ctx->time_is_valid && gw_ctx->cur_ts - _last_resp_time_sync_send_ts >= TIME_SYNC_INTERVAL))
	{
		Ws1300Helpers::build_time_sync_resp(pkt_response, sta_header, gw_ctx->cur_ts);
		_last_resp_time_sync_send_ts = time(NULL);
	}
	else
	{
		Ws1300Helpers::build_ack_resp(pkt_response, sta_header);
	}
	return WsCommon::RetRes::OK;
}

WsCommon::RetRes packet_decoder(WsCommon::Packet* pkt_received,	WsFields::FieldPack* field_pack, WsGps::GpsData* gps_data)
{
	Ws1300::StaPacketType packet_type = Ws1300Helpers::get_packet_type(pkt_received);
	if(packet_type == Ws1300::StaPacketType::NONE) return WsCommon::RetRes::ERROR;

	for(int i = 0; i < Ws1300::FIELDS_COUNT; i++)
	{
		WsFields::Field common_field;
		WsFields::field_init(&common_field, Ws1300::FIELDS[i]);
		// Decode header data for every type of packet
		if(Ws1300::FIELDS[i]->cat == WsFields::FieldCatType::META)
		{
			switch(Ws1300::FIELDS[i]->type)
			{
				case WsFields::FieldType::META_ID:
					common_field.decode_status =  Ws1300Decoders::decode_meta_id(pkt_received, &common_field.value.val_u32);
					break;
				case WsFields::FieldType::META_PCK_COUNT:
					common_field.decode_status =  Ws1300Decoders::decode_meta_pkt_count(pkt_received, &common_field.value.val_u32);
					break;
				case WsFields::FieldType::META_TX_POWER:
					common_field.decode_status =  Ws1300Decoders::decode_meta_tx_power(pkt_received, &common_field.value.val_u16);
					break;
				case WsFields::FieldType::META_BAT_MV:
					common_field.decode_status =  Ws1300Decoders::decode_meta_bat_mv(pkt_received, &common_field.value.val_u16);
					break;
				case WsFields::FieldType::META_BAT_LOW:
					common_field.decode_status =  Ws1300Decoders::decode_meta_bat_low(pkt_received, &common_field.value.val_bool);
					break;
				case WsFields::FieldType::META_RSSI:
					common_field.decode_status =  Ws1300Decoders::decode_meta_rssi(pkt_received, &common_field.value.val_int);
					break;
				case WsFields::FieldType::META_FW_VER:
					common_field.decode_status =  Ws1300Decoders::decode_meta_fw_ver(pkt_received, &common_field.value.val_u8);
					break;
				case WsFields::FieldType::META_TX_INTERVAL:
					common_field.decode_status =  Ws1300Decoders::decode_meta_tx_interval(pkt_received, &common_field.value.val_u16);
					break;
					// TODO: Irradiance is missing and UI is expecting it in field pack
				default:
					break;
			}
		}
		else if(packet_type == StaPacketType::SENSORS)
		{
			switch(Ws1300::FIELDS[i]->type)
			{
				case WsFields::FieldType::WEATHER_TEMP:
					common_field.decode_status =  Ws1300Decoders::decode_weather_temp(pkt_received, &common_field.value.val_float);
					break;
				case WsFields::FieldType::WEATHER_HUM:
					common_field.decode_status =  Ws1300Decoders::decode_weather_hum(pkt_received, &common_field.value.val_float);
					break;
				case WsFields::FieldType::WEATHER_WIND_DIR:
					common_field.decode_status =  Ws1300Decoders::decode_weather_wind_dir(pkt_received, &common_field.value.val_u16);
					break;
				case WsFields::FieldType::WEATHER_WIND_SPEED:
					common_field.decode_status =  Ws1300Decoders::decode_weather_wind_speed(pkt_received, &common_field.value.val_float);
					break;
				case WsFields::FieldType::WEATHER_WIND_GUST:
					common_field.decode_status =  Ws1300Decoders::decode_weather_wind_gust(pkt_received, &common_field.value.val_float);
					break;
				case WsFields::FieldType::WEATHER_ILLUMINANCE:
					common_field.decode_status =  Ws1300Decoders::decode_weather_illuminance(pkt_received, &common_field.value.val_u32);
					break;
				case WsFields::FieldType::WEATHER_UV_INDEX:
					common_field.decode_status =  Ws1300Decoders::decode_weather_uv_index(pkt_received, &common_field.value.val_u8);
					break;
				case WsFields::FieldType::WEATHER_PRECIP_CML:
					common_field.decode_status =  Ws1300Decoders::decode_weather_precip_cml(pkt_received, &common_field.value.val_float);
					break;
				case WsFields::FieldType::WEATHER_PRECIP_RATE:
					common_field.decode_status =  Ws1300Decoders::decode_weather_precip_rate(pkt_received, &common_field.value.val_float);
					break;
				case WsFields::FieldType::WEATHER_TS:
					common_field.decode_status =  Ws1300Decoders::decode_weather_ts(pkt_received, &common_field.value.val_u32);
					break;
				case WsFields::FieldType::WEATHER_PRESSURE:
					common_field.decode_status =  Ws1300Decoders::decode_weather_pressure(pkt_received, &common_field.value.val_float);
					break;
				case WsFields::FieldType::WEATHER_SOLAR_RAD:
					common_field.decode_status =  Ws1300Decoders::decode_weather_solar_rad(pkt_received, &common_field.value.val_float);
					break;
				default:
					break;
			}
		}

		if(common_field.decode_status != WsCommon::RetRes::NONE)
		{
			WsCommon::RetRes ret = WsFields::add_field_to_pack(field_pack, &common_field);
			if(ret != WsCommon::RetRes::OK)
			{
				ESP_LOGE(TAG, "add_field_to_pack failed. Field: %s", common_field.info->name);
				WsCommon::print_ret_val(ret);
			}
		}		
	}

	if(packet_type == StaPacketType::GNSS)
	{
		WsFields::Field gps_field_lat;
		WsFields::field_init(&gps_field_lat, &WsGps::FIELD_INFO_GPS_LAT);
		float lat = 0.0f;
		gps_field_lat.decode_status = Ws1300Decoders::decode_gnss_lat(pkt_received, &lat);
		WsFields::set_field_val(&gps_field_lat, lat);
		WsFields::add_field_to_pack(&gps_data->field_pack, &gps_field_lat);

		WsFields::Field gps_field_lon;
		WsFields::field_init(&gps_field_lon, &WsGps::FIELD_INFO_GPS_LON);
		float lon = 0.0f;
		gps_field_lon.decode_status = Ws1300Decoders::decode_gnss_lon(pkt_received, &lon);
		WsFields::set_field_val(&gps_field_lon, lon);
		WsFields::add_field_to_pack(&gps_data->field_pack, &gps_field_lon);

		WsFields::Field gps_field_ts;
		WsFields::field_init(&gps_field_ts, &WsGps::FIELD_INFO_GPS_TS);
		u32 ts = 0;
		gps_field_ts.decode_status = Ws1300Decoders::decode_gnss_ts(pkt_received, &ts);
		WsFields::set_field_val(&gps_field_ts, ts);
		WsFields::add_field_to_pack(&gps_data->field_pack, &gps_field_ts);

		WsFields::Field gps_field_sats;
		WsFields::field_init(&gps_field_sats, &WsGps::FIELD_INFO_GPS_SATS);
		u8 sats = 0;
		gps_field_sats.decode_status = Ws1300Decoders::decode_gnss_sats(pkt_received, &sats);
		WsFields::set_field_val(&gps_field_sats, sats);
		WsFields::add_field_to_pack(&gps_data->field_pack, &gps_field_sats);

		if(Ws1300Decoders::decode_gnss_fix_ok(pkt_received, &gps_data->valid) != WsCommon::RetRes::OK)
		{
			gps_data->valid = false;
		}

		gps_data->decoding_status = WsGps::DecodingStatus::DECODED;
		gps_data->decoding_type = WsGps::DecodingType::DIRECT;
	}

	return WsCommon::RetRes::OK;
}

WsCommon::RetRes freq_resolver(u32 region_base_freq_hz, const WsBase::PairData* pair_data, u32* resolved_freq_hz)
{
	if(!pair_data || !resolved_freq_hz)
	{
		return WsCommon::RetRes::ERROR_BAD_PARAMS;
	}

	// Always the station's own channel: 8 channels (0 - 7) based on last byte
	// of sn. Sniff-sweep tuning is handled by WsHandler::tune_sniff_slot() and
	// must never leak in here, or a paired gateway would be tuned away from
	// its own station.
	uint8_t channel_num = pair_data->sn[sizeof(pair_data->sn) - 1] % 8;
	*resolved_freq_hz = region_base_freq_hz
						+ FREQ_BASE_OFFSET_KHZ * 1000
						+ channel_num * FREQ_CHANNEL_GAP_KHZ * 1000;
	ESP_LOGI(TAG, "freq_resolver: channel_num=%d, freq=%u Hz, last_sn_byte=0x%02X",
			 channel_num, (unsigned int)*resolved_freq_hz,
			 pair_data->sn[sizeof(pair_data->sn) - 1]);
	return WsCommon::RetRes::OK;
}

}
