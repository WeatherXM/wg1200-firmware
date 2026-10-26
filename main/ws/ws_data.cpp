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
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_attr.h"
#include "esp_memory_utils.h"
#include "esp_rom_crc.h"

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
static uint32_t _precip_rate_station_id = 0;

// Upper physical bound for valid precipitation rate (mm/h) and single-packet tip delta (mm).
// Protects against cross-station cumulative counter jumps and post-boot short-interval spikes.
static constexpr float MAX_VALID_PRECIP_RATE_MM_H = 300.0f;
static constexpr float MAX_SINGLE_PACKET_PRECIP_DELTA_MM = 25.4f;

/** Daily precipitation tracking parameters (resets at 00:00 local time) */
static float _daily_baseline_cml = NAN;
static float _daily_accum_rollover = 0.0f;
static int32_t _daily_current_day_idx = -1;
static float _daily_precip_cached = 0.0f;
static time_t _daily_baseline_ts = 0;
static uint32_t _daily_precip_station_id = 0;

static DailyExtremes _daily_extremes;
static int32_t _daily_extremes_day_idx = -1;

static bool _nvs_daily_loaded = false;
static uint32_t _last_nvs_save_tick = 0;

static constexpr uint32_t DAILY_STATE_MAGIC = 0x57584432U; // "WXD2"
static constexpr const char *NVS_DAILY_KEY = "wx_daily";
// NVS cold-power-loss checkpoint interval (6 hours). Primary retention across
// warm reboots, OTA updates, and USB flashes uses zero-wear RTC_NOINIT_ATTR SRAM.
static constexpr uint32_t NVS_DAILY_CHECKPOINT_INTERVAL_SEC = 6 * 3600;

struct PersistedDailyState
{
	uint32_t magic = DAILY_STATE_MAGIC;
	uint32_t ws_id = 0;
	int32_t precip_day_idx = -1;
	float baseline_cml = NAN;
	float accum_rollover = 0.0f;
	float precip_cached = 0.0f;
	int64_t baseline_ts = 0;
	int32_t extremes_day_idx = -1;
	DailyExtremes extremes;
};

// Pure POD wrapper (no default member initializers) so startup code never
// zeroes or overwrites RTC_NOINIT_ATTR memory across warm resets / OTA / flashing.
struct RtcDailyStateCache
{
	uint8_t state_bytes[sizeof(PersistedDailyState)];
	uint32_t checksum;
};

static RTC_NOINIT_ATTR RtcDailyStateCache _rtc_daily_cache;
static PersistedDailyState _last_nvs_state;
static bool _last_nvs_state_valid = false;
static bool _nvs_force_checkpoint_pending = false;

static int32_t get_local_day_index()
{
	time_t now = time(NULL);
	if(now <= 86400) return -1;
	int32_t tz_offset_sec = Settings::get_tz_offset() * 3600;
	time_t local_now = now + tz_offset_sec;
	return (int32_t)(local_now / 86400);
}

static time_t get_local_day_utc_midnight()
{
	time_t now = time(NULL);
	if(now <= 86400) return 0;
	int32_t tz_offset_sec = Settings::get_tz_offset() * 3600;
	time_t local_now = now + tz_offset_sec;
	time_t local_midnight = local_now - (local_now % 86400);
	return local_midnight - tz_offset_sec;
}

static void build_persisted_daily_state(PersistedDailyState *state)
{
	memset(state, 0, sizeof(*state));
	state->magic = DAILY_STATE_MAGIC;
	state->ws_id = (_daily_precip_station_id != 0) ? _daily_precip_station_id : Settings::get_ws_id();
	state->precip_day_idx = _daily_current_day_idx;
	state->baseline_cml = _daily_baseline_cml;
	state->accum_rollover = _daily_accum_rollover;
	state->precip_cached = _daily_precip_cached;
	state->baseline_ts = (int64_t)_daily_baseline_ts;
	state->extremes_day_idx = _daily_extremes_day_idx;
	state->extremes = _daily_extremes;
}

