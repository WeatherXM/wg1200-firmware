#ifndef MESHCORE_FRAME_H
#define MESHCORE_FRAME_H

/******************************************************************************
 * MeshCore over-the-air frame construction and decoding.
 *
 * Wire protocol format (PAYLOAD_TYPE_GRP_DATA / flood routing):
 *   offset 0     : header (PAYLOAD_TYPE_GRP_DATA << 2) | ROUTE_TYPE_FLOOD = 0x19
 *   offset 1     : path_len (0x00 for flood mode origin, or (sz-1)<<6 | count)
 *   offset 2..   : path bytes (if path_len > 0)
 *   offset ..    : channel_hash (uint8)
 *   offset ..    : HMAC-SHA256 (2 bytes truncated MAC)
 *   offset ..    : AES-128-ECB ciphertext (padded to 16-byte boundary)
 *
 * Plaintext structure inside GRP_DATA:
 *   offset 0..1  : data_type (uint16 LE, 0x5758 for WeatherXM Observation)
 *   offset 2     : data_len (uint8, length of inner payload)
 *   offset 3..   : MeshCoreObsPayload (packed 28-byte struct) or
 *                  APP_SUBTYPE_SIGNED_STASENSORS_V1 (0x01) + 105B signed record
 ******************************************************************************/

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace MeshCoreFrame
{

static const uint8_t APP_SUBTYPE_SIGNED_STASENSORS_V1 = 0x01;
static const uint8_t ROUTE_TYPE_FLOOD = 0x01;
static const uint8_t PAYLOAD_TYPE_GRP_DATA = 0x06;
static const uint8_t MESHCORE_HEADER_GRP_DATA = (PAYLOAD_TYPE_GRP_DATA << 2) | ROUTE_TYPE_FLOOD; // 0x19
static const uint16_t DATA_TYPE_WEATHERXM_OBSERVATION = 0x5758; // 'W', 'X'
static const size_t CIPHER_MAC_SIZE = 2;
static const size_t CIPHER_BLOCK_SIZE = 16;
// Channel hash derived from SHA256(DEFAULT_PSK)[0] = 0xE8
static const uint8_t DEFAULT_CHANNEL_HASH = 0xE8;

static const uint8_t DEFAULT_PSK[16] = {
	0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
	0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
};

struct __attribute__((packed)) MeshCoreObsPayload
{
	uint32_t station_id;
	uint32_t timestamp;
	int16_t  temperature;    // 0.1 deg C
	uint8_t  humidity;       // %
	uint16_t pressure;      // 0.1 hPa
	uint16_t wind_speed;    // 0.1 m/s
	uint16_t wind_gust;     // 0.1 m/s
	uint16_t wind_dir;      // deg
	uint16_t precip_cml;    // 0.1 mm
	uint16_t solar_rad;     // 0.1 W/m^2 or solar mV
	uint8_t  uv_index;       // 0.1
	uint16_t battery_mv;    // mV
	uint16_t valid_mask;
};

enum class GrpPayloadType : uint8_t
{
	UNKNOWN = 0,
	SIGNED_STASENSORS_V1 = 0x01,
	LEGACY_OBSERVATION = 0x02
};

struct DecodedGrpData
{
	GrpPayloadType type;
	uint8_t signed_sta_sensors[105];
	size_t signed_sta_sensors_len;
	MeshCoreObsPayload legacy_obs;
};

typedef void (*AesEcbFn)(const uint8_t *input, uint8_t *output, size_t length, const uint8_t *key_16b, bool encrypt);
typedef void (*HmacSha256Fn)(const uint8_t *data, size_t data_len, const uint8_t *key_16b, uint8_t *out_mac_32b);

/**
 * Builds a MeshCore GRP_DATA frame from an observation payload.
 */
static inline size_t build_grp_data_frame(
	const MeshCoreObsPayload &payload,
	const uint8_t *psk,
	uint8_t channel_hash,
	AesEcbFn aes_fn,
	HmacSha256Fn hmac_fn,
	uint8_t *out_frame,
	size_t capacity)
{
	uint8_t plaintext[3 + sizeof(MeshCoreObsPayload)] = {};
	plaintext[0] = (uint8_t)(DATA_TYPE_WEATHERXM_OBSERVATION & 0xFF);
	plaintext[1] = (uint8_t)(DATA_TYPE_WEATHERXM_OBSERVATION >> 8);
	plaintext[2] = (uint8_t)sizeof(MeshCoreObsPayload);
	memcpy(&plaintext[3], &payload, sizeof(MeshCoreObsPayload));

	size_t padded_len = ((sizeof(plaintext) + CIPHER_BLOCK_SIZE - 1) / CIPHER_BLOCK_SIZE) * CIPHER_BLOCK_SIZE;
	size_t total_frame_len = 3 + CIPHER_MAC_SIZE + padded_len;
	if (total_frame_len > capacity) return 0;

	uint8_t padded[64] = {};
	memcpy(padded, plaintext, sizeof(plaintext));

	uint8_t ciphertext[64] = {};
	aes_fn(padded, ciphertext, padded_len, psk, true);

	uint8_t full_hmac[32] = {};
	hmac_fn(ciphertext, padded_len, psk, full_hmac);

	out_frame[0] = MESHCORE_HEADER_GRP_DATA;
	out_frame[1] = 0x00; // path_len = 0
	out_frame[2] = channel_hash;
	memcpy(&out_frame[3], full_hmac, CIPHER_MAC_SIZE);
	memcpy(&out_frame[3 + CIPHER_MAC_SIZE], ciphertext, padded_len);

	return total_frame_len;
}

/**
 * Builds a MeshCore GRP_DATA frame from a 105-byte signed station record.
 * Formatted with standard upstream GRP_DATA framing: [data_type: u16][data_len: u8][data...]
 */
static inline size_t build_signed_sensor_grp_data_frame(
	const uint8_t *signed_sta_sensors_105b,
	size_t signed_sta_sensors_len,
	const uint8_t *psk,
	uint8_t channel_hash,
	AesEcbFn aes_fn,
	HmacSha256Fn hmac_fn,
	uint8_t *out_frame,
	size_t capacity)
{
	if (signed_sta_sensors_105b == nullptr || signed_sta_sensors_len != 105)
	{
		return 0;
	}

	uint8_t plaintext[3 + 1 + 105] = {};
	plaintext[0] = (uint8_t)(DATA_TYPE_WEATHERXM_OBSERVATION & 0xFF);
	plaintext[1] = (uint8_t)(DATA_TYPE_WEATHERXM_OBSERVATION >> 8);
	plaintext[2] = (uint8_t)(1 + 105); // data_len = 106
	plaintext[3] = APP_SUBTYPE_SIGNED_STASENSORS_V1;
	memcpy(&plaintext[4], signed_sta_sensors_105b, 105);

	size_t padded_len = ((sizeof(plaintext) + CIPHER_BLOCK_SIZE - 1) / CIPHER_BLOCK_SIZE) * CIPHER_BLOCK_SIZE; // 112 bytes
	size_t total_frame_len = 3 + CIPHER_MAC_SIZE + padded_len; // 3 + 2 + 112 = 117 bytes
	if (total_frame_len > capacity) return 0;

	uint8_t padded[128] = {};
	memcpy(padded, plaintext, sizeof(plaintext));

	uint8_t ciphertext[128] = {};
	aes_fn(padded, ciphertext, padded_len, psk, true);

	uint8_t full_hmac[32] = {};
	hmac_fn(ciphertext, padded_len, psk, full_hmac);

	out_frame[0] = MESHCORE_HEADER_GRP_DATA;
	out_frame[1] = 0x00; // path_len = 0
	out_frame[2] = channel_hash;
	memcpy(&out_frame[3], full_hmac, CIPHER_MAC_SIZE);
	memcpy(&out_frame[3 + CIPHER_MAC_SIZE], ciphertext, padded_len);

	return total_frame_len;
}

/**
 * Decodes a MeshCore frame, verifying MAC and decrypting GRP_DATA into DecodedGrpData.
 */
static inline bool decode_grp_data_frame(
	const uint8_t *frame,
	size_t frame_len,
	const uint8_t *psk,
	AesEcbFn aes_fn,
	HmacSha256Fn hmac_fn,
	DecodedGrpData *out_decoded)
{
	if (frame == nullptr || frame_len < 3 + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE || out_decoded == nullptr)
	{
		return false;
	}

	uint8_t header = frame[0];
	uint8_t payload_type = (header >> 2) & 0x0F;
	if (payload_type != PAYLOAD_TYPE_GRP_DATA)
	{
		return false;
	}

	uint8_t path_len = frame[1];
	uint8_t path_hash_size = (path_len >> 6) + 1; // 1..4 bytes
	uint8_t path_hash_count = path_len & 63;      // 0..63 hops
	size_t path_byte_len = (size_t)path_hash_count * (size_t)path_hash_size;
	size_t header_len = 2 + path_byte_len; // header(1) + path_len(1) + path_bytes

	if (frame_len < header_len + 1 + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE)
	{
		return false;
	}

	uint8_t channel_hash = frame[header_len];
	(void)channel_hash;
	const uint8_t *rx_mac = &frame[header_len + 1];
	const uint8_t *ciphertext = &frame[header_len + 1 + CIPHER_MAC_SIZE];
	size_t cipher_len = frame_len - (header_len + 1 + CIPHER_MAC_SIZE);

	if (cipher_len % CIPHER_BLOCK_SIZE != 0 || cipher_len > 128)
	{
		return false;
	}

	// Verify HMAC-SHA256 over ciphertext
	uint8_t calculated_hmac[32] = {};
	hmac_fn(ciphertext, cipher_len, psk, calculated_hmac);
	if (memcmp(rx_mac, calculated_hmac, CIPHER_MAC_SIZE) != 0)
	{
		return false;
	}

	// Decrypt ciphertext
	uint8_t plaintext[128] = {};
	aes_fn(ciphertext, plaintext, cipher_len, psk, false);

	uint16_t data_type = (uint16_t)plaintext[0] | ((uint16_t)plaintext[1] << 8);
	uint8_t data_len = plaintext[2];

	if (data_type == DATA_TYPE_WEATHERXM_OBSERVATION)
	{
		if (data_len == 1 + 105 && plaintext[3] == APP_SUBTYPE_SIGNED_STASENSORS_V1)
		{
			out_decoded->type = GrpPayloadType::SIGNED_STASENSORS_V1;
			out_decoded->signed_sta_sensors_len = 105;
			memcpy(out_decoded->signed_sta_sensors, &plaintext[4], 105);
			return true;
		}
		else if (data_len == sizeof(MeshCoreObsPayload))
		{
			out_decoded->type = GrpPayloadType::LEGACY_OBSERVATION;
			memcpy(&out_decoded->legacy_obs, &plaintext[3], sizeof(MeshCoreObsPayload));
			return true;
		}
	}

	return false;
}

/**
 * Decodes a legacy observation MeshCore frame (backwards compatible helper).
 */
static inline bool decode_grp_data_frame(
	const uint8_t *frame,
	size_t frame_len,
	const uint8_t *psk,
	AesEcbFn aes_fn,
	HmacSha256Fn hmac_fn,
	MeshCoreObsPayload *out_payload)
{
	DecodedGrpData decoded = {};
	if (decode_grp_data_frame(frame, frame_len, psk, aes_fn, hmac_fn, &decoded))
	{
		if (decoded.type == GrpPayloadType::LEGACY_OBSERVATION && out_payload != nullptr)
		{
			*out_payload = decoded.legacy_obs;
			return true;
		}
	}
	return false;
}

} // namespace MeshCoreFrame

#endif // MESHCORE_FRAME_H
