#ifndef MESH_FRAME_H
#define MESH_FRAME_H

/******************************************************************************
 * Meshtastic over-the-air frame construction.
 *
 * Pure logic, no ESP-IDF and no platform dependencies, so that it can be
 * compiled and tested natively. AES is injected as a callback; the firmware
 * supplies an mbedtls-backed one, the test supplies its own.
 *
 * The over-the-air format is NOT a serialized meshtastic.MeshPacket - that
 * message only exists on the phone/serial API. On the LoRa PHY:
 *
 *   offset  0 : to          uint32 little endian
 *   offset  4 : from        uint32 little endian
 *   offset  8 : id          uint32 little endian
 *   offset 12 : flags       uint8   bits 0-2 hop_limit
 *                                   bit  3   want_ack
 *                                   bit  4   via_mqtt
 *                                   bits 5-7 hop_start
 *   offset 13 : channel     uint8   channel hash
 *   offset 14 : next_hop    uint8   0 = unknown
 *   offset 15 : relay_node  uint8   0 = unknown
 *   offset 16+: AES-CTR( serialized meshtastic.Data )
 *
 * Crypto is AES-128-CTR with the channel key. The initial 16 byte counter is
 *   [ packet_id : uint64 LE ][ from : uint32 LE ][ 0 : uint32 ]
 * per CryptoEngine::initNonce in the Meshtastic firmware.
 *
 * The channel hash is xor(channel name bytes) ^ xor(psk bytes), per
 * Channels::generateHash. "LongFast" + the public default PSK gives 0x08.
 *
 * Protobuf field numbers are hand-encoded from meshtastic/telemetry.proto and
 * meshtastic/mesh.proto, so that no GPL-3.0 generated sources are linked into
 * this firmware. Any change here must be checked against the upstream .proto
 * files; tools/mesh_frame_test guards the wire layout.
 ******************************************************************************/

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace MeshFrame
{
	/* ------------------------------------------------------------------ */
	/* Protocol constants                                                  */
	/* ------------------------------------------------------------------ */

	/** Public default channel PSK (psk index 1 / "AQ==" expanded). AES128. */
	static const uint8_t DEFAULT_PSK[16] = {
		0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
		0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
	};

	/** Default primary channel name for the LONG_FAST modem preset. */
	static const char CHANNEL_NAME[] = "LongFast";

	static const uint32_t BROADCAST_ADDR = 0xFFFFFFFFu;
	static const size_t   HEADER_LEN     = 16;
	/** Meshtastic caps the PHY frame at 255 bytes. */
	static const size_t   MAX_FRAME_LEN  = 255;

	/* PortNum, from meshtastic/portnums.proto */
	static const uint32_t PORTNUM_POSITION_APP  = 3;
	static const uint32_t PORTNUM_NODEINFO_APP  = 4;
	static const uint32_t PORTNUM_TELEMETRY_APP = 67;

	/* Header flag layout, from RadioInterface.h */
	static const uint8_t FLAGS_HOP_LIMIT_MASK  = 0x07;
	static const uint8_t FLAGS_HOP_START_MASK  = 0xE0;
	static const uint8_t FLAGS_HOP_START_SHIFT = 5;

	/** Config.DeviceConfig.Role.SENSOR */
	static const uint32_t ROLE_SENSOR = 6;
	/** HardwareModel.PRIVATE_HW - honest "not a Meshtastic device" marker. */
	static const uint32_t HW_PRIVATE_HW = 255;

	/** AES-128 single block ECB encrypt. */
	typedef void (*AesEcbFn)(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

	/* ------------------------------------------------------------------ */
	/* Minimal protobuf writer                                             */
	/* ------------------------------------------------------------------ */

	struct PbBuf
	{
		uint8_t *buf;
		size_t   cap;
		size_t   len;
		bool     ok;
	};

	static inline void pb_init(PbBuf *b, uint8_t *storage, size_t cap)
	{
		b->buf = storage;
		b->cap = cap;
		b->len = 0;
		b->ok  = true;
	}

	static inline void pb_raw(PbBuf *b, const void *src, size_t n)
	{
		if (!b->ok) return;
		if (b->len + n > b->cap) { b->ok = false; return; }
		memcpy(b->buf + b->len, src, n);
		b->len += n;
	}

	static inline void pb_varint(PbBuf *b, uint64_t v)
	{
		uint8_t tmp[10];
		size_t n = 0;
		while (v >= 0x80) {
			tmp[n++] = (uint8_t)((v & 0x7F) | 0x80);
			v >>= 7;
		}
		tmp[n++] = (uint8_t)v;
		pb_raw(b, tmp, n);
	}

	/* Wire types: 0 = varint, 2 = length delimited, 5 = 32 bit */
	static inline void pb_tag(PbBuf *b, uint32_t field, uint8_t wire)
	{
		pb_varint(b, ((uint64_t)field << 3) | wire);
	}

	static inline void pb_float(PbBuf *b, uint32_t field, float v)
	{
		uint32_t bits;
		memcpy(&bits, &v, 4);
		pb_tag(b, field, 5);
		uint8_t le[4] = {
			(uint8_t)(bits & 0xFF),         (uint8_t)((bits >> 8) & 0xFF),
			(uint8_t)((bits >> 16) & 0xFF), (uint8_t)((bits >> 24) & 0xFF)
		};
		pb_raw(b, le, 4);
	}

	static inline void pb_uint32(PbBuf *b, uint32_t field, uint32_t v)
	{
		pb_tag(b, field, 0);
		pb_varint(b, v);
	}

	static inline void pb_fixed32(PbBuf *b, uint32_t field, uint32_t v)
	{
		pb_tag(b, field, 5);
		uint8_t le[4] = {
			(uint8_t)(v & 0xFF),         (uint8_t)((v >> 8) & 0xFF),
			(uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF)
		};
		pb_raw(b, le, 4);
	}

	static inline void pb_bytes(PbBuf *b, uint32_t field, const void *data, size_t n)
	{
		pb_tag(b, field, 2);
		pb_varint(b, n);
		pb_raw(b, data, n);
	}

	static inline void pb_string(PbBuf *b, uint32_t field, const char *s)
	{
		pb_bytes(b, field, s, strlen(s));
	}

	/* ------------------------------------------------------------------ */
	/* Channel hash                                                        */
	/* ------------------------------------------------------------------ */

	static inline uint8_t xor_hash(const uint8_t *p, size_t len)
	{
		uint8_t h = 0;
		for (size_t i = 0; i < len; i++) h ^= p[i];
		return h;
	}

	/** Channels::generateHash equivalent. */
	static inline uint8_t channel_hash(const char *name, const uint8_t *psk, size_t psk_len)
	{
		return (uint8_t)(xor_hash((const uint8_t *)name, strlen(name)) ^ xor_hash(psk, psk_len));
	}

	/** Hash for the public default channel ("LongFast" + default PSK) = 0x08. */
	static inline uint8_t default_channel_hash()
	{
		return channel_hash(CHANNEL_NAME, DEFAULT_PSK, sizeof(DEFAULT_PSK));
	}

	/* ------------------------------------------------------------------ */
	/* AES-128-CTR, built on ECB so no cipher-mode build option is needed  */
	/* ------------------------------------------------------------------ */

	static inline void ctr_xor(const uint8_t key[16], uint32_t from_node, uint32_t packet_id,
							   uint8_t *data, size_t len, AesEcbFn aes)
	{
		uint8_t counter[16];
		memset(counter, 0, sizeof(counter));
		uint64_t pid64 = (uint64_t)packet_id;
		for (int i = 0; i < 8; i++) counter[i]     = (uint8_t)((pid64 >> (8 * i)) & 0xFF);
		for (int i = 0; i < 4; i++) counter[8 + i] = (uint8_t)((from_node >> (8 * i)) & 0xFF);

		uint8_t stream[16];
		size_t off = 0;
		while (off < len) {
			aes(key, counter, stream);
			size_t chunk = (len - off < 16) ? (len - off) : 16;
			for (size_t i = 0; i < chunk; i++) data[off + i] ^= stream[i];
			off += chunk;

			/* Block counter increment. The initial counter block is laid out
			 * little-endian (packet id, then node number), but the increment
			 * itself is big-endian, starting at the last byte - that is what
			 * mbedtls_aes_crypt_ctr does, and Meshtastic's encryptAESCtr is
			 * built on it. Incrementing the other way produces a frame whose
			 * first 16 bytes decode and whose remainder is garbage. */
			for (int i = 16; i > 0; i--) {
				if (++counter[i - 1] != 0) break;
			}
		}
	}

	/* ------------------------------------------------------------------ */
	/* Payload builders                                                    */
	/* ------------------------------------------------------------------ */

	/**
	 * meshtastic.EnvironmentMetrics. Field numbers per telemetry.proto:
	 *   temperature 1 (float, degC), relative_humidity 2 (float, %),
	 *   barometric_pressure 3 (float, hPa), lux 9 (float),
	 *   wind_direction 13 (uint32, deg), wind_speed 14 (float, m/s),
	 *   wind_gust 16 (float, m/s), rainfall_1h 19 (float, mm),
	 *   rainfall_24h 20 (float, mm)
	 *
	 * NOTE: solar irradiance has no home here. EnvironmentMetrics.radiation
	 * (18) is ionizing radiation in uR/h and must not be used for W/m2.
	 */
	struct EnvValues
	{
		bool  has_temperature;      float    temperature;
		bool  has_humidity;         float    humidity;
		bool  has_pressure;         float    pressure;
		bool  has_lux;              float    lux;
		bool  has_wind_direction;   uint32_t wind_direction;
		bool  has_wind_speed;       float    wind_speed;
		bool  has_wind_gust;        float    wind_gust;
		bool  has_rainfall_1h;      float    rainfall_1h;
		bool  has_rainfall_24h;     float    rainfall_24h;
	};

	static inline size_t build_env_metrics(const EnvValues *v, uint8_t *out, size_t cap)
	{
		PbBuf b;
		pb_init(&b, out, cap);
		/* Preserve the core weather readings, including a valid 0 C value. The
		 * optional extended readings are omitted when zero: absent optional
		 * protobuf fields are wire-compatible with every Meshtastic version, and
		 * calm/dry/dark observations otherwise add up to 24 bytes of SF11 airtime
		 * without adding a non-zero measurement.
		 *
		 * That is ~205 ms of extra airtime at SF11/BW250 (~68 -> ~93 symbols),
		 * which is 25% more time for something to disturb the transmit. While
		 * the CRC failures are unresolved, shorter frames are the safer default.
		 * If you would rather advertise "measured zero" than "no reading" - a
		 * defensible call for wind direction/speed - drop the != 0 tests, but
		 * do it as its own change so the airtime effect stays attributable. */
		if (v->has_temperature)                                 pb_float(&b, 1, v->temperature);
		if (v->has_humidity)                                    pb_float(&b, 2, v->humidity);
		if (v->has_pressure)                                    pb_float(&b, 3, v->pressure);
		if (v->has_lux)                                         pb_float(&b, 9, v->lux);
		if (v->has_wind_direction)                              pb_uint32(&b, 13, v->wind_direction);
		if (v->has_wind_speed)                                  pb_float(&b, 14, v->wind_speed);
		if (v->has_wind_gust)                                   pb_float(&b, 16, v->wind_gust);
		if (v->has_rainfall_1h)                                 pb_float(&b, 19, v->rainfall_1h);
		if (v->has_rainfall_24h)                                pb_float(&b, 20, v->rainfall_24h);
		return b.ok ? b.len : 0;
	}

	/**
	 * meshtastic.DeviceMetrics. Field numbers per telemetry.proto:
	 *   battery_level 1 (uint32, 0-100 or >100 powered), voltage 2 (float, V),
	 *   uptime_seconds 5 (uint32, sec)
	 */
	struct DeviceMetricsValues
	{
		bool     has_battery_level; uint32_t battery_level;
		bool     has_voltage;       float    voltage;
		bool     has_uptime;        uint32_t uptime_seconds;
	};

	static inline size_t build_device_metrics(const DeviceMetricsValues *v, uint8_t *out, size_t cap)
	{
		PbBuf b;
		pb_init(&b, out, cap);
		if (v->has_battery_level) pb_uint32(&b, 1, v->battery_level);
		if (v->has_voltage)       pb_float(&b, 2, v->voltage);
		if (v->has_uptime)        pb_uint32(&b, 5, v->uptime_seconds);
		return b.ok ? b.len : 0;
	}

	/** meshtastic.Telemetry { time = 1, environment_metrics = 3 } */
	static inline size_t build_telemetry(uint32_t epoch_sec,
										 const uint8_t *env, size_t env_len,
										 uint8_t *out, size_t cap)
	{
		if (env_len == 0) return 0;
		PbBuf b;
		pb_init(&b, out, cap);
		/* Only stamp a plausible wall-clock epoch. Before NTP/GPS sync the
		 * caller has nothing but uptime, and putting uptime seconds in a Unix
		 * epoch field makes the gateway advertise readings dated 1970. An
		 * absent optional field correctly means "no timestamp"; receivers fall
		 * back to their own rx time. */
		if (epoch_sec > 1600000000u) pb_fixed32(&b, 1, epoch_sec);
		pb_bytes(&b, 3, env, env_len);
		return b.ok ? b.len : 0;
	}

	/** meshtastic.Telemetry { time = 1, device_metrics = 2 } */
	static inline size_t build_telemetry_device_metrics(uint32_t epoch_sec,
														 const uint8_t *dev, size_t dev_len,
														 uint8_t *out, size_t cap)
	{
		if (dev_len == 0) return 0;
		PbBuf b;
		pb_init(&b, out, cap);
		/* See build_telemetry(): never stamp uptime into a Unix epoch field. */
		if (epoch_sec > 1600000000u) pb_fixed32(&b, 1, epoch_sec);
		pb_bytes(&b, 2, dev, dev_len);
		return b.ok ? b.len : 0;
	}

	/**
	 * meshtastic.Position. Field numbers per mesh.proto:
	 *   latitude_i 1 (sfixed32, deg * 1e7), longitude_i 2 (sfixed32, deg * 1e7),
	 *   altitude 3 (int32, meters MSL), time 4 (fixed32, epoch sec)
	 */
	struct PositionValues
	{
		float    lat;
		float    lon;
		bool     has_altitude;
		int32_t  altitude;
		uint32_t time;
	};

	static inline size_t build_position(const PositionValues *v, uint8_t *out, size_t cap)
	{
		PbBuf b;
		pb_init(&b, out, cap);
		int32_t lat_i = (int32_t)((double)v->lat * 1e7);
		int32_t lon_i = (int32_t)((double)v->lon * 1e7);
		pb_fixed32(&b, 1, (uint32_t)lat_i);
		pb_fixed32(&b, 2, (uint32_t)lon_i);
		if (v->has_altitude) pb_uint32(&b, 3, (uint32_t)v->altitude);
		if (v->time > 1600000000u) pb_fixed32(&b, 4, v->time);
		return b.ok ? b.len : 0;
	}

	/** meshtastic.User { id = 1, long_name = 2, short_name = 3, hw_model = 5, role = 7, public_key = 8, is_unmessagable = 9 } */
	static inline size_t build_nodeinfo(const char *id, const char *long_name, const char *short_name,
										uint32_t hw_model, uint32_t role,
										const uint8_t *pubkey, size_t pubkey_len,
										uint8_t *out, size_t cap)
	{
		PbBuf b;
		pb_init(&b, out, cap);
		pb_string(&b, 1, id);
		pb_string(&b, 2, long_name);
		pb_string(&b, 3, short_name);
		pb_uint32(&b, 5, hw_model);
		pb_uint32(&b, 7, role);
		if (pubkey && pubkey_len > 0) {
			pb_bytes(&b, 8, pubkey, pubkey_len);
		}
		pb_uint32(&b, 9, 1); /* is_unmessagable = true */
		return b.ok ? b.len : 0;
	}

	static inline size_t build_nodeinfo(const char *id, const char *long_name, const char *short_name,
										uint32_t hw_model, uint32_t role,
										uint8_t *out, size_t cap)
	{
		return build_nodeinfo(id, long_name, short_name, hw_model, role, nullptr, 0, out, cap);
	}

	/* ------------------------------------------------------------------ */
	/* Frame assembly                                                      */
	/* ------------------------------------------------------------------ */

	/**
	 * Wrap an application payload in meshtastic.Data { portnum = 1, payload = 2 },
	 * encrypt it, and prepend the PHY header.
	 * @return total frame length, or 0 on failure.
	 */
	static inline size_t build_frame(uint32_t to, uint32_t from, uint32_t packet_id,
									 uint8_t chan_hash, uint8_t hop_limit,
									 uint32_t portnum, const uint8_t *payload, size_t payload_len,
									 const uint8_t key[16], AesEcbFn aes,
									 uint8_t *out, size_t cap)
	{
		if (cap < HEADER_LEN || payload_len == 0 || packet_id == 0) return 0;

		uint8_t data_storage[224];
		PbBuf data;
		pb_init(&data, data_storage, sizeof(data_storage));
		pb_uint32(&data, 1, portnum);
		pb_bytes(&data, 2, payload, payload_len);
		if (!data.ok) return 0;

		if (HEADER_LEN + data.len > cap || HEADER_LEN + data.len > MAX_FRAME_LEN) return 0;

		uint8_t flags = (uint8_t)(hop_limit & FLAGS_HOP_LIMIT_MASK);
		flags |= (uint8_t)((hop_limit << FLAGS_HOP_START_SHIFT) & FLAGS_HOP_START_MASK);
		/* want_ack and via_mqtt stay clear: this is a fire and forget beacon. */

		out[0]  = (uint8_t)(to & 0xFF);
		out[1]  = (uint8_t)((to >> 8) & 0xFF);
		out[2]  = (uint8_t)((to >> 16) & 0xFF);
		out[3]  = (uint8_t)((to >> 24) & 0xFF);
		out[4]  = (uint8_t)(from & 0xFF);
		out[5]  = (uint8_t)((from >> 8) & 0xFF);
		out[6]  = (uint8_t)((from >> 16) & 0xFF);
		out[7]  = (uint8_t)((from >> 24) & 0xFF);
		out[8]  = (uint8_t)(packet_id & 0xFF);
		out[9]  = (uint8_t)((packet_id >> 8) & 0xFF);
		out[10] = (uint8_t)((packet_id >> 16) & 0xFF);
		out[11] = (uint8_t)((packet_id >> 24) & 0xFF);
		out[12] = flags;
		out[13] = chan_hash;
		out[14] = 0; /* next_hop unknown */
		out[15] = 0; /* relay_node unknown */

		memcpy(out + HEADER_LEN, data.buf, data.len);
		ctr_xor(key, from, packet_id, out + HEADER_LEN, data.len, aes);

		return HEADER_LEN + data.len;
	}

	/* ------------------------------------------------------------------ */
	/* Frame decoding and Protobuf parsing                                */
	/* ------------------------------------------------------------------ */

	struct DecodedFrame
	{
		uint32_t to;
		uint32_t from;
		uint32_t id;
		uint8_t  hop_limit;
		uint8_t  channel_hash;
		uint32_t portnum;

		bool has_env;
		EnvValues env;

		bool has_device_metrics;
		DeviceMetricsValues device;

		bool has_nodeinfo;
		char user_id[32];
		char long_name[40];
		char short_name[10];
		uint32_t hw_model;
		uint32_t role;

		bool has_position;
		PositionValues position;

		bool has_text;
		char text[128];
	};

	static inline bool pb_read_varint(const uint8_t **ptr, const uint8_t *end, uint64_t *val)
	{
		uint64_t v = 0;
		int shift = 0;
		while (*ptr < end) {
			uint8_t byte = *(*ptr)++;
			v |= (uint64_t)(byte & 0x7F) << shift;
			if ((byte & 0x80) == 0) {
				*val = v;
				return true;
			}
			shift += 7;
			if (shift >= 64) return false;
		}
		return false;
	}

	static inline bool pb_read_fixed32(const uint8_t **ptr, const uint8_t *end, uint32_t *val)
	{
		if (*ptr + 4 > end) return false;
		*val = (uint32_t)(*ptr)[0] | ((uint32_t)(*ptr)[1] << 8) | ((uint32_t)(*ptr)[2] << 16) | ((uint32_t)(*ptr)[3] << 24);
		*ptr += 4;
		return true;
	}

	static inline bool pb_read_float(const uint8_t **ptr, const uint8_t *end, float *val)
	{
		uint32_t u = 0;
		if (!pb_read_fixed32(ptr, end, &u)) return false;
		memcpy(val, &u, sizeof(float));
		return true;
	}

	static inline bool pb_skip_field(const uint8_t **ptr, const uint8_t *end, uint32_t wire_type)
	{
		if (wire_type == 0) { // Varint
			uint64_t dummy;
			return pb_read_varint(ptr, end, &dummy);
		} else if (wire_type == 1) { // 64-bit
			if (*ptr + 8 > end) return false;
			*ptr += 8;
			return true;
		} else if (wire_type == 2) { // Length-delimited
			uint64_t len;
			if (!pb_read_varint(ptr, end, &len)) return false;
			if (len > (size_t)(end - *ptr)) return false;
			*ptr += len;
			return true;
		} else if (wire_type == 5) { // 32-bit
			if (*ptr + 4 > end) return false;
			*ptr += 4;
			return true;
		}
		return false;
	}

	/**
	 * Decodes and decrypts an over-the-air Meshtastic frame into DecodedFrame.
	 */
	static inline bool decode_frame(
		const uint8_t *raw_frame,
		size_t frame_len,
		const uint8_t key[16],
		AesEcbFn aes,
		DecodedFrame *out)
	{
		if (raw_frame == nullptr || frame_len < HEADER_LEN || out == nullptr) return false;

		memset(out, 0, sizeof(*out));
		out->to = (uint32_t)raw_frame[0] | ((uint32_t)raw_frame[1] << 8) |
				  ((uint32_t)raw_frame[2] << 16) | ((uint32_t)raw_frame[3] << 24);
		out->from = (uint32_t)raw_frame[4] | ((uint32_t)raw_frame[5] << 8) |
					((uint32_t)raw_frame[6] << 16) | ((uint32_t)raw_frame[7] << 24);
		out->id = (uint32_t)raw_frame[8] | ((uint32_t)raw_frame[9] << 8) |
				  ((uint32_t)raw_frame[10] << 16) | ((uint32_t)raw_frame[11] << 24);
		out->hop_limit = raw_frame[12] & FLAGS_HOP_LIMIT_MASK;
		out->channel_hash = raw_frame[13];

		if (out->from == 0 || frame_len == HEADER_LEN) {
			return false;
		}

		size_t payload_len = frame_len - HEADER_LEN;
		uint8_t decrypted[MAX_FRAME_LEN];
		if (payload_len > sizeof(decrypted)) return false;
		memcpy(decrypted, raw_frame + HEADER_LEN, payload_len);

		ctr_xor(key, out->from, out->id, decrypted, payload_len, aes);

		// Parse meshtastic.Data
		const uint8_t *ptr = decrypted;
		const uint8_t *end = decrypted + payload_len;

		const uint8_t *app_payload = nullptr;
		size_t app_payload_len = 0;

		while (ptr < end) {
			uint64_t tag;
			if (!pb_read_varint(&ptr, end, &tag)) return false;
			uint32_t wire_type = (uint32_t)(tag & 7);
			uint32_t field_num = (uint32_t)(tag >> 3);

			if (field_num == 1 && wire_type == 0) { // portnum
				uint64_t pn;
				if (!pb_read_varint(&ptr, end, &pn)) return false;
				out->portnum = (uint32_t)pn;
			} else if (field_num == 2 && wire_type == 2) { // payload
				uint64_t len;
				if (!pb_read_varint(&ptr, end, &len)) return false;
				if (len > (size_t)(end - ptr)) return false;
				app_payload = ptr;
				app_payload_len = (size_t)len;
				ptr += len;
			} else {
				if (!pb_skip_field(&ptr, end, wire_type)) return false;
			}
		}

		if (out->portnum == 0 || out->portnum > 512 || app_payload == nullptr || app_payload_len == 0) {
			return false;
		}

		if (app_payload != nullptr && app_payload_len > 0) {
			const uint8_t *ap_ptr = app_payload;
			const uint8_t *ap_end = app_payload + app_payload_len;

			if (out->portnum == PORTNUM_TELEMETRY_APP) { // 67
				while (ap_ptr < ap_end) {
					uint64_t tag;
					if (!pb_read_varint(&ap_ptr, ap_end, &tag)) return false;
					uint32_t wire = (uint32_t)(tag & 7);
					uint32_t field = (uint32_t)(tag >> 3);

					if (field == 2 && wire == 2) { // device_metrics
						uint64_t sublen;
						if (!pb_read_varint(&ap_ptr, ap_end, &sublen)) return false;
						if (sublen > (size_t)(ap_end - ap_ptr)) return false;
						const uint8_t *dm_ptr = ap_ptr;
						const uint8_t *dm_end = ap_ptr + sublen;
						ap_ptr += sublen;

						out->has_device_metrics = true;
						while (dm_ptr < dm_end) {
							uint64_t dm_tag;
							if (!pb_read_varint(&dm_ptr, dm_end, &dm_tag)) return false;
							uint32_t dm_wire = (uint32_t)(dm_tag & 7);
							uint32_t dm_f = (uint32_t)(dm_tag >> 3);
							if (dm_f == 1 && dm_wire == 0) { // battery_level
								uint64_t bl;
								if (!pb_read_varint(&dm_ptr, dm_end, &bl)) return false;
								out->device.has_battery_level = true;
								out->device.battery_level = (uint32_t)bl;
							} else if (dm_f == 2 && dm_wire == 5) { // voltage
								float v;
								if (!pb_read_float(&dm_ptr, dm_end, &v)) return false;
								out->device.has_voltage = true;
								out->device.voltage = v;
							} else if (dm_f == 5 && dm_wire == 0) { // uptime_seconds
								uint64_t up;
								if (!pb_read_varint(&dm_ptr, dm_end, &up)) return false;
								out->device.has_uptime = true;
								out->device.uptime_seconds = (uint32_t)up;
							} else {
								if (!pb_skip_field(&dm_ptr, dm_end, dm_wire)) return false;
							}
						}
					} else if (field == 3 && wire == 2) { // environment_metrics
						uint64_t sublen;
						if (!pb_read_varint(&ap_ptr, ap_end, &sublen)) return false;
						if (sublen > (size_t)(ap_end - ap_ptr)) return false;
						const uint8_t *em_ptr = ap_ptr;
						const uint8_t *em_end = ap_ptr + sublen;
						ap_ptr += sublen;

						out->has_env = true;
						while (em_ptr < em_end) {
							uint64_t em_tag;
							if (!pb_read_varint(&em_ptr, em_end, &em_tag)) return false;
							uint32_t em_wire = (uint32_t)(em_tag & 7);
							uint32_t em_f = (uint32_t)(em_tag >> 3);

							if (em_f == 1 && em_wire == 5) { // temp
								float v;
								if (!pb_read_float(&em_ptr, em_end, &v)) return false;
								out->env.has_temperature = true;
								out->env.temperature = v;
							} else if (em_f == 2 && em_wire == 5) { // humidity
								float v;
								if (!pb_read_float(&em_ptr, em_end, &v)) return false;
								out->env.has_humidity = true;
								out->env.humidity = v;
							} else if (em_f == 3 && em_wire == 5) { // pressure
								float v;
								if (!pb_read_float(&em_ptr, em_end, &v)) return false;
								out->env.has_pressure = true;
								out->env.pressure = v;
							} else if (em_f == 9 && em_wire == 5) { // lux
								float v;
								if (!pb_read_float(&em_ptr, em_end, &v)) return false;
								out->env.has_lux = true;
								out->env.lux = v;
							} else if (em_f == 13 && em_wire == 0) { // wind_direction
								uint64_t v;
								if (!pb_read_varint(&em_ptr, em_end, &v)) return false;
								out->env.has_wind_direction = true;
								out->env.wind_direction = (uint32_t)v;
							} else if (em_f == 14 && em_wire == 5) { // wind_speed
								float v;
								if (!pb_read_float(&em_ptr, em_end, &v)) return false;
								out->env.has_wind_speed = true;
								out->env.wind_speed = v;
							} else if (em_f == 16 && em_wire == 5) { // wind_gust
								float v;
								if (!pb_read_float(&em_ptr, em_end, &v)) return false;
								out->env.has_wind_gust = true;
								out->env.wind_gust = v;
							} else if (em_f == 19 && em_wire == 5) { // rainfall_1h
								float v;
								if (!pb_read_float(&em_ptr, em_end, &v)) return false;
								out->env.has_rainfall_1h = true;
								out->env.rainfall_1h = v;
							} else if (em_f == 20 && em_wire == 5) { // rainfall_24h
								float v;
								if (!pb_read_float(&em_ptr, em_end, &v)) return false;
								out->env.has_rainfall_24h = true;
								out->env.rainfall_24h = v;
							} else {
								if (!pb_skip_field(&em_ptr, em_end, em_wire)) return false;
							}
						}
					} else {
						if (!pb_skip_field(&ap_ptr, ap_end, wire)) return false;
					}
				}
			} else if (out->portnum == PORTNUM_NODEINFO_APP) { // 4
				out->has_nodeinfo = true;
				while (ap_ptr < ap_end) {
					uint64_t tag;
					if (!pb_read_varint(&ap_ptr, ap_end, &tag)) return false;
					uint32_t wire = (uint32_t)(tag & 7);
					uint32_t field = (uint32_t)(tag >> 3);

					if (field == 1 && wire == 2) { // id
						uint64_t slen;
						if (!pb_read_varint(&ap_ptr, ap_end, &slen)) return false;
						if (slen > (size_t)(ap_end - ap_ptr)) return false;
						size_t cplen = (slen < sizeof(out->user_id) - 1) ? (size_t)slen : sizeof(out->user_id) - 1;
						memcpy(out->user_id, ap_ptr, cplen);
						out->user_id[cplen] = '\0';
						ap_ptr += slen;
					} else if (field == 2 && wire == 2) { // long_name
						uint64_t slen;
						if (!pb_read_varint(&ap_ptr, ap_end, &slen)) return false;
						if (slen > (size_t)(ap_end - ap_ptr)) return false;
						size_t cplen = (slen < sizeof(out->long_name) - 1) ? (size_t)slen : sizeof(out->long_name) - 1;
						memcpy(out->long_name, ap_ptr, cplen);
						out->long_name[cplen] = '\0';
						ap_ptr += slen;
					} else if (field == 3 && wire == 2) { // short_name
						uint64_t slen;
						if (!pb_read_varint(&ap_ptr, ap_end, &slen)) return false;
						if (slen > (size_t)(ap_end - ap_ptr)) return false;
						size_t cplen = (slen < sizeof(out->short_name) - 1) ? (size_t)slen : sizeof(out->short_name) - 1;
						memcpy(out->short_name, ap_ptr, cplen);
						out->short_name[cplen] = '\0';
						ap_ptr += slen;
					} else if (field == 5 && wire == 0) { // hw_model
						uint64_t val;
						if (!pb_read_varint(&ap_ptr, ap_end, &val)) return false;
						out->hw_model = (uint32_t)val;
					} else if (field == 7 && wire == 0) { // role
						uint64_t val;
						if (!pb_read_varint(&ap_ptr, ap_end, &val)) return false;
						out->role = (uint32_t)val;
					} else {
						if (!pb_skip_field(&ap_ptr, ap_end, wire)) return false;
					}
				}
			} else if (out->portnum == PORTNUM_POSITION_APP) { // 3
				out->has_position = true;
				while (ap_ptr < ap_end) {
					uint64_t tag;
					if (!pb_read_varint(&ap_ptr, ap_end, &tag)) return false;
					uint32_t wire = (uint32_t)(tag & 7);
					uint32_t field = (uint32_t)(tag >> 3);

					if (field == 1 && wire == 5) { // latitude_i (sfixed32)
						uint32_t raw_lat;
						if (!pb_read_fixed32(&ap_ptr, ap_end, &raw_lat)) return false;
						int32_t lat_i = (int32_t)raw_lat;
						out->position.lat = (float)((double)lat_i * 1e-7);
					} else if (field == 2 && wire == 5) { // longitude_i (sfixed32)
						uint32_t raw_lon;
						if (!pb_read_fixed32(&ap_ptr, ap_end, &raw_lon)) return false;
						int32_t lon_i = (int32_t)raw_lon;
						out->position.lon = (float)((double)lon_i * 1e-7);
					} else if (field == 3 && wire == 0) { // altitude (int32)
						uint64_t val;
						if (!pb_read_varint(&ap_ptr, ap_end, &val)) return false;
						out->position.has_altitude = true;
						out->position.altitude = (int32_t)val;
					} else if (field == 4 && wire == 5) { // time (fixed32)
						uint32_t t;
						if (!pb_read_fixed32(&ap_ptr, ap_end, &t)) return false;
						out->position.time = t;
					} else {
						if (!pb_skip_field(&ap_ptr, ap_end, wire)) return false;
					}
				}
			} else if (out->portnum == 1) { // TEXT_MESSAGE_APP
				out->has_text = true;
				size_t cplen = (app_payload_len < sizeof(out->text) - 1) ? app_payload_len : sizeof(out->text) - 1;
				memcpy(out->text, app_payload, cplen);
				out->text[cplen] = '\0';
			}
		}

		return true;
	}
}

#endif /* MESH_FRAME_H */
