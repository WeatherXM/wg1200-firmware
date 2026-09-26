/******************************************************************************
 * Golden-vector generator for Meshtastic and MeshCore frame encoders/decoders.
 *
 * Builds frames with fixed inputs using the exact firmware encoder
 * (main/ws/include/mesh_frame.h and main/ws/include/meshcore_frame.h)
 * and prints them as hex. verify.py then decodes them with an INDEPENDENT
 * implementation (Python + the cryptography library) and asserts the decoded
 * values match.
 *
 * Build and run:
 *   cd tools/mesh_frame_test && ./run.sh
 ******************************************************************************/

#include "../../main/ws/include/mesh_frame.h"
#include "../../main/ws/include/meshcore_frame.h"

#include <cstdio>
#include <cstring>
#include <cstdint>

/* ---------------------------------------------------------------------- */
/* Compact AES-128 ECB encrypt & decrypt, for the test only.               */
/* ---------------------------------------------------------------------- */

static const uint8_t SBOX[256] = {
	0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
	0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
	0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
	0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
	0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
	0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
	0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
	0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
	0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
	0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
	0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
	0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
	0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
	0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
	0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
	0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t RSBOX[256] = {
	0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
	0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
	0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
	0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
	0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
	0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
	0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
	0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
	0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
	0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
	0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
	0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
	0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
	0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
	0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
	0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00)); }

static inline uint8_t mul(uint8_t a, uint8_t b) {
	uint8_t p = 0;
	for (int i = 0; i < 8; i++) {
		if (b & 1) p ^= a;
		uint8_t hi = a & 0x80;
		a <<= 1;
		if (hi) a ^= 0x1b;
		b >>= 1;
	}
	return p;
}

static void aes128_expand(const uint8_t key[16], uint8_t rk[176])
{
	memcpy(rk, key, 16);
	uint8_t rcon = 1;
	for (int i = 16; i < 176; i += 4) {
		uint8_t t[4];
		memcpy(t, rk + i - 4, 4);
		if (i % 16 == 0) {
			uint8_t tmp = t[0];
			t[0] = (uint8_t)(SBOX[t[1]] ^ rcon);
			t[1] = SBOX[t[2]];
			t[2] = SBOX[t[3]];
			t[3] = SBOX[tmp];
			rcon = xtime(rcon);
		}
		for (int j = 0; j < 4; j++) rk[i + j] = (uint8_t)(rk[i - 16 + j] ^ t[j]);
	}
}

static void aes128_ecb(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
	uint8_t rk[176];
	aes128_expand(key, rk);

	uint8_t s[16];
	for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[i]);

	for (int round = 1; round <= 10; round++) {
		for (int i = 0; i < 16; i++) s[i] = SBOX[s[i]];

		/* ShiftRows (column-major state) */
		uint8_t t;
		t = s[1];  s[1]  = s[5];  s[5]  = s[9];  s[9]  = s[13]; s[13] = t;
		t = s[2];  s[2]  = s[10]; s[10] = t;     t     = s[6];  s[6]  = s[14]; s[14] = t;
		t = s[15]; s[15] = s[11]; s[11] = s[7];  s[7]  = s[3];  s[3]  = t;

		if (round != 10) {
			for (int c = 0; c < 4; c++) {
				uint8_t *p = s + 4 * c;
				uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
				uint8_t all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
				p[0] = (uint8_t)(a0 ^ all ^ xtime((uint8_t)(a0 ^ a1)));
				p[1] = (uint8_t)(a1 ^ all ^ xtime((uint8_t)(a1 ^ a2)));
				p[2] = (uint8_t)(a2 ^ all ^ xtime((uint8_t)(a2 ^ a3)));
				p[3] = (uint8_t)(a3 ^ all ^ xtime((uint8_t)(a3 ^ a0)));
			}
		}

		for (int i = 0; i < 16; i++) s[i] ^= rk[16 * round + i];
	}

	memcpy(out, s, 16);
}

