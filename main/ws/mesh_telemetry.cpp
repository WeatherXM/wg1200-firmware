/******************************************************************************
 * Meshtastic environmental telemetry beacon.
 *
 * Wire format, protobuf field numbers and crypto live in mesh_frame.h, which
 * is platform-independent and covered by tools/mesh_frame_test. This file is
 * the firmware glue: identity, region mapping, scheduling and the radio call.
 ******************************************************************************/

// mbedTLS 4.x (IDF 6.x) moved the AES function declarations behind private
// headers; mbedtls/aes.h alone only gives you the context type. Must be defined
// before any mbedtls include. Mirrors ws_1001_helpers.cpp and submit.cpp.
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS

#include "ws/include/mesh_telemetry.h"
#include "ws/include/mesh_frame.h"
#include "ws/include/meshcore_frame.h"
#include "ws/include/gateway_snapshot.h"
#include "ws/include/adaptive_publish.h"
#include "ws/include/ws_data.h"
#include "ws/include/ws_gps.h"
#include "ws/include/ws_fields.h"
#include "ws/include/ws_handler.h"
#include "weather_logger.h"
#include "credentials.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

#if __has_include("mbedtls/private/aes.h")
#include "mbedtls/private/aes.h"
#else
#include "mbedtls/aes.h"
#endif
#if __has_include("mbedtls/md.h")
#include "mbedtls/md.h"
#endif

namespace MeshTelemetry
{
	static const char TAG[] = "MeshTelem";

	/* The hop limit is NVS-configurable (default 3). Higher values are useful
	 * for range testing but multiply each broadcast's relay airtime, so the
	 * scheduler deliberately emits only one due frame per safe radio window. */

	/* LONG_FAST modem preset. Values are Semtech radio.c indices. */
	static const uint32_t MESH_LORA_SF       = 11; /* SF11                  */
	static const uint32_t MESH_LORA_BW_IDX   = 1;  /* 1 = 250 kHz           */
	static const uint8_t  MESH_LORA_CR       = 1;  /* 1 = 4/5               */
	static const uint16_t MESH_LORA_PREAMBLE = 16; /* preambleLengthDefault */

	/* ------------------------------------------------------------------ */
	/* Module state                                                        */
	/* ------------------------------------------------------------------ */

	static bool     g_inited          = false;
	static uint32_t g_node_num        = 0;
	static char     g_node_id_str[12] = {0};
	static char     g_long_name[40]   = {0};
	static char     g_short_name[10]  = {0};
	static uint8_t  g_channel_hash    = 0;

	static uint32_t g_last_send_ts           = 0; /* environment; seconds since boot */
	static uint32_t g_last_nodeinfo_ts       = 0;
	static uint32_t g_last_device_metrics_ts = 0;
	static uint32_t g_last_position_ts       = 0;
	static uint32_t g_sent_count             = 0;
	static volatile bool g_send_requested    = false;

	static uint32_t g_last_meshcore_send_ts     = 0;
	static uint32_t g_meshcore_sent_count       = 0;
	static volatile bool g_meshcore_send_requested = false;

	static uint32_t g_mesh_failed_backoff_until     = 0;
	static uint32_t g_meshcore_failed_backoff_until = 0;

	static AdaptivePublish::PublisherState g_meshtastic_pub_state = {};
	static AdaptivePublish::PublisherState g_meshcore_pub_state   = {};

	static uint32_t now_sec()
	{
		uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
		return s ? s : 1;
	}

	static bool schedule_due(uint32_t last_send_ts, uint32_t now,
							 uint32_t base_interval_sec, uint8_t multiplier)
	{
		if (last_send_ts == 0) return true;
		uint32_t interval_sec = base_interval_sec * (uint32_t)multiplier;
		return (uint32_t)(now - last_send_ts) >= interval_sec;
	}

	/* ------------------------------------------------------------------ */
	/* AES backend                                                         */
	/* ------------------------------------------------------------------ */

	static void aes_ecb(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
	{
		mbedtls_aes_context aes;
		mbedtls_aes_init(&aes);
		if (mbedtls_aes_setkey_enc(&aes, key, 128) == 0) {
			if (mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, in, out) != 0) {
				memset(out, 0, 16);
			}
		} else {
			memset(out, 0, 16);
		}
		mbedtls_aes_free(&aes);
	}

	/* ------------------------------------------------------------------ */
	/* Identity                                                            */
	/* ------------------------------------------------------------------ */

	/** Refresh the Meshtastic / MeshCore identity from live settings.
	 * @return true when either advertised name changed. */
	bool update_names()
	{
		char next_long_name[sizeof(g_long_name)] = {0};
		char next_short_name[sizeof(g_short_name)] = {0};

		/*
		// WeatherXM given name (leave for future use but disabled):
		const char *dev_name = Settings::get_device_name();
		if (dev_name && dev_name[0] != '\0') {
			snprintf(next_long_name, sizeof(next_long_name), "%.23s", dev_name);
		}
		*/

		bool custom_enabled = Settings::get_mesh_custom_name_enabled();
		const char *c_name = Settings::get_mesh_custom_name();
		const char *c_short = Settings::get_mesh_custom_short_name();

		if (custom_enabled && c_name && c_name[0] != '\0') {
			snprintf(next_long_name, sizeof(next_long_name), "%.23s", c_name);
			if (c_short && c_short[0] != '\0') {
				snprintf(next_short_name, sizeof(next_short_name), "%.4s", c_short);
			} else {
				snprintf(next_short_name, sizeof(next_short_name), "G%03X",
						 (unsigned)(g_node_num & 0x0FFF));
			}
		} else {
			// Default agreed convention: WeatherXM WG1200 XXXX & GXXX
			snprintf(next_long_name, sizeof(next_long_name), "WeatherXM WG1200 %04X",
					 (unsigned)(g_node_num & 0xFFFF));
			snprintf(next_short_name, sizeof(next_short_name), "G%03X",
					 (unsigned)(g_node_num & 0x0FFF));
		}

		bool changed = strcmp(g_long_name, next_long_name) != 0 ||
			strcmp(g_short_name, next_short_name) != 0;
		if (changed) {
			snprintf(g_long_name, sizeof(g_long_name), "%s", next_long_name);
			snprintf(g_short_name, sizeof(g_short_name), "%s", next_short_name);
		}
		return changed;
	}

