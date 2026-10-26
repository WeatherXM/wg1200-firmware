#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS

#include "include/ws_handler.h"
#include "ws/supported/ws_1001/include/ws_1001_helpers.h"
#include "ws/supported/ws_1001/include/ws_1001_decoders.h"
#include "ws/supported/ws_1001/include/ws_1001_helpers.h"
#include "ws/supported/ws_1300/include/ws_1300_helpers.h"
#include "ws/supported/ws_1300/include/ws_1300_decoders.h"
#include "include/ws_common.h"
#include "include/ws_gps.h"
#include "include/packet_reception.h"
#include "datetime_rtc.h"
#include "esp_timer.h"
#include "include/ws_data.h"
#include "freertos/event_groups.h"
#include "time.h"
#include "math.h"
#include "const.h"
#include "settings.h"
#include "global_events.h"
#include "esp_log.h"
#ifndef CONFIG_SCREENLESS
#include "radio.h"
#include "sx126x.h"
#endif
#include "utils.h"
#include "include/tb.h"
#include <ArduinoJson.h>
#include "ws/include/web_server.h"
#include "ws/include/mesh_telemetry.h"
#include "weather_logger.h"
#ifdef CONFIG_SCREENLESS
#include "ws/include/kiss_client.h"
#define KISS_UART_TX_PIN   43
#define KISS_UART_RX_PIN   44
#define KISS_UART_CTS_PIN  -1
#define KISS_UART_RTS_PIN  -1
#endif
#include "ws/include/meshcore_frame.h"
#include "ws/include/remote_observation_queue.h"
#if __has_include("mbedtls/private/aes.h")
#include "mbedtls/private/aes.h"
#else
#include "mbedtls/aes.h"
#endif
#include "mbedtls/md.h"
#include "struct.h"
#include "esp_attr.h"
#include "esp_random.h"