static void aes128_ecb_decrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
	uint8_t rk[176];
	aes128_expand(key, rk);
	uint8_t s[16];
	for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[160 + i]);
	for (int round = 9; round >= 0; round--) {
		uint8_t t;
		t = s[13]; s[13] = s[9];  s[9]  = s[5];  s[5]  = s[1];  s[1] = t;
		t = s[2];  s[2]  = s[10]; s[10] = t;     t     = s[6];  s[6] = s[14]; s[14] = t;
		t = s[3];  s[3]  = s[7];  s[7]  = s[11]; s[11] = s[15]; s[15] = t;
		for (int i = 0; i < 16; i++) s[i] = RSBOX[s[i]];
		for (int i = 0; i < 16; i++) s[i] ^= rk[round * 16 + i];
		if (round > 0) {
			for (int c = 0; c < 4; c++) {
				uint8_t *p = s + 4 * c;
				uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
				p[0] = mul(a0, 0x0e) ^ mul(a1, 0x0b) ^ mul(a2, 0x0d) ^ mul(a3, 0x09);
				p[1] = mul(a0, 0x09) ^ mul(a1, 0x0e) ^ mul(a2, 0x0b) ^ mul(a3, 0x0d);
				p[2] = mul(a0, 0x0d) ^ mul(a1, 0x09) ^ mul(a2, 0x0e) ^ mul(a3, 0x0b);
				p[3] = mul(a0, 0x0b) ^ mul(a1, 0x0d) ^ mul(a2, 0x09) ^ mul(a3, 0x0e);
			}
		}
	}
	memcpy(out, s, 16);
}

static void test_aes_ecb_fn(const uint8_t *in, uint8_t *out, size_t len, const uint8_t *key, bool encrypt)
{
	for (size_t offset = 0; offset < len; offset += 16) {
		if (encrypt) aes128_ecb(key, in + offset, out + offset);
		else aes128_ecb_decrypt(key, in + offset, out + offset);
	}
}

/* ---------------------------------------------------------------------- */
/* Compact SHA-256 & HMAC-SHA256 implementation for the test harness       */
/* ---------------------------------------------------------------------- */

