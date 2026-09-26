#ifndef LOG_ERROR_CODES_H
#define LOG_ERROR_CODES_H

namespace Log
{
	enum class Code
	{
		NONE = 0,
		BOOT,

		/*
		 * System
		 */
		UPTIME = 0x01000000,

		/*
		 * Sniffer
		 */
		// Lost weather station connectivity (not receiving packets)
		SNIFFER_WS_INACTIVE = 0x02000000,

		// Found weather station after losing connectivity with it
		// Meta1: WS ID
		SNIFFER_WS_ACTIVE_OK,

		SNIFFER_WS_ACTIVE_GPS_FIXING,

		SNIFFER_WS_ACTIVE_BAD_DATA,

		SNIFFER_WS_ACTIVE_LOW_RECEPTION,

		SNIFFER_WS_PAIRED,

		SNIFFER_WS_UNPAIRED,

		SNIFFER_TIME_INVALID,

		// Invalid packet received
		SNIFFER_INVALID_PACKET_RECEIVED,

		// Unverified packet received
		SNIFFER_UNVERIFIED_PACKET_RECEIVED,

		// Packet ignored
		SNIFFER_PACKET_IGNORED,

		// Invalid sensor values received 
		SNIFFER_INVALID_SENSOR_VALUES,

		// Any packet received
		SNIFFER_PACKET_RECEIVED,

		// Ack payload sent
		SNIFFER_ACK_SENT,

		// Ack station is losing acks
		SNIFFER_ACK_LOSS,

		SNIFFER_WS_REBOOT,

		SNIFFER_WS_PACKET_LOSS,

		// Station packet RSSI and SNR
		// Meta1: RSSI (dBm), Meta2: SNR (dB)
		SNIFFER_PACKET_RSSI,

		/*
		 * WiFi
		 */
		// WiFi connected
		WIFI_CONNECTED = 0x03000000,

		// WiFi connected no IP
		WIFI_CONNECTED_NO_IP,

		// WiFi connected no internet
		WIFI_CONNECTED_NO_INTERNET,

		// WiFi connection failed
		WIFI_CONNECTION_FAILED,

		// WiFi disconnected
		// meta1: reason
		WIFI_DISCONNECTED,

		// WiFi RSSI
		// Meta1: RSSI
		WIFI_RSSI,

		// Downtime of WiFi
		// Meta1: Duration in seconds
		WIFI_DOWNTIME,

		/*
		 * TB
		 */
		// TB connected
		TB_CONNECTED = 0x04000000,

		// TB disconnected
		TB_DISCONNECTED,

		// Downtime of TB
		// Meta1: Duration in seconds
		TB_DOWNTIME,

		// Publishing failed
		TB_SUBMIT_ITEM_FAILED,

		/*
		 * RTC
		 */
		// RTC synced
		// meta1: source(0:NTP, 1:GPS)
		RTC_SYNCED = 0x05000000,

		// RTC sync failed
		// meta1: source
		RTC_SYNC_FAILED,

		/*
		 * Settings
		 */
		// Settings loaded
		SETTINGS_LOADED = 0x06000000,

		// Settings load failed
		SETTINGS_LOAD_FAILED,

		// Settings commited
		SETTINGS_COMMITED,

		// Settings commit failed
		SETTINGS_COMMIT_FAILED,

		/*
		 * Those need to be in ws generic context, not in ws1001 anymore. Keeping the same offset until TB dashboard change.
		 */
		// Requested 
		WS_GPS_FIX_REQUESTED = 0x07000000,

		WS_GPS_FIX_RECEIVED,

		WS_GPS_EXPIRED,

		WS_GPS_FIX_FAILED,

		WS_GPS_FIX_REQ_NOT_PERMITTED,

		/*
		 * OTA / boot guard
		 */
		// Firmware was rolled back to the previous image after repeated crash
		// boots. Reported retroactively by the first healthy firmware boot.
		// Meta1: consecutive crash boots, Meta2: last reset reason (esp_reset_reason_t)
		OTA_ROLLBACK = 0x08000000,

		// Crash loop was detected but no valid image existed to roll back to
		// Meta1: consecutive crash boots, Meta2: last reset reason
		OTA_ROLLBACK_IMPOSSIBLE,

		// A beta/prototype weather station (pre-release protocol) is paired or
		// being received. Meta1: station type, Meta2: station protocol/fw version
		WS_BETA_STATION_DETECTED = 0x09000000

	};
}

#endif