	static void init_identity()
	{
		uint8_t mac[6] = {0};
		if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
			ESP_LOGW(TAG, "esp_read_mac failed, node number will not be stable across boots");
			g_node_num = esp_random();
		} else {
			/* Meshtastic derives the node number from the last 4 MAC bytes. */
			g_node_num = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
						 ((uint32_t)mac[4] << 8)  | (uint32_t)mac[5];
		}

		/* 0 and the broadcast address are reserved. */
		if (g_node_num == 0 || g_node_num == MeshFrame::BROADCAST_ADDR) {
			g_node_num = 0x00575831; /* "WX1" */
		}

		snprintf(g_node_id_str, sizeof(g_node_id_str), "!%08x", (unsigned)g_node_num);
		update_names();

		g_channel_hash = MeshFrame::default_channel_hash();
	}

	uint32_t get_node_num()       { return g_node_num; }
	const char *get_node_id_str() { return g_node_id_str; }

	/* ------------------------------------------------------------------ */
	/* Region -> Meshtastic frequency                                      */
	/* ------------------------------------------------------------------ */

	uint32_t get_mesh_freq_hz()
	{
		if (Settings::get_mesh_modem_preset() == 255)
		{
			uint32_t custom_freq = Settings::get_mesh_freq_hz();
			if (custom_freq > 0)
			{
				return custom_freq;
			}
		}

		switch (Settings::get_freq_region())
		{
			/* EU_868: freqStart 869.4 MHz, a single 250 kHz slot (869.525 MHz). */
			case WS_FREQ_REGION_EU868:
				return 869525000u;

			/* US_915: 902-928 MHz, 104 slots of 250 kHz; LongFast slot 19 -> 906.875 MHz. */
			case WS_FREQ_REGION_US915:
				return 906875000u;

			/* AS_923 (TW/TH/SG): 920-925 MHz, 20 slots of 250 kHz; LongFast slot 15 -> 923.875 MHz. */
			case WS_FREQ_REGION_AS923:
				return 923875000u;

			/* AU_915 (ANZ): 915-928 MHz, 52 slots of 250 kHz; LongFast slot 19 -> 919.875 MHz. */
			case WS_FREQ_REGION_AU915:
				return 919875000u;

			/* IN_865: 865-867 MHz, 8 slots of 250 kHz; LongFast slot 3 -> 865.875 MHz. */
			case WS_FREQ_REGION_IN865:
				return 865875000u;

			default:
				return 0;
		}
	}

	static uint32_t bw_khz_to_idx(uint16_t bw_khz, uint32_t fallback_idx)
	{
		if (bw_khz == 62 || bw_khz == 63) return 3;
		if (bw_khz == 125) return 0;
		if (bw_khz == 250) return 1;
		if (bw_khz == 500) return 2;
		return fallback_idx;
	}

	static uint8_t cr_to_idx(uint8_t cr, uint8_t fallback_cr_idx)
	{
		if (cr >= 5 && cr <= 8) return (cr - 4);
		if (cr >= 1 && cr <= 4) return cr;
		return fallback_cr_idx;
	}

	static void get_meshtastic_preset_rf(uint8_t preset, uint32_t *sf, uint32_t *bw_idx, uint8_t *cr)
	{
		switch (preset) {
			case 0: // LongFast (Default)
				*sf = 11;
				*bw_idx = 1; // 250 kHz
				*cr = 1;     // 4/5
				break;
			case 1: // LongSlow
				*sf = 12;
				*bw_idx = 0; // 125 kHz
				*cr = 4;     // 4/8
				break;
			case 2: // MediumFast
				*sf = 9;
				*bw_idx = 1; // 250 kHz
				*cr = 1;     // 4/5
				break;
			case 3: // MediumSlow
				*sf = 10;
				*bw_idx = 1; // 250 kHz
				*cr = 1;     // 4/5
				break;
			case 4: // ShortFast
				*sf = 7;
				*bw_idx = 1; // 250 kHz
				*cr = 1;     // 4/5
				break;
			case 5: // ShortSlow
				*sf = 8;
				*bw_idx = 1; // 250 kHz
				*cr = 1;     // 4/5
				break;
			case 6: // LongModerate
				*sf = 11;
				*bw_idx = 0; // 125 kHz
				*cr = 4;     // 4/8
				break;
			case 7: // ShortTurbo
				*sf = 7;
				*bw_idx = 2; // 500 kHz
				*cr = 1;     // 4/5
				break;
			default: // Default to LongFast
				*sf = 11;
				*bw_idx = 1; // 250 kHz
				*cr = 1;     // 4/5
				break;
		}
	}

	/* ------------------------------------------------------------------ */
	/* Observation -> EnvironmentMetrics                                   */
	/* ------------------------------------------------------------------ */

	/**
	 * WsFields base units are already SI and match Meshtastic exactly:
	 * degC, %, hPa, lux, degrees, m/s, m/s, mm. No conversion needed.
	 * Settings::get_*_unit() only affects presentation, never the field pack.
	 */
	static bool collect_env(MeshFrame::EnvValues *v)
	{
		WsFields::FieldPack field_pack;
		if (WsData::get_last_packet(&field_pack) != WsCommon::RetRes::OK) {
			ESP_LOGW(TAG, "No station observation available yet");
			return false;
		}

		memset(v, 0, sizeof(*v));

		float f;
		uint16_t u16;
		uint32_t u32;

		f = NAN;
		WsFields::get_field_val_from_pack<float>(&field_pack, WsFields::FieldType::WEATHER_TEMP, NULL, &f);
		if (!isnan(f)) { v->has_temperature = true; v->temperature = f; }

		f = NAN;
		WsFields::get_field_val_from_pack<float>(&field_pack, WsFields::FieldType::WEATHER_HUM, NULL, &f);
		if (!isnan(f)) { v->has_humidity = true; v->humidity = f; }

		f = NAN;
		WsFields::get_field_val_from_pack<float>(&field_pack, WsFields::FieldType::WEATHER_PRESSURE, NULL, &f);
		if (!isnan(f)) { v->has_pressure = true; v->pressure = f; }

		f = NAN;
		WsFields::get_field_val_from_pack<float>(&field_pack, WsFields::FieldType::WEATHER_WIND_SPEED, NULL, &f);
		if (!isnan(f)) { v->has_wind_speed = true; v->wind_speed = f; }

		f = NAN;
		WsFields::get_field_val_from_pack<float>(&field_pack, WsFields::FieldType::WEATHER_WIND_GUST, NULL, &f);
		if (!isnan(f)) { v->has_wind_gust = true; v->wind_gust = f; }

		/* Calculate 1-hour (last 12 five-minute slots) and 24-hour (all slots) accumulated rainfall from WeatherLogger */
		float precip_1h_sum = 0.0f;
		bool precip_1h_found = false;
		float precip_24h_sum = 0.0f;
		bool precip_24h_found = false;

		auto aggs = WeatherLogger::get_aggregates();
		if (!aggs.empty()) {
			size_t total_slots = aggs.size();
			size_t start_1h_idx = (total_slots > 12) ? (total_slots - 12) : 0;

			for (size_t i = 0; i < total_slots; i++) {
				const auto &agg = aggs[i];
				if (agg.valid && !isnan(agg.precip_accum)) {
					precip_24h_sum += agg.precip_accum;
					precip_24h_found = true;

					if (i >= start_1h_idx) {
						precip_1h_sum += agg.precip_accum;
						precip_1h_found = true;
					}
				}
			}
		}

		if (precip_1h_found) {
			v->has_rainfall_1h = true;
			v->rainfall_1h = precip_1h_sum;
		} else {
			f = NAN;
			WsFields::get_field_val_from_pack<float>(&field_pack, WsFields::FieldType::WEATHER_PRECIP_RATE, NULL, &f);
			if (!isnan(f)) { v->has_rainfall_1h = true; v->rainfall_1h = f; }
		}

		if (precip_24h_found) {
			v->has_rainfall_24h = true;
			v->rainfall_24h = precip_24h_sum;
		}

		u16 = 0;
		if (WsFields::get_field_val_from_pack<uint16_t>(&field_pack, WsFields::FieldType::WEATHER_WIND_DIR,
														NULL, &u16) == WsCommon::RetRes::OK) {
			v->has_wind_direction = true;
			v->wind_direction = (uint32_t)u16;
		}

		float solar_rad = NAN;
		WsFields::get_field_val_from_pack<float>(&field_pack, WsFields::FieldType::WEATHER_SOLAR_RAD, NULL, &solar_rad);

		u32 = 0;
		if (WsFields::get_field_val_from_pack<uint32_t>(&field_pack, WsFields::FieldType::WEATHER_ILLUMINANCE,
														NULL, &u32) == WsCommon::RetRes::OK) {
			/* Check for sensor error sentinels (e.g. 0xFFFFFF or 0x00FFFF from unhandled night/dark state) */
			if (u32 >= 1500000u || u32 == 0x00FFFFFFu || u32 == 0x007FFFFFu || u32 == 0x0000FFFFu) {
				v->has_lux = true;
				v->lux = 0.0f;
			} else {
				/* WS1300 illuminance is encoded in 0.1 Lux units (lux * 10) */
				float calculated_lux = (float)u32 / 10.0f;
				if (calculated_lux > 200000.0f) {
					v->has_lux = true;
					v->lux = 0.0f;
				} else {
					v->has_lux = true;
					v->lux = calculated_lux;
				}
			}
		} else if (!isnan(solar_rad)) {
			/* Fall back to solar radiation conversion (1 W/m^2 ≈ 126.7 Lux) */
			v->has_lux = true;
			v->lux = (solar_rad <= 0.0f) ? 0.0f : (solar_rad * 126.7f);
		}

		/* If solar radiation is zero or negligible (nighttime), force Lux to 0 */
		if (v->has_lux && !isnan(solar_rad) && solar_rad <= 0.01f) {
			v->lux = 0.0f;
		}

		/* Solar radiation (W/m2) is deliberately not sent: the only candidate
		 * field, EnvironmentMetrics.radiation, is ionizing radiation in uSv/h
		 * and would be rendered as a Geiger reading. */

		return v->has_temperature || v->has_humidity || v->has_pressure ||
			   v->has_wind_speed  || v->has_wind_gust || v->has_rainfall_1h ||
			   v->has_rainfall_24h || v->has_wind_direction || v->has_lux;
	}

	/* ------------------------------------------------------------------ */
	/* Observation -> DeviceMetrics                                       */
	/* ------------------------------------------------------------------ */

	static bool collect_device_metrics(MeshFrame::DeviceMetricsValues *dev)
	{
		WsFields::FieldPack field_pack;
		if (WsData::get_last_packet(&field_pack) != WsCommon::RetRes::OK) {
			return false;
		}

		memset(dev, 0, sizeof(*dev));

		uint16_t bat_mv = 0;
		bool bat_low = false;

		if (WsFields::get_field_val_from_pack<uint16_t>(&field_pack, WsFields::FieldType::META_BAT_MV, NULL, &bat_mv) == WsCommon::RetRes::OK && bat_mv > 0) {
			dev->has_voltage = true;
			dev->voltage = (float)bat_mv / 1000.0f;
		}

		WsFields::get_field_val_from_pack<bool>(&field_pack, WsFields::FieldType::META_BAT_LOW, NULL, &bat_low);

		dev->has_battery_level = true;
		if (bat_low) {
			dev->battery_level = 15;
		} else {
			dev->battery_level = 101; /* Powered / Good state */
		}

		dev->has_uptime = true;
		dev->uptime_seconds = now_sec();

		return dev->has_voltage || dev->has_battery_level || dev->has_uptime;
	}

	/* ------------------------------------------------------------------ */
	/* Transmit                                                            */
	/* ------------------------------------------------------------------ */

	static bool transmit(uint32_t portnum, const uint8_t *payload, size_t payload_len,
						 const WsHandler::RadioLease *lease = nullptr)
	{
		uint32_t freq_hz = get_mesh_freq_hz();
		if (freq_hz == 0) {
			ESP_LOGW(TAG, "No Meshtastic frequency mapping for region %d, not transmitting",
					 (int)Settings::get_freq_region());
			return false;
		}

		/* The packet id is both the mesh-wide dedup key and the AES counter
		 * seed, so it must be unique per frame. */
		uint32_t packet_id = esp_random();
		if (packet_id == 0) packet_id = 1;

		uint8_t hop_limit = get_hop_limit();
		uint8_t frame[MeshFrame::MAX_FRAME_LEN];
		size_t frame_len = MeshFrame::build_frame(
			MeshFrame::BROADCAST_ADDR, g_node_num, packet_id,
			g_channel_hash, hop_limit,
			portnum, payload, payload_len,
			MeshFrame::DEFAULT_PSK, aes_ecb,
			frame, sizeof(frame));

		if (frame_len == 0) {
			ESP_LOGE(TAG, "Frame build failed (portnum %u, payload %u bytes)",
					 (unsigned)portnum, (unsigned)payload_len);
			return false;
		}

		uint32_t sf = 11;
		uint32_t bw_idx = 1;
		uint8_t cr = 1;
		uint16_t preamble = 16;

		uint8_t preset = Settings::get_mesh_modem_preset();
		if (preset == 255) {
			uint8_t custom_sf = Settings::get_mesh_sf();
			if (custom_sf >= 7 && custom_sf <= 12) {
				sf = custom_sf;
			}
			uint16_t custom_bw = Settings::get_mesh_bw_khz();
			if (custom_bw > 0) {
				bw_idx = bw_khz_to_idx(custom_bw, bw_idx);
			}
			uint8_t custom_cr = Settings::get_mesh_cr();
			if (custom_cr > 0) {
				cr = cr_to_idx(custom_cr, cr);
			}
		} else {
			get_meshtastic_preset_rf(preset, &sf, &bw_idx, &cr);
		}

		ESP_LOGI(TAG, "TX portnum %u, %u bytes, %.3f MHz, ch hash 0x%02X, from %s, id 0x%08X",
				 (unsigned)portnum, (unsigned)frame_len, freq_hz / 1e6,
				 g_channel_hash, g_node_id_str, (unsigned)packet_id);

		return WsHandler::sx126x_tx_meshtastic(frame, (uint8_t)frame_len, freq_hz,
											   sf, bw_idx, cr, preamble, lease);
	}

	static bool send_telemetry_packet(const MeshFrame::EnvValues *env_values,
									 const WsHandler::RadioLease *lease = nullptr)
	{
		/* Buffers live on the packet handler task stack (8 KB, see
		 * PACKET_HANDLER_TASK_STACK_SIZE). Eight EnvironmentMetrics fields
		 * encode to well under 64 bytes, so keep these tight. */
		uint8_t env_buf[96];
		size_t env_len = MeshFrame::build_env_metrics(env_values, env_buf, sizeof(env_buf));
		if (env_len == 0) {
			ESP_LOGW(TAG, "EnvironmentMetrics encode failed");
			return false;
		}

		uint8_t telem_buf[128];
		time_t now_wall = time(NULL);
		uint32_t telem_time = (now_wall > 1600000000u) ? (uint32_t)now_wall : now_sec();
		size_t telem_len = MeshFrame::build_telemetry(telem_time, env_buf, env_len,
													  telem_buf, sizeof(telem_buf));
		if (telem_len == 0) {
			ESP_LOGW(TAG, "Telemetry encode failed");
			return false;
		}

		ESP_LOGI(TAG, "Env (time=%u): T=%.1fC RH=%.0f%% P=%.1fhPa wind=%.1fm/s gust=%.1fm/s dir=%udeg rain1h=%.2fmm rain24h=%.2fmm lux=%.0f",
				 (unsigned)telem_time,
				 env_values->temperature, env_values->humidity, env_values->pressure,
				 env_values->wind_speed, env_values->wind_gust,
				 (unsigned)env_values->wind_direction, env_values->rainfall_1h, env_values->rainfall_24h, env_values->lux);

		if (!transmit(MeshFrame::PORTNUM_TELEMETRY_APP, telem_buf, telem_len, lease)) return false;

		g_last_send_ts = now_sec();
		g_sent_count++;
		return true;
	}

	static bool send_device_metrics_packet(const MeshFrame::DeviceMetricsValues *dev_values,
										   const WsHandler::RadioLease *lease = nullptr)
	{
		uint8_t dev_buf[64];
		size_t dev_len = MeshFrame::build_device_metrics(dev_values, dev_buf, sizeof(dev_buf));
		if (dev_len == 0) return false;

		uint8_t telem_buf[96];
		time_t now_wall = time(NULL);
		uint32_t telem_time = (now_wall > 1600000000u) ? (uint32_t)now_wall : now_sec();
		size_t telem_len = MeshFrame::build_telemetry_device_metrics(telem_time, dev_buf, dev_len,
																	  telem_buf, sizeof(telem_buf));
		if (telem_len == 0) return false;

		ESP_LOGI(TAG, "DeviceMetrics (time=%u): batt=%u%% volt=%.2fV uptime=%us",
				 (unsigned)telem_time, (unsigned)dev_values->battery_level, dev_values->voltage, (unsigned)dev_values->uptime_seconds);

		if (!transmit(MeshFrame::PORTNUM_TELEMETRY_APP, telem_buf, telem_len, lease)) return false;

		g_last_device_metrics_ts = now_sec();
		return true;
	}

	static bool send_nodeinfo(const WsHandler::RadioLease *lease = nullptr)
	{
		update_names();

		uint8_t pubkey_32[32] = {};
		const bool has_pubkey = (Credentials::get_dev_cert_fingerprint(pubkey_32) == RET_OK);

		uint8_t buf[128];
		size_t n = MeshFrame::build_nodeinfo(g_node_id_str, g_long_name, g_short_name,
											 MeshFrame::HW_PRIVATE_HW, MeshFrame::ROLE_SENSOR,
											 has_pubkey ? pubkey_32 : nullptr,
											 has_pubkey ? sizeof(pubkey_32) : 0,
											 buf, sizeof(buf));
		if (n == 0) return false;

		if (transmit(MeshFrame::PORTNUM_NODEINFO_APP, buf, n, lease)) {
			g_last_nodeinfo_ts = now_sec();
			return true;
		}
		return false;
	}

	static bool collect_gps(MeshFrame::PositionValues *pos)
	{
		WsGps::GpsData gps_data = {};
		WsGps::get_last_gps_data(&gps_data);

		if (WsGps::gps_data_expired(&gps_data)) {
			return false;
		}

		float lat = NAN, lon = NAN;
		if (WsFields::get_field_val_from_pack<float>(&gps_data.field_pack, WsFields::FieldType::GPS_LAT, NULL, &lat) != WsCommon::RetRes::OK ||
			WsFields::get_field_val_from_pack<float>(&gps_data.field_pack, WsFields::FieldType::GPS_LON, NULL, &lon) != WsCommon::RetRes::OK) {
			return false;
		}

		if (isnan(lat) || isnan(lon) || lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f) {
			return false;
		}

		memset(pos, 0, sizeof(*pos));
		pos->lat = lat;
		pos->lon = lon;

		uint32_t gps_ts = 0;
		if (WsFields::get_field_val_from_pack<uint32_t>(&gps_data.field_pack, WsFields::FieldType::GPS_TS, NULL, &gps_ts) == WsCommon::RetRes::OK && gps_ts > 0) {
			pos->time = gps_ts;
		} else {
			pos->time = (uint32_t)time(NULL);
		}

		return true;
	}

	static bool send_position(const WsHandler::RadioLease *lease = nullptr)
	{
		MeshFrame::PositionValues pos;
		if (!collect_gps(&pos)) return false;

		uint8_t pos_buf[64];
		size_t pos_len = MeshFrame::build_position(&pos, pos_buf, sizeof(pos_buf));
		if (pos_len == 0) return false;

		ESP_LOGI(TAG, "GPS Position: lat=%.6f lon=%.6f time=%u", pos.lat, pos.lon, (unsigned)pos.time);

		if (!transmit(MeshFrame::PORTNUM_POSITION_APP, pos_buf, pos_len, lease)) return false;

		g_last_position_ts = now_sec();
		return true;
	}

	/* ------------------------------------------------------------------ */
	/* MeshCore helpers                                                    */
	/* ------------------------------------------------------------------ */

	static void meshcore_aes_ecb(const uint8_t *input, uint8_t *output, size_t length, const uint8_t *key_16b, bool encrypt)
	{
		mbedtls_aes_context ctx;
		mbedtls_aes_init(&ctx);
		if (encrypt) {
			mbedtls_aes_setkey_enc(&ctx, key_16b, 128);
			for (size_t i = 0; i < length; i += 16) {
				mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, input + i, output + i);
			}
		} else {
			mbedtls_aes_setkey_dec(&ctx, key_16b, 128);
			for (size_t i = 0; i < length; i += 16) {
				mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, input + i, output + i);
			}
		}
		mbedtls_aes_free(&ctx);
	}

	static void meshcore_hmac_sha256(const uint8_t *data, size_t data_len, const uint8_t *key_16b, uint8_t *out_mac_32b)
	{
		mbedtls_md_context_t ctx;
		mbedtls_md_init(&ctx);
		const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
		if (md_info != nullptr && mbedtls_md_setup(&ctx, md_info, 1) == 0) {
			if (mbedtls_md_hmac_starts(&ctx, key_16b, 16) == 0) {
				mbedtls_md_hmac_update(&ctx, data, data_len);
				mbedtls_md_hmac_finish(&ctx, out_mac_32b);
			}
		}
		mbedtls_md_free(&ctx);
	}

	uint32_t get_meshcore_freq_hz()
	{
		if (Settings::get_meshcore_gw_profile_idx() == 255) {
			uint32_t custom_freq = Settings::get_meshcore_gw_freq_hz();
			if (custom_freq > 0) {
				return custom_freq;
			}
		}

		switch (Settings::get_freq_region()) {
			case WS_FREQ_REGION_EU868:
				return 869618000;
			case WS_FREQ_REGION_US915:
			case WS_FREQ_REGION_AU915:
				return 915000000;
			case WS_FREQ_REGION_AS923:
				return 923000000;
			case WS_FREQ_REGION_IN865:
				return 865000000;
			default:
				return 869618000;
		}
	}

	static void get_meshcore_rf_profile(uint8_t profile_idx, uint32_t *sf, uint32_t *bw_idx, uint8_t *cr, uint16_t *preamble)
	{
		*preamble = 16;
		*cr = 1; // 4/5
		switch (profile_idx) {
			case 1: // Medium
				*sf = 8;
				*bw_idx = 1; // 250 kHz
				break;
			case 2: // Narrow Extreme
				*sf = 11;
				*bw_idx = 0; // 125 kHz
				break;
			case 3: // Fast
				*sf = 7;
				*bw_idx = 1; // 250 kHz
				break;
			case 4: // EU/UK Narrow (MeshCore EU Standard: SF8 / 62.5kHz / CR 4/8)
				*sf = 8;
				*bw_idx = 3; // 62.5 kHz
				*cr = 4;     // CR 4/8
				break;
			case 0: // Long Range (Default)
			default:
				*sf = 10;
				*bw_idx = 1; // 250 kHz
				break;
		}
	}

	static bool collect_meshcore_obs(MeshCoreFrame::MeshCoreObsPayload *payload)
	{
		if (payload == nullptr) return false;
		memset(payload, 0, sizeof(*payload));

		WsFields::FieldPack field_pack;
		if (WsData::get_last_packet(&field_pack) != WsCommon::RetRes::OK) {
			return false;
		}

		payload->station_id = g_node_num;
		payload->timestamp = field_pack.ts ? field_pack.ts : (uint32_t)time(NULL);

		uint16_t valid_mask = 0;
		float f = NAN;
		uint16_t u16 = 0;
		uint32_t u32 = 0;

		f = NAN;
		WsFields::get_field_val_from_pack<float>(&field_pack, WsFields::FieldType::WEATHER_TEMP, NULL, &f);
		if (!isnan(f)) {
			payload->temperature = (int16_t)roundf(f * 10.0f);
			valid_mask |= 0x0001;
		}

		f = NAN;
		WsFields::get_field_val_from_pack<float>(&field_pack, WsFields::FieldType::WEATHER_HUM, NULL, &f);
		if (!isnan(f)) {
			payload->humidity = (uint8_t)roundf(f);
			valid_mask |= 0x0002;
		}

		f = NAN;
		WsFields::get_field_val_from_pack<float>(&field_pack, WsFields::FieldType::WEATHER_PRESSURE, NULL, &f);
		if (!isnan(f)) {
			payload->pressure = (uint16_t)roundf(f * 10.0f);
			valid_mask |= 0x0004;
		}

		f = NAN;
		WsFields::get_field_val_from_pack<float>(&field_pack, WsFields::FieldType::WEATHER_WIND_SPEED, NULL, &f);
		if (!isnan(f)) {
			payload->wind_speed = (uint16_t)roundf(f * 10.0f);
			valid_mask |= 0x0008;
		}

		f = NAN;
		WsFields::get_field_val_from_pack<float>(&field_pack, WsFields::FieldType::WEATHER_WIND_GUST, NULL, &f);
		if (!isnan(f)) {
			payload->wind_gust = (uint16_t)roundf(f * 10.0f);
			valid_mask |= 0x0010;
		}

		u16 = 0;
		if (WsFields::get_field_val_from_pack<uint16_t>(&field_pack, WsFields::FieldType::WEATHER_WIND_DIR, NULL, &u16) == WsCommon::RetRes::OK) {
			payload->wind_dir = u16;
			valid_mask |= 0x0020;
		}

		// Rainfall 1h
		float precip_1h_sum = 0.0f;
		bool precip_1h_found = false;
		auto aggs = WeatherLogger::get_aggregates();
		if (!aggs.empty()) {
			size_t total_slots = aggs.size();
			size_t start_1h_idx = (total_slots > 12) ? (total_slots - 12) : 0;
			for (size_t i = start_1h_idx; i < total_slots; i++) {
				const auto &agg = aggs[i];
				if (agg.valid && !isnan(agg.precip_accum)) {
					precip_1h_sum += agg.precip_accum;
					precip_1h_found = true;
				}
			}
		}
		if (precip_1h_found) {
			payload->precip_cml = (uint16_t)roundf(precip_1h_sum * 10.0f);
			valid_mask |= 0x0040;
		} else {
			f = NAN;
			WsFields::get_field_val_from_pack<float>(&field_pack, WsFields::FieldType::WEATHER_PRECIP_RATE, NULL, &f);
			if (!isnan(f)) {
				payload->precip_cml = (uint16_t)roundf(f * 10.0f);
				valid_mask |= 0x0040;
			}
		}

		f = NAN;
		WsFields::get_field_val_from_pack<float>(&field_pack, WsFields::FieldType::WEATHER_SOLAR_RAD, NULL, &f);
		if (!isnan(f)) {
			payload->solar_rad = (uint16_t)roundf(f * 10.0f);
			valid_mask |= 0x0080;
		}

		u32 = 0;
		if (WsFields::get_field_val_from_pack<uint32_t>(&field_pack, WsFields::FieldType::WEATHER_UV_INDEX, NULL, &u32) == WsCommon::RetRes::OK) {
			payload->uv_index = (uint8_t)u32;
			valid_mask |= 0x0100;
		}

		uint16_t bat_mv = 0;
		if (WsFields::get_field_val_from_pack<uint16_t>(&field_pack, WsFields::FieldType::META_BAT_MV, NULL, &bat_mv) == WsCommon::RetRes::OK && bat_mv > 0) {
			payload->battery_mv = bat_mv;
			valid_mask |= 0x0200;
		}

		payload->valid_mask = valid_mask;
		return (valid_mask != 0);
	}

	static bool send_meshcore_telemetry_packet(const WsHandler::RadioLease *lease = nullptr)
	{
		MeshCoreFrame::MeshCoreObsPayload obs;
		if (!collect_meshcore_obs(&obs)) {
			ESP_LOGW(TAG, "MeshCore: no valid observation available to send");
			return false;
		}

		uint8_t frame[128];
		size_t frame_len = MeshCoreFrame::build_grp_data_frame(
			obs,
			MeshCoreFrame::DEFAULT_PSK,
			MeshCoreFrame::DEFAULT_CHANNEL_HASH,
			meshcore_aes_ecb,
			meshcore_hmac_sha256,
			frame,
			sizeof(frame));

		if (frame_len == 0) {
			ESP_LOGE(TAG, "MeshCore: failed to build GRP_DATA frame");
			return false;
		}

		uint32_t freq_hz = get_meshcore_freq_hz();
		uint8_t profile_idx = Settings::get_meshcore_gw_profile_idx();
		uint32_t sf = 10, bw_idx = 1;
		uint8_t cr = 1;
		uint16_t preamble = 16;

		if (profile_idx == 255) {
			uint8_t custom_sf = Settings::get_meshcore_gw_sf();
			if (custom_sf >= 7 && custom_sf <= 12) {
				sf = custom_sf;
			}
			uint16_t custom_bw = Settings::get_meshcore_gw_bw_khz();
			if (custom_bw > 0) {
				bw_idx = bw_khz_to_idx(custom_bw, bw_idx);
			}
			uint8_t custom_cr = Settings::get_meshcore_gw_cr();
			if (custom_cr > 0) {
				cr = cr_to_idx(custom_cr, cr);
			}
		} else {
			get_meshcore_rf_profile(profile_idx, &sf, &bw_idx, &cr, &preamble);
		}

		bool ok = WsHandler::sx126x_tx_meshcore(frame, (uint8_t)frame_len, freq_hz, sf, bw_idx, cr, preamble, lease);
		if (ok) {
			g_last_meshcore_send_ts = now_sec();
			g_meshcore_sent_count++;
			ESP_LOGI(TAG, "MeshCore TX observation packet #%u sent successfully (%u bytes @ %u Hz)",
				(unsigned)g_meshcore_sent_count, (unsigned)frame_len, (unsigned)freq_hz);
		}
		return ok;
	}

	/* ------------------------------------------------------------------ */
	/* Public API                                                          */
	/* ------------------------------------------------------------------ */

	esp_err_t init()
	{
		if (g_inited) return ESP_OK;

		init_identity();
		AdaptivePublish::reset_baseline(&g_meshtastic_pub_state);
		AdaptivePublish::reset_baseline(&g_meshcore_pub_state);
		g_inited = true;

		ESP_LOGI(TAG, "Meshtastic beacon ready: node %s \"%s\" (%s), channel \"%s\" hash 0x%02X, "
					  "enabled=%d interval=%us hop_limit=%d",
				 g_node_id_str, g_long_name, g_short_name,
				 MeshFrame::CHANNEL_NAME, g_channel_hash,
				 (int)Settings::get_mesh_env_telemetry_enabled(),
				 (unsigned)Settings::get_mesh_env_telemetry_interval_sec(),
				 (int)get_hop_limit());

		ESP_LOGI(TAG, "MeshCore gateway telemetry: enabled=%d interval=%us profile=%u freq=%u Hz",
				 (int)Settings::get_meshcore_gw_telemetry_enabled(),
				 (unsigned)Settings::get_meshcore_gw_telemetry_interval_sec(),
				 (unsigned)Settings::get_meshcore_gw_profile_idx(),
				 (unsigned)get_meshcore_freq_hz());

		if (get_mesh_freq_hz() == 0) {
			ESP_LOGW(TAG, "Region %d has no Meshtastic frequency mapping - beacon will stay silent",
					 (int)Settings::get_freq_region());
		}

		return ESP_OK;
	}

	void tick(const WsHandler::RadioLease &lease, const WsHandler::PacketOutcome &outcome)
	{
		if (!g_inited) return;
		if (!outcome.paired_p2p_cycle) return;

		bool requested    = g_send_requested;
		bool enabled      = Settings::get_mesh_env_telemetry_enabled();
		bool mc_requested = g_meshcore_send_requested;
		bool mc_enabled   = Settings::get_meshcore_gw_telemetry_enabled();

		if (!enabled && !requested && !mc_enabled && !mc_requested) return;

		/*
		 * Post-station transmission cooldown (~5 seconds):
		 * When our paired WS1300 station transmits its primary weather frame and receives our ACK,
		 * it may immediately borrow its own radio to transmit an auxiliary mesh frame (Meshtastic
		 * LongFast or MeshCore) which takes ~0.5s - 1.5s airtime before the station returns to deep sleep.
		 * By delaying gateway mesh transmissions by 5000ms:
		 * 1. The station completes its auxiliary transmission unhindered (zero CAD collisions).
		 * 2. The station goes to sleep, leaving the local RF channel quiet for the gateway.
		 * 3. The gateway finishes its transmission (~7.5s mark) with >12s of continuous RX margin
		 *    remaining before the next 20s station packet.
		 */
		const uint32_t POST_STATION_COOLDOWN_MS = 5000;
		vTaskDelay(pdMS_TO_TICKS(POST_STATION_COOLDOWN_MS));

		// Dual-stage validation: Stage 1 - Pre-flight Deadline Gate
		int64_t now_ms = (int64_t)(esp_timer_get_time() / 1000LL);
		if (lease.expiry_ms > 0 && (now_ms + 2500LL > lease.expiry_ms)) {
			ESP_LOGW(TAG, "Stage 1: insufficient lease remaining (%lld ms) for aux mesh tick", (long long)(lease.expiry_ms - now_ms));
			return;
		}

		// Cloud settings may rename the gateway after Meshtastic initialized.
		// Reannounce immediately instead of leaving clients with the boot-time
		// fallback name until the periodic NodeInfo timer expires.
		bool identity_changed = update_names();
		if (identity_changed) {
			g_last_nodeinfo_ts = 0;
			ESP_LOGI(TAG, "Meshtastic identity changed: \"%s\" (%s); NodeInfo refresh armed",
					 g_long_name, g_short_name);
		}

		/* Every packet borrows the station's SX1262, so schedule at most one
		 * frame in this safe post-station window. Due frames remain due after a
		 * failed/busy-channel attempt and are retried on the next window. */
		if (WsHandler::get_paired_id() == 0) {
			return;
		}

		uint32_t now = now_sec();

		/* 0. On boot/startup, announce NodeInfo first so receivers create a nodeDB
		 * entry and can name the gateway before environmental telemetry arrives. */
		if (enabled && g_last_nodeinfo_ts == 0) {
			if (send_nodeinfo(&lease)) {
				// NodeInfo transmitted successfully on boot. Give a brief inter-packet delay
				// and proceed within this safe radio lease so initial telemetry is also sent.
				vTaskDelay(pdMS_TO_TICKS(200));
			} else {
				g_mesh_failed_backoff_until = now + 60;
				return;
			}
		}

		/* Extract weather snapshot and active policy */
		AdaptivePublish::WeatherSnapshot snap;
		GatewaySnapshot::build(&snap);
		AdaptivePublish::WeatherChangePolicy policy = Settings::get_weather_policy();

		/* If both Meshtastic and MeshCore are enabled on the gateway, phase-offset MeshCore by
		 * interval / 2 so the two protocols never become due on the same reporting cycle. */
		if (mc_enabled && enabled && !g_meshcore_pub_state.baseline_valid && !mc_requested) {
			uint32_t mc_heartbeat_sec = Settings::get_meshcore_gw_telemetry_interval_sec();
			g_meshcore_pub_state.baseline = snap;
			g_meshcore_pub_state.baseline_valid = true;
			g_meshcore_pub_state.last_heartbeat_ms = now_ms - (int64_t)(mc_heartbeat_sec / 2) * 1000LL;
			ESP_LOGI(TAG, "MeshCore gateway telemetry initialized with phase offset (%u s / 2 = %u s)",
					 (unsigned)mc_heartbeat_sec, (unsigned)(mc_heartbeat_sec / 2));
		}

		if (requested) {
			g_meshtastic_pub_state.pending_reasons |= AdaptivePublish::REASON_MANUAL;
			if (enabled && (now - g_last_nodeinfo_ts >= 60)) {
				send_nodeinfo(&lease);
				vTaskDelay(pdMS_TO_TICKS(200));
			}
		}
		if (mc_requested) {
			g_meshcore_pub_state.pending_reasons |= AdaptivePublish::REASON_MANUAL;
		}

		/* 1. MeshCore Gateway Telemetry */
		if (mc_requested || (mc_enabled && now >= g_meshcore_failed_backoff_until)) {
			uint32_t mc_heartbeat_sec = Settings::get_meshcore_gw_telemetry_interval_sec();
			AdaptivePublish::PublishDecision decision = AdaptivePublish::evaluate(
				snap, policy, &g_meshcore_pub_state, mc_heartbeat_sec, now_ms);

			if (decision.publish) {
				if (send_meshcore_telemetry_packet(&lease)) {
					AdaptivePublish::mark_success(&g_meshcore_pub_state, decision, policy, now_ms);
					g_meshcore_send_requested = false;
					return;
				} else {
					AdaptivePublish::mark_failure(&g_meshcore_pub_state, decision);
					g_meshcore_failed_backoff_until = now + 60;
					g_meshcore_send_requested = false;
					return;
				}
			}
		}

		/* 2. Meshtastic Environmental Telemetry */
		if (requested || (enabled && now >= g_mesh_failed_backoff_until)) {
			uint32_t heartbeat_sec = Settings::get_mesh_env_telemetry_interval_sec();
			AdaptivePublish::PublishDecision decision = AdaptivePublish::evaluate(
				snap, policy, &g_meshtastic_pub_state, heartbeat_sec, now_ms);

			if (decision.publish) {
				MeshFrame::EnvValues env_values;
				if (collect_env(&env_values) && send_telemetry_packet(&env_values, &lease)) {
					AdaptivePublish::mark_success(&g_meshtastic_pub_state, decision, policy, now_ms);
					g_send_requested = false;
					return;
				} else {
					AdaptivePublish::mark_failure(&g_meshtastic_pub_state, decision);
					g_mesh_failed_backoff_until = now + 60;
					g_send_requested = false;
					return;
				}
			}
		}

		/* 3. Decoupled Meshtastic Aux Packets (NodeInfo 2xI, Device 3xI, Position 6xI) */
		if (enabled && now >= g_mesh_failed_backoff_until) {
			uint32_t heartbeat_sec = Settings::get_mesh_env_telemetry_interval_sec();
			uint32_t nodeinfo_interval_sec = heartbeat_sec * 2;
			uint32_t device_interval_sec = heartbeat_sec * 3;
			uint32_t position_interval_sec = heartbeat_sec * 6;

			if (identity_changed || schedule_due(g_last_nodeinfo_ts, now, nodeinfo_interval_sec, 1)) {
				if (send_nodeinfo(&lease)) {
					return;
				} else {
					g_mesh_failed_backoff_until = now + 60;
					return;
				}
			}

			if (schedule_due(g_last_device_metrics_ts, now, device_interval_sec, 1)) {
				MeshFrame::DeviceMetricsValues dev_values;
				if (collect_device_metrics(&dev_values)) {
					if (send_device_metrics_packet(&dev_values, &lease)) {
						return;
					} else {
						g_mesh_failed_backoff_until = now + 60;
						return;
					}
				}
			}

			if (schedule_due(g_last_position_ts, now, position_interval_sec, 1)) {
				if (send_position(&lease)) {
					return;
				} else {
					g_mesh_failed_backoff_until = now + 60;
					return;
				}
			}
		}
	}

	void tick()
	{
		WsHandler::PacketOutcome dummy_outcome = {
			.success = true,
			.paired_p2p_cycle = true,
			.completion_timestamp_ms = (int64_t)(esp_timer_get_time() / 1000LL)
		};
		WsHandler::RadioLease dummy_lease = {
			.expiry_ms = 0,
			.caller_id = "fallback"
		};
		tick(dummy_lease, dummy_outcome);
	}

	bool request_send(uint32_t wait_ms)
	{
		if (!g_inited) return false;

		uint32_t before = g_sent_count;
		g_send_requested = true;

		if (wait_ms == 0) return true;

		uint32_t waited = 0;
		while (waited < wait_ms) {
			vTaskDelay(pdMS_TO_TICKS(100));
			waited += 100;
			if (g_sent_count != before) return true;
			if (!g_send_requested) return false; /* consumed but the send failed */
		}
		return false;
	}

	bool send_now()
	{
		return request_send(25000);
	}

	uint32_t get_seconds_since_last_send()
	{
		if (g_last_send_ts == 0) return 0xFFFFFFFF;
		uint32_t now = now_sec();
		return (now >= g_last_send_ts) ? (now - g_last_send_ts) : 0;
	}

	uint32_t get_sent_count()
	{
		return g_sent_count;
	}

	const char *get_long_name()
	{
		update_names();
		return g_long_name;
	}

	const char *get_short_name()
	{
		update_names();
		return g_short_name;
	}

	void get_default_long_name(char *out, size_t max_len)
	{
		if (!out || max_len == 0) return;
		snprintf(out, max_len, "WeatherXM WG1200 %04X", (unsigned)(g_node_num & 0xFFFF));
	}

	void get_default_short_name(char *out, size_t max_len)
	{
		if (!out || max_len == 0) return;
		snprintf(out, max_len, "G%03X", (unsigned)(g_node_num & 0x0FFF));
	}

	uint8_t get_hop_limit()
	{
		return Settings::get_mesh_env_telemetry_hop_limit();
	}

	bool request_meshcore_send(uint32_t wait_ms)
	{
		if (!g_inited) return false;

		uint32_t before = g_meshcore_sent_count;
		g_meshcore_send_requested = true;

		if (wait_ms == 0) return true;

		uint32_t waited = 0;
		while (waited < wait_ms) {
			vTaskDelay(pdMS_TO_TICKS(100));
			waited += 100;
			if (g_meshcore_sent_count != before) return true;
			if (!g_meshcore_send_requested) return false;
		}
		return false;
	}

	bool send_meshcore_now()
	{
		return request_meshcore_send(25000);
	}

	uint32_t get_meshcore_seconds_since_last_send()
	{
		if (g_last_meshcore_send_ts == 0) return 0xFFFFFFFF;
		uint32_t now = now_sec();
		return (now >= g_last_meshcore_send_ts) ? (now - g_last_meshcore_send_ts) : 0;
	}

	uint32_t get_meshcore_sent_count()
	{
		return g_meshcore_sent_count;
	}

	void reset_baseline()
	{
		AdaptivePublish::reset_baseline(&g_meshtastic_pub_state);
		AdaptivePublish::reset_baseline(&g_meshcore_pub_state);
		ESP_LOGI(TAG, "Evaluator baselines reset");
	}

	EvaluatorStatus get_evaluator_status(MeshProtocol proto)
	{
		EvaluatorStatus status;
		memset(&status, 0, sizeof(status));
		const AdaptivePublish::PublisherState &st = (proto == MeshProtocol::MESHTASTIC) ? g_meshtastic_pub_state : g_meshcore_pub_state;
		status.events_this_hour = st.events_this_hour;
		status.cooldown_level = st.escalation_level;
		status.last_reasons = st.pending_reasons;

		AdaptivePublish::WeatherSnapshot snap;
		GatewaySnapshot::build(&snap);
		AdaptivePublish::format_reasons(st.pending_reasons, snap, st.baseline, status.last_reasons_str, sizeof(status.last_reasons_str));
		return status;
	}
}