static const uint32_t K256[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static inline uint32_t ror32(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static void sha256_transform(uint32_t state[8], const uint8_t data[64]) {
	uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
	uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
	uint32_t w[64];
	for (int i = 0; i < 16; i++) {
		w[i] = ((uint32_t)data[i*4] << 24) | ((uint32_t)data[i*4+1] << 16) |
		       ((uint32_t)data[i*4+2] << 8) | ((uint32_t)data[i*4+3]);
	}
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ror32(w[i-15], 7) ^ ror32(w[i-15], 18) ^ (w[i-15] >> 3);
		uint32_t s1 = ror32(w[i-2], 17) ^ ror32(w[i-2], 19) ^ (w[i-2] >> 10);
		w[i] = w[i-16] + s0 + w[i-7] + s1;
	}
	for (int i = 0; i < 64; i++) {
		uint32_t s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
		uint32_t ch = (e & f) ^ ((~e) & g);
		uint32_t temp1 = h + s1 + ch + K256[i] + w[i];
		uint32_t s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t temp2 = s0 + maj;
		h = g; g = f; f = e; e = d + temp1;
		d = c; c = b; b = a; a = temp1 + temp2;
	}
	state[0] += a; state[1] += b; state[2] += c; state[3] += d;
	state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
	uint32_t state[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
	};
	uint8_t block[64];
	size_t i = 0;
	while (len >= 64) {
		sha256_transform(state, data + i);
		i += 64;
		len -= 64;
	}
	memset(block, 0, 64);
	memcpy(block, data + i, len);
	block[len] = 0x80;
	if (len >= 56) {
		sha256_transform(state, block);
		memset(block, 0, 64);
	}
	uint64_t total_bits = ((uint64_t)(i + len)) * 8;
	for (int j = 0; j < 8; j++) {
		block[63 - j] = (uint8_t)(total_bits >> (j * 8));
	}
	sha256_transform(state, block);
	for (int j = 0; j < 8; j++) {
		out[j*4]   = (uint8_t)(state[j] >> 24);
		out[j*4+1] = (uint8_t)(state[j] >> 16);
		out[j*4+2] = (uint8_t)(state[j] >> 8);
		out[j*4+3] = (uint8_t)(state[j]);
	}
}

static void test_hmac_sha256(const uint8_t *data, size_t data_len, const uint8_t *key_16b, uint8_t out_mac_32b[32]) {
	uint8_t k_pad[64];
	uint8_t inner_hash[32];
	uint8_t temp[64 + 32];
	uint8_t inner_input[256];

	memset(k_pad, 0x36, 64);
	for (int i = 0; i < 16; i++) k_pad[i] ^= key_16b[i];
	memcpy(inner_input, k_pad, 64);
	memcpy(inner_input + 64, data, data_len);
	sha256(inner_input, 64 + data_len, inner_hash);

	memset(k_pad, 0x5c, 64);
	for (int i = 0; i < 16; i++) k_pad[i] ^= key_16b[i];
	memcpy(temp, k_pad, 64);
	memcpy(temp + 64, inner_hash, 32);
	sha256(temp, 64 + 32, out_mac_32b);
}

/* ---------------------------------------------------------------------- */

static void print_hex(const char *label, const uint8_t *buf, size_t len)
{
	printf("%s ", label);
	for (size_t i = 0; i < len; i++) printf("%02x", buf[i]);
	printf("\n");
}

int main()
{
	/* Sanity-check the test's own AES against the FIPS-197 vector */
	{
		const uint8_t k[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
							   0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
		const uint8_t p[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
							   0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
		const uint8_t want[16] = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
								  0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
		uint8_t got[16];
		aes128_ecb(k, p, got);
		if (memcmp(got, want, 16) != 0) {
			fprintf(stderr, "FATAL: test AES-128 is broken\n");
			return 2;
		}
		uint8_t dec[16];
		aes128_ecb_decrypt(k, got, dec);
		if (memcmp(dec, p, 16) != 0) {
			fprintf(stderr, "FATAL: test AES-128 decrypt is broken\n");
			return 2;
		}
	}

	printf("channel_hash %02x\n", MeshFrame::default_channel_hash());

	const uint32_t NODE_NUM  = 0xa1b2c3d4u;
	const uint32_t PACKET_ID = 0x11223344u;
	const uint32_t EPOCH     = 1769472000u;

	/* ---- Telemetry frame ---- */
	MeshFrame::EnvValues env;
	memset(&env, 0, sizeof(env));
	env.has_temperature    = true; env.temperature    = 21.5f;
	env.has_humidity       = true; env.humidity       = 63.0f;
	env.has_pressure       = true; env.pressure       = 1013.25f;
	env.has_lux            = true; env.lux            = 12000.0f;
	env.has_wind_direction = true; env.wind_direction = 275;
	env.has_wind_speed     = true; env.wind_speed     = 4.2f;
	env.has_wind_gust      = true; env.wind_gust      = 7.8f;
	env.has_rainfall_1h    = true; env.rainfall_1h    = 1.25f;
	env.has_rainfall_24h   = true; env.rainfall_24h   = 5.50f;

	uint8_t env_buf[128];
	size_t env_len = MeshFrame::build_env_metrics(&env, env_buf, sizeof(env_buf));

	uint8_t telem_buf[160];
	size_t telem_len = MeshFrame::build_telemetry(EPOCH, env_buf, env_len, telem_buf, sizeof(telem_buf));

	uint8_t frame[MeshFrame::MAX_FRAME_LEN];
	size_t frame_len = MeshFrame::build_frame(
		MeshFrame::BROADCAST_ADDR, NODE_NUM, PACKET_ID,
		MeshFrame::default_channel_hash(), 0,
		MeshFrame::PORTNUM_TELEMETRY_APP, telem_buf, telem_len,
		MeshFrame::DEFAULT_PSK, aes128_ecb,
		frame, sizeof(frame));

	if (frame_len == 0) { fprintf(stderr, "FATAL: frame build failed\n"); return 2; }
	print_hex("telemetry_frame", frame, frame_len);

	/* ---- Device Metrics frame ---- */
	MeshFrame::DeviceMetricsValues dev;
	memset(&dev, 0, sizeof(dev));
	dev.has_battery_level = true; dev.battery_level = 101;
	dev.has_voltage       = true; dev.voltage       = 4.8f;
	dev.has_uptime        = true; dev.uptime_seconds = 3600;

	uint8_t dev_buf[64];
	size_t dev_len = MeshFrame::build_device_metrics(&dev, dev_buf, sizeof(dev_buf));

	uint8_t dev_telem_buf[96];
	size_t dev_telem_len = MeshFrame::build_telemetry_device_metrics(EPOCH, dev_buf, dev_len, dev_telem_buf, sizeof(dev_telem_buf));

	uint8_t frame_dev[MeshFrame::MAX_FRAME_LEN];
	size_t frame_dev_len = MeshFrame::build_frame(
		MeshFrame::BROADCAST_ADDR, NODE_NUM, PACKET_ID,
		MeshFrame::default_channel_hash(), 0,
		MeshFrame::PORTNUM_TELEMETRY_APP, dev_telem_buf, dev_telem_len,
		MeshFrame::DEFAULT_PSK, aes128_ecb,
		frame_dev, sizeof(frame_dev));

	if (frame_dev_len == 0) { fprintf(stderr, "FATAL: device_metrics frame build failed\n"); return 2; }
	print_hex("device_metrics_frame", frame_dev, frame_dev_len);

	/* ---- NodeInfo frame ---- */
	uint8_t dummy_pubkey[32];
	for (size_t i = 0; i < sizeof(dummy_pubkey); i++) dummy_pubkey[i] = (uint8_t)(i + 1);

	uint8_t user_buf[128];
	size_t user_len = MeshFrame::build_nodeinfo("!a1b2c3d4", "WeatherXM WG1200 C3D4", "GC3D",
												MeshFrame::HW_PRIVATE_HW, MeshFrame::ROLE_SENSOR,
												dummy_pubkey, sizeof(dummy_pubkey),
												user_buf, sizeof(user_buf));

	uint8_t frame2[MeshFrame::MAX_FRAME_LEN];
	size_t frame2_len = MeshFrame::build_frame(
		MeshFrame::BROADCAST_ADDR, NODE_NUM, PACKET_ID,
		MeshFrame::default_channel_hash(), 0,
		MeshFrame::PORTNUM_NODEINFO_APP, user_buf, user_len,
		MeshFrame::DEFAULT_PSK, aes128_ecb,
		frame2, sizeof(frame2));

	if (frame2_len == 0) { fprintf(stderr, "FATAL: nodeinfo frame build failed\n"); return 2; }
	print_hex("nodeinfo_frame", frame2, frame2_len);

	/* ---- Position frame ---- */
	MeshFrame::PositionValues pos;
	memset(&pos, 0, sizeof(pos));
	pos.lat  = 37.983810f;
	pos.lon  = 23.727539f;
	pos.time = EPOCH;

	uint8_t pos_buf[64];
	size_t pos_len = MeshFrame::build_position(&pos, pos_buf, sizeof(pos_buf));

	uint8_t frame3[MeshFrame::MAX_FRAME_LEN];
	size_t frame3_len = MeshFrame::build_frame(
		MeshFrame::BROADCAST_ADDR, NODE_NUM, PACKET_ID,
		MeshFrame::default_channel_hash(), 0,
		MeshFrame::PORTNUM_POSITION_APP, pos_buf, pos_len,
		MeshFrame::DEFAULT_PSK, aes128_ecb,
		frame3, sizeof(frame3));

	if (frame3_len == 0) { fprintf(stderr, "FATAL: position frame build failed\n"); return 2; }
	print_hex("position_frame", frame3, frame3_len);

	/* ---- Verify Meshtastic C++ decode ---- */
	MeshFrame::DecodedFrame dec_telem = {};
	if (!MeshFrame::decode_frame(frame, frame_len, MeshFrame::DEFAULT_PSK, aes128_ecb, &dec_telem) ||
		!dec_telem.has_env || dec_telem.from != NODE_NUM || dec_telem.portnum != MeshFrame::PORTNUM_TELEMETRY_APP ||
		dec_telem.env.wind_direction != 275) {
		fprintf(stderr, "FATAL: Meshtastic telemetry C++ decode failed\n");
		return 2;
	}

	MeshFrame::DecodedFrame dec_dev = {};
	if (!MeshFrame::decode_frame(frame_dev, frame_dev_len, MeshFrame::DEFAULT_PSK, aes128_ecb, &dec_dev) ||
		!dec_dev.has_device_metrics || dec_dev.device.battery_level != 101 || dec_dev.device.uptime_seconds != 3600) {
		fprintf(stderr, "FATAL: Meshtastic device metrics C++ decode failed\n");
		return 2;
	}

	MeshFrame::DecodedFrame dec_user = {};
	if (!MeshFrame::decode_frame(frame2, frame2_len, MeshFrame::DEFAULT_PSK, aes128_ecb, &dec_user) ||
		!dec_user.has_nodeinfo || strcmp(dec_user.short_name, "GC3D") != 0 || dec_user.hw_model != MeshFrame::HW_PRIVATE_HW) {
		fprintf(stderr, "FATAL: Meshtastic nodeinfo C++ decode failed\n");
		return 2;
	}

	MeshFrame::DecodedFrame dec_pos = {};
	if (!MeshFrame::decode_frame(frame3, frame3_len, MeshFrame::DEFAULT_PSK, aes128_ecb, &dec_pos) ||
		!dec_pos.has_position || dec_pos.portnum != MeshFrame::PORTNUM_POSITION_APP) {
		fprintf(stderr, "FATAL: Meshtastic position C++ decode failed\n");
		return 2;
	}

	/* ---- MeshCore GRP_DATA frame ---- */
	MeshCoreFrame::MeshCoreObsPayload mc_obs = {};
	mc_obs.station_id  = 0x12345678;
	mc_obs.timestamp   = EPOCH;
	mc_obs.temperature = 215; // 21.5 C
	mc_obs.humidity    = 63;  // 63 %
	mc_obs.pressure    = 10132; // 1013.2 hPa
	mc_obs.wind_speed  = 42;  // 4.2 m/s
	mc_obs.wind_gust   = 78;  // 7.8 m/s
	mc_obs.wind_dir    = 275; // 275 deg
	mc_obs.precip_cml  = 125; // 12.5 mm
	mc_obs.solar_rad   = 4500;// 450.0 W/m2
	mc_obs.uv_index    = 52;  // 5.2
	mc_obs.battery_mv  = 3300;// 3300 mV
	mc_obs.valid_mask  = 0x0FFF;

	uint8_t mc_frame[128];
	size_t mc_frame_len = MeshCoreFrame::build_grp_data_frame(
		mc_obs, MeshCoreFrame::DEFAULT_PSK, MeshCoreFrame::DEFAULT_CHANNEL_HASH,
		test_aes_ecb_fn, test_hmac_sha256, mc_frame, sizeof(mc_frame));

	if (mc_frame_len == 0) { fprintf(stderr, "FATAL: MeshCore frame build failed\n"); return 2; }
	print_hex("meshcore_grp_data_frame", mc_frame, mc_frame_len);

	// Verify MeshCore decode in C++
	MeshCoreFrame::MeshCoreObsPayload decoded_mc_obs = {};
	bool decode_ok = MeshCoreFrame::decode_grp_data_frame(
		mc_frame, mc_frame_len, MeshCoreFrame::DEFAULT_PSK,
		test_aes_ecb_fn, test_hmac_sha256, &decoded_mc_obs);

	if (!decode_ok || decoded_mc_obs.station_id != mc_obs.station_id ||
		decoded_mc_obs.temperature != mc_obs.temperature) {
		fprintf(stderr, "FATAL: MeshCore C++ decode verification failed\n");
		return 2;
	}

	/* ---- MeshCore Signed Sensor GRP_DATA frame ---- */
	uint8_t dummy_signed_105b[105];
	for (size_t i = 0; i < sizeof(dummy_signed_105b); i++) {
		dummy_signed_105b[i] = (uint8_t)(i + 1);
	}

	uint8_t mc_signed_frame[160];
	size_t mc_signed_frame_len = MeshCoreFrame::build_signed_sensor_grp_data_frame(
		dummy_signed_105b, sizeof(dummy_signed_105b),
		MeshCoreFrame::DEFAULT_PSK, MeshCoreFrame::DEFAULT_CHANNEL_HASH,
		test_aes_ecb_fn, test_hmac_sha256, mc_signed_frame, sizeof(mc_signed_frame));

	if (mc_signed_frame_len == 0) {
		fprintf(stderr, "FATAL: MeshCore signed sensor frame build failed\n");
		return 2;
	}
	print_hex("meshcore_signed_grp_data_frame", mc_signed_frame, mc_signed_frame_len);

	MeshCoreFrame::DecodedGrpData decoded_grp = {};
	bool decode_signed_ok = MeshCoreFrame::decode_grp_data_frame(
		mc_signed_frame, mc_signed_frame_len, MeshCoreFrame::DEFAULT_PSK,
		test_aes_ecb_fn, test_hmac_sha256, &decoded_grp);

	if (!decode_signed_ok || decoded_grp.type != MeshCoreFrame::GrpPayloadType::SIGNED_STASENSORS_V1 ||
		decoded_grp.signed_sta_sensors_len != 105 ||
		memcmp(decoded_grp.signed_sta_sensors, dummy_signed_105b, 105) != 0) {
		fprintf(stderr, "FATAL: MeshCore signed sensor C++ decode verification failed\n");
		return 2;
	}

	/* ---- WS1300 Downlink Command Structs (V1, V2, V3) ---- */
	struct RespHeader {
		uint8_t type;
		uint32_t station_id;
		uint16_t packet_id;
	} __attribute__((packed));

	struct RespCmdV1 {
		RespHeader header;
		bool acquire_gps: 1;
		bool reboot: 1;
		bool enable_logs: 1;
		uint8_t unused_1: 5;
		uint8_t tx_pwr: 5;
		uint8_t unused_2: 3;
		uint8_t tx_interval: 6;
		uint8_t unused_3: 2;
		uint8_t hmac[16];
	} __attribute__((packed));

	struct RespCmdV2 {
		RespHeader header;
		bool acquire_gps: 1;
		bool reboot: 1;
		bool enable_logs: 1;
		uint8_t unused_1: 5;
		uint8_t tx_pwr: 5;
		uint8_t unused_2: 3;
		uint8_t tx_interval: 6;
		uint8_t unused_3: 2;
		uint8_t meshtastic_interval_min;
		uint8_t hmac[16];
	} __attribute__((packed));

	struct RespCmdV3 {
		RespHeader header;
		bool acquire_gps: 1;
		bool reboot: 1;
		bool enable_logs: 1;
		bool meshcore_enabled: 1;
		bool meshtastic_enabled: 1;
		uint8_t unused_1: 3;
		uint8_t tx_pwr: 5;
		uint8_t unused_2: 3;
		uint8_t tx_interval: 6;
		uint8_t unused_3: 2;
		uint8_t meshtastic_interval_min;
		uint8_t meshcore_interval_min;
		uint8_t meshcore_profile_idx;
		uint8_t hmac[16];
	} __attribute__((packed));

	static_assert(sizeof(RespCmdV1) == 26, "RespCmdV1 must be exactly 26 bytes");
	static_assert(sizeof(RespCmdV2) == 27, "RespCmdV2 must be exactly 27 bytes");
	static_assert(sizeof(RespCmdV3) == 29, "RespCmdV3 must be exactly 29 bytes");
	printf("RespCmdV1_sizeof: %zu\n", sizeof(RespCmdV1));
	printf("RespCmdV2_sizeof: %zu\n", sizeof(RespCmdV2));
	printf("RespCmdV3_sizeof: %zu\n", sizeof(RespCmdV3));

	return 0;
}
