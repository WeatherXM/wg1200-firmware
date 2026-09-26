/**
 * Handle storage and aggregation of WS packets
 * All received packets are stored in the recv buffer after they pass CRC/checksum
 * integrity checks. Then aggregate_recv_buffer is used to aggregate the packet data
 * in the recv buffer into a single packet, calculate the timespan (oldest tstamp - newest),
 * precipitation rate and add into the aggregate data buffer. This data will be later submitted as is.
 */
#include <math.h>
#include <cmath>
using std::isfinite;
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "include/ws_data.h"
#include "const.h"
#include "utils.h"
#include "app_config.h"
#include "ring_buff.h"
#include <time.h>
#include "pressure.h"
#include "ws/include/ws_base.h"
#include "ws/include/ws_fields.h"
#include "settings.h"
#include "weather_logger.h"

namespace WsData
{
//
// Local funcs
//
void lock_recv_buffer();
void unlock_recv_buffer();
void lock_agg_buffer();
void unlock_agg_buffer();

//
// Local vars
//
/** Logger tag */
static const char TAG[] = "WsData";

/**
 * Decoded packets buffer. All packets after being received and CRC checked
 * are stored here until data submission */
EXT_RAM_BSS_ATTR WsFields::FieldPack _recv_buff[WS_AGGREGATE_BUFF_SIZE_ITEMS];

/** Items currently in receive buffer */
u16 _recv_buff_count = 0;

/** Oldest and newest timestamp of items currently in buffer. Used to calculate timespan when aggregating */
u32 _recv_oldest_tstamp = 0;
u32 _recv_newest_tstamp = 0;

/** Aggregate packets ring buffer. Every X mins, packets in receiver buffer
 * are aggregated and saved in this buffer ready for submission */
EXT_RAM_BSS_ATTR RingBuff<WsFields::FieldPack, WS_AGGREGATE_BUFF_SIZE_ITEMS> _aggregate_ring_buff;

/** Items in aggregate buffer */
u32 _aggregate_buff_count = 0;

/** Recv buffer semaphore */
SemaphoreHandle_t _sem_recv_buffer;
/** Recv buffer static semaphore buffer */
StaticSemaphore_t _sem_recv_buffer_buff;

/** Precipitation rate calculation parameters*/
float _init_tip_bucket_event_cml = 0.0f;
u32 _init_tip_bucket_event_ts = 0;
float _newest_bucket_tip_event_cml = 0.0f;
u32 _newest_bucket_tip_event_ts = 0;
float _oldest_bucket_tip_event_cml = 0.0f;
u32 _oldest_bucket_tip_event_ts = 0;

/** Daily precipitation tracking parameters (resets at 00:00 local time) */
static float _daily_baseline_cml = NAN;
static float _daily_accum_rollover = 0.0f;
static int32_t _daily_current_day_idx = -1;
static float _daily_precip_cached = 0.0f;

static DailyExtremes _daily_extremes;
static int32_t _daily_extremes_day_idx = -1;

static int32_t get_local_day_index()
{
	time_t now = time(NULL);
	if(now <= 0) return -1;
	int32_t tz_offset_sec = Settings::get_tz_offset() * 3600;
	time_t local_now = now + tz_offset_sec;
	return (int32_t)(local_now / 86400);
}

static void reset_precip_rate_state()
{
	_init_tip_bucket_event_cml = 0.0f;
	_init_tip_bucket_event_ts = 0;
	_newest_bucket_tip_event_cml = 0.0f;
	_newest_bucket_tip_event_ts = 0;
	_oldest_bucket_tip_event_cml = 0.0f;
	_oldest_bucket_tip_event_ts = 0;
}

/******************************************************************************
 * @brief Initialzie WsData
 * @retval ERROR failed to create receive buffer mutex
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes init()
{
	// Create mutex to lock recv buffer
	_sem_recv_buffer = xSemaphoreCreateMutexStatic(&_sem_recv_buffer_buff);
	if(_sem_recv_buffer == NULL)
	{
		Utils::die("WS Buffer init failed.");
		return WsCommon::RetRes::ERROR;
	}

	clear_recv();
	clear_aggregates();

	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Add packet to buffer
 * @param field_pack Field pack to be added to receive buffer
 * @retval ERROR receive buffer is full
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes add(WsFields::FieldPack *field_pack)
{
	// Add pressure and precip rate if not in pack
	WsFields::Field field_pressure;
	WsFields::field_init(&field_pressure, &WsFields::FIELD_INFO_WEATHER_PRESSURE);
	float press = Pressure::get_latest_val();
	if (press != -1.0f)
	{
		WsFields::set_field_val(&field_pressure, press);
		field_pressure.decode_status = WsCommon::RetRes::OK;
		WsFields::add_field_to_pack(field_pack, &field_pressure);		
	}
	else
	{
		ESP_LOGE(TAG, "Cannot read pressure");
		field_pressure.decode_status = WsCommon::RetRes::ERROR;
	}

	WsFields::Field field_precip_rate;
	WsFields::field_init(&field_precip_rate, &WsFields::FIELD_INFO_WEATHER_PRECIP_RATE);
	WsFields::set_field_val(&field_precip_rate, get_precip_rate());
	field_precip_rate.decode_status = WsCommon::RetRes::OK;
	WsFields::add_field_to_pack(field_pack, &field_precip_rate);	

	// Check if aggregation is needed
	if(_recv_buff_count > 0)
	{
		if((field_pack->ts / WS_DATA_AGGREGATE_INTERVAL_SEC) > _recv_buff[_recv_buff_count - 1].ts / WS_DATA_AGGREGATE_INTERVAL_SEC)
		{
			if(aggregate_now() != WsCommon::RetRes::OK)
			{
				ESP_LOGW(TAG, "FieldPack not aggregated.");
			}
			else
			{
				ESP_LOGI(TAG, "Aggregate buff items: %d", get_aggregate_count());
			}
		}
	}

	// Add packet to rcv buffer
	lock_recv_buffer();

	// Recv buffer full
	if(_recv_buff_count >= WS_DATA_RECEIVE_BUFF_ITEMS)
	{
		unlock_recv_buffer();
		return WsCommon::RetRes::ERROR;
	}

	// Add packet
	memcpy(&_recv_buff[_recv_buff_count], field_pack, sizeof(WsFields::FieldPack));
	_recv_buff_count++;

	unlock_recv_buffer();

	update_daily_extremes(field_pack);

	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Get number of items in receive buffer
 * @retval Number of items in receive buffer
 ******************************************************************************/
int get_recv_count()
{
	return _recv_buff_count;
}

/******************************************************************************
 * @brief Get number of items in aggregate ring buffer
 * @retval Number of items in aggregate buffer
 ******************************************************************************/
int get_aggregate_count()
{
	return _aggregate_ring_buff.get_item_count();
}

/******************************************************************************
 * @brief Pop item from aggregates ring buffer
 * @retval ERROR buffer is empty
 ******************************************************************************/
WsCommon::RetRes pop_aggregate_item(WsFields::FieldPack *output)
{
	lock_agg_buffer();
	bool ret = _aggregate_ring_buff.pop(output);
	unlock_agg_buffer();
	return ret ? WsCommon::RetRes::OK : WsCommon::RetRes::ERROR;
}

/******************************************************************************
 * @brief Push item to aggregates ring buffer
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes push_aggregate_item(WsFields::FieldPack *input)
{
	lock_agg_buffer();
	_aggregate_ring_buff.push(input);
	unlock_agg_buffer();
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Clear recv buffer. Caller responsible for locking.
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes clear_recv()
{
	memset(&_recv_buff, 0, sizeof(_recv_buff));
	_recv_buff_count = 0;
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Clear aggregates ring buffer.
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes clear_aggregates()
{
	lock_agg_buffer();
	_aggregate_ring_buff.clear();
	unlock_agg_buffer();
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Aggregate all packets from receive buffer and push a new field pack aggregate in ring buffer.
 * @retval OK
 ******************************************************************************/
WsCommon::RetRes aggregate_now()
{
	if(_recv_buff_count < 1)
	{
		return WsCommon::RetRes::ERROR;
	}

	//
	// Aggregate data
	//
	WsFields::FieldPack agg_field_pack = {};

	lock_recv_buffer();
	WsCommon::RetRes ret = WsFields::field_packs_aggregation(_recv_buff, _recv_buff_count, &agg_field_pack);
	if(ret != WsCommon::RetRes::OK)
	{
		ESP_LOGE(TAG, "field_packs_aggregation error: ");
		WsCommon::print_ret_val(ret);
	}
	// Round timestatmp Ceil or Floor to aggregation interval
	if(WsFields::WS_DATA_AGGREGATE_TS_ROUND == WsFields::WsDataAggrTsRound::CEIL)
	{
		agg_field_pack.ts = ((agg_field_pack.ts + WS_DATA_AGGREGATE_INTERVAL_SEC - 1) / WS_DATA_AGGREGATE_INTERVAL_SEC) * WS_DATA_AGGREGATE_INTERVAL_SEC;
	}
	else if(WsFields::WS_DATA_AGGREGATE_TS_ROUND == WsFields::WsDataAggrTsRound::FLOOR)
	{
		agg_field_pack.ts = (agg_field_pack.ts / WS_DATA_AGGREGATE_INTERVAL_SEC) * WS_DATA_AGGREGATE_INTERVAL_SEC;
	}
	

	WsFields::Field field_cell_id_pt_1;
	WsFields::field_init(&field_cell_id_pt_1, &WsGps::FIELD_INFO_GPS_CELL_ID_PT1);
	WsFields::Field field_cell_id_pt_2;
	WsFields::field_init(&field_cell_id_pt_2, &WsGps::FIELD_INFO_GPS_CELL_ID_PT2);

	// Cell_id packing, cell_id_pt1 = cell_id[0:3], cell_id_pt2 = cell_id[4:7]
	uint64_t cell_id = WsGps::calc_h3_index();
	uint8_t* cell_id_byte_ptr = (uint8_t*) &cell_id;
	uint32_t cell_id_pt1;
	uint32_t cell_id_pt2;
	memcpy(&cell_id_pt1, cell_id_byte_ptr, sizeof(cell_id_pt1));
	memcpy(&cell_id_pt2, &(cell_id_byte_ptr)[(sizeof(cell_id)/2)], sizeof(cell_id_pt2));
	field_cell_id_pt_1.decode_status = WsFields::set_field_val(&field_cell_id_pt_1, cell_id_pt1);
	field_cell_id_pt_2.decode_status = WsFields::set_field_val(&field_cell_id_pt_2, cell_id_pt2);
	WsFields::add_field_to_pack(&agg_field_pack, &field_cell_id_pt_1);
	WsFields::add_field_to_pack(&agg_field_pack, &field_cell_id_pt_2);

	push_aggregate_item(&agg_field_pack);
	clear_recv();
	unlock_recv_buffer();	

	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Print tip bucket event params (for debugging)
 ******************************************************************************/
void print_precip_rate_live_params()
{
	ESP_LOGW(TAG, "Tip bucket event detected");
	ESP_LOGW(TAG, "_oldest_bucket_tip_event_cml: %f", _oldest_bucket_tip_event_cml);
	ESP_LOGW(TAG, "_oldest_bucket_tip_event_ts: %d", _oldest_bucket_tip_event_ts);
	ESP_LOGW(TAG, "_newest_bucket_tip_event_cml: %f", _newest_bucket_tip_event_cml);
	ESP_LOGW(TAG, "_newest_bucket_tip_event_ts: %d", _newest_bucket_tip_event_ts);
}

/******************************************************************************
 * @brief Updates the parameters that are used to calculate precipitation rate and daily rain.
 * In each call, tip bucket events are detected and recorded from the provided packet.
 * Accumulation reset and initialization are handled just by calling the function.
 * @param field_pack Field pack holding current accumulated value and timestamp
 ******************************************************************************/
WsCommon::RetRes update_precip_rate_params(WsFields::FieldPack *field_pack)
{
	if(field_pack == NULL) return WsCommon::RetRes::ERROR_BAD_PARAMS;
	float precip_cml = 0;
	WsCommon::RetRes ret = WsFields::get_field_val_from_pack(field_pack, WsFields::FieldType::WEATHER_PRECIP_CML, NULL, &precip_cml);
	if(ret != WsCommon::RetRes::OK)
	{
		return ret;
	}
	if(!isfinite(precip_cml))
	{
		return WsCommon::RetRes::ERROR_BAD_RESULT;
	}

	// Update daily cumulative rain tracker with the latest valid counter
	update_daily_precip(precip_cml);

	uint32_t now_tick = Utils::get_tick_sec();

	// Recover cleanly if an older decoder or corrupted packet poisoned the
	// accumulator before the finite-value checks were added.
	if(!isfinite(_init_tip_bucket_event_cml) ||
	   !isfinite(_oldest_bucket_tip_event_cml) ||
	   !isfinite(_newest_bucket_tip_event_cml))
	{
		reset_precip_rate_state();
	}

	if(_init_tip_bucket_event_ts == 0)
	{
		_init_tip_bucket_event_cml = precip_cml;
		_init_tip_bucket_event_ts = now_tick;
		return WsCommon::RetRes::OK;
	}
	// When _init_tip_bucket_event_ts == 0 it needs initialization value
	// When data->precip_cml < _X_tip_bucket_event_cml the WS reset or the counter overflowed
	if(precip_cml < _init_tip_bucket_event_cml || precip_cml < _oldest_bucket_tip_event_cml || precip_cml < _newest_bucket_tip_event_cml)
	{
		reset_precip_rate_state();
		_init_tip_bucket_event_cml = precip_cml;
		_init_tip_bucket_event_ts = now_tick;
		return WsCommon::RetRes::OK;
	}

	if(precip_cml == _init_tip_bucket_event_cml) return WsCommon::RetRes::OK;

	if(_oldest_bucket_tip_event_ts == 0)
	{
		_oldest_bucket_tip_event_cml = precip_cml;
		_oldest_bucket_tip_event_ts = now_tick;
		_newest_bucket_tip_event_cml = 0.0f;
		_newest_bucket_tip_event_ts = 0;
		return WsCommon::RetRes::OK;
	}

	if(precip_cml == _oldest_bucket_tip_event_cml) return WsCommon::RetRes::OK;

	if(_newest_bucket_tip_event_ts == 0)
	{
		_newest_bucket_tip_event_cml = precip_cml;
		_newest_bucket_tip_event_ts = now_tick;
		return WsCommon::RetRes::OK;
	}

	if(precip_cml == _newest_bucket_tip_event_cml) return WsCommon::RetRes::OK;

	_oldest_bucket_tip_event_cml = _newest_bucket_tip_event_cml;
	_oldest_bucket_tip_event_ts = _newest_bucket_tip_event_ts;
	_newest_bucket_tip_event_cml = precip_cml;
	_newest_bucket_tip_event_ts = now_tick;
	return WsCommon::RetRes::OK;
}

/******************************************************************************
 * @brief Get current precipitation rate with decay in mm/hr
 * @return Precipitation rate
 ******************************************************************************/
float get_precip_rate()
{
	if(!isfinite(_init_tip_bucket_event_cml) ||
	   !isfinite(_oldest_bucket_tip_event_cml) ||
	   !isfinite(_newest_bucket_tip_event_cml))
	{
		reset_precip_rate_state();
		return 0.0f;
	}

	uint32_t now = Utils::get_tick_sec();

	// Case 1: First tip event recorded after initialization or dry baseline
	if(_oldest_bucket_tip_event_cml != 0.0f && _newest_bucket_tip_event_cml == 0.0f)
	{
		if(now < _oldest_bucket_tip_event_ts) return 0.0f;
		uint32_t elapsed_since_tip = now - _oldest_bucket_tip_event_ts;
		if(elapsed_since_tip >= (uint32_t)WS_DATA_PRECIP_RATE_DECAY_TIMEOUT_SEC) return 0.0f;

		float tip_delta = _oldest_bucket_tip_event_cml - _init_tip_bucket_event_cml;
		if (tip_delta <= 0.0f) tip_delta = 0.254f;

		uint32_t init_interval = (_oldest_bucket_tip_event_ts > _init_tip_bucket_event_ts)
			? (_oldest_bucket_tip_event_ts - _init_tip_bucket_event_ts) : 0;
		if (init_interval > 0 && init_interval < (uint32_t)WS_DATA_PRECIP_RATE_DECAY_TIMEOUT_SEC)
		{
			uint32_t elapsed_sec = init_interval + elapsed_since_tip;
			float rate = 3600.0f * tip_delta / elapsed_sec;
			return isfinite(rate) ? rate : 0.0f;
		}

		uint32_t effective_window = elapsed_since_tip > 120 ? elapsed_since_tip : 120;
		float rate = 3600.0f * tip_delta / effective_window;
		return isfinite(rate) ? rate : 0.0f;
	}

	// Case 2: At least two tip events recorded
	if(_oldest_bucket_tip_event_cml == 0.0f || _newest_bucket_tip_event_cml == 0.0f) return 0.0f;

	if(now < _newest_bucket_tip_event_ts) return 0.0f;
	if(now - _newest_bucket_tip_event_ts >= (uint32_t)WS_DATA_PRECIP_RATE_DECAY_TIMEOUT_SEC) return 0.0f;

	float bucket_tip_event_cml_delta = _newest_bucket_tip_event_cml - _oldest_bucket_tip_event_cml;
	uint32_t bucket_tip_event_ts_delta = _newest_bucket_tip_event_ts - _oldest_bucket_tip_event_ts;
	uint32_t current_time_delta = now - _newest_bucket_tip_event_ts;
	uint32_t elapsed_sec = bucket_tip_event_ts_delta + current_time_delta;
	if(elapsed_sec == 0 || bucket_tip_event_cml_delta < 0.0f) return 0.0f;

	float rate = 3600.0f * bucket_tip_event_cml_delta / elapsed_sec;
	return isfinite(rate) ? rate : 0.0f;
}

void reset_daily_precip()
{
	_daily_baseline_cml = NAN;
	_daily_accum_rollover = 0.0f;
	_daily_current_day_idx = -1;
	_daily_precip_cached = 0.0f;
}

void update_daily_precip(float cml_val)
{
	if(!isfinite(cml_val)) return;

	int32_t day_idx = get_local_day_index();
	if(day_idx < 0) return;

	if(_daily_current_day_idx != day_idx)
	{
		_daily_current_day_idx = day_idx;
		_daily_baseline_cml = cml_val;
		_daily_accum_rollover = 0.0f;
		_daily_precip_cached = 0.0f;
		ESP_LOGI(TAG, "Daily rain counter initialized for local day index %ld (baseline: %.2f mm)",
			(long)day_idx, cml_val);
		return;
	}

	if(isnan(_daily_baseline_cml))
	{
		_daily_baseline_cml = cml_val;
		_daily_accum_rollover = 0.0f;
		_daily_precip_cached = 0.0f;
		return;
	}

	if(cml_val >= _daily_baseline_cml)
	{
		_daily_precip_cached = (cml_val - _daily_baseline_cml) + _daily_accum_rollover;
	}
	else
	{
		// Station reset or counter wrap
		_daily_accum_rollover += cml_val;
		_daily_baseline_cml = cml_val;
		_daily_precip_cached = _daily_accum_rollover;
	}
}

float get_daily_precip()
{
	int32_t day_idx = get_local_day_index();
	if(day_idx >= 0 && _daily_current_day_idx != -1 && _daily_current_day_idx != day_idx)
	{
		_daily_current_day_idx = day_idx;
		_daily_accum_rollover = 0.0f;
		_daily_precip_cached = 0.0f;
	}

	// Fallback to WeatherLogger aggregates if daily cache is zero after reboot
	if(_daily_precip_cached == 0.0f && day_idx >= 0)
	{
		time_t now = time(NULL);
		int32_t tz_offset_sec = Settings::get_tz_offset() * 3600;
		time_t local_now = now + tz_offset_sec;
		time_t local_midnight = local_now - (local_now % 86400);
		time_t utc_midnight = local_midnight - tz_offset_sec;

		auto aggs = WeatherLogger::get_aggregates();
		float sum = 0.0f;
		bool found = false;
		for(const auto &agg : aggs)
		{
			if(agg.valid && agg.timestamp >= utc_midnight && !isnan(agg.precip_accum))
			{
				sum += agg.precip_accum;
				found = true;
			}
		}
		if(found)
		{
			return sum;
		}
	}

	return _daily_precip_cached;
}

void reset_daily_extremes()
{
	_daily_extremes = DailyExtremes();
	_daily_extremes_day_idx = -1;
}

void update_daily_extremes(const WsFields::FieldPack* field_pack)
{
	if(field_pack == NULL) return;

	int32_t day_idx = get_local_day_index();
	if(day_idx >= 0)
	{
		if(_daily_extremes_day_idx != day_idx)
		{
			_daily_extremes = DailyExtremes();
			_daily_extremes_day_idx = day_idx;
		}
	}

	float temp = 0.0f;
	if(WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(field_pack), WsFields::FieldType::WEATHER_TEMP, NULL, &temp) == WsCommon::RetRes::OK && isfinite(temp))
	{
		if(isnan(_daily_extremes.temp_min) || temp < _daily_extremes.temp_min) _daily_extremes.temp_min = temp;
		if(isnan(_daily_extremes.temp_max) || temp > _daily_extremes.temp_max) _daily_extremes.temp_max = temp;
		_daily_extremes.has_data = true;
	}

	float press = 0.0f;
	if(WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(field_pack), WsFields::FieldType::WEATHER_PRESSURE, NULL, &press) == WsCommon::RetRes::OK && isfinite(press))
	{
		if(isnan(_daily_extremes.pressure_min) || press < _daily_extremes.pressure_min) _daily_extremes.pressure_min = press;
		if(isnan(_daily_extremes.pressure_max) || press > _daily_extremes.pressure_max) _daily_extremes.pressure_max = press;
		_daily_extremes.has_data = true;
	}