static void save_daily_state_to_rtc(const PersistedDailyState &state)
{
	memcpy(_rtc_daily_cache.state_bytes, &state, sizeof(state));
	_rtc_daily_cache.checksum = esp_rom_crc32_le(
		UINT32_MAX, _rtc_daily_cache.state_bytes, sizeof(_rtc_daily_cache.state_bytes));
}

static bool load_daily_state_from_rtc(PersistedDailyState *out_state)
{
	uint32_t expected_crc = esp_rom_crc32_le(
		UINT32_MAX, _rtc_daily_cache.state_bytes, sizeof(_rtc_daily_cache.state_bytes));
	if(expected_crc != _rtc_daily_cache.checksum)
	{
		return false;
	}
	memcpy(out_state, _rtc_daily_cache.state_bytes, sizeof(*out_state));
	return out_state->magic == DAILY_STATE_MAGIC;
}

static void save_daily_state_to_nvs(bool force)
{
	PersistedDailyState state;
	build_persisted_daily_state(&state);

	// Always retain immediately in RTC_NOINIT_ATTR SRAM (zero flash wear)
	save_daily_state_to_rtc(state);

	if(force)
	{
		_nvs_force_checkpoint_pending = true;
	}

	// Never invoke NVS write/commit from a task whose stack is in external PSRAM
	// (e.g. ui_update_task or sd_writer_task), as SPI flash cache-disable checks
	// abort with esp_task_stack_is_sane_cache_disabled. Any pending forced
	// checkpoint will be flushed on the next call from an internal-RAM task.
	if(!esp_ptr_internal(&state))
	{
		return;
	}

	const bool effective_force = force || _nvs_force_checkpoint_pending;
	uint32_t now_tick = Utils::get_tick_sec();
	if(!effective_force && _last_nvs_save_tick != 0 &&
	   (now_tick - _last_nvs_save_tick) < NVS_DAILY_CHECKPOINT_INTERVAL_SEC)
	{
		return;
	}

	// Skip NVS write if state bytes are identical to the last NVS-committed checkpoint
	if(_last_nvs_state_valid && memcmp(&state, &_last_nvs_state, sizeof(state)) == 0)
	{
		_last_nvs_save_tick = (now_tick != 0) ? now_tick : 1;
		_nvs_force_checkpoint_pending = false;
		return;
	}

	nvs_handle_t handle;
	if(nvs_open("settings", NVS_READWRITE, &handle) != ESP_OK)
	{
		return;
	}

	if(nvs_set_blob(handle, NVS_DAILY_KEY, &state, sizeof(state)) == ESP_OK)
	{
		if(nvs_commit(handle) == ESP_OK)
		{
			_last_nvs_state = state;
			_last_nvs_state_valid = true;
			_last_nvs_save_tick = (now_tick != 0) ? now_tick : 1;
			_nvs_force_checkpoint_pending = false;
		}
	}
	nvs_close(handle);
}

