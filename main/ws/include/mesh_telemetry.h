#ifndef MESH_TELEMETRY_H
#define MESH_TELEMETRY_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "ws/include/adaptive_publish.h"

/******************************************************************************
 * Meshtastic "fire and forget" environmental telemetry beacon.
 *
 * Broadcasts the paired weather station's latest observation as a standard
 * Meshtastic TELEMETRY_APP / EnvironmentMetrics packet on the public default
 * channel, so any stock Meshtastic node in range shows live weather data on
 * its Environment screen with zero configuration.
 *
 * TX only. The gateway never receives, decodes, routes or rebroadcasts
 * Meshtastic traffic - it is not a mesh participant. The outgoing hop limit
 * is configurable; Channel Activity Detection is used before each transmit.
 *
 * IMPORTANT - radio ownership:
 *   The SX1262 is owned by WsHandler and is normally parked in continuous RX
 *   on the weather station's private link (sync word 0x1424, station SF/BW/CR,
 *   station frequency). A Meshtastic transmit has to retune the modem, and for
 *   the duration of that transmit the gateway is deaf to its own station.
 *   For that reason tick() is driven from inside the WsHandler packet handler
 *   task, immediately after a station packet has been serviced - the point in
 *   the cycle where the next station transmission is furthest away. There is
 *   deliberately no independent telemetry task; a second task touching the
 *   radio would race the station link.
 ******************************************************************************/
namespace WsHandler {
	struct RadioLease;
	struct PacketOutcome;
}

namespace MeshTelemetry
{
	enum class MeshProtocol : uint8_t
	{
		MESHTASTIC = 0,
		MESHCORE = 1
	};

	/** Initialise module state. Does not start a task and does not touch the radio. */
	esp_err_t init();

	/**
	 * Drive the beacon. MUST be called only from the WsHandler packet handler
	 * task, at a point where the radio is idle and safe to retune.
	 * Sends at most one packet per call subject to RadioLease deadline validation.
	 * The configured interval I drives Environment 1x, NodeInfo 2x, Device 3x, and Position 6x schedules.
	 * Cheap no-op when nothing is due.
	 */
	void tick(const WsHandler::RadioLease &lease, const WsHandler::PacketOutcome &outcome);
	void tick();

	/**
	 * Ask for a telemetry packet to be sent at the next safe radio window.
	 * Safe to call from any task (e.g. the HTTP server).
	 * @param wait_ms how long to block waiting for the send to actually happen.
	 *                Pass 0 to queue and return immediately.
	 * @return true if a packet was transmitted within wait_ms.
	 */
	bool request_send(uint32_t wait_ms);

	/** Queue a send and wait for it. Used by the web API. */
	bool send_now();

	/** Seconds since last successful transmit, 0xFFFFFFFF if never. */
	uint32_t get_seconds_since_last_send();

	/** Total telemetry packets transmitted since boot. */
	uint32_t get_sent_count();

	/** This gateway's stable Meshtastic node number (derived from the eFuse MAC). */
	uint32_t get_node_num();

	/** Node id in Meshtastic "!hhhhhhhh" form. Points at a static buffer. */
	const char *get_node_id_str();

	/** Meshtastic RF frequency in Hz for the configured region, 0 if unsupported. */
	uint32_t get_mesh_freq_hz();

	/** Long name e.g. "WeatherXM WG1200 XXXX" or custom name (max 23 chars). */
	const char *get_long_name();

	/** Short name e.g. "GXXX" or custom short name (max 4 chars). */
	const char *get_short_name();

	/** Default long name according to convention ("WeatherXM WG1200 XXXX") */
	void get_default_long_name(char *out, size_t max_len);

	/** Default short name according to convention ("GXXX") */
	void get_default_short_name(char *out, size_t max_len);

	/** Refresh identity names from settings. Returns true if changed. */
	bool update_names();

	/** Hop limit used for transmissions. */
	uint8_t get_hop_limit();

	/** Request a MeshCore Gateway telemetry packet to be sent at the next safe radio window. */
	bool request_meshcore_send(uint32_t wait_ms = 0);

	/** Queue a MeshCore Gateway send and wait for it. Used by the web API. */
	bool send_meshcore_now();

	/** Seconds since last successful MeshCore Gateway transmit, 0xFFFFFFFF if never. */
	uint32_t get_meshcore_seconds_since_last_send();

	/** Total MeshCore telemetry packets transmitted since boot. */
	uint32_t get_meshcore_sent_count();

	/** MeshCore RF frequency in Hz for the configured region, 0 if unsupported. */
	uint32_t get_meshcore_freq_hz();

	/** Evaluator diagnostic status for telemetry and Web UI */
	struct EvaluatorStatus {
		uint8_t events_this_hour;
		uint8_t cooldown_level;
		uint16_t last_reasons;
		char last_reasons_str[128];
	};

	/** Get current evaluator status snapshot */
	EvaluatorStatus get_evaluator_status(MeshProtocol proto);

	/** Reset evaluator baselines for both Meshtastic and MeshCore */
	void reset_baseline();
}

#endif // MESH_TELEMETRY_H