	float hum = 0.0f;
	if(WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(field_pack), WsFields::FieldType::WEATHER_HUM, NULL, &hum) == WsCommon::RetRes::OK && isfinite(hum))
	{
		if(isnan(_daily_extremes.hum_min) || hum < _daily_extremes.hum_min) _daily_extremes.hum_min = hum;
		if(isnan(_daily_extremes.hum_max) || hum > _daily_extremes.hum_max) _daily_extremes.hum_max = hum;
		_daily_extremes.has_data = true;
	}

	float gust = 0.0f;
	if(WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(field_pack), WsFields::FieldType::WEATHER_WIND_GUST, NULL, &gust) == WsCommon::RetRes::OK && isfinite(gust))
	{
		if(isnan(_daily_extremes.wind_gust_max) || gust > _daily_extremes.wind_gust_max) _daily_extremes.wind_gust_max = gust;
		_daily_extremes.has_data = true;
	}

	float rate = 0.0f;
	if(WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(field_pack), WsFields::FieldType::WEATHER_PRECIP_RATE, NULL, &rate) == WsCommon::RetRes::OK && isfinite(rate))
	{
		if(isnan(_daily_extremes.precip_rate_max) || rate > _daily_extremes.precip_rate_max) _daily_extremes.precip_rate_max = rate;
	}
	float live_rate = get_precip_rate();
	if(isfinite(live_rate))
	{
		if(isnan(_daily_extremes.precip_rate_max) || live_rate > _daily_extremes.precip_rate_max) _daily_extremes.precip_rate_max = live_rate;
	}

	uint8_t uv = 0;
	if(WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(field_pack), WsFields::FieldType::WEATHER_UV_INDEX, NULL, &uv) == WsCommon::RetRes::OK)
	{
		if(uv > _daily_extremes.uv_max) _daily_extremes.uv_max = uv;
		_daily_extremes.has_data = true;
	}

	float solar = 0.0f;
	if(WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(field_pack), WsFields::FieldType::WEATHER_SOLAR_RAD, NULL, &solar) == WsCommon::RetRes::OK && isfinite(solar))
	{
		if(isnan(_daily_extremes.solar_rad_max) || solar > _daily_extremes.solar_rad_max) _daily_extremes.solar_rad_max = solar;
		_daily_extremes.has_data = true;
	}
}