static void ensure_daily_state_loaded()
{
	if(_nvs_daily_loaded) return;
	_nvs_daily_loaded = true;

	PersistedDailyState nvs_state;
	memset(&nvs_state, 0, sizeof(nvs_state));
	bool has_nvs = false;

	nvs_handle_t handle;
	if(nvs_open("settings", NVS_READONLY, &handle) == ESP_OK)
	{
		size_t len = sizeof(nvs_state);
		esp_err_t err = nvs_get_blob(handle, NVS_DAILY_KEY, &nvs_state, &len);
		nvs_close(handle);
		if(err == ESP_OK && len == sizeof(nvs_state) && nvs_state.magic == DAILY_STATE_MAGIC)
		{
			has_nvs = true;
			_last_nvs_state = nvs_state;
			_last_nvs_state_valid = true;
			_last_nvs_save_tick = Utils::get_tick_sec();
			if(_last_nvs_save_tick == 0) _last_nvs_save_tick = 1;
		}
	}

	PersistedDailyState rtc_state;
	memset(&rtc_state, 0, sizeof(rtc_state));
	bool has_rtc = load_daily_state_from_rtc(&rtc_state);

	if(!has_rtc && !has_nvs)
	{
		_last_nvs_save_tick = Utils::get_tick_sec();
		if(_last_nvs_save_tick == 0) _last_nvs_save_tick = 1;
		return;
	}

	// Prefer RTC_NOINIT_ATTR state across warm reboots / OTA / USB flashes;
	// fall back to NVS checkpoint after a cold power cycle.
	const PersistedDailyState &state = has_rtc ? rtc_state : nvs_state;
	const char *source_label = has_rtc ? "RTC RAM" : "NVS";

	uint32_t paired_id = Settings::get_ws_id();
	if(paired_id != 0 && state.ws_id != 0 && state.ws_id != paired_id)
	{
		ESP_LOGI(TAG, "Ignoring persisted daily state (%s) for old station %lu (current %lu)",
			source_label, (unsigned long)state.ws_id, (unsigned long)paired_id);
		return;
	}

	int32_t cur_day = get_local_day_index();
	if(state.precip_day_idx >= 0 && (cur_day < 0 || state.precip_day_idx == cur_day))
	{
		_daily_current_day_idx = state.precip_day_idx;
		_daily_baseline_cml = state.baseline_cml;
		_daily_accum_rollover = isfinite(state.accum_rollover) ? state.accum_rollover : 0.0f;
		_daily_precip_cached = isfinite(state.precip_cached) ? state.precip_cached : 0.0f;
		_daily_baseline_ts = (time_t)state.baseline_ts;
		_daily_precip_station_id = state.ws_id;
		ESP_LOGI(TAG, "Restored daily rain state from %s for day %ld: baseline=%.2f mm, cached=%.2f mm",
			source_label, (long)_daily_current_day_idx, _daily_baseline_cml, _daily_precip_cached);
	}

	if(state.extremes_day_idx >= 0 && (cur_day < 0 || state.extremes_day_idx == cur_day))
	{
		_daily_extremes_day_idx = state.extremes_day_idx;
		_daily_extremes = state.extremes;
		if(!isnan(_daily_extremes.precip_rate_max) &&
		   (_daily_extremes.precip_rate_max < 0.0f || _daily_extremes.precip_rate_max > MAX_VALID_PRECIP_RATE_MM_H))
		{
			_daily_extremes.precip_rate_max = NAN;
		}
		ESP_LOGI(TAG, "Restored daily extremes from %s for day %ld", source_label, (long)_daily_extremes_day_idx);
	}

	// Ensure RTC cache is seeded if we restored from NVS on a cold boot
	if(!has_rtc && has_nvs)
	{
		save_daily_state_to_rtc(nvs_state);
		if(_last_nvs_save_tick == 0) _last_nvs_save_tick = 1;
	}
	else if(has_rtc && _last_nvs_save_tick == 0)
	{
		_last_nvs_save_tick = Utils::get_tick_sec();
		if(_last_nvs_save_tick == 0) _last_nvs_save_tick = 1;
	}
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
	ensure_daily_state_loaded();

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
	if(!isfinite(precip_cml) || precip_cml < 0.0f)
	{
		return WsCommon::RetRes::ERROR_BAD_RESULT;
	}

	uint32_t pkt_sta_id = 0;
	WsFields::get_field_val_from_pack(field_pack, WsFields::FieldType::META_ID, NULL, &pkt_sta_id);

	if(pkt_sta_id != 0 && _precip_rate_station_id != 0 && pkt_sta_id != _precip_rate_station_id)
	{
		ESP_LOGW(TAG, "Station ID changed (0x%08lX -> 0x%08lX); resetting precip rate state",
			(unsigned long)_precip_rate_station_id, (unsigned long)pkt_sta_id);
		reset_precip_rate_state();
	}
	if(pkt_sta_id != 0)
	{
		_precip_rate_station_id = pkt_sta_id;
	}

	if(pkt_sta_id != 0 && _daily_precip_station_id != 0 && pkt_sta_id != _daily_precip_station_id)
	{
		ESP_LOGW(TAG, "Station ID changed (0x%08lX -> 0x%08lX); re-anchoring daily rain baseline to %.2f mm (preserving %.2f mm)",
			(unsigned long)_daily_precip_station_id, (unsigned long)pkt_sta_id, precip_cml, _daily_precip_cached);
		_daily_accum_rollover = _daily_precip_cached;
		_daily_baseline_cml = precip_cml;
		_daily_precip_station_id = pkt_sta_id;
		save_daily_state_to_nvs(true);
	}
	else if(pkt_sta_id != 0)
	{
		_daily_precip_station_id = pkt_sta_id;
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

	// Reject unphysical single-packet cumulative jumps (e.g. >25.4 mm in one packet)
	// by re-anchoring the baseline rather than computing a multi-thousand mm/h spike.
	float ref_cml = (_newest_bucket_tip_event_ts != 0)
		? _newest_bucket_tip_event_cml
		: ((_oldest_bucket_tip_event_ts != 0) ? _oldest_bucket_tip_event_cml : _init_tip_bucket_event_cml);
	if((precip_cml - ref_cml) > MAX_SINGLE_PACKET_PRECIP_DELTA_MM)
	{
		ESP_LOGW(TAG, "Unphysical cumulative precip jump (%.2f -> %.2f mm); re-anchoring precip rate baseline",
			ref_cml, precip_cml);
		reset_precip_rate_state();
		_init_tip_bucket_event_cml = precip_cml;
		_init_tip_bucket_event_ts = now_tick;
		return WsCommon::RetRes::OK;
	}

	// If previous tip events have completely decayed (>1h ago), reset to dry baseline
	// so the first tip of a new rain event uses Case 1 instead of dividing by a multi-hour dry gap.
	uint32_t last_tip_ts = (_newest_bucket_tip_event_ts != 0) ? _newest_bucket_tip_event_ts : _oldest_bucket_tip_event_ts;
	if(last_tip_ts != 0 && now_tick > last_tip_ts &&
	   (now_tick - last_tip_ts) >= (uint32_t)WS_DATA_PRECIP_RATE_DECAY_TIMEOUT_SEC)
	{
		reset_precip_rate_state();
		_init_tip_bucket_event_cml = ref_cml;
		_init_tip_bucket_event_ts = last_tip_ts;
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
		if (tip_delta <= 0.0f || tip_delta > MAX_SINGLE_PACKET_PRECIP_DELTA_MM) tip_delta = 0.254f;

		uint32_t init_interval = (_oldest_bucket_tip_event_ts > _init_tip_bucket_event_ts)
			? (_oldest_bucket_tip_event_ts - _init_tip_bucket_event_ts) : 0;
		if (init_interval > 0 && init_interval < (uint32_t)WS_DATA_PRECIP_RATE_DECAY_TIMEOUT_SEC)
		{
			uint32_t elapsed_sec = init_interval + elapsed_since_tip;
			uint32_t effective_window = elapsed_sec > 120 ? elapsed_sec : 120;
			float rate = 3600.0f * tip_delta / effective_window;
			if (!isfinite(rate) || rate < 0.0f || rate > MAX_VALID_PRECIP_RATE_MM_H) return 0.0f;
			return rate;
		}

		uint32_t effective_window = elapsed_since_tip > 120 ? elapsed_since_tip : 120;
		float rate = 3600.0f * tip_delta / effective_window;
		if (!isfinite(rate) || rate < 0.0f || rate > MAX_VALID_PRECIP_RATE_MM_H) return 0.0f;
		return rate;
	}

	// Case 2: At least two tip events recorded
	if(_oldest_bucket_tip_event_cml == 0.0f || _newest_bucket_tip_event_cml == 0.0f) return 0.0f;

	if(now < _newest_bucket_tip_event_ts) return 0.0f;
	if(now - _newest_bucket_tip_event_ts >= (uint32_t)WS_DATA_PRECIP_RATE_DECAY_TIMEOUT_SEC) return 0.0f;

	float bucket_tip_event_cml_delta = _newest_bucket_tip_event_cml - _oldest_bucket_tip_event_cml;
	uint32_t bucket_tip_event_ts_delta = _newest_bucket_tip_event_ts - _oldest_bucket_tip_event_ts;
	uint32_t current_time_delta = now - _newest_bucket_tip_event_ts;
	uint32_t elapsed_sec = bucket_tip_event_ts_delta + current_time_delta;
	if(elapsed_sec == 0 || bucket_tip_event_cml_delta <= 0.0f ||
	   bucket_tip_event_cml_delta > MAX_SINGLE_PACKET_PRECIP_DELTA_MM) return 0.0f;

	float rate = 3600.0f * bucket_tip_event_cml_delta / elapsed_sec;
	if (!isfinite(rate) || rate < 0.0f || rate > MAX_VALID_PRECIP_RATE_MM_H) return 0.0f;
	return rate;
}

void reset_daily_precip()
{
	_daily_baseline_cml = NAN;
	_daily_accum_rollover = 0.0f;
	_daily_current_day_idx = -1;
	_daily_precip_cached = 0.0f;
	save_daily_state_to_nvs(true);
}

void update_daily_precip(float cml_val)
{
	ensure_daily_state_loaded();
	if(!isfinite(cml_val)) return;

	int32_t day_idx = get_local_day_index();
	if(day_idx >= 0)
	{
		if(_daily_current_day_idx == -1)
		{
			// Transitioning from pre-NTP to valid local day: preserve any pre-NTP baseline captured since boot
			_daily_current_day_idx = day_idx;
			if(isnan(_daily_baseline_cml))
			{
				_daily_baseline_cml = cml_val;
				_daily_accum_rollover = 0.0f;
				_daily_precip_cached = 0.0f;
				ESP_LOGI(TAG, "Daily rain counter initialized for local day index %ld (baseline: %.2f mm)",
					(long)day_idx, cml_val);
				save_daily_state_to_nvs(true);
				return;
			}
			save_daily_state_to_nvs(true);
		}
		else if(_daily_current_day_idx != day_idx)
		{
			_daily_current_day_idx = day_idx;
			_daily_baseline_cml = cml_val;
			_daily_accum_rollover = 0.0f;
			_daily_precip_cached = 0.0f;
			if(_daily_extremes_day_idx != -1 && _daily_extremes_day_idx != day_idx)
			{
				_daily_extremes = DailyExtremes();
				_daily_extremes_day_idx = day_idx;
			}
			ESP_LOGI(TAG, "Daily rain counter rolled over to local day index %ld (baseline: %.2f mm)",
				(long)day_idx, cml_val);
			save_daily_state_to_nvs(true);
			return;
		}
	}

	if(isnan(_daily_baseline_cml))
	{
		_daily_baseline_cml = cml_val;
		_daily_accum_rollover = 0.0f;
		_daily_precip_cached = 0.0f;
		if(day_idx >= 0) save_daily_state_to_nvs(true);
		return;
	}

	float prev_cached = _daily_precip_cached;
	if(cml_val >= _daily_baseline_cml)
	{
		_daily_precip_cached = (cml_val - _daily_baseline_cml) + _daily_accum_rollover;
	}
	else
	{
		// Station reset or counter wrap
		_daily_accum_rollover = _daily_precip_cached + cml_val;
		_daily_baseline_cml = cml_val;
		_daily_precip_cached = _daily_accum_rollover;
		if(day_idx >= 0) save_daily_state_to_nvs(true);
	}

	if(day_idx >= 0 && fabsf(_daily_precip_cached - prev_cached) >= 0.01f)
	{
		save_daily_state_to_nvs(false);
	}
}

float get_daily_precip()
{
	ensure_daily_state_loaded();

	int32_t day_idx = get_local_day_index();
	if(day_idx >= 0 && _daily_current_day_idx != -1 && _daily_current_day_idx != day_idx)
	{
		_daily_current_day_idx = day_idx;
		_daily_baseline_cml = NAN;
		_daily_accum_rollover = 0.0f;
		_daily_precip_cached = 0.0f;
		if(_daily_extremes_day_idx != -1 && _daily_extremes_day_idx != day_idx)
		{
			_daily_extremes = DailyExtremes();
			_daily_extremes_day_idx = day_idx;
		}
		save_daily_state_to_nvs(false);
	}

	// Reconcile with WeatherLogger aggregates (including SD-hydrated history after reboot)
	if(day_idx >= 0)
	{
		time_t utc_midnight = get_local_day_utc_midnight();
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
		if(found && sum > _daily_precip_cached + 0.05f)
		{
			float missing = sum - _daily_precip_cached;
			_daily_accum_rollover += missing;
			_daily_precip_cached = sum;
			if(_daily_current_day_idx == -1) _daily_current_day_idx = day_idx;
			save_daily_state_to_nvs(false);
		}
		if(found && sum > _daily_precip_cached)
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
	save_daily_state_to_nvs(true);
}

void update_daily_extremes(const WsFields::FieldPack* field_pack)
{
	ensure_daily_state_loaded();
	if(field_pack == NULL) return;

	bool changed = false;
	int32_t day_idx = get_local_day_index();
	if(day_idx >= 0)
	{
		if(_daily_extremes_day_idx == -1)
		{
			_daily_extremes_day_idx = day_idx;
			changed = true;
		}
		else if(_daily_extremes_day_idx != day_idx)
		{
			_daily_extremes = DailyExtremes();
			_daily_extremes_day_idx = day_idx;
			changed = true;
		}
	}

	float temp = 0.0f;
	if(WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(field_pack), WsFields::FieldType::WEATHER_TEMP, NULL, &temp) == WsCommon::RetRes::OK && isfinite(temp))
	{
		if(isnan(_daily_extremes.temp_min) || temp < _daily_extremes.temp_min) { _daily_extremes.temp_min = temp; changed = true; }
		if(isnan(_daily_extremes.temp_max) || temp > _daily_extremes.temp_max) { _daily_extremes.temp_max = temp; changed = true; }
		_daily_extremes.has_data = true;
	}

	float press = 0.0f;
	if(WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(field_pack), WsFields::FieldType::WEATHER_PRESSURE, NULL, &press) == WsCommon::RetRes::OK && isfinite(press))
	{
		if(isnan(_daily_extremes.pressure_min) || press < _daily_extremes.pressure_min) { _daily_extremes.pressure_min = press; changed = true; }
		if(isnan(_daily_extremes.pressure_max) || press > _daily_extremes.pressure_max) { _daily_extremes.pressure_max = press; changed = true; }
		_daily_extremes.has_data = true;
	}

	float hum = 0.0f;
	if(WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(field_pack), WsFields::FieldType::WEATHER_HUM, NULL, &hum) == WsCommon::RetRes::OK && isfinite(hum))
	{
		if(isnan(_daily_extremes.hum_min) || hum < _daily_extremes.hum_min) { _daily_extremes.hum_min = hum; changed = true; }
		if(isnan(_daily_extremes.hum_max) || hum > _daily_extremes.hum_max) { _daily_extremes.hum_max = hum; changed = true; }
		_daily_extremes.has_data = true;
	}

	float gust = 0.0f;
	if(WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(field_pack), WsFields::FieldType::WEATHER_WIND_GUST, NULL, &gust) == WsCommon::RetRes::OK && isfinite(gust))
	{
		if(isnan(_daily_extremes.wind_gust_max) || gust > _daily_extremes.wind_gust_max) { _daily_extremes.wind_gust_max = gust; changed = true; }
		_daily_extremes.has_data = true;
	}

	float rate = 0.0f;
	if(WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(field_pack), WsFields::FieldType::WEATHER_PRECIP_RATE, NULL, &rate) == WsCommon::RetRes::OK &&
	   isfinite(rate) && rate >= 0.0f && rate <= MAX_VALID_PRECIP_RATE_MM_H)
	{
		if(isnan(_daily_extremes.precip_rate_max) || rate > _daily_extremes.precip_rate_max) { _daily_extremes.precip_rate_max = rate; changed = true; }
	}
	float live_rate = get_precip_rate();
	if(isfinite(live_rate) && live_rate >= 0.0f && live_rate <= MAX_VALID_PRECIP_RATE_MM_H)
	{
		if(isnan(_daily_extremes.precip_rate_max) || live_rate > _daily_extremes.precip_rate_max) { _daily_extremes.precip_rate_max = live_rate; changed = true; }
	}

	uint8_t uv = 0;
	if(WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(field_pack), WsFields::FieldType::WEATHER_UV_INDEX, NULL, &uv) == WsCommon::RetRes::OK)
	{
		if(uv > _daily_extremes.uv_max) { _daily_extremes.uv_max = uv; changed = true; }
		_daily_extremes.has_data = true;
	}

	float solar = 0.0f;
	if(WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(field_pack), WsFields::FieldType::WEATHER_SOLAR_RAD, NULL, &solar) == WsCommon::RetRes::OK && isfinite(solar))
	{
		if(isnan(_daily_extremes.solar_rad_max) || solar > _daily_extremes.solar_rad_max) { _daily_extremes.solar_rad_max = solar; changed = true; }
		_daily_extremes.has_data = true;
	}

	if(changed && day_idx >= 0)
	{
		save_daily_state_to_nvs(false);
	}
}

