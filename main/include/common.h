#ifndef MINER_COMMON_H
#define MINER_COMMON_H

#include <inttypes.h>

//
// Common type shorthands
//
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
struct u24
{
	u8 data[3];
};

// Assert value of esp_err_t and return RetResult error on != ESP_OK
#define ASSERT_ESP_RET(x, ...) do { \
	if(x != ESP_OK) {\
		ESP_LOGE(TAG, __VA_ARGS__); \
		return RET_ERROR; \
	}\
}while(0);

// Assert value of RetResult and return error
#define ASSERT_RET(x, ret, ...) do { \
	if(x != RET_OK) {\
		ESP_LOGE(TAG, __VA_ARGS__); \
		return ret; \
	}\
}while(0);

// Item count of array
#define ARRAY_LENGTH(x) (sizeof(x) / sizeof((x)[0]))

#endif
