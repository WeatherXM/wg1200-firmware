#ifndef WS_1300_KEYS_H
#define WS_1300_KEYS_H

#include "ws_1300.h"

// Default Open Edition 44-byte pre-shared key (matches WS1300 Open Edition)
// In Open Edition, a development pre-shared key is used so that the WG1200 can
// authenticate with a matching WS1300 station without proprietary credentials.
// For WeatherXM Network Edition with production HMAC keys, configure your secure credentials.
static const u8 WS1300_OPEN_PRESH_KEY[44] = {
    0x57, 0x58, 0x4d, 0x2d, 0x4f, 0x50, 0x45, 0x4e, // "WXM-OPEN"
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
    0x21, 0x22, 0x23, 0x24
};

const size_t WS1300_HMAC_KEY_LEN = 44;

static inline bool get_ws1300_keys(Ws1300::ByteBuffer::RespHeader* resp_header, u8* gw_sn, u8* key_buff, size_t key_buff_len)
{
    if (key_buff_len < WS1300_HMAC_KEY_LEN) {
        return false;
    }
    memcpy(key_buff, WS1300_OPEN_PRESH_KEY, WS1300_HMAC_KEY_LEN);
    return true;
}

#endif // WS_1300_KEYS_H