DailyExtremes get_daily_extremes()
{
	int32_t day_idx = get_local_day_index();
	if(day_idx >= 0 && _daily_extremes_day_idx != -1 && _daily_extremes_day_idx != day_idx)
	{
		_daily_extremes = DailyExtremes();
		_daily_extremes_day_idx = day_idx;
	}

	// Fallback to WeatherLogger aggregates if no extremes recorded yet today (e.g. fresh reboot)
	if(!_daily_extremes.has_data && day_idx >= 0)
	{
		time_t now = time(NULL);
		int32_t tz_offset_sec = Settings::get_tz_offset() * 3600;
		time_t local_now = now + tz_offset_sec;
		time_t local_midnight = local_now - (local_now % 86400);
		time_t utc_midnight = local_midnight - tz_offset_sec;

		auto aggs = WeatherLogger::get_aggregates();
		for(const auto &agg : aggs)
		{
			if(agg.valid && agg.timestamp >= utc_midnight)
			{
				if(!isnan(agg.temp_avg))
				{
					if(isnan(_daily_extremes.temp_min) || agg.temp_avg < _daily_extremes.temp_min) _daily_extremes.temp_min = agg.temp_avg;
					if(isnan(_daily_extremes.temp_max) || agg.temp_avg > _daily_extremes.temp_max) _daily_extremes.temp_max = agg.temp_avg;
					_daily_extremes.has_data = true;
				}
				if(!isnan(agg.pressure_avg))
				{
					if(isnan(_daily_extremes.pressure_min) || agg.pressure_avg < _daily_extremes.pressure_min) _daily_extremes.pressure_min = agg.pressure_avg;
					if(isnan(_daily_extremes.pressure_max) || agg.pressure_avg > _daily_extremes.pressure_max) _daily_extremes.pressure_max = agg.pressure_avg;
					_daily_extremes.has_data = true;
				}
				if(!isnan(agg.hum_avg))
				{
					if(isnan(_daily_extremes.hum_min) || agg.hum_avg < _daily_extremes.hum_min) _daily_extremes.hum_min = agg.hum_avg;
					if(isnan(_daily_extremes.hum_max) || agg.hum_avg > _daily_extremes.hum_max) _daily_extremes.hum_max = agg.hum_avg;
					_daily_extremes.has_data = true;
				}
				if(!isnan(agg.wind_gust_max))
				{
					if(isnan(_daily_extremes.wind_gust_max) || agg.wind_gust_max > _daily_extremes.wind_gust_max) _daily_extremes.wind_gust_max = agg.wind_gust_max;
					_daily_extremes.has_data = true;
				}
				if(!isnan(agg.precip_avg))
				{
					if(isnan(_daily_extremes.precip_rate_max) || agg.precip_avg > _daily_extremes.precip_rate_max) _daily_extremes.precip_rate_max = agg.precip_avg;
				}
				if(!isnan(agg.solar_rad_avg))
				{
					if(isnan(_daily_extremes.solar_rad_max) || agg.solar_rad_avg > _daily_extremes.solar_rad_max) _daily_extremes.solar_rad_max = agg.solar_rad_avg;
					_daily_extremes.has_data = true;
				}
			}
		}
	}

	return _daily_extremes;
}

