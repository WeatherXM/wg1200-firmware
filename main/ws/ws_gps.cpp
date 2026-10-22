#include "ws/include/ws_gps.h"
#include "esp_log.h"
#include "ws/include/ws_common.h"
#include "freertos/FreeRTOS.h"
#include "ring_buff.h"
#include "const.h"
#include "time.h"
#include "utils.h"
#include "datetime_rtc.h"
#include "esp_rom_crc.h"
#include <math.h>
#include <stddef.h>
#include <string.h>
#include "settings.h"

namespace WsGps
{

EXT_RAM_BSS_ATTR RingBuff<GpsData, 3> _gps_ring_buff;

static const char TAG[] = "WsGps";

// GpsData contains FieldDesc pointers, so it cannot be retained directly
// across a firmware update: the pointed-to flash addresses may move. Keep the
// live object in normal BSS and retain only pointer-free scalar data in RTC.
static GpsData _last_gps_pack = {};

namespace
{

const uint32_t RTC_GPS_CACHE_MAGIC = 0x47505343; // "GPSC"
const uint16_t RTC_GPS_CACHE_VERSION = 1;

struct RtcGpsCache
{
	uint32_t magic;
	uint16_t version;
	uint16_t size;
	uint32_t sample_ts;
	uint32_t gps_ts;
	float lat;
	float lon;
	uint32_t checksum;
};

static RTC_NOINIT_ATTR RtcGpsCache _rtc_gps_cache;
static const uint32_t RTC_TZ_SEEDED_MAGIC = 0x47545A53U; // "GTZS"
static RTC_NOINIT_ATTR uint32_t _rtc_tz_seeded_magic;

static void maybe_seed_tz_from_gps(float lon)
{
	if(!isfinite(lon) || lon < -180.0f || lon > 180.0f)
	{
		return;
	}

	// Only auto-seed timezone from paired GPS once when tz_offset is still at
	// its unconfigured default (0), so manual user timezone overrides (e.g. DST)
	// and longitude jitter near 7.5 + 15*k degree boundaries never cause repeated NVS writes.
	if(Settings::get_tz_offset() != 0 || _rtc_tz_seeded_magic == RTC_TZ_SEEDED_MAGIC)
	{
		_rtc_tz_seeded_magic = RTC_TZ_SEEDED_MAGIC;
		return;
	}

	int auto_tz = (int)roundf(lon / 15.0f);
	if(auto_tz < -12) auto_tz = -12;
	if(auto_tz > 14) auto_tz = 14;

	_rtc_tz_seeded_magic = RTC_TZ_SEEDED_MAGIC;
	if(auto_tz != 0)
	{
		ESP_LOGI(TAG, "Seeding initial timezone offset from paired GPS location: UTC%+d (lon: %.4f)", auto_tz, lon);
		Settings::set_tz_offset(auto_tz);
		Settings::commit();
	}
}

uint32_t rtc_cache_checksum(const RtcGpsCache *cache)
{
	return esp_rom_crc32_le(UINT32_MAX, reinterpret_cast<const uint8_t*>(cache),
		offsetof(RtcGpsCache, checksum));
}

bool extract_required_fields(GpsData *gps_data, uint32_t *gps_ts, float *lat, float *lon)
{
	if(gps_data == NULL || gps_ts == NULL || lat == NULL || lon == NULL ||
		!gps_data->valid || gps_data->decoding_status != DecodingStatus::DECODED ||
		gps_data->decoding_type != DecodingType::DIRECT ||
		gps_data->field_pack.field_count == 0 ||
		gps_data->field_pack.field_count > MAX_FIELDS_IN_GPS_PACK ||
		gps_data->field_pack.field_count > WsFields::MAX_FIELDS_IN_PACK)
	{
		return false;
	}

	if(WsFields::get_field_val_from_pack(&gps_data->field_pack, WsFields::FieldType::GPS_TS, NULL, gps_ts) != WsCommon::RetRes::OK ||
		WsFields::get_field_val_from_pack(&gps_data->field_pack, WsFields::FieldType::GPS_LAT, NULL, lat) != WsCommon::RetRes::OK ||
		WsFields::get_field_val_from_pack(&gps_data->field_pack, WsFields::FieldType::GPS_LON, NULL, lon) != WsCommon::RetRes::OK)
	{
		return false;
	}

	return *gps_ts != 0 && isfinite(*lat) && isfinite(*lon) &&
		*lat >= -90.0f && *lat <= 90.0f && *lon >= -180.0f && *lon <= 180.0f;
}

template <typename T>
bool add_restored_field(WsFields::FieldPack *pack, const WsFields::FieldDesc *desc, T value)
{
	WsFields::Field field;
	if(WsFields::field_init(&field, desc) != WsCommon::RetRes::OK ||
		WsFields::set_field_val(&field, value) != WsCommon::RetRes::OK)
	{
		return false;
	}
	field.decode_status = WsCommon::RetRes::OK;
	return WsFields::add_field_to_pack(pack, &field) == WsCommon::RetRes::OK;
}

void clear_live_gps_data()
{
	memset(&_last_gps_pack, 0, sizeof(_last_gps_pack));
	WsFields::field_pack_init(&_last_gps_pack.field_pack);
}

void clear_retained_gps_data()
{
	memset(&_rtc_gps_cache, 0, sizeof(_rtc_gps_cache));
}

bool restore_retained_gps_data()
{
	if(_rtc_gps_cache.magic != RTC_GPS_CACHE_MAGIC ||
		_rtc_gps_cache.version != RTC_GPS_CACHE_VERSION ||
		_rtc_gps_cache.size != sizeof(RtcGpsCache) ||
		_rtc_gps_cache.checksum != rtc_cache_checksum(&_rtc_gps_cache) ||
		_rtc_gps_cache.sample_ts == 0 || _rtc_gps_cache.gps_ts == 0 ||
		!isfinite(_rtc_gps_cache.lat) || !isfinite(_rtc_gps_cache.lon) ||
		_rtc_gps_cache.lat < -90.0f || _rtc_gps_cache.lat > 90.0f ||
		_rtc_gps_cache.lon < -180.0f || _rtc_gps_cache.lon > 180.0f)
	{
		return false;
	}

	clear_live_gps_data();
	_last_gps_pack.valid = true;
	_last_gps_pack.decoding_status = DecodingStatus::DECODED;
	_last_gps_pack.decoding_type = DecodingType::DIRECT;
	_last_gps_pack.field_pack.ts = _rtc_gps_cache.sample_ts;

	if(!add_restored_field(&_last_gps_pack.field_pack, &FIELD_INFO_GPS_LAT, _rtc_gps_cache.lat) ||
		!add_restored_field(&_last_gps_pack.field_pack, &FIELD_INFO_GPS_LON, _rtc_gps_cache.lon) ||
		!add_restored_field(&_last_gps_pack.field_pack, &FIELD_INFO_GPS_TS, _rtc_gps_cache.gps_ts))
	{
		clear_live_gps_data();
		return false;
	}

	return true;
}

void persist_gps_data(GpsData *gps_data)
{
	uint32_t gps_ts = 0;
	float lat = 0.0f;
	float lon = 0.0f;
	if(!extract_required_fields(gps_data, &gps_ts, &lat, &lon) || gps_data->field_pack.ts == 0)
	{
		clear_retained_gps_data();
		return;
	}

	memset(&_rtc_gps_cache, 0, sizeof(_rtc_gps_cache));
	_rtc_gps_cache.magic = RTC_GPS_CACHE_MAGIC;
	_rtc_gps_cache.version = RTC_GPS_CACHE_VERSION;
	_rtc_gps_cache.size = sizeof(RtcGpsCache);
	_rtc_gps_cache.sample_ts = gps_data->field_pack.ts;
	_rtc_gps_cache.gps_ts = gps_ts;
	_rtc_gps_cache.lat = lat;
	_rtc_gps_cache.lon = lon;
	_rtc_gps_cache.checksum = rtc_cache_checksum(&_rtc_gps_cache);

	maybe_seed_tz_from_gps(lon);
}

}

/******************************************************************************
 * @brief Check that decoded GPS data contain safe, complete coordinates.
 ******************************************************************************/
bool gps_data_has_required_fields(GpsData *gps_data)
{
	uint32_t gps_ts = 0;
	float lat = 0.0f;
	float lon = 0.0f;
	return extract_required_fields(gps_data, &gps_ts, &lat, &lon);
}

/******************************************************************************
 * @brief Check that GPS data are complete and have a usable sample timestamp.
 ******************************************************************************/
bool gps_data_is_usable(GpsData *gps_data)
{
	return gps_data_has_required_fields(gps_data) && gps_data->field_pack.ts != 0;
}

/******************************************************************************
 * @brief Initialize
 * @retval void
 ******************************************************************************/
void init()
{
	clear_live_gps_data();
	if(!restore_retained_gps_data())
	{
		clear_retained_gps_data();
	}
	else
	{
		maybe_seed_tz_from_gps(_rtc_gps_cache.lon);
	}
}

/******************************************************************************
 * @brief Push GPS data to ring buffer
 * @param gps_data Data to push
 * @retval void
 ******************************************************************************/
void push_gps_data(GpsData *gps_data)
{
	_gps_ring_buff.lock();
	_gps_ring_buff.push(gps_data);	
	memcpy(&_last_gps_pack, gps_data, sizeof(GpsData));
	persist_gps_data(&_last_gps_pack);
	_gps_ring_buff.unlock();
}

/******************************************************************************
 * @brief Pop GPS data from ring buffer
 * @param gps_data Data to hold the pop
 * @retval void
 ******************************************************************************/
bool pop_gps_data(GpsData *gps_data)
{
	_gps_ring_buff.lock();
	bool ret_val = _gps_ring_buff.pop(gps_data);
	_gps_ring_buff.unlock();

	return ret_val;
}

/******************************************************************************
 * @brief Get last GPS data
 * @param gps_data Data to hold the last
 * @retval void
 ******************************************************************************/
void get_last_gps_data(GpsData *gps_data)
{
	_gps_ring_buff.lock(); // Using the mutex of buff for latest data too
	memcpy(gps_data, &_last_gps_pack, sizeof(GpsData));
	_gps_ring_buff.unlock();
}

/******************************************************************************
 * @brief Set last GPS data (without pushing to ring buffer)
 * @param gps_data Data to set as last
 * @retval void
 ******************************************************************************/
void set_last_gps_data(GpsData *gps_data)
{
	_gps_ring_buff.lock();
	memcpy(&_last_gps_pack, gps_data, sizeof(GpsData));
	_gps_ring_buff.unlock();
}

/******************************************************************************
 * @brief Check if there are data on buffer awaiting decode
 * @todo Check scenarios when TB implementation is done
 * @retval true if there are data on buffer awaiting decode
 ******************************************************************************/
bool gps_data_awaiting_decode()
{
	if(_gps_ring_buff.get_item_count() == 0)
	{
		return false;
	}
	GpsData *gps_data = NULL;
	_gps_ring_buff.peek_oldest(gps_data);
	if(gps_data == NULL)
	{
		ESP_LOGE(TAG, "_gps_ring_buff.peek_newest failed");
		return false;
	}
	if(gps_data->decoding_status == DecodingStatus::AWAITING
		&& gps_data->decoding_type == WsGps::DecodingType::INDIRECT)
	{
		return true;
	}
	return false;
}

/******************************************************************************
 * @brief Check if data are expired
 * @param gps_data Data to check
 * @retval true if GPS data are expired
 ******************************************************************************/
bool gps_data_expired(WsGps::GpsData *gps_data)
{
	if(!gps_data_is_usable(gps_data)) return true;

	if(!(DatetimeRtc::is_synced() && DatetimeRtc::cur_time_valid()))
	{
		return true;
	}

	time_t now = time(NULL);
	if(gps_data->field_pack.ts > now || now - gps_data->field_pack.ts > WS_GPS_EXPIRATION_SEC)
	{
		return true;
	}

	return false;
}

/******************************************************************************
 * @brief Calculate uint64_t H3 Index from GPS last data.
 * @return Cell id
 *****************************************************************************/
uint64_t calc_h3_index()
{
	WsGps::GpsData gps_data = {};
	WsGps::get_last_gps_data(&gps_data);

	if(!gps_data.valid)
	{
		ESP_LOGW(TAG, "Cell Id not calculated: gps data not valid");
		return H3_INDEX_INITIALIZATION_VAL;
	}

	if(gps_data_expired(&gps_data))
	{
		ESP_LOGE(TAG, "Cell Id not calculated: gps data expired");
		return H3_INDEX_INITIALIZATION_VAL;
	}
	
	H3Index h3_index;
	LatLng location;
	float lat;
	WsCommon::RetRes ret = WsFields::get_field_val_from_pack(&gps_data.field_pack, WsFields::FieldType::GPS_LAT, NULL, &lat);
	if(ret != WsCommon::RetRes::OK)
	{
		ESP_LOGE(TAG, "Cell Id Failed: GPS LAT Field not found in pack");
		return H3_INDEX_INITIALIZATION_VAL;
	}
	float lon;
	ret = WsFields::get_field_val_from_pack(&gps_data.field_pack, WsFields::FieldType::GPS_LON, NULL, &lon);
	if(ret != WsCommon::RetRes::OK)
	{
		ESP_LOGE(TAG, "Cell Id Failed: GPS LON Field not found in pack");
		return H3_INDEX_INITIALIZATION_VAL;
	}

	location.lat = degsToRads(lat);
	location.lng = degsToRads(lon);
	
	if (latLngToCell(&location, H3_CELL_RESOLUTION, &h3_index) != E_SUCCESS)
	{
		ESP_LOGE(TAG, "Cell Id Failed: calculation failed");
		return H3_INDEX_INITIALIZATION_VAL;
	}
	else
	{
		ESP_LOGI(TAG, "Cell Id Calculated: %" PRIx64 "", h3_index);
	}
	return (uint64_t) h3_index;
}

/******************************************************************************
 * @brief Get string from cell_id uint64
 * @param cell_id Cell_Id
 * @param cell_id_str_buff Buffer to hold the calculated string
 * @param cell_id_str_buff_len Buffer length to hold the calculated string
 * @return void
 ******************************************************************************/
void get_cell_id_str(uint64_t cell_id, char* cell_id_str_buff, uint16_t cell_id_str_buff_len)
{
	if(cell_id_str_buff_len < 17) return;
	snprintf(cell_id_str_buff, cell_id_str_buff_len, "%" PRIx64 "", cell_id);
}

/******************************************************************************
 * @brief Print GPS data
 * @param gps_data GPS data
 * @return void
 ******************************************************************************/
void print_gps_data(GpsData* gps_data)
{
	printf("-------------GPS Data----------------\n");
	printf("Decoding Status: ");
	switch(gps_data->decoding_status)
	{
		case WsGps::DecodingStatus::NONE:
			printf("NONE");
			break;
		case WsGps::DecodingStatus::DECODED:
			printf("DECODED");
			break;
		case WsGps::DecodingStatus::AWAITING:
			printf("AWAITING");
			break;
		case WsGps::DecodingStatus::FAILED:
			printf("FAILED");
			break;
	}
	printf("\n");
	printf("Decoding Type: ");
	switch(gps_data->decoding_type)
	{
		case WsGps::DecodingType::NONE:
			printf("NONE");
			break;
		case WsGps::DecodingType::DIRECT:
			printf("DIRECT");
			break;
		case WsGps::DecodingType::INDIRECT:
			printf("INDIRECT");
			break;
	}
	printf("\n");
	printf("Valid: %s\n", gps_data->valid ? "True" : "False");

	if(gps_data->decoding_type == WsGps::DecodingType::NONE) return;
	if(gps_data->decoding_status == WsGps::DecodingStatus::NONE) return;

	if(gps_data->decoding_status == WsGps::DecodingStatus::DECODED)
	{
		WsFields::print_field_pack(&gps_data->field_pack);
	}
	else
	{
		Utils::print_buff_hex(gps_data->raw_data.buff, gps_data->raw_data.buff_len);
		printf("--------------------------------------\n");
	}
}

}