DailyExtremes get_daily_extremes()
{
	ensure_daily_state_loaded();

	int32_t day_idx = get_local_day_index();
	if(day_idx >= 0 && _daily_extremes_day_idx != -1 && _daily_extremes_day_idx != day_idx)
	{
		_daily_extremes = DailyExtremes();
		_daily_extremes_day_idx = day_idx;
		if(_daily_current_day_idx != -1 && _daily_current_day_idx != day_idx)
		{
			_daily_current_day_idx = day_idx;
			_daily_baseline_cml = NAN;
			_daily_accum_rollover = 0.0f;
			_daily_precip_cached = 0.0f;
		}
		save_daily_state_to_nvs(false);
	}

	DailyExtremes res = _daily_extremes;

	// Reconcile with WeatherLogger aggregates for today (handles SD card recovery after mid-day reboot)
	if(day_idx >= 0)
	{
		time_t utc_midnight = get_local_day_utc_midnight();
		auto aggs = WeatherLogger::get_aggregates();
		for(const auto &agg : aggs)
		{
			if(agg.valid && agg.timestamp >= utc_midnight)
			{
				if(!isnan(agg.temp_avg))
				{
					if(isnan(res.temp_min) || agg.temp_avg < res.temp_min) res.temp_min = agg.temp_avg;
					if(isnan(res.temp_max) || agg.temp_avg > res.temp_max) res.temp_max = agg.temp_avg;
					res.has_data = true;
				}
				if(!isnan(agg.pressure_avg))
				{
					if(isnan(res.pressure_min) || agg.pressure_avg < res.pressure_min) res.pressure_min = agg.pressure_avg;
					if(isnan(res.pressure_max) || agg.pressure_avg > res.pressure_max) res.pressure_max = agg.pressure_avg;
					res.has_data = true;
				}
				if(!isnan(agg.hum_avg))
				{
					if(isnan(res.hum_min) || agg.hum_avg < res.hum_min) res.hum_min = agg.hum_avg;
					if(isnan(res.hum_max) || agg.hum_avg > res.hum_max) res.hum_max = agg.hum_avg;
					res.has_data = true;
				}
				if(!isnan(agg.wind_gust_max))
				{
					if(isnan(res.wind_gust_max) || agg.wind_gust_max > res.wind_gust_max) res.wind_gust_max = agg.wind_gust_max;
					res.has_data = true;
				}
				if(!isnan(agg.precip_avg) && agg.precip_avg >= 0.0f && agg.precip_avg <= MAX_VALID_PRECIP_RATE_MM_H)
				{
					if(isnan(res.precip_rate_max) || agg.precip_avg > res.precip_rate_max) res.precip_rate_max = agg.precip_avg;
				}
				if(agg.uv_available)
				{
					if(agg.uv_max > res.uv_max) res.uv_max = agg.uv_max;
					res.has_data = true;
				}
				if(!isnan(agg.solar_rad_avg))
				{
					if(isnan(res.solar_rad_max) || agg.solar_rad_avg > res.solar_rad_max) res.solar_rad_max = agg.solar_rad_avg;
					res.has_data = true;
				}
			}
		}
	}

	return res;
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