/******************************************************************************
 * @brief Lock receive buffer
 ******************************************************************************/
void lock_recv_buffer()
{
	xSemaphoreTake(_sem_recv_buffer, portMAX_DELAY);
}

/******************************************************************************
 * @brief Unlock receive buffer
 ******************************************************************************/
void unlock_recv_buffer()
{
	xSemaphoreGive(_sem_recv_buffer);
}

/******************************************************************************
 * @brief Lock aggregate buffer
 ******************************************************************************/
void lock_agg_buffer()
{
	_aggregate_ring_buff.lock();
}

/******************************************************************************
* @brief Unlock aggregate buffer
******************************************************************************/
void unlock_agg_buffer()
{
	_aggregate_ring_buff.unlock();
}

/******************************************************************************
 * @brief Get last received packet from buffer.
 * Used for debugging
 ******************************************************************************/
WsCommon::RetRes get_last_packet(WsFields::FieldPack* field_pack)
{
	if(_recv_buff_count < 1 || field_pack == NULL)
		return WsCommon::RetRes::ERROR;

	lock_recv_buffer();

	memcpy(field_pack, &_recv_buff[_recv_buff_count - 1], sizeof(WsFields::FieldPack));

	unlock_recv_buffer();

	return WsCommon::RetRes::OK;
}

}