namespace WsHandler
{

const WsBase::MultiStation STATION_LIST[2] = 
{
	{
		Ws1001::NAME,
		Ws1001::TYPE,
		Ws1001::RF_CONFIG,
		Ws1001::CONFIG,
		Ws1001::FIELDS,
		Ws1001::FIELDS_COUNT,
		Ws1001::packet_handler,
		Ws1001::packet_decoder,
		Ws1001::freq_resolver,
		Ws1001::BROADCAST_INTERVAL_SEC,
		Ws1001::BUSY_DURATION_SEC,
		Ws1001::ADAPTER_TB,
		Ws1001::ADAPTER_TB_VALUES_COUNT
	},
	{
		Ws1300::NAME,
		Ws1300::TYPE,
		Ws1300::RF_CONFIG,
		Ws1300::CONFIG,
		Ws1300::FIELDS,
		Ws1300::FIELDS_COUNT,
		Ws1300::packet_handler,
		Ws1300::packet_decoder,
		Ws1300::freq_resolver,
		Ws1300::BROADCAST_INTERVAL_SEC,
		Ws1300::BUSY_DURATION_SEC,
		Ws1300::ADAPTER_TB,
		Ws1300::ADAPTER_TB_VALUES_COUNT
	}
};

static const char TAG[] = "WsHandler";

static uint32_t _current_freq_hz = 0;

bool g_sniff_mode_enabled = true;
bool g_auto_pair_enabled = true;
uint8_t g_sniff_channel = 0;

/** One-shot remote reboot request for the paired WS1300. Consumed (and
 * cleared) by Ws1300::packet_handler on the next ACK cycle - the station
 * reboots as soon as it receives a CMD packet with reboot=true. */
bool g_ws1300_reboot_requested = false;
bool g_ws1300_gps_req_forced = false;

/** RAM-only WS1300 ownership challenge. Receiving a broadcast is not proof
 * that the station accepts this gateway's HMAC. A manual GPS request becomes
 * confirmed only when a subsequent GNSS packet is received from that station.
 * Another gateway assigned to the same station must be in OFF/LISTEN so it
 * cannot issue or collide with downlinks during the challenge. */
bool g_ownership_challenge_armed = false;
bool g_ownership_challenge_pending = false;
bool g_ownership_confirmed = false;
uint32_t g_ownership_challenge_station_id = 0;
uint32_t g_ownership_challenge_started_sec = 0;
uint8_t g_ownership_challenge_packets_seen = 0;
static const uint8_t OWNERSHIP_CHALLENGE_MAX_PACKETS = 4;

/** One-shot "station kick" for a *sniffed* (unpaired-gateway) WS1300: answers
 * the next non-BOOT packet with an authenticated CMD-reboot. The rebooting
 * station emits STA_BOOT, which triggers auto-pairing - the LoRa route to
 * re-adopt an already-paired station (the protocol has no unpair command).
 * HMAC is keyed on this gateway's serial, so only a station paired to this
 * gateway accepts it. */
bool g_ws1300_kick_armed = false;

/** Optional station-ID filter for the kick. 0 = kick the first WS1300 heard;
 * otherwise only a packet from this exact station consumes the one-shot
 * (several stations can share a sniff channel). */
uint32_t g_ws1300_kick_target_id = 0;

/** Exact station selected from the recent list during passive SCAN.
 * 0 accepts the first pairable ADV/BOOT, preserving legacy behavior. */
uint32_t g_scan_target_id = 0;

/** Known radio slot for a targeted pair/repair.
 * -1 keeps ordinary SCAN sweeping all nine slots; 0 locks to base and 1-8
 * lock directly to a WS1300 channel without disabling auto-pairing. */
int8_t g_scan_target_slot = -1;

/** Manual TX power override for the paired WS1300, in dBm.
 * 0 = auto (gateway's adaptive ack-loss algorithm decides), else a fixed
 * value clamped to [Ws1300Helpers::POWER_LEVEL_MIN, POWER_LEVEL_MAX]. */
int16_t g_ws1300_tx_power_override = 0;

/** Manual TX interval override for the paired WS1300, in seconds.
 * 0 = auto (station kept at the fixed 20s broadcast interval), else a value
 * clamped to [20, 240] and rounded down to a multiple of 20 (wire format is
 * a 20s-step multiplier). */
int16_t g_ws1300_tx_interval_override_sec = 0;
uint8_t g_ws1300_meshtastic_interval_min = 0;
uint8_t g_ws1300_meshtastic_preset = 0;
uint8_t g_ws1300_meshtastic_hop_limit = 3;
uint32_t g_ws1300_meshtastic_custom_freq_hz = 0;
uint16_t g_ws1300_meshtastic_custom_bw_khz = 0;
uint8_t g_ws1300_meshtastic_custom_sf = 0;
uint8_t g_ws1300_meshtastic_custom_cr = 0;
int8_t g_ws1300_meshtastic_custom_tx_power_dbm = 0;
bool g_ws1300_meshtastic_changed = false;

uint8_t g_ws1300_meshcore_interval_min = 0;
uint8_t g_ws1300_meshcore_profile_idx = 4;
uint32_t g_ws1300_meshcore_custom_freq_hz = 0;
uint16_t g_ws1300_meshcore_custom_bw_khz = 0;
uint8_t g_ws1300_meshcore_custom_sf = 0;
uint8_t g_ws1300_meshcore_custom_cr = 0;
int8_t g_ws1300_meshcore_custom_tx_power_dbm = 0;
bool g_ws1300_meshcore_changed = false;
bool g_ws1300_weather_policy_changed = false;
uint8_t g_ws1300_station_fw_ver = 0;
WsType g_receiver_profile = WsType::WS_TYPE_ALL;
WsType g_scan_profile = WsType::WS_TYPE_1001;

/** Sniff tuning slot: 0 = shared base frequency (WS1001 + legacy WS1300 ADV),
 * 1-8 = WS1300 channels 0-7.
 * Only advanced while UNPAIRED - a paired gateway never leaves its station's
 * frequency, sniffing there is purely opportunistic. */
uint8_t g_sniff_slot = 0;

char g_ws_fw_warning[64] = "";

void set_station_fw_warning(const char *warning)
{
	if(warning == nullptr) warning = "";
	if(strncmp(g_ws_fw_warning, warning, sizeof(g_ws_fw_warning)) != 0)
	{
		strncpy(g_ws_fw_warning, warning, sizeof(g_ws_fw_warning) - 1);
		g_ws_fw_warning[sizeof(g_ws_fw_warning) - 1] = '\0';

		if(Tb::is_connected())
		{
			StaticJsonDocument<128> doc;
			doc[TB_ATTR_WS_FW_WARN] = (g_ws_fw_warning[0] != '\0') ? g_ws_fw_warning : "OK";
			char buf[128];
			serializeJson(doc, buf, sizeof(buf));
			Tb::publish(TB_ATTRIBUTE_PUBLISH_TOPIC, buf);
		}
	}
}

const char* get_station_fw_warning()
{
	return g_ws_fw_warning;
}

char g_ws_last_err[64] = "";

void set_station_last_err(const char *err)
{
	if(err == nullptr) err = "";
	if(strncmp(g_ws_last_err, err, sizeof(g_ws_last_err)) != 0)
	{
		strncpy(g_ws_last_err, err, sizeof(g_ws_last_err) - 1);
		g_ws_last_err[sizeof(g_ws_last_err) - 1] = '\0';

		if(Tb::is_connected())
		{
			StaticJsonDocument<128> doc;
			doc[TB_ATTR_WS_LAST_ERR] = (g_ws_last_err[0] != '\0') ? g_ws_last_err : "OK";
			char buf[128];
			serializeJson(doc, buf, sizeof(buf));
			Tb::publish(TB_ATTRIBUTE_PUBLISH_TOPIC, buf);
		}
	}
}

const char* get_station_last_err()
{
	return g_ws_last_err;
}

char g_ws_sensor_err[32] = "";

void set_station_sensor_err(const char *err)
{
	if(err == nullptr) err = "";
	if(strncmp(g_ws_sensor_err, err, sizeof(g_ws_sensor_err)) != 0)
	{
		strncpy(g_ws_sensor_err, err, sizeof(g_ws_sensor_err) - 1);
		g_ws_sensor_err[sizeof(g_ws_sensor_err) - 1] = '\0';

		if(Tb::is_connected())
		{
			StaticJsonDocument<128> doc;
			doc[TB_ATTR_WS_SENSOR_ERR] = (g_ws_sensor_err[0] != '\0') ? g_ws_sensor_err : "OK";
			char buf[128];
			serializeJson(doc, buf, sizeof(buf));
			Tb::publish(TB_ATTRIBUTE_PUBLISH_TOPIC, buf);
		}
	}
}

const char* get_station_sensor_err()
{
	return g_ws_sensor_err;
}

char g_ws_gps_status[32] = "IDLE";

void set_station_gps_status(const char *status)
{
	if(status == nullptr) status = "IDLE";
	if(strncmp(g_ws_gps_status, status, sizeof(g_ws_gps_status)) != 0)
	{
		strncpy(g_ws_gps_status, status, sizeof(g_ws_gps_status) - 1);
		g_ws_gps_status[sizeof(g_ws_gps_status) - 1] = '\0';

		if(Tb::is_connected())
		{
			StaticJsonDocument<128> doc;
			doc[TB_ATTR_WS_GPS_STATUS] = g_ws_gps_status;
			char buf[128];
			serializeJson(doc, buf, sizeof(buf));
			Tb::publish(TB_ATTRIBUTE_PUBLISH_TOPIC, buf);
		}
	}
}

const char* get_station_gps_status()
{
	return g_ws_gps_status;
}

bool g_ws_ack_loss = false;

void set_station_ack_loss(bool lost)
{
	if(g_ws_ack_loss != lost)
	{
		g_ws_ack_loss = lost;

		if(Tb::is_connected())
		{
			StaticJsonDocument<128> doc;
			doc[TB_ATTR_WS_ACK_LOSS] = g_ws_ack_loss;
			char buf[128];
			serializeJson(doc, buf, sizeof(buf));
			Tb::publish(TB_ATTRIBUTE_PUBLISH_TOPIC, buf);
		}
	}
}

bool get_station_ack_loss()
{
	return g_ws_ack_loss;
}

uint32_t g_ws_lost_packets_total = 0;

void add_station_lost_packets(uint32_t count)
{
	g_ws_lost_packets_total += count;
}

uint32_t get_station_lost_packets()
{
	return g_ws_lost_packets_total;
}

static ScanPhase _scan_phase = ScanPhase::WS1001;
static uint32_t _scan_cycle_count = 0;
static bool _scan_completed = false;
static uint64_t _ws1001_scan_start_ms = 0;

ScanPhase get_scan_phase()
{
	return _scan_phase;
}

WsType get_scan_profile()
{
	return g_scan_profile;
}

uint32_t get_scan_cycle_count()
{
	return _scan_cycle_count;
}

bool is_scan_completed()
{
	return _scan_completed;
}

// Fallback timing constants (~1 hour full recovery cycle)
static const uint32_t FALLBACK_PHASE_DURATION_MS = 5 * 60 * 1000;         // 5 minutes (Local, Cloud, WS1001)
static const uint32_t FALLBACK_WS1300_SWEEP_DURATION_MS = 16 * 60 * 1000;  // 16 minutes (8 channels * 2 min)
static const uint32_t FALLBACK_WS1300_CHANNEL_DWELL_MS = 2 * 60 * 1000;   // 2 minutes (120s) per WS1300 channel
static const uint32_t FALLBACK_COOLDOWN_DURATION_MS = 30 * 60 * 1000;     // 30 minutes cooldown (~1h full cycle)
static AutoFallbackPhase _fallback_phase = AutoFallbackPhase::PHASE_LOCAL_NVS;
static uint64_t _fallback_phase_start_ms = 0;
static bool _fallback_manual_override = false;
static bool _is_fallback_latch = false;

AutoFallbackPhase get_fallback_phase()
{
	return _fallback_phase;
}

const char* get_fallback_phase_name()
{
	switch (_fallback_phase)
	{
		case AutoFallbackPhase::PHASE_LOCAL_NVS: return "local";
		case AutoFallbackPhase::PHASE_CLOUD_NVS: return "cloud";
		case AutoFallbackPhase::PHASE_WS1001_SCAN: return "ws1001_scan";
		case AutoFallbackPhase::PHASE_WS1300_SWEEP: return "ws1300_sweep";
		case AutoFallbackPhase::PHASE_COOLDOWN: return "cooldown";
		case AutoFallbackPhase::PHASE_LATCHED: return "latched";
		case AutoFallbackPhase::PHASE_MANUAL: return "manual";
		default: return "unknown";
	}
}

uint32_t get_fallback_seconds_remaining()
{
	if (_fallback_phase == AutoFallbackPhase::PHASE_LATCHED ||
	    _fallback_phase == AutoFallbackPhase::PHASE_MANUAL)
	{
		return 0;
	}
	uint64_t now_ms = Utils::get_tick_ms();
	uint32_t duration_ms = FALLBACK_PHASE_DURATION_MS;
	if (_fallback_phase == AutoFallbackPhase::PHASE_WS1300_SWEEP)
	{
		duration_ms = FALLBACK_WS1300_SWEEP_DURATION_MS;
	}
	else if (_fallback_phase == AutoFallbackPhase::PHASE_COOLDOWN)
	{
		duration_ms = FALLBACK_COOLDOWN_DURATION_MS;
	}
	uint64_t elapsed_ms = (now_ms >= _fallback_phase_start_ms) ? (now_ms - _fallback_phase_start_ms) : 0;
	if (elapsed_ms >= duration_ms)
	{
		return 0;
	}
	return (uint32_t)((duration_ms - elapsed_ms) / 1000);
}

void set_fallback_phase(AutoFallbackPhase phase)
{
	_fallback_phase = phase;
	_fallback_phase_start_ms = Utils::get_tick_ms();
	if (phase == AutoFallbackPhase::PHASE_MANUAL)
	{
		_fallback_manual_override = true;
	}
}

void reset_fallback_state_machine()
{
	_fallback_manual_override = false;
	_fallback_phase_start_ms = Utils::get_tick_ms();
	if (Settings::get_pair_mode() == PairMode::PAIR_MODE_CLOUD && Settings::cloud_ws_profile_ready())
	{
		_fallback_phase = AutoFallbackPhase::PHASE_CLOUD_NVS;
	}
	else if (Settings::local_ws_profile_ready())
	{
		_fallback_phase = AutoFallbackPhase::PHASE_LOCAL_NVS;
	}
	else if (Settings::cloud_ws_profile_ready())
	{
		_fallback_phase = AutoFallbackPhase::PHASE_CLOUD_NVS;
	}
#if !FALLBACK_ENABLE_PHASE_3_WS1001
	else
	{
		_fallback_phase = AutoFallbackPhase::PHASE_WS1300_SWEEP;
	}
#else
	else
	{
		_fallback_phase = AutoFallbackPhase::PHASE_WS1001_SCAN;
	}
#endif
}

const char* get_pairing_state_name()
{
	if (Settings::get_pair_mode() == PairMode::PAIR_MODE_OFF)
	{
		return "monitor_only";
	}

	if (is_scan_active() || get_pair_state() != PairState::PAIRED)
	{
		return "standby_discovery";
	}

	if (_is_fallback_latch)
	{
		return "fallback_latch";
	}

	if (Settings::get_pair_mode() == PairMode::PAIR_MODE_LOCAL)
	{
		return "local_override";
	}

	if (Settings::get_pair_mode() == PairMode::PAIR_MODE_CLOUD)
	{
		return "cloud_verified";
	}

	return "standby_discovery";
}

/** Manual listen override (diagnostics, set from the web console):
 * -1 = AUTO (normal behavior: paired lock / sniff sweep),
 * 0 = shared base frequency, 1-8 = WS1300 channels 0-7.
 * While set, the radio is locked to this slot, the sweep is stopped and
 * auto-pairing is disabled. RAM-only: reverts to AUTO on reboot. */
int8_t g_manual_listen_slot = -1;

/** Human-readable description of what the radio is currently tuned to */
static char _listen_desc[32] = "-";

/** Legacy WS1300 frequency compatibility: when true, a paired WS1300 is
 * listened for on the plain region base frequency instead of its
 * serial-derived channel. Early prototype stations (pre-v11 firmware)
 * transmit there. Toggled by the paired frequency probe in
 * packet_handler_task and locked (_paired_channel_locked) wherever the
 * station is actually heard. Responses are always transmitted on the
 * frequency the packet arrived on, regardless of this flag. */
static bool _paired_base_fallback = false;
static bool _paired_channel_locked = false;
static uint16_t _paired_station_interval_sec = 20;

/** Where the paired station was last actually heard (meaningful only while
 * _paired_freq_confirmed): true = legacy base frequency, false =
 * serial-derived channel. A station never migrates between the two (v11+
 * lives on its channel, pre-v11 on base), so after an INACTIVE lock release
 * the re-probe dwells on the confirmed side and only glances at the other -
 * otherwise a brief outage turns into many minutes of listening where the
 * station can never be. RAM-only: a reboot falls back to the
 * ActivityStatus::NONE asymmetric probe. */
static bool _paired_freq_confirmed = false;
static bool _paired_confirmed_on_base = false;

/** Cap on timeout waits while the base/channel probe is active. RX is
 * continuous, so short waits only make the probe flip sooner - without the
 * cap the miss-streak interval model stretches each probe dwell up to
 * 240 s and a full base+channel cycle can exceed 10 minutes. */
static const uint16_t PAIRED_PROBE_WAIT_MAX_SEC = 60;

/** Consecutive paired-listen timeouts before probing the other frequency */
static const int PAIRED_FREQ_PROBE_TIMEOUTS = 3;
static int _paired_miss_streak = 0;
static PairSource _active_pair_source = PairSource::PAIR_SOURCE_NONE;
static RecentStation _recent_stations[RECENT_STATIONS_CAPACITY] = {};

#ifdef CONFIG_SCREENLESS
void sx126x_rx_cb(uint8_t *raw_packet_buff, uint16_t raw_packet_buff_length, int16_t rssi, int8_t snr );
static void kiss_rx_task(void* arg) {
	uint8_t buf[256];
	int8_t snr, rssi;
	uint32_t last_ping_time = 0;
	while (1) {
		uint32_t now = Utils::get_tick_sec();
		if (!KissClient::g_nrf52_uart_ok && (now - last_ping_time >= 5)) {
			last_ping_time = now;
			KissClient::ping();
		}
		int len = KissClient::read_packet(buf, sizeof(buf), &snr, &rssi);
		if (len > 0) {
			sx126x_rx_cb(buf, len, rssi, snr);
		}
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}
#endif

const u8 WS_OK_PACKETS_TS_BUFF_COUNT = 4;
const u32 WS_PACKET_TIMEOUT_TOLERANCE_MS = 5000;
const u32 WS_SNIFF_HOP_INTERVAL_MS = 22000;

// LoRa & Wi-Fi/MQTT collision avoidance timings
const uint32_t POST_PACKET_COOLDOWN_MS = 6000;
const uint32_t PRE_PACKET_GUARD_MS = 7000;
const uint32_t SAFE_SUBMIT_OFFSET_MS = 6500;

const uint32_t SCAN_PASS1_WS1300_DWELL_MS = WS_SNIFF_HOP_INTERVAL_MS; // 22000 ms
const uint32_t SCAN_PASS2_WS1300_DWELL_MS = 45000;                     // 45s (>2x WS1300 20s tx period)
const uint32_t SCAN_PASS1_WS1001_DWELL_MS = 60000;                     // 60s
const uint32_t SCAN_PASS2_WS1001_DWELL_MS = 90000;                     // 90s (>5x WS1001 16s tx period)

static uint32_t get_ws1300_channel_dwell_ms()
{
	if (_fallback_phase == AutoFallbackPhase::PHASE_WS1300_SWEEP)
	{
		return FALLBACK_WS1300_CHANNEL_DWELL_MS;
	}
	if (Settings::get_pair_mode() == PairMode::PAIR_MODE_SCAN)
	{
		// Pass 1: quick sweep (22s). Pass 2: deep sweep (45s)
		return (_scan_cycle_count == 0) ? SCAN_PASS1_WS1300_DWELL_MS : SCAN_PASS2_WS1300_DWELL_MS;
	}
	return WS_SNIFF_HOP_INTERVAL_MS;
}

static uint32_t get_ws1001_channel_dwell_ms()
{
	// Pass 1: fast (60s). Pass 2: deep (90s)
	return (_scan_cycle_count == 0) ? SCAN_PASS1_WS1001_DWELL_MS : SCAN_PASS2_WS1001_DWELL_MS;
}

/** Consecutive bad-value packets required before the station is reported as
 * ACTIVE_BAD_DATA. Isolated malformed samples are normal - the first SENSORS
 * packet after a station reboot carries no sensor readings yet, and one such
 * packet must not raise a warning. Individual bad packets are still logged
 * and still kept out of the telemetry buffer regardless of this threshold. */
const u8 WS_BAD_DATA_STREAK_THRESHOLD = 3;

/** Consecutive bad-value packets seen so far (reset by any good packet) */
static uint8_t _bad_data_streak = 0;

/** How long to hold the sweep on the current channel after a station kick was
 * transmitted, so the rebooting station's STA_BOOT is caught here */
const u32 WS_SNIFF_KICK_HOLD_MS = 60000;

/** Tick when the current sniff slot was tuned. The sweep dwell is enforced on
 * wall-clock time from this stamp - received packets must NOT reset it, or a
 * bystander station broadcasting every ~20s pins the sweep on its channel
 * forever and the other channels are never visited. */
static uint64_t _sniff_slot_tuned_ts_ms = 0;

/** When nonzero, the sweep may not hop before this tick (set after a kick TX) */
static uint64_t _sniff_slot_hold_until_ms = 0;

/** Sniffer packet handler task handle */
static TaskHandle_t _packet_handler_task_handle = NULL;

/** Task stack */
DMA_ATTR StackType_t _packet_handler_task_stack[PACKET_HANDLER_TASK_STACK_SIZE] = {};

/** Task buffer */
StaticTask_t _packet_handler_task_buff;

/** Sniffer event group */
StaticEventGroup_t _evg_buffer;

/** Sniffer event group handle */
EventGroupHandle_t _evg_handle;

struct {
	/** Current station selected */
	WsBase::MultiStation* cur_station = NULL;

	/** Flag to indicate that handler is listening for packets */
	bool is_listening = false;

	/** Current listening freq */
	WsFreqRegion listening_freq = WS_FREQ_REGION_NONE;

	/** Paired state */
	PairState pair_state = PairState::NONE;

	/** Paired station data */
	WsBase::PairData pair_data = {};

	/** Status of the last received WS packet */
	PktStatus last_packet_status = PktStatus::NONE;

	/** Status of the weather station activity */
	ActivityStatus activity_status = ActivityStatus::NONE;

	/** Weather Station reception percentage. 0-100% */
	uint8_t packet_reception = 0;

	/** Number of sequential timeslots lost without receiving a packet */
	uint32_t lost_timeslot_streak = 0;

	/** Latest TX power reported by the station. */
	uint16_t station_tx_power = 0;

	/** Latest TX power selected by the gateway's adaptive controller. */
	uint16_t requested_tx_power = 0;

	/** Signal measurements from the latest accepted station packet. */
	int16_t station_rssi = 0;
	int8_t station_snr = 0;
	bool station_signal_available = false;

	/** Activity status changed timestamp */
	uint32_t activity_status_changed_ts_sec = 0;

	/** Last GPS request timestamp */
	uint32_t gps_last_req_ts_sec = 0;

	/** GPS consecutive request attempt count without a fix */
	uint8_t gps_req_attempts = 0;
} _state;

/** State mutex */
SemaphoreHandle_t _state_mutex;

/** State mutex buffer */
StaticSemaphore_t _state_mutex_buff;

/** The recent-station cache is diagnostic RAM state, independent from the
 * receiver state machine. Packet decoding already owns _state_mutex, so using
 * that same non-recursive mutex here would deadlock on the first RF packet. */
SemaphoreHandle_t _recent_mutex;
StaticSemaphore_t _recent_mutex_buff;

/** Radio ownership mutex.
 *
 * The SX1262 is a single stateful device configured by multi-command SPI
 * sequences (Standby -> SetChannel -> SetModulationParams -> SetPacketParams ->
 * sync word -> SetTx). The driver's own radio_mutex in sx126x_sensecap_board.c
 * only makes each individual SPI burst atomic on the bus; it does nothing to
 * stop a second task interleaving a whole foreign reconfiguration between two
 * of our commands, and it is released before SX126xWaitOnBusy() so a command
 * can be issued while BUSY is still asserted from the previous one.
 *
 * That is not theoretical. Every one of these retunes the radio from a task
 * that is NOT the packet handler task:
 *   apply_station_change()      <- web_server
 *   notify_sniff_mode_toggled() <- web_server
 *   set_manual_listen_slot()    <- web_server
 *   unpair()                    <- web_server AND state_sniffer
 * Landing any of them inside the ~930 ms SF11 Meshtastic transmit drags the
 * carrier off frequency mid-frame. The receiver has already demodulated the
 * explicit header (which carries its own CRC and passes), so the packet is not
 * lost - it is reported as a payload CRC mismatch. Header good, payload bad.
 *
 * RECURSIVE on purpose: set_radio_freq(), tune_sniff_slot(),
 * retune_for_current_state() and sx126x_rx_cont() each take it so that every
 * call site is protected without having to audit all ~20 of them, and those
 * functions call each other freely.
 *
 * Deadlock contract - read before changing anything below:
 *   sx126x_tx_meshtastic() holds this lock for the WHOLE borrow, including the
 *   blocking wait for TX-done. The TX-done callback runs on the expander IO
 *   task, so it must NEVER take this lock while _mesh_tx_in_progress is set.
 *   See sx126x_tx_cb(). */
SemaphoreHandle_t _radio_mutex;

/** How long a caller waits for radio ownership before giving up. Sized to
 * outlast one full Meshtastic borrow: up to 250 ms LBT contention + 2 x 250 ms
 * CAD + ~930 ms SF11 airtime, with the TX-done wait itself capped at 5000 ms.
 * A caller that times out here has hit a genuinely stuck borrow, not a busy one. */
static const uint32_t RADIO_LOCK_TIMEOUT_MS = 7000;

/** Counts retune attempts that arrived while the radio was borrowed for a
 * Meshtastic transmit, i.e. contention that would previously have corrupted a
 * frame in flight. Non-zero here is the fingerprint of the race. */
static volatile uint32_t _radio_contention_count = 0;

/** Timestamp ring buffer of last ok packets */
RingBuff<uint64_t, WS_OK_PACKETS_TS_BUFF_COUNT>  _ws_valid_packets_ts_buff_ms;
PacketReception::CounterWindow _packet_reception_counter_window = {};
static uint32_t _last_valid_packet_rx_ms = 0;

struct PacketQueueMsg
{
	uint8_t buffer[255];
	uint8_t length = 0;
	int16_t rssi = 0;
	int8_t snr = 0;
};

struct PacketCounterSample
{
	uint16_t counter = 0;
	bool valid = false;
	bool station_reset = false;
};
QueueHandle_t _packet_queue = NULL;

#ifndef CONFIG_SCREENLESS
static RadioEvents_t _radio_events;

/** TX power for Meshtastic frames, dBm, by region.
 * The Meshtastic EU868 slot sits at 869.525 MHz, inside the ETSI
 * 869.4-869.65 MHz sub-band, which allows 500 mW ERP (27 dBm) at a 10% duty
 * cycle - so 22 dBm is legal there and matches what the station link already
 * uses. Everywhere else stay at 14 dBm until the region has been checked
 * against its own limits. */
static int8_t mesh_tx_power_dbm()
{
	if (Settings::get_mesh_modem_preset() == 255)
	{
		int8_t custom_pwr = Settings::get_mesh_tx_power_dbm();
		if (custom_pwr > 0)
		{
			return custom_pwr;
		}
	}
	if (!Settings::get_mesh_tx_power_high())
	{
		// Default low power (2 dBm) to avoid near-field LNA saturation on workbench receivers
		return 2;
	}
	switch (Settings::get_freq_region())
	{
		case WS_FREQ_REGION_EU868:
			return 22; // Full power for EU868 (500mW ERP sub-band)
		default:
			return 14;
	}
}

static int8_t meshcore_tx_power_dbm()
{
	if (Settings::get_meshcore_gw_profile_idx() == 255)
	{
		int8_t custom_pwr = Settings::get_meshcore_gw_tx_power_dbm();
		if (custom_pwr > 0)
		{
			return custom_pwr;
		}
	}
	if (!Settings::get_meshcore_gw_tx_power_high())
	{
		return 2;
	}
	switch (Settings::get_freq_region())
	{
		case WS_FREQ_REGION_EU868:
			return 22;
		default:
			return 14;
	}
}

/** Set while the radio is borrowed for a Meshtastic transmit, so the TX-done
 * callback knows to force a frequency restore. */
static volatile bool _mesh_tx_in_progress = false;

/** Incremented on every TX-done callback, whoever the transmit belonged to.
 * A bare "in progress" flag is not enough to wait on: the callback is deferred
 * off the radio IRQ, so the previous station ACK's TX-done can land after the
 * Meshtastic transmit has already started and clear the flag that transmit is
 * waiting on. Waiting on a snapshot of this counter instead cannot be satisfied
 * by a callback that was already queued before the snapshot was taken. */
static volatile uint32_t _tx_done_seq = 0;

/** CAD completion is independent from TX completion. Sequence numbers avoid
 * accepting a delayed IRQ from an earlier scan as the result of a new one. */
static volatile uint32_t _cad_done_seq = 0;
static volatile bool _cad_activity_detected = false;
#endif

WsCommon::RetRes set_cur_station(WsType station_type);
WsBase::MultiStation* get_cur_station();
bool calc_req_gps(bool time_is_valid);
PktStatus handle_packet(PacketQueueMsg* pkt_msg, PacketCounterSample* counter_sample);
PktStatus handle_packet_impl(PacketQueueMsg* pkt_msg, PacketCounterSample* counter_sample);
bool try_sniff_decode(WsCommon::Packet* pkt, WsBase::MultiStation** matched_station,
	WsFields::FieldPack* field_pack, WsGps::GpsData* gps_data);
PairState get_pair_state();
RetResult set_pair_state(PairState state, WsBase::PairData* sta_ctx, bool wipe_nvs = true);
#ifndef CONFIG_SCREENLESS
void sx126x_tx(uint8_t *buffer, uint8_t size);
#endif
bool sx126x_tx_meshtastic(uint8_t *buffer, uint8_t size, uint32_t freq_hz,
                          uint32_t sf, uint32_t bw_idx, uint8_t cr, uint16_t preamble,
                          const RadioLease *lease);
bool sx126x_tx_meshcore(uint8_t *buffer, uint8_t size, uint32_t freq_hz,
                        uint32_t sf, uint32_t bw_idx, uint8_t cr, uint16_t preamble,
                        const RadioLease *lease);
void packet_handler_task(void *data);
uint32_t get_activity_status_duration_sec();
void sx126x_rx_cb(uint8_t *raw_packet_buff, uint16_t raw_packet_buff_length, int16_t rssi, int8_t snr );
#ifndef CONFIG_SCREENLESS
void sx126x_tx_cb();
void sx126x_cad_cb(bool channel_activity_detected);
#endif
RetResult set_radio_freq(WsFreqRegion freq_region);
void tune_sniff_slot();
void retune_for_current_state();
void sx126x_rx_cont();
void set_last_packet_status(PktStatus status);
ActivityStatus get_activity_status();
uint8_t calculate_packet_reception(bool ws_timeout, PktStatus packet_status,
	const PacketCounterSample* counter_sample);
ActivityStatus calculate_ws_status(bool ws_timeout, PktStatus ws_packet_state, uint8_t ws_reception);
RetResult set_activity_status(ActivityStatus status);
uint8_t get_packet_reception();
const char * get_activity_status_title(ActivityStatus status);
WsCommon::RetRes set_cur_station_obj(WsType station_type);
PairSource resolve_active_pair_source();
bool load_station_profile(PairSource source, WsType *type, WsBase::PairData *pair_data);
void record_recent_station(WsBase::MultiStation *station, WsCommon::Packet *packet,
	int16_t rssi, int8_t snr);

void lock_state()
{
	xSemaphoreTakeRecursive(_state_mutex, portMAX_DELAY);
}

void unlock_state()
{
	xSemaphoreGiveRecursive(_state_mutex);
}

void lock_recent()
{
	xSemaphoreTake(_recent_mutex, portMAX_DELAY);
}

void unlock_recent()
{
	xSemaphoreGive(_recent_mutex);
}

/******************************************************************************
 * @brief Take ownership of the radio.
 *
 * Bounded rather than portMAX_DELAY: a stuck borrow must not wedge the web
 * server forever. Callers that fail to acquire MUST abort their operation
 * rather than touching the radio anyway - proceeding unlocked is exactly the
 * bug this mutex exists to close.
 *
 * @param timeout_ms how long to wait for ownership
 * @return true if the lock is held and the caller must call radio_unlock()
 ******************************************************************************/
bool radio_lock(uint32_t timeout_ms)
{
	if(_radio_mutex == NULL)
	{
		// Pre-init callers (WsHandler::init before create) run single threaded.
		return true;
	}

	// Uncontended fast path. Also the instrumentation point: if this fails we
	// are about to block, and if a Meshtastic transmit is in flight that block
	// is a caller that WOULD have retuned the radio mid-frame before this lock
	// existed. Non-zero _radio_contention_count in the field is the fingerprint
	// of the race - it is the evidence, not just a counter.
	if(xSemaphoreTakeRecursive(_radio_mutex, 0) == pdTRUE)
	{
		return true;
	}

#ifndef CONFIG_SCREENLESS
	if(_mesh_tx_in_progress)
	{
		_radio_contention_count++;
		ESP_LOGW(TAG, "radio contention #%u: '%s' wants to reprogram the radio while a "
					  "Meshtastic transmit is in flight - deferring instead of "
					  "corrupting the frame",
			(unsigned)_radio_contention_count, pcTaskGetName(NULL));
	}
#endif

	if(xSemaphoreTakeRecursive(_radio_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
	{
		return true;
	}

	ESP_LOGE(TAG, "radio busy: '%s' could not take ownership within %ums, operation skipped",
		pcTaskGetName(NULL), (unsigned)timeout_ms);
	return false;
}

void radio_unlock()
{
	if(_radio_mutex == NULL) return;
	xSemaphoreGiveRecursive(_radio_mutex);
}

uint32_t get_radio_contention_count()
{
	return _radio_contention_count;
}

void init_pair_data(WsBase::PairData* pair_data)
{
	memset(pair_data->sn, 0xFF, sizeof(pair_data->sn));
	memset(pair_data->pubkey, 0xFF, sizeof(pair_data->pubkey));
	pair_data->id = -1;
}

void record_recent_station(WsBase::MultiStation *station, WsCommon::Packet *packet,
	int16_t rssi, int8_t snr)
{
	if(station == nullptr || packet == nullptr)
	{
		return;
	}

	RecentStation observed = {};
	observed.valid = true;
	observed.type = station->TYPE;
	observed.last_seen_tick_sec = Utils::get_tick_sec();
	observed.rssi = rssi;
	observed.snr = snr;
	observed.channel_slot = g_sniff_slot;

	if(station->TYPE == WsType::WS_TYPE_1300)
	{
		if(Ws1300Decoders::decode_meta_id(packet, &observed.id) != WsCommon::RetRes::OK)
		{
			return;
		}
		Ws1300::StaPacketType packet_type = Ws1300Helpers::get_packet_type(packet);
		observed.packet_type = static_cast<uint8_t>(packet_type);
		observed.pairable = packet_type == Ws1300::StaPacketType::ADV ||
			packet_type == Ws1300::StaPacketType::BOOT;
		if(packet_type == Ws1300::StaPacketType::ADV)
		{
			Ws1300::ByteBuffer::StaAdv *adv =
				reinterpret_cast<Ws1300::ByteBuffer::StaAdv*>(packet->pl);
			memcpy(observed.sn, adv->sn, sizeof(observed.sn));
			observed.has_sn = true;
			memcpy(observed.pubkey, adv->pub_key, sizeof(observed.pubkey));
			observed.has_pubkey = true;
		}
		else if(packet_type == Ws1300::StaPacketType::BOOT)
		{
			Ws1300::ByteBuffer::StaBoot *boot =
				reinterpret_cast<Ws1300::ByteBuffer::StaBoot*>(packet->pl);
			memcpy(observed.sn, boot->sn, sizeof(observed.sn));
			observed.has_sn = true;
			memcpy(observed.pubkey, boot->pub_key, sizeof(observed.pubkey));
			observed.has_pubkey = true;
		}
	}
	else if(station->TYPE == WsType::WS_TYPE_1001)
	{
		if(Ws1001Decoders::decode_meta_id(packet, &observed.id) != WsCommon::RetRes::OK)
		{
			return;
		}
		// WS1001 has no ADV/BOOT distinction; every verified data packet is
		// sufficient for the legacy pairing flow.
		observed.pairable = true;
	}
	else
	{
		return;
	}

	lock_recent();
	int existing_idx = -1;
	for(size_t i = 0; i < RECENT_STATIONS_CAPACITY; i++)
	{
		if(_recent_stations[i].valid &&
		   _recent_stations[i].type == observed.type &&
		   _recent_stations[i].id == observed.id)
		{
			existing_idx = static_cast<int>(i);
			break;
		}
	}
	if(existing_idx >= 0)
	{
		if(!observed.has_sn && _recent_stations[existing_idx].has_sn)
		{
			memcpy(observed.sn, _recent_stations[existing_idx].sn, sizeof(observed.sn));
			observed.has_sn = true;
		}
		if(!observed.has_pubkey && _recent_stations[existing_idx].has_pubkey)
		{
			memcpy(observed.pubkey, _recent_stations[existing_idx].pubkey, sizeof(observed.pubkey));
			observed.has_pubkey = true;
		}
		if(!observed.pairable && _recent_stations[existing_idx].pairable)
		{
			observed.pairable = true;
		}
		_recent_stations[existing_idx] = observed;
	}
	else
	{
		int target_idx = -1;
		uint32_t oldest_tick = UINT32_MAX;
		int oldest_idx = 0;
		for(size_t i = 0; i < RECENT_STATIONS_CAPACITY; i++)
		{
			if(!_recent_stations[i].valid)
			{
				target_idx = static_cast<int>(i);
				break;
			}
			if(_recent_stations[i].last_seen_tick_sec < oldest_tick)
			{
				oldest_tick = _recent_stations[i].last_seen_tick_sec;
				oldest_idx = static_cast<int>(i);
			}
		}
		if(target_idx < 0)
		{
			target_idx = oldest_idx;
		}
		_recent_stations[target_idx] = observed;
	}
	unlock_recent();
}

PairSource resolve_active_pair_source()
{
	PairSource requested = Settings::get_pair_source();
	if(requested == PairSource::PAIR_SOURCE_CLOUD)
	{
		if(Settings::cloud_ws_profile_ready())
		{
			return PairSource::PAIR_SOURCE_CLOUD;
		}
		if(Settings::local_ws_profile_ready())
		{
			return PairSource::PAIR_SOURCE_LOCAL;
		}
		return PairSource::PAIR_SOURCE_LOCAL;
	}
	else
	{
		// Default / LOCAL requested
		if(Settings::local_ws_profile_ready())
		{
			return PairSource::PAIR_SOURCE_LOCAL;
		}
		if(Settings::cloud_ws_profile_ready())
		{
			return PairSource::PAIR_SOURCE_CLOUD;
		}
		return PairSource::PAIR_SOURCE_LOCAL;
	}
}

bool load_station_profile(PairSource source, WsType *type, WsBase::PairData *pair_data)
{
	if(type == nullptr || pair_data == nullptr)
	{
		return false;
	}
	init_pair_data(pair_data);

	if(source == PairSource::PAIR_SOURCE_CLOUD)
	{
		*type = Settings::get_cloud_ws_type();
		if(!Settings::cloud_ws_profile_ready())
		{
			return false;
		}
		pair_data->id = (uint32_t)Settings::get_cloud_ws_id();
		Settings::get_cloud_ws_sn(pair_data->sn, sizeof(pair_data->sn));
		Settings::get_cloud_ws_pubkey(pair_data->pubkey, sizeof(pair_data->pubkey));
		return true;
	}

	*type = Settings::get_ws_type();
	if(!Settings::local_ws_profile_ready())
	{
		return false;
	}
	pair_data->id = (uint32_t)Settings::get_ws_id();
	Settings::get_ws_sn(pair_data->sn, sizeof(pair_data->sn));
	Settings::get_ws_pubkey(pair_data->pubkey, sizeof(pair_data->pubkey));
	return true;
}

RetResult init_state()
{
	// A re-init can change the paired station underneath us (e.g. cloud sync
	// assigning a different SN). Flush buffered weather so data from the old
	// station can never be uplinked attributed to the new one.
	uint32_t prev_pair_id = _state.pair_data.id;
	PairState prev_pair_state = _state.pair_state;
	PairSource prev_pair_source = _active_pair_source;

	_state.activity_status = ActivityStatus::NONE;
	_state.activity_status_changed_ts_sec = Utils::get_tick_sec();
	_state.is_listening = false;
	_state.last_packet_status = PktStatus::IGNORE;
	_state.packet_reception = 0;
	_state.lost_timeslot_streak = 0;
	PacketReception::reset(&_packet_reception_counter_window);
	_state.station_tx_power = 0;
	_state.requested_tx_power = 0;
	_state.station_rssi = 0;
	_state.station_snr = 0;
	_state.station_signal_available = false;
	_state.gps_last_req_ts_sec = 0;
	_bad_data_streak = 0;
	_is_fallback_latch = false;

	PairSource selected_source = resolve_active_pair_source();
	WsType init_type = WsType::WS_TYPE_NONE;
	WsBase::PairData selected_pair_data = {};
	bool selected_profile_ready =
		load_station_profile(selected_source, &init_type, &selected_pair_data);

	if (Settings::get_pair_mode() == PairMode::PAIR_MODE_SCAN)
	{
		selected_profile_ready = false;
	}

#if FALLBACK_TEST_PHASE_4_ONLY
	if (_fallback_phase != AutoFallbackPhase::PHASE_LATCHED)
	{
		selected_profile_ready = false;
		init_type = WsType::WS_TYPE_1300;
	}
#endif

	// Fresh (re)pair starts without a learned legacy-base fallback. Preserve
	// a just-learned fallback across the settings commit fired by auto-pair.
	if(!selected_profile_ready || selected_source != prev_pair_source)
	{
		_paired_base_fallback = false;
		_paired_channel_locked = false;
		_paired_freq_confirmed = false;
		_paired_confirmed_on_base = false;
	}
	_paired_station_interval_sec = 20;
	_paired_miss_streak = 0;

	if(!selected_profile_ready)
	{
#if FALLBACK_TEST_PHASE_4_ONLY
		g_scan_profile = WsType::WS_TYPE_1300;
		init_type = WsType::WS_TYPE_1300;
		_scan_phase = ScanPhase::WS1300;
		g_sniff_slot = 1;
#else
		WsType conf_type = (g_scan_profile != WsType::WS_TYPE_NONE) ? g_scan_profile : Settings::get_ws_type();
		g_scan_profile = (conf_type == WsType::WS_TYPE_1300) ? WsType::WS_TYPE_1300
					   : ((conf_type == WsType::WS_TYPE_ALL) ? WsType::WS_TYPE_ALL
					   : WsType::WS_TYPE_1001);
		init_type = (g_scan_profile == WsType::WS_TYPE_1300) ? WsType::WS_TYPE_1300 : WsType::WS_TYPE_1001;
		_scan_phase = (g_scan_profile == WsType::WS_TYPE_1300) ? ScanPhase::WS1300 : ScanPhase::WS1001;
		if (g_scan_profile == WsType::WS_TYPE_1300)
		{
			g_sniff_slot = 1;
		}
		else
		{
			g_sniff_slot = 0;
		}
#endif
		_scan_cycle_count = 0;
		_scan_completed = false;
		_ws1001_scan_start_ms = Utils::get_tick_ms();
		_sniff_slot_tuned_ts_ms = Utils::get_tick_ms();
		_sniff_slot_hold_until_ms = 0;
		if (Settings::get_pair_mode() == PairMode::PAIR_MODE_SCAN)
		{
			g_sniff_mode_enabled = true;
			g_auto_pair_enabled = (g_scan_target_id != 0);
		}
	}
	else if (init_type == WsType::WS_TYPE_NONE) {
		init_type = WsType::WS_TYPE_1001;
	} else if (init_type == WsType::WS_TYPE_ALL && selected_profile_ready) {
		init_type = WsType::WS_TYPE_1300;
		ESP_LOGW(TAG, "Using WS1300 for paired profile with invalid WS_TYPE_ALL");
	}
	set_cur_station_obj(init_type);

	g_receiver_profile = init_type;
	init_pair_data(&_state.pair_data);
	_active_pair_source = selected_source;

	if(selected_profile_ready)
	{
		memcpy(&_state.pair_data, &selected_pair_data, sizeof(_state.pair_data));
		ESP_LOGI(TAG, "Loaded %s station profile, ID=0x%08X",
			selected_source == PairSource::PAIR_SOURCE_CLOUD ? "cloud" : "local",
			(unsigned int)_state.pair_data.id);
		_state.pair_state = PairState::PAIRED;

		if(get_cur_station() != NULL && _state.cur_station->TYPE == WsType::WS_TYPE_1300)
		{
			// Listen where the station was last confirmed (_paired_base_fallback
			// is preserved across settings re-inits; on cold boot it starts
			// false = serial-derived channel, where all v11+ stations live).
			// The probe in packet_handler_task finds legacy pre-v11 stations
			// on the base frequency if the channel stays silent.
			ESP_LOGI(TAG, "Paired WS1300: listening on %s, frequency probe %s.",
				_paired_base_fallback ? "legacy base frequency" : "serial-derived channel",
				_paired_channel_locked ? "locked" : "armed");
		}
	}
	else
	{
		_state.pair_state = PairState::UNPAIRED;
	}

	if(prev_pair_state == PairState::PAIRED &&
	   (_state.pair_state != PairState::PAIRED ||
	    _state.pair_data.id != prev_pair_id ||
	    _active_pair_source != prev_pair_source))
	{
		ESP_LOGW(TAG, "Paired station changed/removed on re-init: flushing weather buffers");
		WsData::clear_recv();
		WsData::clear_aggregates();
	}

	if(set_radio_freq(Settings::get_freq_region()) != RET_OK)
	{
		ESP_LOGE(TAG, "Set freq error");
		return RET_ERROR;
	}

	// If booting unpaired with sniffing enabled, start the sniff sweep tuning
	// right away instead of waiting for the first packet timeout
	retune_for_current_state();

	sx126x_rx_cont();

	return RET_OK;
}

void event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
	if(NULL == event_data)
	{
		return;
	}
	if(id == (int32_t)GlobalEvents::EventId::SETTINGS_CHANGED)
	{
		if(*((GlobalEvents::SettingsChangedEvent *)event_data) & (u32)Settings::Category::WS)
		{
			lock_state();
			init_state();

			GlobalEvents::WsInfoChangedEvent info_changed_evt = {};
			info_changed_evt.station = get_cur_station();
			info_changed_evt.pair_state = _state.pair_state;
			memcpy(&info_changed_evt.pair_data, &_state.pair_data, sizeof(info_changed_evt.pair_data));
			info_changed_evt.freq_desc = get_freq_desc(nullptr, Settings::get_freq_region());
			GlobalEvents::publish(GlobalEvents::EventId::WS_INFO_CHANGED, &info_changed_evt, sizeof(info_changed_evt));

			unlock_state();
			if (Settings::get_pair_mode() == PairMode::PAIR_MODE_SCAN)
			{
				if (radio_lock(RADIO_LOCK_TIMEOUT_MS))
				{
					tune_sniff_slot();
					sx126x_rx_cont();
					radio_unlock();
				}
			}
			// A task notification is retained if it arrives just before the packet
			// handler starts waiting. vTaskResume() is not: SCAN could therefore
			// lose this wake-up and leave the sweep frozen on its first slot.
			if(_packet_handler_task_handle != NULL)
			{
				xTaskNotifyGive(_packet_handler_task_handle);
			}
		}
	}
}

/******************************************************************************
 * @brief Initialize weather station handler
 * @retval RET_OK on success
 * @retval RET_ERROR on failure
 ******************************************************************************/
RetResult init()
{
	ESP_LOGI(TAG, "WsHandler::init: starting initialization");
	_packet_queue = xQueueCreate(5, sizeof(PacketQueueMsg));

	// Created before Radio.Init() so that no radio path can ever run unlocked.
	// Recursive: set_radio_freq/tune_sniff_slot/retune_for_current_state/
	// sx126x_rx_cont all take it and all call each other.
	_radio_mutex = xSemaphoreCreateRecursiveMutex();
	if(_radio_mutex == NULL)
	{
		ESP_LOGE(TAG, "Failed to create radio ownership mutex");
		return RET_ERROR;
	}

#ifdef CONFIG_SCREENLESS
	KissClient::init(KISS_UART_TX_PIN, KISS_UART_RX_PIN, KISS_UART_CTS_PIN, KISS_UART_RTS_PIN);
	xTaskCreatePinnedToCore(kiss_rx_task, "kiss_rx_task", 4096, NULL, 5, NULL, 1);
#else
	_radio_events.RxDone = sx126x_rx_cb;
	_radio_events.TxDone = sx126x_tx_cb;
	// A timed-out TX leaves the radio in standby; recover exactly like a
	// completed TX (retune to the paired frequency and re-enter RX), or the
	// gateway goes deaf until the next frequency probe
	_radio_events.TxTimeout = sx126x_tx_cb;
	_radio_events.CadDone = sx126x_cad_cb;
	ESP_LOGI(TAG, "WsHandler::init: calling Radio.Init");
	Radio.Init(&_radio_events);
	ESP_LOGI(TAG, "WsHandler::init: Radio.Init completed");
#endif

	_state_mutex = xSemaphoreCreateRecursiveMutexStatic(&_state_mutex_buff);
	_recent_mutex = xSemaphoreCreateMutexStatic(&_recent_mutex_buff);
	GlobalEvents::subscribe(GlobalEvents::EventId::SETTINGS_CHANGED, event_handler, NULL);

	// Initialize WS data buffer
	WsData::init();
	WsGps::init();
	RemoteObservationQueue::init();

	// Event group
	_evg_handle = xEventGroupCreateStatic(&_evg_buffer);
	if(_evg_handle == NULL)
	{
		Utils::die();
		return RET_ERROR;
	}

	lock_state();

	// Station pairing priority on boot / post-OTA (5-5-5-5 fallback state machine):
	// Condition 3: Local profile exists (previously working station) -> Phase 1 (Local NVS)
	// Condition 4: Only Cloud profile exists -> Phase 2 (Cloud NVS)
	// Condition 2: Cloud station type is provisioned (WS1001 or WS1300) without specific ID -> Phase 3 (Targeted Scan)
	// Condition 1: Both empty (out of the box) -> Phase 4 (Universal Scan, WS1001 prioritized)
	PairMode boot_mode = Settings::get_pair_mode();
	PairSource boot_source = PairSource::PAIR_SOURCE_LOCAL;
	bool has_local = Settings::local_ws_profile_ready();
	bool has_cloud = Settings::cloud_ws_profile_ready();
	uint16_t cloud_type = Settings::get_cloud_ws_type();
	bool boot_paired = false;

#if FALLBACK_TEST_PHASE_4_ONLY
	_fallback_phase = AutoFallbackPhase::PHASE_WS1300_SWEEP;
	boot_source = PairSource::PAIR_SOURCE_LOCAL;
	boot_mode = PairMode::PAIR_MODE_SCAN;
	boot_paired = false;
	g_scan_profile = WsType::WS_TYPE_1300;
	g_sniff_slot = 1;
	_sniff_slot_tuned_ts_ms = Utils::get_tick_ms();
	Settings::set_pair_source(PairSource::PAIR_SOURCE_LOCAL);
	Settings::set_pair_mode(boot_mode);
	ESP_LOGW(TAG, "FALLBACK_TEST_PHASE_4_ONLY: Boot fallback -> Phase 4 WS1300 Sweep (CH0-CH7, 2 min per channel)");
#else
	if ((boot_mode == PairMode::PAIR_MODE_CLOUD || Settings::get_pair_source() == PairSource::PAIR_SOURCE_CLOUD) && has_cloud)
	{
		_fallback_phase = AutoFallbackPhase::PHASE_CLOUD_NVS;
		boot_source = PairSource::PAIR_SOURCE_CLOUD;
		if (boot_mode != PairMode::PAIR_MODE_OFF)
		{
			boot_mode = PairMode::PAIR_MODE_CLOUD;
		}
		boot_paired = true;
		Settings::set_pair_source(PairSource::PAIR_SOURCE_CLOUD);
		Settings::set_pair_mode(boot_mode);
		ESP_LOGI(TAG, "Boot fallback (Phase 2 preferred): Using Cloud profile (0x%08X)",
			(unsigned int)Settings::get_cloud_ws_id());
	}
	else if (has_local)
	{
		_fallback_phase = AutoFallbackPhase::PHASE_LOCAL_NVS;
		boot_source = PairSource::PAIR_SOURCE_LOCAL;
		if (boot_mode != PairMode::PAIR_MODE_OFF)
		{
			boot_mode = PairMode::PAIR_MODE_LOCAL;
		}
		boot_paired = true;
		Settings::set_pair_source(PairSource::PAIR_SOURCE_LOCAL);
		Settings::set_pair_mode(boot_mode);
		ESP_LOGI(TAG, "Boot fallback (Phase 1): Using Local NVS profile (0x%08X)",
			(unsigned int)Settings::get_ws_id());
	}
	else if (has_cloud)
	{
		_fallback_phase = AutoFallbackPhase::PHASE_CLOUD_NVS;
		boot_source = PairSource::PAIR_SOURCE_CLOUD;
		if (boot_mode != PairMode::PAIR_MODE_OFF)
		{
			boot_mode = PairMode::PAIR_MODE_CLOUD;
		}
		boot_paired = true;
		Settings::set_pair_source(PairSource::PAIR_SOURCE_CLOUD);
		Settings::set_pair_mode(boot_mode);
		ESP_LOGI(TAG, "Boot fallback (Phase 2): Using Cloud profile (0x%08X)",
			(unsigned int)Settings::get_cloud_ws_id());
	}
	else
	{
#if !FALLBACK_ENABLE_PHASE_3_WS1001
		_fallback_phase = AutoFallbackPhase::PHASE_WS1300_SWEEP;
		boot_source = PairSource::PAIR_SOURCE_LOCAL;
		if (boot_mode != PairMode::PAIR_MODE_OFF)
		{
			boot_mode = PairMode::PAIR_MODE_SCAN;
		}
		boot_paired = false;
		g_scan_profile = WsType::WS_TYPE_1300;
		g_sniff_slot = 1;
		Settings::set_pair_source(PairSource::PAIR_SOURCE_LOCAL);
		Settings::set_pair_mode(boot_mode);
		ESP_LOGI(TAG, "Boot fallback: Empty profiles -> WS1300 Dedicated Sweep (Phase 3 bypassed)");
#else
		_fallback_phase = AutoFallbackPhase::PHASE_WS1001_SCAN;
		boot_source = PairSource::PAIR_SOURCE_LOCAL;
		if (boot_mode != PairMode::PAIR_MODE_OFF)
		{
			boot_mode = PairMode::PAIR_MODE_SCAN;
		}
		boot_paired = false;
		g_scan_profile = WsType::WS_TYPE_1001;
		g_sniff_slot = 0;
		Settings::set_pair_source(PairSource::PAIR_SOURCE_LOCAL);
		Settings::set_pair_mode(boot_mode);
		ESP_LOGI(TAG, "Boot fallback: Empty profiles -> WS1001 Dedicated Scan on Base (868.0 MHz FSK)");
#endif
	}
#endif
	_fallback_phase_start_ms = Utils::get_tick_ms();

	switch(boot_mode)
	{
		case PairMode::PAIR_MODE_OFF:
			// OFF is true receive-only monitoring. Keep the saved profile and
			// station tuning, but route packets through the sniff decoder so no
			// ACK/CMD downlink is transmitted.
			g_sniff_mode_enabled = true;
			g_auto_pair_enabled = false;
			break;
		case PairMode::PAIR_MODE_LOCAL:
		case PairMode::PAIR_MODE_CLOUD:
			g_sniff_mode_enabled = !boot_paired;
			g_auto_pair_enabled = !boot_paired;
			break;
		case PairMode::PAIR_MODE_SCAN:
		default:
			g_sniff_mode_enabled = true;
			g_auto_pair_enabled = (g_scan_target_id != 0);
			break;
	}

	if (!boot_paired && boot_mode != PairMode::PAIR_MODE_OFF)
	{
		_scan_cycle_count = 0;
		if (g_scan_profile == WsType::WS_TYPE_1300)
		{
			_scan_phase = ScanPhase::WS1300;
			set_cur_station_obj(WsType::WS_TYPE_1300);
			g_receiver_profile = WsType::WS_TYPE_1300;
			g_sniff_slot = 1;
			_sniff_slot_tuned_ts_ms = Utils::get_tick_ms();
			ESP_LOGI(TAG, "Booting unpaired: entering targeted scan for WS1300 (slot %u)", (unsigned int)g_sniff_slot);
		}
		else
		{
			_scan_phase = ScanPhase::WS1001;
			_ws1001_scan_start_ms = Utils::get_tick_ms();
			set_cur_station_obj(WsType::WS_TYPE_1001);
			g_receiver_profile = WsType::WS_TYPE_1001;
			g_sniff_slot = 0;
			g_sniff_channel = 0;
			ESP_LOGI(TAG, "Booting unpaired: entering auto-scan mode for %s",
				(g_scan_profile == WsType::WS_TYPE_1001) ? "WS1001 (locked)" : "WS1001/WS1300 (combo)");
		}
	}

	ESP_LOGI(TAG, "WsHandler::init: calling init_state");
	init_state();

	if (_state.pair_state == PairState::PAIRED && boot_mode != PairMode::PAIR_MODE_OFF)
	{
		// Fresh boot: automatically arm GPS challenge to confirm station ownership
		trigger_gps_request();
		if (Settings::is_station_ownership_confirmed() &&
		    Settings::get_station_ownership_id() == _state.pair_data.id)
		{
			g_ownership_confirmed = true;
			ESP_LOGI(TAG, "Station 0x%08X ownership previously confirmed in NVS (ts: %u); re-verifying on boot",
				(unsigned int)_state.pair_data.id,
				(unsigned int)Settings::get_station_ownership_timestamp());
		}
		else
		{
			ESP_LOGI(TAG, "Station 0x%08X armed for ownership challenge on boot",
				(unsigned int)_state.pair_data.id);
		}
	}

	unlock_state();

	_packet_handler_task_handle = xTaskCreateStaticPinnedToCore(packet_handler_task, PACKET_HANDLER_TASK_NAME, sizeof(_packet_handler_task_stack), NULL,
		PACKET_HANDLER_TASK_PRIORITY, _packet_handler_task_stack, &_packet_handler_task_buff, 1);
	if(_packet_handler_task_handle == NULL)
	{
		ESP_LOGE(TAG, "Could not create packet handler task.");
		return RET_ERROR;
	}

	ESP_LOGI(TAG, "WsHandler::init: successfully completed");
	return RET_OK;
}

static void process_fallback_state_machine(ActivityStatus cur_status)
{
	if (_fallback_phase == AutoFallbackPhase::PHASE_MANUAL)
	{
		return;
	}

	uint64_t now_ms = Utils::get_tick_ms();

#if FALLBACK_TEST_PHASE_4_ONLY
	if (_fallback_phase == AutoFallbackPhase::PHASE_LATCHED)
	{
		if (cur_status == ActivityStatus::INACTIVE)
		{
			ESP_LOGW(TAG, "FALLBACK_TEST_PHASE_4_ONLY: Latched station silent for 5 min, returning to Phase 4 WS1300 sweep");
			_fallback_phase = AutoFallbackPhase::PHASE_WS1300_SWEEP;
			_fallback_phase_start_ms = now_ms;
			g_sniff_slot = 1;
			_sniff_slot_tuned_ts_ms = now_ms;
			apply_pair_mode(PairMode::PAIR_MODE_SCAN, 0, -1, WsType::WS_TYPE_1300);
		}
		return;
	}

	if (cur_status == ActivityStatus::ACTIVE_OK || cur_status == ActivityStatus::ACTIVE_BUSY)
	{
		_fallback_phase = AutoFallbackPhase::PHASE_LATCHED;
		ESP_LOGI(TAG, "Fallback: Target station latched successfully!");
		return;
	}

	uint64_t elapsed_ms = (now_ms >= _fallback_phase_start_ms) ? (now_ms - _fallback_phase_start_ms) : 0;

	if (_fallback_phase == AutoFallbackPhase::PHASE_WS1300_SWEEP)
	{
		if (elapsed_ms >= FALLBACK_WS1300_SWEEP_DURATION_MS)
		{
			ESP_LOGW(TAG, "FALLBACK_TEST_PHASE_4_ONLY: Phase 4 16-min sweep expired; restarting Phase 4 sweep from CH0");
			_fallback_phase_start_ms = now_ms;
			g_sniff_slot = 1;
			_sniff_slot_tuned_ts_ms = now_ms;
			apply_pair_mode(PairMode::PAIR_MODE_SCAN, 0, -1, WsType::WS_TYPE_1300);
		}
	}
	else
	{
		_fallback_phase = AutoFallbackPhase::PHASE_WS1300_SWEEP;
		_fallback_phase_start_ms = now_ms;
		g_sniff_slot = 1;
		_sniff_slot_tuned_ts_ms = now_ms;
		apply_pair_mode(PairMode::PAIR_MODE_SCAN, 0, -1, WsType::WS_TYPE_1300);
	}
	return;
#else

	// If currently latched and receiving packets, keep state latched and refresh start timestamp
	if (_fallback_phase == AutoFallbackPhase::PHASE_LATCHED)
	{
		if (cur_status == ActivityStatus::INACTIVE)
		{
			// Previously latched station is silent for 5 minutes (WS_LOST_TIMEOUT_MS)
			// Only alternate between configured NVS profiles; never auto-pair a neighbor station when a profile is already configured.
			if (_active_pair_source == PairSource::PAIR_SOURCE_LOCAL &&
			    Settings::cloud_ws_profile_ready() &&
			    Settings::get_cloud_ws_id() != Settings::get_ws_id())
			{
				ESP_LOGW(TAG, "Fallback: Active Local station silent for 5 min, failing over to Cloud NVS target");
				_fallback_phase = AutoFallbackPhase::PHASE_CLOUD_NVS;
				_fallback_phase_start_ms = now_ms;
				apply_pair_mode(PairMode::PAIR_MODE_CLOUD, (uint32_t)Settings::get_cloud_ws_id());
			}
			else if (_active_pair_source == PairSource::PAIR_SOURCE_CLOUD &&
			         Settings::local_ws_profile_ready() &&
			         Settings::get_ws_id() != Settings::get_cloud_ws_id())
			{
				ESP_LOGW(TAG, "Fallback: Active Cloud station silent for 5 min, failing over to Local NVS target");
				_fallback_phase = AutoFallbackPhase::PHASE_LOCAL_NVS;
				_fallback_phase_start_ms = now_ms;
				apply_pair_mode(PairMode::PAIR_MODE_LOCAL, (uint32_t)Settings::get_ws_id());
			}
			else if (Settings::local_ws_profile_ready() || Settings::cloud_ws_profile_ready())
			{
				// Stay locked on configured NVS target without entering blind WS1001/WS1300 auto-pair scan
				_fallback_phase = (_active_pair_source == PairSource::PAIR_SOURCE_CLOUD)
					? AutoFallbackPhase::PHASE_CLOUD_NVS
					: AutoFallbackPhase::PHASE_LOCAL_NVS;
				_fallback_phase_start_ms = now_ms;
			}
			else
			{
#if !FALLBACK_ENABLE_PHASE_3_WS1001
				ESP_LOGW(TAG, "Fallback: Active station silent for 5 min, advancing to Phase 4 WS1300 Sweep (Phase 3 bypassed)");
				_fallback_phase = AutoFallbackPhase::PHASE_WS1300_SWEEP;
				_fallback_phase_start_ms = now_ms;
				g_sniff_slot = 1; // start on CH0 (slot 1)
				apply_pair_mode(PairMode::PAIR_MODE_SCAN, 0, -1, WsType::WS_TYPE_1300);
#else
				ESP_LOGW(TAG, "Fallback: Active station silent for 5 min, failing over to Phase 3 WS1001 Dedicated Scan");
				_fallback_phase = AutoFallbackPhase::PHASE_WS1001_SCAN;
				_fallback_phase_start_ms = now_ms;
				apply_pair_mode(PairMode::PAIR_MODE_SCAN, 0, 0, WsType::WS_TYPE_1001);
#endif
			}
		}
		return;
	}

	// If we are not latched, check if a target is now active/healthy
	if (cur_status == ActivityStatus::ACTIVE_OK || cur_status == ActivityStatus::ACTIVE_BUSY)
	{
		_fallback_phase = AutoFallbackPhase::PHASE_LATCHED;
		ESP_LOGI(TAG, "Fallback: Target station latched successfully!");
		return;
	}

	uint64_t elapsed_ms = (now_ms >= _fallback_phase_start_ms) ? (now_ms - _fallback_phase_start_ms) : 0;

	switch (_fallback_phase)
	{
		case AutoFallbackPhase::PHASE_LOCAL_NVS:
		{
			if (elapsed_ms >= FALLBACK_PHASE_DURATION_MS)
			{
				_fallback_phase_start_ms = now_ms;
				if (Settings::cloud_ws_profile_ready() &&
				    Settings::get_cloud_ws_id() != Settings::get_ws_id())
				{
					_fallback_phase = AutoFallbackPhase::PHASE_CLOUD_NVS;
					ESP_LOGI(TAG, "Fallback: Phase 1 (Local NVS) 5-min window expired; checking Phase 2 (Cloud NVS: 0x%08X)",
						(unsigned int)Settings::get_cloud_ws_id());
					apply_pair_mode(PairMode::PAIR_MODE_CLOUD, (uint32_t)Settings::get_cloud_ws_id());
				}
				else if (Settings::local_ws_profile_ready())
				{
					// Stay locked on configured Local NVS station; do not enter blind auto-pair scan that would overwrite Local NVS
					ESP_LOGI(TAG, "Fallback: Staying locked on configured Local NVS station (0x%08X)",
						(unsigned int)Settings::get_ws_id());
				}
				else
				{
#if !FALLBACK_ENABLE_PHASE_3_WS1001
					_fallback_phase = AutoFallbackPhase::PHASE_WS1300_SWEEP;
					ESP_LOGW(TAG, "Fallback: Advancing directly to Phase 4 (WS1300 Sweep, 16 min) [Phase 3 bypassed]");
					g_sniff_slot = 1; // start on CH0 (slot 1)
					apply_pair_mode(PairMode::PAIR_MODE_SCAN, 0, -1, WsType::WS_TYPE_1300);
#else
					_fallback_phase = AutoFallbackPhase::PHASE_WS1001_SCAN;
					ESP_LOGI(TAG, "Fallback: Advancing to Phase 3 (Dedicated WS1001 Scan on Base 868.0 MHz FSK, 5 min)");
					apply_pair_mode(PairMode::PAIR_MODE_SCAN, 0, 0, WsType::WS_TYPE_1001);
#endif
				}
			}
			break;
		}

		case AutoFallbackPhase::PHASE_CLOUD_NVS:
		{
			if (elapsed_ms >= FALLBACK_PHASE_DURATION_MS)
			{
				_fallback_phase_start_ms = now_ms;
				if (Settings::local_ws_profile_ready() &&
				    Settings::get_ws_id() != Settings::get_cloud_ws_id())
				{
					_fallback_phase = AutoFallbackPhase::PHASE_LOCAL_NVS;
					ESP_LOGI(TAG, "Fallback: Phase 2 (Cloud NVS) 5-min window expired; checking Phase 1 (Local NVS: 0x%08X)",
						(unsigned int)Settings::get_ws_id());
					apply_pair_mode(PairMode::PAIR_MODE_LOCAL, (uint32_t)Settings::get_ws_id());
				}
				else if (Settings::cloud_ws_profile_ready())
				{
					// Stay locked on configured Cloud NVS station; do not enter blind auto-pair scan that would overwrite Local NVS
					ESP_LOGI(TAG, "Fallback: Staying locked on configured Cloud NVS station (0x%08X)",
						(unsigned int)Settings::get_cloud_ws_id());
				}
				else
				{
#if !FALLBACK_ENABLE_PHASE_3_WS1001
					_fallback_phase = AutoFallbackPhase::PHASE_WS1300_SWEEP;
					ESP_LOGW(TAG, "Fallback: Advancing directly to Phase 4 (WS1300 Sweep, 16 min) [Phase 3 bypassed]");
					g_sniff_slot = 1; // start on CH0 (slot 1)
					apply_pair_mode(PairMode::PAIR_MODE_SCAN, 0, -1, WsType::WS_TYPE_1300);
#else
					_fallback_phase = AutoFallbackPhase::PHASE_WS1001_SCAN;
					ESP_LOGI(TAG, "Fallback: Advancing to Phase 3 (Dedicated WS1001 Scan on Base 868.0 MHz FSK, 5 min)");
					apply_pair_mode(PairMode::PAIR_MODE_SCAN, 0, 0, WsType::WS_TYPE_1001);
#endif
				}
			}
			break;
		}

		case AutoFallbackPhase::PHASE_WS1001_SCAN:
		{
			if (elapsed_ms >= FALLBACK_PHASE_DURATION_MS)
			{
				ESP_LOGW(TAG, "Fallback: Phase 3 (Dedicated WS1001 Scan) 5-minute window expired with no packets.");
				_fallback_phase = AutoFallbackPhase::PHASE_WS1300_SWEEP;
				_fallback_phase_start_ms = now_ms;
				ESP_LOGI(TAG, "Fallback: Advancing to Phase 4 (Dedicated WS1300 Sweep: CH0-CH7, 2 min per channel, 16 min total)");
				g_sniff_slot = 1; // start on CH0 (slot 1)
				apply_pair_mode(PairMode::PAIR_MODE_SCAN, 0, -1, WsType::WS_TYPE_1300);
			}
			break;
		}

		case AutoFallbackPhase::PHASE_WS1300_SWEEP:
		{
			if (elapsed_ms >= FALLBACK_WS1300_SWEEP_DURATION_MS)
			{
				ESP_LOGW(TAG, "Fallback: Phase 4 (WS1300 Sweep) 16-minute window expired with no packets.");
				_fallback_phase = AutoFallbackPhase::PHASE_COOLDOWN;
				_fallback_phase_start_ms = now_ms;
				if (Settings::cloud_ws_profile_ready())
				{
					ESP_LOGI(TAG, "Fallback: Entering 30-minute cooldown parked on Cloud target (0x%08X)",
						(unsigned int)Settings::get_cloud_ws_id());
					apply_pair_mode(PairMode::PAIR_MODE_CLOUD, (uint32_t)Settings::get_cloud_ws_id());
				}
				else if (Settings::local_ws_profile_ready())
				{
					ESP_LOGI(TAG, "Fallback: Entering 30-minute cooldown parked on Local target (0x%08X)",
						(unsigned int)Settings::get_ws_id());
					apply_pair_mode(PairMode::PAIR_MODE_LOCAL, (uint32_t)Settings::get_ws_id());
				}
				else
				{
					ESP_LOGI(TAG, "Fallback: Entering 30-minute cooldown in idle sniff mode");
					apply_pair_mode(PairMode::PAIR_MODE_SCAN, 0, -1, WsType::WS_TYPE_ALL);
				}
			}
			break;
		}

		case AutoFallbackPhase::PHASE_COOLDOWN:
		{
			if (elapsed_ms >= FALLBACK_COOLDOWN_DURATION_MS)
			{
				ESP_LOGI(TAG, "Fallback: 30-minute cooldown complete (~1h full cycle finished); restarting recovery loop from Phase 1");
				reset_fallback_state_machine();
				if (_fallback_phase == AutoFallbackPhase::PHASE_LOCAL_NVS)
				{
					apply_pair_mode(PairMode::PAIR_MODE_LOCAL, (uint32_t)Settings::get_ws_id());
				}
				else if (_fallback_phase == AutoFallbackPhase::PHASE_CLOUD_NVS)
				{
					apply_pair_mode(PairMode::PAIR_MODE_CLOUD, (uint32_t)Settings::get_cloud_ws_id());
				}
				else
				{
#if !FALLBACK_ENABLE_PHASE_3_WS1001
					_fallback_phase = AutoFallbackPhase::PHASE_WS1300_SWEEP;
					g_sniff_slot = 1;
					apply_pair_mode(PairMode::PAIR_MODE_SCAN, 0, -1, WsType::WS_TYPE_1300);
#else
					_fallback_phase = AutoFallbackPhase::PHASE_WS1001_SCAN;
					apply_pair_mode(PairMode::PAIR_MODE_SCAN, 0, 0, WsType::WS_TYPE_1001);
#endif
				}
			}
			break;
		}

		default:
			break;
	}
#endif
}

/******************************************************************************
 * @brief Packet handler task. Handles packets received by lora IC
 * @retval RET_OK on success
 * @retval RET_ERROR on failure
 ******************************************************************************/
void packet_handler_task(void *data)
{
	while(get_cur_station() == NULL && !g_sniff_mode_enabled)
	{
		// Unlike suspend/resume, a direct task notification cannot be lost if
		// the enabling request races with this transition into the wait.
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
	}

	uint32_t next_wait_ms = get_cur_station() != NULL 
		? (get_cur_station()->BROADCAST_INTERVAL_SEC * 1000 + WS_PACKET_TIMEOUT_TOLERANCE_MS)
		: 30000;

	while(1)
	{		
		if (get_cur_station() == NULL && !g_sniff_mode_enabled)
		{
			while(get_cur_station() == NULL && !g_sniff_mode_enabled)
			{
				ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
			}
			next_wait_ms = get_cur_station() != NULL 
				? (get_cur_station()->BROADCAST_INTERVAL_SEC * 1000 + WS_PACKET_TIMEOUT_TOLERANCE_MS)
				: 30000;
		}

		if (g_sniff_mode_enabled && is_scan_active())
		{
			uint32_t active_dwell = (_scan_phase == ScanPhase::WS1300)
				? get_ws1300_channel_dwell_ms()
				: get_ws1001_channel_dwell_ms();
			if (next_wait_ms > active_dwell)
			{
				next_wait_ms = active_dwell;
			}
		}

		// Enforce scan dwell and dual-profile cycling (WS1001 -> WS1300 sweep -> WS1001...)
		// Received packets restart the queue wait, so wall-clock tracking ensures that
		// neither bystander packets nor short packet timeouts derail the dwell sequence.
		if (g_sniff_mode_enabled && g_manual_listen_slot < 0 &&
		    g_scan_target_slot < 0 &&
		    get_pair_state() == PairState::UNPAIRED &&
		    !_scan_completed)
		{
			uint64_t now_ms = Utils::get_tick_ms();
			if (_scan_phase == ScanPhase::WS1001)
			{
				uint32_t ws1001_dwell = get_ws1001_channel_dwell_ms();
				uint64_t hold_until_ms = _ws1001_scan_start_ms + ws1001_dwell;
				if (now_ms >= hold_until_ms)
				{
					if (g_scan_profile == WsType::WS_TYPE_1001)
					{
						_scan_cycle_count++;
						if (_scan_cycle_count >= 2 && Settings::get_pair_mode() == PairMode::PAIR_MODE_SCAN)
						{
							_scan_completed = true;
							ESP_LOGI(TAG, "WS1001 user scan sweeps complete (2 passes: quick + deep 5m). Waiting for user action.");
							continue;
						}
						_ws1001_scan_start_ms = Utils::get_tick_ms();
						lock_state();
						sx126x_rx_cont();
						unlock_state();
						hold_until_ms = _ws1001_scan_start_ms + get_ws1001_channel_dwell_ms();
						ESP_LOGI(TAG, "WS1001 scan window (cycle %u) refreshed; remaining on base frequency (WS1001-only mode)",
							(unsigned int)_scan_cycle_count);
					}
					else
					{
						ESP_LOGI(TAG, "WS1001 scan window (%u ms, cycle %u) exhausted; switching to WS1300 sweep",
							(unsigned int)ws1001_dwell, (unsigned int)_scan_cycle_count);
						_scan_phase = ScanPhase::WS1300;
						lock_state();
						set_cur_station_obj(WsType::WS_TYPE_1300);
						g_receiver_profile = WsType::WS_TYPE_1300;
						g_sniff_slot = 1;
						g_sniff_channel = 0;
						_sniff_slot_tuned_ts_ms = Utils::get_tick_ms();
						_sniff_slot_hold_until_ms = 0;
						tune_sniff_slot();
						sx126x_rx_cont();
						unlock_state();
						hold_until_ms = _sniff_slot_tuned_ts_ms + get_ws1300_channel_dwell_ms();
					}
				}
				uint32_t remaining_ms = (uint32_t)(hold_until_ms - Utils::get_tick_ms());
				if (remaining_ms < 50) remaining_ms = 50;
				if (next_wait_ms > remaining_ms)
				{
					next_wait_ms = remaining_ms;
				}
			}
			else // ScanPhase::WS1300
			{
				uint32_t hop_interval_ms = get_ws1300_channel_dwell_ms();
				uint64_t hold_until_ms = _sniff_slot_tuned_ts_ms + hop_interval_ms;
				if (_sniff_slot_hold_until_ms > hold_until_ms)
				{
					hold_until_ms = _sniff_slot_hold_until_ms;
				}
				if (now_ms >= hold_until_ms)
				{
					if (g_sniff_slot < 8)
					{
						g_sniff_slot++;
						_sniff_slot_hold_until_ms = 0;
						lock_state();
						tune_sniff_slot();
						sx126x_rx_cont();
						unlock_state();
						hold_until_ms = _sniff_slot_tuned_ts_ms + hop_interval_ms;
					}
					else
					{
						_scan_cycle_count++;
						if (_scan_cycle_count >= 2 && Settings::get_pair_mode() == PairMode::PAIR_MODE_SCAN)
						{
							_scan_completed = true;
							ESP_LOGI(TAG, "WS1300 user scan sweeps complete (Pass 1 quick + Pass 2 deep 5m). Found stations buffered. Waiting for user action.");
							continue;
						}
						if (g_scan_profile == WsType::WS_TYPE_1300)
						{
							uint8_t wrap_slot = 1;
							uint32_t new_hop_interval_ms = get_ws1300_channel_dwell_ms();
							ESP_LOGI(TAG, "WS1300 sweep complete (cycle %u); wrapping to slot %u (WS1300-only mode, dwell %u ms)",
								(unsigned int)_scan_cycle_count, (unsigned int)wrap_slot, (unsigned int)new_hop_interval_ms);
							g_sniff_slot = wrap_slot;
							_sniff_slot_tuned_ts_ms = Utils::get_tick_ms();
							_sniff_slot_hold_until_ms = 0;
							lock_state();
							tune_sniff_slot();
							sx126x_rx_cont();
							unlock_state();
							hold_until_ms = _sniff_slot_tuned_ts_ms + new_hop_interval_ms;
						}
						else
						{
							uint32_t new_ws1001_dwell = get_ws1001_channel_dwell_ms();
							ESP_LOGI(TAG, "WS1300 sweep complete (cycle %u); switching to WS1001 scan (dwell %u ms)",
								(unsigned int)_scan_cycle_count, (unsigned int)new_ws1001_dwell);
							_scan_phase = ScanPhase::WS1001;
							lock_state();
							set_cur_station_obj(WsType::WS_TYPE_1001);
							g_receiver_profile = WsType::WS_TYPE_1001;
							g_sniff_slot = 0;
							g_sniff_channel = 0;
							_ws1001_scan_start_ms = Utils::get_tick_ms();
							_sniff_slot_tuned_ts_ms = Utils::get_tick_ms();
							_sniff_slot_hold_until_ms = 0;
							tune_sniff_slot();
							sx126x_rx_cont();
							unlock_state();
							hold_until_ms = _ws1001_scan_start_ms + new_ws1001_dwell;
						}
					}
				}
				uint32_t remaining_ms = (uint32_t)(hold_until_ms - Utils::get_tick_ms());
				if (remaining_ms < 50) remaining_ms = 50;
				if (next_wait_ms > remaining_ms)
				{
					next_wait_ms = remaining_ms;
				}
			}
		}

		// Wait for packet-received event, sent by interrupt on DIO0
		ESP_LOGI(TAG, "Waiting %dms for packet...", next_wait_ms);
		bool ws_timeout = false;
		PktStatus received_packet_status = PktStatus::NONE;
		PacketCounterSample counter_sample = {};
		uint32_t wait_start_ts_ms = Utils::get_tick_ms();
		PacketQueueMsg packet_msg = {};
		bool packet_received =
			xQueueReceive(_packet_queue, &packet_msg, pdMS_TO_TICKS(next_wait_ms));
		// Both branches below update receiver state and both release the mutex
		// at the end. Previously only the packet branch took it, so every radio
		// timeout attempted to give a mutex this task did not own.
		lock_state();
		if(packet_received)
		{
			received_packet_status = handle_packet(&packet_msg, &counter_sample);

			if(received_packet_status != PktStatus::IGNORE)
			{
				// Heard our own station: current frequency (computed channel or
				// legacy base fallback) is the right one, stop probing
				_paired_miss_streak = 0;
				_last_valid_packet_rx_ms = Utils::get_tick_ms();
			}

			uint32_t wait_duration_ms = Utils::get_tick_ms() - wait_start_ts_ms;
			
			if(received_packet_status == PktStatus::OK_BUSY)
			{
				next_wait_ms = get_cur_station() != NULL 
					? (get_cur_station()->BUSY_DURATION_SEC * 1000 + 10000)
					: 30000;
			}
			else if(received_packet_status == PktStatus::IGNORE)
			{
				next_wait_ms = (wait_duration_ms > next_wait_ms) ? 0 : (next_wait_ms - wait_duration_ms); //checking safely for overflow
			}
			else
			{
				uint32_t interval_sec = get_cur_station() != NULL 
					? get_cur_station()->BROADCAST_INTERVAL_SEC 
					: 30;
				if (get_cur_station() != NULL && get_cur_station()->TYPE == WsType::WS_TYPE_1300)
				{
					interval_sec = _paired_station_interval_sec;
				}
				next_wait_ms = interval_sec * 1000 + WS_PACKET_TIMEOUT_TOLERANCE_MS;
			}
		}
		else
		{
			ESP_LOGE(TAG, "Wait for packet timeout.");
			ws_timeout = true;
			uint32_t interval_sec = get_cur_station() != NULL 
				? get_cur_station()->BROADCAST_INTERVAL_SEC 
				: 30;
			if (get_cur_station() != NULL && get_cur_station()->TYPE == WsType::WS_TYPE_1300)
			{
				if (_state.activity_status == ActivityStatus::NONE)
				{
					// Never heard the station yet (fresh boot/pair): keep the
					// waits short so the frequency probe cycles quickly. The
					// interval back-off below models a *known* station
					// stretching its TX interval on missed ACKs.
					interval_sec = 20;
				}
				else
				{
					// Scale up expected interval based on consecutive timeouts (miss streak)
					interval_sec = 20 + _paired_miss_streak * 20;
					if (interval_sec > 240) interval_sec = 240;
				}
				_paired_station_interval_sec = interval_sec;
			}
			next_wait_ms = interval_sec * 1000 + WS_PACKET_TIMEOUT_TOLERANCE_MS;

			// Scan mode dwell & sweep progression: only while UNPAIRED and no manual channel
			// lock. A paired gateway must never leave its station's frequency -
			// there, sniff mode only means opportunistically decoding other
			// stations heard on the same tuning.
			if (g_sniff_mode_enabled && g_manual_listen_slot < 0 &&
			    g_scan_target_slot < 0 &&
			    get_pair_state() == PairState::UNPAIRED &&
			    !_scan_completed)
			{
				uint64_t sweep_now_ms = Utils::get_tick_ms();
				if (_scan_phase == ScanPhase::WS1001)
				{
					uint32_t ws1001_dwell = get_ws1001_channel_dwell_ms();
					uint64_t sweep_hold_ms = _ws1001_scan_start_ms + ws1001_dwell;
					if (sweep_now_ms >= sweep_hold_ms)
					{
						if (g_scan_profile == WsType::WS_TYPE_1001)
						{
							_scan_cycle_count++;
							if (_scan_cycle_count >= 2 && Settings::get_pair_mode() == PairMode::PAIR_MODE_SCAN)
							{
								_scan_completed = true;
								ESP_LOGI(TAG, "WS1001 user scan sweeps complete (2 passes: quick + deep 5m). Waiting for user action.");
								next_wait_ms = 30000;
								continue;
							}
							if (_scan_cycle_count >= 2 && _fallback_phase == AutoFallbackPhase::PHASE_MANUAL)
							{
								if (Settings::local_ws_profile_ready())
								{
									ESP_LOGW(TAG, "WS1001 scan sweep (%u cycles) exhausted without finding new station. Restoring saved local station (0x%08X)",
										(unsigned int)_scan_cycle_count, (unsigned int)Settings::get_ws_id());
									unlock_state();
									apply_pair_mode(PairMode::PAIR_MODE_LOCAL, (uint32_t)Settings::get_ws_id());
									continue;
								}
								else if (Settings::cloud_ws_profile_ready())
								{
									ESP_LOGW(TAG, "WS1001 scan sweep (%u cycles) exhausted without finding new station. Restoring saved cloud station (0x%08X)",
										(unsigned int)_scan_cycle_count, (unsigned int)Settings::get_cloud_ws_id());
									unlock_state();
									apply_pair_mode(PairMode::PAIR_MODE_CLOUD, (uint32_t)Settings::get_cloud_ws_id());
									continue;
								}
							}
							_ws1001_scan_start_ms = Utils::get_tick_ms();
							lock_state();
							sx126x_rx_cont();
							unlock_state();
							next_wait_ms = get_ws1001_channel_dwell_ms();
							ESP_LOGI(TAG, "WS1001 scan window (cycle %u) refreshed; remaining on base frequency (WS1001-only mode)",
								(unsigned int)_scan_cycle_count);
						}
						else
						{
							ESP_LOGI(TAG, "WS1001 scan window (%u ms, cycle %u) exhausted; switching to WS1300 sweep",
								(unsigned int)ws1001_dwell, (unsigned int)_scan_cycle_count);
							_scan_phase = ScanPhase::WS1300;
							lock_state();
							set_cur_station_obj(WsType::WS_TYPE_1300);
							g_receiver_profile = WsType::WS_TYPE_1300;
							g_sniff_slot = 1;
							g_sniff_channel = 0;
							_sniff_slot_tuned_ts_ms = Utils::get_tick_ms();
							_sniff_slot_hold_until_ms = 0;
							tune_sniff_slot();
							sx126x_rx_cont();
							unlock_state();
							next_wait_ms = get_ws1300_channel_dwell_ms();
						}
					}
					else
					{
						next_wait_ms = (uint32_t)(sweep_hold_ms - sweep_now_ms);
					}
				}
				else // ScanPhase::WS1300
				{
					// Respect a post-kick hold: the kicked station is silent while it
					// reboots and its STA_BOOT must be caught on this channel
					uint32_t sweep_hop_ms = get_ws1300_channel_dwell_ms();
					uint64_t sweep_hold_ms = _sniff_slot_tuned_ts_ms + sweep_hop_ms;
					if (_sniff_slot_hold_until_ms > sweep_hold_ms)
					{
						sweep_hold_ms = _sniff_slot_hold_until_ms;
					}
					if (sweep_now_ms >= sweep_hold_ms)
					{
						if (g_sniff_slot < 8)
						{
							g_sniff_slot++;
							_sniff_slot_hold_until_ms = 0;
							lock_state();
							tune_sniff_slot();
							sx126x_rx_cont();
							unlock_state();
							next_wait_ms = sweep_hop_ms;
						}
						else
						{
							_scan_cycle_count++;
							if (_scan_cycle_count >= 2 && Settings::get_pair_mode() == PairMode::PAIR_MODE_SCAN)
							{
								_scan_completed = true;
								ESP_LOGI(TAG, "WS1300 user scan sweeps complete (Pass 1 quick + Pass 2 deep 5m). Found stations buffered. Waiting for user action.");
								next_wait_ms = 30000;
								continue;
							}
							if (_scan_cycle_count >= 2 && _fallback_phase == AutoFallbackPhase::PHASE_MANUAL)
							{
								if (Settings::local_ws_profile_ready())
								{
									ESP_LOGW(TAG, "Scan sweep (%u cycles) exhausted without finding new station. Restoring saved local station (0x%08X)",
										(unsigned int)_scan_cycle_count, (unsigned int)Settings::get_ws_id());
									unlock_state();
									apply_pair_mode(PairMode::PAIR_MODE_LOCAL, (uint32_t)Settings::get_ws_id());
									continue;
								}
								else if (Settings::cloud_ws_profile_ready())
								{
									ESP_LOGW(TAG, "Scan sweep (%u cycles) exhausted without finding new station. Restoring saved cloud station (0x%08X)",
										(unsigned int)_scan_cycle_count, (unsigned int)Settings::get_cloud_ws_id());
									unlock_state();
									apply_pair_mode(PairMode::PAIR_MODE_CLOUD, (uint32_t)Settings::get_cloud_ws_id());
									continue;
								}
							}
							if (g_scan_profile == WsType::WS_TYPE_1300)
							{
								uint8_t wrap_slot = 1;
								uint32_t new_hop_ms = get_ws1300_channel_dwell_ms();
								ESP_LOGI(TAG, "WS1300 sweep complete (cycle %u); wrapping to slot %u (WS1300-only mode, dwell %u ms)",
									(unsigned int)_scan_cycle_count, (unsigned int)wrap_slot, (unsigned int)new_hop_ms);
								g_sniff_slot = wrap_slot;
								_sniff_slot_tuned_ts_ms = Utils::get_tick_ms();
								_sniff_slot_hold_until_ms = 0;
								lock_state();
								tune_sniff_slot();
								sx126x_rx_cont();
								unlock_state();
								next_wait_ms = new_hop_ms;
							}
							else
							{
								uint32_t new_ws1001_dwell = get_ws1001_channel_dwell_ms();
								ESP_LOGI(TAG, "WS1300 sweep complete (cycle %u); switching to WS1001 scan (dwell %u ms)",
									(unsigned int)_scan_cycle_count, (unsigned int)new_ws1001_dwell);
								_scan_phase = ScanPhase::WS1001;
								lock_state();
								set_cur_station_obj(WsType::WS_TYPE_1001);
								g_receiver_profile = WsType::WS_TYPE_1001;
								g_sniff_slot = 0;
								g_sniff_channel = 0;
								_ws1001_scan_start_ms = Utils::get_tick_ms();
								_sniff_slot_tuned_ts_ms = Utils::get_tick_ms();
								_sniff_slot_hold_until_ms = 0;
								tune_sniff_slot();
								sx126x_rx_cont();
								unlock_state();
								next_wait_ms = new_ws1001_dwell;
							}
						}
					}
					else
					{
						next_wait_ms = (uint32_t)(sweep_hold_ms - sweep_now_ms);
					}
				}
			}

			// WS1001 watchdog: if the radio ever silently fell out of RX (e.g. after a
			// failed ACK transmit, missed packet, or SPI peripheral hiccup), re-arm it.
			if (get_pair_state() == PairState::PAIRED && g_manual_listen_slot < 0 &&
			    get_cur_station() != NULL && get_cur_station()->TYPE == WsType::WS_TYPE_1001)
			{
				_paired_miss_streak++;

				if (_paired_miss_streak >= 2)
				{
					sx126x_rx_cont();
				}

				// If silent for 15+ streaks (~5 minutes): refresh radio RX
				if (_paired_miss_streak >= 15 && (_paired_miss_streak % 15 == 0))
				{
					ESP_LOGW(TAG, "WS1001 silent for %d missed intervals; refreshing radio RX",
						_paired_miss_streak);
					if(radio_lock(RADIO_LOCK_TIMEOUT_MS))
					{
#ifndef CONFIG_SCREENLESS
						Radio.Standby();
#endif
						set_radio_freq(Settings::get_freq_region());
						sx126x_rx_cont();
						radio_unlock();
					}
				}
			}

			// Legacy WS1300 compatibility: prototype/early stations transmit on
			// the plain region base frequency (no per-serial channel offset).
			// If the paired station stays silent on its computed channel,
			// alternate between the channel and the base frequency until it is
			// heard, then stick with whatever frequency works.
			if (get_pair_state() == PairState::PAIRED && g_manual_listen_slot < 0 &&
			    get_cur_station() != NULL && get_cur_station()->TYPE == WsType::WS_TYPE_1300)
			{
				_paired_miss_streak++;

				// A previously confirmed station that has gone fully silent
				// may have changed frequency (e.g. reflashed from legacy
				// pre-v11 firmware to a channelized build): release the lock
				// so the base/channel probe can find it again.
				if (_paired_channel_locked && _state.activity_status == ActivityStatus::INACTIVE)
				{
					_paired_channel_locked = false;
					ESP_LOGW(TAG, "Paired station inactive, releasing frequency lock to re-probe");
				}

				// Defensive: if the radio ever silently fell out of RX (e.g. a
				// failed TX left it in standby), re-arm it. Radio.Rx on an
				// already-listening radio is harmless, and while the frequency
				// lock is held nothing else would ever restart RX.
				if (_paired_channel_locked && _paired_miss_streak >= 2)
				{
					sx126x_rx_cont();
				}

				// Stay locked on the known correct frequency if channel lock is acquired.
				// While the station has never been heard (ActivityStatus::NONE) the
				// waits are fixed at ~22 s: dwell 5 timeouts (~110 s) on the
				// serial-derived channel where v11+ stations live, but only 1
				// timeout on the legacy base probe - a legacy station ADVs
				// every 20 s there, so one wait is enough to catch it.
				int target_timeouts = PAIRED_FREQ_PROBE_TIMEOUTS;
				if (_state.activity_status == ActivityStatus::NONE)
				{
					target_timeouts = _paired_base_fallback ? 1 : 5;
				}
				else if (_paired_freq_confirmed)
				{
					// The station was already heard on a known frequency and
					// stations never migrate between base and channel: dwell
					// on the confirmed side, only glance at the other
					target_timeouts = (_paired_base_fallback == _paired_confirmed_on_base) ? 5 : 1;
				}
				if (!_paired_channel_locked && _paired_miss_streak >= target_timeouts)
				{
					_paired_miss_streak = 0;
					_paired_base_fallback = !_paired_base_fallback;
					ESP_LOGW(TAG, "Paired station silent, probing %s frequency",
						_paired_base_fallback ? "legacy base" : "serial-derived channel");
					set_radio_freq(Settings::get_freq_region());
					sx126x_rx_cont();
				}

				// While the probe is active keep the waits short so it converges
				// quickly. RX is continuous - a short wait only makes the probe
				// flip sooner, it cannot miss a packet on the current tuning.
				if (!_paired_channel_locked &&
				    next_wait_ms > (uint32_t)PAIRED_PROBE_WAIT_MAX_SEC * 1000)
				{
					next_wait_ms = (uint32_t)PAIRED_PROBE_WAIT_MAX_SEC * 1000;
					_paired_station_interval_sec = PAIRED_PROBE_WAIT_MAX_SEC;
				}
			}
		}

		set_last_packet_status(received_packet_status);
		_state.packet_reception = calculate_packet_reception(
			ws_timeout, received_packet_status, &counter_sample);
		ActivityStatus calculated_ws_status = calculate_ws_status(ws_timeout, received_packet_status, _state.packet_reception);
		set_activity_status(calculated_ws_status);

		if(calculated_ws_status == ActivityStatus::INACTIVE)
		{
			_state.packet_reception = 0;
			_ws_valid_packets_ts_buff_ms.lock();
			_ws_valid_packets_ts_buff_ms.clear();
			_ws_valid_packets_ts_buff_ms.unlock();
			PacketReception::reset(&_packet_reception_counter_window);
		}
		else if(calculated_ws_status == ActivityStatus::ACTIVE_BUSY)
		{
			_ws_valid_packets_ts_buff_ms.lock();
			_ws_valid_packets_ts_buff_ms.clear();
			_ws_valid_packets_ts_buff_ms.unlock();
			PacketReception::reset(&_packet_reception_counter_window);
		}

		unlock_state();

		ESP_LOGI(TAG, "Packet Reception: %d%%", get_packet_reception());
		ESP_LOGI(TAG, "Activity Status: %s", get_activity_status_title(get_activity_status()));

		process_fallback_state_machine(calculated_ws_status);
		// Meshtastic / MeshCore beacon. Driven from here, and only from here, because a
		// transmit retunes the shared SX1262 and the gateway is deaf to its own
		// station while it runs. Borrow the radio for auxiliary mesh telemetry ONLY
		// immediately after confirming a packet from our station, when the next station
		// transmission is a full interval away. Never borrow on timeout or ignored frames.
		PacketOutcome outcome = {
			.success = (received_packet_status == PktStatus::OK || received_packet_status == PktStatus::OK_BUSY),
			.paired_p2p_cycle = (packet_received && received_packet_status != PktStatus::IGNORE),
			.completion_timestamp_ms = (int64_t)(esp_timer_get_time() / 1000LL),
		};

		if (outcome.paired_p2p_cycle)
		{
			int64_t now_ms = (int64_t)(esp_timer_get_time() / 1000LL);
			uint32_t nominal_interval_ms = (get_cur_station() != NULL && get_cur_station()->TYPE == WsType::WS_TYPE_1300)
				? (_paired_station_interval_sec * 1000)
				: (get_cur_station() != NULL ? get_cur_station()->BROADCAST_INTERVAL_SEC * 1000 : 30000);
			int64_t station_arrival_ms = outcome.completion_timestamp_ms + nominal_interval_ms;
			int64_t lease_expiry_ms = station_arrival_ms - PRE_PACKET_GUARD_MS;
			int64_t lease_budget_ms = lease_expiry_ms - now_ms;

			if (lease_budget_ms >= 3000LL)
			{
				RadioLease lease = {
					.expiry_ms = lease_expiry_ms,
					.caller_id = "WsHandler"
				};
				MeshTelemetry::tick(lease, outcome);
			}
			else
			{
				ESP_LOGW(TAG, "Remaining cycle budget (%lld ms) too small for auxiliary mesh telemetry lease (need >= 3000ms before guard window)", (long long)lease_budget_ms);
			}

			// Subtract elapsed time spent in MeshTelemetry::tick so the next queue wait
			// matches the station's scheduled arrival deadline.
			int64_t elapsed_after_rx_ms = (int64_t)(esp_timer_get_time() / 1000LL) - outcome.completion_timestamp_ms;
			if (elapsed_after_rx_ms > 0)
			{
				if ((uint32_t)elapsed_after_rx_ms < next_wait_ms)
				{
					next_wait_ms -= (uint32_t)elapsed_after_rx_ms;
				}
				else
				{
					next_wait_ms = WS_PACKET_TIMEOUT_TOLERANCE_MS;
				}
			}
		}
	}
}

/******************************************************************************
 * @brief Handling function called by packet handling task. Passes packet handling to station's impl\
 * @param pkt_msg Message of packet received from lora IC in the msg queue
 * @retval OK 
 * @retval OK_BUSY if station reports that is busy
 * @retval IGNORE if packet is bad or came from other station
 * @retval BAD_DATA if packet contains bad data
 ******************************************************************************/
PktStatus handle_packet(PacketQueueMsg* pkt_msg, PacketCounterSample* counter_sample)
{
	if(counter_sample != nullptr)
	{
		*counter_sample = {};
	}
	PktStatus status = handle_packet_impl(pkt_msg, counter_sample);
	bool filtered = (status == PktStatus::IGNORE || status == PktStatus::BAD_DATA);
	WebServer::log_packet(pkt_msg->buffer, pkt_msg->length, pkt_msg->rssi, pkt_msg->snr, filtered);
	return status;
}

/******************************************************************************
 * @brief Try to attribute a packet to any supported station type (sniffing).
 * Validates, verifies (WS1001) and decodes against every entry in
 * STATION_LIST until one claims the packet.
 * @param pkt Received packet
 * @param matched_station Station type that claimed the packet
 * @param field_pack Decoded weather fields
 * @param gps_data Decoded GPS data
 * @retval true if a station type claimed and decoded the packet
 ******************************************************************************/
bool try_sniff_decode(WsCommon::Packet* pkt, WsBase::MultiStation** matched_station,
	WsFields::FieldPack* field_pack, WsGps::GpsData* gps_data)
{
	for (int s_idx = 0; s_idx < (sizeof(STATION_LIST)/sizeof(WsBase::MultiStation)); s_idx++)
	{
		WsCommon::RetRes val_ret = WsCommon::RetRes::ERROR;
		if (STATION_LIST[s_idx].TYPE == WsType::WS_TYPE_1001)
		{
			val_ret = Ws1001Helpers::packet_validate(pkt);
		}
		else if (STATION_LIST[s_idx].TYPE == WsType::WS_TYPE_1300)
		{
			val_ret = Ws1300Helpers::packet_validate(pkt);
		}

		if (val_ret != WsCommon::RetRes::OK)
		{
			continue;
		}

		// For WS1001, strictly verify the signature using derived keys /
		// secrets prefixes - validation alone is too permissive and would
		// claim WS1300 packets as WS1001
		if (STATION_LIST[s_idx].TYPE == WsType::WS_TYPE_1001)
		{
			if (Ws1001Helpers::packet_verify(pkt) != WsCommon::RetRes::OK)
			{
				ESP_LOGW(TAG, "Sniffed packet failed WS1001 signature verification. Ignoring.");
				continue;
			}
		}

		WsFields::FieldPack temp_f_pack = {};
		WsGps::GpsData temp_g_data = {};
		if (STATION_LIST[s_idx].packet_decoder(pkt, &temp_f_pack, &temp_g_data) == WsCommon::RetRes::OK)
		{
			*matched_station = (WsBase::MultiStation*)&STATION_LIST[s_idx];
			*field_pack = temp_f_pack;
			*gps_data = temp_g_data;
			return true;
		}
	}

	return false;
}

static void mbedtls_aes_ecb_fn(const uint8_t *input, uint8_t *output, size_t length, const uint8_t *key_16b, bool encrypt)
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

static void mbedtls_hmac_sha256_fn(const uint8_t *data, size_t data_len, const uint8_t *key_16b, uint8_t *out_mac_32b)
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

static bool decode_meshcore_to_fieldpack(const MeshCoreFrame::MeshCoreObsPayload &obs, WsFields::FieldPack *pack)
{
	if (pack == nullptr) return false;
	memset(pack, 0, sizeof(*pack));
	pack->ts = obs.timestamp;
	snprintf(pack->packer_name, sizeof(pack->packer_name), "MESHCR");

	// Add Station ID (Meta)
	WsFields::Field f_id;
	WsFields::field_init(&f_id, &WsFields::FIELD_INFO_META_ID);
	f_id.value.val_u32 = obs.station_id;
	f_id.decode_status = WsCommon::RetRes::OK;
	WsFields::add_field_to_pack(pack, &f_id);

	// Add Weather TS
	WsFields::Field f_ts;
	WsFields::field_init(&f_ts, &WsFields::FIELD_INFO_WEATHER_TS);
	f_ts.value.val_u32 = obs.timestamp;
	f_ts.decode_status = WsCommon::RetRes::OK;
	WsFields::add_field_to_pack(pack, &f_ts);

	// Valid mask bits:
	// 0x0001: Temp (0.1 deg C)
	if (obs.valid_mask & 0x0001) {
		WsFields::Field f;
		WsFields::field_init(&f, &WsFields::FIELD_INFO_WEATHER_TEMP);
		f.value.val_float = (float)obs.temperature / 10.0f;
		f.decode_status = WsCommon::RetRes::OK;
		WsFields::add_field_to_pack(pack, &f);
	}
	// 0x0002: Humidity (%)
	if (obs.valid_mask & 0x0002) {
		WsFields::Field f;
		WsFields::field_init(&f, &WsFields::FIELD_INFO_WEATHER_HUM);
		f.value.val_float = (float)obs.humidity;
		f.decode_status = WsCommon::RetRes::OK;
		WsFields::add_field_to_pack(pack, &f);
	}
	// 0x0004: Pressure (0.1 hPa)
	if (obs.valid_mask & 0x0004) {
		WsFields::Field f;
		WsFields::field_init(&f, &WsFields::FIELD_INFO_WEATHER_PRESSURE);
		f.value.val_float = (float)obs.pressure / 10.0f;
		f.decode_status = WsCommon::RetRes::OK;
		WsFields::add_field_to_pack(pack, &f);
	}
	// 0x0008: Wind speed (0.1 m/s)
	if (obs.valid_mask & 0x0008) {
		WsFields::Field f;
		WsFields::field_init(&f, &WsFields::FIELD_INFO_WEATHER_WIND_SPEED);
		f.value.val_float = (float)obs.wind_speed / 10.0f;
		f.decode_status = WsCommon::RetRes::OK;
		WsFields::add_field_to_pack(pack, &f);
	}
	// 0x0010: Wind gust (0.1 m/s)
	if (obs.valid_mask & 0x0010) {
		WsFields::Field f;
		WsFields::field_init(&f, &WsFields::FIELD_INFO_WEATHER_WIND_GUST);
		f.value.val_float = (float)obs.wind_gust / 10.0f;
		f.decode_status = WsCommon::RetRes::OK;
		WsFields::add_field_to_pack(pack, &f);
	}
	// 0x0020: Wind direction (deg)
	if (obs.valid_mask & 0x0020) {
		WsFields::Field f;
		WsFields::field_init(&f, &WsFields::FIELD_INFO_WEATHER_WIND_DIR);
		f.value.val_u16 = obs.wind_dir;
		f.decode_status = WsCommon::RetRes::OK;
		WsFields::add_field_to_pack(pack, &f);
	}
	// 0x0040: Precip cml (0.1 mm)
	if (obs.valid_mask & 0x0040) {
		WsFields::Field f;
		WsFields::field_init(&f, &WsFields::FIELD_INFO_WEATHER_PRECIP_CML);
		f.value.val_float = (float)obs.precip_cml / 10.0f;
		f.decode_status = WsCommon::RetRes::OK;
		WsFields::add_field_to_pack(pack, &f);
	}
	// 0x0080: Solar rad (0.1 W/m2)
	if (obs.valid_mask & 0x0080) {
		WsFields::Field f;
		WsFields::field_init(&f, &WsFields::FIELD_INFO_WEATHER_SOLAR_RAD);
		f.value.val_float = (float)obs.solar_rad / 10.0f;
		f.decode_status = WsCommon::RetRes::OK;
		WsFields::add_field_to_pack(pack, &f);
	}
	// 0x0100: UV Index (0.1)
	if (obs.valid_mask & 0x0100) {
		WsFields::Field f;
		WsFields::field_init(&f, &WsFields::FIELD_INFO_WEATHER_UV_INDEX);
		f.value.val_u8 = obs.uv_index;
		f.decode_status = WsCommon::RetRes::OK;
		WsFields::add_field_to_pack(pack, &f);
	}
	// 0x0200: Battery mV
	if (obs.valid_mask & 0x0200) {
		WsFields::Field f;
		WsFields::field_init(&f, &WsFields::FIELD_INFO_META_BAT_MV);
		f.value.val_u16 = obs.battery_mv;
		f.decode_status = WsCommon::RetRes::OK;
		WsFields::add_field_to_pack(pack, &f);
	}

	return true;
}

PktStatus handle_packet_impl(PacketQueueMsg* pkt_msg, PacketCounterSample* counter_sample)
{
	// TODO: This will need a lot QA testing
	uint8_t response_buff[255];
	WsCommon::Packet pkt_received = {.pl = pkt_msg->buffer, .pl_len = pkt_msg->length};
	WsCommon::Packet pkt_response = {.pl = response_buff, .pl_len = 0};

	_state.station_rssi = pkt_msg->rssi;
	_state.station_snr = pkt_msg->snr;
	_state.station_signal_available = true;

#if WXM_ENV_DEBUG
	// DIAGNOSTIC LOGGING: Dump all incoming packets raw before validation/processing
	char raw_payload_hex[512] = "";
	Utils::buff_to_hex_str(pkt_received.pl, pkt_received.pl_len, raw_payload_hex, sizeof(raw_payload_hex));
	ESP_LOGI(TAG, "[DIAGNOSTIC] RAW LORA PACKET: len=%d, rssi=%d, snr=%d, hex=%s", 
		(int)pkt_received.pl_len, pkt_msg->rssi, pkt_msg->snr, raw_payload_hex);
#endif

	// Check for MeshCore GRP_DATA observation frame
	if (pkt_received.pl != nullptr && pkt_received.pl_len >= 35)
	{
		uint8_t mc_hdr = pkt_received.pl[0];
		if (((mc_hdr >> 2) & 0x0F) == MeshCoreFrame::PAYLOAD_TYPE_GRP_DATA)
		{
			MeshCoreFrame::DecodedGrpData mc_decoded = {};
			if (MeshCoreFrame::decode_grp_data_frame(
					pkt_received.pl, pkt_received.pl_len,
					MeshCoreFrame::DEFAULT_PSK,
					mbedtls_aes_ecb_fn, mbedtls_hmac_sha256_fn,
					&mc_decoded))
			{
				if (mc_decoded.type == MeshCoreFrame::GrpPayloadType::SIGNED_STASENSORS_V1)
				{
					WsCommon::Packet inner_pkt = {
						.pl = mc_decoded.signed_sta_sensors,
						.pl_len = (uint16_t)mc_decoded.signed_sta_sensors_len
					};

					if (Ws1300Helpers::packet_validate(&inner_pkt) != WsCommon::RetRes::OK)
					{
						ESP_LOGW(TAG, "MeshCore signed packet failed validation");
						return PktStatus::BAD_DATA;
					}

					uint32_t sta_id = 0;
					if (Ws1300Decoders::decode_meta_id(&inner_pkt, &sta_id) != WsCommon::RetRes::OK)
					{
						ESP_LOGW(TAG, "MeshCore signed packet failed station_id decoding");
						return PktStatus::BAD_DATA;
					}

					bool is_paired_sta = (get_pair_state() == PairState::PAIRED) && (_state.pair_data.id == sta_id);
					bool fwd_enabled = Settings::get_mesh_multi_station_forwarding_enabled();

					if (!is_paired_sta && !fwd_enabled)
					{
						ESP_LOGI(TAG, "MeshCore signed packet from non-paired station 0x%08X (multi-station forwarding disabled)",
							(unsigned int)sta_id);
						return PktStatus::IGNORE;
					}

					if (is_paired_sta)
					{
						bool pubkey_valid = false;
						for (size_t i = 0; i < sizeof(_state.pair_data.pubkey); i++)
						{
							if (_state.pair_data.pubkey[i] != 0x00 && _state.pair_data.pubkey[i] != 0xFF)
							{
								pubkey_valid = true;
								break;
							}
						}
						if (pubkey_valid)
						{
							if (Ws1300Helpers::packet_verify(&inner_pkt, _state.pair_data.pubkey, sizeof(_state.pair_data.pubkey)) != WsCommon::RetRes::OK)
							{
								ESP_LOGW(TAG, "MeshCore signed packet failed signature verification for station 0x%08X", (unsigned int)sta_id);
								return PktStatus::BAD_DATA;
							}
							ESP_LOGI(TAG, "MeshCore signed packet signature verified OK for station 0x%08X", (unsigned int)sta_id);
						}
						else
						{
							ESP_LOGW(TAG, "MeshCore signed packet accepted without ECDSA verification for station 0x%08X (no pubkey stored)", (unsigned int)sta_id);
						}

						WsFields::FieldPack f_pack = {};
						WsGps::GpsData g_data = {};
						if (Ws1300::packet_decoder(&inner_pkt, &f_pack, &g_data) != WsCommon::RetRes::OK)
						{
							ESP_LOGW(TAG, "MeshCore signed packet decoder failed for station 0x%08X", (unsigned int)sta_id);
							return PktStatus::BAD_DATA;
						}

						if (f_pack.ts == 0)
						{
							u32 sta_ts = 0;
							if (WsFields::get_field_val_from_pack(&f_pack, WsFields::FieldType::WEATHER_TS, NULL, &sta_ts) == WsCommon::RetRes::OK && sta_ts > 0)
							{
								f_pack.ts = sta_ts;
							}
							else if (DatetimeRtc::cur_time_valid())
							{
								f_pack.ts = (uint32_t)time(NULL);
							}
						}

						WsData::update_precip_rate_params(&f_pack);
						if (WsData::add(&f_pack) == WsCommon::RetRes::OK)
						{
							ESP_LOGI(TAG, "MeshCore signed observation added to buffer for paired station 0x%08X", (unsigned int)sta_id);
							if (!f_pack.bad_values_detected && Settings::get_pair_mode() != PairMode::PAIR_MODE_OFF)
							{
								GlobalEvents::publish(GlobalEvents::EventId::WS_DATA_RECEIVED, &f_pack, sizeof(f_pack));
							}
							return PktStatus::OK;
						}
						else
						{
							ESP_LOGW(TAG, "Failed to add MeshCore signed observation to buffer (buffer full?)");
							if (!f_pack.bad_values_detected && Settings::get_pair_mode() != PairMode::PAIR_MODE_OFF)
							{
								GlobalEvents::publish(GlobalEvents::EventId::WS_DATA_RECEIVED, &f_pack, sizeof(f_pack));
							}
							return PktStatus::OK;
						}
					}
					else
					{
						// Multi-station packet from non-paired station:
						// Enqueue in RemoteObservationQueue for dedicated forwarding without touching local WsData buffer.
						// Return IGNORE so the paired station state machine (miss streak, arrival timer, active status) is not affected.
						RemoteObservationQueue::RemoteObservation robs = {};
						robs.station_id = sta_id;
						robs.is_signed = true;
						robs.rssi = pkt_msg->rssi;
						robs.snr = pkt_msg->snr;
						robs.rx_uptime_ms = (int64_t)(esp_timer_get_time() / 1000LL);
						if (mc_decoded.signed_sta_sensors_len <= sizeof(robs.raw_payload)) {
							memcpy(robs.raw_payload, mc_decoded.signed_sta_sensors, mc_decoded.signed_sta_sensors_len);
							robs.raw_payload_len = (uint8_t)mc_decoded.signed_sta_sensors_len;
						}
						uint32_t pkt_ts = 0;
						if (Ws1300Decoders::decode_weather_ts(&inner_pkt, &pkt_ts) == WsCommon::RetRes::OK && pkt_ts > 0) {
							robs.timestamp = pkt_ts;
						} else {
							robs.timestamp = (uint32_t)time(NULL);
						}
						WsFields::FieldPack f_pack = {};
						WsGps::GpsData g_data = {};
						if (Ws1300::packet_decoder(&inner_pkt, &f_pack, &g_data) == WsCommon::RetRes::OK) {
							robs.field_pack = f_pack;
						}
						RemoteObservationQueue::push(robs);
						ESP_LOGI(TAG, "MeshCore multi-station packet queued for remote station 0x%08X", (unsigned int)sta_id);
						return PktStatus::IGNORE;
					}
				}
				else if (mc_decoded.type == MeshCoreFrame::GrpPayloadType::LEGACY_OBSERVATION)
				{
					const auto &mc_payload = mc_decoded.legacy_obs;
					ESP_LOGI(TAG, "MeshCore legacy GRP_DATA frame received from station 0x%08X (ts=%u, temp=%.1f C, hum=%u%%, press=%.1f hPa)",
						(unsigned int)mc_payload.station_id, (unsigned int)mc_payload.timestamp,
						(float)mc_payload.temperature / 10.0f, (unsigned int)mc_payload.humidity,
						(float)mc_payload.pressure / 10.0f);

					WsFields::FieldPack mc_pack = {};
					if (decode_meshcore_to_fieldpack(mc_payload, &mc_pack))
					{
						bool is_paired_sta = (get_pair_state() == PairState::PAIRED) && (_state.pair_data.id == mc_payload.station_id);
						bool fwd_enabled = Settings::get_mesh_multi_station_forwarding_enabled();

						if (is_paired_sta)
						{
							WsData::update_precip_rate_params(&mc_pack);
							if (WsData::add(&mc_pack) == WsCommon::RetRes::OK)
							{
								ESP_LOGI(TAG, "MeshCore observation added to buffer for paired station 0x%08X", (unsigned int)mc_payload.station_id);
							}
							else
							{
								ESP_LOGW(TAG, "Failed to add MeshCore observation to buffer (buffer full?)");
							}
							if (!mc_pack.bad_values_detected && Settings::get_pair_mode() != PairMode::PAIR_MODE_OFF)
							{
								GlobalEvents::publish(GlobalEvents::EventId::WS_DATA_RECEIVED, &mc_pack, sizeof(mc_pack));
							}
							return PktStatus::OK;
						}
						else if (fwd_enabled)
						{
							RemoteObservationQueue::RemoteObservation robs = {};
							robs.station_id = mc_payload.station_id;
							robs.timestamp = mc_payload.timestamp;
							robs.is_signed = false;
							robs.rssi = pkt_msg->rssi;
							robs.snr = pkt_msg->snr;
							robs.rx_uptime_ms = (int64_t)(esp_timer_get_time() / 1000LL);
							robs.field_pack = mc_pack;
							RemoteObservationQueue::push(robs);
							ESP_LOGI(TAG, "MeshCore legacy observation queued for remote station 0x%08X", (unsigned int)mc_payload.station_id);
							return PktStatus::IGNORE;
						}
						else
						{
							ESP_LOGI(TAG, "MeshCore observation from non-paired station 0x%08X (multi-station forwarding disabled)",
								(unsigned int)mc_payload.station_id);
							return PktStatus::IGNORE;
						}
					}

					return PktStatus::IGNORE;
				}
			}
		}
	}

	// Initialize gateway context
	WsBase::GwCtx gw_ctx = {
		.is_paired = get_pair_state() == PairState::PAIRED ? true : false,
		// Do not check sync for gps reqμ time validity. Drift is goodenough for gps request check,
		// and also it can bypass scenario where packet comes before time sync on reboot,
		// while valid time exists in RTC.
		.gps_needed = calc_req_gps(DatetimeRtc::cur_time_valid()),
		.time_is_valid = DatetimeRtc::is_synced() && DatetimeRtc::cur_time_valid(),
		.cur_ts = time(NULL)
	};


	gw_ctx.paired_info.id = _state.pair_data.id;
	memcpy(gw_ctx.gw_sn, Credentials::get_serial(), sizeof(gw_ctx.gw_sn));
	// Use the credentials of the profile actually selected at runtime. Reading
	// Settings::get_ws_pubkey() here always selected LOCAL and made a different
	// CLOUD target fail signature verification after a source switch.
	memcpy(gw_ctx.paired_info.pubkey, _state.pair_data.pubkey, sizeof(gw_ctx.paired_info.pubkey));
	memcpy(gw_ctx.paired_info.sn, _state.pair_data.sn, sizeof(gw_ctx.paired_info.sn));

	// Initialize station context
	WsBase::StaCtx sta_ctx = {};
	memset(sta_ctx.pair_data.pubkey, 0x00, sizeof(sta_ctx.pair_data.pubkey));
	memset(sta_ctx.pair_data.sn, 0x00, sizeof(sta_ctx.pair_data.sn));

	WsBase::MultiStation* active_station = NULL;
	bool sniff_decoded_ok = false;
	WsFields::FieldPack sniff_field_pack = {};
	WsGps::GpsData sniff_gps_data = {};

	// A saved pairing in OFF mode is deliberately not serviced as paired
	// traffic. It remains tuned and decodable, but must never transmit ACKs:
	// OFF/LISTEN is also how an operator silences a duplicate gateway while
	// verifying which gateway a WS1300 actually trusts.
	bool is_paired = (get_pair_state() == PairState::PAIRED) &&
		(get_cur_station() != NULL) &&
		(Settings::get_pair_mode() != PairMode::PAIR_MODE_OFF);

	/** True when this packet is NOT serviced as paired traffic but only
	 * decoded for display (sniffing). Foreign packets are never acked and
	 * never pushed to the telemetry buffers. */
	bool foreign_packet = false;

	WsCommon::RetRes ret = WsCommon::RetRes::ERROR;

	if (is_paired)
	{
		// A paired gateway always services its own station first - full packet
		active_station = get_cur_station();
		ret = active_station->packet_handler(&gw_ctx, &pkt_received, &pkt_response, &sta_ctx);

		if (ret == WsCommon::RetRes::OK && active_station->TYPE == WsType::WS_TYPE_1300)
		{
			_paired_station_interval_sec = 20; // reset on successful ACK
			if (!_paired_channel_locked)
			{
				// Station confirmed on the current tuning - stop probing and
				// stay here, whether that is the serial-derived channel
				// (v11+ firmware) or the legacy base frequency (pre-v11)
				_paired_channel_locked = true;
				_paired_freq_confirmed = true;
				_paired_confirmed_on_base = _paired_base_fallback;
				ESP_LOGI(TAG, "Paired station confirmed on %s. Locking listen frequency.",
					_paired_base_fallback ? "legacy base frequency" : "serial-derived channel");
			}
		}

		if (ret != WsCommon::RetRes::OK &&
			(g_sniff_mode_enabled || g_receiver_profile == WsType::WS_TYPE_ALL || g_manual_listen_slot >= 0))
		{
			WsBase::MultiStation* sniff_station = NULL;
			if (try_sniff_decode(&pkt_received, &sniff_station, &sniff_field_pack, &sniff_gps_data))
			{
				active_station = sniff_station;
				sniff_decoded_ok = true;
				foreign_packet = true;
				pkt_response.pl_len = 0; // Never ack foreign stations
				ret = WsCommon::RetRes::OK;
			}
			// else: keep packet_handler's ret, handled by the error path below
		}
	}
	else if (g_sniff_mode_enabled || g_receiver_profile == WsType::WS_TYPE_ALL || g_manual_listen_slot >= 0)
	{
		if (try_sniff_decode(&pkt_received, &active_station, &sniff_field_pack, &sniff_gps_data))
		{
			sniff_decoded_ok = true;
			foreign_packet = true;
			pkt_response.pl_len = 0;
			ret = WsCommon::RetRes::OK;
		}
		else
		{
			ESP_LOGW(TAG, "Sniffed packet failed validation or verification. Ignoring.");
			if (Settings::get_debug_mode_verbosity() == Log::Level::DEBUG)
			{
				char payload_hex[LOG_MSG_LEN] = "";
				if(RET_OK == Utils::buff_to_hex_str(pkt_received.pl, pkt_received.pl_len, (char*)payload_hex, sizeof(payload_hex)))
				{
					Log::log((char*)payload_hex);
				}
				Log::log(Log::Code::SNIFFER_PACKET_RSSI, (int)pkt_msg->rssi, (int)pkt_msg->snr);
			}
			return PktStatus::IGNORE;
		}
	}
	else
	{
		active_station = get_cur_station();
		if (active_station != NULL)
		{
			ret = active_station->packet_handler(&gw_ctx, &pkt_received, &pkt_response, &sta_ctx);
		}
	}

	if (active_station == NULL)
	{
		ESP_LOGW(TAG, "No active station for packet.");
		return PktStatus::IGNORE;
	}

	if(ret != WsCommon::RetRes::OK)
	{
		if(Settings::get_debug_mode_verbosity() == Log::Level::DEBUG)
		{
			char payload_hex[LOG_MSG_LEN] = "";
			if(RET_OK == Utils::buff_to_hex_str(pkt_received.pl, pkt_received.pl_len, (char*)payload_hex, sizeof(payload_hex)))
			{
				Log::log((char*)payload_hex);
			}
			Log::log(Log::Code::SNIFFER_PACKET_RSSI, (int)pkt_msg->rssi, (int)pkt_msg->snr);
		}

		uint32_t station_id = _state.pair_data.id;
		if (station_id == 0 && active_station != NULL)
		{
			if (active_station->TYPE == WsType::WS_TYPE_1001)
			{
				Ws1001Decoders::decode_meta_id(&pkt_received, &station_id);
			}
			else if (active_station->TYPE == WsType::WS_TYPE_1300)
			{
				Ws1300Decoders::decode_meta_id(&pkt_received, &station_id);
			}
		}

		char err_buf[64] = "";
		if(ret == WsCommon::RetRes::ERROR_BAD_SIGNATURE)
		{
			snprintf(err_buf, sizeof(err_buf), "UNVERIFIED: 0x%08lX", (unsigned long)station_id);
		}
		else
		{
			snprintf(err_buf, sizeof(err_buf), "INVALID: 0x%08lX (ret=%d)", (unsigned long)station_id, (int) ret);
		}
		set_station_last_err(err_buf);
		
		ESP_LOGW(TAG, "Packet received, but ignored");
		WsCommon::print_ret_val(ret);
		return PktStatus::IGNORE;
	}

	// Successfully validated packet clears the last error attribute
	if (g_ws_last_err[0] != '\0')
	{
		set_station_last_err("");
	}

	// Keep only the two most recently decoded station identities in RAM for
	// the Web UI. This is intentionally never persisted: normal radio traffic
	// must not cause NVS writes or become pairing authority.
	record_recent_station(active_station, &pkt_received, pkt_msg->rssi, pkt_msg->snr);

	WsFields::FieldPack field_pack = {};
	WsGps::GpsData gps_data = {};
	if (foreign_packet && sniff_decoded_ok)
	{
		field_pack = sniff_field_pack;
		gps_data = sniff_gps_data;
		if (active_station != NULL && active_station->TYPE == WsType::WS_TYPE_1300)
		{
			ESP_LOGI(TAG, "Successfully sniffed packet while listening on %s", _listen_desc);
		}
		ret = WsCommon::RetRes::OK;

		// --- AUTO-PAIRING FOR UNPAIRED GATEWAY ---
		// Disabled while a manual listen channel is locked: manual mode is a
		// pure observation mode for diagnostics
		bool is_fallback_desperate = (_fallback_phase == AutoFallbackPhase::PHASE_WS1001_SCAN ||
		                              _fallback_phase == AutoFallbackPhase::PHASE_WS1300_SWEEP);

		if ((g_auto_pair_enabled || is_fallback_desperate) &&
		    get_pair_state() == PairState::UNPAIRED &&
		    (!(Settings::get_pair_mode() == PairMode::PAIR_MODE_SCAN && g_scan_target_id == 0) || is_fallback_desperate) &&
		    (Settings::get_pair_mode() == PairMode::PAIR_MODE_SCAN ||
		     is_fallback_desperate ||
		     (!Settings::local_ws_profile_ready() && !Settings::cloud_ws_profile_ready())) &&
		    g_manual_listen_slot < 0 &&
		    (is_fallback_desperate ||
		     active_station->TYPE == g_receiver_profile || g_receiver_profile == WsType::WS_TYPE_ALL ||
		     active_station->TYPE == g_scan_profile || g_scan_profile == WsType::WS_TYPE_ALL))
		{
			WsBase::PairData pair_data = {};
			bool can_pair = false;

			if (active_station->TYPE == WsType::WS_TYPE_1001)
			{
				if (_fallback_phase != AutoFallbackPhase::PHASE_WS1300_SWEEP)
				{
					Ws1001Decoders::decode_meta_id(&pkt_received, &pair_data.id);
					memset(pair_data.pubkey, 0xFF, sizeof(pair_data.pubkey));
					memset(pair_data.sn, 0xFF, sizeof(pair_data.sn));
					can_pair = true;
				}
			}
			else if (active_station->TYPE == WsType::WS_TYPE_1300)
			{
				Ws1300::StaPacketType pkt_type = Ws1300Helpers::get_packet_type(&pkt_received);
				if (pkt_type == Ws1300::StaPacketType::ADV)
				{
					Ws1300Decoders::decode_meta_id(&pkt_received, &pair_data.id);
					Ws1300::ByteBuffer::StaAdv* adv = (Ws1300::ByteBuffer::StaAdv*)pkt_received.pl;
					memcpy(pair_data.pubkey, adv->pub_key, sizeof(pair_data.pubkey));
					memcpy(pair_data.sn, adv->sn, sizeof(pair_data.sn));
					can_pair = true;
				}
				else if (pkt_type == Ws1300::StaPacketType::BOOT)
				{
					Ws1300Decoders::decode_meta_id(&pkt_received, &pair_data.id);
					Ws1300::ByteBuffer::StaBoot* boot = (Ws1300::ByteBuffer::StaBoot*)pkt_received.pl;
					memcpy(pair_data.pubkey, boot->pub_key, sizeof(pair_data.pubkey));
					memcpy(pair_data.sn, boot->sn, sizeof(pair_data.sn));
					can_pair = true;
				}
				else if (pkt_type == Ws1300::StaPacketType::SENSORS || pkt_type == Ws1300::StaPacketType::GNSS)
				{
					Ws1300Decoders::decode_meta_id(&pkt_received, &pair_data.id);
					uint32_t cloud_id = (uint32_t)Settings::get_cloud_ws_id();
					uint32_t local_id = (uint32_t)Settings::get_ws_id();
					if (Settings::cloud_ws_profile_ready() && pair_data.id == cloud_id)
					{
						Settings::get_cloud_ws_pubkey(pair_data.pubkey, sizeof(pair_data.pubkey));
						Settings::get_cloud_ws_sn(pair_data.sn, sizeof(pair_data.sn));
						can_pair = true;
						ESP_LOGI(TAG, "Unpaired sniff mode: WS1300 %s packet accepted for cloud-assigned station 0x%08X",
							pkt_type == Ws1300::StaPacketType::SENSORS ? "SENSORS" : "GNSS",
							(unsigned int)pair_data.id);
					}
					else if (Settings::local_ws_profile_ready() && pair_data.id == local_id)
					{
						Settings::get_ws_pubkey(pair_data.pubkey, sizeof(pair_data.pubkey));
						Settings::get_ws_sn(pair_data.sn, sizeof(pair_data.sn));
						can_pair = true;
						ESP_LOGI(TAG, "Unpaired sniff mode: WS1300 %s packet accepted for local saved station 0x%08X",
							pkt_type == Ws1300::StaPacketType::SENSORS ? "SENSORS" : "GNSS",
							(unsigned int)pair_data.id);
					}
					else if (_fallback_phase == AutoFallbackPhase::PHASE_WS1300_SWEEP)
					{
						// In Phase 4 (desperate mode): accept ANY found station!
						uint8_t ch = (g_sniff_slot > 0 && g_sniff_slot <= 8) ? (uint8_t)(g_sniff_slot - 1) : 0;
						memset(pair_data.sn, 0x00, sizeof(pair_data.sn));
						pair_data.sn[0] = (pair_data.id >> 24) & 0xFF;
						pair_data.sn[1] = (pair_data.id >> 16) & 0xFF;
						pair_data.sn[2] = (pair_data.id >> 8) & 0xFF;
						pair_data.sn[3] = pair_data.id & 0xFF;
						pair_data.sn[4] = 0xAA;
						pair_data.sn[5] = 0x55;
						pair_data.sn[6] = 0xAA;
						pair_data.sn[7] = 0x55;
						pair_data.sn[8] = 0x00;
						pair_data.sn[sizeof(pair_data.sn) - 1] = ch;
						memset(pair_data.pubkey, 0x00, sizeof(pair_data.pubkey));
						can_pair = true;
						ESP_LOGW(TAG, "Fallback Phase 4 (Desperate Mode): Accepting WS1300 station 0x%08X from %s packet on slot %d (CH%u)!",
							(unsigned int)pair_data.id,
							pkt_type == Ws1300::StaPacketType::SENSORS ? "SENSORS" : "GNSS",
							g_sniff_slot, (unsigned int)ch);
					}
					else
					{
						ESP_LOGI(TAG, "Unpaired sniff mode: WS1300 packet type 0x%02X from 0x%08X ignored (not known profile).",
							(int)pkt_type, (unsigned int)pair_data.id);
					}
				}
				else
				{
					if (_fallback_phase == AutoFallbackPhase::PHASE_WS1300_SWEEP)
					{
						Ws1300Decoders::decode_meta_id(&pkt_received, &pair_data.id);
						uint8_t ch = (g_sniff_slot > 0 && g_sniff_slot <= 8) ? (uint8_t)(g_sniff_slot - 1) : 0;
						memset(pair_data.sn, 0x00, sizeof(pair_data.sn));
						pair_data.sn[0] = (pair_data.id >> 24) & 0xFF;
						pair_data.sn[1] = (pair_data.id >> 16) & 0xFF;
						pair_data.sn[2] = (pair_data.id >> 8) & 0xFF;
						pair_data.sn[3] = pair_data.id & 0xFF;
						pair_data.sn[4] = 0xAA;
						pair_data.sn[5] = 0x55;
						pair_data.sn[6] = 0xAA;
						pair_data.sn[7] = 0x55;
						pair_data.sn[8] = 0x00;
						pair_data.sn[sizeof(pair_data.sn) - 1] = ch;
						memset(pair_data.pubkey, 0x00, sizeof(pair_data.pubkey));
						can_pair = true;
						ESP_LOGW(TAG, "Fallback Phase 4 (Desperate Mode): Accepting WS1300 station 0x%08X from packet type 0x%02X on slot %d (CH%u)!",
							(unsigned int)pair_data.id, (int)pkt_type, g_sniff_slot, (unsigned int)ch);
					}
					else
					{
						ESP_LOGI(TAG, "Unpaired sniff mode: WS1300 packet type 0x%02X cannot trigger auto-pairing (needs ADV or BOOT).", (int)pkt_type);
					}
				}
			}

			if (can_pair)
			{
				// Manual discovery scan should never auto-pair with discovered stations;
				// it discovers stations into the buffer and lets the user choose which one to pair.
				// However, in automated recovery fallback (Phase 3 WS1001 scan or Phase 4 WS1300 sweep),
				// desperate mode MUST auto-pair to restore connectivity without requiring human UI interaction.
				if ((Settings::get_pair_mode() == PairMode::PAIR_MODE_SCAN || !g_auto_pair_enabled) &&
				    g_scan_target_id == 0 &&
				    _fallback_phase != AutoFallbackPhase::PHASE_WS1001_SCAN &&
				    _fallback_phase != AutoFallbackPhase::PHASE_WS1300_SWEEP)
				{
					can_pair = false;
				}

				uint32_t expected_target = g_scan_target_id;
				// In Phase 1 or Phase 2, lock strictly to the designated target ID.
				// In Phase 3 (WS1001 scan) or Phase 4 (WS1300 sweep), allow pairing with
				// any candidate station in desperate mode.
				if (expected_target == 0 &&
				    (_fallback_phase == AutoFallbackPhase::PHASE_LOCAL_NVS ||
				     _fallback_phase == AutoFallbackPhase::PHASE_CLOUD_NVS) &&
				    Settings::cloud_ws_profile_ready())
				{
					expected_target = (uint32_t)Settings::get_cloud_ws_id();
				}

				if (expected_target != 0 &&
				    _fallback_phase != AutoFallbackPhase::PHASE_WS1001_SCAN &&
				    _fallback_phase != AutoFallbackPhase::PHASE_WS1300_SWEEP &&
				    pair_data.id != expected_target)
				{
					ESP_LOGI(TAG, "Pairable station 0x%08X ignored; target is 0x%08X",
						(unsigned int)pair_data.id, (unsigned int)expected_target);
					can_pair = false;
				}
			}

			if (can_pair)
			{
				ESP_LOGI(TAG, "Auto-pairing triggered for station ID: 0x%08X", pair_data.id);
				Settings::set_ws_type(active_station->TYPE);
				set_cur_station_obj(active_station->TYPE);
				g_receiver_profile = active_station->TYPE;
				g_sniff_mode_enabled = false; // Turn off sniff mode once paired
				g_manual_listen_slot = -1;    // Clear manual diagnostic channel lock once paired

				bool is_cloud_match = (Settings::cloud_ws_profile_ready() &&
					(uint32_t)Settings::get_cloud_ws_id() == pair_data.id);

				if (is_cloud_match)
				{
					_active_pair_source = PairSource::PAIR_SOURCE_CLOUD;
					Settings::set_pair_mode(PairMode::PAIR_MODE_CLOUD);
					set_pair_state(PairState::PAIRED, &pair_data);
					g_auto_pair_enabled = false;
					g_scan_target_id = 0;
					g_scan_target_slot = -1;
					Settings::commit();
					ESP_LOGI(TAG, "SCAN matched configured cloud station 0x%08X: locked to CLOUD mode",
						(unsigned int)pair_data.id);
				}
				else
				{
					_active_pair_source = PairSource::PAIR_SOURCE_LOCAL;
					Settings::set_pair_mode(PairMode::PAIR_MODE_LOCAL);
					Settings::set_local_pair_sync_pending(false);
					set_pair_state(PairState::PAIRED, &pair_data);
					g_auto_pair_enabled = false;
					g_scan_target_id = 0;
					g_scan_target_slot = -1;

					// Explicitly persist to Local NVS immediately
					Settings::set_ws_id(pair_data.id);
					Settings::set_ws_pubkey(pair_data.pubkey, WS_PUB_KEY_LEN);
					Settings::set_ws_sn(pair_data.sn, WS_SN_LEN);
					Settings::set_ws_type(active_station->TYPE);
					Settings::set_pair_source(PairSource::PAIR_SOURCE_LOCAL);
					Settings::set_pair_mode(PairMode::PAIR_MODE_LOCAL);
					Settings::commit();

					ESP_LOGI(TAG, "SCAN succeeded with station 0x%08X: saved to Local NVS; cloud assignment preserved",
						(unsigned int)pair_data.id);
				}

				if (_fallback_phase == AutoFallbackPhase::PHASE_WS1001_SCAN ||
				    _fallback_phase == AutoFallbackPhase::PHASE_WS1300_SWEEP)
				{
					_fallback_phase = AutoFallbackPhase::PHASE_LATCHED;
					if (!is_cloud_match)
					{
						_is_fallback_latch = true;
					}
					ESP_LOGI(TAG, "Fallback: %s latched station 0x%08X; scan stopped.",
						_is_fallback_latch ? "fallback_latch" : "cloud_verified",
						(unsigned int)pair_data.id);
				}

				if (active_station->TYPE == WsType::WS_TYPE_1300)
				{
					// Reset the helper's power/packet-count tracking statics -
					// they survive from a previous pairing or sniffing session
					// and would otherwise produce garbage lost-packet counts
					// and a wrong power CMD on the first exchanges.
					Ws1300Helpers::init_sta_status((Ws1300::ByteBuffer::StaHeader*)pkt_received.pl);

					// Instant Latch: Arm GPS ownership challenge downlink to verify ownership
					g_ownership_challenge_armed = true;
					g_ownership_challenge_pending = false;
					g_ownership_confirmed = false;
					g_ownership_challenge_station_id = pair_data.id;
					g_ownership_challenge_started_sec = Utils::get_tick_sec();
					g_ownership_challenge_packets_seen = 0;
					_state.gps_last_req_ts_sec = 0; // Trigger immediately on next response window
					ESP_LOGI(TAG, "Instant Latch: WS1300 0x%08X armed for GPS challenge ownership confirmation",
						(unsigned int)pair_data.id);

					// Explicitly generate the PAIR response for the first ADV packet so we pair instantly!
					Ws1300::StaPacketType p_type = Ws1300Helpers::get_packet_type(&pkt_received);
					if (p_type == Ws1300::StaPacketType::ADV)
					{
						Ws1300::ByteBuffer::RespPair* resp_pair = (Ws1300::ByteBuffer::RespPair*) pkt_response.pl;
						resp_pair->header.type = Ws1300::RespPacketType::PAIR;
						resp_pair->header.packet_id = ((Ws1300::ByteBuffer::StaHeader*)pkt_received.pl)->packet_id;
						resp_pair->header.station_id = ((Ws1300::ByteBuffer::StaHeader*)pkt_received.pl)->station_id;
						memcpy(resp_pair->sn, gw_ctx.gw_sn, sizeof(resp_pair->sn));
						Ws1300Helpers::gen_resp_hmac(pkt_response.pl, sizeof(Ws1300::ByteBuffer::RespPair) - sizeof(resp_pair->hmac),
													resp_pair->hmac, sizeof(resp_pair->hmac));
						pkt_response.pl_len = sizeof(Ws1300::ByteBuffer::RespPair);
					}
				}

				if (active_station->TYPE == WsType::WS_TYPE_1300 && g_sniff_slot == 0)
				{
					// Heard on the plain base frequency: this is a legacy
					// (pre-v11) station - it lives there, so we do too
					_paired_base_fallback = true;
					ESP_LOGW(TAG, "Auto-paired WS1300 from legacy/base frequency; staying on base");
				}

				if (pkt_response.pl_len == 0)
				{
					// Nothing to transmit (e.g. WS1001): settle onto the
					// paired frequency right away. When a PAIR response is
					// pending, the retune is deferred to TX-done so the
					// response goes out on the frequency the station is
					// listening on.
					set_radio_freq(Settings::get_freq_region());
					sx126x_rx_cont();
				}

				// This packet just paired the station; treat it as paired station
				// traffic so signal metrics, telemetry buffer, and the TFT LCD
				// update immediately on this packet instead of waiting for the next.
				foreign_packet = false;
				is_paired = (Settings::get_pair_mode() != PairMode::PAIR_MODE_OFF);
			}
		}

		// Explicit one-shot station kick: answer a sniffed WS1300 with an authenticated
		// CMD-reboot. The rebooting station transmits STA_BOOT, which is an
		// auto-pair trigger - the LoRa way to make an already-paired station
		// pairable again (its own protocol has no unpair command). The CMD HMAC
		// is keyed on this gateway's serial, so only a station paired to THIS
		// gateway accepts it; anyone else's station ignores the packet. SCAN
		// deliberately does not arm this: automatically rebooting an already
		// paired station can make its BOOT win the scan ahead of the intended ADV.
		if (g_ws1300_kick_armed && pkt_response.pl_len == 0 &&
		    active_station->TYPE == WsType::WS_TYPE_1300)
		{
			Ws1300::StaPacketType kick_pkt_type = Ws1300Helpers::get_packet_type(&pkt_received);
			Ws1300::ByteBuffer::StaHeader* kick_hdr = (Ws1300::ByteBuffer::StaHeader*)pkt_received.pl;
			uint32_t kick_sta_id = Ws1300::ByteBuffer::be_to_le(kick_hdr->station_id);
			// The station rejects CMD responses to BOOT/ADV packets; a target filter
			// keeps the one-shot from being consumed by a bystander station
			if (kick_pkt_type != Ws1300::StaPacketType::NONE &&
			    kick_pkt_type != Ws1300::StaPacketType::BOOT &&
			    kick_pkt_type != Ws1300::StaPacketType::ADV &&
			    (g_ws1300_kick_target_id == 0 || g_ws1300_kick_target_id == kick_sta_id))
			{
				Ws1300::ByteBuffer::RespCmd* resp_cmd = (Ws1300::ByteBuffer::RespCmd*)pkt_response.pl;
				memset(resp_cmd, 0x00, sizeof(*resp_cmd));
				resp_cmd->header.type = Ws1300::RespPacketType::CMD;
				resp_cmd->header.packet_id = kick_hdr->packet_id;
				resp_cmd->header.station_id = kick_hdr->station_id;
				resp_cmd->reboot = true;
				Ws1300Helpers::gen_resp_hmac(pkt_response.pl, sizeof(Ws1300::ByteBuffer::RespCmd) - sizeof(resp_cmd->hmac),
											resp_cmd->hmac, sizeof(resp_cmd->hmac));
				pkt_response.pl_len = sizeof(Ws1300::ByteBuffer::RespCmd);
				g_ws1300_kick_armed = false; // one-shot
				// Hold the sweep here: the station goes silent while rebooting and
				// its STA_BOOT (the pairable packet) will arrive on this channel
				_sniff_slot_hold_until_ms = Utils::get_tick_ms() + WS_SNIFF_KICK_HOLD_MS;
				ESP_LOGW(TAG, "Station kick: CMD reboot transmitted to sniffed station 0x%08X (pkt 0x%02X), holding channel for BOOT",
					(unsigned int)kick_sta_id, (int)kick_pkt_type);
			}
		}
	}
	else
	{
		ret = active_station->packet_decoder(&pkt_received, &field_pack, &gps_data);
	}

	if(ret != WsCommon::RetRes::OK)
	{
		ESP_LOGE(TAG, "Packet decoding failed");
		return PktStatus::IGNORE;
	}

	if(sta_ctx.pair_permitted)
	{
		if (g_auto_pair_enabled && !gw_ctx.is_paired && !g_sniff_mode_enabled)
		{
			set_pair_state(PairState::PAIRED, &sta_ctx.pair_data);
		}
		else if (gw_ctx.is_paired && active_station->TYPE == WsType::WS_TYPE_1300)
		{
			// If already paired, but we received a permitted pairing packet (e.g. ADV from our own station with updated key),
			// update the pairing credentials/public key in NVS.
			set_pair_state(PairState::PAIRED, &sta_ctx.pair_data);
		}
	}

	// Transmit the response (ACK/CMD/PAIR/TIME_SYNC) on the frequency the
	// packet was received on - the station's 300 ms RX window is always on
	// its own TX frequency. Settling onto the expected paired frequency
	// happens after TX completes (sx126x_tx_cb), never before.
	if(pkt_response.pl_len > 0)
	{
#ifdef CONFIG_SCREENLESS
		KissClient::send_data(pkt_response.pl, pkt_response.pl_len);
		// KISS TX is synchronous and has no TX-done callback: settle now
		if(get_pair_state() == PairState::PAIRED && g_manual_listen_slot < 0)
		{
			set_radio_freq(Settings::get_freq_region());
			sx126x_rx_cont();
		}
#else
		sx126x_tx(pkt_response.pl, pkt_response.pl_len);
#endif
	}

	// Record power only after starting the time-critical response. This state
	// feeds warning gating and the LCD; it must never delay the station's short
	// receive window.
	if(!foreign_packet)
	{
		_state.station_rssi = pkt_msg->rssi;
		_state.station_snr = pkt_msg->snr;
		_state.station_signal_available = true;
		uint16_t reported_tx_power = 0;
		WsCommon::RetRes power_ret = active_station->TYPE == WsType::WS_TYPE_1300
			? Ws1300Decoders::decode_meta_tx_power(&pkt_received, &reported_tx_power)
			: Ws1001Decoders::decode_meta_tx_power(&pkt_received, &reported_tx_power);
		if(power_ret == WsCommon::RetRes::OK)
		{
			_state.station_tx_power = reported_tx_power;
			_state.requested_tx_power = sta_ctx.req_tx_power != 0
				? sta_ctx.req_tx_power : reported_tx_power;
		}
	}

	memcpy(field_pack.packer_name, active_station->NAME, sizeof(field_pack.packer_name));

	// ============== Print Raw Packet ========================
	printf("--- %s Packet Received ---\n", active_station->NAME);
	printf("RSSI: %d, SNR: %d\n", pkt_msg->rssi, pkt_msg->snr);
	if (active_station->TYPE == WsType::WS_TYPE_1300)
	{
		Ws1300::StaPacketType ws1300_type = Ws1300Helpers::get_packet_type(&pkt_received);
		if (ws1300_type == Ws1300::StaPacketType::ADV)
		{
			Ws1300::ByteBuffer::StaAdv* adv = (Ws1300::ByteBuffer::StaAdv*)pkt_received.pl;
			printf("--- WS1300 ADV Packet Decoded Parameters ---\n");
			printf("Station ID: 0x%08X\n", (unsigned int)Ws1300::ByteBuffer::be_to_le(adv->header.station_id));
			printf("FW Version: %u\n", adv->header.fw_ver);
			printf("HW Version: %u\n", adv->header.hw_ver);
			printf("Packet ID: %u\n", (unsigned int)Ws1300::ByteBuffer::be_to_le(adv->header.packet_id));
			printf("Last Resp RSSI: -%d dBm\n", adv->header.last_resp_rssi);
			printf("TX Power: %d dBm (raw: %u)\n", adv->header.aux_byte_1.tx_power - 9, adv->header.aux_byte_1.tx_power);
			printf("TX Interval: %u s (raw: %u)\n", adv->header.tx_int * 20, adv->header.tx_int);
			printf("Battery: %u mV (raw: %u)\n", adv->header.batt_mv * 20, adv->header.batt_mv);
			printf("Low Battery: %s\n", adv->header.aux_byte_2.low_batt ? "True" : "False");
			printf("Internal Battery: %s\n", adv->header.aux_byte_2.int_batt ? "True" : "False");
			printf("External Battery: %s\n", adv->header.aux_byte_2.ext_batt ? "True" : "False");
			printf("GPS Acquiring: %s\n", adv->header.aux_byte_2.acquiring_gps ? "True" : "False");
			printf("Contains Signature: %s\n", adv->header.aux_byte_2.contains_sig ? "True" : "False");
			
			char sn_str[21] = {0};
			Utils::buff_to_hex_str(adv->sn, sizeof(adv->sn), sn_str, sizeof(sn_str));
			printf("Serial Number: %s\n", sn_str);

			char pubkey_str[129] = {0};
			Utils::buff_to_hex_str(adv->pub_key, sizeof(adv->pub_key), pubkey_str, sizeof(pubkey_str));
			printf("Public Key: %s\n", pubkey_str);
			printf("--------------------------------------------\n");
		}
		else if (ws1300_type == Ws1300::StaPacketType::BOOT)
		{
			Ws1300::ByteBuffer::StaBoot* boot = (Ws1300::ByteBuffer::StaBoot*)pkt_received.pl;
			printf("--- WS1300 BOOT Packet Decoded Parameters ---\n");
			printf("Station ID: 0x%08X\n", (unsigned int)Ws1300::ByteBuffer::be_to_le(boot->header.station_id));
			printf("FW Version: %u\n", boot->header.fw_ver);
			printf("HW Version: %u\n", boot->header.hw_ver);
			printf("Packet ID: %u\n", (unsigned int)Ws1300::ByteBuffer::be_to_le(boot->header.packet_id));
			printf("Last Resp RSSI: -%d dBm\n", boot->header.last_resp_rssi);
			printf("TX Power: %d dBm (raw: %u)\n", boot->header.aux_byte_1.tx_power - 9, boot->header.aux_byte_1.tx_power);
			printf("TX Interval: %u s (raw: %u)\n", boot->header.tx_int * 20, boot->header.tx_int);
			printf("Battery: %u mV (raw: %u)\n", boot->header.batt_mv * 20, boot->header.batt_mv);
			printf("Low Battery: %s\n", boot->header.aux_byte_2.low_batt ? "True" : "False");
			printf("Internal Battery: %s\n", boot->header.aux_byte_2.int_batt ? "True" : "False");
			printf("External Battery: %s\n", boot->header.aux_byte_2.ext_batt ? "True" : "False");
			printf("GPS Acquiring: %s\n", boot->header.aux_byte_2.acquiring_gps ? "True" : "False");
			printf("Contains Signature: %s\n", boot->header.aux_byte_2.contains_sig ? "True" : "False");
			printf("FO Version: %u\n", boot->fo_ver);
			printf("Boot Reason: %u\n", boot->boot_reason);
			
			char sn_str[21] = {0};
			Utils::buff_to_hex_str(boot->sn, sizeof(boot->sn), sn_str, sizeof(sn_str));
			printf("Serial Number: %s\n", sn_str);

			char pubkey_str[129] = {0};
			Utils::buff_to_hex_str(boot->pub_key, sizeof(boot->pub_key), pubkey_str, sizeof(pubkey_str));
			printf("Public Key: %s\n", pubkey_str);
			printf("---------------------------------------------\n");
		}

		if (!foreign_packet && (ws1300_type == Ws1300::StaPacketType::ADV || ws1300_type == Ws1300::StaPacketType::BOOT))
		{
			uint8_t const *new_pk = (ws1300_type == Ws1300::StaPacketType::ADV)
				? ((Ws1300::ByteBuffer::StaAdv*)pkt_received.pl)->pub_key
				: ((Ws1300::ByteBuffer::StaBoot*)pkt_received.pl)->pub_key;
			uint8_t const *new_sn = (ws1300_type == Ws1300::StaPacketType::ADV)
				? ((Ws1300::ByteBuffer::StaAdv*)pkt_received.pl)->sn
				: ((Ws1300::ByteBuffer::StaBoot*)pkt_received.pl)->sn;

			// If current pairing profile has a placeholder/empty/changed pubkey or synthesized/blank/changed SN, upgrade it immediately
			bool cur_has_pubkey = false;
			for (size_t i = 0; i < sizeof(_state.pair_data.pubkey); i++)
			{
				if (_state.pair_data.pubkey[i] != 0 && _state.pair_data.pubkey[i] != 0xFF)
				{
					cur_has_pubkey = true;
					break;
				}
			}
			bool cur_sn_blank_or_synth = (_state.pair_data.sn[4] == 0xAA &&
			                              _state.pair_data.sn[5] == 0x55 &&
			                              _state.pair_data.sn[6] == 0xAA &&
			                              _state.pair_data.sn[7] == 0x55);
			if (!cur_sn_blank_or_synth)
			{
				bool all_zero = true;
				bool all_ff = true;
				for (size_t i = 0; i < sizeof(_state.pair_data.sn); i++)
				{
					if (_state.pair_data.sn[i] != 0x00) all_zero = false;
					if (_state.pair_data.sn[i] != 0xFF) all_ff = false;
				}
				cur_sn_blank_or_synth = all_zero || all_ff;
			}
			bool pk_or_sn_diff = (memcmp(_state.pair_data.pubkey, new_pk, sizeof(_state.pair_data.pubkey)) != 0) ||
			                     (memcmp(_state.pair_data.sn, new_sn, sizeof(_state.pair_data.sn)) != 0);
			if (!cur_has_pubkey || cur_sn_blank_or_synth || pk_or_sn_diff)
			{
				ESP_LOGI(TAG, "WS1300: Upgrading pairing profile with authentic SN + ECDSA public key from %s packet!",
					ws1300_type == Ws1300::StaPacketType::ADV ? "ADV" : "BOOT");
				memcpy(_state.pair_data.pubkey, new_pk, sizeof(_state.pair_data.pubkey));
				memcpy(_state.pair_data.sn, new_sn, sizeof(_state.pair_data.sn));
				bool saved_any = false;
				if (_active_pair_source == PairSource::PAIR_SOURCE_LOCAL ||
				    (uint32_t)Settings::get_ws_id() == _state.pair_data.id)
				{
					Settings::set_ws_pubkey((u8*)new_pk, sizeof(_state.pair_data.pubkey));
					Settings::set_ws_sn((u8*)new_sn, sizeof(_state.pair_data.sn));
					saved_any = true;
				}
				if (_active_pair_source == PairSource::PAIR_SOURCE_CLOUD ||
				    (Settings::get_cloud_ws_valid() && (uint32_t)Settings::get_cloud_ws_id() == _state.pair_data.id))
				{
					Settings::set_cloud_ws_pubkey((u8*)new_pk, sizeof(_state.pair_data.pubkey));
					Settings::set_cloud_ws_sn((u8*)new_sn, sizeof(_state.pair_data.sn));
					saved_any = true;
				}
				if (saved_any)
				{
					Settings::commit();
				}
			}

			// Whenever the paired WS1300 transmits ADV or BOOT, arm the GPS ownership challenge
			// so its next SENSORS packet automatically verifies ownership without needing manual UI action.
			if (!g_ownership_confirmed || ws1300_type == Ws1300::StaPacketType::ADV)
			{
				g_ownership_challenge_armed = true;
				g_ownership_challenge_pending = false;
				g_ownership_confirmed = false;
				g_ownership_challenge_station_id = _state.pair_data.id;
				g_ownership_challenge_started_sec = Utils::get_tick_sec();
				g_ownership_challenge_packets_seen = 0;
				_state.gps_last_req_ts_sec = 0;
				_state.gps_req_attempts = 0;
				ESP_LOGI(TAG, "WS1300: Armed GPS ownership challenge after receiving %s from 0x%08X",
					ws1300_type == Ws1300::StaPacketType::ADV ? "ADV" : "BOOT",
					(unsigned int)_state.pair_data.id);
			}
		}
	}
	if(Settings::get_debug_mode_verbosity() == Log::Level::DEBUG)
	{
		Log::log(Log::Code::SNIFFER_PACKET_RSSI, (int)pkt_msg->rssi, (int)pkt_msg->snr);
	}
	Utils::print_buff_hex(pkt_received.pl, pkt_received.pl_len);
	// ============== Print Raw Ack ========================
	printf("--- %s Packet Response ---\n", active_station->NAME);
	Utils::print_buff_hex(pkt_response.pl, pkt_response.pl_len);
	ESP_LOGI(TAG, "|ACK Response| Req Interval: %d, Req GPS: %s", active_station->BROADCAST_INTERVAL_SEC, 
		gw_ctx.gps_needed && sta_ctx.gps_req_permitted ? "True" : "False");

	if(!gw_ctx.time_is_valid)
	{
		static bool s_time_invalid_logged = false;
		if(!s_time_invalid_logged)
		{
			ESP_LOGW(TAG, "Gateway time invalid while receiving packet");
			s_time_invalid_logged = true;
		}
	}
	
	if(gps_data.valid && !WsGps::gps_data_has_required_fields(&gps_data))
	{
		ESP_LOGW(TAG, "Discarding incomplete GPS fix");
		gps_data.valid = false;
	}

	if(!foreign_packet && (g_ownership_challenge_armed || g_ownership_challenge_pending) &&
	   (g_ownership_challenge_station_id == _state.pair_data.id || g_ownership_challenge_station_id == 0))
	{
		if(g_ownership_challenge_pending && g_ownership_challenge_packets_seen < UINT8_MAX)
		{
			g_ownership_challenge_packets_seen++;
		}
		// An acquiring-GPS / busy flag is already proof that the station accepted this
		// gateway's authenticated command; a signed GPS packet or valid coordinates
		// is the stronger response. Do not wait several minutes for a complete fix when the
		// accepted state change is visible on the very next broadcasts.
		if(sta_ctx.is_busy || sta_ctx.gps_pkt_received || gps_data.valid)
		{
			g_ownership_challenge_armed = false;
			g_ownership_challenge_pending = false;
			g_ownership_confirmed = true;
			uint32_t own_ts = (Settings::is_station_ownership_confirmed() &&
			                   Settings::get_station_ownership_id() == _state.pair_data.id &&
			                   Settings::get_station_ownership_timestamp() != 0)
			                      ? Settings::get_station_ownership_timestamp()
			                      : (uint32_t)time(NULL);
			Settings::set_station_ownership_confirmed(true, _state.pair_data.id, own_ts);

			// Confirm ownership: store to Local NVS, and sync credentials to Cloud NVS if targeting Cloud
			Settings::set_ws_id(_state.pair_data.id);
			Settings::set_ws_pubkey(_state.pair_data.pubkey, WS_PUB_KEY_LEN);
			Settings::set_ws_sn(_state.pair_data.sn, WS_SN_LEN);
			Settings::set_ws_type(active_station->TYPE);
			if (_active_pair_source == PairSource::PAIR_SOURCE_CLOUD ||
			    (Settings::get_cloud_ws_valid() && (uint32_t)Settings::get_cloud_ws_id() == _state.pair_data.id))
			{
				Settings::set_cloud_ws_pubkey(_state.pair_data.pubkey, WS_PUB_KEY_LEN);
				Settings::set_cloud_ws_sn(_state.pair_data.sn, WS_SN_LEN);
			}
			if (_active_pair_source != PairSource::PAIR_SOURCE_CLOUD)
			{
				Settings::set_pair_source(PairSource::PAIR_SOURCE_LOCAL);
				Settings::set_pair_mode(PairMode::PAIR_MODE_LOCAL);
			}
			Settings::commit();

			if (_fallback_phase != AutoFallbackPhase::PHASE_MANUAL)
			{
				_fallback_phase = AutoFallbackPhase::PHASE_LATCHED;
			}

			ESP_LOGW(TAG,
				"Station ownership confirmed by GPS response for 0x%08X after %u packet(s); saved to NVS",
				(unsigned int)_state.pair_data.id,
				(unsigned int)g_ownership_challenge_packets_seen);
		}
		else if(g_ownership_challenge_pending && g_ownership_challenge_packets_seen >= OWNERSHIP_CHALLENGE_MAX_PACKETS)
		{
			g_ownership_challenge_pending = false;
			// Preserve prior NVS confirmation if it was already confirmed for this specific station ID
			if(Settings::get_station_ownership_id() != _state.pair_data.id)
			{
				Settings::set_station_ownership_confirmed(false, _state.pair_data.id, (uint32_t)time(NULL));
				Settings::commit();
			}
			ESP_LOGW(TAG,
				"Station ownership challenge window ended for 0x%08X after %u response windows (unconfirmed); keeping station in Local NVS and keeping scan stopped",
				(unsigned int)_state.pair_data.id,
				(unsigned int)g_ownership_challenge_packets_seen);
		}
	}

	if(gps_data.valid)
	{
		gps_data.field_pack.ts = 0;
		set_station_gps_status("FIXED");
		u32 gps_ts;
		if(WsFields::get_field_val_from_pack(&gps_data.field_pack, WsFields::FieldType::GPS_TS, NULL, &gps_ts) == WsCommon::RetRes::OK)
		{
			gps_data.field_pack.ts = gps_ts;
			if(!DatetimeRtc::cur_time_valid())
			{
				DatetimeRtc::set_tstamp_from_gps(gps_ts);
			}
		}
		else if(gw_ctx.time_is_valid)
		{
			gps_data.field_pack.ts = time(NULL);
		}

		if(gps_data.field_pack.ts != 0)
		{
			memcpy(gps_data.field_pack.packer_name, active_station->NAME, sizeof(gps_data.field_pack.packer_name));
			if (foreign_packet)
			{
				ESP_LOGI(TAG, "Foreign (sniffed) packet: updating last GPS pack without pushing to queue");
				WsGps::set_last_gps_data(&gps_data);
			}
			else
			{
				WsGps::push_gps_data(&gps_data);
				WeatherLogger::log_gps_fix(_state.pair_data.id,
					active_station->NAME, &gps_data);
			}
			WsGps::print_gps_data(&gps_data);
		}
		else
		{
			ESP_LOGE(TAG, "Not valid TS to add to GPS data");
		}
	}
	else if(sta_ctx.gps_pkt_received)
	{
		ESP_LOGE(TAG, "Station GPS fix failed");
		u8 sats;
		if(WsFields::get_field_val_from_pack(&gps_data.field_pack, WsFields::FieldType::GPS_SATS, NULL, &sats) == WsCommon::RetRes::OK)
		{
			char gps_err[32];
			snprintf(gps_err, sizeof(gps_err), "FAILED (sats=%u)", (unsigned int)sats);
			set_station_gps_status(gps_err);
		}
		else
		{
			set_station_gps_status("FAILED");
		}
	}
	
	// Only weather measurement packets belong in the telemetry buffer. A
	// WS1300 ADV/BOOT/GNSS packet produces a META-only pack that WsData::add() would pad
	// with the gateway's own barometer reading and upload as "weather".
	bool telemetry_pkt = true;
	if(active_station->TYPE == WsType::WS_TYPE_1300)
	{
		Ws1300::StaPacketType ws1300_type = Ws1300Helpers::get_packet_type(&pkt_received);
		telemetry_pkt = ws1300_type == Ws1300::StaPacketType::SENSORS;
	}

	if(field_pack.field_count > 0)
	{
		u32 sta_ts;
		field_pack.ts = 0;
		if(WsFields::get_field_val_from_pack(&field_pack, WsFields::FieldType::WEATHER_TS, NULL, &sta_ts) == WsCommon::RetRes::OK)
		{
			field_pack.ts = sta_ts;
		}
		else if(gw_ctx.time_is_valid)
		{
			field_pack.ts = time(NULL);
		}

		if(field_pack.ts != 0)
		{
			bool paired_and_not_foreign = (get_pair_state() == PairState::PAIRED) &&
				(get_cur_station() != NULL) &&
				(Settings::get_pair_mode() != PairMode::PAIR_MODE_OFF) &&
				!foreign_packet;

			if(field_pack.bad_values_detected)
			{
				// bad data exist
				ESP_LOGE(TAG, "Invalid data detected.");
				set_station_sensor_err("BAD_VALUES");
			}
			else
			{			
				if(g_ws_sensor_err[0] != '\0')
				{
					set_station_sensor_err("");
				}
				//add packet to buffer only if valid TS exists
				if (foreign_packet)
				{
					ESP_LOGI(TAG, "Foreign (sniffed) packet: not adding to telemetry buffer");
				}
				else if (get_pair_state() != PairState::PAIRED)
				{
					// Unpaired gateways decode for display only - with several
					// stations in range, buffering would mix their data upstream
					ESP_LOGI(TAG, "Unpaired: not adding to telemetry buffer");
				}
				else if (!telemetry_pkt)
				{
					ESP_LOGI(TAG, "Non-weather packet: not adding to telemetry buffer");
				}
				else
				{
					if(paired_and_not_foreign)
					{
						WsData::update_precip_rate_params(&field_pack);
					}
					if(WsData::add(&field_pack) == WsCommon::RetRes::OK)
					{
						ESP_LOGI(TAG, "Valid packet added to buffer. Len: %d", WsData::get_recv_count());
					}			
					else // Failed to add to buffer, probably full
					{
						ESP_LOGI(TAG, "Could not add packet to recv buffer.");
						if(WsData::get_recv_count() == WS_DATA_RECEIVE_BUFF_ITEMS)
						{
							ESP_LOGI(TAG, "Buffer full.");
						}
					}
				}
			}
		}
		else
		{
			ESP_LOGE(TAG, "Not valid TS to add to WEATHER data");
		}
	}

	// Print here to have TS
	if(field_pack.field_count > 0)
	{
		if(!field_pack.bad_values_detected)
		{
			bool paired_and_not_foreign = (get_pair_state() == PairState::PAIRED) &&
				(get_cur_station() != NULL) &&
				(Settings::get_pair_mode() != PairMode::PAIR_MODE_OFF) &&
				!foreign_packet;

			// Strictly only publish weather events for the paired station's telemetry data.
			// Non-telemetry packets (WS1300 ADV/BOOT/GNSS) contain only META fields and would
			// overwrite the valid weather FieldPack on the TFT LCD and in WeatherLogger.
			if (paired_and_not_foreign && telemetry_pkt)
			{
				GlobalEvents::publish(GlobalEvents::EventId::WS_DATA_RECEIVED, &field_pack, sizeof(field_pack));
			}
			else if (!telemetry_pkt)
			{
				ESP_LOGI(TAG, "Skipping WS_DATA_RECEIVED publish for non-telemetry packet (META-only).");
			}
			else
			{
				ESP_LOGI(TAG, "Filtering out unpaired/foreign packet from weather data stream.");
			}
		}		
		WsFields::print_field_pack(&field_pack);
	}	

	if(gw_ctx.gps_needed && sta_ctx.gps_req_permitted)
	{
		_state.gps_last_req_ts_sec = Utils::get_tick_sec();
		_state.gps_req_attempts++;
		g_ws1300_gps_req_forced = false;
		if(g_ownership_challenge_armed)
		{
			g_ownership_challenge_armed = false;
			g_ownership_challenge_pending = true;
			g_ownership_confirmed = false;
			g_ownership_challenge_station_id = _state.pair_data.id;
			g_ownership_challenge_started_sec = Utils::get_tick_sec();
			g_ownership_challenge_packets_seen = 0;
			ESP_LOGW(TAG, "Station ownership challenge sent to 0x%08X",
				(unsigned int)g_ownership_challenge_station_id);
		}
		ESP_LOGW(TAG, "GPS request sent to station (Attempt %d)", (int)_state.gps_req_attempts);
		set_station_gps_status("REQUESTED");
	}

	set_station_ack_loss(sta_ctx.ack_loss_detected);
	if(sta_ctx.ack_loss_detected)
	{
		ESP_LOGW(TAG, "Station is losing ACKs");
	}

	if(sta_ctx.lost_packets > 0)
	{
		add_station_lost_packets(sta_ctx.lost_packets);
		ESP_LOGW(TAG, "Packet loss detected (%u lost)", (unsigned int)sta_ctx.lost_packets);
	}

	if(sta_ctx.reboot_detected)
	{
		Log::logw(Log::Code::SNIFFER_WS_REBOOT);
		ESP_LOGW(TAG, "Station rebooted");
	}

	// Only a validated packet from the paired P2P station can advance the
	// reception window. Sniffed/mesh packets must not affect this link metric.
	if(is_paired && !foreign_packet && sta_ctx.packet_index_valid && counter_sample != nullptr)
	{
		counter_sample->counter = sta_ctx.packet_index;
		counter_sample->valid = true;
		counter_sample->station_reset = sta_ctx.reboot_detected;
	}

	if(sta_ctx.is_busy)
	{
		return PktStatus::OK_BUSY;
	}

	return field_pack.bad_values_detected ? PktStatus::BAD_DATA : PktStatus::OK;
}

WsCommon::RetRes set_cur_station_obj(WsType station_type)
{
	if (station_type == WsType::WS_TYPE_ALL)
	{
		if (get_pair_state() == PairState::PAIRED && _state.cur_station != NULL)
		{
			// A paired gateway keeps its station object - ALL only affects
			// unpaired sniffing, and the paired link must keep working
			return WsCommon::RetRes::OK;
		}
		_state.cur_station = NULL;
		return WsCommon::RetRes::OK;
	}

	for(int i = 0; i < (sizeof(STATION_LIST)/sizeof(WsBase::MultiStation)); i++)
	{
		if(STATION_LIST[i].TYPE == station_type)
		{
			_state.cur_station = (WsBase::MultiStation*) &STATION_LIST[i];
			return WsCommon::RetRes::OK;
		}
	}

	_state.cur_station = NULL;

	return WsCommon::RetRes::ERROR_NOT_SUPPORTED;
}

/** Called from the web server task. Without the ownership lock this retunes the
 * radio out from under whatever the packet handler task is doing, including a
 * Meshtastic transmit in flight. */
WsCommon::RetRes apply_station_change(WsType station_type)
{
	if(!radio_lock(RADIO_LOCK_TIMEOUT_MS))
	{
		return WsCommon::RetRes::ERROR_TIMEOUT;
	}

	WsCommon::RetRes res = set_cur_station_obj(station_type);
	if(res == WsCommon::RetRes::OK)
	{
		retune_for_current_state();
		sx126x_rx_cont();
	}

	radio_unlock();
	return res;
}

/** Called from the web server task - see apply_station_change(). */
void notify_sniff_mode_toggled()
{
	if (_packet_handler_task_handle != NULL)
	{
		xTaskNotifyGive(_packet_handler_task_handle);
	}

	if(!radio_lock(RADIO_LOCK_TIMEOUT_MS))
	{
		// The handler task was already notified and will retune on its next
		// pass, so dropping this one is safe.
		return;
	}
	retune_for_current_state();
	sx126x_rx_cont();
	radio_unlock();
}

WsBase::MultiStation* get_cur_station()
{
	if (g_sniff_mode_enabled && _state.cur_station == NULL)
	{
		WsType station_type = g_receiver_profile;
		for(int i = 0; i < (sizeof(STATION_LIST)/sizeof(WsBase::MultiStation)); i++)
		{
			if(STATION_LIST[i].TYPE == station_type)
			{
				return (WsBase::MultiStation*)&STATION_LIST[i];
			}
		}
		return (WsBase::MultiStation*)&STATION_LIST[1]; // Default to WS1300 for sniffing / WS_TYPE_ALL
	}
	return _state.cur_station;
}

/******************************************************************************
 * @brief Calculate if new GPS data are required. Data need to be updated every WS_GPS_REQUEST_INTERVAL_SEC,
 * with a max request rate (if received fix failed) of WS_GPS_REQ_COOLDOWN_SEC.
 * @param time_is_valid True if time is valid
 * @return true if needed
 ******************************************************************************/
bool calc_req_gps(bool time_is_valid)
{
	WsGps::GpsData last_gps_data = {};
	WsGps::get_last_gps_data(&last_gps_data);

	// If we already have a usable GPS fix, reset attempt counter
	if(WsGps::gps_data_is_usable(&last_gps_data))
	{
		_state.gps_req_attempts = 0;
	}

	if(g_ws1300_gps_req_forced || g_ownership_challenge_armed)
	{
		_state.gps_req_attempts = 0;
		ESP_LOGI(TAG, "Forced GPS request / ownership challenge active");
		return true;
	}

	bool gps_update_needed = false;
	bool gps_cooldown_needed = false;

	if(!time_is_valid)
	{
		gps_update_needed = true;
	}
	else if(!WsGps::gps_data_is_usable(&last_gps_data) ||
		last_gps_data.field_pack.ts > time(NULL) ||
		time(NULL) - last_gps_data.field_pack.ts > WS_GPS_REQUEST_INTERVAL_SEC)
	{
		gps_update_needed = true;
	}

	// Cooldown logic:
	// - If a usable fix exists -> 8-hour cooldown (WS_GPS_REQ_COOLDOWN_SEC)
	// - If NO fix exists:
	//   * 1 single retry (attempt 1 -> 2) is permitted after 5 minutes (300s)
	//   * After 2 attempts (attempts >= 2), fallback to 8-hour cooldown to save station battery
	uint32_t cooldown_sec = WS_GPS_REQ_COOLDOWN_SEC;
	if(!WsGps::gps_data_is_usable(&last_gps_data) && _state.gps_req_attempts < 2)
	{
		cooldown_sec = 300; // 5 minutes gap before single 2nd try
	}

	if(_state.gps_last_req_ts_sec != 0 && Utils::get_tick_sec() - _state.gps_last_req_ts_sec <= cooldown_sec)
	{
		gps_cooldown_needed = true;
	}

	return gps_update_needed && !gps_cooldown_needed;
}

RetResult trigger_gps_request()
{
	g_ws1300_gps_req_forced = true;
	g_ownership_challenge_armed = true;
	g_ownership_challenge_pending = false;
	g_ownership_confirmed = false;
	g_ownership_challenge_station_id = _state.pair_data.id;
	g_ownership_challenge_started_sec = 0;
	g_ownership_challenge_packets_seen = 0;
	_state.gps_last_req_ts_sec = 0; // Reset cooldown
	_state.gps_req_attempts = 0;    // Reset attempt counter
	ESP_LOGW(TAG, "Manual GPS request armed for next station check-in");
	return RET_OK;
}

/******************************************************************************
 * @brief Enable continuous RX
 ******************************************************************************/
void sx126x_rx_cont()
{
	// Reapplies the station modem config including SetPublicNetwork(false),
	// which overwrites the Meshtastic sync word. Running this concurrently with
	// a mesh borrow is precisely the failure the ownership lock exists to stop.
	if(!radio_lock(RADIO_LOCK_TIMEOUT_MS))
	{
		return;
	}

	if(Settings::get_gateway_radio_mode() == Settings::GatewayRadioMode::MESHCORE_GATEWAY)
	{
		uint32_t sf = 10, bw_idx = 1;
		uint8_t cr = 1;
		uint16_t preamble = 16;
		uint8_t prof_idx = Settings::get_meshcore_gw_profile_idx();
		if (prof_idx == 255) {
			uint32_t custom_sf = Settings::get_meshcore_gw_sf();
			uint16_t custom_bw = Settings::get_meshcore_gw_bw_khz();
			uint8_t custom_cr = Settings::get_meshcore_gw_cr();
			
			if (custom_sf >= 7 && custom_sf <= 12) sf = custom_sf;
			if (custom_bw == 62 || custom_bw == 63) bw_idx = 3;
			else if (custom_bw == 125) bw_idx = 0;
			else if (custom_bw == 250) bw_idx = 1;
			else if (custom_bw == 500) bw_idx = 2;
			if (custom_cr >= 5 && custom_cr <= 8) cr = custom_cr - 4;
			else if (custom_cr >= 1 && custom_cr <= 4) cr = custom_cr;
		} else {
			switch (prof_idx) {
				case 1: sf = 8; bw_idx = 1; cr = 1; break; // Medium (250 kHz, CR 4/5)
				case 2: sf = 11; bw_idx = 0; cr = 1; break; // Narrow Extreme (125 kHz, CR 4/5)
				case 3: sf = 7; bw_idx = 1; cr = 1; break; // Fast (250 kHz, CR 4/5)
				case 4: sf = 8; bw_idx = 3; cr = 4; break; // EU/UK Narrow (62.5 kHz, CR 4/8 - MeshCore EU Standard)
				case 0:
				default: sf = 10; bw_idx = 1; cr = 1; break; // Long Range (250 kHz, CR 4/5)
			}
		}

#ifdef CONFIG_SCREENLESS
		uint32_t bw_hz = (bw_idx == 3) ? 62500 : ((bw_idx == 0) ? 125000 : ((bw_idx == 1) ? 250000 : 500000));
		KissClient::set_radio(_current_freq_hz, bw_hz, sf, cr);
		KissClient::set_preamble(preamble);
#else
		Radio.SetModem(MODEM_LORA);
		Radio.SetPublicNetwork(false);
		Radio.SetRxConfig(MODEM_LORA, bw_idx, sf, cr, 0, preamble, 0, false, 0, true, false, 0, false, true, 0, 0);
		Radio.Rx(0);
#endif
		radio_unlock();
		return;
	}

	const WsBase::MultiStation* active_station = get_cur_station();
	if (active_station == NULL)
	{
		if (g_sniff_mode_enabled)
		{
			// Default to first station (WS1001) for sniffing if not paired
			active_station = &STATION_LIST[0];
		}
		else
		{
#ifndef CONFIG_SCREENLESS
			Radio.Standby();
#endif
			radio_unlock();
			return;
		}
	}

#ifdef CONFIG_SCREENLESS
	uint32_t bw_hz = 125000;
	if (active_station->RF_CONFIG.config.config_lora.bandwidth == WsBase::ConfigLora::BW_250_KHZ) {
		bw_hz = 250000;
	} else if (active_station->RF_CONFIG.config.config_lora.bandwidth == WsBase::ConfigLora::BW_500_KHZ) {
		bw_hz = 500000;
	}

	uint8_t sf = (uint8_t)active_station->RF_CONFIG.config.config_lora.datarate;
	uint8_t cr = (uint8_t)active_station->RF_CONFIG.config.config_lora.coderate;
	uint16_t preamble = active_station->RF_CONFIG.config.config_lora.preamble_len;

	uint32_t freq_hz = _current_freq_hz;
	if (freq_hz == 0) {
		const WsFreqDesc *desc = get_freq_desc(nullptr, Settings::get_freq_region());
		if (desc != nullptr) freq_hz = desc->freq_hz;
	}

	KissClient::set_radio(freq_hz, bw_hz, sf, cr);
	KissClient::set_preamble(preamble);
#else
	switch(active_station->RF_CONFIG.rf_type)
	{
		case WsBase::RfType::FSK:
			Radio.SetModem(MODEM_FSK);
			break;
		case WsBase::RfType::LORA:
			Radio.SetModem(MODEM_LORA);
			Radio.SetPublicNetwork(false); // Use private sync word (0x1424) for direct P2P nodes (both WS1001 & WS1300)
			Radio.SetRxConfig(MODEM_LORA, 
								active_station->RF_CONFIG.config.config_lora.bandwidth,
								active_station->RF_CONFIG.config.config_lora.datarate,
								active_station->RF_CONFIG.config.config_lora.coderate,
								0,
								active_station->RF_CONFIG.config.config_lora.preamble_len,
								active_station->RF_CONFIG.config.config_lora.symb_timeout, 
								active_station->RF_CONFIG.config.config_lora.fixed_len, 
								active_station->RF_CONFIG.config.config_lora.pl_len, 
								active_station->RF_CONFIG.config.config_lora.crc_on,
								false, 0, false, true, 0, 0);
			{
				uint8_t sw1 = Radio.Read(0x0740);
				uint8_t sw2 = Radio.Read(0x0741);
				ESP_LOGI(TAG, "LoRa SyncWord registers (0x0740/0x0741): 0x%02X 0x%02X", sw1, sw2);
			}
			break;
		default:
			Radio.SetModem(MODEM_LORA);
			break;
	}
	Radio.Rx(0);
#endif
	radio_unlock();
}

/******************************************************************************
 * @brief RX callback function called by the sx126x driver when a packet has arrived.
 * @param raw_packet_buff Buffer containing received packet
 * @param raw_packet_buff_length Length of buffer containing received packet
 * @param rssi RSSI of received packet
 * @param snr Signal to noise ratio of received packet
 ******************************************************************************/
void sx126x_rx_cb(uint8_t *raw_packet_buff, uint16_t raw_packet_buff_length, int16_t rssi, int8_t snr )
{
	if(raw_packet_buff_length <= 255)
	{
		PacketQueueMsg packet_msg = {};
		memcpy(packet_msg.buffer, raw_packet_buff, raw_packet_buff_length);
		packet_msg.length = raw_packet_buff_length;
		packet_msg.rssi = rssi;
		packet_msg.snr = snr;

		if(xQueueSend(_packet_queue, &packet_msg, pdMS_TO_TICKS(1000)) == pdFALSE)
		{
			ESP_LOGE(TAG, "sx126x_rx_cb packet_msg lost");
		}
	}	
}

#ifndef CONFIG_SCREENLESS
/******************************************************************************
 * @brief Transmit payload
 ******************************************************************************/
void sx126x_tx(uint8_t *buffer, uint8_t size)
{
	if(get_cur_station() == NULL) return;

	// Same task as the Meshtastic borrow today, so this cannot currently
	// interleave with one - but the config-then-send sequence below is exactly
	// the kind of thing that must not be split, so take ownership explicitly
	// rather than relying on that staying true.
	if(!radio_lock(RADIO_LOCK_TIMEOUT_MS))
	{
		ESP_LOGW(TAG, "sx126x_tx: radio busy, station transmit skipped");
		return;
	}

	switch(get_cur_station()->RF_CONFIG.rf_type)
	{
		case WsBase::RfType::FSK:
			radio_unlock();
			return; // TODO: Not gonna need it probably
			break;
		case WsBase::RfType::LORA:
			Radio.SetTxConfig(MODEM_LORA,
							22,
							0,
							get_cur_station()->RF_CONFIG.config.config_lora.bandwidth,
							get_cur_station()->RF_CONFIG.config.config_lora.datarate,
							get_cur_station()->RF_CONFIG.config.config_lora.coderate,
							get_cur_station()->RF_CONFIG.config.config_lora.preamble_len, 
							get_cur_station()->RF_CONFIG.config.config_lora.fixed_len, 
							get_cur_station()->RF_CONFIG.config.config_lora.crc_on,
							false, 0, false, 1000);
			break;
		default:
			ESP_LOGE(TAG, "sx126x_tx: RF type unhandled, abort TX");
			radio_unlock();
			return;
			break;
	}

	Radio.Send(buffer, size);

	if (get_cur_station() != nullptr && get_cur_station()->RF_CONFIG.rf_type == WsBase::RfType::LORA) {
		uint32_t freq_hz = 0;
		get_listen_info(&freq_hz, nullptr, 0);
		WebServer::log_tx_packet(buffer, size, freq_hz,
			(uint8_t)get_cur_station()->RF_CONFIG.config.config_lora.datarate,
			125000,
			(uint8_t)(get_cur_station()->RF_CONFIG.config.config_lora.coderate + 4),
			22);
	}

	// Released as soon as the payload is handed to the chip. The station TX-done
	// callback needs this lock to run the restore, so holding it across the
	// transmit would deadlock exactly the way the Meshtastic path would.
	radio_unlock();
}

/** Put the station link back. MUST be called with radio ownership held - the
 * nested set_radio_freq()/sx126x_rx_cont() re-take the recursive lock, but this
 * function on its own does not establish ownership. */
static void restore_station_radio_after_mesh_borrow()
{
	_mesh_tx_in_progress = false;
	Radio.Standby();
	set_radio_freq(Settings::get_freq_region());
	sx126x_rx_cont();
}

/** Meshtastic-style listen-before-talk. A randomized contention pause avoids
 * two locally generated senders always scanning in lockstep, then two native
 * SX1262 CAD passes look for the configured LoRa waveform. A busy or
 * indeterminate result is conservative: defer instead of transmitting. */
static bool meshtastic_channel_clear(uint32_t sf)
{
	const uint32_t CONTENTION_MIN_MS = 50;
	const uint32_t CONTENTION_SPAN_MS = 201; /* 50..250 ms inclusive */
	const uint32_t CAD_DONE_WAIT_MS = 250;
	const uint8_t CAD_PASSES = 2;
	const uint8_t CAD_DET_MIN = 10;
	const uint8_t cad_det_peak = (uint8_t)(sf + 13); /* Semtech/RadioLib default */

	uint32_t contention_ms = CONTENTION_MIN_MS + (esp_random() % CONTENTION_SPAN_MS);
	vTaskDelay(pdMS_TO_TICKS(contention_ms));

	for (uint8_t pass = 0; pass < CAD_PASSES; pass++)
	{
		uint32_t seq_before = _cad_done_seq;
		_cad_activity_detected = false;

		SX126xClearIrqStatus(IRQ_RADIO_ALL);
		SX126xSetCadParams(LORA_CAD_02_SYMBOL, cad_det_peak, CAD_DET_MIN,
							 LORA_CAD_ONLY, 0);
		Radio.StartCad();

		uint32_t waited_ms = 0;
		while (_cad_done_seq == seq_before && waited_ms < CAD_DONE_WAIT_MS)
		{
			vTaskDelay(pdMS_TO_TICKS(5));
			waited_ms += 5;
		}

		if (_cad_done_seq == seq_before)
		{
			ESP_LOGW(TAG, "mesh LBT: CAD pass %u timed out; deferring",
				(unsigned)(pass + 1));
			return false;
		}

		if (_cad_activity_detected)
		{
			ESP_LOGI(TAG, "mesh LBT: channel busy on CAD pass %u after %ums contention; deferring",
				(unsigned)(pass + 1), (unsigned)contention_ms);
			return false;
		}
	}

	ESP_LOGD(TAG, "mesh LBT: channel clear after %ums contention and %u CAD passes",
		(unsigned)contention_ms, (unsigned)CAD_PASSES);
	return true;
}

/******************************************************************************
 * @brief Transmit a raw frame with foreign LoRa parameters (Meshtastic).
 *
 * The station link owns this radio. We borrow it for one packet: park it,
 * retune, transmit, wait for TX-done, and let sx126x_tx_cb hand it back.
 * See ws/include/ws_handler.h for the contract.
 ******************************************************************************/
bool sx126x_tx_meshtastic(uint8_t *buffer, uint8_t size, uint32_t freq_hz,
                          uint32_t sf, uint32_t bw_idx, uint8_t cr, uint16_t preamble,
                          const RadioLease *lease)
{
	if(buffer == NULL || size == 0) return false;
	if(get_cur_station() == NULL)
	{
		ESP_LOGW(TAG, "mesh tx: no active station, radio not initialised");
		return false;
	}

	// Never cut into an in-flight station ACK. The ACK has to land inside the
	// station's ~300 ms receive window; a foreign transmit here would lose the
	// station's data packet for this cycle.
	const uint32_t IDLE_WAIT_MS = 1500;
	uint32_t waited_ms = 0;
	while(Radio.GetStatus() == RF_TX_RUNNING && waited_ms < IDLE_WAIT_MS)
	{
		vTaskDelay(pdMS_TO_TICKS(10));
		waited_ms += 10;
	}
	if(Radio.GetStatus() == RF_TX_RUNNING)
	{
		ESP_LOGW(TAG, "mesh tx: radio still transmitting after %ums, skipping", (unsigned)IDLE_WAIT_MS);
		return false;
	}

	// An idle radio is NOT the same as a quiet radio. sx126x_tx_cb is dispatched
	// off the radio IRQ, so the station ACK's TX-done can still be queued while
	// the chip already reports idle. If it lands after the retune below, it runs
	// set_radio_freq + sx126x_rx_cont and drags the radio back to the station
	// frequency, private sync word and station modem config - and Radio.Send()
	// then puts a Meshtastic frame on air with all of it wrong. Nothing in the
	// log looks amiss; the packet simply never decodes anywhere.
	// Wait for the callback counter to hold still before touching anything.
	const uint32_t QUIET_WINDOW_MS = 40;
	const uint32_t QUIET_WAIT_MAX_MS = 600;
	uint32_t quiet_ms = 0;
	uint32_t quiet_waited_ms = 0;
	uint32_t last_seq = _tx_done_seq;
	while(quiet_ms < QUIET_WINDOW_MS && quiet_waited_ms < QUIET_WAIT_MAX_MS)
	{
		vTaskDelay(pdMS_TO_TICKS(10));
		quiet_waited_ms += 10;
		if(_tx_done_seq != last_seq)
		{
			last_seq = _tx_done_seq;
			quiet_ms = 0; // a callback landed, restart the quiet window
		}
		else
		{
			quiet_ms += 10;
		}
	}
	if(quiet_ms < QUIET_WINDOW_MS)
	{
		ESP_LOGW(TAG, "mesh tx: radio callbacks never went quiet, skipping this cycle");
		return false;
	}

	// Take exclusive ownership of the radio for the whole borrow: config, LBT,
	// transmit and restore. Until this existed, four web/state_sniffer entry
	// points could retune the chip from another task at any point below,
	// including during the ~930 ms SF11 transmit itself - which corrupts the
	// tail of a frame whose header the receiver has already accepted, and shows
	// up at the far end as a payload CRC mismatch rather than a lost packet.
	if(!radio_lock(RADIO_LOCK_TIMEOUT_MS))
	{
		ESP_LOGW(TAG, "mesh tx: could not take radio ownership, skipping this cycle");
		return false;
	}

	uint32_t expected_airtime_ms = Radio.TimeOnAir(MODEM_LORA, bw_idx, sf, cr,
												   preamble, false, size, true);

	// Dual-stage validation: Stage 2 - Pre-RF Retune Gate under radio_lock
	if(lease != nullptr && lease->expiry_ms > 0)
	{
		int64_t now_ms = (int64_t)(esp_timer_get_time() / 1000LL);
		int64_t required_ms = (int64_t)expected_airtime_ms + 1000LL;
		if(now_ms + required_ms > lease->expiry_ms)
		{
			ESP_LOGW(TAG, "mesh tx: aborting borrow, remaining lease %lld ms < required %lld ms",
				(long long)(lease->expiry_ms - now_ms), (long long)required_ms);
			radio_unlock();
			return false;
		}
	}

	// Tell the TX-done callback to force a frequency restore. The normal path
	// only restores when PAIRED, which would otherwise strand an unpaired
	// gateway on the Meshtastic frequency.
	//
	// It also tells the callback NOT to touch the radio at all: we own it and we
	// are about to block waiting for that callback, so a callback that tried to
	// take the lock would deadlock against us. See sx126x_tx_cb().
	_mesh_tx_in_progress = true;

	Radio.Standby();
	Radio.SetChannel(freq_hz);
	Radio.SetTxConfig(MODEM_LORA,
					  mesh_tx_power_dbm(),
					  0,                 // fdev, FSK only
					  bw_idx,
					  sf,
					  cr,
					  preamble,
					  false,             // fixLen: Meshtastic uses the explicit header
					  true,              // crcOn
					  false, 0, false,   // no freq hopping, no IQ inversion
					  3000);             // TX timeout ms

	// Meshtastic sync word is 0x2b. RadioLib's SX126x::setSyncWord(0x2b, 0x44)
	// writes the register pair below; Radio.SetPublicNetwork() cannot express
	// it (it only offers the LoRaWAN 0x3444 / private 0x1424 pair).
	Radio.Write(0x0740, 0x24);
	Radio.Write(0x0741, 0xB4);

	ESP_LOGI(TAG, "mesh tx: %u bytes @ %u Hz SF%u BW_IDX%u CR4/%u preamble %u %ddBm, "
				  "expect ~%ums airtime",
		(unsigned)size, (unsigned)freq_hz, (unsigned)sf, (unsigned)bw_idx,
		(unsigned)(cr + 4), (unsigned)preamble, (int)mesh_tx_power_dbm(),
		(unsigned)expected_airtime_ms);

	if (!meshtastic_channel_clear(sf))
	{
		restore_station_radio_after_mesh_borrow();
		radio_unlock();
		return false;
	}

	// CAD_ONLY drops the chip to STDBY_RC, which powers down the XOSC. The next
	// SetTx restarts it and the state machine rewrites XTA/XTB to 0x12. Re-enter
	// STDBY_XOSC and restore the trim so the write is not clobbered on TX entry.
	Radio.Standby();
	Radio.Write(0x0736, 0x0D); // Ensure Standard IQ polarity register (Semtech DS 15.4)
	Radio.Write(0x0740, 0x24); // Ensure Meshtastic sync word 0x2B
	Radio.Write(0x0741, 0xB4);

	// Read the sync word back off the chip rather than trusting the write, and
	// do it HERE - after LBT, immediately before the transmit. The check used to
	// sit right after the write, which left it verifying a state that was then
	// up to 750 ms stale by the time the frame actually went out (50-250 ms
	// contention plus two 250 ms CAD passes). Whatever the check is guarding
	// against had a three-quarter-second window to happen unobserved.
	//
	// This is the single setting that silently drops a frame into the void:
	// every other parameter being right still gets you nothing if a stray
	// SetPublicNetwork() put 0x1424 back.
	uint8_t sw_hi = Radio.Read(0x0740);
	uint8_t sw_lo = Radio.Read(0x0741);
	if(sw_hi != 0x24 || sw_lo != 0xB4)
	{
		ESP_LOGE(TAG, "mesh tx: sync word is 0x%02X%02X, not Meshtastic 0x2b - aborting, "
					  "the frame would be undecodable", sw_hi, sw_lo);
		restore_station_radio_after_mesh_borrow();
		radio_unlock();
		return false;
	}

	// Snapshot the callback counter as late as possible: any TX-done already
	// dispatched is accounted for here, so the wait below can only be satisfied
	// by OUR transmit.
	uint32_t seq_before = _tx_done_seq;

	Radio.Send(buffer, size);
	ESP_LOGI(TAG, "mesh tx: active SyncWord (0x0740/0x0741): 0x%02X 0x%02X",
			 Radio.Read(0x0740), Radio.Read(0x0741));

	// Block until the radio is back on the station link. SF11 airtime for a
	// ~60 byte frame is roughly 900 ms; the cap is generous but bounded.
	const uint32_t TX_DONE_WAIT_MS = 5000;
	waited_ms = 0;
	while(_tx_done_seq == seq_before && waited_ms < TX_DONE_WAIT_MS)
	{
		vTaskDelay(pdMS_TO_TICKS(10));
		waited_ms += 10;
	}

	if(_tx_done_seq == seq_before)
	{
		// TX-done never fired. Recover the station link by hand rather than
		// leaving the gateway parked on a foreign frequency.
		ESP_LOGE(TAG, "mesh tx: TX-done timeout after %ums, forcing station link restore",
			(unsigned)TX_DONE_WAIT_MS);
		restore_station_radio_after_mesh_borrow();
		radio_unlock();
		return false;
	}

	// The restore is done HERE, by the borrowing task, under the ownership lock -
	// not by sx126x_tx_cb on the expander IO task as it used to be. Two reasons:
	// the callback cannot take the lock without deadlocking against the wait
	// above, and doing the restore inline means the radio is back on the station
	// link before any other task is allowed near it.
	restore_station_radio_after_mesh_borrow();

	bool airtime_plausible = !(expected_airtime_ms > 50 &&
							   waited_ms + 20 < (expected_airtime_ms / 2));

	radio_unlock();

	// A transmit that "finishes" far faster than its own airtime did not happen.
	// Report it instead of returning success, which is what the previous
	// "complete in 0ms" was doing.
	if(!airtime_plausible)
	{
		ESP_LOGE(TAG, "mesh tx: completed in %ums but airtime should be ~%ums - "
					  "the frame was almost certainly not transmitted",
			(unsigned)waited_ms, (unsigned)expected_airtime_ms);
		return false;
	}

	ESP_LOGI(TAG, "mesh tx: complete in %ums (expected ~%ums), station link restored",
		(unsigned)waited_ms, (unsigned)expected_airtime_ms);

	uint32_t bw_hz = 125000;
	if (bw_idx == 0) bw_hz = 125000;
	else if (bw_idx == 1) bw_hz = 250000;
	else if (bw_idx == 2) bw_hz = 500000;
	else if (bw_idx == 3) bw_hz = 62500;
	WebServer::log_tx_packet(buffer, size, freq_hz, (uint8_t)sf, bw_hz, (uint8_t)(cr > 4 ? cr : cr + 4), mesh_tx_power_dbm());

	return true;
}

/******************************************************************************
 * @brief Transmit a raw frame with foreign LoRa parameters (MeshCore).
 *
 * The station link owns this radio. We borrow it for one packet: park it,
 * retune, transmit, wait for TX-done, and let sx126x_tx_cb hand it back.
 * See ws/include/ws_handler.h for the contract.
 ******************************************************************************/
bool sx126x_tx_meshcore(uint8_t *buffer, uint8_t size, uint32_t freq_hz,
                        uint32_t sf, uint32_t bw_idx, uint8_t cr, uint16_t preamble,
                        const RadioLease *lease)
{
	if(buffer == NULL || size == 0) return false;
	if(get_cur_station() == NULL)
	{
		ESP_LOGW(TAG, "meshcore tx: no active station, radio not initialised");
		return false;
	}

	const uint32_t IDLE_WAIT_MS = 1500;
	uint32_t waited_ms = 0;
	while(Radio.GetStatus() == RF_TX_RUNNING && waited_ms < IDLE_WAIT_MS)
	{
		vTaskDelay(pdMS_TO_TICKS(10));
		waited_ms += 10;
	}
	if(Radio.GetStatus() == RF_TX_RUNNING)
	{
		ESP_LOGW(TAG, "meshcore tx: radio still transmitting after %ums, skipping", (unsigned)IDLE_WAIT_MS);
		return false;
	}

	const uint32_t QUIET_WINDOW_MS = 40;
	const uint32_t QUIET_WAIT_MAX_MS = 600;
	uint32_t quiet_ms = 0;
	uint32_t quiet_waited_ms = 0;
	uint32_t last_seq = _tx_done_seq;
	while(quiet_ms < QUIET_WINDOW_MS && quiet_waited_ms < QUIET_WAIT_MAX_MS)
	{
		vTaskDelay(pdMS_TO_TICKS(10));
		quiet_waited_ms += 10;
		if(_tx_done_seq != last_seq)
		{
			last_seq = _tx_done_seq;
			quiet_ms = 0;
		}
		else
		{
			quiet_ms += 10;
		}
	}
	if(quiet_ms < QUIET_WINDOW_MS)
	{
		ESP_LOGW(TAG, "meshcore tx: radio callbacks never went quiet, skipping this cycle");
		return false;
	}

	if(!radio_lock(RADIO_LOCK_TIMEOUT_MS))
	{
		ESP_LOGW(TAG, "meshcore tx: could not take radio ownership, skipping this cycle");
		return false;
	}

	uint32_t expected_airtime_ms = Radio.TimeOnAir(MODEM_LORA, bw_idx, sf, cr,
												   preamble, false, size, true);

	// Dual-stage validation: Stage 2 - Pre-RF Retune Gate under radio_lock
	if(lease != nullptr && lease->expiry_ms > 0)
	{
		int64_t now_ms = (int64_t)(esp_timer_get_time() / 1000LL);
		int64_t required_ms = (int64_t)expected_airtime_ms + 1000LL;
		if(now_ms + required_ms > lease->expiry_ms)
		{
			ESP_LOGW(TAG, "meshcore tx: aborting borrow, remaining lease %lld ms < required %lld ms",
				(long long)(lease->expiry_ms - now_ms), (long long)required_ms);
			radio_unlock();
			return false;
		}
	}

	_mesh_tx_in_progress = true;

	Radio.Standby();
	Radio.SetChannel(freq_hz);
	Radio.SetTxConfig(MODEM_LORA,
					  meshcore_tx_power_dbm(),
					  0,
					  bw_idx,
					  sf,
					  cr,
					  preamble,
					  false,
					  true,
					  false, 0, false,
					  3000);

	// MeshCore standard private sync word (0x14, 0x24)
	Radio.Write(0x0740, 0x14);
	Radio.Write(0x0741, 0x24);

	ESP_LOGI(TAG, "meshcore tx: %u bytes @ %u Hz SF%u BW_IDX%u CR4/%u preamble %u %ddBm, "
				  "expect ~%ums airtime",
		(unsigned)size, (unsigned)freq_hz, (unsigned)sf, (unsigned)bw_idx,
		(unsigned)(cr + 4), (unsigned)preamble, (int)meshcore_tx_power_dbm(),
		(unsigned)expected_airtime_ms);

	if (!meshtastic_channel_clear(sf))
	{
		restore_station_radio_after_mesh_borrow();
		radio_unlock();
		return false;
	}

	Radio.Standby();
	Radio.Write(0x0736, 0x0D);
	Radio.Write(0x0740, 0x14);
	Radio.Write(0x0741, 0x24);

	uint8_t sw_hi = Radio.Read(0x0740);
	uint8_t sw_lo = Radio.Read(0x0741);
	if(sw_hi != 0x14 || sw_lo != 0x24)
	{
		ESP_LOGE(TAG, "meshcore tx: sync word is 0x%02X%02X, not MeshCore 0x1424 - aborting", sw_hi, sw_lo);
		restore_station_radio_after_mesh_borrow();
		radio_unlock();
		return false;
	}

	uint32_t seq_before = _tx_done_seq;

	Radio.Send(buffer, size);

	const uint32_t TX_DONE_WAIT_MS = 5000;
	waited_ms = 0;
	while(_tx_done_seq == seq_before && waited_ms < TX_DONE_WAIT_MS)
	{
		vTaskDelay(pdMS_TO_TICKS(10));
		waited_ms += 10;
	}

	if(_tx_done_seq == seq_before)
	{
		ESP_LOGE(TAG, "meshcore tx: TX-done timeout after %ums, forcing station link restore",
			(unsigned)TX_DONE_WAIT_MS);
		restore_station_radio_after_mesh_borrow();
		radio_unlock();
		return false;
	}

	restore_station_radio_after_mesh_borrow();

	bool airtime_plausible = !(expected_airtime_ms > 50 &&
							   waited_ms + 20 < (expected_airtime_ms / 2));

	radio_unlock();

	if(!airtime_plausible)
	{
		ESP_LOGE(TAG, "meshcore tx: completed in %ums but airtime should be ~%ums",
			(unsigned)waited_ms, (unsigned)expected_airtime_ms);
		return false;
	}

	ESP_LOGI(TAG, "meshcore tx: completed successfully in %ums", (unsigned)waited_ms);

	uint32_t bw_hz = 125000;
	if (bw_idx == 0) bw_hz = 125000;
	else if (bw_idx == 1) bw_hz = 250000;
	else if (bw_idx == 2) bw_hz = 500000;
	else if (bw_idx == 3) bw_hz = 62500;
	WebServer::log_tx_packet(buffer, size, freq_hz, (uint8_t)sf, bw_hz, (uint8_t)(cr > 4 ? cr : cr + 4), meshcore_tx_power_dbm());

	return true;
}

/******************************************************************************
 * @brief Channel Activity Detection finished callback.
 ******************************************************************************/
void sx126x_cad_cb(bool channel_activity_detected)
{
	_cad_activity_detected = channel_activity_detected;
	_cad_done_seq++;
}

/******************************************************************************
 * @brief Transmit finished callback implementation
 ******************************************************************************/
void sx126x_tx_cb()
{
	// DEADLOCK CONTRACT - this runs on the expander IO task, not the packet
	// handler task.
	//
	// While a Meshtastic borrow is in flight, sx126x_tx_meshtastic() holds the
	// radio ownership lock AND is blocked waiting on _tx_done_seq. If we tried
	// to take the lock here we would block forever and it would wait forever.
	// So during a borrow this callback does exactly one thing: publish the
	// counter. The borrowing task performs the restore itself, inline, under the
	// lock it already holds.
	// Note this does NOT clear _mesh_tx_in_progress. A stale station TX-done can
	// still be queued when the borrow starts, and clearing the flag here would
	// hand the rest of the borrow back to the station path - so OUR TX-done
	// would then try to take the lock the borrowing task is holding, stall for
	// the full lock timeout, and make a perfectly good transmit look like a
	// TX-done timeout. The borrowing task owns this flag start to finish and
	// clears it in restore_station_radio_after_mesh_borrow().
	if (_mesh_tx_in_progress)
	{
		_tx_done_seq++;
		return;
	}

	// Station-link transmit. TX complete, enable RX continuous mode again. If
	// paired, settle onto the frequency the station is expected on:
	// set_radio_freq honors _paired_base_fallback, which tracks where the
	// station was actually heard (legacy prototypes live on the plain base
	// frequency, v11+ stations on their serial-derived channel). This also
	// moves a freshly auto-paired gateway off its sniff-slot tuning.
	if(radio_lock(RADIO_LOCK_TIMEOUT_MS))
	{
		if (get_pair_state() == PairState::PAIRED && g_manual_listen_slot < 0)
		{
			set_radio_freq(Settings::get_freq_region());
		}
		// sx126x_rx_cont() reapplies the station modem config, including
		// SetPublicNetwork(false), which undoes the Meshtastic sync word.
		sx126x_rx_cont();
		radio_unlock();
	}

	// Published last, after the restore is complete: sx126x_tx_meshtastic waits
	// on this counter and returns as soon as it changes, so bumping it earlier
	// would let the packet handler task race the restore.
	_tx_done_seq++;
}
#else
/******************************************************************************
 * @brief Meshtastic transmit stub for screenless builds (CONFIG_SCREENLESS).
 *
 * Screenless targets drive the radio through the MeshCore KISS link rather
 * than the on-board SX1262 driver, so the borrow-the-radio approach used on
 * standard WG1200 does not apply. Left unimplemented on purpose - the screenless path needs
 * KISS-level framing support before it can carry Meshtastic frames.
 ******************************************************************************/
bool sx126x_tx_meshtastic(uint8_t *buffer, uint8_t size, uint32_t freq_hz,
                          uint32_t sf, uint32_t bw_idx, uint8_t cr, uint16_t preamble,
                          const RadioLease *lease)
{
	(void)buffer; (void)size; (void)freq_hz;
	(void)sf; (void)bw_idx; (void)cr; (void)preamble; (void)lease;
	ESP_LOGW(TAG, "mesh tx: not supported on screenless builds (KISS radio path)");
	return false;
}

bool sx126x_tx_meshcore(uint8_t *buffer, uint8_t size, uint32_t freq_hz,
                        uint32_t sf, uint32_t bw_idx, uint8_t cr, uint16_t preamble,
                        const RadioLease *lease)
{
	(void)buffer; (void)size; (void)freq_hz;
	(void)sf; (void)bw_idx; (void)cr; (void)preamble; (void)lease;
	ESP_LOGW(TAG, "meshcore tx: not supported on screenless builds (KISS radio path)");
	return false;
}
#endif

/******************************************************************************
 * @brief Calculate Weather Station Status based on timeout and packet state
 * @param ws_timeout true if timeout occured while waiting for packet
 * @param ws_packet_state Received packet state
 * @param ws_reception Weather station reception
 * @retval ACTIVE_OK everything ok
 * @retval ACTIVE_BAD_DATA last packet contained bad data
 * @retval ACTIVE_BUSY last packet depicted station busy
 * @retval ACTIVE_LOW_RECEPTION reception is below the threshold after the
 * adaptive TX-power controller has reached maximum power
 * @retval INACTIVE station is inactive for 5 minutes, excluding BUSY time
 ******************************************************************************/
ActivityStatus calculate_ws_status(bool ws_timeout, PktStatus ws_packet_state, uint8_t ws_reception)
{
	if(get_cur_station() == NULL)
	{
		return ActivityStatus::INACTIVE;
	}

	static uint32_t last_seen_ts_ms = 0;

	ActivityStatus calculated_ws_status = ActivityStatus::NONE;

	if(ws_timeout || ws_packet_state == PktStatus::IGNORE || ws_packet_state == PktStatus::NONE)
	{
		uint32_t ws_lost_timeout_ms = WS_LOST_TIMEOUT_MS;
		if(get_activity_status() == ActivityStatus ::ACTIVE_BUSY)
		{
			ws_lost_timeout_ms = get_cur_station()->BUSY_DURATION_SEC * 1000 + WS_PACKET_TIMEOUT_TOLERANCE_MS;
		}

		if(Utils::get_tick_ms() - last_seen_ts_ms > ws_lost_timeout_ms)
		{
			return ActivityStatus::INACTIVE;
		}
		else
		{
			calculated_ws_status = get_activity_status();
			if(calculated_ws_status == ActivityStatus::NONE)
			{
				return ActivityStatus::NONE;
			}
			return calculated_ws_status;
		}	
	}

	last_seen_ts_ms = Utils::get_tick_ms();

	if(ws_packet_state == PktStatus::BAD_DATA)
	{
		if(_bad_data_streak < 0xFF)
		{
			_bad_data_streak++;
		}

		if(_bad_data_streak >= WS_BAD_DATA_STREAK_THRESHOLD)
		{
			calculated_ws_status = ActivityStatus::ACTIVE_BAD_DATA;
		}
		else
		{
			// Isolated bad sample (typically the first SENSORS packet after a
			// station reboot, before the sensor module has produced readings):
			// the link itself is healthy, so don't flag the station yet
			calculated_ws_status = (ws_reception < WS_LOW_RECEPTION_THRESHOLD &&
				is_station_power_control_exhausted())
				? ActivityStatus::ACTIVE_LOW_RECEPTION
				: ActivityStatus::ACTIVE_OK;
		}
	}
	else if(ws_packet_state == PktStatus::OK)
	{
		_bad_data_streak = 0;
		if(ws_reception < WS_LOW_RECEPTION_THRESHOLD &&
			is_station_power_control_exhausted())
		{
			calculated_ws_status = ActivityStatus::ACTIVE_LOW_RECEPTION;
		}
		else
		{
			calculated_ws_status = ActivityStatus::ACTIVE_OK;
		}
	}
	else if(ws_packet_state == PktStatus::OK_BUSY)
	{
		_bad_data_streak = 0;
		calculated_ws_status = ActivityStatus::ACTIVE_BUSY;
	}

	if(calculated_ws_status == ActivityStatus::NONE)
	{
		return ActivityStatus::INACTIVE;
	}

	return calculated_ws_status;
}

/******************************************************************************
 * @brief Get latest calculated reception percentage
 * @return Reception percentage
 ******************************************************************************/
uint8_t get_packet_reception()
{
	if(get_cur_station() == NULL)
	{
		return 0;
	}
	return _state.packet_reception;
}

static void get_station_power_limits(uint16_t *min_power, uint16_t *max_power)
{
	if(get_cur_station() != NULL && get_cur_station()->TYPE == WsType::WS_TYPE_1300)
	{
		*min_power = Ws1300Helpers::POWER_LEVEL_MIN;
		*max_power = Ws1300Helpers::POWER_LEVEL_MAX;
	}
	else
	{
		*min_power = Ws1001Helpers::POWER_LEVEL_MIN;
		*max_power = Ws1001Helpers::POWER_LEVEL_MAX;
	}
}

StationPowerSetting get_station_power_setting()
{
	uint16_t effort = _state.requested_tx_power != 0
		? _state.requested_tx_power : _state.station_tx_power;
	if(effort == 0 || get_cur_station() == NULL)
	{
		return StationPowerSetting::UNKNOWN;
	}

	uint16_t min_power = 0;
	uint16_t max_power = 0;
	get_station_power_limits(&min_power, &max_power);
	effort = effort < min_power ? min_power : (effort > max_power ? max_power : effort);
	uint32_t span = max_power - min_power + 1;
	uint32_t position = effort - min_power;
	if(position * 3 < span) return StationPowerSetting::LOW;
	if(position * 3 < span * 2) return StationPowerSetting::MID;
	return StationPowerSetting::HIGH;
}

const char *get_station_power_setting_title()
{
	switch(get_station_power_setting())
	{
		case StationPowerSetting::LOW: return "Low";
		case StationPowerSetting::MID: return "Mid";
		case StationPowerSetting::HIGH: return "High";
		default: return "--";
	}
}

bool is_station_power_control_exhausted()
{
	if(get_cur_station() == NULL || _state.requested_tx_power == 0)
	{
		return false;
	}
	uint16_t min_power = 0;
	uint16_t max_power = 0;
	get_station_power_limits(&min_power, &max_power);
	return _state.requested_tx_power >= max_power;
}

bool get_station_signal(int16_t *rssi, int8_t *snr)
{
	if(rssi == NULL || snr == NULL || !_state.station_signal_available)
	{
		return false;
	}
	*rssi = _state.station_rssi;
	*snr = _state.station_snr;
	return true;
}

/******************************************************************************
 * @brief Calculate reception percentage
 * @param ws_timeout true if timeout occurred
 * @param packet_status last packet status
 * @param counter_sample validated packet counter from the paired station
 * @return Reception percentage
 ******************************************************************************/
uint8_t calculate_packet_reception(bool ws_timeout, PktStatus packet_status,
	const PacketCounterSample* counter_sample)
{
	(void)counter_sample;

	if(get_cur_station() == NULL)
	{
		return 0;
	}

	if(packet_status == PktStatus::IGNORE)
	{
		return _state.packet_reception;
	}

	if(ws_timeout)
	{
		_state.lost_timeslot_streak++;
	}
	else
	{
		_state.lost_timeslot_streak = 0;
		uint64_t packet_ts = Utils::get_tick_sec();
		_ws_valid_packets_ts_buff_ms.lock();
		_ws_valid_packets_ts_buff_ms.push(&packet_ts);
		_ws_valid_packets_ts_buff_ms.unlock();
	}	

	_ws_valid_packets_ts_buff_ms.lock();
	uint8_t ok_packets_ts_count = _ws_valid_packets_ts_buff_ms.get_item_count();
	if(ok_packets_ts_count == 0)
	{
		_ws_valid_packets_ts_buff_ms.unlock();
		return 0;
	}

	uint64_t oldest;
	uint64_t newest;
	_ws_valid_packets_ts_buff_ms.peek_oldest(&oldest);
	_ws_valid_packets_ts_buff_ms.peek_newest(&newest);

	uint32_t timeslot_count = (newest - oldest)/(get_cur_station()->BROADCAST_INTERVAL_SEC) + 1 + _state.lost_timeslot_streak;
	float packet_loss_perc_float = ((float)ok_packets_ts_count/(float)timeslot_count) * 100.0f;
	_ws_valid_packets_ts_buff_ms.unlock();

	if(packet_loss_perc_float > 100.0f)
	{
		packet_loss_perc_float = 100.0f;
	}
	if(packet_loss_perc_float < 0.0f)
	{
		packet_loss_perc_float = 0.0f;
	}

	return (uint8_t) packet_loss_perc_float;
}

/******************************************************************************
 * @brief Unpair currently paired weather station
 ******************************************************************************/
void unpair(bool wipe_nvs)
{
	lock_state();
	set_pair_state(PairState::UNPAIRED, nullptr, wipe_nvs);
	set_activity_status(ActivityStatus::INACTIVE);
	_last_valid_packet_rx_ms = 0;
	g_sniff_mode_enabled = true; // Automatically turn sniff mode back ON when unpairing
	g_auto_pair_enabled = true;  // Restore auto-pairing functionality
	unlock_state();

	MeshTelemetry::reset_baseline();

	// Reached from the web server task AND from state_sniffer - the latter with
	// no human involved, which makes it the likeliest source of a retune landing
	// inside a Meshtastic transmit. The unpair itself has already been applied
	// above; only the radio reprogramming is gated.
	if(radio_lock(RADIO_LOCK_TIMEOUT_MS))
	{
		retune_for_current_state();
		sx126x_rx_cont();
		radio_unlock();
	}
	Log::log(Log::Code::SNIFFER_WS_UNPAIRED, _state.pair_data.id);
}

/******************************************************************************
 * @brief Atomically switch the pairing mode: unpairs/sets runtime flags/persists
 * in one place so the mode can never end up half-applied.
 * @retval RET_INVALID_STATE when LOCAL is requested while unpaired (LOCAL is an
 * outcome of a successful SCAN, not a directly commandable state)
 ******************************************************************************/
RetResult apply_pair_mode(PairMode mode, uint32_t scan_target_id,
	int8_t requested_target_slot, WsType scan_profile)
{
	if(mode == PairMode::PAIR_MODE_LOCAL && !Settings::local_ws_profile_ready())
	{
		return RET_INVALID_STATE;
	}

	// Resolve a saved target's channel before SCAN unpairs and clears LOCAL.
	// Cloud and Local profiles contain the full WS1300 serial, so targeted
	// repair can listen directly on serial[last] % 8 instead of sweeping.
	int8_t resolved_target_slot =
		(requested_target_slot >= 0 && requested_target_slot <= 8)
			? requested_target_slot : -1;
	WsType target_type = WsType::WS_TYPE_NONE;
	if(mode == PairMode::PAIR_MODE_SCAN && scan_target_id != 0)
	{
		uint8_t target_sn[WS_SN_LEN] = {};
		bool target_sn_found = false;

		if(Settings::cloud_ws_profile_ready() &&
		   (uint32_t)Settings::get_cloud_ws_id() == scan_target_id)
		{
			target_sn_found =
				Settings::get_cloud_ws_sn(target_sn, sizeof(target_sn)) == RET_OK;
			target_type = Settings::get_cloud_ws_type();
		}
		else if(Settings::local_ws_profile_ready() &&
				(uint32_t)Settings::get_ws_id() == scan_target_id)
		{
			target_sn_found =
				Settings::get_ws_sn(target_sn, sizeof(target_sn)) == RET_OK;
			target_type = Settings::get_ws_type();
		}

		if(target_type == WsType::WS_TYPE_NONE)
		{
			RecentStation recent[RECENT_STATIONS_CAPACITY] = {};
			size_t recent_count = get_recent_stations(recent, RECENT_STATIONS_CAPACITY);
			for(size_t i = 0; i < recent_count; i++)
			{
				if(recent[i].id == scan_target_id)
				{
					target_type = recent[i].type;
					if(resolved_target_slot < 0 && recent[i].channel_slot <= 8)
					{
						resolved_target_slot = (int8_t)recent[i].channel_slot;
					}
					break;
				}
			}
		}

		if(target_type == WsType::WS_TYPE_1001)
		{
			// WS1001 stations do NOT use channel-as-serial calculation or CH0..CH7.
			// They always operate on Base channel (slot 0).
			resolved_target_slot = 0;
		}
		else if(target_sn_found && target_type == WsType::WS_TYPE_1300)
		{
			resolved_target_slot =
				(int8_t)((target_sn[WS_SN_LEN - 1] % 8) + 1);
		}
	}

	// Select the target profile before any unpair/re-init work. In particular,
	// SCAN must clear/replace only the local profile while preserving the cached
	// cloud assignment.
	Settings::set_pair_mode(mode);
	g_scan_target_id = mode == PairMode::PAIR_MODE_SCAN ? scan_target_id : 0;
	g_scan_target_slot =
		mode == PairMode::PAIR_MODE_SCAN ? resolved_target_slot : -1;
	_is_fallback_latch = false;

	switch(mode)
	{
		case PairMode::PAIR_MODE_OFF:
			// True monitor mode: the pairing is preserved. OFF used to call unpair(),
			// which wiped the stored station credentials from NVS - so "turning the
			// uplink off" silently destroyed the pairing and forced a full re-SCAN to
			// get back. Nothing is uplinked in this mode because submit_ws_data() and
			// submit_gps_data() check for PAIR_MODE_OFF, not because we are unpaired.
			lock_state();
			// Stay locked to the paired station's channel and decode via the
			// receive-only sniff path. This must remain true even with a saved
			// pairing; otherwise the normal paired handler still sends ACKs.
			g_sniff_mode_enabled = true;
			g_auto_pair_enabled = false;
			unlock_state();
			break;

		case PairMode::PAIR_MODE_CLOUD:
		{
			if(!Settings::cloud_ws_profile_ready() &&
			   Settings::get_cloud_ws_valid() &&
			   Settings::get_cloud_ws_id() > 0 &&
			   Settings::get_cloud_ws_type() == WsType::WS_TYPE_1300)
			{
				uint32_t cloud_id = (uint32_t)Settings::get_cloud_ws_id();
				if(Settings::local_ws_profile_ready() &&
				   (uint32_t)Settings::get_ws_id() == cloud_id)
				{
					uint8_t local_sn[WS_SN_LEN] = {0};
					uint8_t local_pubkey[WS_PUB_KEY_LEN] = {0};
					if(Settings::get_ws_sn(local_sn, sizeof(local_sn)) == RET_OK)
					{
						Settings::set_cloud_ws_sn(local_sn, sizeof(local_sn));
					}
					if(Settings::get_ws_pubkey(local_pubkey, sizeof(local_pubkey)) == RET_OK)
					{
						Settings::set_cloud_ws_pubkey(local_pubkey, sizeof(local_pubkey));
					}
				}
				else
				{
					RecentStation recent[RECENT_STATIONS_CAPACITY] = {};
					size_t recent_count = get_recent_stations(recent, RECENT_STATIONS_CAPACITY);
					for(size_t i = 0; i < recent_count; i++)
					{
						if(recent[i].id == cloud_id)
						{
							uint8_t ws_sn[WS_SN_LEN] = {0};
							if(recent[i].has_sn)
							{
								memcpy(ws_sn, recent[i].sn, sizeof(ws_sn));
							}
							else if(recent[i].channel_slot <= 8)
							{
								ws_sn[0] = (uint8_t)((cloud_id >> 24) & 0xFF);
								ws_sn[1] = (uint8_t)((cloud_id >> 16) & 0xFF);
								ws_sn[2] = (uint8_t)((cloud_id >> 8) & 0xFF);
								ws_sn[3] = (uint8_t)(cloud_id & 0xFF);
								ws_sn[4] = 0xAA;
								ws_sn[5] = 0x55;
								ws_sn[6] = 0xAA;
								ws_sn[7] = 0x55;
								ws_sn[8] = 0x00;
								ws_sn[9] = (recent[i].channel_slot > 0)
									? (uint8_t)(recent[i].channel_slot - 1) : 0;
							}
							if(recent[i].has_sn || recent[i].channel_slot <= 8)
							{
								Settings::set_cloud_ws_sn(ws_sn, sizeof(ws_sn));
							}
							if(recent[i].has_pubkey)
							{
								Settings::set_cloud_ws_pubkey(recent[i].pubkey, sizeof(recent[i].pubkey));
							}
							break;
						}
					}
				}
			}

			WsType station_type = WsType::WS_TYPE_NONE;
			WsBase::PairData pair_data = {};
			if(load_station_profile(PairSource::PAIR_SOURCE_CLOUD, &station_type, &pair_data))
			{
				lock_state();
				_active_pair_source = PairSource::PAIR_SOURCE_CLOUD;
				set_cur_station_obj(station_type);
				g_receiver_profile = station_type;
				unlock_state();
				set_pair_state(PairState::PAIRED, &pair_data);
				if (station_type == WsType::WS_TYPE_1300)
				{
					if (Settings::is_station_ownership_confirmed() &&
					    Settings::get_station_ownership_id() == pair_data.id)
					{
						g_ownership_confirmed = true;
						g_ownership_challenge_armed = false;
						g_ownership_challenge_pending = false;
					}
					else
					{
						g_ownership_confirmed = false;
						g_ownership_challenge_armed = true;
						g_ownership_challenge_pending = false;
						g_ownership_challenge_station_id = pair_data.id;
						g_ownership_challenge_started_sec = Utils::get_tick_sec();
						g_ownership_challenge_packets_seen = 0;
						_state.gps_last_req_ts_sec = 0;
						_state.gps_req_attempts = 0;
					}
				}
			}
			lock_state();
			// Auto-pair fallback is only active if neither cloud nor local profile exists
			g_auto_pair_enabled = !Settings::cloud_ws_profile_ready() && !Settings::local_ws_profile_ready();
			g_sniff_mode_enabled = (get_pair_state() != PairState::PAIRED);
			g_manual_listen_slot = -1;
			unlock_state();
			break;
		}

		case PairMode::PAIR_MODE_LOCAL:
		{
			WsType station_type = WsType::WS_TYPE_NONE;
			WsBase::PairData pair_data = {};
			if(load_station_profile(PairSource::PAIR_SOURCE_LOCAL, &station_type, &pair_data))
			{
				lock_state();
				_active_pair_source = PairSource::PAIR_SOURCE_LOCAL;
				set_cur_station_obj(station_type);
				g_receiver_profile = station_type;
				unlock_state();
				set_pair_state(PairState::PAIRED, &pair_data);
				if (station_type == WsType::WS_TYPE_1300)
				{
					if (Settings::is_station_ownership_confirmed() &&
					    Settings::get_station_ownership_id() == pair_data.id)
					{
						g_ownership_confirmed = true;
						g_ownership_challenge_armed = false;
						g_ownership_challenge_pending = false;
					}
					else
					{
						g_ownership_confirmed = false;
						g_ownership_challenge_armed = true;
						g_ownership_challenge_pending = false;
						g_ownership_challenge_station_id = pair_data.id;
						g_ownership_challenge_started_sec = Utils::get_tick_sec();
						g_ownership_challenge_packets_seen = 0;
						_state.gps_last_req_ts_sec = 0;
						_state.gps_req_attempts = 0;
					}
				}
			}
			lock_state();
			g_auto_pair_enabled = false;
			g_sniff_mode_enabled = false;
			g_manual_listen_slot = -1;
			unlock_state();
			break;
		}

		case PairMode::PAIR_MODE_SCAN:
		{
			// Close the small window before the settings-changed event is
			// consumed: a newly heard ADV must be saved as LOCAL, never CLOUD.
			lock_state();
			_active_pair_source = PairSource::PAIR_SOURCE_LOCAL;
			unlock_state();
			unpair(false);
			lock_state();
			g_sniff_mode_enabled = true;
			g_auto_pair_enabled = (g_scan_target_id != 0);
			// SCAN uses g_scan_target_slot for a targeted lock; the separate
			// manual diagnostics override must not remain active.
			g_manual_listen_slot = -1;
			if(g_scan_target_id != 0)
			{
				if (target_type != WsType::WS_TYPE_NONE)
				{
					g_scan_profile = target_type;
					_scan_phase = (target_type == WsType::WS_TYPE_1300) ? ScanPhase::WS1300 : ScanPhase::WS1001;
					set_cur_station_obj(target_type == WsType::WS_TYPE_1300 ? WsType::WS_TYPE_1300 : WsType::WS_TYPE_1001);
					g_receiver_profile = (target_type == WsType::WS_TYPE_1300) ? WsType::WS_TYPE_1300 : WsType::WS_TYPE_1001;
				}
				if(g_scan_target_slot >= 0)
				{
					if(g_scan_target_slot == 0)
					{
						ESP_LOGI(TAG,
							"Targeted SCAN for 0x%08X locked directly to base",
							(unsigned int)g_scan_target_id);
					}
					else
					{
						ESP_LOGI(TAG,
							"Targeted SCAN for 0x%08X locked directly to WS1300 CH%d",
							(unsigned int)g_scan_target_id,
							(int)g_scan_target_slot - 1);
					}
				}
				else
				{
					ESP_LOGI(TAG,
						"Targeted SCAN for 0x%08X has no saved serial; sweeping",
						(unsigned int)g_scan_target_id);
				}
			}
			else
			{
				// Keep detected stations buffer persistent in RAM across sweeps and failed pairing attempts.
				// Stations remain available for selection until device reboot or explicit manual clear.
				_scan_cycle_count = 0;
				_scan_completed = false;

				if (scan_profile == WsType::WS_TYPE_NONE)
				{
					if (g_scan_profile != WsType::WS_TYPE_NONE)
					{
						scan_profile = g_scan_profile;
					}
					else if (g_receiver_profile != WsType::WS_TYPE_NONE)
					{
						scan_profile = g_receiver_profile;
					}
					else
					{
						WsType conf_type = Settings::get_ws_type();
						scan_profile = (conf_type == WsType::WS_TYPE_1300) ? WsType::WS_TYPE_1300
									 : ((conf_type == WsType::WS_TYPE_ALL) ? WsType::WS_TYPE_ALL
									 : WsType::WS_TYPE_1001);
					}
				}
				g_scan_profile = scan_profile;

				if (g_scan_profile == WsType::WS_TYPE_1300)
				{
					_scan_phase = ScanPhase::WS1300;
					set_cur_station_obj(WsType::WS_TYPE_1300);
					g_receiver_profile = WsType::WS_TYPE_1300;
					g_sniff_slot = (g_scan_target_slot >= 0) ? (uint8_t)g_scan_target_slot : 1;
					g_sniff_channel = 0;
					_sniff_slot_tuned_ts_ms = Utils::get_tick_ms();
					_sniff_slot_hold_until_ms = 0;
					tune_sniff_slot();
					sx126x_rx_cont();
					if (g_scan_target_slot >= 0)
					{
						ESP_LOGI(TAG, "Starting dedicated WS1300 scan locked directly to slot %u", (unsigned int)g_sniff_slot);
					}
					else
					{
						ESP_LOGI(TAG, "Starting dedicated WS1300-only channel sweep (starting on slot %u / CH0)", (unsigned int)g_sniff_slot);
					}
				}
				else if (g_scan_profile == WsType::WS_TYPE_1001)
				{
					_scan_phase = ScanPhase::WS1001;
					_ws1001_scan_start_ms = Utils::get_tick_ms();
					set_cur_station_obj(WsType::WS_TYPE_1001);
					g_receiver_profile = WsType::WS_TYPE_1001;
					g_sniff_slot = 0;
					g_sniff_channel = 0;
					_sniff_slot_tuned_ts_ms = Utils::get_tick_ms();
					_sniff_slot_hold_until_ms = 0;
					tune_sniff_slot();
					sx126x_rx_cont();
					ESP_LOGI(TAG, "Starting dedicated WS1001-only scan locked to base frequency");
				}
				else
				{
					_scan_phase = ScanPhase::WS1001;
					_ws1001_scan_start_ms = Utils::get_tick_ms();
					set_cur_station_obj(WsType::WS_TYPE_1001);
					g_receiver_profile = WsType::WS_TYPE_1001;
					g_sniff_slot = 0;
					g_sniff_channel = 0;
					_sniff_slot_tuned_ts_ms = Utils::get_tick_ms();
					_sniff_slot_hold_until_ms = 0;
					tune_sniff_slot();
					sx126x_rx_cont();
					ESP_LOGI(TAG, "Starting combined WS1001/WS1300 dual-profile scan");
				}
			}
			unlock_state();
			break;
		}

		default:
			return RET_INVALID_PARAMS;
	}

	if(Settings::commit_blocking() != RET_OK)
	{
		return RET_ERROR;
	}

	notify_sniff_mode_toggled(); // wake handler task, retune, restart RX

	ESP_LOGI(TAG, "Pair mode applied: %d (0=OFF 1=CLOUD 2=LOCAL 3=SCAN)", (int)mode);

	return RET_OK;
}

/******************************************************************************
 * @brief Get last packet from reception timestamp buffer
 * @param ts Timestamp to return
 ******************************************************************************/
RetResult get_last_verified_ts(uint64_t *ts)
{
	_ws_valid_packets_ts_buff_ms.lock();
	if(_ws_valid_packets_ts_buff_ms.peek_newest(ts))
	{
		_ws_valid_packets_ts_buff_ms.unlock();
		return RET_OK;
	}
	_ws_valid_packets_ts_buff_ms.unlock();
	return RET_ERROR;
}

/******************************************************************************
 * @brief Checks if current time is within station LoRa packet arrival or post-packet
 * mesh cooldown guard windows, to prevent concurrent WiFi/MQTT activity from dropping
 * LoRa packets over the I2C IO expander.
 * @param defer_ms Optional pointer to store recommended delay (ms) into safe window
 * @return true if currently inside guard window, false if safe to transmit
 ******************************************************************************/
bool is_in_station_packet_guard_window(uint32_t *defer_ms)
{
	lock_state();
	if (get_cur_station() == nullptr || get_pair_state() != PairState::PAIRED || _last_valid_packet_rx_ms == 0)
	{
		unlock_state();
		if (defer_ms) *defer_ms = 0;
		return false;
	}

	uint32_t interval_sec = get_cur_station()->BROADCAST_INTERVAL_SEC;
	if (get_cur_station()->TYPE == WsType::WS_TYPE_1300)
	{
		interval_sec = _paired_station_interval_sec;
	}
	if (interval_sec == 0) interval_sec = 20;
	uint32_t interval_ms = interval_sec * 1000;

	uint32_t now_ms = Utils::get_tick_ms();
	uint32_t elapsed_ms = now_ms - _last_valid_packet_rx_ms;
	uint32_t cycle_offset_ms = elapsed_ms % interval_ms;
	uint32_t rem_to_next_packet_ms = interval_ms - cycle_offset_ms;

	// Post-packet window: 0 to 6.0s (includes ACK + gateway mesh delay)
	// Pre-packet window: within 7.0s of next expected station packet
	// Safe quiet window: 6.5s to (interval - 7.0s) = 6.5s to 13.0s (for 20s cycle)

	if (cycle_offset_ms < POST_PACKET_COOLDOWN_MS)
	{
		if (defer_ms)
		{
			*defer_ms = SAFE_SUBMIT_OFFSET_MS - cycle_offset_ms;
		}
		unlock_state();
		return true;
	}

	if (rem_to_next_packet_ms < PRE_PACKET_GUARD_MS)
	{
		if (defer_ms)
		{
			*defer_ms = rem_to_next_packet_ms + SAFE_SUBMIT_OFFSET_MS;
		}
		unlock_state();
		return true;
	}

	unlock_state();
	if (defer_ms) *defer_ms = 0;
	return false;
}

/******************************************************************************
 * @brief Get last packet status
 * @return PktStatus
 ******************************************************************************/
PktStatus get_last_packet_status()
{
	return _state.last_packet_status;
}

void set_last_packet_status(PktStatus status)
{
	_state.last_packet_status = status;
}

/******************************************************************************
 * @brief Get station status
 * @return ActivityStatus
 ******************************************************************************/
ActivityStatus get_activity_status()
{
	return _state.activity_status;
}

/******************************************************************************
 * @brief Get station status title str
 * @return pointer to str
 ******************************************************************************/
const char * get_activity_status_title(ActivityStatus status)
{
	for(uint8_t i = 0; i < (sizeof(ACTIVITY_STATUS_TITLES)/sizeof(ActivityStatusDescriptor)); i++)
	{
		if(ACTIVITY_STATUS_TITLES[i].status == status)
		{
			return ACTIVITY_STATUS_TITLES[i].title;
		}
	}
	// return NONE if status not found
	return ACTIVITY_STATUS_TITLES[0].title;
}

/******************************************************************************
 * @brief Set current weather station status
 * @param status Station status
 * @return RetResult
 ******************************************************************************/
RetResult set_activity_status(ActivityStatus status)
{
	if(status == _state.activity_status)
	{
		return RET_OK;
	}
	else if(status == ActivityStatus::NONE)
	{
		return RET_INVALID_PARAMS;
	}

	GlobalEvents::WsStatusChangedEvent app_event = GlobalEvents::WsStatusChangedEvent::NONE;
	_state.activity_status = status;
	app_event = _state.activity_status;
	_state.activity_status_changed_ts_sec = Utils::get_tick_sec();
	
	if(_state.activity_status == ActivityStatus::INACTIVE)
	{
		ESP_LOGE(TAG, "Weather station inactive. Last packet state enum: %d", (uint16_t)get_last_packet_status());
		Log::log(Log::Code::SNIFFER_WS_INACTIVE, _state.pair_data.id);
	}
	else if(_state.activity_status == ActivityStatus::ACTIVE_BAD_DATA)
	{
		Log::log(Log::Code::SNIFFER_WS_ACTIVE_BAD_DATA, _state.pair_data.id);
	}
	else if(_state.activity_status == ActivityStatus::ACTIVE_BUSY)
	{
		Log::log(Log::Code::SNIFFER_WS_ACTIVE_GPS_FIXING, _state.pair_data.id);
	}
	else if(_state.activity_status == ActivityStatus::ACTIVE_LOW_RECEPTION)
	{
		Log::log(Log::Code::SNIFFER_WS_ACTIVE_LOW_RECEPTION, _state.pair_data.id);
	}
	else if(_state.activity_status == ActivityStatus::ACTIVE_OK)
	{
		Log::log(Log::Code::SNIFFER_WS_ACTIVE_OK, _state.pair_data.id);
	}
	GlobalEvents::publish(GlobalEvents::EventId::WS_STATUS_CHANGED, &app_event, sizeof(app_event));
	return RET_OK;
}

/*******************************************************************************
 * @brief Get how many seconds have passed since last status change.
 * @return
 *   uint32_t Seconds since last status change.
 ******************************************************************************/
uint32_t get_activity_status_duration_sec()
{
	if(_state.activity_status_changed_ts_sec == 0)
	{
		return 0;
	}

	return Utils::get_tick_sec() - _state.activity_status_changed_ts_sec;
}

/******************************************************************************
 * @brief Get current pair state
 * @return PairState
 ******************************************************************************/
PairState get_pair_state()
{
	return _state.pair_state;
}

PairSource get_active_pair_source()
{
	return _active_pair_source;
}

/******************************************************************************
 * @brief Get station address currently listening to
 * @return Station address
 ******************************************************************************/
u32 get_paired_id()
{
	return _state.pair_data.id;
}

/******************************************************************************
 * @brief Get station sn currently listening to
 * @param buff Buffer to copy the sn
 * @param buff_len Length of buffer to copy the sn
 * @return RET_OK on success
 ******************************************************************************/
RetResult get_paired_sn(uint8_t* buff, size_t buff_len)
{
	if(buff == NULL || buff_len < sizeof(_state.pair_data.sn))
	{
		return RetResult::RET_INVALID_PARAMS;
	}

	lock_state();
	memcpy(buff, _state.pair_data.sn, buff_len);
	unlock_state();

	return RetResult::RET_OK;
}

/******************************************************************************
 * @brief Set current pair state
 * @param state Current pair state
 * @param pair_data Pair data
 * @return RetResult
 ******************************************************************************/
RetResult set_pair_state(PairState state, WsBase::PairData* pair_data, bool wipe_nvs)
{
	if(_state.pair_state == state)
	{
		if (state == PairState::PAIRED && pair_data != nullptr)
		{
			// Check if we need to update/re-save changed credentials (like public key after a station reset)
			if (memcmp(_state.pair_data.pubkey, pair_data->pubkey, WS_PUB_KEY_LEN) != 0 ||
				memcmp(_state.pair_data.sn, pair_data->sn, WS_SN_LEN) != 0 ||
				_state.pair_data.id != pair_data->id)
			{
				ESP_LOGW(TAG, "Re-saving pairing info with updated credentials (station ID, SN, or public key changed).");
				if(_state.pair_data.id != pair_data->id)
				{
					// Different physical station: buffered weather belongs to the old one
					WsData::clear_recv();
					WsData::clear_aggregates();
				}
				if(_active_pair_source == PairSource::PAIR_SOURCE_CLOUD)
				{
					Settings::set_cloud_ws_id(pair_data->id);
					Settings::set_cloud_ws_pubkey(pair_data->pubkey, WS_PUB_KEY_LEN);
					Settings::set_cloud_ws_sn(pair_data->sn, WS_SN_LEN);
					Settings::set_cloud_ws_type(get_cur_station()->TYPE);
					Settings::set_cloud_ws_valid(true);
				}
				else
				{
					Settings::set_ws_id(pair_data->id);
					Settings::set_ws_pubkey(pair_data->pubkey, WS_PUB_KEY_LEN);
					Settings::set_ws_sn(pair_data->sn, WS_SN_LEN);
				}
				_state.pair_data.id = pair_data->id;
				memcpy(_state.pair_data.sn, pair_data->sn, sizeof(_state.pair_data.sn));
				memcpy(_state.pair_data.pubkey, pair_data->pubkey, sizeof(_state.pair_data.pubkey));
				_paired_base_fallback = false;
				_paired_channel_locked = false;
				_paired_freq_confirmed = false;
				_paired_confirmed_on_base = false;
				_paired_station_interval_sec = 20;
				_paired_miss_streak = 0;
				Settings::commit();
			}
		}
		return RET_OK;
	}
	if(_state.pair_state == PairState::NONE)
	{
		return RET_INVALID_STATE;
	}

	if(state == PairState::PAIRED)
	{
		if(pair_data == nullptr) return RET_INVALID_PARAMS;

		_state.pair_state = PairState::PAIRED;
		g_manual_listen_slot = -1;
		if(_active_pair_source == PairSource::PAIR_SOURCE_CLOUD)
		{
			Settings::set_cloud_ws_id(pair_data->id);
			Settings::set_cloud_ws_pubkey(pair_data->pubkey, WS_PUB_KEY_LEN);
			Settings::set_cloud_ws_sn(pair_data->sn, WS_SN_LEN);
			Settings::set_cloud_ws_type(get_cur_station()->TYPE);
			Settings::set_cloud_ws_valid(true);
		}
		else
		{
			Settings::set_ws_id(pair_data->id);
			Settings::set_ws_pubkey(pair_data->pubkey, WS_PUB_KEY_LEN);
			Settings::set_ws_sn(pair_data->sn, WS_SN_LEN);
		}
		ESP_LOGI(TAG, "Pairing with ID:0x%08X", pair_data->id);
		_state.pair_data.id = pair_data->id;
		memcpy(_state.pair_data.sn, pair_data->sn, sizeof(_state.pair_data.sn));
		memcpy(_state.pair_data.pubkey, pair_data->pubkey, sizeof(_state.pair_data.pubkey));
		
		_paired_base_fallback = false;
		_paired_channel_locked = false;
		_paired_freq_confirmed = false;
		_paired_confirmed_on_base = false;
		_paired_station_interval_sec = 20;
		_paired_miss_streak = 0;
	}
	else if(state == PairState::UNPAIRED)
	{
		_state.pair_state = PairState::UNPAIRED;
		// Unpaired gateways never uplink weather - drop anything still buffered
		WsData::clear_recv();
		WsData::clear_aggregates();
		// An unpublished local pairing no longer exists - nothing to sync
		Settings::set_local_pair_sync_pending(false);
		if(wipe_nvs)
		{
			// Unpair replaces the local profile in NVS only if wipe_nvs is true.
			// Non-destructive scan leaves the stored local profile intact.
			Settings::set_ws_id(-1);
			u8 clear_pub_key[WS_PUB_KEY_LEN];
			u8 clear_sn[WS_SN_LEN];
			memset(clear_pub_key, 0xFF, WS_PUB_KEY_LEN);
			memset(clear_sn, 0xFF, WS_SN_LEN);
			Settings::set_ws_pubkey(clear_pub_key, WS_PUB_KEY_LEN);
			Settings::set_ws_sn(clear_sn, WS_SN_LEN);
			Settings::commit();
		}
		ESP_LOGI(TAG, "Unpairing from ID:0x%08X (wipe_nvs: %s)", _state.pair_data.id, wipe_nvs ? "true" : "false");
		_state.pair_data.id = 0x00;
		memset(_state.pair_data.sn, 0xFF, sizeof(_state.pair_data.sn));
	}
	else
	{
		return RET_INVALID_PARAMS;
	}

	if(state == PairState::PAIRED)
	{
		Settings::commit();
	}
	
	return RET_OK;
}

/******************************************************************************
 * @brief Get frequency descriptor by region, title or symbol
 ******************************************************************************/
const WsFreqDesc *get_freq_desc(char *title_or_symbol, WsFreqRegion freq_region)
{
	// Only name or freq id must be set
	if(!((title_or_symbol == nullptr) ^ (freq_region == WS_FREQ_REGION_NONE)))
	{
		return nullptr;
	}

	const int LAST_INDEX = sizeof(SNIFFER_FREQUENCIES) / sizeof(SNIFFER_FREQUENCIES[0]);
	int found_index = -1;

	if(title_or_symbol != nullptr)
	{
		for (size_t i = 0; i < LAST_INDEX; i++)
		{
			if(strcmp(title_or_symbol, SNIFFER_FREQUENCIES[i].symbol) == 0 || strcmp(title_or_symbol, SNIFFER_FREQUENCIES[i].title) == 0)
			{
				found_index = i;
				break;
			}
		}
	}
	else
	{
		for (size_t i = 0; i < LAST_INDEX; i++)
		{
			if(SNIFFER_FREQUENCIES[i].freq_region == freq_region)
			{
				found_index = i;
				break;
			}
		}
	}

	return found_index >= 0 ? &SNIFFER_FREQUENCIES[found_index] : nullptr;
}

/******************************************************************************
 * @brief Set radio frequency
 * @param freq_region Frequency
 ******************************************************************************/
RetResult set_radio_freq(WsFreqRegion freq_region)
{
	if(Settings::get_gateway_radio_mode() == Settings::GatewayRadioMode::MESHCORE_GATEWAY)
	{
		if(!radio_lock(RADIO_LOCK_TIMEOUT_MS))
		{
			return RET_ERROR;
		}
		uint32_t resolved_freq = MeshTelemetry::get_meshcore_freq_hz();
		_current_freq_hz = resolved_freq;
		snprintf(_listen_desc, sizeof(_listen_desc), "MeshCore GW Continuous");
		ESP_LOGI(TAG, "Tuning radio to %u Hz [%s]", (unsigned int)resolved_freq, _listen_desc);
#ifndef CONFIG_SCREENLESS
		Radio.SetChannel(resolved_freq);
#endif
		radio_unlock();
		return RET_OK;
	}

	const WsFreqDesc *desc = get_freq_desc(nullptr, freq_region);
	if(desc == nullptr)
	{
		ESP_LOGE(TAG, "Invalid freq");
		return RET_ERROR;
	}

	auto *station = get_cur_station();
	if(station == nullptr)
	{
		ESP_LOGE(TAG, "Invalid station, can't set freq.");
		return RET_ERROR;
	}

	// Everything below mutates the shared tuning state and then reprograms the
	// chip. Taking ownership here means a Meshtastic borrow in flight blocks us
	// instead of us yanking its carrier off frequency mid-frame.
	if(!radio_lock(RADIO_LOCK_TIMEOUT_MS))
	{
		return RET_ERROR;
	}

	u32 resolved_freq = desc->freq_hz; //failsafe initialization
	get_cur_station()->freq_resolver(desc->freq_hz, &_state.pair_data, &resolved_freq);

	// Track what we are listening to, for console/web display
	const char *paired_suffix = (get_pair_state() == PairState::PAIRED) ? " (paired)" : "";
	if(station->TYPE == WsType::WS_TYPE_1300)
	{
		if(_paired_base_fallback && get_pair_state() == PairState::PAIRED)
		{
			// Legacy prototype WS1300: transmits on the plain region base
			// frequency, no per-serial channel offset
			resolved_freq = desc->freq_hz;
			g_sniff_slot = 0;
			g_sniff_channel = 0;
			snprintf(_listen_desc, sizeof(_listen_desc), "WS1300 base (compat)");
		}
		else
		{
			uint8_t ch = _state.pair_data.sn[sizeof(_state.pair_data.sn) - 1] % 8;
			g_sniff_channel = ch;
			g_sniff_slot = ch + 1;
			snprintf(_listen_desc, sizeof(_listen_desc), "WS1300 CH%d%s", ch, paired_suffix);
		}
	}
	else
	{
		g_sniff_slot = 0;
		g_sniff_channel = 0;
		snprintf(_listen_desc, sizeof(_listen_desc), "WS1001%s", paired_suffix);
	}

	ESP_LOGI(TAG, "Tuning radio to %u Hz [%s] (Region base: %u Hz)",
		(unsigned int)resolved_freq, _listen_desc, (unsigned int)desc->freq_hz);

	_current_freq_hz = resolved_freq;
#ifndef CONFIG_SCREENLESS
	Radio.SetChannel(resolved_freq);
#endif

	radio_unlock();
	return RET_OK;
}

/******************************************************************************
 * @brief Tune the radio to the current sniff slot.
 * Slot 0 = shared base frequency (WS1001 and legacy WS1300 ADV),
 * slots 1-8 = WS1300 channels 0-7 (200 kHz spacing, 100 kHz base offset).
 * Caller must restart RX (sx126x_rx_cont) afterwards.
 ******************************************************************************/
void tune_sniff_slot()
{
	const WsFreqDesc *desc = get_freq_desc(nullptr, Settings::get_freq_region());
	if(desc == nullptr)
	{
		ESP_LOGE(TAG, "Invalid freq");
		return;
	}

	if(!radio_lock(RADIO_LOCK_TIMEOUT_MS))
	{
		return;
	}

	const char *mode_tag = (g_manual_listen_slot >= 0) ? "manual" :
		(g_scan_target_slot >= 0 ? "target" : "sniff");
	u32 freq;
	if(g_sniff_slot == 0)
	{
		freq = desc->freq_hz;
		const char *type_tag = (g_receiver_profile == WsType::WS_TYPE_1001) ? "WS1001" : "WS1300";
		snprintf(_listen_desc, sizeof(_listen_desc), "%s base (%s)", type_tag, mode_tag);
	}
	else
	{
		uint8_t ch = g_sniff_slot - 1;
		g_sniff_channel = ch;
		freq = desc->freq_hz + ((u32)Ws1300::FREQ_BASE_OFFSET_KHZ + (u32)ch * Ws1300::FREQ_CHANNEL_GAP_KHZ) * 1000;
		snprintf(_listen_desc, sizeof(_listen_desc), "WS1300 CH%d (%s)", ch, mode_tag);
	}

	_current_freq_hz = freq;
	_sniff_slot_tuned_ts_ms = Utils::get_tick_ms();
	ESP_LOGI(TAG, "Sniff sweep: listening on %s @ %u Hz", _listen_desc, (unsigned int)freq);
#ifndef CONFIG_SCREENLESS
	Radio.SetChannel(freq);
#endif
	radio_unlock();
}

/******************************************************************************
 * @brief Retune the radio according to pair state, sniff mode and profile.
 * Paired (or sniff off): the paired/profile station frequency - a paired
 * gateway never leaves its station's channel, sniffing there only decodes
 * whatever else happens to arrive on the same tuning.
 * Unpaired + sniffing with WS1300/ALL: restart the sniff frequency sweep.
 ******************************************************************************/
void retune_for_current_state()
{
	// Held across the whole decision, not just the tune: g_sniff_slot and
	// g_sniff_channel are read by tune_sniff_slot() and must not be rewritten by
	// a second retuner in between. Recursive, so the nested tune_sniff_slot() /
	// set_radio_freq() calls below re-take it harmlessly.
	if(!radio_lock(RADIO_LOCK_TIMEOUT_MS))
	{
		return;
	}

	if(Settings::get_gateway_radio_mode() == Settings::GatewayRadioMode::MESHCORE_GATEWAY)
	{
		set_radio_freq(Settings::get_freq_region());
		radio_unlock();
		return;
	}

	// Manual channel lock (diagnostics) overrides everything, incl. paired tuning
	if(g_manual_listen_slot >= 0)
	{
		g_sniff_slot = (uint8_t)g_manual_listen_slot;
		tune_sniff_slot();
	}
	else if(g_sniff_mode_enabled && get_pair_state() == PairState::UNPAIRED &&
	   (g_receiver_profile == WsType::WS_TYPE_1300 || g_receiver_profile == WsType::WS_TYPE_ALL))
	{
		// A saved Cloud/Local repair target has a known serial-derived channel.
		// Unknown/passive SCAN still starts on base and sweeps all nine slots.
		g_sniff_slot = g_scan_target_slot >= 0
			? (uint8_t)g_scan_target_slot
			: (_fallback_phase == AutoFallbackPhase::PHASE_WS1300_SWEEP ? (g_sniff_slot >= 1 && g_sniff_slot <= 8 ? g_sniff_slot : 1) : 0);
		tune_sniff_slot();
	}
	else if(g_sniff_mode_enabled && get_pair_state() == PairState::UNPAIRED &&
	        g_receiver_profile == WsType::WS_TYPE_1001)
	{
		g_sniff_slot = 0;
		g_sniff_channel = 0;
		tune_sniff_slot();
	}
	else
	{
		// WS1001 has no channel plan - every station lives on the region base
		// frequency - so the sweep never runs for this profile. Clear the slot
		// anyway: it is left wherever the previous WS1300/ALL sweep stopped, and
		// it is what log_packet stamps onto every captured frame and what
		// get_listen_info reports. A stale value makes base-frequency WS1001
		// captures show up badged as "Ch 4" in the sniffer table.
		if(g_receiver_profile == WsType::WS_TYPE_1001)
		{
			g_sniff_slot = 0;
			g_sniff_channel = 0;
		}
		set_radio_freq(Settings::get_freq_region());
	}

	radio_unlock();
}

/******************************************************************************
 * @brief Set or clear the manual listen channel override (web diagnostics)
 * @param slot -1 = AUTO, 0 = WS1001 base freq, 1-8 = WS1300 channels 0-7
 ******************************************************************************/
RetResult set_manual_listen_slot(int8_t slot)
{
	if(slot < -1 || slot > 8)
	{
		return RET_INVALID_PARAMS;
	}
	// Called from the web server task. Added in 7222495, after the Meshtastic
	// beacon landed - a retune path the borrow logic never knew about.
	if(!radio_lock(RADIO_LOCK_TIMEOUT_MS))
	{
		return RET_ERROR;
	}

	g_manual_listen_slot = slot;
	if (slot >= 0)
	{
		_fallback_phase = AutoFallbackPhase::PHASE_MANUAL;
		_fallback_manual_override = true;
	}
	else
	{
		reset_fallback_state_machine();
	}
	ESP_LOGW(TAG, "Manual listen slot set to %d (%s)", slot, slot < 0 ? "AUTO" : "locked");
	retune_for_current_state();
	sx126x_rx_cont();

	radio_unlock();
	return RET_OK;
}

/******************************************************************************
 * @brief Arm or disarm a remote reboot of the paired WS1300.
 * Armed state persists (survives the web UI closing/reloading) until the
 * station's next packet exchange actually delivers it - the station is
 * request/response only, so there is no way to push this immediately. If the
 * station is asleep/off, it stays armed indefinitely until one arrives.
 * @param armed true to arm, false to cancel a pending request
 * @retval RET_ERROR if arming while not currently paired to a WS1300
 ******************************************************************************/
RetResult set_ws1300_reboot_armed(bool armed)
{
	if(!armed)
	{
		g_ws1300_reboot_requested = false;
		ESP_LOGW(TAG, "Remote reboot request cancelled");
		return RET_OK;
	}
	if(get_pair_state() != PairState::PAIRED || get_cur_station() == NULL ||
	   get_cur_station()->TYPE != WsType::WS_TYPE_1300)
	{
		return RET_ERROR;
	}
	g_ws1300_reboot_requested = true;
	ESP_LOGW(TAG, "Remote reboot armed for paired WS1300 (ID: 0x%08lX)", (unsigned long)_state.pair_data.id);
	return RET_OK;
}

/******************************************************************************
 * @brief Arm or cancel a one-shot "station kick" (CMD-reboot to a sniffed
 * WS1300). Unlike set_ws1300_reboot_armed this works while the gateway is
 * UNPAIRED: the next sniffed non-BOOT WS1300 packet is answered with an
 * authenticated CMD-reboot, making the station emit a pairable STA_BOOT.
 * Sniffing must be active for a packet to be received at all.
 * @param armed true to arm, false to cancel
 * @param target_id 0 = kick the first WS1300 heard, else only this station ID
 ******************************************************************************/
RetResult set_ws1300_kick_armed(bool armed, uint32_t target_id)
{
	if(!armed)
	{
		g_ws1300_kick_armed = false;
		g_ws1300_kick_target_id = 0;
		ESP_LOGW(TAG, "Station kick cancelled");
		return RET_OK;
	}
	if(!g_sniff_mode_enabled && get_pair_state() != PairState::PAIRED)
	{
		// Nothing is listening - the kick could never be delivered
		return RET_INVALID_STATE;
	}
	g_ws1300_kick_target_id = target_id;
	g_ws1300_kick_armed = true;
	if(target_id != 0)
	{
		ESP_LOGW(TAG, "Station kick armed for station 0x%08X", (unsigned int)target_id);
	}
	else
	{
		ESP_LOGW(TAG, "Station kick armed: next sniffed WS1300 packet gets a CMD reboot");
	}
	return RET_OK;
}

/******************************************************************************
 * @brief Set or clear the manual TX power override for the paired WS1300
 * @param dbm 0 = auto (adaptive), else desired power in dBm (clamped)
 ******************************************************************************/
RetResult set_ws1300_tx_power_override(int16_t dbm)
{
	if(dbm != 0)
	{
		if(dbm < Ws1300Helpers::POWER_LEVEL_MIN) dbm = Ws1300Helpers::POWER_LEVEL_MIN;
		if(dbm > Ws1300Helpers::POWER_LEVEL_MAX) dbm = Ws1300Helpers::POWER_LEVEL_MAX;
	}
	g_ws1300_tx_power_override = dbm;
	if(dbm == 0) ESP_LOGW(TAG, "WS1300 TX power override set to AUTO");
	else ESP_LOGW(TAG, "WS1300 TX power override set to %d dBm", dbm);
	return RET_OK;
}

/******************************************************************************
 * @brief Set or clear the manual TX interval override for the paired WS1300
 * @param sec 0 = auto (fixed 20s), else desired interval in seconds
 * (clamped to [20,240], rounded down to a multiple of 20)
 ******************************************************************************/
RetResult set_ws1300_tx_interval_override(int16_t sec)
{
	if(sec != 0)
	{
		if(sec < 20) sec = 20;
		if(sec > 240) sec = 240;
		sec = (sec / 20) * 20;
	}
	g_ws1300_tx_interval_override_sec = sec;
	if(sec == 0) ESP_LOGW(TAG, "WS1300 TX interval override set to AUTO");
	else ESP_LOGW(TAG, "WS1300 TX interval override set to %d s", sec);
	return RET_OK;
}

/******************************************************************************
 * @brief Set Meshtastic configuration for the paired WS1300 station (v15+)
 * @param interval_min 0 = off, else 5, 10, 15, 30, or 60 minutes
 * @param preset Modem preset (0=LongFast..7=ShortTurbo, 255=Custom)
 * @param hop_limit Hop limit (1..5, default 3)
 * @param freq_hz Custom frequency in Hz (0 = auto)
 * @param bw_khz Custom bandwidth in kHz (0 = auto, 62, 125, 250, 500)
 * @param sf Custom SF (0 = auto, 7..12)
 * @param cr Custom CR (0 = auto, 5..8)
 * @param tx_power_dbm Custom TX power dBm (0 = auto, 2..22)
 ******************************************************************************/
RetResult set_ws1300_station_meshtastic(uint8_t interval_min, uint8_t preset, uint8_t hop_limit,
                                        uint32_t freq_hz, uint16_t bw_khz, uint8_t sf, uint8_t cr, int8_t tx_power_dbm)
{
	if (interval_min != 0 && interval_min != 5 && interval_min != 10 &&
	    interval_min != 15 && interval_min != 30 && interval_min != 60) {
		ESP_LOGW(TAG, "Invalid WS1300 station Meshtastic interval: %d min", interval_min);
		return RET_ERROR;
	}
	if (hop_limit < 1 || hop_limit > 5) {
		hop_limit = 3;
	}
	g_ws1300_meshtastic_interval_min = interval_min;
	g_ws1300_meshtastic_preset = preset;
	g_ws1300_meshtastic_hop_limit = hop_limit;
	g_ws1300_meshtastic_custom_freq_hz = freq_hz;
	g_ws1300_meshtastic_custom_bw_khz = bw_khz;
	g_ws1300_meshtastic_custom_sf = sf;
	g_ws1300_meshtastic_custom_cr = cr;
	g_ws1300_meshtastic_custom_tx_power_dbm = tx_power_dbm;
	g_ws1300_meshtastic_changed = true;
	ESP_LOGW(TAG, "WS1300 station Meshtastic armed: interval=%d min, preset=%d, hops=%d, freq=%u, bw=%u, sf=%d, cr=%d, pwr=%d",
	         interval_min, preset, hop_limit, (unsigned)freq_hz, bw_khz, sf, cr, tx_power_dbm);
	return RET_OK;
}

RetResult set_ws1300_station_meshtastic_interval(uint8_t interval_min)
{
	if (interval_min != 0 && interval_min != 5 && interval_min != 10 &&
	    interval_min != 15 && interval_min != 30 && interval_min != 60) {
		ESP_LOGW(TAG, "Invalid WS1300 station Meshtastic interval: %d min", interval_min);
		return RET_ERROR;
	}
	g_ws1300_meshtastic_interval_min = interval_min;
	g_ws1300_meshtastic_changed = true;
	ESP_LOGW(TAG, "WS1300 station Meshtastic armed: interval=%d min", interval_min);
	return RET_OK;
}

/******************************************************************************
 * @brief Set MeshCore configuration for the paired WS1300 (v16+)
 * @param interval_min 0 = off, else 1-60 minutes
 * @param profile_idx MeshCore profile index (0-255)
 * @param freq_hz Custom frequency in Hz (0 = auto)
 * @param bw_khz Custom bandwidth in kHz (0 = auto, 62, 125, 250, 500)
 * @param sf Custom SF (0 = auto, 7..12)
 * @param cr Custom CR (0 = auto, 5..8)
 * @param tx_power_dbm Custom TX power dBm (0 = auto, 2..22)
 ******************************************************************************/
RetResult set_ws1300_station_meshcore(uint8_t interval_min, uint8_t profile_idx,
                                      uint32_t freq_hz, uint16_t bw_khz, uint8_t sf, uint8_t cr, int8_t tx_power_dbm)
{
	if (interval_min > 60) {
		ESP_LOGW(TAG, "Invalid WS1300 station MeshCore interval: %d min", interval_min);
		return RET_ERROR;
	}
	g_ws1300_meshcore_interval_min = interval_min;
	g_ws1300_meshcore_profile_idx = profile_idx;
	g_ws1300_meshcore_custom_freq_hz = freq_hz;
	g_ws1300_meshcore_custom_bw_khz = bw_khz;
	g_ws1300_meshcore_custom_sf = sf;
	g_ws1300_meshcore_custom_cr = cr;
	g_ws1300_meshcore_custom_tx_power_dbm = tx_power_dbm;
	g_ws1300_meshcore_changed = true;
	ESP_LOGW(TAG, "WS1300 station MeshCore armed: interval=%d min, profile=%d, freq=%u, bw=%u, sf=%d, cr=%d, pwr=%d",
	         interval_min, profile_idx, (unsigned)freq_hz, bw_khz, sf, cr, tx_power_dbm);
	return RET_OK;
}

RetResult set_ws1300_station_meshcore_interval(uint8_t interval_min)
{
	if (interval_min > 60) {
		ESP_LOGW(TAG, "Invalid WS1300 station MeshCore interval: %d min", interval_min);
		return RET_ERROR;
	}
	g_ws1300_meshcore_interval_min = interval_min;
	g_ws1300_meshcore_changed = true;
	ESP_LOGW(TAG, "WS1300 station MeshCore armed: interval=%d min", interval_min);
	return RET_OK;
}

/******************************************************************************
 * @brief Get the frequency and a description of what the radio listens to
 ******************************************************************************/
RetResult get_listen_info(uint32_t *freq_hz, char *desc, size_t desc_len)
{
	uint32_t f = _current_freq_hz;
	if(f == 0)
	{
		const WsFreqDesc *d = get_freq_desc(nullptr, Settings::get_freq_region());
		if(d != nullptr)
		{
			f = d->freq_hz;
		}
	}
	if(freq_hz != nullptr)
	{
		*freq_hz = f;
	}
	if(desc != nullptr && desc_len > 0)
	{
		snprintf(desc, desc_len, "%s", _listen_desc);
	}
	return RET_OK;
}

/******************************************************************************
 * @brief Get current sniff slot (0-8)
 ******************************************************************************/
uint8_t get_sniff_slot()
{
	return g_sniff_slot;
}

/******************************************************************************
 * @brief Check if scanning is currently active
 ******************************************************************************/
bool is_scan_active()
{
	if (_scan_completed) return false;
	if (get_pair_state() != PairState::UNPAIRED) return false;
	if (g_manual_listen_slot >= 0) return false;
	return (g_auto_pair_enabled || (Settings::get_pair_mode() == PairMode::PAIR_MODE_SCAN && g_sniff_mode_enabled));
}

size_t get_recent_stations(RecentStation *stations, size_t max_count)
{
	if(stations == nullptr || max_count == 0)
	{
		return 0;
	}
	lock_recent();
	size_t count = 0;
	for(size_t i = 0;
	    i < sizeof(_recent_stations) / sizeof(_recent_stations[0]) && count < max_count;
	    i++)
	{
		if(_recent_stations[i].valid)
		{
			stations[count++] = _recent_stations[i];
		}
	}
	unlock_recent();
	return count;
}

void clear_recent_stations()
{
	lock_recent();
	memset(_recent_stations, 0, sizeof(_recent_stations));
	unlock_recent();
}

/******************************************************************************
 * @brief Get total time in seconds for a full scan sweep cycle
 ******************************************************************************/
uint32_t get_scan_total_cycle_sec()
{
	uint32_t ws1001_dwell = get_ws1001_channel_dwell_ms();
	uint32_t ws1300_channel_dwell = get_ws1300_channel_dwell_ms();
	if (g_scan_profile == WsType::WS_TYPE_1001)
	{
		return ws1001_dwell / 1000;
	}
	else if (g_scan_profile == WsType::WS_TYPE_1300)
	{
		return (8 * ws1300_channel_dwell) / 1000;
	}
	else
	{
		return (ws1001_dwell + 8 * ws1300_channel_dwell) / 1000;
	}
}

/******************************************************************************
 * @brief Get calculated ETA remaining in seconds for the active scan sweep
 ******************************************************************************/
uint32_t get_scan_eta_sec()
{
	if (!is_scan_active())
	{
		return 0;
	}
	if(g_scan_target_slot >= 0)
	{
		return 0;
	}
	uint64_t now_ms = Utils::get_tick_ms();
	uint32_t ws1300_channel_dwell = get_ws1300_channel_dwell_ms();
	uint32_t total_remain_ms = 0;
	if (_scan_phase == ScanPhase::WS1001)
	{
		uint32_t ws1001_dwell = get_ws1001_channel_dwell_ms();
		uint64_t elapsed = (now_ms >= _ws1001_scan_start_ms) ? (now_ms - _ws1001_scan_start_ms) : 0;
		uint32_t ws1001_rem_ms = (ws1001_dwell > elapsed) ? (ws1001_dwell - elapsed) : 0;
		if (g_scan_profile == WsType::WS_TYPE_1001)
		{
			total_remain_ms = ws1001_rem_ms;
		}
		else
		{
			uint32_t ws1300_sweep_ms = 8 * ws1300_channel_dwell;
			total_remain_ms = ws1001_rem_ms + ws1300_sweep_ms;
		}
	}
	else
	{
		uint64_t elapsed_in_slot = (now_ms >= _sniff_slot_tuned_ts_ms) ? (now_ms - _sniff_slot_tuned_ts_ms) : 0;
		uint32_t slot_remain_ms = (ws1300_channel_dwell > elapsed_in_slot) ? (ws1300_channel_dwell - elapsed_in_slot) : 0;
		uint32_t slots_left = (8 >= g_sniff_slot) ? (8 - g_sniff_slot) : 0;
		total_remain_ms = slot_remain_ms + (slots_left * ws1300_channel_dwell);
	}

	if (_scan_cycle_count == 0 && Settings::get_pair_mode() == PairMode::PAIR_MODE_SCAN)
	{
		if (g_scan_profile == WsType::WS_TYPE_1001)
		{
			total_remain_ms += SCAN_PASS2_WS1001_DWELL_MS;
		}
		else if (g_scan_profile == WsType::WS_TYPE_1300)
		{
			total_remain_ms += 8 * SCAN_PASS2_WS1300_DWELL_MS;
		}
		else
		{
			total_remain_ms += SCAN_PASS2_WS1001_DWELL_MS + (8 * SCAN_PASS2_WS1300_DWELL_MS);
		}
	}

	return total_remain_ms / 1000;
}

}
