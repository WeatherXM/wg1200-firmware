#ifndef WS_HANDLER_H
#define WS_HANDLER_H

#include "ws/supported/ws_1001/include/ws_1001.h"
#include "ws/supported/ws_1300/include/ws_1300.h"
#include "ws/include/ws_base.h"
#include "struct.h"

namespace WsHandler
{
extern const WsBase::MultiStation STATION_LIST[2];

/******************************************************************************
 * @brief Pair state. Either PAIRED or UNPAIRED
 ******************************************************************************/
enum class PairState
{
	NONE = 0,
	PAIRED,
	UNPAIRED
};

/******************************************************************************
 * @brief Packet status after it is handled by station handler
 ******************************************************************************/
enum class PktStatus
{
	NONE = 0,
	OK,
	OK_BUSY,
	IGNORE,
	BAD_DATA
};

/******************************************************************************
 * @brief Station status
 ******************************************************************************/
enum class ActivityStatus
{
	NONE = 0,
	ACTIVE_OK,
	ACTIVE_BAD_DATA,
	ACTIVE_BUSY,
	ACTIVE_LOW_RECEPTION,
	INACTIVE
};

/** Coarse indication of how hard the gateway is driving station TX power. */
enum class StationPowerSetting
{
	UNKNOWN = 0,
	LOW,
	MID,
	HIGH
};

/******************************************************************************
 * @brief Station status to title binding
 ******************************************************************************/
struct ActivityStatusDescriptor{
	const char *title;
	ActivityStatus status;
};

#define RECENT_STATIONS_CAPACITY 10

/** RAM-only summary of recently decoded weather stations.
 * Discovery registry for user selection and diagnostics, never automatic pairing authority. */
struct RecentStation
{
	bool valid;
	WsType type;
	uint32_t id;
	bool has_sn;
	uint8_t sn[WS_SN_LEN];
	bool has_pubkey;
	uint8_t pubkey[WS_PUB_KEY_LEN];
	bool pairable;
	uint8_t packet_type;
	uint32_t last_seen_tick_sec;
	int16_t rssi;
	int8_t snr;
	uint8_t channel_slot;
};

/** WS status descriptor*/
const ActivityStatusDescriptor ACTIVITY_STATUS_TITLES[] = {
	{ "NONE", ActivityStatus::NONE },
	{ "ACTIVE_OK", ActivityStatus::ACTIVE_OK },
	{ "ACTIVE_LOW_RECEPTION", ActivityStatus::ACTIVE_LOW_RECEPTION },
	{ "ACTIVE_BAD_DATA", ActivityStatus::ACTIVE_BAD_DATA },
	{ "ACTIVE_BUSY", ActivityStatus::ACTIVE_BUSY},
	{ "INACTIVE", ActivityStatus::INACTIVE}
};


/** Radio borrow contract and deadline enforcement */
struct RadioLease
{
	int64_t expiry_ms;
	const char *caller_id;
};

/** Outcome of a station packet reception cycle */
struct PacketOutcome
{
	bool success;
	bool paired_p2p_cycle;
	int64_t completion_timestamp_ms;
};

RetResult init();
WsBase::MultiStation* get_cur_station();
void unpair(bool wipe_nvs = true);
void sx126x_tx(uint8_t *buffer, uint8_t size);
void sx126x_rx_cont();
void retune_for_current_state();

/******************************************************************************
 * @brief Transmit a raw frame using foreign LoRa parameters (Meshtastic).
 *
 * Retunes the SX1262 away from the weather station link, waits a randomized
 * contention period, performs LoRa Channel Activity Detection, transmits once
 * only when clear, then hands the radio back to the station link.
 *
 * MUST be called from the packet handler task only. The gateway is deaf to its
 * own station for the duration of the call (~1 s at SF11).
 *
 * @param buffer     frame to send
 * @param size       frame length in bytes
 * @param freq_hz    RF centre frequency
 * @param sf         spreading factor (7..12)
 * @param bw_idx     Semtech bandwidth index: 0 = 125 kHz, 1 = 250 kHz, 2 = 500 kHz
 * @param cr         coding rate index: 1 = 4/5 ... 4 = 4/8
 * @param preamble   preamble length in symbols
 * @param lease      optional RadioLease contract to enforce dual-stage deadline checks
 * @return true if TX completed; false when busy/deferred or on radio failure
 ******************************************************************************/
bool sx126x_tx_meshtastic(uint8_t *buffer, uint8_t size, uint32_t freq_hz,
                          uint32_t sf, uint32_t bw_idx, uint8_t cr, uint16_t preamble,
                          const RadioLease *lease = nullptr);
bool sx126x_tx_meshcore(uint8_t *buffer, uint8_t size, uint32_t freq_hz,
                        uint32_t sf, uint32_t bw_idx, uint8_t cr, uint16_t preamble,
                        const RadioLease *lease = nullptr);
RetResult apply_pair_mode(PairMode mode, uint32_t scan_target_id = 0,
	int8_t requested_target_slot = -1,
	WsType scan_profile = WsType::WS_TYPE_NONE);
PktStatus get_last_packet_status();
ActivityStatus get_activity_status();
const char * get_activity_status_title(ActivityStatus status);
uint32_t get_activity_status_duration_sec();
PairState get_pair_state();
PairSource get_active_pair_source();
RetResult get_last_verified_ts(uint64_t *ts);
uint8_t get_packet_reception();
StationPowerSetting get_station_power_setting();
const char *get_station_power_setting_title();
bool is_station_power_control_exhausted();
bool get_station_signal(int16_t *rssi, int8_t *snr);
bool is_in_station_packet_guard_window(uint32_t *defer_ms = nullptr);
u32 get_paired_id();
RetResult get_paired_sn(uint8_t* buff, size_t buff_len);
const WsFreqDesc *get_freq_desc(char *name, WsFreqRegion freq_region);

/******************************************************************************
 * Radio ownership.
 *
 * The SX1262 is a single stateful device shared between the weather station
 * link and the Meshtastic beacon, and it is reprogrammed by multi-command SPI
 * sequences that must not be interleaved. Any code that retunes, reconfigures
 * or transmits on the radio must hold this lock for the whole sequence - not
 * just for the individual SPI transactions, which the driver already
 * serialises on its own.
 *
 * Recursive, so nested helpers may re-take it. Bounded wait: a caller that
 * fails to acquire MUST abandon its operation rather than touch the radio.
 ******************************************************************************/
bool radio_lock(uint32_t timeout_ms);
void radio_unlock();

/** Number of times a task had to wait for the radio because a Meshtastic
 * transmit was in flight. Before the ownership lock these were silent
 * mid-frame retunes, seen at the receiver as payload CRC failures. Exposed for
 * the diagnostics UI: non-zero here confirms the contention is real. */
uint32_t get_radio_contention_count();

extern bool g_sniff_mode_enabled;
extern bool g_auto_pair_enabled;
extern uint8_t g_sniff_channel;
extern uint8_t g_sniff_slot;
extern int8_t g_manual_listen_slot;
extern WsType g_receiver_profile;
extern WsType g_scan_profile;
RetResult set_manual_listen_slot(int8_t slot);
WsCommon::RetRes apply_station_change(WsType station_type);
void notify_sniff_mode_toggled();
RetResult get_listen_info(uint32_t *freq_hz, char *desc, size_t desc_len);
bool try_sniff_decode(WsCommon::Packet* pkt, WsBase::MultiStation** matched_station,
	WsFields::FieldPack* field_pack, WsGps::GpsData* gps_data);
uint8_t get_sniff_slot();
enum class ScanPhase {
	WS1001,
	WS1300
};
ScanPhase get_scan_phase();
WsType get_scan_profile();
uint32_t get_scan_cycle_count();
uint32_t get_scan_eta_sec();
uint32_t get_scan_total_cycle_sec();
bool is_scan_active();
bool is_scan_completed();
size_t get_recent_stations(RecentStation *stations, size_t max_count);
void clear_recent_stations();

/**
 * @brief Compile-time developer testing flag for automated fallback recovery.
 * Set to false to bypass Phase 3 (WS1001 Dedicated Scan) and advance immediately
 * to Phase 4 (WS1300 LoRa Sweep) in environments with active WS1001 stations.
 */
#define FALLBACK_ENABLE_PHASE_3_WS1001 true

/**
 * @brief Developer flag to test Phase 4 (WS1300 Channel Sweep CH0-CH7) exclusively.
 * When enabled (true), the gateway boots directly into Phase 4 (WS1300 Sweep), bypassing
 * Phase 1 (Local NVS), Phase 2 (Cloud NVS), Phase 3 (WS1001 Scan), and Phase 5 Cooldown.
 */
#define FALLBACK_TEST_PHASE_4_ONLY false

/**
 * @brief Automated 5-Phase Fallback & Recovery State Machine (~1 Hour Total Cycle)
 *
 *  +---------------------------------------------------------------------------------+
 *  | Phase 1: Local NVS (5 min / 300s)                                               |
 *  | Listen on saved local station frequency (FSK 868.0 or LoRa CHx).                |
 *  | [Match? -> LATCHED] -- [Timeout?] -----------------------------------+          |
 *  +----------------------------------------------------------------------|----------+
 *                                                                         |
 *                                                                         v
 *  +---------------------------------------------------------------------------------+
 *  | Phase 2: Cloud NVS (5 min / 300s)                                               |
 *  | Listen on cloud-assigned station frequency (skipped if identical to Local NVS). |
 *  | [Match? -> LATCHED] -- [Timeout?] -----------------------------------+          |
 *  +----------------------------------------------------------------------|----------+
 *                                                                         |
 *                                                                         v
 *  +---------------------------------------------------------------------------------+
 *  | Phase 3: Dedicated WS1001 Scan (5 min / 300s)                                   |
 *  | Radio locked to Base frequency (868.0 MHz FSK).                                 |
 *  | Accepts ANY WS1001 ID, saves to Local NVS, latches, and halts scan.             |
 *  | (Bypassed if FALLBACK_ENABLE_PHASE_3_WS1001 is set to false)                    |
 *  | [Any WS1001? -> LATCHED] -- [Timeout?] ------------------------------+          |
 *  +----------------------------------------------------------------------|----------+
 *                                                                         |
 *                                                                         v
 *  +---------------------------------------------------------------------------------+
 *  | Phase 4: Dedicated WS1300 Sweep (16 min / 960s)                                 |
 *  | Sweeps LoRa CH0 through CH7 (868.1 to 869.5 MHz), dwelling 2 min (120s) each.   |
 *  | Catches WS1300 stations backing off in slowest submit mode.                     |
 *  | [Any WS1300 ADV/BOOT/Saved? -> Instant Latch + GPS challenge -> LATCHED]        |
 *  | [Timeout?] ----------------------------------------------------------+          |
 *  +----------------------------------------------------------------------|----------+
 *                                                                         |
 *                                                                         v
 *  +---------------------------------------------------------------------------------+
 *  | Phase 5: Cooldown (30 min / 1800s)                                              |
 *  | Radio parked on primary target (Cloud if available, else Local).                |
 *  | [Packets? -> LATCHED] -- [30 min Expired? -> Restart at Phase 1]                 |
 *  +---------------------------------------------------------------------------------+
 *
 * Total Cycle: 5m + (5m) + 5m + 16m + 30m = ~56 to 61 minutes (~1 hour).
 * Latching Rule: Discovered stations write strictly to Local NVS, preserving Cloud NVS.
 */
enum class AutoFallbackPhase {
	PHASE_LOCAL_NVS = 0,     // Phase 1: Local NVS station
	PHASE_CLOUD_NVS = 1,     // Phase 2: Cloud NVS station (if different)
	PHASE_WS1001_SCAN = 2,   // Phase 3: Dedicated WS1001 scan on Base (868.0 MHz FSK)
	PHASE_WS1300_SWEEP = 3,  // Phase 4: Dedicated WS1300 sweep (CH0-CH7, 2 min per channel)
	PHASE_COOLDOWN = 4,      // Phase 5: 30-minute cooldown parked on primary target
	PHASE_LATCHED = 5,       // Station latched and locked to Local NVS
	PHASE_MANUAL = 6         // Manual override from Web UI / user command
};
AutoFallbackPhase get_fallback_phase();
uint32_t get_fallback_seconds_remaining();
const char* get_fallback_phase_name();
void set_fallback_phase(AutoFallbackPhase phase);
void reset_fallback_state_machine();

/**
 * @brief Get high-level pairing lifecycle state name:
 *        "standby_discovery", "cloud_verified", "local_override", "fallback_latch", "monitor_only"
 */
const char* get_pairing_state_name();

extern bool g_ws1300_reboot_requested;
extern bool g_ws1300_gps_req_forced;
extern bool g_ownership_challenge_armed;
extern bool g_ownership_challenge_pending;
extern bool g_ownership_confirmed;
extern uint32_t g_ownership_challenge_station_id;
extern uint32_t g_ownership_challenge_started_sec;
extern uint8_t g_ownership_challenge_packets_seen;
extern bool g_ws1300_kick_armed;
extern uint32_t g_ws1300_kick_target_id;
extern uint32_t g_scan_target_id;
extern int8_t g_scan_target_slot;
extern int16_t g_ws1300_tx_power_override;
extern int16_t g_ws1300_tx_interval_override_sec;
extern uint8_t g_ws1300_meshtastic_interval_min;
extern uint8_t g_ws1300_meshtastic_preset;
extern uint8_t g_ws1300_meshtastic_hop_limit;
extern uint32_t g_ws1300_meshtastic_custom_freq_hz;
extern uint16_t g_ws1300_meshtastic_custom_bw_khz;
extern uint8_t g_ws1300_meshtastic_custom_sf;
extern uint8_t g_ws1300_meshtastic_custom_cr;
extern int8_t g_ws1300_meshtastic_custom_tx_power_dbm;
extern bool g_ws1300_meshtastic_changed;

extern uint8_t g_ws1300_meshcore_interval_min;
extern uint8_t g_ws1300_meshcore_profile_idx;
extern uint32_t g_ws1300_meshcore_custom_freq_hz;
extern uint16_t g_ws1300_meshcore_custom_bw_khz;
extern uint8_t g_ws1300_meshcore_custom_sf;
extern uint8_t g_ws1300_meshcore_custom_cr;
extern int8_t g_ws1300_meshcore_custom_tx_power_dbm;
extern bool g_ws1300_meshcore_changed;
extern bool g_ws1300_weather_policy_changed;
extern uint8_t g_ws1300_station_fw_ver;
RetResult set_ws1300_reboot_armed(bool armed);
RetResult trigger_gps_request();
RetResult set_ws1300_kick_armed(bool armed, uint32_t target_id);
RetResult set_ws1300_tx_power_override(int16_t dbm);
RetResult set_ws1300_tx_interval_override(int16_t sec);
RetResult set_ws1300_station_meshtastic(uint8_t interval_min, uint8_t preset, uint8_t hop_limit,
                                        uint32_t freq_hz, uint16_t bw_khz, uint8_t sf, uint8_t cr, int8_t tx_power_dbm);
RetResult set_ws1300_station_meshtastic_interval(uint8_t interval_min);
RetResult set_ws1300_station_meshcore(uint8_t interval_min, uint8_t profile_idx,
                                      uint32_t freq_hz = 0, uint16_t bw_khz = 0, uint8_t sf = 0, uint8_t cr = 0, int8_t tx_power_dbm = 0);
RetResult set_ws1300_station_meshcore_interval(uint8_t interval_min);

extern char g_ws_fw_warning[64];
void set_station_fw_warning(const char *warning);
const char* get_station_fw_warning();

void set_station_last_err(const char *err);
const char* get_station_last_err();

void set_station_sensor_err(const char *err);
const char* get_station_sensor_err();

void set_station_gps_status(const char *status);
const char* get_station_gps_status();

void set_station_ack_loss(bool lost);
bool get_station_ack_loss();

void add_station_lost_packets(uint32_t count);
uint32_t get_station_lost_packets();

}

#endif
