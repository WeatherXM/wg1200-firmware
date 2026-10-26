#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "include/web_server.h"
#include "include/dashboard_html_gz.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <sys/param.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

// Include system headers
#include "include/app_config.h"
#include "pressure.h"
#include "settings.h"
#include "submit.h"
#include "tb.h"
#include "utils.h"
#include "wifi.h"
#include "ws/include/kiss_client.h"
#include "ws/include/ws_handler.h"
#include "ws/include/ws_data.h"
#include "ws/include/mesh_telemetry.h"
#include "ws/include/adaptive_publish.h"
#include "ws/supported/ws_1300/include/ws_1300_helpers.h"
#include "ws/supported/ws_1001/include/ws_1001.h"
#include "state_sniffer.h"
#include "units.h"
#include "weather_logger.h"
#include "mdns_discovery.h"
#include "bsp_storage.h"
#include "bsp_board.h"
#include "lv_port.h"
#include <string>
#include <cmath>

#if __has_include("mbedtls/private/aes.h")
#include "mbedtls/private/aes.h"
#else
#include "mbedtls/aes.h"
#endif
#if __has_include("mbedtls/private/md.h")
#include "mbedtls/private/md.h"
#else
#include "mbedtls/md.h"
#endif
#include "ws/include/mesh_frame.h"
#include "ws/include/meshcore_frame.h"

static const char TAG[] = "WebServer";

// Add counters for TB telemetry
uint32_t g_tb_success_count = 0;
uint32_t g_tb_fail_count = 0;

namespace WebServer {

static void ws_aes_ecb(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
	mbedtls_aes_context aes;
	mbedtls_aes_init(&aes);
	if (mbedtls_aes_setkey_enc(&aes, key, 128) == 0) {
		if (mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, in, out) != 0) {
			memset(out, 0, 16);
		}
	} else {
		memset(out, 0, 16);
	}
	mbedtls_aes_free(&aes);
}

static void ws_meshcore_aes_ecb(const uint8_t *input, uint8_t *output, size_t length, const uint8_t *key_16b, bool encrypt)
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

static void ws_meshcore_hmac_sha256(const uint8_t *data, size_t data_len, const uint8_t *key_16b, uint8_t *out_mac_32b)
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

struct LoggedPacket {
  uint32_t timestamp_sec;
  uint16_t length;
  int16_t rssi;
  int8_t snr;
  bool filtered;
  bool is_tx;
  int8_t tx_power_dbm;
  uint8_t sf;
  uint32_t bw_hz;
  uint8_t cr;
  uint8_t channel_slot;
  uint32_t freq_hz;
  uint8_t payload[256];
};

static constexpr int PACKET_LOG_CAPACITY = 30;
static LoggedPacket _packet_log[PACKET_LOG_CAPACITY];
static int _packet_log_head = 0;
static int _packet_log_count = 0;
static SemaphoreHandle_t _packet_log_mutex = NULL;
/** Every LoRa frame the radio demodulated since boot, decoded or not.
 * Exposed in /status so "the radio heard nothing" can be told apart from
 * "the UI failed to render what it heard" without a serial cable. */
static uint32_t _rx_packet_total = 0;
static httpd_handle_t _server_handle = NULL;
uint32_t g_last_web_request_ts = 0;

static void format_json_float(char *out, size_t out_len, float value,
                              unsigned int decimals) {
  if (std::isfinite(value)) {
    snprintf(out, out_len, "%.*f", (int)decimals, value);
  } else {
    snprintf(out, out_len, "null");
  }
}

void log_packet(const uint8_t *payload, uint16_t len, int16_t rssi, int8_t snr,
                bool filtered) {
  if (_packet_log_mutex == NULL) {
    _packet_log_mutex = xSemaphoreCreateMutex();
  }

  if (xSemaphoreTake(_packet_log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    LoggedPacket &pkt = _packet_log[_packet_log_head];
    pkt.timestamp_sec = (uint32_t)time(NULL);
    pkt.length = MIN(len, sizeof(pkt.payload));
    pkt.rssi = rssi;
    pkt.snr = snr;
    pkt.filtered = filtered;
    pkt.is_tx = false;
    pkt.tx_power_dbm = 0;
    pkt.sf = 7;
    pkt.bw_hz = 125000;
    pkt.cr = 5;
    pkt.channel_slot = WsHandler::g_sniff_slot;
    uint32_t cur_freq = 0;
    WsHandler::get_listen_info(&cur_freq, nullptr, 0);
    pkt.freq_hz = cur_freq;
    memcpy(pkt.payload, payload, pkt.length);

    _packet_log_head = (_packet_log_head + 1) % PACKET_LOG_CAPACITY;
    if (_packet_log_count < PACKET_LOG_CAPACITY) {
      _packet_log_count++;
    }
    _rx_packet_total++;
    xSemaphoreGive(_packet_log_mutex);
  }
}

void log_tx_packet(const uint8_t *payload, uint16_t len, uint32_t freq_hz,
                   uint8_t sf, uint32_t bw_hz, uint8_t cr, int8_t tx_power_dbm) {
  if (_packet_log_mutex == NULL) {
    _packet_log_mutex = xSemaphoreCreateMutex();
  }

  if (xSemaphoreTake(_packet_log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    LoggedPacket &pkt = _packet_log[_packet_log_head];
    pkt.timestamp_sec = (uint32_t)time(NULL);
    pkt.length = MIN(len, sizeof(pkt.payload));
    pkt.rssi = tx_power_dbm;
    pkt.snr = 0;
    pkt.filtered = false;
    pkt.is_tx = true;
    pkt.tx_power_dbm = tx_power_dbm;
    pkt.sf = sf;
    pkt.bw_hz = bw_hz;
    pkt.cr = cr;
    pkt.channel_slot = 0;
    pkt.freq_hz = freq_hz;
    memcpy(pkt.payload, payload, pkt.length);

    _packet_log_head = (_packet_log_head + 1) % PACKET_LOG_CAPACITY;
    if (_packet_log_count < PACKET_LOG_CAPACITY) {
      _packet_log_count++;
    }
    xSemaphoreGive(_packet_log_mutex);
  }
}

// Inline gorgeous Glassmorphic Dashboard HTML/CSS/JS
static const char g_dashboard_html[] = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>WeatherXM Gateway - Diagnostics</title>
    <link rel="help" type="text/markdown" href="/llms.txt" title="LLM & Agent Documentation">
    <link rel="alternate" type="application/json" href="/status" title="Current Status JSON">
    <link href="https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;600;800&display=swap" rel="stylesheet">
    <link rel="stylesheet" href="/uPlot.min.css">
    <script src="/uPlot.iife.min.js"></script>
    <style>
        :root {
            --bg-color: #0b0f19;
            --card-bg: rgba(255, 255, 255, 0.03);
            --card-border: rgba(255, 255, 255, 0.08);
            --glass-blur: blur(12px);
            --primary: #00d2ff;
            --secondary: #00f2fe;
            --accent-green: #00f5a0;
            --accent-red: #ff416c;
            --text-color: #f1f3f9;
            --text-muted: #8c9cb2;
        }

        * {
            box-sizing: border-box;
            margin: 0;
            padding: 0;
        }

        body {
            font-family: 'Outfit', sans-serif;
            background-color: var(--bg-color);
            background-image: 
                radial-gradient(at 0% 0%, rgba(0, 210, 255, 0.1) 0px, transparent 50%),
                radial-gradient(at 100% 100%, rgba(0, 242, 254, 0.08) 0px, transparent 50%);
            color: var(--text-color);
            min-height: 100vh;
            padding: 0.6rem 0.8rem;
            display: flex;
            flex-direction: column;
            align-items: center;
        }

        .container {
            width: 100%;
            max-width: 100%;
        }

        header {
            width: 100%;
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 0.5rem;
            padding-bottom: 0.3rem;
            border-bottom: 1px solid var(--card-border);
        }

        header h1 {
            font-weight: 800;
            font-size: 1.3rem;
            background: linear-gradient(135deg, var(--primary), var(--secondary));
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
        }

        .badge-live {
            display: inline-flex;
            align-items: center;
            gap: 0.35rem;
            font-size: 0.72rem;
            font-weight: 600;
            border-radius: 20px;
            cursor: default;
            user-select: none;
            transition: all 0.2s ease;
        }

        .badge-live.state-healthy {
            padding: 0.35rem;
            background: transparent;
            border: none;
        }

        .badge-live.state-healthy::before {
            content: '';
            display: inline-block;
            width: 8px;
            height: 8px;
            background-color: var(--accent-green);
            border-radius: 50%;
            box-shadow: 0 0 8px var(--accent-green);
            animation: pulse 1.8s infinite alternate;
        }

        .badge-live.state-error {
            background: rgba(239, 68, 68, 0.15);
            color: #ef4444;
            border: 1px solid rgba(239, 68, 68, 0.35);
            padding: 0.2rem 0.6rem;
        }

        .badge-live.state-error::before {
            content: '';
            display: inline-block;
            width: 7px;
            height: 7px;
            background-color: #ef4444;
            border-radius: 50%;
            box-shadow: 0 0 6px #ef4444;
            margin-right: 2px;
        }

        .badge-live.state-warning {
            background: rgba(245, 158, 11, 0.15);
            color: #f59e0b;
            border: 1px solid rgba(245, 158, 11, 0.35);
            padding: 0.2rem 0.6rem;
        }

        .badge-live.state-warning::before {
            content: '';
            display: inline-block;
            width: 7px;
            height: 7px;
            background-color: #f59e0b;
            border-radius: 50%;
            box-shadow: 0 0 6px #f59e0b;
            margin-right: 2px;
        }

        @keyframes pulse {
            0% { transform: scale(0.9); opacity: 0.6; }
            100% { transform: scale(1.2); opacity: 1; }
        }

        .badge-sniffing {
            background: rgba(168, 85, 247, 0.15);
            color: #c084fc;
            padding: 0.25rem 0.6rem;
            border-radius: 20px;
            font-size: 0.75rem;
            font-weight: 600;
            display: flex;
            align-items: center;
            gap: 0.4rem;
            border: 1px solid rgba(168, 85, 247, 0.25);
        }

        .badge-sniffing::before {
            content: '';
            display: inline-block;
            width: 7px;
            height: 7px;
            background-color: #c084fc;
            border-radius: 50%;
            box-shadow: 0 0 8px #c084fc;
            animation: pulse-purple 1.5s infinite alternate;
            margin-right: 4px;
        }

        @keyframes pulse-purple {
            from { transform: scale(0.8); opacity: 0.5; box-shadow: 0 0 4px #c084fc; }
            to { transform: scale(1.2); opacity: 1; box-shadow: 0 0 10px #c084fc; }
        }

        @keyframes spin {
            0% { transform: rotate(0deg); }
            100% { transform: rotate(360deg); }
        }

        @keyframes progress-shimmer {
            0% { background-position: 100% 50%; }
            100% { background-position: 0% 50%; }
        }

        /* Navigation Bar */
        .nav-bar {
            display: flex;
            gap: 0.5rem;
            margin-bottom: 0.6rem;
            border-bottom: 1px solid var(--card-border);
            padding-bottom: 0.3rem;
            width: 100%;
        }

        .nav-item {
            background: transparent;
            color: var(--text-muted);
            border: none;
            padding: 0.35rem 0.8rem;
            font-size: 0.85rem;
            font-weight: 600;
            font-family: 'Outfit', sans-serif;
            cursor: pointer;
            border-radius: 6px;
            transition: all 0.3s ease;
            position: relative;
        }

        .nav-item.active {
            color: var(--primary);
            background: rgba(0, 210, 255, 0.08);
        }

        .nav-item.active::after {
            content: '';
            position: absolute;
            bottom: -0.3rem;
            left: 0;
            width: 100%;
            height: 2px;
            background: linear-gradient(90deg, var(--primary), var(--secondary));
            box-shadow: 0 0 8px var(--primary);
        }

        .nav-item:hover:not(.active) {
            color: var(--text-color);
            background: rgba(255, 255, 255, 0.03);
        }

        .page-content {
            width: 100%;
            animation: fadeIn 0.35s ease-out;
        }

        @keyframes fadeIn {
            from { opacity: 0; transform: translateY(8px); }
            to { opacity: 1; transform: translateY(0); }
        }

        .grid-stats {
            display: grid;
            grid-template-columns: repeat(5, 1fr);
            gap: 0.5rem;
            margin-bottom: 0.6rem;
        }

        @media (max-width: 992px) {
            .grid-stats {
                grid-template-columns: repeat(2, 1fr);
            }
        }

        @media (max-width: 576px) {
            .grid-stats {
                grid-template-columns: 1fr;
            }
        }

        .grid-weather {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(100px, 1fr));
            gap: 0.4rem;
            margin-bottom: 0.5rem;
        }

        .card-weather {
            background: rgba(255, 255, 255, 0.015);
            border: 1px solid rgba(255, 255, 255, 0.04);
            border-radius: 8px;
            padding: 0.4rem 0.35rem;
            text-align: center;
            transition: transform 0.2s ease, border-color 0.2s ease;
        }

        .card-weather:hover {
            border-color: rgba(0, 210, 255, 0.2);
            background: rgba(255, 255, 255, 0.03);
            transform: translateY(-2px);
        }

        .card-weather .w-title {
            font-size: 0.6rem;
            font-weight: 700;
            color: var(--text-muted);
            text-transform: uppercase;
            letter-spacing: 0.4px;
            margin-bottom: 0.15rem;
        }

        .card-weather .w-val {
            font-size: 0.92rem;
            font-weight: 600;
            color: #00d2ff;
        }

        .card-weather .w-sub {
            font-size: 0.58rem;
            color: var(--text-muted);
            margin-top: 0.1rem;
        }

        .card-stat {
            background: var(--card-bg);
            border: 1px solid var(--card-border);
            backdrop-filter: var(--glass-blur);
            border-radius: 10px;
            padding: 0.5rem 0.75rem;
            transition: transform 0.3s ease, border-color 0.3s ease;
            min-width: 0;
        }

        .card-stat:hover {
            transform: translateY(-2px);
            border-color: rgba(0, 210, 255, 0.25);
        }

        .card-stat h3 {
            font-size: 0.68rem;
            color: var(--text-muted);
            text-transform: uppercase;
            letter-spacing: 0.4px;
            margin-bottom: 0.2rem;
        }

        .card-stat .value {
            font-size: 1rem;
            font-weight: 600;
            color: var(--text-color);
            overflow: hidden;
            text-overflow: ellipsis;
            white-space: nowrap;
        }

        .card-stat .sub-text {
            font-size: 0.68rem;
            color: var(--text-muted);
            margin-top: 0.15rem;
            overflow: hidden;
            text-overflow: ellipsis;
            white-space: nowrap;
        }

        .status-on {
            color: var(--accent-green) !important;
        }

        .status-off {
            color: var(--accent-red) !important;
        }

        .layout-main {
            display: grid;
            grid-template-columns: 1fr;
            gap: 0.6rem;
            width: 100%;
        }

        .section-panel {
            background: var(--card-bg);
            border: 1px solid var(--card-border);
            backdrop-filter: var(--glass-blur);
            border-radius: 12px;
            padding: 0.75rem 0.9rem;
            margin-bottom: 0.6rem;
        }

        .section-panel h2 {
            font-size: 1.1rem;
            font-weight: 600;
            margin-bottom: 0.4rem;
            border-bottom: 1px solid var(--card-border);
            padding-bottom: 0.25rem;
        }

        /* Live Chart uPlot Layout Customization: Legend on the LEFT, full width chart */
        #chart-container .uplot {
            display: flex !important;
            flex-direction: row-reverse !important;
            align-items: center !important;
            justify-content: flex-end !important;
            gap: 0.75rem !important;
            width: 100% !important;
        }

        #chart-container .uplot .u-legend {
            display: flex !important;
            flex-direction: column !important;
            gap: 0.2rem !important;
            min-width: 230px !important;
            max-width: 270px !important;
            padding: 0.5rem 0.75rem !important;
            background: rgba(0, 0, 0, 0.3) !important;
            border: 1px solid var(--card-border) !important;
            border-radius: 8px !important;
            font-size: 0.75rem !important;
            text-align: left !important;
            flex-shrink: 0 !important;
            white-space: nowrap !important;
        }

        #chart-container .uplot .u-legend * {
            white-space: nowrap !important;
            word-break: keep-all !important;
        }

        #chart-container .uplot .u-legend tr, #chart-container .uplot .u-legend .u-series {
            display: flex !important;
            align-items: center !important;
            justify-content: space-between !important;
            gap: 0.5rem !important;
            padding: 0.1rem 0 !important;
            white-space: nowrap !important;
        }

        #chart-container .uplot .u-legend .u-marker {
            width: 10px !important;
            height: 10px !important;
            border-radius: 2px !important;
            display: inline-block !important;
            flex-shrink: 0 !important;
        }

        #chart-container .uplot .u-wrap {
            flex: 1 !important;
            width: 100% !important;
        }

        /* History Grid Layout, Summary Badges & Non-Wrapping Tabular Legends */
        #history-container .grid-charts {
            display: grid !important;
            grid-template-columns: repeat(2, minmax(0, 1fr)) !important;
            gap: 1rem !important;
            width: 100% !important;
        }

        #history-container .grid-charts > .card {
            min-width: 0 !important;
            overflow: hidden !important;
        }

        @media (max-width: 850px) {
            #history-container .grid-charts {
                grid-template-columns: minmax(0, 1fr) !important;
            }
        }

        .hist-card-header {
            display: flex;
            flex-direction: column;
            gap: 0.4rem;
            margin-bottom: 0.55rem;
        }

        .hist-card-title-row {
            display: flex;
            align-items: center;
            justify-content: space-between;
            gap: 0.5rem;
            flex-wrap: wrap;
        }

        .hist-card-title-row h3 {
            margin: 0;
            font-size: 0.98rem;
            font-weight: 700;
            letter-spacing: -0.01em;
        }

        .hist-stats-strip {
            display: flex;
            flex-wrap: wrap;
            gap: 0.35rem 0.5rem;
            align-items: center;
            min-height: 22px;
        }

        .hist-stat-pill {
            display: inline-flex;
            align-items: center;
            gap: 0.3rem;
            font-size: 0.69rem;
            line-height: 1.2;
            padding: 0.16rem 0.45rem;
            border-radius: 6px;
            background: rgba(255, 255, 255, 0.035);
            border: 1px solid rgba(255, 255, 255, 0.07);
            color: var(--text-muted);
            font-variant-numeric: tabular-nums;
            white-space: nowrap;
        }

        .hist-stat-pill .dot {
            width: 7px;
            height: 7px;
            border-radius: 50%;
            flex-shrink: 0;
        }

        .hist-stat-pill strong {
            color: var(--text);
            font-weight: 600;
        }

        #history-container .uplot .u-legend {
            display: flex !important;
            flex-wrap: nowrap !important;
            align-items: center !important;
            justify-content: flex-start !important;
            gap: 0.45rem !important;
            font-size: 0.72rem !important;
            font-variant-numeric: tabular-nums !important;
            padding: 0.2rem 0.3rem !important;
            margin: 0.25rem 0 0 0 !important;
            height: 26px !important;
            min-height: 26px !important;
            max-height: 26px !important;
            white-space: nowrap !important;
            overflow-x: auto !important;
            overflow-y: hidden !important;
            scrollbar-width: none !important;
        }

        #history-container .uplot .u-legend::-webkit-scrollbar {
            display: none !important;
        }

        #history-container .uplot .u-legend .u-series {
            display: inline-flex !important;
            align-items: center !important;
            flex-shrink: 0 !important;
            padding: 0 !important;
            margin: 0 !important;
        }

        #history-container .uplot .u-legend .u-series th,
        #history-container .uplot .u-legend .u-series td {
            padding: 0 2px !important;
            white-space: nowrap !important;
        }

        #history-container .uplot .u-legend .u-marker {
            width: 8px !important;
            height: 8px !important;
            border-radius: 2px !important;
            margin-right: 3px !important;
        }

        #history-container .uplot .u-legend .u-value {
            display: inline-block !important;
            min-width: 5.2ch !important;
            text-align: left !important;
            font-variant-numeric: tabular-nums !important;
            color: var(--text) !important;
            font-weight: 600 !important;
        }

        #history-container .uplot .u-legend .u-series:first-child .u-value {
            min-width: 9.6ch !important;
            color: var(--primary) !important;
        }

        .table-packets {
            width: 100%;
            border-collapse: collapse;
            font-size: 0.9rem;
        }

        .table-packets th, .table-packets td {
            padding: 1rem;
            text-align: left;
            border-bottom: 1px solid var(--card-border);
        }

        .table-packets th {
            color: var(--text-muted);
            font-weight: 600;
        }

        .table-packets tr {
            transition: background 0.2s ease;
        }

        .table-packets tr:hover {
            background: rgba(255, 255, 255, 0.015);
        }

        .table-packets tr td:first-child {
            border-left: 4px solid transparent;
            transition: border-left-color 0.2s ease;
        }

        .table-packets tr.cat-weatherxm td:first-child {
            border-left: 4px solid #17a2b8;
        }

        .table-packets tr.cat-meshtastic td:first-child {
            border-left: 4px solid #5fd589;
        }

        .table-packets tr.cat-meshcore td:first-child {
            border-left: 4px solid #c084fc;
        }

        .table-packets tr.cat-other td:first-child {
            border-left: 4px solid #f59e0b;
        }

        .table-packets tr.cat-weatherxm:hover {
            background: rgba(23, 162, 184, 0.04);
        }

        .table-packets tr.cat-meshtastic:hover {
            background: rgba(95, 213, 137, 0.04);
        }

        .table-packets tr.cat-meshcore:hover {
            background: rgba(168, 85, 247, 0.04);
        }

        .table-packets tr.cat-other:hover {
            background: rgba(245, 158, 11, 0.04);
        }

        .font-mono {
            font-family: monospace;
            word-break: break-all;
            color: var(--secondary);
        }

        .upload-area {
            border: 2px dashed rgba(255, 255, 255, 0.15);
            border-radius: 12px;
            padding: 3.5rem 1.5rem;
            text-align: center;
            cursor: pointer;
            transition: border-color 0.3s ease, background 0.3s ease;
            position: relative;
        }

        .upload-area:hover {
            border-color: var(--primary);
            background: rgba(0, 210, 255, 0.02);
        }

        .upload-area input[type="file"] {
            position: absolute;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            opacity: 0;
            cursor: pointer;
        }

        .upload-btn {
            background: linear-gradient(135deg, var(--primary), var(--secondary));
            color: var(--bg-color);
            border: none;
            padding: 0.8rem 2rem;
            border-radius: 30px;
            font-weight: 600;
            cursor: pointer;
            margin-top: 1.2rem;
            transition: transform 0.2s ease, box-shadow 0.2s ease;
        }

        .upload-btn:hover {
            transform: scale(1.02);
            box-shadow: 0 0 15px rgba(0, 210, 255, 0.4);
        }

        .progress-bar-container {
            width: 100%;
            background: rgba(255, 255, 255, 0.1);
            height: 10px;
            border-radius: 5px;
            margin-top: 1.5rem;
            display: none;
            overflow: hidden;
        }

        .progress-bar {
            width: 0%;
            height: 100%;
            background: linear-gradient(90deg, var(--primary), var(--accent-green));
            transition: width 0.1s ease;
        }

        #ota-status {
            font-size: 0.9rem;
            margin-top: 0.8rem;
            font-weight: 600;
        }

        /* Switch/Toggle Slider */
        .switch {
            position: relative;
            display: inline-block;
            width: 48px;
            height: 24px;
        }

        .switch input {
            opacity: 0;
            width: 0;
            height: 0;
        }

        .slider {
            position: absolute;
            cursor: pointer;
            top: 0;
            left: 0;
            right: 0;
            bottom: 0;
            background-color: rgba(255, 255, 255, 0.1);
            transition: .3s;
            border: 1px solid var(--card-border);
        }

        .slider:before {
            position: absolute;
            content: "";
            height: 16px;
            width: 16px;
            left: 3px;
            bottom: 3px;
            background-color: var(--text-color);
            transition: .3s;
        }

        input:checked + .slider {
            background-color: rgba(0, 245, 160, 0.2);
            border-color: var(--accent-green);
        }

        input:checked + .slider:before {
            background-color: var(--accent-green);
            transform: translateX(24px);
        }

        .slider.round {
            border-radius: 24px;
        }

        .slider.round:before {
            border-radius: 50%;
        }

        /* Profile Buttons */
        .btn-group {
            display: flex;
            background: rgba(255, 255, 255, 0.05);
            border: 1px solid var(--card-border);
            border-radius: 30px;
            overflow: hidden;
            padding: 2px;
        }

        .profile-btn {
            background: transparent;
            color: var(--text-muted);
            border: none;
            padding: 0.4rem 1.2rem;
            font-size: 0.85rem;
            font-weight: 600;
            cursor: pointer;
            border-radius: 30px;
            transition: all 0.3s ease;
        }

        .profile-btn.active {
            background: linear-gradient(135deg, var(--primary), var(--secondary));
            color: var(--bg-color);
            box-shadow: 0 0 10px rgba(0, 210, 255, 0.3);
        }

        .profile-btn:hover:not(.active) {
            color: var(--text-color);
            background: rgba(255, 255, 255, 0.05);
        }

        /* Tri-state Power Segmented Control */
        .tri-pwr-group {
            display: inline-flex;
            background: rgba(0, 0, 0, 0.35);
            border: 1px solid var(--card-border);
            border-radius: 8px;
            padding: 3px;
            gap: 3px;
        }

        .tri-pwr-btn {
            background: transparent;
            color: var(--text-muted);
            border: none;
            padding: 0.3rem 0.75rem;
            font-size: 0.78rem;
            font-weight: 600;
            cursor: pointer;
            border-radius: 6px;
            transition: all 0.2s ease;
            white-space: nowrap;
        }

        .tri-pwr-btn.active {
            background: linear-gradient(135deg, var(--primary), var(--secondary));
            color: var(--bg-color);
            font-weight: 700;
            box-shadow: 0 0 8px rgba(0, 210, 255, 0.35);
        }

        .tri-pwr-btn:hover:not(.active) {
            color: var(--text-color);
            background: rgba(255, 255, 255, 0.06);
        }

        /* Decoded JSON Container styling */
        .decoded-container {
            margin-top: 0.5rem;
            padding: 0.6rem;
            background: rgba(0, 0, 0, 0.2);
            border-radius: 8px;
            border: 1px solid var(--card-border);
            font-size: 0.8rem;
            color: var(--text-color);
            max-width: 380px;
            word-wrap: break-word;
            overflow-wrap: break-word;
        }

        .decoded-item {
            display: inline-block;
            margin-right: 1rem;
            margin-bottom: 0.2rem;
        }

        .decoded-key {
            color: var(--text-muted);
            font-weight: 600;
        }

        .decoded-val {
            color: var(--accent-green);
            font-family: monospace;
            font-weight: bold;
            word-break: break-all;
        }

        /* Responsive CSS for Small Screens / Smartphones */
        @media (max-width: 992px) {
            body {
                padding: 1rem 0.75rem;
            }

            header {
                flex-direction: column;
                align-items: flex-start;
                gap: 0.8rem;
                padding-bottom: 0.8rem;
            }

            .badge-live {
                align-self: flex-start;
            }

            .section-panel {
                padding: 1.2rem;
                border-radius: 16px;
            }

            /* Convert table to high-end mobile-friendly cards */
            .table-packets, 
            .table-packets thead, 
            .table-packets tbody, 
            .table-packets th, 
            .table-packets td, 
            .table-packets tr {
                display: block;
            }

            .table-packets thead {
                display: none; /* Hide traditional horizontal table headers */
            }

            .table-packets tr {
                background: rgba(255, 255, 255, 0.015);
                border: 1px solid var(--card-border);
                border-radius: 14px;
                padding: 1.1rem;
                margin-bottom: 1rem;
                box-shadow: 0 4px 20px rgba(0, 0, 0, 0.15);
                transition: transform 0.2s ease, background 0.3s ease;
            }

            .table-packets tr:hover {
                background: rgba(255, 255, 255, 0.035);
                transform: translateY(-2px);
            }

            .table-packets tr.cat-weatherxm {
                border-left: 4px solid #17a2b8 !important;
            }

            .table-packets tr.cat-meshtastic {
                border-left: 4px solid #5fd589 !important;
            }

            .table-packets tr.cat-meshcore {
                border-left: 4px solid #c084fc !important;
            }

            .table-packets tr.cat-other {
                border-left: 4px solid #f59e0b !important;
            }

            .table-packets tr td:first-child {
                border-left: none !important;
            }

            .table-packets td {
                border-bottom: 1px solid rgba(255, 255, 255, 0.05);
                padding: 0.65rem 0 !important;
                display: flex;
                justify-content: space-between;
                align-items: center;
                font-size: 0.85rem;
                gap: 1rem;
                text-align: right;
            }

            /* Truncate/word-wrap the raw KISS payload value perfectly on the right */
            .table-packets td.font-mono .payload-val {
                word-break: break-all;
                font-size: 0.75rem;
                max-width: 60%;
                text-align: right;
                display: inline-block;
            }

            /* Format decoded parameters to occupy full-width on bottom of the card */
            .table-packets td:last-child {
                border-bottom: none;
                padding-bottom: 0 !important;
                flex-direction: column;
                align-items: stretch;
                gap: 0.5rem;
                text-align: left;
            }

            /* Responsive row labels using CSS ::before */
            .table-packets td:nth-of-type(1)::before { content: "Time"; }
            .table-packets td:nth-of-type(2)::before { content: "Signal (RSSI / SNR)"; }
            .table-packets td:nth-of-type(3)::before { content: "Station"; }
            .table-packets td:nth-of-type(4)::before { content: "Station ID"; }
            .table-packets td:nth-of-type(5)::before { content: "Proto Ver"; }
            .table-packets td:nth-of-type(6)::before { content: "Raw KISS Payload"; }
            
            .table-packets td:last-child::before {
                content: "Decoded Parameters";
                display: block;
                color: var(--text-muted);
                font-weight: 600;
                font-size: 0.75rem;
                text-transform: uppercase;
                letter-spacing: 0.05em;
                margin-bottom: 0.1rem;
                text-align: left;
            }

            .table-packets td::before {
                color: var(--text-muted);
                font-weight: 600;
                font-size: 0.75rem;
                text-transform: uppercase;
                letter-spacing: 0.05em;
                flex-shrink: 0;
                text-align: left;
            }

            .table-packets td > * {
                text-align: right;
            }
            
            .decoded-container {
                margin-top: 0.2rem;
                padding: 0.5rem;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <div>
                <h1>Unknown Device Name</h1>
                <p style="color: var(--text-muted); font-size: 0.9rem;">WeatherXM Gateway WG1200 - Web Console <span id="fw-version" style="opacity: 0.7; font-weight: 400; margin-left: 0.5rem; border-left: 1px solid var(--card-border); padding-left: 0.5rem;">v-</span></p>
            </div>
            <div style="display: flex; gap: 0.5rem; align-items: center;">
                <button onclick="openAiPromptModal()" style="background: rgba(0, 245, 160, 0.12); color: #00f5a0; border: 1px solid rgba(0, 245, 160, 0.3); padding: 0.25rem 0.65rem; border-radius: 20px; font-size: 0.75rem; font-weight: 600; display: flex; align-items: center; gap: 0.35rem; cursor: pointer; transition: all 0.2s;" onmouseover="this.style.background='rgba(0,245,160,0.22)'" onmouseout="this.style.background='rgba(0,245,160,0.12)'" title="Generate AI prompts & Home Assistant configuration">
                    <span>🤖</span> AI / Home Assistant
                </button>
                <div id="badge-sniffing" class="badge-sniffing" style="display: none;">Sniffer Active</div>
                <div id="badge-live" class="badge-live state-healthy" style="display: none;" title="System Normal"></div>
            </div>
        </header>

        <nav class="nav-bar">
            <button id="tab-dashboard" class="nav-item active" onclick="switchTab('dashboard')">Dashboard</button>
            <button id="tab-history" class="nav-item" onclick="switchTab('history')">History</button>
            <button id="tab-settings" class="nav-item" onclick="switchTab('settings')">Settings</button>
            <button id="tab-docs" class="nav-item" onclick="switchTab('docs')">API Docs</button>
        </nav>

        <!-- Global Unusable SD Card Warning Banner -->
        <div id="sd-unusable-banner" style="display: none; margin-bottom: 1.25rem; padding: 0.85rem 1.1rem; border-radius: 12px; background: rgba(255, 65, 108, 0.1); border: 1px solid rgba(255, 65, 108, 0.35); align-items: center; justify-content: space-between; flex-wrap: wrap; gap: 0.8rem;">
            <div style="display: flex; align-items: flex-start; gap: 0.65rem; flex: 1 1 280px;">
                <span style="font-size: 1.2rem; line-height: 1.2;">⚠️</span>
                <div>
                    <div id="sd-unusable-banner-title" style="font-weight: 700; font-size: 0.88rem; color: #ff416c; margin-bottom: 0.15rem;">microSD Card Cannot Be Used (Format Required)</div>
                    <div id="sd-unusable-banner-desc" style="font-size: 0.78rem; color: var(--text-muted); line-height: 1.35;">A microSD card is inserted, but it is unformatted or uses an unsupported filesystem. Gateway operation continues normally without SD logging.</div>
                </div>
            </div>
            <div style="display: flex; gap: 0.5rem; flex-wrap: wrap;">
                <button onclick="switchTab('history')" style="background: rgba(255, 255, 255, 0.08); border: 1px solid var(--card-border); color: #fff; font-weight: 600; font-size: 0.75rem; padding: 0.4rem 0.85rem; border-radius: 8px; cursor: pointer;">Inspect Card</button>
                <button onclick="switchTab('history'); formatSdCard();" style="background: linear-gradient(135deg, #ff416c, #ff4b2b); border: none; color: #fff; font-weight: 700; font-size: 0.75rem; padding: 0.4rem 0.9rem; border-radius: 8px; cursor: pointer;">Format as FAT32</button>
            </div>
        </div>

        <!-- Global Ejected SD Card Notice Banner -->
        <div id="sd-ejected-banner" style="display: none; margin-bottom: 1.25rem; padding: 0.85rem 1.1rem; border-radius: 12px; background: rgba(255, 177, 0, 0.1); border: 1px solid rgba(255, 177, 0, 0.4); align-items: center; justify-content: space-between; flex-wrap: wrap; gap: 0.8rem;">
            <div style="display: flex; align-items: flex-start; gap: 0.65rem; flex: 1 1 280px;">
                <span style="font-size: 1.2rem; line-height: 1.2;">⏏️</span>
                <div>
                    <div id="sd-ejected-banner-title" style="font-weight: 700; font-size: 0.88rem; color: #ffb100; margin-bottom: 0.15rem;">microSD Card Ready to Eject (Unmounted)</div>
                    <div id="sd-ejected-banner-desc" style="font-size: 0.78rem; color: var(--text-muted); line-height: 1.35;">The microSD card is safely unmounted and logging is paused. Remove the card now, or click "Mount SD Card" to resume logging.</div>
                </div>
            </div>
            <div style="display: flex; gap: 0.5rem; flex-wrap: wrap;">
                <button onclick="switchTab('history')" style="background: rgba(255, 255, 255, 0.08); border: 1px solid var(--card-border); color: #fff; font-weight: 600; font-size: 0.75rem; padding: 0.4rem 0.85rem; border-radius: 8px; cursor: pointer;">Open SD Panel</button>
                <button onclick="mountSdCard()" style="background: linear-gradient(135deg, #00f5a0, #00b4db); border: none; color: #0b0f19; font-weight: 700; font-size: 0.75rem; padding: 0.4rem 0.9rem; border-radius: 8px; cursor: pointer;">Mount SD Card</button>
            </div>
        </div>

        <!-- Dashboard View -->
        <div id="page-dashboard" class="page-content">
            <div class="grid-stats">
                <div class="card-stat">
                    <h3>Wi-Fi</h3>
                    <div class="value" id="wifi-val">-</div>
                    <div class="sub-text" id="wifi-ssid-val">Connecting...</div>
                </div>
                <div class="card-stat">
                    <h3>WeatherXM MQTT</h3>
                    <div class="value" id="mqtt-val">-</div>
                    <div class="sub-text" id="mqtt-stats">Tx: - · Err: -</div>
                </div>
                <div class="card-stat">
                    <h3>LoRa Radio</h3>
                    <div class="value status-on" id="lora-link-val">-</div>
                    <div class="sub-text" id="lora-link-sub">Reception: -</div>
                </div>
                <div class="card-stat">
                    <h3>Station Link</h3>
                    <div class="value" id="link-val">-</div>
                    <div class="sub-text" id="link-sub">-</div>
                    <div class="sub-text" id="paired-model-chan" style="display: none;"></div>
                </div>
                <div class="card-stat">
                    <h3>Station Ownership</h3>
                    <div class="value" id="ownership-val">-</div>
                    <div class="sub-text" id="ownership-sub">Loading ownership state...</div>
                    <button id="ownership-verify-btn" onclick="requestGpsFix(true)" style="display:none; margin-top:0.35rem; background:rgba(255,183,77,0.12); border:1px solid rgba(255,183,77,0.35); color:#ffb74d; font-weight:600; font-size:0.7rem; padding:0.2rem 0.5rem; border-radius:5px; cursor:pointer;">Verify with GPS response</button>
                    <div id="operation-mode-val" style="display:none;"></div>
                    <div id="operation-mode-sub" style="display:none;"></div>
                </div>
            </div>

            <div class="layout-main">
                <!-- Weather Panels Row -->
                <div class="section-panel">
                    <h3 style="font-size: 0.9rem; margin-top: 0; margin-bottom: 0.5rem; font-weight: 600; color: var(--text-color); display: flex; align-items: center; gap: 0.4rem; border: none; padding-bottom: 0;">
                        <span style="display: inline-block; width: 7px; height: 7px; background: #00d2ff; border-radius: 50%; box-shadow: 0 0 6px #00d2ff;"></span>
                        Latest Weather Conditions
                    </h3>
                    <div class="grid-weather">
                        <div class="card-weather">
                            <div class="w-title">Temperature</div>
                            <div class="w-val" id="w-temp">-</div>
                        </div>
                        <div class="card-weather">
                            <div class="w-title">Feels Like</div>
                            <div class="w-val" id="w-feels-like">-</div>
                        </div>
                        <div class="card-weather">
                            <div class="w-title">Humidity</div>
                            <div class="w-val" id="w-humidity">-</div>
                        </div>
                        <div class="card-weather">
                            <div class="w-title">Wind Speed</div>
                            <div class="w-val" id="w-wind-speed">-</div>
                            <div class="w-sub" id="w-wind-dir">-</div>
                        </div>
                        <div class="card-weather">
                            <div class="w-title">Wind Gust</div>
                            <div class="w-val" id="w-wind-gust">-</div>
                        </div>
                        <div class="card-weather">
                            <div class="w-title">Precip Rate</div>
                            <div class="w-val" id="w-precip-rate">-</div>
                            <div class="w-sub" id="w-precip-daily">-</div>
                        </div>
                        <div class="card-weather">
                            <div class="w-title">Pressure</div>
                            <div class="w-val" id="w-pressure">-</div>
                        </div>
                        <div class="card-weather">
                            <div class="w-title">Solar / UV</div>
                            <div class="w-val" id="w-irradiance">-</div>
                            <div class="w-sub" id="w-uv">-</div>
                        </div>
                    </div>
                </div>

                <div class="section-panel">
                    <h3 style="font-size: 0.9rem; margin-top: 0; margin-bottom: 0.25rem; font-weight: 600; color: var(--text-color); display: flex; align-items: center; gap: 0.4rem; border: none; padding-bottom: 0;">
                        <span style="display: inline-block; width: 7px; height: 7px; background: #00f5a0; border-radius: 50%; box-shadow: 0 0 6px #00f5a0;"></span>
                        Live Weather Trends
                    </h3>
                    <p style="color: var(--text-muted); margin-bottom: 0.5rem; font-size: 0.75rem;">Weather station sensor values of last 15 min.</p>
                    <div id="chart-container" style="width: 100%; display: flex; justify-content: center; overflow-x: auto; padding: 0.2rem 0;"></div>
                </div>
            </div>
        </div>

        <!-- Settings View -->
        <div id="page-settings" class="page-content" style="display: none;">
            <!-- Row 1: System Connections (Left) & Wi-Fi OTA Firmware Update / Security (Right) -->
            <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(340px, 1fr)); gap: 1.5rem; margin-bottom: 1.5rem; align-items: start;">
                <!-- General Controls Card -->
                <div class="section-panel" style="margin: 0; padding: 1.5rem;">
                    <h2 style="font-size: 1.2rem; margin-bottom: 0.5rem; border-bottom: none; padding-bottom: 0;">System Connections</h2>
                    <p style="color: var(--text-muted); font-size: 0.85rem; margin-bottom: 1.25rem;">Configure real-time transceiver radios and background network services.</p>
                    
                    <!-- Wi-Fi Control -->
                    <div style="display: flex; align-items: center; justify-content: space-between; padding: 0.8rem; background: rgba(255,255,255,0.02); border: 1px solid var(--card-border); border-radius: 12px; margin-bottom: 0.75rem;">
                        <div>
                            <div style="font-weight: 600; font-size: 0.9rem;">Wi-Fi Radio</div>
                            <div style="font-size: 0.75rem; color: var(--text-muted);">Enable/Disable local network transceiver</div>
                        </div>
                        <label class="switch">
                            <input type="checkbox" id="wifi-toggle-btn" checked onchange="toggleWifiRadio(this.checked)">
                            <span class="slider round"></span>
                        </label>
                    </div>

                    <!-- MQTT Control (WeatherXM Cloud) -->
                    <div style="display: flex; align-items: center; justify-content: space-between; padding: 0.8rem; background: rgba(255,255,255,0.02); border: 1px solid var(--card-border); border-radius: 12px; margin-bottom: 0.75rem;">
                        <div>
                            <div style="font-weight: 600; font-size: 0.9rem;">WeatherXM Cloud</div>
                            <div style="font-size: 0.75rem; color: var(--text-muted);" id="mqtt-toggle-desc">Enable/Disable the upload of weather data to WeatherXM cloud infrastructure</div>
                        </div>
                        <label class="switch" id="mqtt-switch-label">
                            <input type="checkbox" id="mqtt-toggle-btn" onchange="toggleMqttBtn(this.checked)">
                            <span class="slider round"></span>
                        </label>
                    </div>

                    <!-- WeatherXM LoRa P2P Station Link Control -->
                    <div style="display: flex; align-items: center; justify-content: space-between; padding: 0.8rem; background: rgba(255,255,255,0.02); border: 1px solid var(--card-border); border-radius: 12px; margin-bottom: 0.75rem;">
                        <div>
                            <div style="font-weight: 600; font-size: 0.9rem;">WeatherXM LoRa P2P Station Link</div>
                            <div style="font-size: 0.75rem; color: var(--text-muted);">Enable/Disable local weather station data communications via LoRa P2P protocol</div>
                        </div>
                        <label class="switch">
                            <input type="checkbox" id="wxm-p2p-toggle-btn" checked onchange="toggleWxmP2P(this.checked)">
                            <span class="slider round"></span>
                        </label>
                    </div>

                    <!-- Station TX Power Control (Tri-state: Low / Auto / High) -->
                    <div style="display: flex; align-items: center; justify-content: space-between; padding: 0.8rem; background: rgba(255,255,255,0.02); border: 1px solid var(--card-border); border-radius: 12px; gap: 1rem;">
                        <div>
                            <div style="font-weight: 600; font-size: 0.9rem;">Station LoRa TX Power</div>
                            <div style="font-size: 0.75rem; color: var(--text-muted);">Auto adapts power based on gateway distance / link quality; Low (2 dBm) for workbench; High (22 dBm) for max range, Auto for adapting to minimum power with no packet loss.</div>
                        </div>
                        <div class="tri-pwr-group" id="sta-tx-pwr-group">
                            <button type="button" class="tri-pwr-btn" id="sta-pwr-btn-low" onclick="setStationPowerMode('low')">Low</button>
                            <button type="button" class="tri-pwr-btn active" id="sta-pwr-btn-auto" onclick="setStationPowerMode('auto')">Auto</button>
                            <button type="button" class="tri-pwr-btn" id="sta-pwr-btn-high" onclick="setStationPowerMode('high')">High</button>
                        </div>
                    </div>
                </div>

                <!-- Wi-Fi OTA Firmware Update Card & Security -->
                <div class="section-panel" style="margin: 0; padding: 1.5rem;">
                    <h2 style="font-size: 1.2rem; margin-bottom: 0.35rem; border-bottom: none; padding-bottom: 0;">Wi-Fi OTA Firmware Update</h2>
                    <p style="color: var(--text-muted); font-size: 0.82rem; margin-bottom: 0.75rem;">Upload a firmware binary file (`.bin`) to upgrade the gateway.</p>
                    <div class="upload-area" id="drop-zone" style="padding: 0.6rem 0.85rem; border-radius: 8px; display: flex; align-items: center; justify-content: space-between; gap: 0.75rem;">
                        <div style="text-align: left;">
                            <div style="font-weight: 600; font-size: 0.82rem;">Drag &amp; Drop binary here</div>
                            <div style="font-size: 0.7rem; color: var(--text-muted);">or click to browse (.bin)</div>
                        </div>
                        <input type="file" id="ota-file" accept=".bin">
                        <button class="upload-btn" style="font-size: 0.72rem; padding: 0.3rem 0.75rem; border-radius: 6px; white-space: nowrap; pointer-events: none;">Select BIN</button>
                    </div>
                    <div class="progress-bar-container" id="progress-container" style="margin-top: 0.5rem;">
                        <div class="progress-bar" id="progress-fill"></div>
                    </div>
                    <div id="ota-status" style="font-size: 0.72rem; margin-top: 0.25rem;">Ready</div>

                    <!-- Display Units (Synchronized with LCD Screen) -->
                    <div style="margin-top: 1.05rem; padding-top: 0.9rem; border-top: 1px solid rgba(255,255,255,0.08);">
                        <div style="display: flex; align-items: center; justify-content: space-between; gap: 0.5rem; margin-bottom: 0.3rem;">
                            <h3 style="font-size: 0.95rem; margin: 0; display: flex; align-items: center; gap: 0.45rem;">
                                Display Units
                                <span class="badge-slot" style="background: rgba(0, 210, 255, 0.14); color: #00d2ff; font-size: 0.62rem; padding: 0.12rem 0.4rem; border-radius: 4px; font-weight: 700; text-transform: uppercase; margin-bottom: 0;">Web &amp; LCD Sync</span>
                            </h3>
                            <span id="unit-sync-status" style="font-size: 0.7rem; color: var(--text-muted);">Synced with LCD</span>
                        </div>
                        <p style="color: var(--text-muted); font-size: 0.74rem; margin-top: 0; margin-bottom: 0.65rem;">Changes apply immediately to both the Web UI (cards &amp; charts) and the gateway LCD screen.</p>
                        <div style="display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 0.55rem;">
                            <div style="display: flex; align-items: center; justify-content: space-between; gap: 0.5rem; padding: 0.5rem 0.65rem; background: rgba(255,255,255,0.02); border: 1px solid var(--card-border); border-radius: 8px;">
                                <label for="unit-temp-select" style="font-size: 0.78rem; font-weight: 600; color: var(--text-color);">Temperature</label>
                                <select id="unit-temp-select" onchange="setDisplayUnits()" style="background: rgba(30, 30, 30, 0.85); border: 1px solid rgba(255, 255, 255, 0.16); color: #00d2ff; font-size: 0.76rem; font-weight: 600; padding: 0.25rem 0.45rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="0">°C (Celsius)</option>
                                    <option value="1">°F (Fahrenheit)</option>
                                </select>
                            </div>
                            <div style="display: flex; align-items: center; justify-content: space-between; gap: 0.5rem; padding: 0.5rem 0.65rem; background: rgba(255,255,255,0.02); border: 1px solid var(--card-border); border-radius: 8px;">
                                <label for="unit-wind-select" style="font-size: 0.78rem; font-weight: 600; color: var(--text-color);">Wind Speed</label>
                                <select id="unit-wind-select" onchange="setDisplayUnits()" style="background: rgba(30, 30, 30, 0.85); border: 1px solid rgba(255, 255, 255, 0.16); color: #00d2ff; font-size: 0.76rem; font-weight: 600; padding: 0.25rem 0.45rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="0">m/s</option>
                                    <option value="1">km/h</option>
                                    <option value="2">mph</option>
                                    <option value="3">ft/s</option>
                                    <option value="4">bft</option>
                                    <option value="5">kn</option>
                                </select>
                            </div>
                            <div style="display: flex; align-items: center; justify-content: space-between; gap: 0.5rem; padding: 0.5rem 0.65rem; background: rgba(255,255,255,0.02); border: 1px solid var(--card-border); border-radius: 8px;">
                                <label for="unit-precip-select" style="font-size: 0.78rem; font-weight: 600; color: var(--text-color);">Precipitation</label>
                                <select id="unit-precip-select" onchange="setDisplayUnits()" style="background: rgba(30, 30, 30, 0.85); border: 1px solid rgba(255, 255, 255, 0.16); color: #00d2ff; font-size: 0.76rem; font-weight: 600; padding: 0.25rem 0.45rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="0">mm</option>
                                    <option value="1">in</option>
                                </select>
                            </div>
                            <div style="display: flex; align-items: center; justify-content: space-between; gap: 0.5rem; padding: 0.5rem 0.65rem; background: rgba(255,255,255,0.02); border: 1px solid var(--card-border); border-radius: 8px;">
                                <label for="unit-press-select" style="font-size: 0.78rem; font-weight: 600; color: var(--text-color);">Pressure</label>
                                <select id="unit-press-select" onchange="setDisplayUnits()" style="background: rgba(30, 30, 30, 0.85); border: 1px solid rgba(255, 255, 255, 0.16); color: #00d2ff; font-size: 0.76rem; font-weight: 600; padding: 0.25rem 0.45rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="0">hPa</option>
                                    <option value="1">mmHg</option>
                                    <option value="2">inHg</option>
                                </select>
                            </div>
                        </div>
                    </div>

                    <!-- Local API Security (Disabled / Coming Soon) -->
                    <div style="margin-top: 0.9rem; padding-top: 0.75rem; border-top: 1px solid rgba(255,255,255,0.06); opacity: 0.5;">
                        <h3 style="font-size: 0.9rem; margin-top: 0; margin-bottom: 0.35rem; display: flex; align-items: center; gap: 0.4rem;">
                            Local API Security
                            <span class="badge-slot" style="background: rgba(255,193,7,0.1); color: #ffc107; font-size: 0.62rem; padding: 0.1rem 0.35rem; border-radius: 4px; font-weight: 700; text-transform: uppercase; margin-bottom: 0;">Coming Soon</span>
                        </h3>
                        <p style="color: var(--text-muted); font-size: 0.72rem; margin-bottom: 0.6rem;">Configure access control for local endpoints and Web Console.</p>
                        
                        <!-- Toggle -->
                        <div style="display: flex; align-items: center; justify-content: space-between; padding: 0.45rem 0.65rem; background: rgba(255,255,255,0.01); border: 1px solid rgba(255,255,255,0.03); border-radius: 8px; margin-bottom: 0.5rem; opacity: 0.5;">
                            <div>
                                <div style="font-weight: 600; font-size: 0.8rem; color: rgba(255,255,255,0.7);">Local HTTP API Password</div>
                                <div style="font-size: 0.68rem; color: var(--text-muted);">Require credentials to authenticate requests</div>
                            </div>
                            <label class="switch" style="cursor: not-allowed; margin-left: auto;">
                                <input type="checkbox" id="local-auth-toggle-btn" disabled style="cursor: not-allowed;">
                                <span class="slider round" style="background-color: rgba(255,255,255,0.1);"></span>
                            </label>
                        </div>
                        
                        <!-- Password Input -->
                        <div style="opacity: 0.5;">
                            <label style="display: block; font-size: 0.72rem; font-weight: 600; color: rgba(255,255,255,0.7); margin-bottom: 0.25rem;">Define Password</label>
                            <div style="position: relative; display: flex; align-items: center; width: 100%;">
                                <input type="password" id="api-passwd-input" value="mypassword123" disabled style="background: rgba(0,0,0,0.2); border: 1px solid rgba(255,255,255,0.06); border-radius: 6px; color: rgba(255,255,255,0.4); padding: 0.35rem 2.2rem 0.35rem 0.65rem; font-size: 0.75rem; width: 100%; box-sizing: border-box; cursor: not-allowed;">
                                <button type="button" disabled style="position: absolute; right: 0.65rem; background: none; border: none; padding: 0; color: rgba(255,255,255,0.3); cursor: not-allowed; display: flex; align-items: center; justify-content: center; width: auto;" onclick="togglePasswordVisibility()">
                                    <svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" fill="currentColor" class="bi bi-eye" viewBox="0 0 16 16" style="pointer-events: none;">
                                        <path d="M16 8s-3-5.5-8-5.5S0 8 0 8s3 5.5 8 5.5S16 8 16 8M1.173 8a13 13 0 0 1 1.66-2.043C4.12 4.668 5.88 3.5 8 3.5s3.879 1.168 5.168 2.457A13 13 0 0 1 14.828 8q-.086.13-.195.288c-.335.48-.83 1.12-1.465 1.755C11.879 11.332 10.119 12.5 8 12.5s-3.879-1.168-5.168-2.457A13 13 0 0 1 1.172 8z"/>
                                        <path d="M8 5.5a2.5 2.5 0 1 0 0 5 2.5 2.5 0 0 0 0-5M4.5 8a3.5 3.5 0 1 1 7 0 3.5 3.5 0 0 1-7 0"/>
                                    </svg>
                                </button>
                            </div>
                        </div>
                    </div>
                </div>
            </div>

            <!-- Row 2: Meshtastic Network (Left) & MeshCore Network (Right) -->
            <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(340px, 1fr)); gap: 1.5rem; margin-bottom: 1.5rem; align-items: start;">
                <!-- Meshtastic Network Panel (Left) -->
                <div id="meshtastic-panel" class="section-panel" style="margin: 0; padding: 1rem 1.5rem; transition: padding 0.2s ease;">
                    <div id="meshtastic-header" onclick="toggleMeshtasticPanelManual()" style="display: flex; align-items: center; justify-content: space-between; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0; cursor: pointer; user-select: none;">
                        <h2 style="font-size: 1.2rem; margin: 0; border-bottom: none; padding-bottom: 0; display: flex; align-items: center; gap: 0.5rem;">
                            Meshtastic Network
                            <span class="badge-slot" style="background: rgba(255,193,7,0.1); color: #ffc107; font-size: 0.65rem; padding: 0.1rem 0.4rem; border-radius: 4px; font-weight: 700; text-transform: uppercase;">Beta</span>
                        </h2>
                        <div style="display: flex; align-items: center; gap: 0.5rem;">
                            <span id="meshtastic-header-badge" class="badge-slot" style="background: rgba(255, 255, 255, 0.08); color: var(--text-muted); font-size: 0.72rem; padding: 0.2rem 0.5rem; border-radius: 6px;">Disabled</span>
                            <span id="meshtastic-chevron" style="font-size: 0.85rem; color: var(--text-muted); transition: transform 0.2s ease; display: inline-block; padding: 0.2rem 0.4rem; transform: rotate(-90deg);">▼</span>
                        </div>
                    </div>
                    <div id="meshtastic-content" style="display: none; margin-top: 1rem;">
                    <p style="color: var(--text-muted); font-size: 0.85rem; margin-bottom: 1.25rem;">Broadcast open weather telemetry and node metrics across the Meshtastic mesh.</p>

                    <!-- Meshtastic RF & Modulation Presets Box -->
                    <div style="padding: 0.85rem; background: rgba(255,255,255,0.02); border: 1px solid var(--card-border); border-radius: 12px; margin-bottom: 0.85rem;">
                        <div style="font-size: 0.8rem; font-weight: 600; color: #00d2ff; text-transform: uppercase; letter-spacing: 0.05em; margin-bottom: 0.75rem; display: flex; align-items: center; justify-content: space-between;">
                            <span>Radio &amp; Modulation Presets</span>
                            <span style="font-size: 0.7rem; color: var(--text-muted); text-transform: none; font-weight: 400;">Gateway &amp; Station</span>
                        </div>

                        <div style="display: flex; align-items: center; justify-content: space-between; gap: 0.75rem; margin-bottom: 0.7rem;">
                            <div style="font-size: 0.8rem; color: var(--text-muted);">Region &amp; Preset</div>
                            <div style="display: flex; align-items: center; gap: 0.4rem;">
                                <select id="meshtastic-gw-region" onchange="setRegion(this.value)" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #00d2ff; font-size: 0.78rem; padding: 0.3rem 0.5rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="868">EU 868 MHz</option>
                                    <option value="915">US 915 MHz</option>
                                    <option value="923">AS 923 MHz</option>
                                    <option value="916">AU 915 MHz</option>
                                    <option value="865">IN 865 MHz</option>
                                </select>
                                <select id="meshtastic-preset" onchange="onMeshtasticPresetChange(this.value)" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #fff; font-size: 0.78rem; padding: 0.3rem 0.5rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="0">LongFast</option>
                                    <option value="1">LongSlow</option>
                                    <option value="2">MediumFast</option>
                                    <option value="3">MediumSlow</option>
                                    <option value="4">ShortFast</option>
                                    <option value="5">ShortSlow</option>
                                    <option value="6">LongModerate</option>
                                    <option value="7">ShortTurbo</option>
                                </select>
                            </div>
                        </div>

                        <!-- Custom RF Mode Toggle & Parameters -->
                        <div style="background: rgba(0,0,0,0.25); border: 1px solid rgba(255,255,255,0.08); border-radius: 8px; padding: 0.6rem 0.75rem; margin-bottom: 0.7rem;">
                            <div style="display: flex; align-items: center; justify-content: space-between; margin-bottom: 0.5rem;">
                                <div style="font-size: 0.75rem; font-weight: 600; color: #00d2ff;">Custom RF Parameters</div>
                                <label style="display: flex; align-items: center; gap: 0.35rem; font-size: 0.72rem; color: var(--text-muted); cursor: pointer;">
                                    <span>Custom RF</span>
                                    <input type="checkbox" id="meshtastic-custom-toggle" onchange="toggleMeshtasticCustomMode(this.checked)" style="accent-color: #00d2ff; cursor: pointer;">
                                </label>
                            </div>
                            <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 0.5rem; font-size: 0.75rem;">
                                <div>
                                    <label style="color: var(--text-muted); display: block; margin-bottom: 0.2rem;">Freq (Hz)</label>
                                    <input type="number" id="mesh-custom-freq" placeholder="e.g. 869525000" disabled style="width: 100%; box-sizing: border-box; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; font-size: 0.75rem; padding: 0.25rem 0.4rem; border-radius: 4px; opacity: 0.7;">
                                </div>
                                <div>
                                    <label style="color: var(--text-muted); display: block; margin-bottom: 0.2rem;">Bandwidth</label>
                                    <select id="mesh-custom-bw" disabled style="width: 100%; box-sizing: border-box; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; font-size: 0.75rem; padding: 0.25rem 0.4rem; border-radius: 4px; opacity: 0.7;">
                                        <option value="62">62.5 kHz</option>
                                        <option value="125">125 kHz</option>
                                        <option value="250" selected>250 kHz</option>
                                        <option value="500">500 kHz</option>
                                    </select>
                                </div>
                                <div>
                                    <label style="color: var(--text-muted); display: block; margin-bottom: 0.2rem;">Spreading Factor</label>
                                    <select id="mesh-custom-sf" disabled style="width: 100%; box-sizing: border-box; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; font-size: 0.75rem; padding: 0.25rem 0.4rem; border-radius: 4px; opacity: 0.7;">
                                        <option value="7">SF7</option>
                                        <option value="8">SF8</option>
                                        <option value="9">SF9</option>
                                        <option value="10">SF10</option>
                                        <option value="11" selected>SF11</option>
                                        <option value="12">SF12</option>
                                    </select>
                                </div>
                                <div>
                                    <label style="color: var(--text-muted); display: block; margin-bottom: 0.2rem;">Coding Rate</label>
                                    <select id="mesh-custom-cr" disabled style="width: 100%; box-sizing: border-box; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; font-size: 0.75rem; padding: 0.25rem 0.4rem; border-radius: 4px; opacity: 0.7;">
                                        <option value="5" selected>4/5</option>
                                        <option value="6">4/6</option>
                                        <option value="7">4/7</option>
                                        <option value="8">4/8</option>
                                    </select>
                                </div>
                                <div>
                                    <label style="color: var(--text-muted); display: block; margin-bottom: 0.2rem;">TX Power (dBm)</label>
                                    <input type="number" id="mesh-custom-tx-pwr" min="2" max="22" placeholder="2..22 dBm" disabled style="width: 100%; box-sizing: border-box; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; font-size: 0.75rem; padding: 0.25rem 0.4rem; border-radius: 4px; opacity: 0.7;">
                                </div>
                                <div style="display: flex; align-items: flex-end;">
                                    <button id="mesh-save-rf-btn" onclick="saveMeshtasticCustomRf()" disabled style="width: 100%; background: rgba(0, 210, 255, 0.2); border: 1px solid rgba(0, 210, 255, 0.4); color: #00d2ff; font-weight: 600; font-size: 0.72rem; padding: 0.3rem 0.5rem; border-radius: 4px; cursor: pointer; opacity: 0.5;">Save RF</button>
                                </div>
                            </div>
                        </div>

                        <div style="display: flex; align-items: center; justify-content: space-between; gap: 0.75rem; margin-bottom: 0.7rem;">
                            <div style="font-size: 0.8rem; color: var(--text-muted);">Hop limit</div>
                            <select id="meshtastic-hop-limit" onchange="setMeshtasticHopLimit(this.value)" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #fff; font-size: 0.78rem; padding: 0.3rem 0.5rem; border-radius: 6px; outline: none; cursor: pointer;">
                                <option value="1">1 hop</option>
                                <option value="2">2 hops</option>
                                <option value="3" selected>3 hops (default)</option>
                                <option value="4">4 hops</option>
                                <option value="5">5 hops</option>
                            </select>
                        </div>

                        <div style="display: flex; align-items: center; justify-content: space-between; gap: 0.75rem;">
                            <div>
                                <div style="font-weight: 600; font-size: 0.8rem;">High TX Power (22 dBm EU / 14 dBm other)</div>
                                <div style="font-size: 0.72rem; color: var(--text-muted);">Enable maximum regulatory power for this region</div>
                            </div>
                            <label class="switch">
                                <input type="checkbox" id="meshtastic-pwr-toggle-btn" onchange="setMeshtasticPower(this.checked)">
                                <span class="slider round"></span>
                            </label>
                        </div>
                    </div>

                    <!-- Gateway Meshtastic Sub-Card -->
                    <div style="padding: 0.85rem; background: rgba(255,255,255,0.02); border: 1px solid var(--card-border); border-radius: 12px; margin-bottom: 0.85rem;">
                        <div style="display: flex; align-items: center; justify-content: space-between;">
                            <div>
                                <div style="font-weight: 600; font-size: 0.9rem;">Gateway as Meshtastic Node</div>
                                <div style="font-size: 0.75rem; color: var(--text-muted);">Broadcast gateway weather telemetry across the mesh</div>
                            </div>
                            <label class="switch">
                                <input type="checkbox" id="meshtastic-toggle-btn" onchange="toggleMeshtastic(this.checked)">
                                <span class="slider round"></span>
                            </label>
                        </div>

                        <div id="meshtastic-detail" style="display: none; margin-top: 0.8rem; padding-top: 0.8rem; border-top: 1px solid var(--card-border);">
                            <div style="display: flex; align-items: center; justify-content: space-between; gap: 0.75rem; margin-bottom: 0.7rem;">
                                <div style="font-size: 0.8rem; color: var(--text-muted);">Gateway Broadcast interval</div>
                                <select id="meshtastic-interval" onchange="setMeshtasticInterval(this.value)" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #fff; font-size: 0.78rem; padding: 0.3rem 0.5rem; border-radius: 6px; outline: none; cursor: pointer;">

                                    <option value="300">5 minutes</option>
                                    <option value="600">10 minutes</option>
                                    <option value="900">15 minutes</option>
                                    <option value="1800">30 minutes</option>
                                    <option value="3600" selected>60 minutes (1 hour - Recommended)</option>
                                </select>
                            </div>

                            <div id="mesh-interval-flood-warning" style="display: none; margin-top: -0.2rem; margin-bottom: 0.7rem; font-size: 0.73rem; color: #ffc107; background: rgba(255,193,7,0.08); border: 1px solid rgba(255,193,7,0.3); border-left: 3px solid #ffc107; border-radius: 6px; padding: 0.55rem 0.7rem; line-height: 1.45;">
                                <div style="font-weight: 700; display: flex; align-items: center; gap: 0.35rem; margin-bottom: 0.25rem;">
                                    <span>⚠️</span>
                                    <span>High Broadcast Rate Warning (Mesh Flooding &amp; Airtime Saturation)</span>
                                </div>
                                <div style="color: #e0e0e0; font-size: 0.71rem;">
                                    Mesh networks (Meshtastic / MeshCore) rely on <strong>multi-hop broadcast flood routing</strong>, where every packet is repeated across neighboring nodes up to the hop limit. Transmitting telemetry faster than <strong>60 minutes (1 hour)</strong> rapidly saturates shared LoRa airtime, causes packet collisions, exhausts regulatory duty cycles (e.g. 1% in EU868), and degrades the mesh for all users. <strong>60 minutes or higher is strongly recommended</strong> for ambient weather telemetry. If you choose a shorter interval, ensure you understand the network-wide implications and coordinate with your local mesh community first.
                                </div>
                            </div>

                            <div id="mesh-schedule-note" style="font-size: 0.7rem; color: var(--text-muted); line-height: 1.45; background: rgba(255,193,7,0.06); border-left: 2px solid rgba(255,193,7,0.55); border-radius: 4px; padding: 0.4rem 0.55rem; margin: -0.1rem 0 0.7rem;">
                                Schedule: Environment 1&times;I &middot; NodeInfo 2&times;I &middot; Device 3&times;I &middot; Position 6&times;I. Transmits during safe radio windows.
                            </div>

                            <!-- Node Name input fields & custom toggle -->
                            <div style="margin-bottom: 0.7rem; background: rgba(0,0,0,0.15); border: 1px solid var(--card-border); border-radius: 8px; padding: 0.5rem 0.65rem;">
                                <div style="display: flex; align-items: center; justify-content: space-between; gap: 0.5rem; margin-bottom: 0.35rem;">
                                    <span style="font-size: 0.78rem; color: var(--text-muted); font-weight: 600;">Node Identity</span>
                                    <label style="display: flex; align-items: center; gap: 0.3rem; font-size: 0.75rem; color: var(--text-muted); cursor: pointer; user-select: none;">
                                        <input type="checkbox" id="mesh-custom-name-chk" onchange="toggleMeshtasticCustomName(this.checked)" style="cursor: pointer;">
                                        <span>Custom</span>
                                    </label>
                                </div>
                                <div style="display: flex; align-items: center; gap: 0.4rem;">
                                    <input type="text" id="mesh-custom-long-name" maxlength="23" placeholder="WeatherXM WG1200 XXXX" disabled style="flex: 1; min-width: 0; background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #fff; font-size: 0.76rem; font-family: monospace; padding: 0.3rem 0.5rem; border-radius: 6px; outline: none; opacity: 0.7;" title="Meshtastic Long Name (max 23 bytes)">
                                    <input type="text" id="mesh-custom-short-name" maxlength="4" placeholder="GXXX" disabled style="width: 55px; background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #bb86fc; font-weight: 600; font-size: 0.76rem; font-family: monospace; padding: 0.3rem 0.5rem; border-radius: 6px; outline: none; opacity: 0.7; text-align: center;" title="Meshtastic Short Name (max 4 bytes)">
                                    <button id="mesh-save-name-btn" onclick="saveMeshtasticCustomName()" disabled style="background: rgba(0, 210, 255, 0.2); border: 1px solid rgba(0, 210, 255, 0.4); color: #00d2ff; font-weight: 600; font-size: 0.72rem; padding: 0.32rem 0.6rem; border-radius: 6px; cursor: pointer; opacity: 0.5; white-space: nowrap;">Save</button>
                                </div>
                            </div>

                            <div style="font-size: 0.72rem; color: var(--text-muted); line-height: 1.6; font-family: monospace; background: rgba(0,0,0,0.25); border-radius: 8px; padding: 0.5rem 0.65rem; margin-bottom: 0.7rem;">
                                <div>Node&nbsp;&nbsp;&nbsp;<span id="mesh-node-id" style="color: #00d2ff;">-</span> <span id="mesh-node-num" style="color: var(--text-muted);"></span></div>
                                <div>Radio&nbsp;&nbsp;<span id="mesh-freq" style="color: #00d2ff;">-</span> &middot; LongFast &middot; hop limit <span id="mesh-hop-limit">3</span></div>
                                <div>Sent&nbsp;&nbsp;&nbsp;<span id="mesh-sent-count" style="color: #00d2ff;">0</span> packets &middot; last <span id="mesh-last-sent" style="color: #00d2ff;">never</span></div>
                            </div>

                            <div style="display: flex; align-items: center; gap: 0.6rem;">
                                <button onclick="meshtasticSendNow()" style="background: rgba(0, 210, 255, 0.15); border: 1px solid rgba(0, 210, 255, 0.3); color: #00d2ff; font-weight: 600; font-size: 0.75rem; padding: 0.35rem 0.8rem; border-radius: 6px; cursor: pointer; white-space: nowrap;">Broadcast now</button>
                                <span id="mesh-send-status" style="font-size: 0.72rem; color: var(--text-muted);"></span>
                            </div>

                            <div id="mesh-region-warning" style="display: none; margin-top: 0.7rem; font-size: 0.72rem; color: #ffc107; background: rgba(255,193,7,0.08); border: 1px solid rgba(255,193,7,0.2); border-radius: 8px; padding: 0.5rem 0.65rem;">
                                No Meshtastic frequency is defined for this region.
                            </div>
                        </div>
                    </div>

                    <!-- Station Meshtastic Sub-Card (WS1300 v15+) -->
                    <div id="sta-meshtastic-card" style="padding: 0.85rem; background: rgba(255,255,255,0.02); border: 1px solid var(--card-border); border-radius: 12px;">
                        <div style="display: flex; align-items: center; justify-content: space-between;">
                            <div>
                                <div style="font-weight: 600; font-size: 0.9rem; display: flex; align-items: center; gap: 0.5rem;">
                                    Station as Meshtastic Node
                                    <span id="sta-mesh-badge" class="badge-slot" style="background: rgba(255,193,7,0.1); color: #ffc107; font-size: 0.65rem; padding: 0.1rem 0.4rem; border-radius: 4px; font-weight: 700; text-transform: uppercase;">Beta</span>
                                </div>
                                <div style="font-size: 0.75rem; color: var(--text-muted);">WS1300 will broadcast open Meshtastic telemetry using the active gateway radio configuration.</div>
                            </div>
                        </div>

                        <div id="sta-mesh-detail" style="margin-top: 0.8rem; padding-top: 0.8rem; border-top: 1px solid var(--card-border);">
                            <div style="display: flex; align-items: center; justify-content: space-between; gap: 0.75rem; margin-bottom: 0.75rem;">
                                <div style="font-size: 0.8rem; color: var(--text-muted);">Station Broadcast interval</div>
                                <span id="sta-mesh-current-val" style="font-size: 0.78rem; font-weight: 600; color: #fff; background: rgba(255,255,255,0.06); padding: 0.2rem 0.6rem; border-radius: 6px; border: 1px solid rgba(255,255,255,0.1);">Off</span>
                            </div>
                            <div style="display: flex; align-items: center; flex-wrap: wrap; gap: 0.65rem; margin-top: 0.65rem;">
                                <select id="sta-mesh-target-interval" onchange="onStationMeshtasticIntervalChange(this.value)" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #fff; font-size: 0.78rem; padding: 0.3rem 0.5rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="0">Off</option>
                                    <option value="5">5 minutes</option>
                                    <option value="10">10 minutes</option>
                                    <option value="15">15 minutes</option>
                                    <option value="30">30 minutes</option>
                                    <option value="60" selected>60 minutes (1 hour - Recommended)</option>
                                </select>
                                <label style="display: inline-flex; align-items: center; gap: 0.45rem; font-size: 0.75rem; color: #eee; cursor: pointer; user-select: none;">
                                    <input type="checkbox" id="sta-mesh-send-checkbox" onchange="toggleStationMeshtasticQueue(this.checked)" style="accent-color: #00d2ff; width: 14px; height: 14px; cursor: pointer; margin: 0;">
                                    <span>Send at next station communication</span>
                                </label>
                                <span id="sta-mesh-command-status" style="font-size: 0.72rem;"></span>
                            </div>
                            <div id="sta-mesh-interval-flood-warning" style="display: none; margin-top: 0.6rem; margin-bottom: 0.5rem; font-size: 0.73rem; color: #ffc107; background: rgba(255,193,7,0.08); border: 1px solid rgba(255,193,7,0.3); border-left: 3px solid #ffc107; border-radius: 6px; padding: 0.55rem 0.7rem; line-height: 1.45;">
                                <div style="font-weight: 700; display: flex; align-items: center; gap: 0.35rem; margin-bottom: 0.25rem;">
                                    <span>⚠️</span>
                                    <span>High Broadcast Rate Warning (Mesh Flooding &amp; Airtime Saturation)</span>
                                </div>
                                <div style="color: #e0e0e0; font-size: 0.71rem;">
                                    Mesh networks (Meshtastic / MeshCore) rely on <strong>multi-hop broadcast flood routing</strong>, where every packet is repeated across neighboring nodes up to the hop limit. Transmitting telemetry faster than <strong>60 minutes (1 hour)</strong> rapidly saturates shared LoRa airtime, causes packet collisions, exhausts regulatory duty cycles (e.g. 1% in EU868), and degrades the mesh for all users. <strong>60 minutes or higher is strongly recommended</strong> for ambient weather telemetry. If you choose a shorter interval, ensure you understand the network-wide implications and coordinate with your local mesh community first.
                                </div>
                            </div>
                            <div style="margin-top: 0.5rem; font-size: 0.72rem; color: var(--text-muted);">
                                Configures Meshtastic broadcasts on the paired WS1300. Sent during the downlink window.
                            </div>
                        </div>
                    </div>
                    </div>
                </div>

                <!-- MeshCore Network Panel (Right) -->
                <div id="meshcore-panel" class="section-panel" style="margin: 0; padding: 1rem 1.5rem; transition: padding 0.2s ease;">
                    <div id="meshcore-header" onclick="toggleMeshcorePanelManual()" style="display: flex; align-items: center; justify-content: space-between; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0; cursor: pointer; user-select: none;">
                        <h2 style="font-size: 1.2rem; margin: 0; border-bottom: none; padding-bottom: 0; display: flex; align-items: center; gap: 0.5rem;">
                            MeshCore Network
                            <span class="badge-slot" style="background: rgba(255,193,7,0.1); color: #ffc107; font-size: 0.65rem; padding: 0.1rem 0.4rem; border-radius: 4px; font-weight: 700; text-transform: uppercase;">Beta</span>
                        </h2>
                        <div style="display: flex; align-items: center; gap: 0.5rem;">
                            <span id="meshcore-header-badge" class="badge-slot" style="background: rgba(255, 255, 255, 0.08); color: var(--text-muted); font-size: 0.72rem; padding: 0.2rem 0.5rem; border-radius: 6px;">Disabled</span>
                            <span id="meshcore-chevron" style="font-size: 0.85rem; color: var(--text-muted); transition: transform 0.2s ease; display: inline-block; padding: 0.2rem 0.4rem; transform: rotate(-90deg);">▼</span>
                        </div>
                    </div>
                    <div id="meshcore-content" style="display: none; margin-top: 1rem;">
                    <p style="color: var(--text-muted); font-size: 0.85rem; margin-bottom: 1.25rem;">Broadcast encrypted weather telemetry and node records across the MeshCore mesh.</p>

                    <!-- MeshCore RF & Modulation Presets Box -->
                    <div style="padding: 0.85rem; background: rgba(255,255,255,0.02); border: 1px solid var(--card-border); border-radius: 12px; margin-bottom: 0.85rem;">
                        <div style="font-size: 0.8rem; font-weight: 600; color: #00d2ff; text-transform: uppercase; letter-spacing: 0.05em; margin-bottom: 0.75rem; display: flex; align-items: center; justify-content: space-between;">
                            <span>Radio &amp; Modulation Presets</span>
                            <span style="font-size: 0.7rem; color: var(--text-muted); text-transform: none; font-weight: 400;">Gateway &amp; Station</span>
                        </div>

                        <div style="display: flex; align-items: center; justify-content: space-between; gap: 0.75rem; margin-bottom: 0.7rem;">
                            <div style="font-size: 0.8rem; color: var(--text-muted);">Region &amp; RF Profile</div>
                            <div style="display: flex; align-items: center; gap: 0.4rem;">
                                <select id="meshcore-gw-region" onchange="setRegion(this.value)" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #00d2ff; font-size: 0.78rem; padding: 0.3rem 0.5rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="868">EU 868 MHz</option>
                                    <option value="915">US 915 MHz</option>
                                    <option value="923">AS 923 MHz</option>
                                    <option value="916">AU 915 MHz</option>
                                    <option value="865">IN 865 MHz</option>
                                </select>
                                <select id="meshcore-gw-profile" onchange="onMeshcoreProfileChange(this.value)" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #fff; font-size: 0.78rem; padding: 0.3rem 0.5rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="4">4: EU/UK Narrow (SF8 / 62.5kHz / CR4/8) [EU Std]</option>
                                    <option value="0">0: Long Range (SF10 / 250kHz)</option>
                                    <option value="1">1: Medium (SF8 / 250kHz)</option>
                                    <option value="2">2: Narrow Extreme (SF11 / 125kHz)</option>
                                    <option value="3">3: Fast (SF7 / 250kHz)</option>
                                </select>
                            </div>
                        </div>

                        <!-- Custom RF Mode Toggle & Parameters -->
                        <div style="background: rgba(0,0,0,0.25); border: 1px solid rgba(255,255,255,0.08); border-radius: 8px; padding: 0.6rem 0.75rem; margin-bottom: 0.7rem;">
                            <div style="display: flex; align-items: center; justify-content: space-between; margin-bottom: 0.5rem;">
                                <div style="font-size: 0.75rem; font-weight: 600; color: #00d2ff;">Custom RF Parameters</div>
                                <label style="display: flex; align-items: center; gap: 0.35rem; font-size: 0.72rem; color: var(--text-muted); cursor: pointer;">
                                    <span>Custom RF</span>
                                    <input type="checkbox" id="meshcore-custom-toggle" onchange="toggleMeshcoreCustomMode(this.checked)" style="accent-color: #00d2ff; cursor: pointer;">
                                </label>
                            </div>
                            <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 0.5rem; font-size: 0.75rem;">
                                <div>
                                    <label style="color: var(--text-muted); display: block; margin-bottom: 0.2rem;">Freq (Hz)</label>
                                    <input type="number" id="meshcore-gw-custom-freq" placeholder="e.g. 869618000" disabled style="width: 100%; box-sizing: border-box; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; font-size: 0.75rem; padding: 0.25rem 0.4rem; border-radius: 4px; opacity: 0.7;">
                                </div>
                                <div>
                                    <label style="color: var(--text-muted); display: block; margin-bottom: 0.2rem;">Bandwidth</label>
                                    <select id="meshcore-gw-custom-bw" disabled style="width: 100%; box-sizing: border-box; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; font-size: 0.75rem; padding: 0.25rem 0.4rem; border-radius: 4px; opacity: 0.7;">
                                        <option value="62" selected>62.5 kHz</option>
                                        <option value="125">125 kHz</option>
                                        <option value="250">250 kHz</option>
                                        <option value="500">500 kHz</option>
                                    </select>
                                </div>
                                <div>
                                    <label style="color: var(--text-muted); display: block; margin-bottom: 0.2rem;">Spreading Factor</label>
                                    <select id="meshcore-gw-custom-sf" disabled style="width: 100%; box-sizing: border-box; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; font-size: 0.75rem; padding: 0.25rem 0.4rem; border-radius: 4px; opacity: 0.7;">
                                        <option value="7">SF7</option>
                                        <option value="8" selected>SF8</option>
                                        <option value="9">SF9</option>
                                        <option value="10">SF10</option>
                                        <option value="11">SF11</option>
                                        <option value="12">SF12</option>
                                    </select>
                                </div>
                                <div>
                                    <label style="color: var(--text-muted); display: block; margin-bottom: 0.2rem;">Coding Rate</label>
                                    <select id="meshcore-gw-custom-cr" disabled style="width: 100%; box-sizing: border-box; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; font-size: 0.75rem; padding: 0.25rem 0.4rem; border-radius: 4px; opacity: 0.7;">
                                        <option value="5">4/5</option>
                                        <option value="6">4/6</option>
                                        <option value="7">4/7</option>
                                        <option value="8" selected>4/8</option>
                                    </select>
                                </div>
                                <div>
                                    <label style="color: var(--text-muted); display: block; margin-bottom: 0.2rem;">TX Power (dBm)</label>
                                    <input type="number" id="meshcore-gw-custom-tx-pwr" min="2" max="22" placeholder="2..22 dBm" disabled style="width: 100%; box-sizing: border-box; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; font-size: 0.75rem; padding: 0.25rem 0.4rem; border-radius: 4px; opacity: 0.7;">
                                </div>
                                <div style="display: flex; align-items: flex-end;">
                                    <button id="meshcore-save-rf-btn" onclick="saveMeshcoreGwCustomRf()" disabled style="width: 100%; background: rgba(0, 210, 255, 0.2); border: 1px solid rgba(0, 210, 255, 0.4); color: #00d2ff; font-weight: 600; font-size: 0.72rem; padding: 0.3rem 0.5rem; border-radius: 4px; cursor: pointer; opacity: 0.5;">Save RF</button>
                                </div>
                            </div>
                        </div>

                        <div style="display: flex; align-items: center; justify-content: space-between; gap: 0.75rem;">
                            <div>
                                <div style="font-size: 0.8rem; color: var(--text-muted);">High TX Power (22 dBm EU / 14 dBm other)</div>
                                <div style="font-size: 0.72rem; color: var(--text-muted);">Enable maximum regulatory power for this region</div>
                            </div>
                            <label class="switch">
                                <input type="checkbox" id="meshcore-gw-pwr-toggle-btn" onchange="setMeshcoreGwPower(this.checked)">
                                <span class="slider round"></span>
                            </label>
                        </div>
                    </div>

                    <!-- Gateway MeshCore Sub-Card -->
                    <div style="padding: 0.85rem; background: rgba(255,255,255,0.02); border: 1px solid var(--card-border); border-radius: 12px; margin-bottom: 0.85rem;">
                        <div style="display: flex; align-items: center; justify-content: space-between;">
                            <div>
                                <div style="font-weight: 600; font-size: 0.9rem;">Gateway as MeshCore Node</div>
                                <div style="font-size: 0.75rem; color: var(--text-muted);">Broadcast gateway weather telemetry across MeshCore</div>
                            </div>
                            <label class="switch">
                                <input type="checkbox" id="meshcore-gw-toggle-btn" onchange="toggleMeshcoreGw(this.checked)">
                                <span class="slider round"></span>
                            </label>
                        </div>

                        <div id="meshcore-gw-detail" style="display: none; margin-top: 0.8rem; padding-top: 0.8rem; border-top: 1px solid var(--card-border);">
                            <div style="display: flex; align-items: center; justify-content: space-between; gap: 0.75rem; margin-bottom: 0.7rem;">
                                <div style="font-size: 0.8rem; color: var(--text-muted);">Gateway Broadcast interval</div>
                                <select id="meshcore-gw-interval" onchange="setMeshcoreGwInterval(this.value)" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #fff; font-size: 0.78rem; padding: 0.3rem 0.5rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="60">1 minute</option>
                                    <option value="120">2 minutes</option>
                                    <option value="300">5 minutes</option>
                                    <option value="600">10 minutes</option>
                                    <option value="900">15 minutes</option>
                                    <option value="1800">30 minutes</option>
                                    <option value="3600" selected>60 minutes (1 hour - Recommended)</option>
                                </select>
                            </div>

                            <div id="meshcore-gw-interval-flood-warning" style="display: none; margin-top: -0.2rem; margin-bottom: 0.7rem; font-size: 0.73rem; color: #ffc107; background: rgba(255,193,7,0.08); border: 1px solid rgba(255,193,7,0.3); border-left: 3px solid #ffc107; border-radius: 6px; padding: 0.55rem 0.7rem; line-height: 1.45;">
                                <div style="font-weight: 700; display: flex; align-items: center; gap: 0.35rem; margin-bottom: 0.25rem;">
                                    <span>⚠️</span>
                                    <span>High Broadcast Rate Warning (Mesh Flooding &amp; Airtime Saturation)</span>
                                </div>
                                <div style="color: #e0e0e0; font-size: 0.71rem;">
                                    Mesh networks (Meshtastic / MeshCore) rely on <strong>multi-hop broadcast flood routing</strong>, where every packet is repeated across neighboring nodes up to the hop limit. Transmitting telemetry faster than <strong>60 minutes (1 hour)</strong> rapidly saturates shared LoRa airtime, causes packet collisions, exhausts regulatory duty cycles (e.g. 1% in EU868), and degrades the mesh for all users. <strong>60 minutes or higher is strongly recommended</strong> for ambient weather telemetry. If you choose a shorter interval, ensure you understand the network-wide implications and coordinate with your local mesh community first.
                                </div>
                            </div>

                            <div id="meshcore-schedule-note" style="font-size: 0.7rem; color: var(--text-muted); line-height: 1.45; background: rgba(0,210,255,0.06); border-left: 2px solid rgba(0,210,255,0.55); border-radius: 4px; padding: 0.4rem 0.55rem; margin: -0.1rem 0 0.7rem;">
                                Transmits 0x19 GRP_DATA (WeatherXM encrypted cluster channel) during safe radio windows.
                            </div>

                            <!-- Node Name input fields & custom toggle -->
                            <div style="margin-bottom: 0.7rem; background: rgba(0,0,0,0.15); border: 1px solid var(--card-border); border-radius: 8px; padding: 0.5rem 0.65rem;">
                                <div style="display: flex; align-items: center; justify-content: space-between; gap: 0.5rem; margin-bottom: 0.35rem;">
                                    <span style="font-size: 0.78rem; color: var(--text-muted); font-weight: 600;">Node Identity</span>
                                    <label style="display: flex; align-items: center; gap: 0.3rem; font-size: 0.75rem; color: var(--text-muted); cursor: pointer; user-select: none;">
                                        <input type="checkbox" id="meshcore-custom-name-chk" onchange="toggleMeshcoreCustomName(this.checked)" style="cursor: pointer;">
                                        <span>Custom</span>
                                    </label>
                                </div>
                                <div style="display: flex; align-items: center; gap: 0.4rem;">
                                    <input type="text" id="meshcore-custom-long-name" maxlength="23" placeholder="WeatherXM WG1200 XXXX" disabled style="flex: 1; min-width: 0; background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #fff; font-size: 0.76rem; font-family: monospace; padding: 0.3rem 0.5rem; border-radius: 6px; outline: none; opacity: 0.7;" title="MeshCore Long Name (max 23 bytes)">
                                    <input type="text" id="meshcore-custom-short-name" maxlength="4" placeholder="GXXX" disabled style="width: 55px; background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #bb86fc; font-weight: 600; font-size: 0.76rem; font-family: monospace; padding: 0.3rem 0.5rem; border-radius: 6px; outline: none; opacity: 0.7; text-align: center;" title="MeshCore Short Name (max 4 bytes)">
                                    <button id="meshcore-save-name-btn" onclick="saveMeshcoreCustomName()" disabled style="background: rgba(0, 210, 255, 0.2); border: 1px solid rgba(0, 210, 255, 0.4); color: #00d2ff; font-weight: 600; font-size: 0.72rem; padding: 0.32rem 0.6rem; border-radius: 6px; cursor: pointer; opacity: 0.5; white-space: nowrap;">Save</button>
                                </div>
                            </div>

                            <div style="font-size: 0.72rem; color: var(--text-muted); line-height: 1.6; font-family: monospace; background: rgba(0,0,0,0.25); border-radius: 8px; padding: 0.5rem 0.65rem; margin-bottom: 0.7rem;">
                                <div>Node&nbsp;&nbsp;&nbsp;<span id="meshcore-gw-node-id" style="color: #00d2ff;">-</span> <span id="meshcore-gw-node-num" style="color: var(--text-muted);"></span></div>
                                <div>Radio&nbsp;&nbsp;<span id="meshcore-gw-freq" style="color: #00d2ff;">-</span> &middot; Profile <span id="meshcore-gw-profile-label">4 (EU/UK Narrow)</span></div>
                                <div>Sent&nbsp;&nbsp;&nbsp;<span id="meshcore-gw-sent-count" style="color: #00d2ff;">0</span> packets &middot; last <span id="meshcore-gw-last-sent" style="color: #00d2ff;">never</span></div>
                            </div>

                            <div style="display: flex; align-items: center; gap: 0.6rem;">
                                <button onclick="meshcoreSendNow()" style="background: rgba(0, 210, 255, 0.15); border: 1px solid rgba(0, 210, 255, 0.3); color: #00d2ff; font-weight: 600; font-size: 0.75rem; padding: 0.35rem 0.8rem; border-radius: 6px; cursor: pointer; white-space: nowrap;">Broadcast now</button>
                                <span id="meshcore-gw-send-status" style="font-size: 0.72rem; color: var(--text-muted);"></span>
                            </div>
                        </div>
                    </div>

                    <!-- Meshcore Multi-Station Forwarding -->
                    <div style="display: flex; align-items: center; justify-content: space-between; padding: 0.85rem; background: rgba(255,255,255,0.02); border: 1px solid var(--card-border); border-radius: 12px; margin-bottom: 0.85rem;">
                        <div>
                            <div style="font-weight: 600; font-size: 0.9rem; display: flex; align-items: center; gap: 0.5rem;">
                                MeshCore Multi-Station Forwarding
                                <span class="badge-slot" style="background: rgba(0,210,255,0.1); color: #00d2ff; font-size: 0.65rem; padding: 0.1rem 0.4rem; border-radius: 4px; font-weight: 700; text-transform: uppercase;">Active</span>
                            </div>
                            <div style="font-size: 0.75rem; color: var(--text-muted);">Forward decoded frames from additional nearby MeshCore stations to WeatherXM</div>
                        </div>
                        <label class="switch">
                            <input type="checkbox" id="meshcore-fwd-toggle-btn" onchange="toggleMeshcoreForwarding(this.checked)">
                            <span class="slider round"></span>
                        </label>
                    </div>

                    <!-- Station MeshCore Sub-Card (WS1300 v16+) -->
                    <div id="sta-meshcore-card" style="padding: 0.85rem; background: rgba(255,255,255,0.02); border: 1px solid var(--card-border); border-radius: 12px;">
                        <div style="display: flex; align-items: center; justify-content: space-between;">
                            <div>
                                <div style="font-weight: 600; font-size: 0.9rem; display: flex; align-items: center; gap: 0.5rem;">
                                    Station as MeshCore Node
                                    <span id="sta-meshcore-badge" class="badge-slot" style="background: rgba(255,193,7,0.1); color: #ffc107; font-size: 0.65rem; padding: 0.1rem 0.4rem; border-radius: 4px; font-weight: 700; text-transform: uppercase;">Beta</span>
                                </div>
                                <div style="font-size: 0.75rem; color: var(--text-muted);">WS1300 will broadcast encrypted MeshCore telemetry using the active gateway radio configuration.</div>
                            </div>
                        </div>

                        <div id="sta-meshcore-detail" style="margin-top: 0.8rem; padding-top: 0.8rem; border-top: 1px solid var(--card-border);">
                            <div style="display: flex; align-items: center; justify-content: space-between; gap: 0.75rem; margin-bottom: 0.75rem;">
                                <div style="font-size: 0.8rem; color: var(--text-muted);">Station Broadcast interval</div>
                                <span id="sta-meshcore-current-val" style="font-size: 0.78rem; font-weight: 600; color: #fff; background: rgba(255,255,255,0.06); padding: 0.2rem 0.6rem; border-radius: 6px; border: 1px solid rgba(255,255,255,0.1);">Off</span>
                            </div>
                            <div style="display: flex; align-items: center; flex-wrap: wrap; gap: 0.65rem; margin-top: 0.65rem;">
                                <select id="sta-meshcore-target-interval" onchange="onStationMeshcoreIntervalChange(this.value)" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #fff; font-size: 0.78rem; padding: 0.3rem 0.5rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="0">Off</option>
                                    <option value="5">5 minutes</option>
                                    <option value="10">10 minutes</option>
                                    <option value="15">15 minutes</option>
                                    <option value="30">30 minutes</option>
                                    <option value="60" selected>60 minutes (1 hour - Recommended)</option>
                                </select>
                                <label style="display: inline-flex; align-items: center; gap: 0.45rem; font-size: 0.75rem; color: #eee; cursor: pointer; user-select: none;">
                                    <input type="checkbox" id="sta-meshcore-send-checkbox" onchange="toggleStationMeshcoreQueue(this.checked)" style="accent-color: #00d2ff; width: 14px; height: 14px; cursor: pointer; margin: 0;">
                                    <span>Send at next station communication</span>
                                </label>
                                <span id="sta-meshcore-command-status" style="font-size: 0.72rem;"></span>
                            </div>
                            <div id="sta-meshcore-interval-flood-warning" style="display: none; margin-top: 0.6rem; margin-bottom: 0.5rem; font-size: 0.73rem; color: #ffc107; background: rgba(255,193,7,0.08); border: 1px solid rgba(255,193,7,0.3); border-left: 3px solid #ffc107; border-radius: 6px; padding: 0.55rem 0.7rem; line-height: 1.45;">
                                <div style="font-weight: 700; display: flex; align-items: center; gap: 0.35rem; margin-bottom: 0.25rem;">
                                    <span>⚠️</span>
                                    <span>High Broadcast Rate Warning (Mesh Flooding &amp; Airtime Saturation)</span>
                                </div>
                                <div style="color: #e0e0e0; font-size: 0.71rem;">
                                    Mesh networks (Meshtastic / MeshCore) rely on <strong>multi-hop broadcast flood routing</strong>, where every packet is repeated across neighboring nodes up to the hop limit. Transmitting telemetry faster than <strong>60 minutes (1 hour)</strong> rapidly saturates shared LoRa airtime, causes packet collisions, exhausts regulatory duty cycles (e.g. 1% in EU868), and degrades the mesh for all users. <strong>60 minutes or higher is strongly recommended</strong> for ambient weather telemetry. If you choose a shorter interval, ensure you understand the network-wide implications and coordinate with your local mesh community first.
                                </div>
                            </div>
                            <div style="margin-top: 0.5rem; font-size: 0.72rem; color: var(--text-muted);">
                                Configures MeshCore broadcasts on the paired WS1300. Sent during the downlink window.
                            </div>
                        </div>
                    </div>
                    </div>
                </div>
            </div>

            <!-- Row 3: Adaptive Weather Publishing (Full-Width Panel) -->
            <div id="adaptive-weather-panel" class="section-panel" style="margin-bottom: 1.5rem; padding: 1rem 1.5rem; transition: padding 0.2s ease;">
                <div id="adaptive-weather-header" onclick="toggleAdaptiveWeatherManual()" style="display: flex; align-items: center; justify-content: space-between; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0; cursor: pointer; user-select: none;">
                    <h2 style="font-size: 1.2rem; margin: 0; border-bottom: none; padding-bottom: 0; display: flex; align-items: center; gap: 0.5rem;">
                        Adaptive Weather Publishing
                        <span class="badge-slot" style="background: rgba(0, 210, 255, 0.15); color: #00d2ff; font-size: 0.65rem; padding: 0.1rem 0.4rem; border-radius: 4px; font-weight: 700; text-transform: uppercase;">Smart Mesh Telemetry</span>
                    </h2>
                    <div style="display: flex; align-items: center; gap: 0.5rem;">
                        <span id="wx-policy-station-badge" class="badge-slot" style="background: rgba(255, 255, 255, 0.08); color: var(--text-muted); font-size: 0.72rem; padding: 0.2rem 0.5rem; border-radius: 6px;">Mesh Disabled</span>
                        <span id="adaptive-weather-chevron" style="font-size: 0.85rem; color: var(--text-muted); transition: transform 0.2s ease; display: inline-block; padding: 0.2rem 0.4rem; transform: rotate(-90deg);">▼</span>
                    </div>
                </div>
                <div id="adaptive-weather-content" style="display: none; margin-top: 1rem;">
                    <div id="adaptive-weather-disabled-banner" style="display: none; background: rgba(255, 193, 7, 0.1); border: 1px solid rgba(255, 193, 7, 0.25); color: #ffc107; padding: 0.6rem 0.8rem; border-radius: 6px; font-size: 0.8rem; margin-bottom: 1.25rem; align-items: center; gap: 0.5rem;">
                        <span>⚠️</span>
                        <span>Mesh broadcasting is disabled. Enable Meshtastic or MeshCore above to activate Adaptive Weather Publishing.</span>
                    </div>
                    <div id="adaptive-weather-inner" style="transition: opacity 0.2s ease;">
                    <p style="color: var(--text-muted); font-size: 0.85rem; margin-bottom: 1.25rem;">
                    Dynamic event-driven telemetry triggers when weather conditions change rapidly (wind gusts, rain onset, temperature shifts) while preserving airtime during calm weather with a 60-minute background baseline.
                </p>

                <!-- Status Summary Strip -->
                <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 0.75rem; margin-bottom: 1.25rem;">
                    <div style="background: rgba(255,255,255,0.03); border: 1px solid var(--card-border); border-radius: 8px; padding: 0.6rem 0.8rem;">
                        <div style="font-size: 0.7rem; color: var(--text-muted); text-transform: uppercase;">Policy Version</div>
                        <div id="wx-policy-ver" style="font-size: 1.05rem; font-weight: 700; color: #fff;">v1</div>
                    </div>
                    <div style="background: rgba(255,255,255,0.03); border: 1px solid var(--card-border); border-radius: 8px; padding: 0.6rem 0.8rem;">
                        <div style="font-size: 0.7rem; color: var(--text-muted); text-transform: uppercase;">Policy CRC16</div>
                        <div id="wx-policy-crc" style="font-size: 1.05rem; font-weight: 700; color: #00d2ff; font-family: monospace;">0x0000</div>
                    </div>
                    <div style="background: rgba(255,255,255,0.03); border: 1px solid var(--card-border); border-radius: 8px; padding: 0.6rem 0.8rem;">
                        <div style="font-size: 0.7rem; color: var(--text-muted); text-transform: uppercase;">Event Cooldown</div>
                        <div id="wx-policy-cooldown-display" style="font-size: 1.05rem; font-weight: 700; color: #fff;">5 min</div>
                    </div>
                    <div style="background: rgba(255,255,255,0.03); border: 1px solid var(--card-border); border-radius: 8px; padding: 0.6rem 0.8rem;">
                        <div style="font-size: 0.7rem; color: var(--text-muted); text-transform: uppercase;">Hourly Budget</div>
                        <div id="wx-policy-budget-display" style="font-size: 1.05rem; font-weight: 700; color: #fff;">6 / hr</div>
                    </div>
                </div>

                <!-- Evaluator Diagnostics Strip -->
                <div style="background: rgba(0,0,0,0.25); border: 1px solid rgba(255,255,255,0.08); border-radius: 10px; padding: 0.85rem; margin-bottom: 1.25rem;">
                    <div style="font-size: 0.78rem; font-weight: 600; color: #00d2ff; text-transform: uppercase; letter-spacing: 0.05em; margin-bottom: 0.6rem;">Live Mesh Evaluator Diagnostics</div>
                    <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 1rem; font-size: 0.8rem;">
                        <div style="background: rgba(255,255,255,0.02); padding: 0.6rem 0.75rem; border-radius: 6px; border-left: 3px solid #5fd589;">
                            <div style="font-weight: 600; color: #5fd589; margin-bottom: 0.3rem;">Meshtastic Evaluator</div>
                            <div style="color: var(--text-muted); display: flex; justify-content: space-between; margin-bottom: 0.2rem;"><span>Events this hour:</span> <span id="wx-diag-mt-events" style="color: #fff; font-weight: 600;">0 / 6</span></div>
                            <div style="color: var(--text-muted); display: flex; justify-content: space-between; margin-bottom: 0.2rem;"><span>Cooldown Level:</span> <span id="wx-diag-mt-cd-level" style="color: #fff;">Level 0 (Normal)</span></div>
                            <div style="color: var(--text-muted); display: flex; justify-content: space-between;"><span>Last Reasons:</span> <span id="wx-diag-mt-reasons" style="color: #5fd589; font-family: monospace;">-</span></div>
                        </div>
                        <div style="background: rgba(255,255,255,0.02); padding: 0.6rem 0.75rem; border-radius: 6px; border-left: 3px solid #c084fc;">
                            <div style="font-weight: 600; color: #c084fc; margin-bottom: 0.3rem;">MeshCore Evaluator</div>
                            <div style="color: var(--text-muted); display: flex; justify-content: space-between; margin-bottom: 0.2rem;"><span>Events this hour:</span> <span id="wx-diag-mc-events" style="color: #fff; font-weight: 600;">0 / 6</span></div>
                            <div style="color: var(--text-muted); display: flex; justify-content: space-between; margin-bottom: 0.2rem;"><span>Cooldown Level:</span> <span id="wx-diag-mc-cd-level" style="color: #fff;">Level 0 (Normal)</span></div>
                            <div style="color: var(--text-muted); display: flex; justify-content: space-between;"><span>Last Reasons:</span> <span id="wx-diag-mc-reasons" style="color: #c084fc; font-family: monospace;">-</span></div>
                        </div>
                    </div>
                </div>

                <!-- Policy Configuration Section -->
                <div style="background: rgba(255,255,255,0.02); border: 1px solid var(--card-border); border-radius: 12px; padding: 1rem; margin-bottom: 1rem;">
                    <div style="font-size: 0.85rem; font-weight: 600; color: #fff; margin-bottom: 0.85rem;">General Parameters</div>
                    <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 0.85rem; font-size: 0.8rem; margin-bottom: 1rem;">
                        <div>
                            <label style="color: var(--text-muted); display: block; margin-bottom: 0.25rem;">Event Cooldown (sec)</label>
                            <input type="number" id="wx-input-cooldown" min="60" max="3600" step="10" style="width: 100%; box-sizing: border-box; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; font-size: 0.8rem; padding: 0.35rem 0.5rem; border-radius: 6px;">
                        </div>
                        <div>
                            <label style="color: var(--text-muted); display: block; margin-bottom: 0.25rem;">Hourly Event Budget</label>
                            <input type="number" id="wx-input-budget" min="1" max="60" step="1" style="width: 100%; box-sizing: border-box; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; font-size: 0.8rem; padding: 0.35rem 0.5rem; border-radius: 6px;">
                        </div>
                        <div>
                            <label style="color: var(--text-muted); display: block; margin-bottom: 0.25rem;">Wind Dir Calm Gate (m/s)</label>
                            <input type="number" id="wx-input-calm-gate" min="0" max="20" step="0.5" style="width: 100%; box-sizing: border-box; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; font-size: 0.8rem; padding: 0.35rem 0.5rem; border-radius: 6px;">
                        </div>
                        <div>
                            <label style="color: var(--text-muted); display: block; margin-bottom: 0.25rem;">Rain ON Threshold (mm/h)</label>
                            <input type="number" id="wx-input-rain-on" min="0" max="50" step="0.1" style="width: 100%; box-sizing: border-box; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; font-size: 0.8rem; padding: 0.35rem 0.5rem; border-radius: 6px;">
                        </div>
                        <div>
                            <label style="color: var(--text-muted); display: block; margin-bottom: 0.25rem;">Rain OFF Threshold (mm/h)</label>
                            <input type="number" id="wx-input-rain-off" min="0" max="50" step="0.1" style="width: 100%; box-sizing: border-box; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; font-size: 0.8rem; padding: 0.35rem 0.5rem; border-radius: 6px;">
                        </div>
                        <div style="display: flex; align-items: center; gap: 0.5rem; margin-top: 1rem;">
                            <input type="checkbox" id="wx-input-rain-state" style="accent-color: #00d2ff; cursor: pointer; width: 16px; height: 16px;">
                            <label for="wx-input-rain-state" style="color: #fff; font-size: 0.8rem; cursor: pointer;">Trigger on Rain Start / Stop Hysteresis</label>
                        </div>
                    </div>

                    <!-- Metric Thresholds Table -->
                    <div style="font-size: 0.85rem; font-weight: 600; color: #fff; margin-bottom: 0.6rem;">Metric Deltas &amp; Triggers</div>
                    <div style="overflow-x: auto;">
                        <table style="width: 100%; border-collapse: collapse; font-size: 0.78rem;">
                            <thead>
                                <tr style="border-bottom: 1px solid rgba(255,255,255,0.1); color: var(--text-muted); text-align: left;">
                                    <th style="padding: 0.4rem 0.5rem;">Metric</th>
                                    <th style="padding: 0.4rem 0.5rem; text-align: center;">Enabled</th>
                                    <th style="padding: 0.4rem 0.5rem;">Delta Threshold</th>
                                    <th style="padding: 0.4rem 0.5rem;">Unit</th>
                                    <th style="padding: 0.4rem 0.5rem;">Behavior</th>
                                </tr>
                            </thead>
                            <tbody>
                                <tr style="border-bottom: 1px solid rgba(255,255,255,0.04);">
                                    <td style="padding: 0.4rem 0.5rem; font-weight: 600; color: #fff;">Temperature</td>
                                    <td style="padding: 0.4rem 0.5rem; text-align: center;"><input type="checkbox" id="wx-en-temp" style="accent-color: #00d2ff; cursor: pointer;"></td>
                                    <td style="padding: 0.4rem 0.5rem;"><input type="number" id="wx-th-temp" step="0.1" style="width: 90px; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; padding: 0.2rem 0.4rem; border-radius: 4px;"></td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">°C</td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">Delta absolute</td>
                                </tr>
                                <tr style="border-bottom: 1px solid rgba(255,255,255,0.04);">
                                    <td style="padding: 0.4rem 0.5rem; font-weight: 600; color: #fff;">Humidity</td>
                                    <td style="padding: 0.4rem 0.5rem; text-align: center;"><input type="checkbox" id="wx-en-hum" style="accent-color: #00d2ff; cursor: pointer;"></td>
                                    <td style="padding: 0.4rem 0.5rem;"><input type="number" id="wx-th-hum" step="1" style="width: 90px; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; padding: 0.2rem 0.4rem; border-radius: 4px;"></td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">%</td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">Delta absolute</td>
                                </tr>
                                <tr style="border-bottom: 1px solid rgba(255,255,255,0.04);">
                                    <td style="padding: 0.4rem 0.5rem; font-weight: 600; color: #fff;">Barometric Pressure</td>
                                    <td style="padding: 0.4rem 0.5rem; text-align: center;"><input type="checkbox" id="wx-en-press" style="accent-color: #00d2ff; cursor: pointer;"></td>
                                    <td style="padding: 0.4rem 0.5rem;"><input type="number" id="wx-th-press" step="0.1" style="width: 90px; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; padding: 0.2rem 0.4rem; border-radius: 4px;"></td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">hPa</td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">Delta absolute</td>
                                </tr>
                                <tr style="border-bottom: 1px solid rgba(255,255,255,0.04);">
                                    <td style="padding: 0.4rem 0.5rem; font-weight: 600; color: #fff;">Wind Speed</td>
                                    <td style="padding: 0.4rem 0.5rem; text-align: center;"><input type="checkbox" id="wx-en-wind_speed" style="accent-color: #00d2ff; cursor: pointer;"></td>
                                    <td style="padding: 0.4rem 0.5rem;"><input type="number" id="wx-th-wind_speed" step="0.5" style="width: 90px; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; padding: 0.2rem 0.4rem; border-radius: 4px;"></td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">m/s</td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">Delta absolute</td>
                                </tr>
                                <tr style="border-bottom: 1px solid rgba(255,255,255,0.04);">
                                    <td style="padding: 0.4rem 0.5rem; font-weight: 600; color: #fff;">Wind Gust</td>
                                    <td style="padding: 0.4rem 0.5rem; text-align: center;"><input type="checkbox" id="wx-en-wind_gust" style="accent-color: #00d2ff; cursor: pointer;"></td>
                                    <td style="padding: 0.4rem 0.5rem;"><input type="number" id="wx-th-wind_gust" step="0.5" style="width: 90px; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; padding: 0.2rem 0.4rem; border-radius: 4px;"></td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">m/s</td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">Delta absolute</td>
                                </tr>
                                <tr style="border-bottom: 1px solid rgba(255,255,255,0.04);">
                                    <td style="padding: 0.4rem 0.5rem; font-weight: 600; color: #fff;">Wind Direction</td>
                                    <td style="padding: 0.4rem 0.5rem; text-align: center;"><input type="checkbox" id="wx-en-wind_dir" style="accent-color: #00d2ff; cursor: pointer;"></td>
                                    <td style="padding: 0.4rem 0.5rem;"><input type="number" id="wx-th-wind_dir" step="5" style="width: 90px; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; padding: 0.2rem 0.4rem; border-radius: 4px;"></td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">deg</td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">Circular shortest arc (calm-gated)</td>
                                </tr>
                                <tr style="border-bottom: 1px solid rgba(255,255,255,0.04);">
                                    <td style="padding: 0.4rem 0.5rem; font-weight: 600; color: #fff;">Precipitation Accumulation</td>
                                    <td style="padding: 0.4rem 0.5rem; text-align: center;"><input type="checkbox" id="wx-en-precip_cml" style="accent-color: #00d2ff; cursor: pointer;"></td>
                                    <td style="padding: 0.4rem 0.5rem;"><input type="number" id="wx-th-precip_cml" step="0.1" style="width: 90px; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; padding: 0.2rem 0.4rem; border-radius: 4px;"></td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">mm</td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">Accumulative step &gt;= threshold</td>
                                </tr>
                                <tr style="border-bottom: 1px solid rgba(255,255,255,0.04);">
                                    <td style="padding: 0.4rem 0.5rem; font-weight: 600; color: #fff;">Precipitation Rate</td>
                                    <td style="padding: 0.4rem 0.5rem; text-align: center;"><input type="checkbox" id="wx-en-precip_rate" style="accent-color: #00d2ff; cursor: pointer;"></td>
                                    <td style="padding: 0.4rem 0.5rem;"><input type="number" id="wx-th-precip_rate" step="0.5" style="width: 90px; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; padding: 0.2rem 0.4rem; border-radius: 4px;"></td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">mm/h</td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">Delta absolute</td>
                                </tr>
                                <tr style="border-bottom: 1px solid rgba(255,255,255,0.04);">
                                    <td style="padding: 0.4rem 0.5rem; font-weight: 600; color: #fff;">Solar Illuminance</td>
                                    <td style="padding: 0.4rem 0.5rem; text-align: center;"><input type="checkbox" id="wx-en-illum" style="accent-color: #00d2ff; cursor: pointer;"></td>
                                    <td style="padding: 0.4rem 0.5rem;"><input type="number" id="wx-th-illum" step="1000" style="width: 90px; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; padding: 0.2rem 0.4rem; border-radius: 4px;"></td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">lux</td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">Delta absolute</td>
                                </tr>
                                <tr style="border-bottom: 1px solid rgba(255,255,255,0.04);">
                                    <td style="padding: 0.4rem 0.5rem; font-weight: 600; color: #fff;">Solar Radiation</td>
                                    <td style="padding: 0.4rem 0.5rem; text-align: center;"><input type="checkbox" id="wx-en-solar_rad" style="accent-color: #00d2ff; cursor: pointer;"></td>
                                    <td style="padding: 0.4rem 0.5rem;"><input type="number" id="wx-th-solar_rad" step="10" style="width: 90px; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; padding: 0.2rem 0.4rem; border-radius: 4px;"></td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">W/m²</td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">Delta absolute</td>
                                </tr>
                                <tr>
                                    <td style="padding: 0.4rem 0.5rem; font-weight: 600; color: #fff;">UV Index</td>
                                    <td style="padding: 0.4rem 0.5rem; text-align: center;"><input type="checkbox" id="wx-en-uv" style="accent-color: #00d2ff; cursor: pointer;"></td>
                                    <td style="padding: 0.4rem 0.5rem;"><input type="number" id="wx-th-uv" step="1" style="width: 90px; background: rgba(30,30,30,0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; padding: 0.2rem 0.4rem; border-radius: 4px;"></td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">index</td>
                                    <td style="padding: 0.4rem 0.5rem; color: var(--text-muted);">Delta absolute</td>
                                </tr>
                            </tbody>
                        </table>
                    </div>
                </div>

                <!-- Action Buttons -->
                <div style="display: flex; gap: 0.75rem; flex-wrap: wrap; justify-content: flex-end;">
                    <button onclick="resetWeatherPolicyBaseline()" style="background: rgba(255, 255, 255, 0.08); border: 1px solid rgba(255, 255, 255, 0.15); color: #fff; font-weight: 600; font-size: 0.8rem; padding: 0.45rem 1rem; border-radius: 6px; cursor: pointer; transition: all 0.2s;">Reset Baseline</button>
                    <button onclick="resetWeatherPolicyDefaults()" style="background: rgba(255, 193, 7, 0.15); border: 1px solid rgba(255, 193, 7, 0.3); color: #ffc107; font-weight: 600; font-size: 0.8rem; padding: 0.45rem 1rem; border-radius: 6px; cursor: pointer; transition: all 0.2s;">Reset to Defaults</button>
                    <button onclick="saveWeatherPolicy()" style="background: linear-gradient(135deg, #00d2ff, #007bff); border: none; color: #fff; font-weight: 600; font-size: 0.8rem; padding: 0.45rem 1.25rem; border-radius: 6px; cursor: pointer; transition: all 0.2s;">Save Policy</button>
                </div>
                </div>
                </div>
            </div>

             <!-- LoRa Sniffer Packets (Full-Width Panel) -->
            <style>
            .badge-slot {
                display: inline-block;
                padding: 0.15rem 0.4rem;
                border-radius: 4px;
                font-size: 0.75rem;
                font-weight: 600;
                text-align: center;
                white-space: nowrap;
            }
            .badge-slot.base {
                background: rgba(255, 193, 7, 0.15);
                border: 1px solid #ffc107;
                color: #ffc107;
            }
            .badge-slot.channel {
                background: rgba(0, 123, 255, 0.15);
                border: 1px solid #007bff;
                color: #007bff;
            }
            .badge-proto {
                display: inline-flex;
                align-items: center;
                gap: 4px;
                padding: 0.15rem 0.45rem;
                border-radius: 4px;
                font-size: 0.75rem;
                font-weight: 700;
                line-height: 1.2;
                white-space: nowrap;
            }
            .badge-proto.wxm {
                background: rgba(23, 162, 184, 0.15);
                border: 1px solid rgba(23, 162, 184, 0.4);
                color: #17a2b8;
            }
            .badge-proto.meshtastic {
                background: rgba(95, 213, 137, 0.15);
                border: 1px solid rgba(95, 213, 137, 0.4);
                color: #5fd589;
            }
            .badge-proto.meshcore {
                background: rgba(168, 85, 247, 0.15);
                border: 1px solid rgba(168, 85, 247, 0.4);
                color: #c084fc;
            }
            .badge-proto.other {
                background: rgba(245, 158, 11, 0.15);
                border: 1px solid rgba(245, 158, 11, 0.4);
                color: #fbbf24;
            }
            .badge-type {
                display: inline-block;
                padding: 0.15rem 0.4rem;
                border-radius: 4px;
                font-size: 0.75rem;
                font-weight: 700;
                text-align: center;
                white-space: nowrap;
            }
            .badge-type.wxm {
                background: rgba(23, 162, 184, 0.15);
                border: 1px solid #17a2b8;
                color: #17a2b8;
            }
            .badge-type.boot {
                background: rgba(40, 167, 69, 0.15);
                border: 1px solid #28a745;
                color: #28a745;
            }
            .badge-type.adv {
                background: rgba(23, 162, 184, 0.15);
                border: 1px solid #17a2b8;
                color: #17a2b8;
            }
            .badge-type.sensors {
                background: rgba(220, 53, 69, 0.15);
                border: 1px solid #dc3545;
                color: #dc3545;
            }
            .badge-type.gnss {
                background: rgba(111, 66, 193, 0.15);
                border: 1px solid #6f42c1;
                color: #6f42c1;
            }
            .badge-type.ack {
                background: rgba(0, 245, 160, 0.15);
                border: 1px solid #00f5a0;
                color: #00f5a0;
            }
            .badge-type.cmd {
                background: rgba(255, 193, 7, 0.15);
                border: 1px solid #ffc107;
                color: #ffc107;
            }
            .badge-type.timesync {
                background: rgba(0, 210, 255, 0.15);
                border: 1px solid #00d2ff;
                color: #00d2ff;
            }
            .badge-type.pair {
                background: rgba(168, 85, 247, 0.15);
                border: 1px solid #a855f7;
                color: #c084fc;
            }
            .badge-type.unknown, .badge-type.other {
                background: rgba(245, 158, 11, 0.15);
                border: 1px solid #f59e0b;
                color: #fbbf24;
            }
            .badge-type.meshtastic {
                background: rgba(95, 213, 137, 0.15);
                border: 1px solid #5fd589;
                color: #5fd589;
            }
            .badge-type.meshcore {
                background: rgba(168, 85, 247, 0.15);
                border: 1px solid #a855f7;
                color: #c084fc;
            }
            .badge-direction.tx {
                display: inline-block;
                padding: 0.1rem 0.35rem;
                border-radius: 4px;
                font-size: 0.68rem;
                font-weight: 700;
                background: rgba(0, 242, 254, 0.15);
                border: 1px solid rgba(0, 242, 254, 0.4);
                color: #00f2fe;
                line-height: 1.2;
            }
            .badge-direction.rx {
                display: inline-block;
                padding: 0.1rem 0.35rem;
                border-radius: 4px;
                font-size: 0.68rem;
                font-weight: 700;
                background: rgba(0, 245, 160, 0.15);
                border: 1px solid rgba(0, 245, 160, 0.4);
                color: #00f5a0;
                line-height: 1.2;
            }
            .packet-filter-bar {
                display: flex;
                align-items: center;
                gap: 0.4rem;
                flex-wrap: wrap;
                margin-top: 0.4rem;
            }
            .filter-btn {
                background: rgba(255, 255, 255, 0.04);
                border: 1px solid var(--card-border);
                color: var(--text-muted);
                font-size: 0.75rem;
                font-weight: 600;
                padding: 0.25rem 0.65rem;
                border-radius: 6px;
                cursor: pointer;
                transition: all 0.2s ease;
                display: inline-flex;
                align-items: center;
                gap: 0.35rem;
                user-select: none;
                font-family: inherit;
            }
            .filter-btn:hover {
                background: rgba(255, 255, 255, 0.08);
                color: var(--text-color);
            }
            .filter-btn.active {
                background: rgba(0, 210, 255, 0.15);
                border-color: rgba(0, 210, 255, 0.4);
                color: #00d2ff;
            }
            .filter-btn.active.filter-wxm {
                background: rgba(23, 162, 184, 0.15);
                border-color: rgba(23, 162, 184, 0.4);
                color: #17a2b8;
            }
            .filter-btn.active.filter-ws1001 {
                background: rgba(56, 189, 248, 0.15);
                border-color: rgba(56, 189, 248, 0.4);
                color: #38bdf8;
            }
            .filter-btn.active.filter-ws1300 {
                background: rgba(0, 210, 255, 0.15);
                border-color: rgba(0, 210, 255, 0.4);
                color: #00d2ff;
            }
            .filter-btn.active.filter-meshtastic {
                background: rgba(95, 213, 137, 0.15);
                border-color: rgba(95, 213, 137, 0.4);
                color: #5fd589;
            }
            .filter-btn.active.filter-meshcore {
                background: rgba(168, 85, 247, 0.15);
                border-color: rgba(168, 85, 247, 0.4);
                color: #c084fc;
            }
            .filter-btn.active.filter-other {
                background: rgba(245, 158, 11, 0.15);
                border-color: rgba(245, 158, 11, 0.4);
                color: #fbbf24;
            }
            .filter-btn .filter-dot {
                width: 6px;
                height: 6px;
                border-radius: 50%;
                background-color: currentColor;
            }
            .th-info {
                display: inline-flex;
                align-items: center;
                justify-content: center;
                width: 13px;
                height: 13px;
                border-radius: 50%;
                background: rgba(255, 255, 255, 0.12);
                color: var(--text-muted);
                font-size: 0.6rem;
                font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
                font-weight: 700;
                font-style: normal;
                margin-left: 3px;
                cursor: help;
                transition: all 0.2s ease;
                vertical-align: middle;
                line-height: 1;
            }
            .th-info:hover {
                background: #00d2ff;
                color: #000;
            }
            .btn-sm-ctrl {
                background: rgba(255,255,255,0.05);
                border: 1px solid rgba(255,255,255,0.1);
                color: var(--text-muted);
                font-size: 0.75rem;
                padding: 0.25rem 0.6rem;
                border-radius: 6px;
                cursor: pointer;
                transition: all 0.2s ease;
                font-weight: 500;
            }
            .btn-sm-ctrl:hover {
                background: rgba(255,255,255,0.1);
                color: #fff;
                border-color: rgba(255,255,255,0.2);
            }
            .btn-sm-ctrl.active {
                background: rgba(187, 134, 252, 0.15) !important;
                border-color: #bb86fc !important;
                color: #bb86fc !important;
                font-weight: 600;
                box-shadow: 0 0 10px rgba(187, 134, 252, 0.1);
            }
            @media (max-width: 768px) {
                .sniffer-ctrl-container {
                    flex-direction: column;
                    align-items: stretch !important;
                }
                .sniffer-ctrl-group {
                    justify-content: space-between;
                    width: 100%;
                }
            }
            .sniffer-ctrl-container div {
                transition: opacity 0.3s ease, filter 0.3s ease;
            }
            .sniffer-ctrl-container div.disabled {
                opacity: 0.4;
                pointer-events: none;
                filter: grayscale(100%);
            }
            </style>
            <!-- Station Pairing & Radio Controls Panel -->
            <div class="section-panel" style="margin-bottom: 1.5rem; padding: 1.5rem;">
                <div style="display: flex; flex-wrap: wrap; justify-content: space-between; align-items: center; gap: 1rem; margin-bottom: 0.75rem; border-bottom: 1px solid var(--card-border); padding-bottom: 0.75rem;">
                    <div>
                        <h2 style="font-size: 1.2rem; margin-bottom: 0.25rem; border-bottom: none; padding-bottom: 0;">Weather Station Pairing</h2>
                        <p style="color: var(--text-muted); font-size: 0.85rem; margin: 0;">How this gateway selects the weather station it listens to and uplinks data for.</p>
                    </div>
                    <div style="display: flex; align-items: center; gap: 0.5rem;">
                        <span id="fallback-phase-badge" style="display: none; font-size: 0.65rem; font-weight: 700; padding: 0.2rem 0.5rem; border-radius: 4px; text-transform: uppercase;">-</span>
                        <span id="uplink-badge" style="font-size: 0.65rem; font-weight: 700; padding: 0.2rem 0.5rem; border-radius: 4px; text-transform: uppercase;">-</span>
                        <span id="pair-state-badge" style="font-size: 0.65rem; font-weight: 700; padding: 0.2rem 0.5rem; border-radius: 4px; text-transform: uppercase;">-</span>
                    </div>
                </div>
                <!-- Combined Station Target & Detected Candidates Container -->
                <div id="station-pairing-top-row" style="display: flex; flex-wrap: wrap; gap: 0.75rem; margin-bottom: 0.85rem; align-items: stretch;">
                    <div id="local-cloud-match-info" style="flex: 1 1 100%; min-width: 280px; font-size: 0.75rem;"></div>
                    <div id="detected-stations-container" style="display: none; flex: 1 1 calc(50% - 0.375rem); min-width: 280px; flex-direction: column; gap: 0.5rem;">
                        <div style="display: flex; justify-content: space-between; align-items: center;">
                            <span style="font-size: 0.75rem; font-weight: 700; color: #bb86fc; text-transform: uppercase; letter-spacing: 0.5px;">Discovered Stations (RAM Buffer)</span>
                            <button onclick="clearRecentStations()" class="btn-sm-ctrl" style="font-size: 0.65rem; padding: 0.15rem 0.45rem; color: var(--text-muted); border-color: rgba(255,255,255,0.15);" title="Clear discovered stations buffer from RAM">Clear List</button>
                        </div>
                        <div id="recent-stations-list" style="display: flex; flex-direction: column; gap: 0.5rem; width: 100%;"></div>
                    </div>
                </div>
                <div class="sniffer-ctrl-container" style="display: flex; flex-wrap: wrap; align-items: stretch; gap: 0.75rem;">
                    <!-- Pairing Mode Group -->
                    <div id="ctrl-autopair" style="display: flex; flex-direction: column; gap: 0.4rem; background: rgba(187, 134, 252, 0.05); border: 1px solid rgba(187, 134, 252, 0.25); padding: 0.6rem 0.75rem; border-radius: 8px; flex: 1.3 1 250px; min-width: 230px;">
                        <span style="font-size: 0.75rem; font-weight: 700; color: #bb86fc; text-transform: uppercase; letter-spacing: 0.5px;">Pairing Mode</span>
                        <div class="segment-control" style="display: flex; background: rgba(0, 0, 0, 0.25); padding: 2px; border-radius: 6px; width: 100%;">
                            <button id="pair-btn-off" onclick="setPairState('off')" title="Stop weather uplink while preserving the currently selected cloud or local target." style="flex: 1; text-align: center; border: none; background: transparent; color: var(--text-muted); font-size: 0.75rem; font-weight: 600; padding: 0.4rem 0; border-radius: 4px; cursor: pointer; transition: all 0.2s;">OFF</button>
                            <button id="pair-btn-cloud" onclick="setPairState('cloud')" title="Target the station assignment cached from WeatherXM cloud." style="flex: 1; text-align: center; border: none; background: transparent; color: var(--text-muted); font-size: 0.75rem; font-weight: 600; padding: 0.4rem 0; border-radius: 4px; cursor: pointer; transition: all 0.2s;">CLOUD</button>
                            <button id="pair-btn-local" onclick="setPairState('local')" title="Target the locally paired station saved by SCAN." style="flex: 1; text-align: center; border: none; background: transparent; color: var(--text-muted); font-size: 0.75rem; font-weight: 600; padding: 0.4rem 0; border-radius: 4px; cursor: pointer; transition: all 0.2s;">LOCAL</button>
                        </div>
                        <p id="pair-mode-caption" style="color: var(--text-muted); font-size: 0.73rem; margin: 0; line-height: 1.3; min-height: 1.5em;"></p>
                    </div>

                    <!-- Station Scan & Discovery Group -->
                    <div id="ctrl-scan" style="display: flex; flex-direction: column; gap: 0.45rem; background: rgba(187, 134, 252, 0.04); border: 1px solid rgba(187, 134, 252, 0.22); padding: 0.6rem 0.75rem; border-radius: 8px; flex: 1.7 1 290px; min-width: 260px;">
                        <div style="display: flex; justify-content: space-between; align-items: center;">
                            <span style="font-size: 0.75rem; font-weight: 700; color: #bb86fc; text-transform: uppercase; letter-spacing: 0.5px;">Station Scan</span>
                            <span id="scan-state-badge" style="font-size: 0.65rem; font-weight: 700; padding: 0.15rem 0.4rem; border-radius: 4px; background: rgba(255,255,255,0.06); color: var(--text-muted); text-transform: uppercase;">IDLE</span>
                        </div>
                        <div id="ctrl-scan-profile" style="display: flex; align-items: center; justify-content: space-between; gap: 0.35rem;">
                            <span style="font-size: 0.72rem; color: var(--text-muted); font-weight: 600;">Scan Profile:</span>
                            <select id="scan-profile-select" onchange="setScanProfile(this.value)" style="background: rgba(30, 30, 30, 0.85); border: 1px solid rgba(187, 134, 252, 0.35); color: #bb86fc; font-size: 0.74rem; padding: 0.2rem 0.45rem; border-radius: 6px; outline: none; cursor: pointer; flex: 1; max-width: 175px;">
                                <option value="0">All (WS1001 + WS1300)</option>
                                <option value="1300">WS1300 Only (Multi-Channel)</option>
                                <option value="1001">WS1001 Only (Single Channel)</option>
                            </select>
                        </div>
                        <button id="btn-scan-toggle" onclick="toggleDiscoveryScan()" class="btn-sm-ctrl" style="font-weight: 700; font-size: 0.76rem; padding: 0.35rem 0.7rem; background: rgba(187, 134, 252, 0.18); border-color: #bb86fc; color: #bb86fc; cursor: pointer; display: flex; align-items: center; justify-content: center; gap: 0.35rem; width: 100%;">
                            <span id="btn-scan-icon">🔍</span><span id="btn-scan-text">START SCAN (ALL)</span>
                        </button>
                        <p id="scan-idle-caption" style="color: var(--text-muted); font-size: 0.73rem; margin: 0; line-height: 1.3;">Sweeps WS1001 base + WS1300 CH0–CH7 in 2 passes (quick + deep). ~11 min total.</p>
                        <div id="scan-progress-info" style="font-size: 0.75rem; display: none;"></div>
                    </div>

                    <!-- Radio Group -->
                    <div style="display: flex; flex-direction: column; gap: 0.4rem; background: rgba(255,255,255,0.03); border: 1px solid var(--card-border); padding: 0.6rem 0.75rem; border-radius: 8px; flex: 1 1 240px; min-width: 220px;">
                        <span style="font-size: 0.75rem; font-weight: 700; color: var(--text-muted); text-transform: uppercase; letter-spacing: 0.5px;">Radio</span>
                        <div style="display: flex; flex-wrap: wrap; align-items: center; gap: 0.75rem;">
                            <div id="ctrl-region" style="display: flex; align-items: center; gap: 0.4rem;">
                                <span style="font-size: 0.8rem; font-weight: 500; color: var(--text-muted);">Region:</span>
                                <select id="region-select" onchange="setRegion(this.value)" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; font-size: 0.75rem; padding: 0.15rem 0.4rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="868">EU 868 MHz</option>
                                    <option value="915">US 915 MHz</option>
                                    <option value="923">AS 923 MHz</option>
                                    <option value="916">AU 915 MHz</option>
                                    <option value="865">IN 865 MHz</option>
                                </select>
                            </div>
                            <div id="ctrl-channel" style="display: flex; align-items: center; gap: 0.4rem;">
                                <span style="font-size: 0.8rem; font-weight: 500; color: var(--text-muted);">Channel:</span>
                                <select id="channel-select" onchange="setChannel(this.value)" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255,255,255,0.15); color: #fff; font-size: 0.75rem; padding: 0.15rem 0.4rem; border-radius: 6px; outline: none; cursor: pointer;">
                                </select>
                            </div>
                            <div id="ctrl-sniffer" style="display: flex; align-items: center; gap: 0.4rem;">
                                <span style="font-size: 0.8rem; font-weight: 500; color: var(--text-muted);">Sniffer:</span>
                                <label class="switch">
                                    <input type="checkbox" id="sniff-toggle" onchange="toggleSniff()">
                                    <span class="slider round"></span>
                                </label>
                            </div>
                        </div>
                        <div style="display: flex; justify-content: space-between; align-items: center; margin-top: 0.2rem;">
                            <p style="color: var(--text-muted); font-size: 0.72rem; margin: 0;">Listen frequency &amp; packet capture.</p>
                            <button id="btn-req-gps" onclick="requestGpsFix()" style="background: rgba(0, 210, 255, 0.12); border: 1px solid rgba(0, 210, 255, 0.3); color: #00d2ff; font-weight: 600; font-size: 0.7rem; padding: 0.2rem 0.45rem; border-radius: 4px; cursor: pointer; transition: all 0.2s;" title="Force station to turn on GPS and transmit coordinates on next check-in">📡 Request GPS</button>
                        </div>
                    </div>
                </div>
            </div>

            <div class="section-panel" style="margin-bottom: 1.5rem; padding: 1.5rem;">
                <div style="margin-bottom: 1rem; border-bottom: 1px solid var(--card-border); padding-bottom: 0.75rem; display: flex; flex-wrap: wrap; justify-content: space-between; align-items: flex-start; gap: 1rem;">
                    <div>
                        <h2 style="font-size: 1.2rem; margin-bottom: 0.25rem; border-bottom: none; padding-bottom: 0;">LoRa Packets</h2>
                        <p style="color: var(--text-muted); font-size: 0.85rem; margin: 0 0 0.5rem 0;">Real-time LoRa packet logs captured and transmitted by the local transceiver radio module. Packets are displayed in every pairing mode; only the paired station's data is uplinked.</p>
                        <div class="packet-filter-bar">
                            <span style="font-size: 0.75rem; color: var(--text-muted); font-weight: 600; margin-right: 0.2rem;">Filter:</span>
                            <button id="filter-wxm" class="filter-btn active filter-wxm" onclick="togglePacketFilter('weatherxm')"><span class="filter-dot"></span>WeatherXM P2P</button>
                            <button id="filter-ws1001" class="filter-btn active filter-ws1001" onclick="togglePacketFilter('ws1001')"><span class="filter-dot"></span>WS1001</button>
                            <button id="filter-ws1300" class="filter-btn active filter-ws1300" onclick="togglePacketFilter('ws1300')"><span class="filter-dot"></span>WS1300</button>
                            <button id="filter-meshtastic" class="filter-btn active filter-meshtastic" onclick="togglePacketFilter('meshtastic')"><span class="filter-dot"></span>Meshtastic</button>
                            <button id="filter-meshcore" class="filter-btn active filter-meshcore" onclick="togglePacketFilter('meshcore')"><span class="filter-dot"></span>MeshCore</button>
                            <button id="filter-other" class="filter-btn active filter-other" onclick="togglePacketFilter('other')"><span class="filter-dot"></span>Other</button>
                        </div>
                    </div>
                    <!-- Radio-level frame counter. Counts every frame the SX1262 demodulated,
                         decoded or not, so an empty table below can be read correctly: a rising
                         count means the radio hears traffic the decoder rejects; a flat count
                         means the radio is on the wrong frequency or modem config. -->
                    <div style="text-align: right; white-space: nowrap; font-size: 0.75rem; color: var(--text-muted); line-height: 1.5;">
                        <div>Frames demodulated: <span id="rx-total" style="color: #00d2ff; font-weight: 700; font-family: monospace;">0</span></div>
                        <div>Listening on <span id="rx-listen-desc" style="color: #00d2ff; font-family: monospace;">-</span></div>
                    </div>
                </div>
                <div style="overflow-x: auto; background: rgba(0,0,0,0.25); border-radius: 12px; border: 1px solid var(--card-border); padding: 0.5rem;">
                    <table class="table-packets" style="width: 100%; border-collapse: collapse; font-size: 0.85rem;">
                        <thead>
                            <tr style="border-bottom: 1px solid var(--card-border); text-align: left; color: var(--text-muted);">
                                <th style="width: 8%; padding: 0.5rem;" title="Timestamp when the LoRa packet was received and demodulated by the gateway.">Time <span class="th-info" title="Timestamp when the LoRa packet was received and demodulated by the gateway.">i</span></th>
                                <th style="width: 13%; padding: 0.5rem;" title="LoRa RF Link Quality:&#010;&#010;• RSSI (Received Signal Strength Indicator):&#010;  Best: -40 to -65 dBm (close / bench)&#010;  Good: -70 to -105 dBm (normal range)&#010;  Borderline: -115 to -122 dBm&#010;  Floor: -124 dBm (SF7 sensitivity limit)&#010;&#010;• SNR (Signal-to-Noise Ratio):&#010;  Best: +10 to +14 dB (clean signal)&#010;  Good: +3 to +10 dB&#010;  Borderline: -4 to -7.5 dB&#010;  Floor: -7.5 dB (SF7 demod limit)&#010;&#010;• Health (0-100%): Combined Link Quality score based on SNR margin and RSSI.">Signal <span class="th-info" title="LoRa RF Link Quality:&#010;&#010;• RSSI (Received Signal Strength Indicator):&#010;  Best: -40 to -65 dBm (close / bench)&#010;  Good: -70 to -105 dBm (normal range)&#010;  Borderline: -115 to -122 dBm&#010;  Floor: -124 dBm (SF7 sensitivity limit)&#010;&#010;• SNR (Signal-to-Noise Ratio):&#010;  Best: +10 to +14 dB (clean signal)&#010;  Good: +3 to +10 dB&#010;  Borderline: -4 to -7.5 dB&#010;  Floor: -7.5 dB (SF7 demod limit)&#010;&#010;• Health (0-100%): Combined Link Quality score based on SNR margin and RSSI.">i</span></th>
                                <th style="width: 8%; padding: 0.5rem;" title="Detected station model (e.g. WS1001, WS1300, Meshtastic, MeshCore) based on packet structure and signature.">Station <span class="th-info" title="Detected station model (e.g. WS1001, WS1300, Meshtastic, MeshCore) based on packet structure and signature.">i</span></th>
                                <th style="width: 8%; padding: 0.5rem;" title="Hardware / Station Identifier:&#010;• WeatherXM: 32-bit Station ID in hexadecimal (e.g. 0x0001ABCD)&#010;• Meshtastic: Node ID hex (e.g. !A1B2C3D4)">ID <span class="th-info" title="Hardware / Station Identifier:&#010;• WeatherXM: 32-bit Station ID in hexadecimal (e.g. 0x0001ABCD)&#010;• Meshtastic: Node ID hex (e.g. !A1B2C3D4)">i</span></th>
                                <th style="width: 11%; padding: 0.5rem;" title="Active RF channel slot and center frequency the packet was received on. Hover over row for modulation details (SF / BW / CR).">Slot / Freq <span class="th-info" title="Active RF channel slot and center frequency the packet was received on. Hover over row for modulation details (SF / BW / CR).">i</span></th>
                                <th style="width: 9%; padding: 0.5rem;" title="Packet type indicator:&#010;• WeatherXM: BOOT, ADV, SENSORS, GNSS&#010;• Meshtastic: MESH: TELEM, MESH: DEV, MESH: NODEINFO, MESH: POS, MESH: TEXT&#010;• MeshCore: MESHCORE: SIGNED, MESHCORE: OBS">Type <span class="th-info" title="Packet type indicator:&#010;• WeatherXM: BOOT, ADV, SENSORS, GNSS&#010;• Meshtastic: MESH: TELEM, MESH: DEV, MESH: NODEINFO, MESH: POS, MESH: TEXT&#010;• MeshCore: MESHCORE: SIGNED, MESHCORE: OBS">i</span></th>
                                <th style="width: 8%; padding: 0.5rem;" title="Protocol or firmware revision detected in the packet header.">Proto <span class="th-info" title="Protocol or firmware revision detected in the packet header.">i</span></th>
                                <th style="width: 23%; padding: 0.5rem;" title="Demodulated, decrypted, and parsed sensor metrics (temperature, humidity, wind, solar, etc.).">Decoded Parameters <span class="th-info" title="Demodulated, decrypted, and parsed sensor metrics (temperature, humidity, wind, solar, etc.).">i</span></th>
                                <th style="width: 12%; padding: 0.5rem;" title="Raw hexadecimal bytes of the received demodulated LoRa packet payload.">Raw Payload <span class="th-info" title="Raw hexadecimal bytes of the received demodulated LoRa packet payload.">i</span></th>
                            </tr>
                        </thead>
                        <tbody id="packet-tbody">
                            <tr>
                                <td colspan="9" style="text-align: center; color: var(--text-muted); padding: 2rem;">No packets received yet. Waiting...</td>
                            </tr>
                        </tbody>
                    </table>
                </div>
            </div>
        </div>

        <!-- API Documentation View -->
        <div id="page-docs" class="page-content" style="display: none;">
            <div class="section-panel" style="max-width: 900px; margin: 0 auto;">
                <h2>Local Gateway HTTP API Reference</h2>
                <p style="color: var(--text-muted); font-size: 0.95rem; margin-bottom: 2rem;">
                    The gateway exposes a set of RESTful HTTP endpoints for real-time diagnostics, remote station management, and hardware controls. 
                    These endpoints can be integrated into automation scripts, local home assistant dashboards, or used directly via curl/postman.
                </p>

                <div class="api-endpoints">
                    <!-- GET /llms.txt -->
                    <div class="api-card" style="border: 1px solid rgba(0, 245, 160, 0.35); border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; background: rgba(0, 245, 160, 0.03);">
                        <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.8rem;">
                            <div style="display: flex; align-items: center; gap: 0.8rem;">
                                <span class="badge-get" style="background: rgba(0, 245, 160, 0.15); color: #00f5a0; font-weight: 700; font-size: 0.8rem; padding: 0.25rem 0.6rem; border-radius: 6px; text-transform: uppercase;">GET</span>
                                <code style="font-size: 1.1rem; color: #fff; font-weight: 600; font-family: monospace;">/llms.txt</code>
                            </div>
                            <span style="font-size: 0.8rem; color: #00f5a0; font-weight: 600;">LLM / Agent Standard</span>
                        </div>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">
                            Token-efficient machine-readable reference adhering to the <code style="color: #00d2ff; font-family: monospace;">llms.txt</code> standard. Designed for AI coding agents, LLMs, custom scripts, and home automation. Includes complete JSON schemas for telemetry, history, RF diagnostics, mesh networking, SD logging, and control endpoints, plus Home Assistant configuration templates.
                        </p>
                        <div style="font-weight: 600; font-size: 0.8rem; color: var(--text-color); margin-bottom: 0.4rem;">Example Request:</div>
                        <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); border-radius: 8px; padding: 0.6rem 0.8rem; overflow-x: auto; margin-bottom: 0; gap: 1rem;">
                            <code style="font-size: 0.85rem; color: #e0e0e0; font-family: monospace; white-space: nowrap;">curl http://<span class="api-ip">192.168.1.100</span>/llms.txt</code>
                            <button onclick="testGet('/llms.txt')" style="background: rgba(0, 245, 160, 0.15); border: 1px solid rgba(0, 245, 160, 0.3); color: #00f5a0; font-weight: 600; font-size: 0.75rem; padding: 0.3rem 0.75rem; border-radius: 6px; cursor: pointer; transition: all 0.2s; white-space: nowrap; margin-left: auto;">Test (New Tab)</button>
                        </div>
                    </div>

                    <!-- GET /status -->
                    <div class="api-card" style="border: 1px solid var(--card-border); border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; background: rgba(255, 255, 255, 0.01);">
                        <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.8rem;">
                            <div style="display: flex; align-items: center; gap: 0.8rem;">
                                <span class="badge-get" style="background: rgba(0, 210, 255, 0.15); color: #00d2ff; font-weight: 700; font-size: 0.8rem; padding: 0.25rem 0.6rem; border-radius: 6px; text-transform: uppercase;">GET</span>
                                <code style="font-size: 1.1rem; color: #fff; font-weight: 600; font-family: monospace;">/status</code>
                            </div>
                            <span style="font-size: 0.8rem; color: var(--text-muted);">JSON Response</span>
                        </div>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">Retrieve live telemetry and comprehensive configuration parameters of the gateway and paired weather station (reception rates, RSSI, local settings, weather sensor values).</p>
                        <div style="font-weight: 600; font-size: 0.8rem; color: var(--text-color); margin-bottom: 0.4rem;">Example Request:</div>
                        <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); border-radius: 8px; padding: 0.6rem 0.8rem; overflow-x: auto; margin-bottom: 0; gap: 1rem;">
                            <code style="font-size: 0.85rem; color: #e0e0e0; font-family: monospace; white-space: nowrap;">curl http://<span class="api-ip">192.168.1.100</span>/status</code>
                            <button onclick="testGet('/status')" style="background: rgba(0, 210, 255, 0.15); border: 1px solid rgba(0, 210, 255, 0.3); color: #00d2ff; font-weight: 600; font-size: 0.75rem; padding: 0.3rem 0.75rem; border-radius: 6px; cursor: pointer; transition: all 0.2s; white-space: nowrap; margin-left: auto;">Test (New Tab)</button>
                        </div>
                    </div>

                    <!-- GET /packet_log -->
                    <div class="api-card" style="border: 1px solid var(--card-border); border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; background: rgba(255, 255, 255, 0.01);">
                        <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.8rem;">
                            <div style="display: flex; align-items: center; gap: 0.8rem;">
                                <span class="badge-get" style="background: rgba(0, 210, 255, 0.15); color: #00d2ff; font-weight: 700; font-size: 0.8rem; padding: 0.25rem 0.6rem; border-radius: 6px; text-transform: uppercase;">GET</span>
                                <code style="font-size: 1.1rem; color: #fff; font-weight: 600; font-family: monospace;">/packet_log</code>
                            </div>
                            <span style="font-size: 0.8rem; color: var(--text-muted);">JSON Response</span>
                        </div>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">Retrieve a list of the last 20-32 received LoRa/KISS packets containing signal data, paired station information, raw payloads, and parsed sensor values.</p>
                        <div style="font-weight: 600; font-size: 0.8rem; color: var(--text-color); margin-bottom: 0.4rem;">Example Request:</div>
                        <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); border-radius: 8px; padding: 0.6rem 0.8rem; overflow-x: auto; margin-bottom: 0; gap: 1rem;">
                            <code style="font-size: 0.85rem; color: #e0e0e0; font-family: monospace; white-space: nowrap;">curl http://<span class="api-ip">192.168.1.100</span>/packet_log</code>
                            <button onclick="testGet('/packet_log')" style="background: rgba(0, 210, 255, 0.15); border: 1px solid rgba(0, 210, 255, 0.3); color: #00d2ff; font-weight: 600; font-size: 0.75rem; padding: 0.3rem 0.75rem; border-radius: 6px; cursor: pointer; transition: all 0.2s; white-space: nowrap; margin-left: auto;">Test (New Tab)</button>
                        </div>
                    </div>

                    <!-- POST /toggle_sniff -->
                    <div class="api-card" style="border: 1px solid var(--card-border); border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; background: rgba(255, 255, 255, 0.01);">
                        <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.8rem;">
                            <div style="display: flex; align-items: center; gap: 0.8rem;">
                                <span class="badge-post" style="background: rgba(187, 134, 252, 0.15); color: #bb86fc; font-weight: 700; font-size: 0.8rem; padding: 0.25rem 0.6rem; border-radius: 6px; text-transform: uppercase;">POST</span>
                                <code style="font-size: 1.1rem; color: #fff; font-weight: 600; font-family: monospace;">/toggle_sniff</code>
                            </div>
                            <span style="font-size: 0.8rem; color: var(--text-muted);">JSON Response</span>
                        </div>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">Toggle gateway's live Sniff Mode. When enabled, the gateway records and shows packets from other nearby weather stations without pairing or interrupting telemetry pipelines.</p>
                        <div style="font-weight: 600; font-size: 0.8rem; color: var(--text-color); margin-bottom: 0.4rem;">Example Request:</div>
                        <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); border-radius: 8px; padding: 0.6rem 0.8rem; overflow-x: auto; margin-bottom: 0; gap: 1rem;">
                            <code style="font-size: 0.85rem; color: #e0e0e0; font-family: monospace; white-space: nowrap;">curl -X POST http://<span class="api-ip">192.168.1.100</span>/toggle_sniff</code>
                            <button onclick="testPost('/toggle_sniff')" style="background: rgba(187, 134, 252, 0.15); border: 1px solid rgba(187, 134, 252, 0.3); color: #bb86fc; font-weight: 600; font-size: 0.75rem; padding: 0.3rem 0.75rem; border-radius: 6px; cursor: pointer; transition: all 0.2s; white-space: nowrap; margin-left: auto;">Execute</button>
                        </div>
                    </div>

                    <!-- POST /toggle_mqtt -->
                    <div class="api-card" style="border: 1px solid var(--card-border); border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; background: rgba(255, 255, 255, 0.01);">
                        <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.8rem;">
                            <div style="display: flex; align-items: center; gap: 0.8rem;">
                                <span class="badge-post" style="background: rgba(187, 134, 252, 0.15); color: #bb86fc; font-weight: 700; font-size: 0.8rem; padding: 0.25rem 0.6rem; border-radius: 6px; text-transform: uppercase;">POST</span>
                                <code style="font-size: 1.1rem; color: #fff; font-weight: 600; font-family: monospace;">/toggle_mqtt</code>
                            </div>
                            <span style="font-size: 0.8rem; color: var(--text-muted);">JSON Response</span>
                        </div>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">Enable or disable the local MQTT client (WeatherXM connection). Disabling it saves local gateway power and limits external cloud telemetry transmissions.</p>
                        <div style="font-weight: 600; font-size: 0.8rem; color: var(--text-color); margin-bottom: 0.4rem;">Example Request:</div>
                        <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); border-radius: 8px; padding: 0.6rem 0.8rem; overflow-x: auto; margin-bottom: 0; gap: 1rem;">
                            <code style="font-size: 0.85rem; color: #e0e0e0; font-family: monospace; white-space: nowrap;">curl -X POST http://<span class="api-ip">192.168.1.100</span>/toggle_mqtt</code>
                            <button onclick="testPost('/toggle_mqtt')" style="background: rgba(187, 134, 252, 0.15); border: 1px solid rgba(187, 134, 252, 0.3); color: #bb86fc; font-weight: 600; font-size: 0.75rem; padding: 0.3rem 0.75rem; border-radius: 6px; cursor: pointer; transition: all 0.2s; white-space: nowrap; margin-left: auto;">Execute</button>
                        </div>
                    </div>

                    <!-- POST /set_station -->
                    <div class="api-card" style="border: 1px solid var(--card-border); border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; background: rgba(255, 255, 255, 0.01);">
                        <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.8rem;">
                            <div style="display: flex; align-items: center; gap: 0.8rem;">
                                <span class="badge-post" style="background: rgba(187, 134, 252, 0.15); color: #bb86fc; font-weight: 700; font-size: 0.8rem; padding: 0.25rem 0.6rem; border-radius: 6px; text-transform: uppercase;">POST</span>
                                <code style="font-size: 1.1rem; color: #fff; font-weight: 600; font-family: monospace;">/set_station</code>
                            </div>
                            <span style="font-size: 0.8rem; color: var(--text-muted);">Query Parameter</span>
                        </div>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">Configure the active weather station receiver profile. Profile value is saved persistently to NVS storage.</p>
                        <div style="margin-bottom: 1rem; font-size: 0.85rem;">
                            <strong style="color: var(--text-color);">Parameters:</strong>
                            <ul style="color: var(--text-muted); margin-top: 0.3rem; padding-left: 1.2rem;">
                                <li><code style="font-family: monospace; color: #00d2ff;">type=1300</code>: Listen only for WS1300 Multi-Channel station</li>
                                <li><code style="font-family: monospace; color: #00d2ff;">type=1001</code>: Listen only for WS1001 Single Channel station</li>
                                <li><code style="font-family: monospace; color: #00d2ff;">type=0</code>: Listen/Scan all supported models (transient)</li>
                            </ul>
                        </div>
                        <div style="font-weight: 600; font-size: 0.8rem; color: var(--text-color); margin-bottom: 0.4rem;">Example Request:</div>
                        <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); border-radius: 8px; padding: 0.6rem 0.8rem; overflow-x: auto; margin-bottom: 0; gap: 1rem;">
                            <code style="font-size: 0.85rem; color: #e0e0e0; font-family: monospace; white-space: nowrap;">curl -X POST "http://<span class="api-ip">192.168.1.100</span>/set_station?type=1300"</code>
                            <div style="display: flex; align-items: center; gap: 0.5rem; margin-left: auto;">
                                <select id="param-set_station" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #fff; font-size: 0.75rem; padding: 0.25rem 0.4rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="1300">WS1300 (Multi-Channel)</option>
                                    <option value="1001">WS1001 (Single Channel)</option>
                                    <option value="0">ALL (Scan)</option>
                                </select>
                                <button onclick="testPost('/set_station')" style="background: rgba(187, 134, 252, 0.15); border: 1px solid rgba(187, 134, 252, 0.3); color: #bb86fc; font-weight: 600; font-size: 0.75rem; padding: 0.3rem 0.75rem; border-radius: 6px; cursor: pointer; transition: all 0.2s; white-space: nowrap;">Execute</button>
                            </div>
                        </div>
                    </div>

                    <!-- POST /set_channel -->
                    <div class="api-card" style="border: 1px solid var(--card-border); border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; background: rgba(255, 255, 255, 0.01);">
                        <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.8rem;">
                            <div style="display: flex; align-items: center; gap: 0.8rem;">
                                <span class="badge-post" style="background: rgba(187, 134, 252, 0.15); color: #bb86fc; font-weight: 700; font-size: 0.8rem; padding: 0.25rem 0.6rem; border-radius: 6px; text-transform: uppercase;">POST</span>
                                <code style="font-size: 1.1rem; color: #fff; font-weight: 600; font-family: monospace;">/set_channel</code>
                            </div>
                            <span style="font-size: 0.8rem; color: var(--text-muted);">Query Parameter</span>
                        </div>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">Manually lock or override the radio frequency channel on which the gateway listens for weather station packets (diagnostic lock).</p>
                        <div style="margin-bottom: 1rem; font-size: 0.85rem;">
                            <strong style="color: var(--text-color);">Parameters:</strong>
                            <ul style="color: var(--text-muted); margin-top: 0.3rem; padding-left: 1.2rem;">
                                <li><code style="font-family: monospace; color: #00d2ff;">slot=-1</code>: Enable Auto sweep mode (normal)</li>
                                <li><code style="font-family: monospace; color: #00d2ff;">slot=0</code>: Lock to WS1001 Single Channel base frequency</li>
                                <li><code style="font-family: monospace; color: #00d2ff;">slot=1..8</code>: Lock to WS1300 Sub-GHz Channels 0 to 7</li>
                            </ul>
                        </div>
                        <div style="font-weight: 600; font-size: 0.8rem; color: var(--text-color); margin-bottom: 0.4rem;">Example Request:</div>
                        <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); border-radius: 8px; padding: 0.6rem 0.8rem; overflow-x: auto; margin-bottom: 0; gap: 1rem;">
                            <code style="font-size: 0.85rem; color: #e0e0e0; font-family: monospace; white-space: nowrap;">curl -X POST "http://<span class="api-ip">192.168.1.100</span>/set_channel?slot=3"</code>
                            <div style="display: flex; align-items: center; gap: 0.5rem; margin-left: auto;">
                                <select id="param-set_channel" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #fff; font-size: 0.75rem; padding: 0.25rem 0.4rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="-1">Auto Sweep</option>
                                    <option value="0">WS1001 (Single Channel)</option>
                                    <option value="1">Channel 0</option>
                                    <option value="2">Channel 1</option>
                                    <option value="3" selected>Channel 2</option>
                                    <option value="4">Channel 3</option>
                                    <option value="5">Channel 4</option>
                                    <option value="6">Channel 5</option>
                                    <option value="7">Channel 6</option>
                                    <option value="8">Channel 7</option>
                                </select>
                                <button onclick="testPost('/set_channel')" style="background: rgba(187, 134, 252, 0.15); border: 1px solid rgba(187, 134, 252, 0.3); color: #bb86fc; font-weight: 600; font-size: 0.75rem; padding: 0.3rem 0.75rem; border-radius: 6px; cursor: pointer; transition: all 0.2s; white-space: nowrap;">Execute</button>
                            </div>
                        </div>
                    </div>

                    <!-- POST /remote_reboot -->
                    <div class="api-card" style="border: 1px solid var(--card-border); border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; background: rgba(255, 255, 255, 0.01);">
                        <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.8rem;">
                            <div style="display: flex; align-items: center; gap: 0.8rem;">
                                <span class="badge-post" style="background: rgba(187, 134, 252, 0.15); color: #bb86fc; font-weight: 700; font-size: 0.8rem; padding: 0.25rem 0.6rem; border-radius: 6px; text-transform: uppercase;">POST</span>
                                <code style="font-size: 1.1rem; color: #fff; font-weight: 600; font-family: monospace;">/remote_reboot</code>
                            </div>
                            <span style="font-size: 0.8rem; color: var(--text-muted);">Query Parameter</span>
                        </div>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">Arm or disarm a remote hardware reboot of the paired WS1300 station. Because WS1300 is a low-power, polling-based device, this command stays armed until the station's next packet check-in, where the reboot command is safely delivered over the radio link.</p>
                        <div style="margin-bottom: 1rem; font-size: 0.85rem;">
                            <strong style="color: var(--text-color);">Parameters:</strong>
                            <ul style="color: var(--text-muted); margin-top: 0.3rem; padding-left: 1.2rem;">
                                <li><code style="font-family: monospace; color: #00d2ff;">enable=1</code>: Arm remote reboot (default)</li>
                                <li><code style="font-family: monospace; color: #00d2ff;">enable=0</code>: Cancel a pending reboot command</li>
                            </ul>
                        </div>
                        <div style="font-weight: 600; font-size: 0.8rem; color: var(--text-color); margin-bottom: 0.4rem;">Example Request:</div>
                        <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); border-radius: 8px; padding: 0.6rem 0.8rem; overflow-x: auto; margin-bottom: 0; gap: 1rem;">
                            <code style="font-size: 0.85rem; color: #e0e0e0; font-family: monospace; white-space: nowrap;">curl -X POST "http://<span class="api-ip">192.168.1.100</span>/remote_reboot?enable=1"</code>
                            <div style="display: flex; align-items: center; gap: 0.5rem; margin-left: auto;">
                                <select id="param-remote_reboot" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #fff; font-size: 0.75rem; padding: 0.25rem 0.4rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="1">Arm Reboot (1)</option>
                                    <option value="0">Cancel Reboot (0)</option>
                                </select>
                                <button onclick="testPost('/remote_reboot')" style="background: rgba(187, 134, 252, 0.15); border: 1px solid rgba(187, 134, 252, 0.3); color: #bb86fc; font-weight: 600; font-size: 0.75rem; padding: 0.3rem 0.75rem; border-radius: 6px; cursor: pointer; transition: all 0.2s; white-space: nowrap;">Execute</button>
                            </div>
                        </div>
                    </div>

                    <!-- POST /ws1300_kick_station -->
                    <div class="api-card" style="border: 1px solid var(--card-border); border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; background: rgba(255, 255, 255, 0.01);">
                        <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.8rem;">
                            <div style="display: flex; align-items: center; gap: 0.8rem;">
                                <span class="badge-post" style="background: rgba(187, 134, 252, 0.15); color: #bb86fc; font-weight: 700; font-size: 0.8rem; padding: 0.25rem 0.6rem; border-radius: 6px; text-transform: uppercase;">POST</span>
                                <code style="font-size: 1.1rem; color: #fff; font-weight: 600; font-family: monospace;">/ws1300_kick_station</code>
                            </div>
                            <span style="font-size: 0.8rem; color: var(--text-muted);">Query Parameter</span>
                        </div>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">Arms a one-shot LoRa CMD-reboot for the next sniffed WS1300 packet. Forces the station to reboot and transmit a pairable BOOT packet containing its public key and serial number for over-the-air re-pairing.</p>
                        <div style="margin-bottom: 1rem; font-size: 0.85rem;">
                            <strong style="color: var(--text-color);">Parameters:</strong>
                            <ul style="color: var(--text-muted); margin-top: 0.3rem; padding-left: 1.2rem;">
                                <li><code style="font-family: monospace; color: #00d2ff;">id=0x...</code>: Optional target station ID filter (e.g. 0xBFE45699; 0 = first heard)</li>
                                <li><code style="font-family: monospace; color: #00d2ff;">cancel=1</code>: Disarm / cancel the pending kick command</li>
                            </ul>
                        </div>
                        <div style="font-weight: 600; font-size: 0.8rem; color: var(--text-color); margin-bottom: 0.4rem;">Example Request:</div>
                        <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); border-radius: 8px; padding: 0.6rem 0.8rem; overflow-x: auto; margin-bottom: 0; gap: 1rem;">
                            <code style="font-size: 0.85rem; color: #e0e0e0; font-family: monospace; white-space: nowrap;">curl -X POST "http://<span class="api-ip">192.168.1.100</span>/ws1300_kick_station"</code>
                            <div style="display: flex; align-items: center; gap: 0.5rem; margin-left: auto;">
                                <select id="param-ws1300_kick" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #fff; font-size: 0.75rem; padding: 0.25rem 0.4rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="0">Arm Kick (0)</option>
                                    <option value="1">Cancel Kick (1)</option>
                                </select>
                                <button onclick="testPost('/ws1300_kick_station')" style="background: rgba(187, 134, 252, 0.15); border: 1px solid rgba(187, 134, 252, 0.3); color: #bb86fc; font-weight: 600; font-size: 0.75rem; padding: 0.3rem 0.75rem; border-radius: 6px; cursor: pointer; transition: all 0.2s; white-space: nowrap;">Execute</button>
                            </div>
                        </div>
                    </div>

                    <!-- POST /set_tx_power -->
                    <div class="api-card" style="border: 1px solid var(--card-border); border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; background: rgba(255, 255, 255, 0.01);">
                        <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.8rem;">
                            <div style="display: flex; align-items: center; gap: 0.8rem;">
                                <span class="badge-post" style="background: rgba(187, 134, 252, 0.15); color: #bb86fc; font-weight: 700; font-size: 0.8rem; padding: 0.25rem 0.6rem; border-radius: 6px; text-transform: uppercase;">POST</span>
                                <code style="font-size: 1.1rem; color: #fff; font-weight: 600; font-family: monospace;">/set_tx_power</code>
                            </div>
                            <span style="font-size: 0.8rem; color: var(--text-muted);">Query Parameter</span>
                        </div>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">Manually override transmission (TX) power settings for the paired WS1300 station to improve range or save battery.</p>
                        <div style="margin-bottom: 1rem; font-size: 0.85rem;">
                            <strong style="color: var(--text-color);">Parameters:</strong>
                            <ul style="color: var(--text-muted); margin-top: 0.3rem; padding-left: 1.2rem;">
                                <li><code style="font-family: monospace; color: #00d2ff;">dbm=0</code>: Auto mode (adaptive power based on gateway distance and acknowledgement loss)</li>
                                <li><code style="font-family: monospace; color: #00d2ff;">dbm=2</code>: Low power mode (2 dBm, workbench test mode)</li>
                                <li><code style="font-family: monospace; color: #00d2ff;">dbm=22</code>: High power mode (22 dBm, max range)</li>
                                <li><code style="font-family: monospace; color: #00d2ff;">dbm=1..22</code>: Custom fixed transmission power in dBm (clamped to physical station hardware limit)</li>
                            </ul>
                        </div>
                        <div style="font-weight: 600; font-size: 0.8rem; color: var(--text-color); margin-bottom: 0.4rem;">Example Request:</div>
                        <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); border-radius: 8px; padding: 0.6rem 0.8rem; overflow-x: auto; margin-bottom: 0; gap: 1rem;">
                            <code style="font-size: 0.85rem; color: #e0e0e0; font-family: monospace; white-space: nowrap;">curl -X POST "http://<span class="api-ip">192.168.1.100</span>/set_tx_power?dbm=10"</code>
                            <div style="display: flex; align-items: center; gap: 0.5rem; margin-left: auto;">
                                <select id="param-set_tx_power" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #fff; font-size: 0.75rem; padding: 0.25rem 0.4rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="0">Auto (Adaptive)</option>
                                    <option value="22">22 dBm (High)</option>
                                    <option value="14">14 dBm</option>
                                    <option value="12">12 dBm</option>
                                    <option value="10" selected>10 dBm</option>
                                    <option value="8">8 dBm</option>
                                    <option value="5">5 dBm</option>
                                    <option value="2">2 dBm (Low)</option>
                                </select>
                                <button onclick="testPost('/set_tx_power')" style="background: rgba(187, 134, 252, 0.15); border: 1px solid rgba(187, 134, 252, 0.3); color: #bb86fc; font-weight: 600; font-size: 0.75rem; padding: 0.3rem 0.75rem; border-radius: 6px; cursor: pointer; transition: all 0.2s; white-space: nowrap;">Execute</button>
                            </div>
                        </div>
                    </div>

                    <!-- POST /set_tx_interval -->
                    <div class="api-card" style="border: 1px solid var(--card-border); border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; background: rgba(255, 255, 255, 0.01);">
                        <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.8rem;">
                            <div style="display: flex; align-items: center; gap: 0.8rem;">
                                <span class="badge-post" style="background: rgba(187, 134, 252, 0.15); color: #bb86fc; font-weight: 700; font-size: 0.8rem; padding: 0.25rem 0.6rem; border-radius: 6px; text-transform: uppercase;">POST</span>
                                <code style="font-size: 1.1rem; color: #fff; font-weight: 600; font-family: monospace;">/set_tx_interval</code>
                            </div>
                            <span style="font-size: 0.8rem; color: var(--text-muted);">Query Parameter</span>
                        </div>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">Set transmission interval for the paired WS1300 station. (Interval is clamped to 20s - 240s, and rounded to a multiple of 20s).</p>
                        <div style="margin-bottom: 1rem; font-size: 0.85rem;">
                            <strong style="color: var(--text-color);">Parameters:</strong>
                            <ul style="color: var(--text-muted); margin-top: 0.3rem; padding-left: 1.2rem;">
                                <li><code style="font-family: monospace; color: #00d2ff;">sec=0</code>: Default interval (fixed 20 seconds)</li>
                                <li><code style="font-family: monospace; color: #00d2ff;">sec=20..240</code>: Custom interval in seconds</li>
                            </ul>
                        </div>
                        <div style="font-weight: 600; font-size: 0.8rem; color: var(--text-color); margin-bottom: 0.4rem;">Example Request:</div>
                        <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); border-radius: 8px; padding: 0.6rem 0.8rem; overflow-x: auto; margin-bottom: 0; gap: 1rem;">
                            <code style="font-size: 0.85rem; color: #e0e0e0; font-family: monospace; white-space: nowrap;">curl -X POST "http://<span class="api-ip">192.168.1.100</span>/set_tx_interval?sec=60"</code>
                            <div style="display: flex; align-items: center; gap: 0.5rem; margin-left: auto;">
                                <select id="param-set_tx_interval" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #fff; font-size: 0.75rem; padding: 0.25rem 0.4rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="0">Default (20s)</option>
                                    <option value="20">20s</option>
                                    <option value="40">40s</option>
                                    <option value="60" selected>60s</option>
                                    <option value="120">120s</option>
                                    <option value="180">180s</option>
                                    <option value="240">240s</option>
                                </select>
                                <button onclick="testPost('/set_tx_interval')" style="background: rgba(187, 134, 252, 0.15); border: 1px solid rgba(187, 134, 252, 0.3); color: #bb86fc; font-weight: 600; font-size: 0.75rem; padding: 0.3rem 0.75rem; border-radius: 6px; cursor: pointer; transition: all 0.2s; white-space: nowrap;">Execute</button>
                            </div>
                        </div>
                    </div>

                    <!-- GET /api/v1/meshtastic/config -->
                    <div class="api-card" style="border: 1px solid var(--card-border); border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; background: rgba(255, 255, 255, 0.01);">
                        <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.8rem;">
                            <div style="display: flex; align-items: center; gap: 0.8rem;">
                                <span class="badge-get" style="background: rgba(0, 210, 255, 0.15); color: #00d2ff; font-weight: 700; font-size: 0.8rem; padding: 0.25rem 0.6rem; border-radius: 6px; text-transform: uppercase;">GET</span>
                                <code style="font-size: 1.1rem; color: #fff; font-weight: 600; font-family: monospace;">/api/v1/meshtastic/config</code>
                            </div>
                            <span style="font-size: 0.8rem; color: var(--text-muted);">JSON Response</span>
                        </div>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">Read the state of the Meshtastic environment telemetry beacon: enablement, interval, the gateway's Meshtastic node identity, the RF frequency in use, and transmit counters. <code style="font-family: monospace; color: #00d2ff;">seconds_since_last</code> is <code style="font-family: monospace; color: #00d2ff;">4294967295</code> when nothing has been sent yet.</p>
                        <div style="font-weight: 600; font-size: 0.8rem; color: var(--text-color); margin-bottom: 0.4rem;">Example Request:</div>
                        <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); border-radius: 8px; padding: 0.6rem 0.8rem; overflow-x: auto; margin-bottom: 0; gap: 1rem;">
                            <code style="font-size: 0.85rem; color: #e0e0e0; font-family: monospace; white-space: nowrap;">curl http://<span class="api-ip">192.168.1.100</span>/api/v1/meshtastic/config</code>
                            <button onclick="testGet('/api/v1/meshtastic/config')" style="background: rgba(0, 210, 255, 0.15); border: 1px solid rgba(0, 210, 255, 0.3); color: #00d2ff; font-weight: 600; font-size: 0.75rem; padding: 0.3rem 0.75rem; border-radius: 6px; cursor: pointer; transition: all 0.2s; white-space: nowrap; margin-left: auto;">Test (New Tab)</button>
                        </div>
                    </div>

                    <!-- POST /api/v1/meshtastic/config -->
                    <div class="api-card" style="border: 1px solid var(--card-border); border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; background: rgba(255, 255, 255, 0.01);">
                        <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.8rem;">
                            <div style="display: flex; align-items: center; gap: 0.8rem;">
                                <span class="badge-post" style="background: rgba(187, 134, 252, 0.15); color: #bb86fc; font-weight: 700; font-size: 0.8rem; padding: 0.25rem 0.6rem; border-radius: 6px; text-transform: uppercase;">POST</span>
                                <code style="font-size: 1.1rem; color: #fff; font-weight: 600; font-family: monospace;">/api/v1/meshtastic/config</code>
                            </div>
                            <span style="font-size: 0.8rem; color: var(--text-muted);">Query Parameters</span>
                        </div>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">Enable or disable the beacon and set its broadcast interval. Both settings persist in NVS. Returns the full config object, same shape as the GET.</p>
                        <div style="margin-bottom: 1rem; font-size: 0.85rem;">
                            <strong style="color: var(--text-color);">Parameters:</strong>
                            <ul style="color: var(--text-muted); margin-top: 0.3rem; padding-left: 1.2rem;">
                                <li><code style="font-family: monospace; color: #00d2ff;">enabled=true|false</code>: Turn the beacon on or off (<code style="font-family: monospace; color: #00d2ff;">1</code>/<code style="font-family: monospace; color: #00d2ff;">0</code> also accepted)</li>
                                <li><code style="font-family: monospace; color: #00d2ff;">interval=300..3600</code>: Seconds between broadcasts, clamped to a 60 s minimum</li>
                            </ul>
                        </div>
                        <div style="font-weight: 600; font-size: 0.8rem; color: var(--text-color); margin-bottom: 0.4rem;">Example Request:</div>
                        <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); border-radius: 8px; padding: 0.6rem 0.8rem; overflow-x: auto; margin-bottom: 0; gap: 1rem;">
                            <code style="font-size: 0.85rem; color: #e0e0e0; font-family: monospace; white-space: nowrap;">curl -X POST "http://<span class="api-ip">192.168.1.100</span>/api/v1/meshtastic/config?enabled=true&amp;interval=3600"</code>
                            <div style="display: flex; align-items: center; gap: 0.5rem; margin-left: auto;">
                                <select id="param-mesh_enabled" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #fff; font-size: 0.75rem; padding: 0.25rem 0.4rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="true" selected>Enabled</option>
                                    <option value="false">Disabled</option>
                                </select>
                                <select id="param-mesh_interval" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #fff; font-size: 0.75rem; padding: 0.25rem 0.4rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="300">5 min</option>
                                    <option value="600">10 min</option>
                                    <option value="1200">20 min</option>
                                    <option value="1800">30 min</option>
                                    <option value="3600" selected>1 hour</option>
                                </select>
                                <button onclick="testPost('/api/v1/meshtastic/config')" style="background: rgba(187, 134, 252, 0.15); border: 1px solid rgba(187, 134, 252, 0.3); color: #bb86fc; font-weight: 600; font-size: 0.75rem; padding: 0.3rem 0.75rem; border-radius: 6px; cursor: pointer; transition: all 0.2s; white-space: nowrap;">Execute</button>
                            </div>
                        </div>
                    </div>

                    <!-- POST /api/v1/meshtastic/send_telemetry -->
                    <div class="api-card" style="border: 1px solid var(--card-border); border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; background: rgba(255, 255, 255, 0.01);">
                        <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.8rem;">
                            <div style="display: flex; align-items: center; gap: 0.8rem;">
                                <span class="badge-post" style="background: rgba(187, 134, 252, 0.15); color: #bb86fc; font-weight: 700; font-size: 0.8rem; padding: 0.25rem 0.6rem; border-radius: 6px; text-transform: uppercase;">POST</span>
                                <code style="font-size: 1.1rem; color: #fff; font-weight: 600; font-family: monospace;">/api/v1/meshtastic/send_telemetry</code>
                            </div>
                            <span style="font-size: 0.8rem; color: var(--text-muted);">JSON Response</span>
                        </div>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">Queue an immediate Meshtastic broadcast of the latest station observation. Returns straight away with <code style="font-family: monospace; color: #00d2ff;">queued: true</code> - it does <em>not</em> wait for the transmit.</p>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">The SX1262 is shared with the weather station link, so the frame only goes out at the next safe radio window: just after the gateway finishes servicing a station packet. Expect up to one station interval (20-240 s) of delay. Poll <code style="font-family: monospace; color: #00d2ff;">/api/v1/meshtastic/config</code> and watch <code style="font-family: monospace; color: #00d2ff;">sent_count</code> increment to confirm it went out.</p>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">Works even when the beacon is disabled - a manual request is honoured once. Alias: <code style="font-family: monospace; color: #00d2ff;">/api/v1/mesh/send_telemetry</code>.</p>
                        <div style="font-weight: 600; font-size: 0.8rem; color: var(--text-color); margin-bottom: 0.4rem;">Example Request:</div>
                        <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); border-radius: 8px; padding: 0.6rem 0.8rem; overflow-x: auto; margin-bottom: 0; gap: 1rem;">
                            <code style="font-size: 0.85rem; color: #e0e0e0; font-family: monospace; white-space: nowrap;">curl -X POST http://<span class="api-ip">192.168.1.100</span>/api/v1/meshtastic/send_telemetry</code>
                            <button onclick="testPost('/api/v1/meshtastic/send_telemetry')" style="background: rgba(187, 134, 252, 0.15); border: 1px solid rgba(187, 134, 252, 0.3); color: #bb86fc; font-weight: 600; font-size: 0.75rem; padding: 0.3rem 0.75rem; border-radius: 6px; cursor: pointer; transition: all 0.2s; white-space: nowrap; margin-left: auto;">Execute</button>
                        </div>
                    </div>

                    <!-- GET /api/v1/meshcore/config -->
                    <div class="api-card" style="border: 1px solid var(--card-border); border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; background: rgba(255, 255, 255, 0.01);">
                        <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.8rem;">
                            <div style="display: flex; align-items: center; gap: 0.8rem;">
                                <span class="badge-get" style="background: rgba(0, 210, 255, 0.15); color: #00d2ff; font-weight: 700; font-size: 0.8rem; padding: 0.25rem 0.6rem; border-radius: 6px; text-transform: uppercase;">GET</span>
                                <code style="font-size: 1.1rem; color: #fff; font-weight: 600; font-family: monospace;">/api/v1/meshcore/config</code>
                            </div>
                            <span style="font-size: 0.8rem; color: var(--text-muted);">JSON Response</span>
                        </div>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">Read MeshCore configuration and telemetry state: multi-station forwarding, gateway sensor node enablement, interval, RF profile, transmit power, RF frequency, and transmit counters.</p>
                        <div style="font-weight: 600; font-size: 0.8rem; color: var(--text-color); margin-bottom: 0.4rem;">Example Request:</div>
                        <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); border-radius: 8px; padding: 0.6rem 0.8rem; overflow-x: auto; margin-bottom: 0; gap: 1rem;">
                            <code style="font-size: 0.85rem; color: #e0e0e0; font-family: monospace; white-space: nowrap;">curl http://<span class="api-ip">192.168.1.100</span>/api/v1/meshcore/config</code>
                            <button onclick="testGet('/api/v1/meshcore/config')" style="background: rgba(0, 210, 255, 0.15); border: 1px solid rgba(0, 210, 255, 0.3); color: #00d2ff; font-weight: 600; font-size: 0.75rem; padding: 0.3rem 0.75rem; border-radius: 6px; cursor: pointer; transition: all 0.2s; white-space: nowrap; margin-left: auto;">Test (New Tab)</button>
                        </div>
                    </div>

                    <!-- POST /api/v1/meshcore/config -->
                    <div class="api-card" style="border: 1px solid var(--card-border); border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; background: rgba(255, 255, 255, 0.01);">
                        <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.8rem;">
                            <div style="display: flex; align-items: center; gap: 0.8rem;">
                                <span class="badge-post" style="background: rgba(187, 134, 252, 0.15); color: #bb86fc; font-weight: 700; font-size: 0.8rem; padding: 0.25rem 0.6rem; border-radius: 6px; text-transform: uppercase;">POST</span>
                                <code style="font-size: 1.1rem; color: #fff; font-weight: 600; font-family: monospace;">/api/v1/meshcore/config</code>
                            </div>
                            <span style="font-size: 0.8rem; color: var(--text-muted);">Query Parameters</span>
                        </div>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">Configure MeshCore gateway telemetry settings (enablement, interval, RF profile preset, transmit power) and multi-station forwarding.</p>
                        <div style="margin-bottom: 1rem; font-size: 0.85rem;">
                            <strong style="color: var(--text-color);">Parameters:</strong>
                            <ul style="color: var(--text-muted); margin-top: 0.3rem; padding-left: 1.2rem;">
                                <li><code style="font-family: monospace; color: #00d2ff;">gw_enabled=true|false</code>: Turn gateway MeshCore telemetry broadcasts on or off</li>
                                <li><code style="font-family: monospace; color: #00d2ff;">gw_interval=300..3600</code>: Seconds between broadcasts (clamped to 60s min)</li>
                                <li><code style="font-family: monospace; color: #00d2ff;">gw_profile=0..3</code>: RF profile (0: LongRange, 1: Medium, 2: Narrow Extreme, 3: Fast)</li>
                                <li><code style="font-family: monospace; color: #00d2ff;">gw_tx_power_high=true|false</code>: High TX power toggle</li>
                                <li><code style="font-family: monospace; color: #00d2ff;">forwarding=true|false</code>: Multi-station forwarding enable</li>
                            </ul>
                        </div>
                        <div style="font-weight: 600; font-size: 0.8rem; color: var(--text-color); margin-bottom: 0.4rem;">Example Request:</div>
                        <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); border-radius: 8px; padding: 0.6rem 0.8rem; overflow-x: auto; margin-bottom: 0; gap: 1rem;">
                            <code style="font-size: 0.85rem; color: #e0e0e0; font-family: monospace; white-space: nowrap;">curl -X POST "http://<span class="api-ip">192.168.1.100</span>/api/v1/meshcore/config?gw_enabled=true&amp;gw_interval=3600&amp;gw_profile=0"</code>
                            <div style="display: flex; align-items: center; gap: 0.5rem; margin-left: auto;">
                                <select id="param-meshcore_enabled" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #fff; font-size: 0.75rem; padding: 0.25rem 0.4rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="true" selected>Enabled</option>
                                    <option value="false">Disabled</option>
                                </select>
                                <select id="param-meshcore_interval" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #fff; font-size: 0.75rem; padding: 0.25rem 0.4rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="300">5 min</option>
                                    <option value="600">10 min</option>
                                    <option value="1800">30 min</option>
                                    <option value="3600" selected>1 hour</option>
                                </select>
                                <select id="param-meshcore_profile" style="background: rgba(30, 30, 30, 0.8); border: 1px solid rgba(255, 255, 255, 0.15); color: #fff; font-size: 0.75rem; padding: 0.25rem 0.4rem; border-radius: 6px; outline: none; cursor: pointer;">
                                    <option value="0" selected>0: LongRange</option>
                                    <option value="1">1: Medium</option>
                                    <option value="2">2: Narrow</option>
                                    <option value="3">3: Fast</option>
                                </select>
                                <button onclick="testPost('/api/v1/meshcore/config')" style="background: rgba(187, 134, 252, 0.15); border: 1px solid rgba(187, 134, 252, 0.3); color: #bb86fc; font-weight: 600; font-size: 0.75rem; padding: 0.3rem 0.75rem; border-radius: 6px; cursor: pointer; transition: all 0.2s; white-space: nowrap;">Execute</button>
                            </div>
                        </div>
                    </div>

                    <!-- POST /api/v1/meshcore/send_telemetry -->
                    <div class="api-card" style="border: 1px solid var(--card-border); border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; background: rgba(255, 255, 255, 0.01);">
                        <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.8rem;">
                            <div style="display: flex; align-items: center; gap: 0.8rem;">
                                <span class="badge-post" style="background: rgba(187, 134, 252, 0.15); color: #bb86fc; font-weight: 700; font-size: 0.8rem; padding: 0.25rem 0.6rem; border-radius: 6px; text-transform: uppercase;">POST</span>
                                <code style="font-size: 1.1rem; color: #fff; font-weight: 600; font-family: monospace;">/api/v1/meshcore/send_telemetry</code>
                            </div>
                            <span style="font-size: 0.8rem; color: var(--text-muted);">JSON Response</span>
                        </div>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">Queue an immediate MeshCore encrypted group observation broadcast (0x19 GRP_DATA). Returns straight away with <code style="font-family: monospace; color: #00d2ff;">queued: true</code>.</p>
                        <div style="font-weight: 600; font-size: 0.8rem; color: var(--text-color); margin-bottom: 0.4rem;">Example Request:</div>
                        <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); border-radius: 8px; padding: 0.6rem 0.8rem; overflow-x: auto; margin-bottom: 0; gap: 1rem;">
                            <code style="font-size: 0.85rem; color: #e0e0e0; font-family: monospace; white-space: nowrap;">curl -X POST http://<span class="api-ip">192.168.1.100</span>/api/v1/meshcore/send_telemetry</code>
                            <button onclick="testPost('/api/v1/meshcore/send_telemetry')" style="background: rgba(187, 134, 252, 0.15); border: 1px solid rgba(187, 134, 252, 0.3); color: #bb86fc; font-weight: 600; font-size: 0.75rem; padding: 0.3rem 0.75rem; border-radius: 6px; cursor: pointer; transition: all 0.2s; white-space: nowrap; margin-left: auto;">Execute</button>
                        </div>
                    </div>

                    <!-- GET /api/v1/weather_policy -->
                    <div class="api-card" style="border: 1px solid var(--card-border); border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; background: rgba(255, 255, 255, 0.01);">
                        <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.8rem;">
                            <div style="display: flex; align-items: center; gap: 0.8rem;">
                                <span class="badge-get" style="background: rgba(0, 210, 255, 0.15); color: #00d2ff; font-weight: 700; font-size: 0.8rem; padding: 0.25rem 0.6rem; border-radius: 6px; text-transform: uppercase;">GET</span>
                                <code style="font-size: 1.1rem; color: #fff; font-weight: 600; font-family: monospace;">/api/v1/weather_policy</code>
                            </div>
                            <span style="font-size: 0.8rem; color: var(--text-muted);">JSON Response</span>
                        </div>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">Read adaptive weather publishing policy thresholds, cooldowns, hourly budgets, enabled flags, CRC16 verification checksum, and live mesh evaluator diagnostics.</p>
                        <div style="font-weight: 600; font-size: 0.8rem; color: var(--text-color); margin-bottom: 0.4rem;">Example Request:</div>
                        <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); border-radius: 8px; padding: 0.6rem 0.8rem; overflow-x: auto; margin-bottom: 0; gap: 1rem;">
                            <code style="font-size: 0.85rem; color: #e0e0e0; font-family: monospace; white-space: nowrap;">curl http://<span class="api-ip">192.168.1.100</span>/api/v1/weather_policy</code>
                            <button onclick="testGet('/api/v1/weather_policy')" style="background: rgba(0, 210, 255, 0.15); border: 1px solid rgba(0, 210, 255, 0.3); color: #00d2ff; font-weight: 600; font-size: 0.75rem; padding: 0.3rem 0.75rem; border-radius: 6px; cursor: pointer; transition: all 0.2s; white-space: nowrap; margin-left: auto;">Test (New Tab)</button>
                        </div>
                    </div>

                    <!-- POST /api/v1/weather_policy -->
                    <div class="api-card" style="border: 1px solid var(--card-border); border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; background: rgba(255, 255, 255, 0.01);">
                        <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.8rem;">
                            <div style="display: flex; align-items: center; gap: 0.8rem;">
                                <span class="badge-post" style="background: rgba(187, 134, 252, 0.15); color: #bb86fc; font-weight: 700; font-size: 0.8rem; padding: 0.25rem 0.6rem; border-radius: 6px; text-transform: uppercase;">POST</span>
                                <code style="font-size: 1.1rem; color: #fff; font-weight: 600; font-family: monospace;">/api/v1/weather_policy</code>
                            </div>
                            <span style="font-size: 0.8rem; color: var(--text-muted);">Query Parameters</span>
                        </div>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">Configure adaptive weather publishing thresholds, event cooldown, hourly budget, hysteresis parameters, or trigger baseline/defaults resets.</p>
                        <div style="margin-bottom: 1rem; font-size: 0.85rem;">
                            <strong style="color: var(--text-color);">Parameters:</strong>
                            <ul style="color: var(--text-muted); margin-top: 0.3rem; padding-left: 1.2rem;">
                                <li><code style="font-family: monospace; color: #00d2ff;">cooldown_sec=60..3600</code>: Minimum seconds between event-driven broadcasts</li>
                                <li><code style="font-family: monospace; color: #00d2ff;">budget_per_hour=1..60</code>: Maximum event broadcasts permitted per rolling hour</li>
                                <li><code style="font-family: monospace; color: #00d2ff;">temp_th, hum_th, press_th, wind_speed_th, wind_gust_th, wind_dir_th, precip_cml_th, precip_rate_th, illum_th, solar_rad_th, uv_th</code>: Metric delta thresholds</li>
                                <li><code style="font-family: monospace; color: #00d2ff;">temp_en, hum_en, press_en, wind_speed_en, ...=true|false</code>: Enable/disable individual metric triggers</li>
                                <li><code style="font-family: monospace; color: #00d2ff;">reset_defaults=1</code> / <code style="font-family: monospace; color: #00d2ff;">reset_baseline=1</code>: Operational actions</li>
                            </ul>
                        </div>
                        <div style="font-weight: 600; font-size: 0.8rem; color: var(--text-color); margin-bottom: 0.4rem;">Example Request:</div>
                        <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); border-radius: 8px; padding: 0.6rem 0.8rem; overflow-x: auto; margin-bottom: 0; gap: 1rem;">
                            <code style="font-size: 0.85rem; color: #e0e0e0; font-family: monospace; white-space: nowrap;">curl -X POST "http://<span class="api-ip">192.168.1.100</span>/api/v1/weather_policy?cooldown_sec=300&amp;budget_per_hour=6&amp;temp_th=1.0"</code>
                            <button onclick="testPost('/api/v1/weather_policy')" style="background: rgba(187, 134, 252, 0.15); border: 1px solid rgba(187, 134, 252, 0.3); color: #bb86fc; font-weight: 600; font-size: 0.75rem; padding: 0.3rem 0.75rem; border-radius: 6px; cursor: pointer; transition: all 0.2s; white-space: nowrap; margin-left: auto;">Execute</button>
                        </div>
                    </div>

                    <!-- POST /unpair -->
                    <div class="api-card" style="border: 1px solid var(--card-border); border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; background: rgba(255, 255, 255, 0.01);">
                        <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.8rem;">
                            <div style="display: flex; align-items: center; gap: 0.8rem;">
                                <span class="badge-post" style="background: rgba(187, 134, 252, 0.15); color: #bb86fc; font-weight: 700; font-size: 0.8rem; padding: 0.25rem 0.6rem; border-radius: 6px; text-transform: uppercase;">POST</span>
                                <code style="font-size: 1.1rem; color: #fff; font-weight: 600; font-family: monospace;">/unpair</code>
                            </div>
                            <span style="font-size: 0.8rem; color: var(--text-muted);">JSON Response</span>
                        </div>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">Unpair the currently locked weather station. The gateway immediately clears its pairing lock and shifts back to scanning mode (ALL) on the radio receiver profile.</p>
                        <div style="font-weight: 600; font-size: 0.8rem; color: var(--text-color); margin-bottom: 0.4rem;">Example Request:</div>
                        <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); border-radius: 8px; padding: 0.6rem 0.8rem; overflow-x: auto; margin-bottom: 0; gap: 1rem;">
                            <code style="font-size: 0.85rem; color: #e0e0e0; font-family: monospace; white-space: nowrap;">curl -X POST http://<span class="api-ip">192.168.1.100</span>/unpair</code>
                            <button onclick="testPost('/unpair')" style="background: rgba(187, 134, 252, 0.15); border: 1px solid rgba(187, 134, 252, 0.3); color: #bb86fc; font-weight: 600; font-size: 0.75rem; padding: 0.3rem 0.75rem; border-radius: 6px; cursor: pointer; transition: all 0.2s; white-space: nowrap; margin-left: auto;">Execute</button>
                        </div>
                    </div>

                    <!-- POST /ota -->
                    <div class="api-card" style="border: 1px solid var(--card-border); border-radius: 12px; padding: 1.5rem; margin-bottom: 1.5rem; background: rgba(255, 255, 255, 0.01);">
                        <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.8rem;">
                            <div style="display: flex; align-items: center; gap: 0.8rem;">
                                <span class="badge-post" style="background: rgba(187, 134, 252, 0.15); color: #bb86fc; font-weight: 700; font-size: 0.8rem; padding: 0.25rem 0.6rem; border-radius: 6px; text-transform: uppercase;">POST</span>
                                <code style="font-size: 1.1rem; color: #fff; font-weight: 600; font-family: monospace;">/ota</code>
                            </div>
                            <span style="font-size: 0.8rem; color: var(--text-muted);">Multipart Payload</span>
                        </div>
                        <p style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 1rem;">Performs live OTA firmware binary upgrade. This endpoint parses raw binary input stream chunks and updates the active partition.</p>
                        <div style="font-weight: 600; font-size: 0.8rem; color: var(--text-color); margin-bottom: 0.4rem;">Example Request:</div>
                        <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); border-radius: 8px; padding: 0.6rem 0.8rem; overflow-x: auto; margin-bottom: 0; gap: 1rem;">
                            <code style="font-size: 0.85rem; color: #e0e0e0; font-family: monospace; white-space: nowrap;">curl -X POST --data-binary @firmware.bin http://<span class="api-ip">192.168.1.100</span>/ota</code>
                            <button style="background: rgba(255, 255, 255, 0.05); border: 1px solid rgba(255, 255, 255, 0.1); color: var(--text-muted); font-weight: 600; font-size: 0.75rem; padding: 0.3rem 0.75rem; border-radius: 6px; cursor: not-allowed; white-space: nowrap; margin-left: auto;" disabled>Use Settings Tab</button>
                        </div>
                    </div>
                </div>
            </div>
        </div>

        <!-- History View -->
        <div id="page-history" class="page-content" style="display: none;">
            <div class="card" style="background: var(--card-bg); border: 1px solid var(--card-border); border-radius: 16px; padding: 1.5rem; margin-bottom: 1.5rem; backdrop-filter: var(--glass-blur);">
                <div style="display: flex; align-items: flex-start; justify-content: space-between; flex-wrap: wrap; gap: 0.75rem; margin-bottom: 1.1rem;">
                    <div>
                        <h2 style="margin-top: 0; margin-bottom: 0.35rem; background: linear-gradient(135deg, var(--primary), var(--secondary)); -webkit-background-clip: text; -webkit-text-fill-color: transparent; font-size: 1.4rem;">Historical Weather Trends</h2>
                        <p style="color: var(--text-muted); font-size: 0.83rem; margin: 0;">Aggregated 5-min weather history of the last 24 hours · Synchronized crosshair &amp; zoom across all charts.</p>
                    </div>
                    <div style="display: flex; align-items: center; gap: 0.5rem; flex-wrap: wrap;">
                        <span style="font-size: 0.72rem; color: var(--text-muted); background: rgba(255,255,255,0.04); border: 1px solid rgba(255,255,255,0.08); padding: 0.28rem 0.6rem; border-radius: 999px;">Drag to zoom · Double-click to reset</span>
                        <button type="button" onclick="resetHistoryZoom()" style="background: rgba(0, 229, 255, 0.1); border: 1px solid rgba(0, 229, 255, 0.28); color: var(--primary); font-size: 0.73rem; font-weight: 600; padding: 0.28rem 0.65rem; border-radius: 8px; cursor: pointer;">Reset Zoom</button>
                    </div>
                </div>
                <div id="history-container" style="width: 100%; display: flex; flex-direction: column; gap: 1.5rem;">
                    <div style="text-align: center; color: var(--text-muted); padding: 3rem;">
                        Loading 24-Hour aggregated history...
                    </div>
                </div>
            </div>

            <!-- SD Card Management (Full-Width Panel) -->
            <div id="sd-card-panel" class="section-panel" style="display: none; margin-top: 1.5rem; margin-bottom: 1.5rem; padding: 1.5rem;">
                <h2 style="font-size: 1.2rem; margin-bottom: 0.5rem; border-bottom: none; padding-bottom: 0;">SD Card Management</h2>
                <p style="color: var(--text-muted); font-size: 0.85rem; margin-bottom: 1.25rem;">Manage daily historical logs stored on the external microSD card.</p>
                
                <div id="sd-management-card" style="background: rgba(255,255,255,0.02); border: 1px solid var(--card-border); border-radius: 12px; overflow: hidden;">
                    <div style="display: flex; align-items: center; justify-content: space-between; flex-wrap: wrap; gap: 0.8rem; padding: 0.8rem 1rem;">
                        <div>
                            <span style="font-weight: 600; font-size: 0.9rem;">FAT filesystem:</span>
                            <span id="sd-status-val" style="font-weight: 700; color: var(--text-muted); margin-left: 0.4rem;">Checking...</span>
                            <span id="sd-refresh-note" style="display: block; color: var(--text-muted); font-size: 0.72rem; margin-top: 0.2rem;">Physical card and mount state are checked together.</span>
                        </div>
                        <div style="display: flex; gap: 0.6rem; flex-wrap: wrap;">
                            <button id="sd-refresh-btn" class="upload-btn" onclick="updateSdCardInfo()" style="background: rgba(255,255,255,0.05); color: #fff; border: 1px solid var(--card-border); font-size: 0.75rem; padding: 0.4rem 1rem; border-radius: 8px; cursor: pointer;">Refresh</button>
                            <button id="sd-eject-btn" class="upload-btn" onclick="ejectSdCard()" style="background: rgba(255, 177, 0, 0.16); color: #ffb100; border: 1px solid rgba(255, 177, 0, 0.45); font-size: 0.75rem; padding: 0.4rem 1rem; border-radius: 8px; cursor: pointer;">Eject SD Card</button>
                            <button id="sd-mount-btn" class="upload-btn" onclick="mountSdCard()" style="display: none; background: rgba(0, 245, 160, 0.16); color: #00f5a0; border: 1px solid rgba(0, 245, 160, 0.45); font-size: 0.75rem; padding: 0.4rem 1rem; border-radius: 8px; cursor: pointer;">Mount SD Card</button>
                            <button id="sd-test-btn" class="upload-btn" onclick="testSdCard()" style="background: linear-gradient(135deg, #00b4db, #0083b0); font-size: 0.75rem; padding: 0.4rem 1rem; border-radius: 8px;">Test SD Card (W/R)</button>
                            <button class="upload-btn" onclick="formatSdCard()" style="background: linear-gradient(135deg, #ff416c, #ff4b2b); font-size: 0.75rem; padding: 0.4rem 1rem; border-radius: 8px;">Format SD Card</button>
                        </div>
                    </div>

                    <div id="sd-logger-panel" style="border-top: 1px solid var(--card-border); padding: 0.7rem 1rem; font-size: 0.82rem;">
                        <div style="display: flex; justify-content: space-between; gap: 0.8rem; flex-wrap: wrap;">
                            <div><strong>Writer task:</strong> <span id="sd-logger-health" style="font-weight: 700; color: var(--text-muted);">Checking...</span></div>
                            <div id="sd-logger-counts" style="color: var(--text-muted);">Weather: -- | Metadata: -- | Retries: -- | Queue: --</div>
                        </div>
                        <div id="sd-logger-last-success" style="margin-top: 0.35rem; color: var(--text-muted);">Last successful write: --</div>
                        <div id="sd-logger-error" style="display: none; margin-top: 0.55rem; color: #ff416c; background: rgba(255,65,108,0.08); border: 1px solid rgba(255,65,108,0.2); padding: 0.5rem 0.65rem; border-radius: 8px;"></div>
                    </div>

                    <div id="sd-diag-panel" style="border-top: 1px solid var(--card-border); padding: 0.7rem 1rem; font-size: 0.82rem; display: none;">
                        <div style="font-weight: 600; margin-bottom: 0.5rem;">Card and partition diagnostics</div>
                        <div id="sd-diag-content" style="display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 0.55rem; color: var(--text-muted);">
                            <!-- Dynamic hardware properties -->
                        </div>
                        <div id="sd-diag-partitions" style="margin-top: 0.6rem; border-top: 1px solid rgba(255,255,255,0.05); padding-top: 0.5rem;">
                            <!-- Partition tables -->
                        </div>
                    </div>

                    <div style="border-top: 1px solid var(--card-border); padding: 0.7rem 1rem 1rem;">
                        <div style="font-weight: 600; font-size: 0.82rem; margin-bottom: 0.45rem;">Files on card</div>
                        <div style="max-height: 300px; overflow-y: auto; background: rgba(0,0,0,0.25); border-radius: 9px; border: 1px solid var(--card-border); padding: 0.4rem;">
                            <table style="width: 100%; border-collapse: collapse; font-size: 0.82rem;">
                                <thead>
                                    <tr style="border-bottom: 1px solid var(--card-border); text-align: left; color: var(--text-muted);">
                                        <th style="padding: 0.5rem;">File Name</th>
                                        <th style="padding: 0.5rem;">File Size</th>
                                        <th style="padding: 0.5rem; text-align: right;">Actions</th>
                                    </tr>
                                </thead>
                                <tbody id="sd-file-tbody">
                                    <tr>
                                        <td colspan="3" style="text-align: center; color: var(--text-muted); padding: 1.2rem;">Checking card contents...</td>
                                    </tr>
                                </tbody>
                            </table>
                        </div>
                    </div>
                </div>
            </div>

            <!-- SD Format Modal Overlay -->
            <div id="sd-format-modal" style="display: none; position: fixed; top: 0; left: 0; width: 100vw; height: 100vh; background: rgba(0, 0, 0, 0.75); backdrop-filter: blur(8px); -webkit-backdrop-filter: blur(8px); z-index: 10000; align-items: center; justify-content: center;">
                <div style="background: #181926; border: 1px solid var(--card-border); border-radius: 16px; padding: 2rem; max-width: 440px; width: 90%; text-align: center; box-shadow: 0 15px 40px rgba(0,0,0,0.6);">
                    <div id="sd-format-spinner" style="margin-bottom: 1.2rem;">
                        <div style="display: inline-block; width: 50px; height: 50px; border: 4px solid rgba(255,255,255,0.1); border-radius: 50%; border-top-color: #ff416c; animation: spin 1s linear infinite;"></div>
                    </div>
                    <h3 id="sd-format-modal-title" style="margin: 0 0 0.75rem 0; font-size: 1.3rem; color: #fff;">Formatting SD Card</h3>
                    <p id="sd-format-modal-msg" style="color: var(--text-muted); font-size: 0.9rem; line-height: 1.5; margin: 0 0 1rem 0;">
                        Formatting in progress...<br>
                        Please do not turn off or unplug the device.
                    </p>
                    <div id="sd-format-progress-container" style="display: none; width: 100%; background: rgba(255,255,255,0.08); height: 10px; border-radius: 6px; overflow: hidden; margin-bottom: 0.6rem; border: 1px solid rgba(255,255,255,0.05);">
                        <div id="sd-format-progress-bar" style="width: 0%; height: 100%; background: linear-gradient(90deg, #ff416c 0%, #ff4b2b 25%, #00f5a0 50%, #ff416c 75%, #ff4b2b 100%); background-size: 200% 100%; animation: progress-shimmer 2s linear infinite; transition: width 0.3s ease; border-radius: 6px;"></div>
                    </div>
                    <div id="sd-format-eta" style="display: none; font-size: 0.82rem; color: #00f5a0; font-weight: 600; margin-bottom: 0.8rem;"></div>
                    <div id="sd-format-modal-status" style="display: none; font-size: 0.85rem; font-weight: 600; padding: 0.6rem 1rem; border-radius: 8px; margin-top: 0.5rem;"></div>
                </div>
            </div>
        </div>

        <!-- AI & Home Assistant Integration Modal -->
        <div id="ai-prompt-modal" onclick="if(event.target === this) closeAiPromptModal();" style="display: none; position: fixed; top: 0; left: 0; width: 100vw; height: 100vh; background: rgba(0, 0, 0, 0.75); backdrop-filter: blur(8px); -webkit-backdrop-filter: blur(8px); z-index: 10000; align-items: center; justify-content: center; padding: 1rem;">
            <div style="background: #121727; border: 1px solid var(--card-border); border-radius: 16px; padding: 1.5rem; max-width: 600px; width: 100%; box-shadow: 0 15px 40px rgba(0,0,0,0.6); max-height: 90vh; overflow-y: auto;">
                <div style="display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid var(--card-border); padding-bottom: 0.8rem; margin-bottom: 1rem;">
                    <div style="display: flex; align-items: center; gap: 0.6rem;">
                        <span style="font-size: 1.4rem;">🤖</span>
                        <div>
                            <h3 style="font-size: 1rem; font-weight: 700; color: #fff; margin: 0;">AI Agent & Integration Setup</h3>
                            <p style="font-size: 0.75rem; color: var(--text-muted); margin: 0;">Works with any AI Agent (Claude Code, Antigravity, ChatGPT, Codex) & Home Assistant</p>
                        </div>
                    </div>
                    <button onclick="closeAiPromptModal()" style="background: transparent; border: none; color: var(--text-muted); font-size: 1.2rem; cursor: pointer; padding: 0.2rem 0.5rem; border-radius: 6px;" onmouseover="this.style.color='#fff'" onmouseout="this.style.color='var(--text-muted)'">✕</button>
                </div>

                <div style="background: rgba(0, 245, 160, 0.05); border: 1px solid rgba(0, 245, 160, 0.25); border-radius: 12px; padding: 1rem; margin-bottom: 1rem;">
                    <p style="font-size: 0.78rem; color: var(--text-muted); margin-bottom: 0.75rem; line-height: 1.4;">
                        Copy and paste this prompt into your AI agent to query live or historical weather data, diagnose RF & station health, configure the gateway, or generate integrations (scripts, dashboards, Home Assistant):
                    </p>
                    <div style="position: relative;">
                        <pre id="ai-modal-prompt-text" style="background: rgba(0,0,0,0.5); border: 1px solid var(--card-border); border-radius: 8px; padding: 0.75rem; font-size: 0.75rem; font-family: monospace; color: #00f5a0; white-space: pre-wrap; max-height: 220px; overflow-y: auto; margin: 0; line-height: 1.35;"></pre>
                    </div>
                    <div style="display: flex; gap: 0.6rem; align-items: center; margin-top: 0.8rem;">
                        <button id="ai-modal-copy-btn" onclick="copyAiPrompt()" style="background: #00f5a0; color: #0b0f19; font-weight: 700; font-size: 0.78rem; padding: 0.45rem 1rem; border-radius: 8px; border: none; cursor: pointer; transition: all 0.2s; box-shadow: 0 2px 8px rgba(0,245,160,0.3); display: flex; align-items: center; gap: 0.4rem;">
                            <span>📋</span> <span id="ai-modal-copy-text">Copy Prompt for AI</span>
                        </button>
                        <button onclick="testGet('/llms.txt')" style="background: rgba(255,255,255,0.06); border: 1px solid var(--card-border); color: var(--text-color); font-weight: 600; font-size: 0.78rem; padding: 0.45rem 0.85rem; border-radius: 8px; cursor: pointer; transition: all 0.2s;">
                            Open /llms.txt
                        </button>
                    </div>
                </div>

                <div style="display: flex; justify-content: flex-end;">
                    <button onclick="closeAiPromptModal()" style="background: rgba(255, 255, 255, 0.08); border: 1px solid var(--card-border); color: #fff; font-size: 0.8rem; padding: 0.4rem 1rem; border-radius: 8px; cursor: pointer;">Close</button>
                </div>
            </div>
        </div>
    </div>

    <script>
        function openAiPromptModal() {
            const host = window.location.host || '192.168.1.100';
            const prompt = `I have a WeatherXM weather gateway (WG1200) on my local network at http://${host}.
Full API reference & JSON schemas: http://${host}/llms.txt

Key HTTP endpoints (no authentication required):
- GET http://${host}/api/v1/info — gateway identity, Wi-Fi, paired station, RF region, and active units
- GET http://${host}/api/v1/observations — latest real-time weather observation (add ?buffer=full for 15-min raw buffer)
- GET http://${host}/api/v1/history — 24-hour historical series (5-min buckets)
- GET http://${host}/status — complete gateway & RF link diagnostics, pairing state, and weather snapshot
- GET http://${host}/packet_log — rolling LoRa RF packet log (RX/TX, RSSI/SNR, decoded fields)

Please fetch and read http://${host}/llms.txt to learn all available telemetry, history, diagnostic, mesh (Meshtastic/MeshCore), SD card, and control endpoints, then help me query weather data, diagnose station/RF health, configure the gateway, or build an integration (e.g. scripts, dashboards, or Home Assistant).`;

            const promptEl = document.getElementById('ai-modal-prompt-text');
            if (promptEl) promptEl.textContent = prompt;
            
            const modal = document.getElementById('ai-prompt-modal');
            if (modal) modal.style.display = 'flex';
        }

        function closeAiPromptModal() {
            const modal = document.getElementById('ai-prompt-modal');
            if (modal) modal.style.display = 'none';
        }

        function copyAiPrompt() {
            const promptEl = document.getElementById('ai-modal-prompt-text');
            if (!promptEl) return;
            const text = promptEl.textContent || '';

            const showCopiedFeedback = () => {
                const btnText = document.getElementById('ai-modal-copy-text');
                const btn = document.getElementById('ai-modal-copy-btn');
                if (btnText && btn) {
                    if (!btn.dataset.origLabel) {
                        btn.dataset.origLabel = btnText.textContent;
                    }
                    btnText.textContent = '✓ Copied!';
                    btn.style.background = '#00d2ff';
                    if (btn._copyTimer) clearTimeout(btn._copyTimer);
                    btn._copyTimer = setTimeout(() => {
                        btnText.textContent = btn.dataset.origLabel || 'Copy Prompt for AI';
                        btn.style.background = '#00f5a0';
                    }, 2500);
                }
            };

            const fallbackCopy = () => {
                try {
                    const ta = document.createElement('textarea');
                    ta.value = text;
                    ta.setAttribute('readonly', '');
                    ta.style.position = 'fixed';
                    ta.style.top = '-9999px';
                    ta.style.left = '-9999px';
                    ta.style.opacity = '0';
                    document.body.appendChild(ta);
                    ta.focus();
                    ta.select();
                    ta.setSelectionRange(0, text.length);
                    const ok = document.execCommand('copy');
                    document.body.removeChild(ta);
                    if (ok) {
                        showCopiedFeedback();
                        return;
                    }
                } catch (e) {}

                try {
                    const range = document.createRange();
                    range.selectNodeContents(promptEl);
                    const sel = window.getSelection();
                    sel.removeAllRanges();
                    sel.addRange(range);
                    if (document.execCommand('copy')) {
                        sel.removeAllRanges();
                        showCopiedFeedback();
                        return;
                    }
                } catch (e) {}
            };

            if (navigator.clipboard && typeof navigator.clipboard.writeText === 'function' && window.isSecureContext) {
                navigator.clipboard.writeText(text).then(showCopiedFeedback).catch(fallbackCopy);
            } else {
                fallbackCopy();
            }
        }

        function testGet(path) {
            const host = window.location.host || '192.168.1.100';
            const baseUrl = window.location.protocol === 'file:' ? 'http://192.168.1.100' : '';
            window.open(`${baseUrl}${path}`, '_blank');
        }

        function testPost(path) {
            const baseUrl = window.location.protocol === 'file:' ? 'http://192.168.1.100' : '';
            
            if (path === '/unpair' && !confirm("Are you sure you want to unpair the weather station? This will clear the pairing lock.")) {
                return;
            }

            let url = `${baseUrl}${path}`;
            
            if (path === '/set_station') {
                const el = document.getElementById('param-set_station');
                if (el) url += `?type=${el.value}`;
            } else if (path === '/set_channel') {
                const el = document.getElementById('param-set_channel');
                if (el) url += `?slot=${el.value}`;
            } else if (path === '/remote_reboot') {
                const el = document.getElementById('param-remote_reboot');
                if (el) url += `?enable=${el.value}`;
            } else if (path === '/ws1300_kick_station') {
                const el = document.getElementById('param-ws1300_kick');
                if (el) url += `?cancel=${el.value}`;
            } else if (path === '/set_tx_power') {
                const el = document.getElementById('param-set_tx_power');
                if (el) url += `?dbm=${el.value}`;
            } else if (path === '/set_tx_interval') {
                const el = document.getElementById('param-set_tx_interval');
                if (el) url += `?sec=${el.value}`;
            } else if (path === '/api/v1/meshtastic/config') {
                const en = document.getElementById('param-mesh_enabled');
                const iv = document.getElementById('param-mesh_interval');
                if (en && iv) url += `?enabled=${en.value}&interval=${iv.value}`;
            } else if (path === '/api/v1/meshcore/config') {
                const en = document.getElementById('param-meshcore_enabled');
                const iv = document.getElementById('param-meshcore_interval');
                const pr = document.getElementById('param-meshcore_profile');
                if (en && iv && pr) url += `?gw_enabled=${en.value}&gw_interval=${iv.value}&gw_profile=${pr.value}`;
            }

            fetch(url, { method: 'POST' })
                .then(res => {
                    if (!res.ok) {
                        return res.text().then(text => {
                            let errMsg = `HTTP ${res.status}: ${text || res.statusText}`;
                            if (res.status === 400 && text.includes("Not currently paired to a WS1300")) {
                                errMsg += "\n\n(Tip: The gateway must be paired with a WS1300 weather station to issue remote reboot, TX power, or TX interval command overrides.)";
                            }
                            throw new Error(errMsg);
                        });
                    }
                    return res.json();
                })
                .then(data => {
                    alert(`POST to ${path} completed successfully!\n\nResponse:\n${JSON.stringify(data, null, 2)}`);
                    updateStats();
                })
                .catch(err => {
                    alert(`POST to ${path} failed!\nError: ${err.message}`);
                });
        }

        function switchTab(tabId) {
            document.querySelectorAll('.page-content').forEach(el => {
                el.style.display = 'none';
            });
            document.querySelectorAll('.nav-item').forEach(el => {
                el.classList.remove('active');
            });
            
            if (tabId === 'dashboard') {
                document.getElementById('page-dashboard').style.display = 'block';
                document.getElementById('tab-dashboard').classList.add('active');
                updateLiveChart();
            } else if (tabId === 'history') {
                document.getElementById('page-history').style.display = 'block';
                document.getElementById('tab-history').classList.add('active');
                initHistoryCharts();
                if (!g_isScreenless) updateSdCardInfo();
            } else if (tabId === 'settings') {
                document.getElementById('page-settings').style.display = 'block';
                document.getElementById('tab-settings').classList.add('active');
            } else if (tabId === 'docs') {
                document.getElementById('page-docs').style.display = 'block';
                document.getElementById('tab-docs').classList.add('active');
            }
        }

        var g_isPaired = false;
        var g_hasLocalProfile = false;
        var g_activePairSource = 'none';
        var g_isScanningActive = false;
        var g_pairProfiles = {
            cloud: { present: false, ready: false, id: -1 },
            local: { present: false, ready: false, id: -1 }
        };
        var g_edition = 'wxm';
        var g_isUiLocked = false;
        var g_isScreenless = null;
        var g_sniffMode = false;
        var g_scanEtaSec = 0;
        var g_scanEtaReceivedAt = 0;
        var g_scanEtaIsDone = false;

        function formatEtaCountdown(sec) {
            if (sec <= 0) return '0s';
            const h = Math.floor(sec / 3600);
            const m = Math.floor((sec % 3600) / 60);
            const s = sec % 60;
            if (h > 0) return `${h}h ${m}m ${s}s`;
            return `${m}m ${s}s`;
        }

        function tickScanEta() {
            if (!window.g_isScanningActive || g_scanEtaIsDone || !g_scanEtaReceivedAt) return;
            const etaEl = document.getElementById('scan-eta-display');
            if (!etaEl) return;
            const elapsed = Math.floor((Date.now() - g_scanEtaReceivedAt) / 1000);
            const remaining = Math.max(0, g_scanEtaSec - elapsed);
            etaEl.textContent = `ETA ~${formatEtaCountdown(remaining)} remaining`;
        }

        function updateStats() {
            if (g_isUiLocked) return;
            const currentHost = window.location.host || '192.168.1.100';
            document.querySelectorAll('.api-ip').forEach(el => {
                el.textContent = currentHost;
            });

            fetch('/status')
                .then(res => res.json())
                .then(data => {
                    g_edition = data.edition || 'wxm';
                    const wifiEl = document.getElementById('wifi-val');
                    wifiEl.textContent = data.wifi_status;
                    wifiEl.className = 'value ' + (data.wifi_status === 'CONNECTED' ? 'status-on' : 'status-off');
                    document.getElementById('wifi-ssid-val').textContent = data.wifi_ssid ? `${data.wifi_ssid} (${data.wifi_rssi} dBm)` : 'Disconnected';

                    const mqttEl = document.getElementById('mqtt-val');
                    mqttEl.textContent = data.mqtt_status;
                    mqttEl.className = 'value ' + (data.mqtt_status === 'CONNECTED' ? 'status-on' : 'status-off');
                    document.getElementById('mqtt-stats').textContent = `Tx: ${data.mqtt_success_count || 0} · Err: ${data.mqtt_fail_count || 0}`;

                    const mqttToggleBtn = document.getElementById('mqtt-toggle-btn');
                    const mqttSwitchLabel = document.getElementById('mqtt-switch-label') || (mqttToggleBtn ? mqttToggleBtn.parentElement : null);
                    if (mqttToggleBtn) {
                        if (data.edition !== 'open') {
                            mqttToggleBtn.checked = true;
                            mqttToggleBtn.disabled = false;
                            mqttToggleBtn.onclick = function(e) {
                                e.preventDefault();
                                alert("Disabling WeatherXM Cloud upload is only possible in OPEN edition.\n\nIn WXM edition, cloud publishing is permanently enabled to ensure data continuity.");
                                mqttToggleBtn.checked = true;
                                return false;
                            };
                            if (mqttSwitchLabel) {
                                mqttSwitchLabel.title = "WeatherXM Cloud upload is locked ON in WXM edition (only possible in OPEN edition)";
                                mqttSwitchLabel.style.opacity = "0.75";
                                mqttSwitchLabel.style.cursor = "pointer";
                                mqttSwitchLabel.onclick = function(e) {
                                    if (e.target !== mqttToggleBtn) {
                                        alert("Disabling WeatherXM Cloud upload is only possible in OPEN edition.\n\nIn WXM edition, cloud publishing is permanently enabled to ensure data continuity.");
                                    }
                                };
                            }
                            const mqttDesc = document.getElementById('mqtt-toggle-desc');
                            if (mqttDesc) mqttDesc.textContent = "Enable/Disable the upload of weather data to WeatherXM cloud infrastructure";
                        } else {
                            mqttToggleBtn.disabled = false;
                            mqttToggleBtn.onclick = null;
                            if (mqttSwitchLabel) {
                                mqttSwitchLabel.title = "";
                                mqttSwitchLabel.style.opacity = "1.0";
                                mqttSwitchLabel.style.cursor = "pointer";
                                mqttSwitchLabel.onclick = null;
                            }
                            mqttToggleBtn.checked = !data.mqtt_disabled;
                            const mqttDesc = document.getElementById('mqtt-toggle-desc');
                            if (mqttDesc) mqttDesc.textContent = "Enable/Disable the upload of weather data to WeatherXM cloud infrastructure";
                        }
                    }
                    const wifiToggleBtn = document.getElementById('wifi-toggle-btn');
                    if (wifiToggleBtn) {
                        wifiToggleBtn.checked = (data.wifi_status === 'CONNECTED' || data.wifi_status === 'CONNECTING' || data.wifi_status === 'CONNECTED_NO_IP' || data.wifi_status === 'CONNECTED_NO_INTERNET');
                    }
                    const p2pToggleBtn = document.getElementById('wxm-p2p-toggle-btn');
                    if (p2pToggleBtn && document.activeElement !== p2pToggleBtn) {
                        p2pToggleBtn.checked = (data.pair_mode !== 'off');
                    }
                    if (data.tx_power_override !== undefined) {
                        updateStationPowerUI(data.tx_power_override);
                    }

                    const rxTotalEl = document.getElementById('rx-total');
                    if (rxTotalEl && data.rx_packet_total !== undefined) {
                        rxTotalEl.textContent = data.rx_packet_total;
                    }
                    const rxListenEl = document.getElementById('rx-listen-desc');
                    if (rxListenEl) {
                        const mhz = data.listen_freq_hz ? (data.listen_freq_hz / 1e6).toFixed(3) : '?';
                        rxListenEl.textContent = `${mhz} MHz (${data.listen_desc || '-'})`;
                    }

                    // Meshtastic and MeshCore state are owned by the firmware (NVS), not the
                    // browser. Refresh them from the device rather than localStorage.
                    refreshMeshtastic();
                    refreshMeshcore();
                    refreshWeatherPolicy();

                    const meshtasticRouterBtn = document.getElementById('meshtastic-router-btn');
                    if (meshtasticRouterBtn) {
                        meshtasticRouterBtn.checked = localStorage.getItem('meshtastic_router_enabled') === 'true';
                    }
                    const staMeshCard = document.getElementById('sta-meshtastic-card');
                    const staMeshCurrentVal = document.getElementById('sta-mesh-current-val');
                    const staMeshTargetInterval = document.getElementById('sta-mesh-target-interval');
                    const staMeshSendCheckbox = document.getElementById('sta-mesh-send-checkbox');
                    const staMeshBadge = document.getElementById('sta-mesh-badge');
                    const staMeshCommandStatus = document.getElementById('sta-mesh-command-status');

                    const staMeshcoreCard = document.getElementById('sta-meshcore-card');
                    const staMeshcoreCurrentVal = document.getElementById('sta-meshcore-current-val');
                    const staMeshcoreTargetInterval = document.getElementById('sta-meshcore-target-interval');
                    const staMeshcoreSendCheckbox = document.getElementById('sta-meshcore-send-checkbox');
                    const staMeshcoreBadge = document.getElementById('sta-meshcore-badge');
                    const staMeshcoreCommandStatus = document.getElementById('sta-meshcore-command-status');

                    const isPaired = (data.is_paired !== undefined)
                        ? !!data.is_paired
                        : (data.paired_id > 0 && data.paired_id !== 4294967295);
                    const isWs1300 = (data.station_type === 1300) || (isPaired && (data.active_source === 'cloud' ? data.cloud_ws_type === 1300 : data.local_ws_type === 1300));
                    const isWs1001 = (data.station_type === 1001) || (isPaired && !isWs1300);
                    const isReceiveOnly = (data.pair_mode === 'off' || data.ownership_status === 'receive_only' || data.operation_mode === 'listen' || data.operation_mode === 'off');
                    const hasPairedWs1300 = isPaired && isWs1300 && !isReceiveOnly;
                    g_isWs1300 = isWs1300;
                    g_isD1Mode = isPaired && isWs1001;
                    updateAdaptiveWeatherPanelState();
                    updateMeshtasticPanelState();
                    updateMeshcorePanelState();

                    if (staMeshCard) {
                        staMeshCard.style.opacity = hasPairedWs1300 ? '1.0' : '0.55';
                    }
                    if (staMeshBadge) {
                        staMeshBadge.textContent = hasPairedWs1300 ? 'Beta' : 'No WS1300 paired';
                        staMeshBadge.style.background = hasPairedWs1300
                            ? 'rgba(255, 193, 7, 0.1)' : 'rgba(255, 255, 255, 0.08)';
                        staMeshBadge.style.color = hasPairedWs1300 ? '#ffc107' : 'var(--text-muted)';
                    }

                    const staMeshInt = data.station_meshtastic_interval_min || 0;
                    const isStaMeshPending = !!data.station_meshtastic_command_pending;

                    if (staMeshCurrentVal) {
                        if (!hasPairedWs1300) {
                            staMeshCurrentVal.textContent = 'No Station';
                            staMeshCurrentVal.style.color = 'var(--text-muted)';
                        } else {
                            staMeshCurrentVal.textContent = staMeshInt > 0 ? `${staMeshInt} min` : 'Off';
                            staMeshCurrentVal.style.color = staMeshInt > 0 ? '#ffc107' : 'var(--text-muted)';
                        }
                    }

                    if (staMeshTargetInterval) {
                        staMeshTargetInterval.disabled = !hasPairedWs1300;
                        const staMeshMin = Number(staMeshTargetInterval.value);
                        updateMeshIntervalFloodWarning('sta-mesh-interval-flood-warning', staMeshMin * 60, staMeshMin === 0 || !hasPairedWs1300);
                    }
                    if (staMeshSendCheckbox) {
                        staMeshSendCheckbox.disabled = !hasPairedWs1300;
                        if (document.activeElement !== staMeshSendCheckbox) {
                            staMeshSendCheckbox.checked = isStaMeshPending;
                        }
                    }
                    if (staMeshCommandStatus) {
                        if (!hasPairedWs1300) {
                            staMeshCommandStatus.textContent = 'Unavailable';
                            staMeshCommandStatus.style.color = 'var(--text-muted)';
                        } else if (isStaMeshPending) {
                            staMeshCommandStatus.textContent = '⏳ Armed (awaiting communication)';
                            staMeshCommandStatus.style.color = '#ffca28';
                        } else {
                            staMeshCommandStatus.textContent = '';
                        }
                    }

                    if (staMeshcoreCard) {
                        staMeshcoreCard.style.opacity = hasPairedWs1300 ? '1.0' : '0.55';
                    }
                    if (staMeshcoreBadge) {
                        staMeshcoreBadge.textContent = hasPairedWs1300 ? 'Beta' : 'No WS1300 paired';
                        staMeshcoreBadge.style.background = hasPairedWs1300
                            ? 'rgba(255, 193, 7, 0.1)' : 'rgba(255, 255, 255, 0.08)';
                        staMeshcoreBadge.style.color = hasPairedWs1300 ? '#ffc107' : 'var(--text-muted)';
                    }

                    const staMeshcoreInt = data.station_meshcore_interval_min || 0;
                    const isStaMeshcorePending = !!data.station_meshcore_command_pending;

                    if (staMeshcoreCurrentVal) {
                        if (!hasPairedWs1300) {
                            staMeshcoreCurrentVal.textContent = 'No Station';
                            staMeshcoreCurrentVal.style.color = 'var(--text-muted)';
                        } else {
                            staMeshcoreCurrentVal.textContent = staMeshcoreInt > 0 ? `${staMeshcoreInt} min` : 'Off';
                            staMeshcoreCurrentVal.style.color = staMeshcoreInt > 0 ? '#ffc107' : 'var(--text-muted)';
                        }
                    }

                    if (staMeshcoreTargetInterval) {
                        staMeshcoreTargetInterval.disabled = !hasPairedWs1300;
                        const staMeshcoreMin = Number(staMeshcoreTargetInterval.value);
                        updateMeshIntervalFloodWarning('sta-meshcore-interval-flood-warning', staMeshcoreMin * 60, staMeshcoreMin === 0 || !hasPairedWs1300);
                    }
                    if (staMeshcoreSendCheckbox) {
                        staMeshcoreSendCheckbox.disabled = !hasPairedWs1300;
                        if (document.activeElement !== staMeshcoreSendCheckbox) {
                            staMeshcoreSendCheckbox.checked = isStaMeshcorePending;
                        }
                    }
                    if (staMeshcoreCommandStatus) {
                        if (!hasPairedWs1300) {
                            staMeshcoreCommandStatus.textContent = 'Unavailable';
                            staMeshcoreCommandStatus.style.color = 'var(--text-muted)';
                        } else if (isStaMeshcorePending) {
                            staMeshcoreCommandStatus.textContent = '⏳ Armed (awaiting communication)';
                            staMeshcoreCommandStatus.style.color = '#ffca28';
                        } else {
                            staMeshcoreCommandStatus.textContent = '';
                        }
                    }

                    g_isPaired = isPaired;
                    g_sniffMode = !!data.sniff_mode;
                    const localProfileReady = (data.local_profile_ready !== undefined)
                        ? !!data.local_profile_ready : !!data.local_ws_valid;
                    const localProfilePresent = (data.local_ws_present !== undefined)
                        ? !!data.local_ws_present : localProfileReady;
                    g_hasLocalProfile = localProfileReady;
                    g_activePairSource = data.active_source || 'none';
                    g_pairProfiles.cloud = {
                        present: !!data.cloud_ws_valid,
                        ready: !!data.cloud_profile_ready,
                        id: data.cloud_ws_id,
                        type: data.cloud_ws_type || 1300,
                        hasPk: !!data.cloud_has_pubkey,
                        snSynth: !!data.cloud_sn_synthesized
                    };
                    g_pairProfiles.local = {
                        present: localProfilePresent,
                        ready: localProfileReady,
                        id: data.local_ws_id,
                        type: data.local_ws_type || 1300,
                        hasPk: !!data.local_has_pubkey,
                        snSynth: !!data.local_sn_synthesized
                    };
                    const badgeLive = document.getElementById('badge-live');
                    if (badgeLive) {
                        const isWifiOk = (data.wifi_status === 'CONNECTED');
                        const isCloudOk = (data.mqtt_status === 'CONNECTED');
                        const isStationOk = !!data.active_link;

                        if (isWifiOk && isCloudOk && isStationOk) {
                            badgeLive.className = 'badge-live state-healthy';
                            badgeLive.textContent = '';
                            badgeLive.title = 'System Normal: Wi-Fi, Cloud & Station connected';
                            badgeLive.style.display = 'inline-flex';
                        } else if (!isWifiOk) {
                            badgeLive.className = 'badge-live state-error';
                            badgeLive.textContent = 'No Wi-Fi';
                            badgeLive.title = 'Wi-Fi Disconnected';
                            badgeLive.style.display = 'inline-flex';
                        } else if (!isCloudOk) {
                            badgeLive.className = 'badge-live state-error';
                            badgeLive.textContent = 'No Cloud Link';
                            badgeLive.title = 'WeatherXM Cloud Disconnected';
                            badgeLive.style.display = 'inline-flex';
                        } else if (!isStationOk) {
                            badgeLive.className = 'badge-live state-warning';
                            badgeLive.textContent = 'No Station Link';
                            badgeLive.title = 'No live weather station packets detected';
                            badgeLive.style.display = 'inline-flex';
                        } else {
                            badgeLive.style.display = 'none';
                        }
                    }
                    const linkEl = document.getElementById('link-val');
                    const linkSubEl = document.getElementById('link-sub');
                    if (linkEl) {
                        if (isPaired) {
                            const pairedHex = '0x' + (data.paired_id >>> 0).toString(16).toUpperCase().padStart(8, '0');
                            const source = (data.active_source || 'target').toUpperCase();
                            const modelStr = data.station_type ? `WS${data.station_type}` : 'WS';
                            linkEl.textContent = pairedHex;
                            linkEl.className = 'value ' + (data.active_link ? 'status-on' : 'status-off');
                            if (linkSubEl) {
                                if (data.operation_mode === 'off') {
                                    linkSubEl.textContent = `${modelStr} · Uplink Off`;
                                } else if (data.operation_mode === 'listen') {
                                    linkSubEl.textContent = `${modelStr} · Listen Only`;
                                } else if (data.operation_mode === 'scan') {
                                    linkSubEl.textContent = 'Scanning...';
                                } else if (data.active_link) {
                                    linkSubEl.textContent = `${modelStr} · Receiving (${source})`;
                                } else {
                                    linkSubEl.textContent = `${modelStr} · Waiting (${source})`;
                                }
                            }
                        } else {
                            linkEl.textContent = 'UNPAIRED';
                            linkEl.className = 'value status-off';
                            if (linkSubEl) {
                                linkSubEl.textContent = data.operation_mode === 'scan' ? 'Scanning...' : 'No station paired';
                            }
                        }
                    }

                    const ownershipEl = document.getElementById('ownership-val');
                    const ownershipSubEl = document.getElementById('ownership-sub');
                    const ownershipVerifyBtn = document.getElementById('ownership-verify-btn');
                    if (ownershipEl && ownershipSubEl) {
                        if (!isPaired) {
                            ownershipEl.textContent = 'NOT APPLICABLE';
                            ownershipSubEl.textContent = 'No station assignment is active.';
                            ownershipEl.style.color = 'var(--text-muted)';
                        } else if (isReceiveOnly) {
                            ownershipEl.textContent = 'RECEIVE ONLY';
                            ownershipSubEl.textContent = 'Downlinks are silenced. Saved assignment is preserved but ownership is not claimed.';
                            ownershipEl.style.color = '#00d2ff';
                        } else if (data.ownership_status === 'confirmed') {
                            ownershipEl.textContent = 'CONFIRMED';
                            const confTs = data.ownership_confirmed_ts ? new Date(data.ownership_confirmed_ts * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }) : '';
                            ownershipSubEl.textContent = confTs ? `Verified at ${confTs}` : 'Verified via GPS challenge';
                            ownershipEl.style.color = '#81c784';
                        } else if (data.ownership_status === 'pending') {
                            ownershipEl.textContent = 'VERIFYING';
                            ownershipSubEl.textContent = `Challenge sent (${data.ownership_packets_seen || 0}/4)`;
                            ownershipEl.style.color = '#ffca28';
                        } else if (data.ownership_status === 'failed') {
                            ownershipEl.textContent = 'NOT CONFIRMED';
                            const activePkKnown = (data.active_source === 'cloud') ? !!data.cloud_has_pubkey : !!data.local_has_pubkey;
                            ownershipSubEl.textContent = (data.station_type === 1300 && activePkKnown)
                                ? 'Downlink ignored (paired to another GW or RF collision) · Retry'
                                : 'No GPS response · Retry';
                            ownershipEl.style.color = '#ff416c';
                        } else {
                            ownershipEl.textContent = 'UNVERIFIED';
                            ownershipSubEl.textContent = data.active_link
                                ? 'Broadcasts only · Unverified'
                                : 'No response observed';
                            ownershipEl.style.color = '#ffb74d';
                        }
                    }
                    const isScanRunning = !!data.scan_active || (data.pair_mode === 'scan') || (data.autopair_enabled && !isPaired);
                    if (ownershipVerifyBtn) {
                        ownershipVerifyBtn.style.display = (isPaired && !isScanRunning && !isReceiveOnly && data.ownership_status !== 'confirmed') ? 'inline-block' : 'none';
                        ownershipVerifyBtn.disabled = data.ownership_status === 'pending';
                        ownershipVerifyBtn.style.opacity = ownershipVerifyBtn.disabled ? '0.55' : '1';
                    }

                    const operationEl = document.getElementById('operation-mode-val');
                    const operationSubEl = document.getElementById('operation-mode-sub');
                    if (operationEl && operationSubEl) {
                        const targetHex = data.scan_target_id
                            ? '0x' + (data.scan_target_id >>> 0).toString(16).toUpperCase().padStart(8, '0')
                            : null;
                        if (data.operation_mode === 'repair') {
                            operationEl.textContent = 'TARGETED REPAIR';
                            const targetChannel = data.scan_target_slot >= 0
                                ? (data.scan_target_slot === 0
                                    ? ' on base'
                                    : ` on WS1300 CH${data.scan_target_slot - 1}`)
                                : ' while sweeping for its channel';
                            operationSubEl.textContent = `Authenticated kick${targetHex ? ` for ${targetHex}` : ''}${targetChannel}; waiting for BOOT. Pair result is saved to LOCAL.`;
                            operationEl.style.color = '#ffb74d';
                        } else if (data.operation_mode === 'scan') {
                            const lockedChannel = data.scan_target_slot >= 0
                                ? (data.scan_target_slot === 0
                                    ? 'base'
                                    : `WS1300 CH${data.scan_target_slot - 1}`)
                                : null;
                            operationEl.textContent = lockedChannel
                                ? 'TARGETED PAIR' : 'PASSIVE SCAN';
                            operationSubEl.textContent = targetHex
                                ? `Listening only for ${targetHex} ADV/BOOT${lockedChannel ? ` on ${lockedChannel}` : ' while sweeping'}; no station is being kicked.`
                                : 'Listening for the first genuine ADV/BOOT; no station is being kicked.';
                            operationEl.style.color = '#bb86fc';
                        } else if (data.operation_mode === 'listen') {
                            operationEl.textContent = 'LISTEN ONLY';
                            operationSubEl.textContent = `Manual ${data.listen_desc || 'radio'} monitoring; packets are not paired or uplinked.`;
                            operationEl.style.color = '#00d2ff';
                        } else if (data.operation_mode === 'off') {
                            operationEl.textContent = 'OFF';
                            operationSubEl.textContent = 'Weather uplink is disabled; Cloud and Local profiles remain saved.';
                            operationEl.style.color = '#ff416c';
                        } else if (data.operation_mode === 'cloud') {
                            operationEl.textContent = data.active_source === 'cloud' ? 'CLOUD TARGET' : 'CLOUD SELECTED';
                            operationSubEl.textContent = data.active_source === 'cloud'
                                ? 'Listening and uplinking only for the saved cloud assignment.'
                                : `Cloud is selected, but the radio currently uses the ${(data.active_source || 'available').toUpperCase()} fallback because the cloud profile is incomplete.`;
                            operationEl.style.color = '#81c784';
                        } else {
                            operationEl.textContent = 'LOCAL TARGET';
                            operationSubEl.textContent = 'Listening and uplinking only for the saved local pairing.';
                            operationEl.style.color = '#00d2ff';
                        }
                    }

                    const recentContainer = document.getElementById('recent-stations-list');
                    const detectedWrapper = document.getElementById('detected-stations-container');
                    const matchContainer = document.getElementById('local-cloud-match-info');

                    const recent = Array.isArray(data.recent_stations) ? data.recent_stations : [];

                    if (recent.length === 0) {
                        if (detectedWrapper) detectedWrapper.style.display = 'none';
                        if (matchContainer) matchContainer.style.flex = '1 1 100%';
                        if (recentContainer) recentContainer.innerHTML = '';
                    } else {
                        if (detectedWrapper) detectedWrapper.style.display = 'flex';
                        if (matchContainer) matchContainer.style.flex = '1 1 calc(50% - 0.375rem)';
                        if (detectedWrapper) detectedWrapper.style.flex = '1 1 calc(50% - 0.375rem)';
                        if (recentContainer) {
                            recentContainer.innerHTML = recent.map(station => {
                                const id = station.id >>> 0;
                                const idHex = '0x' + id.toString(16).toUpperCase().padStart(8, '0');
                                const isCloud = !!data.cloud_ws_valid && (data.cloud_ws_id >>> 0) === id;
                                const isLocal = !!data.local_ws_present && (data.local_ws_id >>> 0) === id;
                                const isCurrentTarget = isPaired && (data.paired_id >>> 0) === id;
                                const isLinkActive = isCurrentTarget && (
                                    !!data.active_link ||
                                    (data.activity_status && data.activity_status !== 'NONE' && data.activity_status !== 'INACTIVE')
                                );
                                const knownAs = [isCloud ? 'CLOUD' : '', isLocal ? 'LOCAL' : ''].filter(Boolean).join(' + ');
                                const channel = station.channel_slot === 0 ? 'Base' : `CH${station.channel_slot - 1}`;
                                const snrStr = (station.type === 1300 && station.snr !== undefined) ? ` · SNR ${station.snr} dB` : '';

                                let statusBadge = '';
                                if (isCurrentTarget) {
                                    if (isLinkActive) {
                                        statusBadge = `<span style="font-size:0.68rem;font-weight:700;color:#81c784;background:rgba(129,199,132,0.18);padding:0.15rem 0.4rem;border-radius:4px;letter-spacing:0.5px;">PAIRED (ONLINE)</span>`;
                                    } else {
                                        statusBadge = `<span style="font-size:0.68rem;font-weight:700;color:#ffb74d;background:rgba(255,183,77,0.18);padding:0.15rem 0.4rem;border-radius:4px;letter-spacing:0.5px;">PAIRED (WAITING)</span>`;
                                    }
                                } else {
                                    statusBadge = `<span style="font-size:0.68rem;font-weight:700;color:#bb86fc;background:rgba(187,134,252,0.15);padding:0.15rem 0.4rem;border-radius:4px;letter-spacing:0.5px;">DETECTED</span>`;
                                }

                                const hasPkRam = !!station.has_pubkey;
                                const hasSnRam = !!station.has_sn;
                                const pkBadge = (station.type === 1300)
                                    ? (hasPkRam
                                        ? `<span style="font-size:0.65rem;font-weight:700;color:#81c784;background:rgba(129,199,132,0.14);padding:0.1rem 0.35rem;border-radius:4px;" title="Station Public Key &amp; Serial Number already captured in RAM from a recent BOOT packet">PK ✓</span>`
                                        : `<span style="font-size:0.65rem;font-weight:600;color:#ffb74d;background:rgba(255,183,77,0.12);padding:0.1rem 0.35rem;border-radius:4px;" title="Only weather packets seen so far; reboot station after targeting to capture Public Key">No PK</span>`)
                                    : '';
                                const pairLocalBtn = `<button onclick="pairToLocal(${id}, ${station.channel_slot}, ${station.type}, ${hasPkRam}, ${hasSnRam})" class="btn-sm-ctrl" style="color:#00e5ff;border-color:rgba(0,229,255,0.6);font-size:0.68rem;padding:0.25rem 0.55rem;font-weight:700;background:rgba(0,229,255,0.12);" title="Save this station (and any RAM-cached SN/PubKey) to Local NVS and target immediately">PAIR TO LOCAL</button>`;
                                let action = '';
                                if (isCurrentTarget) {
                                    action = `<div style="display:flex;gap:0.3rem;flex-wrap:wrap;justify-content:flex-end;align-items:center;">
                                        <span style="font-size:0.75rem;color:${isLinkActive ? '#81c784' : '#ffb74d'};font-weight:600;">${isLinkActive ? 'Current Target ✓' : 'Connecting...'}</span>
                                        <button onclick="setPairState('auto')" class="btn-sm-ctrl" style="color:#ff5252;border-color:rgba(255,82,82,0.45);font-size:0.68rem;padding:0.25rem 0.55rem;font-weight:700;" title="Disconnect and resume scanning">UNPAIR / RESCAN</button>
                                    </div>`;
                                } else if (station.pairable) {
                                    action = `<div style="display:flex;gap:0.3rem;flex-wrap:wrap;justify-content:flex-end;">${pairLocalBtn}<button onclick="startTargetedPair(${id}, ${station.channel_slot}, ${station.type}, true)" class="btn-sm-ctrl" style="color:#bb86fc;border-color:rgba(187,134,252,0.45);font-size:0.68rem;padding:0.25rem 0.55rem;font-weight:700;">PAIR</button></div>`;
                                } else if (station.type === 1300) {
                                    const pairOnReboot = `<button onclick="startTargetedPair(${id}, ${station.channel_slot}, ${station.type}, false)" class="btn-sm-ctrl" style="color:#bb86fc;border-color:rgba(187,134,252,0.45);font-size:0.68rem;padding:0.25rem 0.55rem;font-weight:700;" title="The gateway locks that station's exact ID and channel; reboot the station only after the lock is active so its one-shot BOOT cannot be missed by the sweep.">LOCK CHANNEL</button>`;
                                    const kickRepair = (isCloud || isLocal)
                                        ? `<button onclick="startTargetedRepair(${id}, '${knownAs}')" class="btn-sm-ctrl" style="color:#ffb74d;border-color:rgba(255,183,77,0.45);font-size:0.68rem;padding:0.25rem 0.55rem;font-weight:700;">KICK + REPAIR</button>`
                                        : '';
                                    action = `<div style="display:flex;gap:0.3rem;flex-wrap:wrap;justify-content:flex-end;">${pairLocalBtn}${pairOnReboot}${kickRepair}</div>`;
                                } else {
                                    action = `<div style="display:flex;gap:0.3rem;flex-wrap:wrap;justify-content:flex-end;">${pairLocalBtn}<button onclick="listenToRecent(${station.channel_slot}, ${id})" class="btn-sm-ctrl" style="color:#00d2ff;border-color:rgba(0,210,255,0.45);font-size:0.68rem;padding:0.25rem 0.55rem;font-weight:700;">LISTEN</button></div>`;
                                }
                                return `<div style="padding:0.6rem 0.85rem;border:1px solid ${isCurrentTarget ? (isLinkActive ? 'rgba(129,199,132,0.5)' : 'rgba(255,183,77,0.5)') : 'rgba(187,134,252,0.3)'};border-radius:8px;background:${isCurrentTarget ? (isLinkActive ? 'rgba(129,199,132,0.06)' : 'rgba(255,183,77,0.06)') : 'rgba(187,134,252,0.04)'};display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:0.6rem;min-height:44px;box-sizing:border-box;">
                                    <div style="display:flex;align-items:center;gap:0.55rem;flex-wrap:wrap;">
                                        ${statusBadge}
                                        <span style="font-family:monospace;font-size:1.05rem;font-weight:700;color:#fff;letter-spacing:0.5px;">${idHex}</span>
                                        <span style="font-size:0.75rem;color:var(--text-muted);font-weight:600;">WS${station.type}</span>
                                        ${pkBadge}
                                        <span style="font-size:0.72rem;color:var(--text-muted);">${channel} · RSSI ${station.rssi} dBm${snrStr} · ${station.age_sec}s ago${knownAs ? ` · ${knownAs}` : ''}</span>
                                    </div>
                                    <div>${action}</div>
                                </div>`;
                            }).join('');
                        }
                    }
                    const loraSubEl = document.getElementById('lora-link-sub');
                    if (loraSubEl) {
                        let actStr = data.activity_status || 'Active';
                        if (actStr === 'ACTIVE_OK') actStr = 'Active';
                        else if (actStr === 'ACTIVE_BAD_DATA') actStr = 'Bad Data';
                        else if (actStr === 'ACTIVE_LOW_RECEPTION') actStr = 'Low Reception';
                        else if (actStr === 'ACTIVE_BUSY') actStr = 'Busy';
                        loraSubEl.textContent = isPaired
                            ? `Rx: ${data.packet_reception}% · ${actStr}`
                            : 'Rx: N/A';
                    }

                    const isScreenless = !!data.is_screenless;
                    g_isScreenless = isScreenless;
                    const sdCardPanel = document.getElementById('sd-card-panel');
                    if (sdCardPanel) sdCardPanel.style.display = isScreenless ? 'none' : 'block';
                    const sdBanner = document.getElementById('sd-unusable-banner');
                    if (sdBanner) {
                        if (!isScreenless && data.sd_unusable) {
                            const sdBannerText = document.getElementById('sd-unusable-banner-text');
                            if (sdBannerText) {
                                const capStr = data.sd_capacity_gb > 0 ? ` (${Number(data.sd_capacity_gb).toFixed(1)} GB)` : '';
                                const fsStr = data.sd_fs_type ? ` [${data.sd_fs_type}]` : '';
                                const reasonStr = data.sd_status_reason || 'Inserted microSD card does not contain a mountable FAT16/FAT32 filesystem.';
                                sdBannerText.textContent = `microSD card${capStr}${fsStr} cannot be used: ${reasonStr} Open Connections -> SD Card and click "Format SD Card" to initialize it as FAT32. Gateway operation continues normally.`;
                            }
                            sdBanner.style.display = 'flex';
                        } else {
                            sdBanner.style.display = 'none';
                        }
                    }
                    const sdEjectedBanner = document.getElementById('sd-ejected-banner');
                    if (sdEjectedBanner) {
                        if (!isScreenless && data.sd_ejected) {
                            const titleEl = document.getElementById('sd-ejected-banner-title');
                            const descEl = document.getElementById('sd-ejected-banner-desc');
                            if (titleEl) {
                                titleEl.textContent = data.sd_present
                                    ? 'microSD Card Ready to Eject (Safe to Remove)'
                                    : 'microSD Card Ejected (Card Removed)';
                            }
                            if (descEl) {
                                descEl.textContent = data.sd_present
                                    ? 'The microSD card is safely unmounted and SD logging is paused. Remove the card now, or click "Mount SD Card" to resume logging.'
                                    : 'The microSD card is in the ejected state and physically removed. Re-insert the card to automatically mount it, or click "Mount SD Card".';
                            }
                            sdEjectedBanner.style.display = 'flex';
                        } else {
                            sdEjectedBanner.style.display = 'none';
                        }
                    }
                    window.g_sdCapacityGb = data.sd_capacity_gb || 0;

                    if (data.units) {
                        const u = data.units;
                        const prevSig = window.g_unitsSignature || '';
                        const newSig = `${u.temp_id}:${u.wind_id}:${u.precip_id}:${u.press_id}`;
                        window.g_units = {
                            temp_id: Number(u.temp_id) || 0,
                            wind_id: Number(u.wind_id) || 0,
                            precip_id: Number(u.precip_id) || 0,
                            press_id: Number(u.press_id) || 0,
                            temp: u.temp || 'C',
                            wind: u.wind || 'm/s',
                            precip: u.precip || 'mm',
                            press: u.press || 'hPa'
                        };
                        const tempSel = document.getElementById('unit-temp-select');
                        if (tempSel && document.activeElement !== tempSel) tempSel.value = String(window.g_units.temp_id);
                        const windSel = document.getElementById('unit-wind-select');
                        if (windSel && document.activeElement !== windSel) windSel.value = String(window.g_units.wind_id);
                        const precipSel = document.getElementById('unit-precip-select');
                        if (precipSel && document.activeElement !== precipSel) precipSel.value = String(window.g_units.precip_id);
                        const pressSel = document.getElementById('unit-press-select');
                        if (pressSel && document.activeElement !== pressSel) pressSel.value = String(window.g_units.press_id);

                        if (prevSig && prevSig !== newSig) {
                            if (typeof renderLiveChartData === 'function' && window.g_lastLiveTrendsData) {
                                renderLiveChartData(window.g_lastLiveTrendsData, true);
                            }
                            if (typeof renderHistoryChartsData === 'function' && window.g_lastHistoryData &&
                                document.getElementById('page-history') && document.getElementById('page-history').style.display === 'block') {
                                renderHistoryChartsData(window.g_lastHistoryData);
                            }
                        }
                        window.g_unitsSignature = newSig;
                    }

                    const pressureValEl = document.getElementById('pressure-val');
                    if (pressureValEl) {
                        if (data.pressure_hpa > 0) {
                            const convP = typeof convertPressFromHpa === 'function' ? convertPressFromHpa(data.pressure_hpa) : data.pressure_hpa;
                            const uPress = (window.g_units && window.g_units.press) ? window.g_units.press : 'hPa';
                            const pDec = uPress === 'inHg' ? 2 : 1;
                            pressureValEl.textContent = `${convP.toFixed(pDec)} ${uPress}`;
                        } else {
                            pressureValEl.textContent = '-';
                        }
                    }
                    const baroSubEl = document.getElementById('baro-sensor-info');
                    if (baroSubEl) {
                        baroSubEl.textContent = isScreenless ? 'BMP390 (I2C on nRF52)' : 'BMP390 (I2C on ESP32)';
                    }

                    const unpairContainer = document.getElementById('unpair-action-container');
                    if (isPaired) {
                        const modelChanEl = document.getElementById('paired-model-chan');
                        if (modelChanEl) {
                            modelChanEl.textContent = `${data.listen_desc} · ${(data.listen_freq_hz / 1000000).toFixed(3)} MHz`;
                            modelChanEl.style.display = 'block';
                        }
                        if (unpairContainer) unpairContainer.style.display = 'flex';
                    } else {
                        const modelChanEl = document.getElementById('paired-model-chan');
                        if (modelChanEl) {
                            modelChanEl.style.display = 'none';
                            modelChanEl.textContent = '';
                        }
                        if (unpairContainer) unpairContainer.style.display = 'none';
                    }

                    // WS1300 remote command controls (reboot / TX power / TX interval)
                    // only make sense while paired to a WS1300
                    const ws1300CmdContainer = document.getElementById('ws1300-cmd-container');
                    if (ws1300CmdContainer) {
                        ws1300CmdContainer.style.display = (isPaired && data.station_type === 1300 && !isReceiveOnly) ? 'flex' : 'none';
                    }
                    const txPowerSel = document.getElementById('tx-power-select');
                    if (txPowerSel && txPowerSel.options.length <= 1) {
                        for (let p = 22; p >= 1; p--) {
                            const el = document.createElement('option');
                            el.value = p;
                            el.textContent = `${p} dBm`;
                            txPowerSel.appendChild(el);
                        }
                    }
                    if (txPowerSel && document.activeElement !== txPowerSel) {
                        txPowerSel.value = String(data.tx_power_override || 0);
                    }
                    const txIntervalSel = document.getElementById('tx-interval-select');
                    if (txIntervalSel && txIntervalSel.options.length <= 1) {
                        for (let s = 20; s <= 240; s += 20) {
                            const el = document.createElement('option');
                            el.value = s;
                            el.textContent = `${s}s`;
                            txIntervalSel.appendChild(el);
                        }
                    }
                    if (txIntervalSel && document.activeElement !== txIntervalSel) {
                        txIntervalSel.value = String(data.tx_interval_override || 0);
                    }
                    const rebootToggle = document.getElementById('remote-reboot-toggle');
                    const rebootStatusEl = document.getElementById('remote-reboot-status');
                    if (rebootToggle && document.activeElement !== rebootToggle) {
                        rebootToggle.checked = !!data.ws1300_reboot_pending;
                    }
                    if (rebootStatusEl) {
                        rebootStatusEl.textContent = data.ws1300_reboot_pending
                            ? 'armed - awaiting station check-in' : '';
                    }

                    const reqGpsBtn = document.getElementById('btn-req-gps');
                    if (reqGpsBtn) {
                        if (isPaired) {
                            reqGpsBtn.disabled = false;
                            reqGpsBtn.style.cursor = 'pointer';
                            if (data.ws1300_gps_req_pending) {
                                reqGpsBtn.textContent = '⏳ GPS Request Armed...';
                                reqGpsBtn.style.opacity = '1';
                                reqGpsBtn.style.background = 'rgba(255, 193, 7, 0.2)';
                                reqGpsBtn.style.borderColor = 'rgba(255, 193, 7, 0.5)';
                                reqGpsBtn.style.color = '#ffca28';
                                reqGpsBtn.title = 'GPS request armed - awaiting station check-in (~20s)';
                            } else {
                                reqGpsBtn.textContent = '📡 Request GPS Fix';
                                reqGpsBtn.style.opacity = '1';
                                reqGpsBtn.style.background = 'rgba(0, 210, 255, 0.15)';
                                reqGpsBtn.style.borderColor = 'rgba(0, 210, 255, 0.35)';
                                reqGpsBtn.style.color = '#00d2ff';
                                reqGpsBtn.title = 'Force station to turn on GPS and transmit coordinates on next check-in';
                            }
                        } else {
                            reqGpsBtn.disabled = true;
                            reqGpsBtn.textContent = '📡 Request GPS Fix';
                            reqGpsBtn.style.opacity = '0.4';
                            reqGpsBtn.style.cursor = 'not-allowed';
                            reqGpsBtn.style.background = 'rgba(255, 255, 255, 0.05)';
                            reqGpsBtn.style.borderColor = 'rgba(255, 255, 255, 0.1)';
                            reqGpsBtn.style.color = 'var(--text-muted)';
                            reqGpsBtn.title = 'Station must be paired (in Cloud or Local mode) to request GPS fix';
                        }
                    }

                    if (data.weather) {
                        const w = data.weather;
                        const tUnit = w.unit_temp ? (w.unit_temp.startsWith('°') ? w.unit_temp : `°${w.unit_temp}`) : '°C';
                        const wDec = w.unit_wind === 'bft' ? 0 : 1;
                        const pDec = w.unit_press === 'inHg' ? 2 : 1;
                        if (w.has_weather) {
                            const formatValue = (value, decimals, suffix) =>
                                Number.isFinite(value) ? `${value.toFixed(decimals)} ${suffix}` : '-';
                            document.getElementById('w-temp').textContent = formatValue(w.temp, 1, tUnit);
                            document.getElementById('w-feels-like').textContent = formatValue(w.feels_like, 1, tUnit);
                            document.getElementById('w-humidity').textContent = formatValue(w.humidity, 0, '%');
                            document.getElementById('w-wind-speed').textContent = formatValue(w.wind_speed, wDec, w.unit_wind);
                            document.getElementById('w-wind-dir').textContent = Number.isFinite(w.wind_dir_deg)
                                ? (w.wind_dir_cardinal ? `${w.wind_dir_deg.toFixed(0)}° (${w.wind_dir_cardinal})` : `${w.wind_dir_deg.toFixed(0)}°`)
                                : '-';
                            document.getElementById('w-wind-gust').textContent = formatValue(w.wind_gust, wDec, w.unit_wind);
                            document.getElementById('w-precip-rate').textContent = formatValue(w.precip_rate, 2, `${w.unit_precip}/h`);
                            document.getElementById('w-precip-daily').textContent = Number.isFinite(w.precip_daily) ? `Daily: ${w.precip_daily.toFixed(2)} ${w.unit_precip}` : 'Daily: -';
                            document.getElementById('w-pressure').textContent = formatValue(w.pressure, pDec, w.unit_press);
                            document.getElementById('w-uv').textContent = `UV Index: ${w.uv}`;
                            document.getElementById('w-irradiance').textContent = formatValue(w.irradiance, 1, 'W/m²');
                        } else {
                            document.getElementById('w-temp').textContent = '-';
                            document.getElementById('w-feels-like').textContent = '-';
                            document.getElementById('w-humidity').textContent = '-';
                            document.getElementById('w-wind-speed').textContent = '-';
                            document.getElementById('w-wind-dir').textContent = '-';
                            document.getElementById('w-wind-gust').textContent = '-';
                            document.getElementById('w-precip-rate').textContent = '-';
                            document.getElementById('w-precip-daily').textContent = 'Daily: -';
                            if (w.pressure > 0) {
                                document.getElementById('w-pressure').textContent = `${w.pressure.toFixed(pDec)} ${w.unit_press}`;
                            } else {
                                document.getElementById('w-pressure').textContent = '-';
                            }
                            document.getElementById('w-uv').textContent = 'UV Index: -';
                            document.getElementById('w-irradiance').textContent = '-';
                        }
                    }

                    if (data.fw_version) {
                        document.getElementById('fw-version').textContent = 'v' + data.fw_version;
                    }

                    // Dynamically adapt dashboard headers and card titles depending on screenless vs screen model
                    const headerH1 = document.querySelector('header h1');
                    if (headerH1) {
                        headerH1.textContent = data.dev_name || 'Unknown Device Name';
                    }
                    const headerP = document.querySelector('header p');
                    if (headerP) {
                        const editionLabel = (data.edition === 'open') ? 'OPEN edition' : 'WXM edition';
                        const gwName = isScreenless ? 'Solar Gateway' : 'Gateway';
                        let baseHeader = `WeatherXM - ${editionLabel} - ${gwName}`;
                        if (isPaired) {
                            if (isWs1001) {
                                baseHeader = `WeatherXM D1 - ${editionLabel} - ${gwName}`;
                            } else if (isWs1300) {
                                baseHeader = `WeatherXM D2 - ${editionLabel} - ${gwName}`;
                            }
                        }
                        if (headerP.firstChild) {
                            headerP.firstChild.textContent = `${baseHeader} `;
                        }
                        document.title = `${baseHeader} - Diagnostics`;
                    }

                    const loraValEl = document.getElementById('lora-link-val');
                    if (loraValEl) {
                        const freqStr = (data.listen_freq_hz && data.listen_freq_hz > 0)
                            ? `${(data.listen_freq_hz / 1000000).toFixed(3)} MHz`
                            : 'Auto';
                        loraValEl.textContent = freqStr;
                        loraValEl.className = 'value status-on';
                    }

                    // Update sniff toggle state
                    const sniffToggle = document.getElementById('sniff-toggle');
                    if (sniffToggle) {
                        sniffToggle.checked = !!data.sniff_mode;
                    }

                    // Update header sniffing badge status
                    const badgeSniffing = document.getElementById('badge-sniffing');
                    if (badgeSniffing) {
                        badgeSniffing.style.display = !!data.sniff_mode ? 'inline-block' : 'none';
                    }

                    // Show current listening channel & frequency

                    // Populate the channel selector once (needs region base freq)
                    const chSel = document.getElementById('channel-select');
                    if (chSel && data.region_base_hz && chSel.options.length === 0) {
                        const base = data.region_base_hz;
                        const opts = [
                            { v: -1, t: 'Auto (sweep)' },
                            { v: 0, t: `WS1001 · ${(base / 1000000).toFixed(3)} MHz` }
                        ];
                        for (let c = 0; c < 8; c++) {
                            const f = base + 100000 + c * 200000;
                            opts.push({ v: c + 1, t: `WS1300 CH${c} · ${(f / 1000000).toFixed(3)} MHz` });
                        }
                        for (const o of opts) {
                            const el = document.createElement('option');
                            el.value = o.v;
                            el.textContent = o.t;
                            chSel.appendChild(el);
                        }
                    }
                    // Keep selection in sync unless the user is interacting with it.
                    // If channel selection is disabled, update it to the currently active listening channel.
                    if (chSel && chSel.options.length > 0 && document.activeElement !== chSel) {
                        const isChannelSelectDisabled = isPaired || (!isPaired && !!data.autopair_enabled && !data.cloud_pairing_enabled);
                        let activeSlot = data.manual_slot;
                        if (isChannelSelectDisabled && data.listen_desc) {
                            const desc = data.listen_desc;
                            if (desc.includes('WS1001') || desc.includes('base')) {
                                activeSlot = 0;
                            } else if (desc.includes('WS1300 CH')) {
                                let ch = 0;
                                const match = desc.match(/WS1300 CH(\d)/);
                                if (match) {
                                    ch = parseInt(match[1], 10);
                                }
                                activeSlot = ch + 1;
                            }
                        }
                        chSel.value = String(activeSlot);
                    }
                    
                    // Update Region select state across all region dropdowns
                    const regIds = ['region-select', 'meshtastic-gw-region', 'meshcore-gw-region', 'sta-mesh-region', 'sta-meshcore-region'];
                    if (data.freq_region) {
                        regIds.forEach(id => {
                            const el = document.getElementById(id);
                            if (el && document.activeElement !== el) {
                                el.value = String(data.freq_region);
                            }
                        });
                    }

                    // Synchronize Station Scan Profile dropdown
                    const scanTypeFromStatus = (data.scan_profile === 1) ? 1300 : ((data.scan_profile === 0) ? 1001 : 0);
                    if (window.g_currentScanType === undefined) {
                        window.g_currentScanType = scanTypeFromStatus;
                    }
                    const activeScanType = (window.g_currentScanType !== undefined)
                        ? window.g_currentScanType
                        : scanTypeFromStatus;
                    window.g_currentStationType = activeScanType;

                    const scanProfileSel = document.getElementById('scan-profile-select');
                    if (scanProfileSel && document.activeElement !== scanProfileSel) {
                        scanProfileSel.value = String(data.scan_active ? scanTypeFromStatus : activeScanType);
                    }
                    if (scanProfileSel) {
                        scanProfileSel.disabled = g_isUiLocked || !!data.scan_active;
                    }

                    // Update Quad-State Pair Mode controls. The firmware reports
                    // its persisted mode directly; the flag-based derivation is
                    // only a fallback for older firmware without the field.
                    let pairState;
                    if (data.pair_mode) {
                        pairState = (data.pair_mode === 'scan') ? 'auto' : data.pair_mode;
                    } else {
                        pairState = 'cloud';
                        if (!data.cloud_pairing_enabled) {
                            if (isPaired) {
                                pairState = 'local';
                            } else if (data.autopair_enabled) {
                                pairState = 'auto';
                            } else {
                                pairState = 'off';
                            }
                        }
                    }
                    const isAutoPairOn = (pairState === 'auto') && !isPaired;

                    const badge = document.getElementById('pair-state-badge');
                    const btnOff = document.getElementById('pair-btn-off');
                    const btnLocal = document.getElementById('pair-btn-local');
                    const btnCloud = document.getElementById('pair-btn-cloud');

                    if (badge) {
                        if (pairState === 'cloud') {
                            badge.textContent = 'CLOUD TARGET' +
                                (data.cloud_profile_ready ? '' : ' · INCOMPLETE');
                            badge.style.background = data.cloud_profile_ready
                                ? 'rgba(76, 175, 80, 0.1)' : 'rgba(255, 183, 77, 0.1)';
                            badge.style.color = data.cloud_profile_ready ? '#4caf50' : '#ffb74d';
                            badge.style.border = data.cloud_profile_ready
                                ? '1px solid rgba(76, 175, 80, 0.2)'
                                : '1px solid rgba(255, 183, 77, 0.25)';
                        } else if (pairState === 'off') {
                            badge.textContent = 'UPLINK OFF';
                            badge.style.background = 'rgba(255, 65, 108, 0.1)';
                            badge.style.color = '#ff416c';
                            badge.style.border = '1px solid rgba(255, 65, 108, 0.2)';
                        } else if (pairState === 'local') {
                            badge.textContent = 'LOCAL TARGET';
                            badge.style.background = 'rgba(0, 210, 255, 0.1)';
                            badge.style.color = '#00d2ff';
                            badge.style.border = '1px solid rgba(0, 210, 255, 0.2)';
                        } else if (pairState === 'auto') {
                            badge.textContent = 'SCANNING';
                            badge.style.background = 'rgba(187, 134, 252, 0.1)';
                            badge.style.color = '#bb86fc';
                            badge.style.border = '1px solid rgba(187, 134, 252, 0.2)';
                        }
                    }

                    const fallbackBadge = document.getElementById('fallback-phase-badge');
                    if (fallbackBadge) {
                        if (data.fallback_phase && data.fallback_phase !== 'manual' && data.fallback_phase !== 'latched') {
                            fallbackBadge.style.display = 'inline-block';
                            let phaseLabel = 'RECOVERY';
                            if (data.fallback_phase === 'local') phaseLabel = '1: LOCAL NVS';
                            else if (data.fallback_phase === 'cloud') phaseLabel = '2: CLOUD NVS';
                            else if (data.fallback_phase === 'ws1001_scan' || data.fallback_phase === 'scan_targeted') phaseLabel = '3: WS1001 SCAN';
                            else if (data.fallback_phase === 'ws1300_sweep' || data.fallback_phase === 'scan_universal') phaseLabel = '4: WS1300 SWEEP';
                            else if (data.fallback_phase === 'cooldown') phaseLabel = '5: COOLDOWN';
                            
                            let remText = (data.fallback_sec_remaining !== undefined && data.fallback_sec_remaining > 0)
                                ? (' (' + data.fallback_sec_remaining + 's)') : '';
                            fallbackBadge.textContent = phaseLabel + remText;
                            fallbackBadge.style.background = 'rgba(255, 183, 77, 0.15)';
                            fallbackBadge.style.color = '#ffb74d';
                            fallbackBadge.style.border = '1px solid rgba(255, 183, 77, 0.3)';
                        } else {
                            fallbackBadge.style.display = 'none';
                        }
                    }

                    // Distinguish an enabled pipeline from an actually verified link.
                    const uplinkBadge = document.getElementById('uplink-badge');
                    if (uplinkBadge) {
                        if (data.uplink_active) {
                            uplinkBadge.textContent = 'uplink active';
                            uplinkBadge.style.background = 'rgba(76, 175, 80, 0.1)';
                            uplinkBadge.style.color = '#4caf50';
                            uplinkBadge.style.border = '1px solid rgba(76, 175, 80, 0.2)';
                        } else if (data.uplink_enabled) {
                            uplinkBadge.textContent = 'uplink waiting for station data';
                            uplinkBadge.style.background = 'rgba(255, 183, 77, 0.1)';
                            uplinkBadge.style.color = '#ffb74d';
                            uplinkBadge.style.border = '1px solid rgba(255, 183, 77, 0.25)';
                        } else {
                            uplinkBadge.textContent = 'uplink disabled';
                            uplinkBadge.style.background = 'rgba(255,255,255,0.05)';
                            uplinkBadge.style.color = 'var(--text-muted)';
                            uplinkBadge.style.border = '1px solid var(--card-border)';
                        }
                    }

                    // Explain the active mode in one line under the buttons
                    const modeCaption = document.getElementById('pair-mode-caption');
                    if (modeCaption) {
                        if (pairState === 'off') {
                            modeCaption.textContent = `Uplink is disabled. The ${data.active_source || 'selected'} target remains saved for later.`;
                        } else if (pairState === 'cloud') {
                            modeCaption.textContent = data.cloud_profile_ready
                                ? 'The radio targets the cached cloud assignment. The local profile is preserved.'
                                : 'Waiting for a complete cloud assignment. Any saved local profile is preserved as a fallback.';
                        } else if (pairState === 'local') {
                            modeCaption.textContent = 'The radio targets the saved local station. The cloud assignment remains cached and unchanged.';
                        } else if (pairState === 'auto') {
                            modeCaption.textContent = 'Station discovery scan active. Select Cloud or Local to stop scan and resume normal operation.';
                        }
                    }

                    // Always show the two persisted identities and the exact profile
                    // currently driving the radio.
                    if (matchContainer) {
                        matchContainer.style.display = 'block';
                        const fmtId = (valid, id) => valid
                            ? '0x' + (id >>> 0).toString(16).toUpperCase().padStart(8, '0')
                            : 'None';
                        const fmtSn = (valid, sn) => valid && sn
                            ? sn.toUpperCase()
                            : '—';
                        const isDummySn = (sn) => {
                            if (!sn) return true;
                            const s = String(sn).trim().toUpperCase();
                            return !s || s === '—' || s.includes('FFFFFF') || /^0+$/.test(s) || /^F+$/.test(s);
                        };
                        const hideCloudSn = (data.station_type === 1001 || data.cloud_ws_type === 1001 || isWs1001) &&
                                            (isDummySn(data.cloud_ws_sn) || data.cloud_ws_type === 1001);
                        const hideLocalSn = (data.station_type === 1001 || data.local_ws_type === 1001 || isWs1001) &&
                                            (isDummySn(data.local_ws_sn) || data.local_ws_type === 1001);
                        const localPresent = (data.local_ws_present !== undefined)
                            ? !!data.local_ws_present : !!data.local_ws_valid;
                        const localReady = (data.local_profile_ready !== undefined)
                            ? !!data.local_profile_ready : !!data.local_ws_valid;
                        const cloudReady = !!data.cloud_profile_ready;
                        const cloudPresent = !!data.cloud_ws_valid;
                        const bothProfilesReady = cloudReady && localReady;
                        const identitiesDiffer = bothProfilesReady && !data.cloud_ws_match && (data.cloud_ws_id >>> 0) !== (data.local_ws_id >>> 0);
                        const cloudId = fmtId(cloudPresent, data.cloud_ws_id);
                        const localId = fmtId(localPresent, data.local_ws_id);
                        const activeId = fmtId(isPaired, data.paired_id);
                        const isReceiveOnly = (data.pair_mode === 'off' || data.ownership_status === 'receive_only' || data.operation_mode === 'listen' || data.operation_mode === 'off');
                        const isCloudActive = data.active_source === 'cloud' && isPaired;
                        const isLocalActive = data.active_source === 'local' && isPaired;

                        let ownershipBadgeColor = '#ffb74d';
                        let ownershipBadgeText = 'UNVERIFIED';
                        if (!isPaired) {
                            ownershipBadgeColor = 'var(--text-muted)';
                            ownershipBadgeText = 'NOT PAIRED';
                        } else if (isReceiveOnly) {
                            ownershipBadgeColor = '#00d2ff';
                            ownershipBadgeText = 'RECEIVE ONLY';
                        } else if (data.ownership_status === 'confirmed') {
                            ownershipBadgeColor = '#81c784';
                            ownershipBadgeText = 'CONFIRMED';
                        } else if (data.ownership_status === 'pending') {
                            ownershipBadgeColor = '#ffca28';
                            ownershipBadgeText = 'VERIFYING';
                        } else if (data.ownership_status === 'failed') {
                            ownershipBadgeColor = '#ff416c';
                            ownershipBadgeText = 'NOT CONFIRMED';
                        }

                        const isScanningNow = !!data.scan_active || (data.pair_mode === 'scan') || (pairState === 'auto');
                        const verifyBtn = (isPaired && !isScanningNow && !isReceiveOnly && data.ownership_status !== 'confirmed') ? `
                            <button onclick="requestGpsFix(true)" class="btn-sm-ctrl" style="color:${ownershipBadgeColor}; border-color:${ownershipBadgeColor}66; padding:0.2rem 0.5rem; font-size:0.68rem;" ${data.ownership_status === 'pending' ? 'disabled' : ''}>
                                Verify GPS
                            </button>
                        ` : '';

                        const getWs1300SlotFromSn = (sn) => {
                            if (!sn || typeof sn !== 'string') return 0;
                            const clean = sn.trim().toUpperCase();
                            if (clean.length < 2) return 0;
                            const lastByte = parseInt(clean.slice(-2), 16);
                            if (isNaN(lastByte)) return 0;
                            return (lastByte % 8) + 1;
                        };

                        const isSynthSn = (sn, synthFlag) => {
                            if (synthFlag !== undefined) return !!synthFlag;
                            if (!sn) return false;
                            const s = String(sn).trim().toUpperCase();
                            return s.length === 20 && s.slice(8, 16) === 'AA55AA55';
                        };
                        const cloudSnSynth = isSynthSn(data.cloud_ws_sn, data.cloud_sn_synthesized);
                        const localSnSynth = isSynthSn(data.local_ws_sn, data.local_sn_synthesized);
                        const cloudHasPk = !!data.cloud_has_pubkey;
                        const localHasPk = !!data.local_has_pubkey;

                        const cloudSlot = data.cloud_ws_type === 1300 ? getWs1300SlotFromSn(data.cloud_ws_sn) : 0;
                        const cloudChanText = data.cloud_ws_type === 1300
                            ? (cloudSlot === 0 ? 'Base (868.0 MHz)' : `CH${cloudSlot - 1} (${(868.1 + (cloudSlot - 1) * 0.2).toFixed(1)} MHz)`)
                            : 'Base (868.0 MHz)';

                        const localSlot = data.local_ws_type === 1300 ? getWs1300SlotFromSn(data.local_ws_sn) : 0;
                        const localChanText = data.local_ws_type === 1300
                            ? (localSlot === 0 ? 'Base (868.0 MHz)' : `CH${localSlot - 1} (${(868.1 + (localSlot - 1) * 0.2).toFixed(1)} MHz)`)
                            : 'Base (868.0 MHz)';

                        const fmtKnownSnHtml = (wsType, valid, sn, isSynth, slot) => {
                            if (wsType === 1001) return `<span style="color:var(--text-muted);">N/A (WS1001)</span>`;
                            if (!valid || isDummySn(sn)) return `<span style="color:#ffb74d; font-weight:600;">Missing</span>`;
                            if (isSynth) return `<span style="color:#ffb74d; font-weight:600;" title="Synthesized channel-hint serial (${sn})">Synthesized (${slot > 0 ? 'CH' + (slot - 1) : 'Base'} hint)</span>`;
                            return `<span style="font-family:monospace; color:#81c784;">${fmtSn(valid, sn)}</span>`;
                        };
                        const fmtKnownPkHtml = (wsType, hasPk) => {
                            if (wsType === 1001) return `<span style="color:var(--text-muted);">N/A</span>`;
                            return hasPk
                                ? `<span style="color:#81c784; font-weight:600;">✔ Present</span>`
                                : `<span style="color:#ffb74d; font-weight:600;" title="ECDSA signature check bypassed; telemetry still active. Run REPAIR to capture full credentials from BOOT.">⚠ Missing (unverified)</span>`;
                        };

                        const isManualLock = (data.manual_slot !== undefined && data.manual_slot >= 0);
                        const manualChanDesc = isManualLock
                            ? (data.manual_slot === 0 ? 'Base Frequency' : `WS1300 CH${data.manual_slot - 1}`)
                            : '';
                        const manualLockBannerHtml = isManualLock ? `
                            <div style="margin-bottom:0.55rem; padding:0.45rem 0.75rem; border-radius:6px; background:rgba(0,210,255,0.1); border:1px solid rgba(0,210,255,0.4); display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:0.5rem;">
                                <div style="font-size:0.74rem; color:#80deea;">
                                    🔒 <strong>Manual Channel Lock Active (${manualChanDesc}):</strong> Auto-pairing and background channel scanning are paused while locked to a manual channel.
                                </div>
                                <button onclick="setChannel(-1)" class="btn-sm-ctrl" style="color:#111; background:#00d2ff; border-color:#00d2ff; font-size:0.68rem; padding:0.2rem 0.55rem; font-weight:700;">
                                    Return to Auto Channel
                                </button>
                            </div>` : '';

                        if (isPaired && !identitiesDiffer) {
                            // OPTION A - MATCHED / NORMAL STATE
                            const activeModel = data.station_type ? `WS${data.station_type}` : (data.local_ws_type ? `WS${data.local_ws_type}` : (data.cloud_ws_type ? `WS${data.cloud_ws_type}` : ''));
                            const isSynced = (data.cloud_ws_match || (cloudReady && localReady));
                            const syncBadge = isSynced
                                ? `<span style="font-size:0.72rem; color:#81c784; font-weight:600; display:inline-flex; align-items:center; gap:0.25rem;">✔ Cloud &amp; Local Synced</span>`
                                : (cloudReady
                                    ? `<span style="font-size:0.72rem; color:#81c784; font-weight:600;">☁ Cloud Profile</span>`
                                    : `<span style="font-size:0.72rem; color:#00d2ff; font-weight:600;">📻 Local Profile</span>`);

                            const targetSource = (data.active_source || 'local').toUpperCase();
                            const activeSlot = (data.station_type === 1300) ? (isCloudActive ? cloudSlot : localSlot) : 0;
                            const activeChanText = isCloudActive ? cloudChanText : localChanText;
                            const activeSn = isCloudActive ? data.cloud_ws_sn : data.local_ws_sn;
                            const activeSnSynth = isCloudActive ? cloudSnSynth : localSnSynth;
                            const activeHasPk = isCloudActive ? cloudHasPk : localHasPk;
                            const activeIdNum = (data.paired_id >>> 0) || (isCloudActive ? (data.cloud_ws_id >>> 0) : (data.local_ws_id >>> 0));
                            const activeNeedsRepair = (data.station_type === 1300) && (!activeHasPk || activeSnSynth || isDummySn(activeSn) || data.ownership_status === 'failed');
                            const activeNeedsBootHint = (data.station_type === 1300) && (activeNeedsRepair || data.ownership_status !== 'confirmed');
                            const downlinkRejectedWarn = (data.station_type === 1300 && data.ownership_status === 'failed' && activeHasPk);

                            matchContainer.innerHTML = `
                                ${manualLockBannerHtml}
                                <div style="padding:0.6rem 0.85rem; border:1px solid ${activeNeedsRepair ? 'rgba(255,183,77,0.45)' : 'rgba(129,199,132,0.3)'}; border-radius:8px; background:rgba(129,199,132,0.03); display:flex; flex-direction:column; gap:0.45rem; min-height:44px; box-sizing:border-box;">
                                    <div style="display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:0.6rem;">
                                        <div style="display:flex; align-items:center; gap:0.65rem; flex-wrap:wrap;">
                                            <span style="font-size:0.72rem; font-weight:700; color:#81c784; text-transform:uppercase; letter-spacing:0.5px;">Active Station</span>
                                            <span style="font-family:monospace; font-size:1.05rem; font-weight:700; color:#fff; letter-spacing:0.5px;">${activeId}</span>
                                            ${activeModel ? `<span style="font-size:0.75rem; color:var(--text-muted); font-weight:600;">${activeModel}</span>` : ''}
                                            <span style="font-size:0.68rem; padding:0.15rem 0.45rem; border-radius:4px; background:rgba(255,255,255,0.07); color:var(--text-muted); font-weight:600;">TARGET: ${targetSource}</span>
                                            ${syncBadge}
                                            ${activeNeedsRepair ? `<span style="font-size:0.66rem; font-weight:700; padding:0.15rem 0.45rem; border-radius:4px; background:rgba(255,183,77,0.16); color:#ffb74d; border:1px solid rgba(255,183,77,0.4);" title="Station credentials incomplete (missing PubKey or full SN) or ownership unconfirmed. Reboot the station or click REPAIR to capture its BOOT packet.">⚠ REPAIR RECOMMENDED</span>` : ''}
                                        </div>
                                        <div style="display:flex; align-items:center; gap:0.4rem; flex-wrap:wrap;">
                                            <button onclick="listenToRecent(${activeSlot}, ${activeIdNum})" class="btn-sm-ctrl" style="color:#00d2ff; border-color:rgba(0,210,255,0.45); font-size:0.68rem; padding:0.2rem 0.5rem; font-weight:700;" title="Listen / sniff on this station's channel">LISTEN</button>
                                            ${data.station_type === 1300 ? `<button onclick="startTargetedRepair(${activeIdNum}, '${targetSource}', ${activeSlot})" class="btn-sm-ctrl" style="color:${activeNeedsRepair ? '#111' : '#ffb74d'}; background:${activeNeedsRepair ? '#ffb74d' : 'transparent'}; border-color:rgba(255,183,77,0.65); font-size:0.68rem; padding:0.2rem 0.5rem; font-weight:700;" title="Send authenticated reboot command to capture full SN + Public Key from BOOT">REPAIR</button>` : ''}
                                            ${!isCloudActive && localPresent ? `<button onclick="clearLocalProfile()" class="btn-sm-ctrl" style="color:#ff8a80; border-color:rgba(255,82,82,0.45); font-size:0.68rem; padding:0.2rem 0.5rem; font-weight:700;" title="Delete saved Local NVS station profile">CLEAR LOCAL</button>` : ''}
                                            ${!isScanningNow ? `
                                            <span style="font-size:0.68rem; font-weight:700; color:${ownershipBadgeColor}; background:rgba(255,255,255,0.04); border:1px solid ${ownershipBadgeColor}55; padding:0.2rem 0.5rem; border-radius:4px;" title="Gateway decodes, displays, and uploads telemetry even when ownership is unverified">
                                                OWNERSHIP ${ownershipBadgeText}
                                            </span>
                                            ${verifyBtn}` : ''}
                                        </div>
                                    </div>
                                    <div style="font-size:0.7rem; color:var(--text-muted); display:flex; flex-wrap:wrap; gap:0.85rem; padding-top:0.25rem; border-top:1px solid rgba(255,255,255,0.06);">
                                        <span>Channel: <strong style="color:#ddd;">${activeChanText}</strong></span>
                                        <span>SN: ${fmtKnownSnHtml(data.station_type, true, activeSn, activeSnSynth, activeSlot)}</span>
                                        <span>PubKey: ${fmtKnownPkHtml(data.station_type, activeHasPk)}</span>
                                    </div>
                                    ${downlinkRejectedWarn ? `
                                    <div style="font-size:0.7rem; color:#ff8a80; background:rgba(255,65,108,0.1); border:1px solid rgba(255,65,108,0.35); border-radius:5px; padding:0.35rem 0.6rem;">
                                        ⚠ <strong>Station did not accept downlink challenge:</strong> Public Key is present (telemetry is verified and uploaded), but the station ignored our signed GPS request. Either <strong>the station is paired to a different gateway's serial</strong>, or <strong>another gateway in range is transmitting simultaneously</strong> and colliding during the 300 ms RX window.
                                    </div>` : ''}
                                    ${activeNeedsBootHint ? `
                                    <div style="font-size:0.7rem; color:#ffcc80; background:rgba(255,183,77,0.07); border:1px solid rgba(255,183,77,0.22); border-radius:5px; padding:0.3rem 0.55rem;">
                                        ℹ <strong>Locked to station (no timeout):</strong> To take ownership and capture full SN &amp; Public Key, <strong>reboot/reset the weather station</strong> (or click <strong>REPAIR</strong> if already paired to this gateway) so it transmits a <code>BOOT</code>/<code>ADV</code> packet.
                                    </div>` : ''}
                                </div>`;
                        } else if (cloudPresent || localPresent) {
                            // SHOW DETAILED CARDS: When profiles differ OR gateway is unpaired/scanning but profile(s) exist
                            let cardsHtml = '';

                            if (cloudPresent) {
                                const cloudNeedsRepair = (data.cloud_ws_type === 1300) && (!cloudHasPk || cloudSnSynth || isDummySn(data.cloud_ws_sn));
                                const cloudBadge = isCloudActive
                                    ? '<span style="background:rgba(129,199,132,0.2);color:#81c784;border:1px solid #81c784;padding:0.15rem 0.4rem;border-radius:4px;font-size:0.65rem;font-weight:700;">ACTIVE TARGET</span>'
                                    : '<span style="background:rgba(255,183,77,0.15);color:#ffb74d;border:1px solid #ffb74d;padding:0.15rem 0.4rem;border-radius:4px;font-size:0.65rem;font-weight:700;">SAVED IN CLOUD</span>';

                                const cloudButtons = [];
                                cloudButtons.push(`<button onclick="listenToRecent(${cloudSlot}, ${data.cloud_ws_id >>> 0})" class="btn-sm-ctrl" style="color:#00d2ff; border-color:rgba(0,210,255,0.45); font-size:0.68rem; padding:0.2rem 0.5rem; font-weight:700;" title="Listen / sniff on this station's channel">LISTEN</button>`);
                                if (data.cloud_ws_type === 1300) {
                                    cloudButtons.push(`<button onclick="startTargetedRepair(${data.cloud_ws_id >>> 0}, 'cloud', ${cloudSlot})" class="btn-sm-ctrl" style="color:${cloudNeedsRepair ? '#111' : '#ffb74d'}; background:${cloudNeedsRepair ? '#ffb74d' : 'transparent'}; border-color:rgba(255,183,77,0.65); font-size:0.68rem; padding:0.2rem 0.5rem; font-weight:700;" title="Send authenticated reboot command to capture full SN + Public Key from BOOT">REPAIR</button>`);
                                }
                                if (!isCloudActive) {
                                    cloudButtons.push(`<button onclick="switchActiveTarget('cloud')" class="btn-sm-ctrl" style="color:#81c784; border-color:rgba(129,199,132,0.45); font-size:0.68rem; padding:0.2rem 0.5rem; font-weight:700;" title="Switch radio target to this cloud profile">TARGET THIS</button>`);
                                }

                                cardsHtml += `
                                    <div style="background:rgba(255,255,255,0.03); border:1px solid ${isCloudActive ? '#81c784' : 'var(--card-border)'}; border-radius:8px; padding:0.65rem 0.85rem; display:flex; flex-direction:column; justify-content:space-between; gap:0.4rem;">
                                        <div>
                                            <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:0.35rem; gap:0.4rem; flex-wrap:wrap;">
                                                <span style="font-size:0.75rem; font-weight:700; color:#81c784; text-transform:uppercase;">Cloud Saved</span>
                                                <div style="display:flex; gap:0.3rem; align-items:center;">
                                                    ${cloudNeedsRepair ? '<span style="background:rgba(255,183,77,0.16);color:#ffb74d;border:1px solid rgba(255,183,77,0.4);padding:0.12rem 0.38rem;border-radius:4px;font-size:0.62rem;font-weight:700;" title="Missing PubKey or full SN; click REPAIR to capture BOOT credentials">⚠ REPAIR NEEDED</span>' : ''}
                                                    ${cloudBadge}
                                                </div>
                                            </div>
                                            <div style="display:flex; align-items:baseline; gap:0.5rem;">
                                                <span style="font-family:monospace; font-size:1.15rem; font-weight:700; color:#fff;">${fmtId(cloudPresent, data.cloud_ws_id)}</span>
                                                <span style="font-size:0.75rem; color:var(--text-muted); font-weight:600;">${data.cloud_ws_type ? `WS${data.cloud_ws_type}` : 'WS1300'}</span>
                                            </div>
                                            <div style="font-size:0.7rem; color:var(--text-muted); margin-top:0.25rem; display:flex; flex-wrap:wrap; gap:0.65rem;">
                                                <span>Channel: <strong style="color:#ddd;">${cloudChanText}</strong></span>
                                                <span>SN: ${fmtKnownSnHtml(data.cloud_ws_type, cloudPresent, data.cloud_ws_sn, cloudSnSynth, cloudSlot)}</span>
                                                <span>PubKey: ${fmtKnownPkHtml(data.cloud_ws_type, cloudHasPk)}</span>
                                            </div>
                                        </div>
                                        <div style="display:flex; flex-wrap:wrap; gap:0.35rem; margin-top:0.5rem;">
                                            ${cloudButtons.join('')}
                                        </div>
                                    </div>`;
                            }

                            if (localPresent) {
                                const isLocalActive = (data.active_source === 'local');
                                const localNeedsRepair = (data.local_ws_type === 1300) && (!localHasPk || localSnSynth || isDummySn(data.local_ws_sn));
                                const localBadge = isLocalActive
                                    ? '<span style="background:rgba(129,199,132,0.2);color:#81c784;border:1px solid #81c784;padding:0.15rem 0.4rem;border-radius:4px;font-size:0.65rem;font-weight:700;">ACTIVE TARGET</span>'
                                    : '<span style="background:rgba(0,210,255,0.15);color:#00d2ff;border:1px solid #00d2ff;padding:0.15rem 0.4rem;border-radius:4px;font-size:0.65rem;font-weight:700;">SAVED IN LOCAL</span>';

                                const localButtons = [];
                                localButtons.push(`<button onclick="listenToRecent(${localSlot}, ${data.local_ws_id >>> 0})" class="btn-sm-ctrl" style="color:#00d2ff; border-color:rgba(0,210,255,0.45); font-size:0.68rem; padding:0.2rem 0.5rem; font-weight:700;" title="Listen / sniff on this station's channel">LISTEN</button>`);
                                if (data.local_ws_type === 1300) {
                                    localButtons.push(`<button onclick="startTargetedRepair(${data.local_ws_id >>> 0}, 'local', ${localSlot})" class="btn-sm-ctrl" style="color:${localNeedsRepair ? '#111' : '#ffb74d'}; background:${localNeedsRepair ? '#ffb74d' : 'transparent'}; border-color:rgba(255,183,77,0.65); font-size:0.68rem; padding:0.2rem 0.5rem; font-weight:700;" title="Send authenticated reboot command to capture full SN + Public Key from BOOT">REPAIR</button>`);
                                }
                                if (!isLocalActive) {
                                    localButtons.push(`<button onclick="switchActiveTarget('local')" class="btn-sm-ctrl" style="color:#81c784; border-color:rgba(129,199,132,0.45); font-size:0.68rem; padding:0.2rem 0.5rem; font-weight:700;" title="Switch radio target to this local profile">TARGET THIS</button>`);
                                }
                                localButtons.push(`<button onclick="clearLocalProfile()" class="btn-sm-ctrl" style="color:#ff8a80; border-color:rgba(255,82,82,0.45); font-size:0.68rem; padding:0.2rem 0.5rem; font-weight:700;" title="Delete saved Local NVS station profile">CLEAR LOCAL</button>`);

                                cardsHtml += `
                                    <div style="background:rgba(255,255,255,0.03); border:1px solid ${isLocalActive ? '#81c784' : 'var(--card-border)'}; border-radius:8px; padding:0.65rem 0.85rem; display:flex; flex-direction:column; justify-content:space-between; gap:0.4rem;">
                                        <div>
                                            <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:0.35rem; gap:0.4rem; flex-wrap:wrap;">
                                                <span style="font-size:0.75rem; font-weight:700; color:#00d2ff; text-transform:uppercase;">Local Saved</span>
                                                <div style="display:flex; gap:0.3rem; align-items:center;">
                                                    ${localNeedsRepair ? '<span style="background:rgba(255,183,77,0.16);color:#ffb74d;border:1px solid rgba(255,183,77,0.4);padding:0.12rem 0.38rem;border-radius:4px;font-size:0.62rem;font-weight:700;" title="Missing PubKey or full SN; click REPAIR to capture BOOT credentials">⚠ REPAIR NEEDED</span>' : ''}
                                                    ${localBadge}
                                                </div>
                                            </div>
                                            <div style="display:flex; align-items:baseline; gap:0.5rem;">
                                                <span style="font-family:monospace; font-size:1.15rem; font-weight:700; color:#fff;">${fmtId(localPresent, data.local_ws_id)}</span>
                                                <span style="font-size:0.75rem; color:var(--text-muted); font-weight:600;">${data.local_ws_type ? `WS${data.local_ws_type}` : 'WS1300'}</span>
                                            </div>
                                            <div style="font-size:0.7rem; color:var(--text-muted); margin-top:0.25rem; display:flex; flex-wrap:wrap; gap:0.65rem;">
                                                <span>Channel: <strong style="color:#ddd;">${localChanText}</strong></span>
                                                <span>SN: ${fmtKnownSnHtml(data.local_ws_type, localPresent, data.local_ws_sn, localSnSynth, localSlot)}</span>
                                                <span>PubKey: ${fmtKnownPkHtml(data.local_ws_type, localHasPk)}</span>
                                            </div>
                                        </div>
                                        <div style="display:flex; flex-wrap:wrap; gap:0.35rem; margin-top:0.5rem;">
                                            ${localButtons.join('')}
                                        </div>
                                    </div>`;
                            }

                            let bottomBannerHtml = '';
                            if (!isScanningNow) {
                                const activeIsWs1300 = (data.station_type === 1300);
                                const activeHasPkCard = isCloudActive ? cloudHasPk : localHasPk;
                                const downlinkRejectedCard = isPaired && activeIsWs1300 && data.ownership_status === 'failed' && activeHasPkCard;
                                const activeCardNeedsBoot = isPaired && activeIsWs1300 && (
                                    data.ownership_status !== 'confirmed' ||
                                    (isCloudActive && (!cloudHasPk || cloudSnSynth || isDummySn(data.cloud_ws_sn))) ||
                                    (!isCloudActive && (!localHasPk || localSnSynth || isDummySn(data.local_ws_sn)))
                                );
                                const bootInstruction = activeCardNeedsBoot
                                    ? ` To take ownership and capture full SN &amp; Public Key, <strong>reboot/reset the station</strong> (or click <strong>REPAIR</strong>) so it sends a <code>BOOT</code>/<code>ADV</code> packet.`
                                    : '';
                                const footerBanner = !isPaired
                                    ? `<div style="font-size:0.74rem; color:#ffcc80;">
                                           ℹ Gateway is currently unpaired. Click <strong>TARGET THIS</strong> above to lock onto a saved station (without timeout), or use the controls below.
                                       </div>`
                                    : `<div style="font-size:0.74rem; color:#ffcc80;">
                                           ℹ Radio locked to <strong>${(data.active_source || 'target').toUpperCase()} (${activeId})</strong> (no timeout).${bootInstruction}
                                       </div>`;

                                const ownershipSection = isPaired ? `
                                    <div style="display:flex; align-items:center; gap:0.4rem;">
                                        <span style="font-size:0.68rem; font-weight:700; color:${ownershipBadgeColor}; background:rgba(255,255,255,0.04); border:1px solid ${ownershipBadgeColor}55; padding:0.15rem 0.45rem; border-radius:4px;">
                                            OWNERSHIP ${ownershipBadgeText}
                                        </span>
                                        ${verifyBtn}
                                    </div>` : '';

                                bottomBannerHtml = `
                                    ${downlinkRejectedCard ? `
                                    <div style="margin-top:0.5rem; font-size:0.7rem; color:#ff8a80; background:rgba(255,65,108,0.1); border:1px solid rgba(255,65,108,0.35); border-radius:6px; padding:0.4rem 0.7rem;">
                                        ⚠ <strong>Station did not accept downlink challenge:</strong> Public Key is present (telemetry is verified and uploaded), but the station ignored our signed GPS request. Either <strong>the station is paired to a different gateway's serial</strong>, or <strong>another gateway in range is transmitting simultaneously</strong> and colliding during the 300 ms RX window.
                                    </div>` : ''}
                                    <div style="margin-top:0.5rem; padding:0.45rem 0.75rem; border-radius:6px; background:rgba(255,183,77,0.07); border:1px solid rgba(255,183,77,0.25); display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:0.5rem;">
                                        ${footerBanner}
                                        ${ownershipSection}
                                    </div>`;
                            }

                            matchContainer.innerHTML = `
                                ${manualLockBannerHtml}
                                <div style="display:grid; grid-template-columns:repeat(auto-fit,minmax(280px,1fr)); gap:0.6rem;">
                                    ${cardsHtml}
                                </div>
                                ${bottomBannerHtml}`;
                        } else {
                            // Completely unpaired with NO saved cloud or local profile
                            matchContainer.innerHTML = `
                                ${manualLockBannerHtml}
                                <div style="padding:0.6rem 0.85rem; border:1px solid var(--card-border); border-radius:8px; background:rgba(255,255,255,0.02); display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:0.5rem;">
                                    <div style="font-size:0.75rem; color:var(--text-muted);">
                                        <strong style="color:#ffb74d;">No station paired.</strong> Select <strong>SCAN</strong> below to detect nearby weather stations, or assign via WeatherXM Cloud.
                                    </div>
                                </div>`;
                        }
                    }

                    // Scan Progress, Station Scan Controls & ETA Countdown
                    const scanContainer = document.getElementById('scan-progress-info');
                    const scanIdleCaption = document.getElementById('scan-idle-caption');
                    const scanBadge = document.getElementById('scan-state-badge');
                    const scanBtn = document.getElementById('btn-scan-toggle');
                    const scanText = document.getElementById('btn-scan-text');
                    const scanIcon = document.getElementById('btn-scan-icon');

                    const isScanActive = (pairState === 'auto') || (data.pair_mode === 'scan') || !!data.scan_active || !!data.scan_completed;
                    const isDone = !!data.scan_completed;
                    g_isScanningActive = !!data.scan_active;
                    g_scanEtaSec = data.scan_eta_sec || 0;
                    g_scanEtaReceivedAt = Date.now();
                    g_scanEtaIsDone = isDone;

                    if (scanIdleCaption) {
                        scanIdleCaption.style.display = isScanActive ? 'none' : 'block';
                        if (!isScanActive) {
                            if (activeScanType === 1300) {
                                scanIdleCaption.textContent = 'Sweeps WS1300 Multi-Channel CH0–CH7 in 2 passes (quick + 5-min deep). ~42 min total.';
                            } else if (activeScanType === 1001) {
                                scanIdleCaption.textContent = 'Scans WS1001 Single Channel Base frequency in 2 passes. ~6 min total.';
                            } else {
                                scanIdleCaption.textContent = 'Sweeps WS1001 base + WS1300 CH0–CH7 in 2 passes (quick + 5-min deep). ~48 min total.';
                            }
                        }
                    }

                    if (scanBadge) {
                        if (data.scan_active) {
                            scanBadge.textContent = 'SCANNING';
                            scanBadge.style.background = 'rgba(187, 134, 252, 0.15)';
                            scanBadge.style.color = '#bb86fc';
                            scanBadge.style.border = '1px solid rgba(187, 134, 252, 0.3)';
                        } else if (isDone) {
                            scanBadge.textContent = 'COMPLETE';
                            scanBadge.style.background = 'rgba(129, 199, 132, 0.15)';
                            scanBadge.style.color = '#81c784';
                            scanBadge.style.border = '1px solid rgba(129, 199, 132, 0.3)';
                        } else {
                            scanBadge.textContent = 'IDLE';
                            scanBadge.style.background = 'rgba(255, 255, 255, 0.05)';
                            scanBadge.style.color = 'var(--text-muted)';
                            scanBadge.style.border = '1px solid rgba(255, 255, 255, 0.1)';
                        }
                    }

                    if (scanBtn && scanText) {
                        const scopeName = (activeScanType === 1300) ? 'WS1300' : ((activeScanType === 1001) ? 'WS1001' : 'ALL');
                        if (data.scan_active) {
                            scanText.textContent = 'STOP SCAN';
                            if (scanIcon) scanIcon.textContent = '⏹';
                            scanBtn.style.background = 'rgba(255, 82, 82, 0.18)';
                            scanBtn.style.borderColor = '#ff5252';
                            scanBtn.style.color = '#ff5252';
                            scanBtn.title = 'Stop channel sweep and return to normal mode';
                        } else if (isDone) {
                            scanText.textContent = `RESCAN (${scopeName})`;
                            if (scanIcon) scanIcon.textContent = '🔄';
                            scanBtn.style.background = 'rgba(187, 134, 252, 0.18)';
                            scanBtn.style.borderColor = '#bb86fc';
                            scanBtn.style.color = '#bb86fc';
                            scanBtn.title = 'Run another channel sweep';
                        } else {
                            scanText.textContent = `START SCAN (${scopeName})`;
                            if (scanIcon) scanIcon.textContent = '🔍';
                            scanBtn.style.background = 'rgba(187, 134, 252, 0.18)';
                            scanBtn.style.borderColor = '#bb86fc';
                            scanBtn.style.color = '#bb86fc';
                            scanBtn.title = 'Sweep channels to discover nearby stations';
                        }
                    }

                    if (scanContainer) {
                        if (isScanActive && (pairState !== 'off' || data.pair_mode === 'scan')) {
                            scanContainer.style.display = 'block';
                            const curSlot = (data.scan_slot !== undefined) ? data.scan_slot : 0;
                            const totalSlots = data.scan_total_slots || 8;
                            const cycle = (data.scan_cycle !== undefined) ? data.scan_cycle : 0;
                            const etaSec = data.scan_eta_sec || 0;
                            const etaMinStr = formatEtaCountdown(etaSec);
                            const scanProfile = (data.scan_profile !== undefined) ? data.scan_profile : (data.station_type === 1300 ? 1 : (data.station_type === 1001 ? 2 : 0));
                            const is1300Only = (scanProfile === 1 || (scanProfile === undefined && totalSlots === 8));
                            const is1001Only = (scanProfile === 2 || (scanProfile === undefined && totalSlots === 1));
                            const isCombo = (!is1300Only && !is1001Only);

                            let passLabel;
                            let slotName;
                            if (isCombo) {
                                let passNum = 1;
                                let passName = 'WS1001 Quick Sweep';
                                if (cycle === 0) {
                                    if (data.scan_phase === 'ws1300') {
                                        passNum = 2;
                                        passName = 'WS1300 Quick Sweep';
                                    } else {
                                        passNum = 1;
                                        passName = 'WS1001 Quick Sweep';
                                    }
                                } else {
                                    if (data.scan_phase === 'ws1300') {
                                        passNum = 4;
                                        passName = 'WS1300 5-Min Deep Sweep';
                                    } else {
                                        passNum = 3;
                                        passName = 'WS1001 5-Min Deep Sweep';
                                    }
                                }
                                passLabel = isDone
                                    ? 'Scan Complete (All 4 Passes Finished)'
                                    : `Pass ${passNum}/4 (${passName})`;
                                slotName = (data.scan_phase === 'ws1001' || curSlot === 0)
                                    ? 'Base Freq (WS1001 / ADV)'
                                    : `WS1300 CH${curSlot > 0 ? curSlot - 1 : 0}`;
                            } else if (is1300Only) {
                                passLabel = isDone
                                    ? 'Scan Complete (2 Passes Finished)'
                                    : (cycle === 0 ? 'Pass 1/2 (Quick Sweep)' : 'Pass 2/2 (5-Min Deep Sweep)');
                                slotName = `WS1300 CH${curSlot > 0 ? curSlot - 1 : 0}`;
                            } else {
                                passLabel = isDone
                                    ? 'Scan Complete (2 Passes Finished)'
                                    : (cycle === 0 ? 'Pass 1/2 (Quick Sweep)' : 'Pass 2/2 (5-Min Deep Sweep)');
                                slotName = 'Base Freq (WS1001 / ADV)';
                            }

                            let pillsHtml = '';
                            if (is1300Only) {
                                for (let i = 1; i <= 8; i++) {
                                    const isActive = (!isDone && i === curSlot);
                                    const bg = isActive ? '#bb86fc' : 'rgba(255, 255, 255, 0.1)';
                                    const color = isActive ? '#000' : 'var(--text-muted)';
                                    const label = `CH${i - 1}`;
                                    pillsHtml += `<span style="background: ${bg}; color: ${color}; font-size: 0.65rem; font-weight: 700; padding: 2px 5px; border-radius: 3px; border: ${isActive ? '1px solid #bb86fc' : 'none'};">${label}</span> `;
                                }
                            } else if (is1001Only) {
                                const isActive = !isDone;
                                const bg = isActive ? '#bb86fc' : 'rgba(255, 255, 255, 0.1)';
                                const color = isActive ? '#000' : 'var(--text-muted)';
                                pillsHtml = `<span style="background: ${bg}; color: ${color}; font-size: 0.65rem; font-weight: 700; padding: 2px 5px; border-radius: 3px; border: ${isActive ? '1px solid #bb86fc' : 'none'};">Base (Single Channel)</span>`;
                            } else {
                                for (let i = 0; i < 9; i++) {
                                    let isActive = false;
                                    if (!isDone) {
                                        if (data.scan_phase === 'ws1001') {
                                            isActive = (i === 0);
                                        } else {
                                            isActive = (i === curSlot);
                                        }
                                    }
                                    const bg = isActive ? '#bb86fc' : 'rgba(255, 255, 255, 0.1)';
                                    const color = isActive ? '#000' : 'var(--text-muted)';
                                    const label = (i === 0) ? 'Base' : `CH${i - 1}`;
                                    pillsHtml += `<span style="background: ${bg}; color: ${color}; font-size: 0.65rem; font-weight: 700; padding: 2px 5px; border-radius: 3px; border: ${isActive ? '1px solid #bb86fc' : 'none'};">${label}</span> `;
                                }
                            }

                            const headerColor = isDone ? '#81c784' : '#bb86fc';
                            const headerText = isDone
                                ? (isCombo ? '✓ Scan Complete (All 4 Passes Finished) — Waiting for user action' : '✓ Scan Complete (2 Passes Finished) — Waiting for user action')
                                : `${passLabel} · ${slotName}`;
                            const etaText = isDone
                                ? '<span id="scan-eta-display" style="font-weight: 700; color: #81c784;">Sweep Finished</span>'
                                : `<span id="scan-eta-display" style="font-weight: 700; color: #00d2ff;">ETA ~${etaMinStr} remaining</span>`;

                            scanContainer.innerHTML = `<div style="background: ${isDone ? 'rgba(129, 199, 132, 0.08)' : 'rgba(187, 134, 252, 0.08)'}; border: 1px solid ${isDone ? 'rgba(129, 199, 132, 0.35)' : 'rgba(187, 134, 252, 0.25)'}; padding: 0.5rem 0.65rem; border-radius: 6px;">
                                <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 0.35rem;">
                                    <span style="font-weight: 700; color: ${headerColor};">${headerText}</span>
                                    ${etaText}
                                </div>
                                <div style="display: flex; align-items: center; gap: 0.25rem; flex-wrap: wrap;">
                                    <span style="font-size: 0.7rem; color: var(--text-muted); margin-right: 0.25rem;">Channels:</span>
                                    ${pillsHtml}
                                </div>
                            </div>`;
                        } else {
                            scanContainer.style.display = 'none';
                        }
                    }

                    if (btnOff && btnLocal && btnCloud) {
                        // LOCAL may be saved while CLOUD is currently active.
                        // Enable it based on the persisted local profile, not
                        // on whether some (possibly cloud) target is active.
                        btnLocal.disabled = !localProfileReady;
                        btnLocal.style.opacity = localProfileReady ? '1' : '0.4';
                        btnLocal.title = localProfileReady
                            ? 'Switch the radio to the saved local station'
                            : 'No complete local profile is saved - run SCAN first';

                        btnOff.style.background = 'transparent';
                        btnOff.style.color = 'var(--text-muted)';
                        btnLocal.style.background = 'transparent';
                        btnLocal.style.color = 'var(--text-muted)';
                        btnCloud.style.background = 'transparent';
                        btnCloud.style.color = 'var(--text-muted)';
                        btnOff.style.boxShadow = 'none';
                        btnLocal.style.boxShadow = 'none';
                        btnCloud.style.boxShadow = 'none';
                        btnOff.style.border = 'none';
                        btnLocal.style.border = 'none';
                        btnCloud.style.border = 'none';

                        if (pairState === 'off') {
                            btnOff.style.background = 'rgba(255, 65, 108, 0.15)';
                            btnOff.style.color = '#ff416c';
                            btnOff.style.border = '1px solid rgba(255, 65, 108, 0.3)';
                            btnOff.style.boxShadow = '0 2px 6px rgba(255, 65, 108, 0.1)';
                        } else if (pairState === 'local') {
                            btnLocal.style.background = 'rgba(0, 210, 255, 0.15)';
                            btnLocal.style.color = '#00d2ff';
                            btnLocal.style.border = '1px solid rgba(0, 210, 255, 0.3)';
                            btnLocal.style.boxShadow = '0 2px 6px rgba(0, 210, 255, 0.1)';
                        } else if (pairState === 'cloud') {
                            btnCloud.style.background = 'rgba(76, 175, 80, 0.15)';
                            btnCloud.style.color = '#4caf50';
                            btnCloud.style.border = '1px solid rgba(76, 175, 80, 0.3)';
                            btnCloud.style.boxShadow = '0 2px 6px rgba(76, 175, 80, 0.1)';
                        }
                    }

                    if (sniffToggle) {
                        sniffToggle.checked = !!data.sniff_mode;
                        sniffToggle.disabled = isAutoPairOn; // Only lock sniff toggle when actively auto-pairing
                    }

                    // Add/remove .disabled class on control card wrapper containers to style them dynamically
                    const divAutopair = document.getElementById('ctrl-autopair');
                    if (divAutopair) {
                        divAutopair.classList.remove('disabled'); // Always active/interactive
                    }

                    const divSniffer = document.getElementById('ctrl-sniffer');
                    if (divSniffer) {
                        if (isAutoPairOn) divSniffer.classList.add('disabled');
                        else divSniffer.classList.remove('disabled');
                    }

                    // Scope buttons (Any, WS1001, WS1300) are always enabled so user can set scan scope anytime.
                    const isScanMode = (pairState === 'auto') || (data.pair_mode === 'scan') || !!data.scan_active || !!data.scan_completed;
                    const isChannelSelectDisabled = isPaired || isAutoPairOn || isScanMode;
                    const isRegionSelectDisabled = isPaired || isScanMode;

                    if (pairProfileSel) pairProfileSel.disabled = g_isUiLocked || !!data.scan_active;
                    if (scanProfileSel) scanProfileSel.disabled = g_isUiLocked || !!data.scan_active;
                    if (chSel) chSel.disabled = isChannelSelectDisabled;
                    if (regSel) regSel.disabled = isRegionSelectDisabled;

                    const divRegion = document.getElementById('ctrl-region');
                    if (divRegion) {
                        if (isRegionSelectDisabled) divRegion.classList.add('disabled');
                        else divRegion.classList.remove('disabled');
                    }

                    const divPairProfile = document.getElementById('ctrl-pair-profile');
                    if (divPairProfile) {
                        divPairProfile.classList.remove('disabled');
                    }

                    const divChannel = document.getElementById('ctrl-channel');
                    if (divChannel) {
                        if (isChannelSelectDisabled) divChannel.classList.add('disabled');
                        else divChannel.classList.remove('disabled');
                    }
                })
                .catch(err => console.error('Status fetch failed:', err));
        }

        function setRegion(region) {
            const mReg = document.getElementById('meshtastic-gw-region');
            if (mReg) mReg.value = String(region);
            const mcReg = document.getElementById('meshcore-gw-region');
            if (mcReg) mcReg.value = String(region);

            if (typeof updateMeshtasticFieldsFromPreset === 'function') updateMeshtasticFieldsFromPreset(true);
            if (typeof updateMeshcoreFieldsFromProfile === 'function') updateMeshcoreFieldsFromProfile(true);

            fetch(`/set_region?region=${region}`, { method: 'POST' })
                .then(res => res.json())
                .then(data => {
                    console.log('Region updated:', data);
                    // Clear channel-select options so it repopulates with the new region's base freq!
                    const chSel = document.getElementById('channel-select');
                    if (chSel) chSel.innerHTML = '';
                    updateStats();
                    if (typeof refreshMeshtastic === 'function') refreshMeshtastic();
                    if (typeof refreshMeshcore === 'function') refreshMeshcore();
                })
                .catch(err => console.error('Set region failed:', err));
        }

        function togglePasswordVisibility() {
            const input = document.getElementById('api-passwd-input');
            if (input) {
                input.type = input.type === 'password' ? 'text' : 'password';
            }
        }

        function toggleSniff() {
            fetch('/toggle_sniff', { method: 'POST' })
                .then(res => res.json())
                .then(data => {
                    console.log('Sniff mode updated:', data.sniff_mode);
                    updateStats();
                })
                .catch(err => console.error('Toggle sniff failed:', err));
        }

        function stationHex(id) {
            return '0x' + (id >>> 0).toString(16).toUpperCase().padStart(8, '0');
        }

        async function pairToLocal(id, slot, type, hasPk = false, hasSn = false) {
            const idHex = stationHex(id);
            const targetSlot = (type === 1001) ? 0 : slot;
            const channel = (targetSlot === 0) ? 'base' : `CH${targetSlot - 1}`;
            const rebootNote = (type === 1300)
                ? (hasPk && hasSn
                    ? `\n\n✔ Full Serial Number & Public Key are already cached in RAM from a recent BOOT packet and will be saved directly to Local NVS.`
                    : `\n\nNote: Only weather broadcasts have been seen so far (no Public Key in RAM). After targeting, reboot/reset the weather station so it transmits a BOOT/ADV packet.`)
                : '';
            if (!confirm(`Save station ${idHex} (WS${type}) on ${channel} to Local NVS and target it immediately?\n\nThe gateway will stay locked to this station (no auto-fallback timeout).${rebootNote}`)) {
                return;
            }
            try {
                const resp = await fetch(`/pair_local?id=${idHex}&slot=${targetSlot}&type=${type}`, { method: 'POST' });
                if (!resp.ok) throw new Error(await resp.text());
                updateStats();
            } catch (err) {
                alert(`Could not pair to local NVS: ${err.message || err}`);
            }
        }

        async function clearLocalProfile(localId, hasCloudFallback) {
            const idHex = stationHex(localId);
            const fallbackNote = hasCloudFallback
                ? 'The gateway will automatically switch to your saved Cloud station profile.'
                : 'No Cloud station profile is saved, so the gateway will become unpaired.';
            if (!confirm(`Clear saved Local NVS station ${idHex}?\n\n${fallbackNote}`)) {
                return;
            }
            try {
                const resp = await fetch('/clear_local', { method: 'POST' });
                if (!resp.ok) throw new Error(await resp.text());
                updateStats();
            } catch (err) {
                alert(`Could not clear Local NVS profile: ${err.message || err}`);
            }
        }

        async function clearRecentStations() {
            if (!confirm("Clear the list of discovered stations from RAM?")) return;
            try {
                const resp = await fetch('/clear_recent', { method: 'POST' });
                if (!resp.ok) throw new Error(await resp.text());
                updateStats();
            } catch (err) {
                console.error('Clear recent stations failed:', err);
            }
        }

        async function startTargetedPair(id, slot, type, pairableNow) {
            const idHex = stationHex(id);
            const targetSlot = (type === 1001) ? 0 : slot;
            const channel = targetSlot === 0 ? 'base' : `CH${targetSlot - 1}`;
            const nextStep = pairableNow
                ? 'The station is advertising now, so pairing may complete immediately.'
                : 'After this lock is active, reboot the station so its next BOOT is received on the correct channel.';
            if (!confirm(`Pair ${idHex} on ${channel}?\n\nThe gateway will stay on this channel and accept ADV/BOOT only from this exact station. ${nextStep}\n\nNo kick or reboot command will be sent. A successful result replaces LOCAL; CLOUD is preserved.`)) {
                return;
            }
            try {
                const resp = await fetch(`/set_pair_mode?mode=scan&id=${idHex}&slot=${targetSlot}`, { method: 'POST' });
                if (!resp.ok) throw new Error(await resp.text());
                updateStats();
                if (!pairableNow) {
                    alert(`Target locked: ${idHex} on ${channel}.\n\nReboot the WS1300 now. The gateway will remain on this channel and pair when BOOT arrives.`);
                }
            } catch (err) {
                alert(`Could not start targeted pair: ${err.message || err}`);
            }
        }

        async function startTargetedRepair(id, knownAs, slot) {
            const idHex = stationHex(id);
            if (!confirm(`Repair ${idHex} (${knownAs || 'known station'})?\n\nThe gateway will enter targeted SCAN and send an authenticated reboot only to this station. It succeeds only if this gateway is the station's current owner. The resulting pair replaces LOCAL; CLOUD is preserved.`)) {
                return;
            }
            try {
                let url = `/set_pair_mode?mode=scan&id=${idHex}&type=1300`;
                if (slot !== undefined && slot >= 0) url += `&slot=${slot}`;
                const scanResp = await fetch(url, { method: 'POST' });
                if (!scanResp.ok) throw new Error(await scanResp.text());
                const kickResp = await fetch(`/ws1300_kick_station?id=${idHex}`, { method: 'POST' });
                if (!kickResp.ok) throw new Error(await kickResp.text());
                updateStats();
            } catch (err) {
                alert(`Could not start targeted repair: ${err.message || err}\n\nPassive targeted SCAN may still be active; select OFF, CLOUD, or LOCAL to cancel it.`);
                updateStats();
            }
        }

        async function copyCloudToLocal() {
            if (!confirm('Copy Cloud assignment to Local pairing?\n\nThis configures the local profile to match the cloud assignment and switches the gateway to LOCAL pairing mode.')) {
                return;
            }
            try {
                const resp = await fetch('/copy_cloud_to_local', { method: 'POST' });
                if (!resp.ok) throw new Error(await resp.text());
                updateStats();
            } catch (err) {
                alert(`Could not copy cloud profile: ${err.message || err}`);
            }
        }

        async function listenToRecent(slot, id) {
            const idHex = stationHex(id);
            if (!confirm(`Listen to ${idHex} on its last channel?\n\nThis enters LISTEN ONLY: weather uplink is disabled, while Cloud and Local profiles remain saved. Return to CLOUD or LOCAL when finished.`)) {
                return;
            }
            try {
                const offResp = await fetch('/set_pair_mode?mode=off', { method: 'POST' });
                if (!offResp.ok) throw new Error(await offResp.text());
                if (!g_sniffMode) {
                    const sniffResp = await fetch('/toggle_sniff', { method: 'POST' });
                    if (!sniffResp.ok) throw new Error(await sniffResp.text());
                }
                const listenResp = await fetch(`/set_channel?slot=${slot}`, { method: 'POST' });
                if (!listenResp.ok) throw new Error(await listenResp.text());
                updateStats();
            } catch (err) {
                alert(`Could not enter listen-only mode: ${err.message || err}`);
            }
        }

        function toggleDiscoveryScan() {
            if (window.g_isScanningActive) {
                if (!confirm("Stop the station discovery scan and return to normal mode?")) return;
                const targetMode = (g_activePairSource === 'cloud') ? 'cloud' : (g_hasLocalProfile ? 'local' : 'off');
                setPairState(targetMode);
            } else {
                setPairState('auto');
            }
        }

        function switchActiveTarget(state) {
            return setPairState(state);
        }

        async function setPairState(state) {
            g_isUiLocked = true;

            const btnOff = document.getElementById('pair-btn-off');
            const btnLocal = document.getElementById('pair-btn-local');
            const btnCloud = document.getElementById('pair-btn-cloud');
            const scanBtn = document.getElementById('btn-scan-toggle');
            if (btnOff) btnOff.disabled = true;
            if (btnLocal) btnLocal.disabled = true;
            if (btnCloud) btnCloud.disabled = true;
            if (scanBtn) scanBtn.disabled = true;

            try {
                if (state === 'auto') {
                    if (!confirm("Start scanning for nearby weather stations?")) return;
                }
                if ((state === 'cloud' || state === 'local') &&
                    g_activePairSource !== state) {
                    const profile = g_pairProfiles[state];
                    const idText = profile.present
                        ? '0x' + (profile.id >>> 0).toString(16).toUpperCase().padStart(8, '0')
                        : 'no station ID';
                    const readinessText = profile.ready
                        ? `The radio will stay locked to ${idText} (no timeout).`
                        : `That profile is incomplete (${idText}); the radio cannot target it yet and may keep a saved fallback active.`;
                    const isWs1300 = (profile.type || 1300) === 1300;
                    const needsBoot = profile.ready && isWs1300 && (!profile.hasPk || profile.snSynth);
                    const rebootHint = needsBoot
                        ? `\n\nTo take ownership of ${idText} and capture its full Serial Number & Public Key, reboot/reset the weather station (or click REPAIR if already owned) so it transmits a BOOT/ADV packet.`
                        : (profile.ready && isWs1300
                            ? `\n\nIf this station was previously paired to another gateway, reboot/reset the weather station so it transmits a BOOT/ADV packet and pairs with this gateway.`
                            : '');
                    const proceed = confirm(
                        `Switch to ${state.toUpperCase()}? ${readinessText} ` +
                        `The other profile will remain saved.${rebootHint}`
                    );
                    if (!proceed) return;
                }

                // The firmware applies the whole mode atomically (unpair, flags,
                // persistence, retune) in a single endpoint
                const mode = (state === 'auto') ? 'scan' : state;
                let url = `/set_pair_mode?mode=${mode}`;
                if (mode === 'scan') {
                    const scanType = (window.g_currentScanType !== undefined) ? window.g_currentScanType : (window.g_currentStationType ?? 0);
                    url += `&type=${scanType}`;
                }
                console.log(`Setting pair mode to ${mode.toUpperCase()}...`);
                const resp = await fetch(url, { method: 'POST' });
                if (!resp.ok) {
                    const text = await resp.text();
                    console.error(`Pair mode change rejected: ${text}`);
                    alert(`Could not switch mode: ${text}`);
                }
            } catch (err) {
                console.error(`Failed to set Pair State to ${state}:`, err);
            } finally {
                g_isUiLocked = false;
                if (btnOff) btnOff.disabled = false;
                if (btnLocal) btnLocal.disabled = !g_hasLocalProfile;
                if (btnCloud) btnCloud.disabled = false;
                if (scanBtn) scanBtn.disabled = false;
                const sSel = document.getElementById('scan-profile-select');
                if (sSel) sSel.disabled = false;
                updateStats();
            }
        }

        function setChannel(slot) {
            fetch(`/set_channel?slot=${slot}`, { method: 'POST' })
                .then(res => res.json())
                .then(data => {
                    console.log('Listen channel updated:', data);
                    updateStats();
                })
                .catch(err => console.error('Set channel failed:', err));
        }

        function toggleMqtt() {
            fetch('/toggle_mqtt', { method: 'POST' })
                .then(res => res.json())
                .then(data => {
                    console.log('MQTT state updated:', data.mqtt_disabled ? 'Disabled' : 'Enabled');
                    updateStats();
                })
                .catch(err => console.error('Toggle MQTT failed:', err));
        }

        function setRemoteReboot(armed) {
            // The station only initiates its own transmissions - this arms/
            // disarms a pending reboot, it cannot be pushed immediately. It
            // stays armed (surviving page reloads, since state lives on the
            // gateway) until actually delivered on the station's next
            // check-in, then the toggle syncs back off on its own.
            fetch(`/remote_reboot?enable=${armed ? 1 : 0}`, { method: 'POST' })
                .then(res => res.json())
                .then(data => {
                    console.log('Remote reboot armed state:', data);
                    if (!data.reboot_pending && armed) {
                        // Arm request rejected (not paired to a WS1300) - revert the toggle
                        const t = document.getElementById('remote-reboot-toggle');
                        if (t) t.checked = false;
                    }
                    updateStats();
                })
                .catch(err => console.error('Remote reboot toggle failed:', err));
        }

        function setTxPower(dbm) {
            fetch(`/set_tx_power?dbm=${dbm}`, { method: 'POST' })
                .then(res => res.json())
                .then(data => {
                    console.log('TX power override updated:', data);
                    updateStats();
                })
                .catch(err => console.error('Set TX power failed:', err));
        }

        function setStationPowerMode(mode) {
            let dbm = 0;
            if (mode === 'low') dbm = 2;
            else if (mode === 'high') dbm = 22;
            else dbm = 0; // auto

            setTxPower(dbm);
            updateStationPowerUI(dbm);
        }

        function updateStationPowerUI(dbm) {
            const btnLow = document.getElementById('sta-pwr-btn-low');
            const btnAuto = document.getElementById('sta-pwr-btn-auto');
            const btnHigh = document.getElementById('sta-pwr-btn-high');
            if (!btnLow || !btnAuto || !btnHigh) return;

            btnLow.classList.remove('active');
            btnAuto.classList.remove('active');
            btnHigh.classList.remove('active');

            if (dbm > 0 && dbm <= 2) {
                btnLow.classList.add('active');
            } else if (dbm >= 20) {
                btnHigh.classList.add('active');
            } else {
                btnAuto.classList.add('active');
            }
        }

        function setTxInterval(sec) {
            fetch(`/set_tx_interval?sec=${sec}`, { method: 'POST' })
                .then(res => res.json())
                .then(data => console.log('TX interval override updated:', data))
                .catch(err => console.error('Set TX interval failed:', err));
        }

        function requestGpsFix(ownershipChallenge = false) {
            if (ownershipChallenge && !confirm('Verify that this gateway owns the station?\\n\\nThe station must accept this gateway’s authenticated GPS request and respond before ownership is confirmed.')) {
                return;
            }
            fetch('/request_gps', { method: 'POST' })
                .then(res => res.json())
                .then(data => {
                    alert(ownershipChallenge
                        ? "Ownership challenge armed. Confirmation will occur when the station responds to the GPS request."
                        : "GPS Request Armed!\n\nThe station will be commanded to turn on its GPS receiver and transmit coordinates on its next check-in.");
                    updateStats();
                })
                .catch(err => alert("Failed to issue GPS request: " + err));
        }

        function setPairProfile(type) {
            const parsedType = parseInt(type, 10);
            window.g_currentPairProfile = parsedType;
            fetch(`/set_station?type=${parsedType}`, { method: 'POST' })
                .then(res => {
                    if (!res.ok) throw new Error('Failed to set station profile');
                    return res.json();
                })
                .then(data => {
                    console.log('Pair profile set to:', data.station_type);
                    updateStats();
                })
                .catch(err => console.error('Set pair profile failed:', err));
        }

        function setScanProfile(type) {
            const parsedType = parseInt(type, 10);
            window.g_currentScanType = parsedType;
            window.g_currentStationType = parsedType;
            const scanBtnText = document.getElementById('btn-scan-text');
            const scanIdleCaption = document.getElementById('scan-idle-caption');
            if (!window.g_isScanningActive) {
                const scopeName = (parsedType === 1300) ? 'WS1300' : ((parsedType === 1001) ? 'WS1001' : 'ALL');
                if (scanBtnText) scanBtnText.textContent = `START SCAN (${scopeName})`;
                if (scanIdleCaption) {
                    if (parsedType === 1300) {
                        scanIdleCaption.textContent = 'Sweeps WS1300 Multi-Channel CH0–CH7 in 2 passes (quick + deep). ~9 min total.';
                    } else if (parsedType === 1001) {
                        scanIdleCaption.textContent = 'Scans WS1001 Single Channel Base frequency in 2 passes. ~2.5 min total.';
                    } else {
                        scanIdleCaption.textContent = 'Sweeps WS1001 base + WS1300 CH0–CH7 in 2 passes (quick + deep). ~11 min total.';
                    }
                }
            }
            fetch(`/set_station?scope=${parsedType}`, { method: 'POST' })
                .then(res => res.json())
                .then(data => {
                    console.log('Scan scope set to:', data.scan_scope);
                    updateStats();
                })
                .catch(err => console.error('Set scan profile failed:', err));
        }

        function setStationType(type) {
            setPairProfile(type);
        }

        let packetFilters = {
            weatherxm: true,
            ws1001: true,
            ws1300: true,
            meshtastic: true,
            meshcore: true,
            other: true
        };

        function syncPacketFilterButtons() {
            const map = {
                weatherxm: 'filter-wxm',
                ws1001: 'filter-ws1001',
                ws1300: 'filter-ws1300',
                meshtastic: 'filter-meshtastic',
                meshcore: 'filter-meshcore',
                other: 'filter-other'
            };
            for (const [key, btnId] of Object.entries(map)) {
                const btn = document.getElementById(btnId);
                if (btn) {
                    btn.classList.toggle('active', !!packetFilters[key]);
                }
            }
        }

        function togglePacketFilter(cat) {
            if (packetFilters[cat] !== undefined) {
                packetFilters[cat] = !packetFilters[cat];
                if (cat === 'weatherxm') {
                    packetFilters.ws1001 = packetFilters.weatherxm;
                    packetFilters.ws1300 = packetFilters.weatherxm;
                } else if (cat === 'ws1001' || cat === 'ws1300') {
                    packetFilters.weatherxm = !!(packetFilters.ws1001 || packetFilters.ws1300);
                }
                syncPacketFilterButtons();
                updatePackets();
            }
        }

        function getPacketCategory(pkt) {
            const t = (pkt.packet_type || '').toLowerCase();
            const st = (pkt.station_type || '').toLowerCase();
            if (pkt.category === 'meshcore' || t.includes('meshcore') || st.includes('meshcore')) {
                return 'meshcore';
            }
            if (pkt.category === 'meshtastic' || t.includes('mesh') || st.includes('meshtastic')) {
                return 'meshtastic';
            }
            if (pkt.category === 'weatherxm' || pkt.category === 'other') {
                return pkt.category;
            }
            if (st.includes('ws1') || st.includes('ws2') || st.includes('weatherxm') || t.includes('sensors') || t.includes('boot') || t.includes('adv') || t.includes('gnss')) {
                return 'weatherxm';
            }
            return 'other';
        }

        function updatePackets() {
            fetch('/packet_log')
                .then(res => res.json())
                .then(data => {
                    const tbody = document.getElementById('packet-tbody');
                    if (!tbody) return;
                    if (!data || data.length === 0) {
                        tbody.innerHTML = '<tr><td colspan="9" style="text-align: center; color: var(--text-muted); padding: 2rem;">No packets received or transmitted yet. Waiting...</td></tr>';
                        return;
                    }

                    const filteredData = data.filter(pkt => {
                        const cat = getPacketCategory(pkt);
                        if (cat === 'weatherxm') {
                            const st = (pkt.station_type || '').toUpperCase();
                            if (st.includes('1001')) return !!packetFilters.ws1001;
                            if (st.includes('1300')) return !!packetFilters.ws1300;
                            return !!packetFilters.weatherxm;
                        }
                        return !!packetFilters[cat];
                    });

                    if (filteredData.length === 0) {
                        tbody.innerHTML = '<tr><td colspan="9" style="text-align: center; color: var(--text-muted); padding: 2rem;">No packets matching selected filter(s).</td></tr>';
                        return;
                    }

                    const recentData = filteredData.reverse().slice(0, 15);
                    tbody.innerHTML = recentData.map(pkt => {
                        const date = new Date(pkt.time * 1000);
                        const timeStr = date.toTimeString().split(' ')[0];
                        const cat = getPacketCategory(pkt);
                        
                        let decodedHtml = '';
                        if (pkt.decoded && Object.keys(pkt.decoded).length > 0) {
                            decodedHtml = '<div class="decoded-container">';
                            for (const [key, val] of Object.entries(pkt.decoded)) {
                                if (key === "Station ID") continue;
                                let displayVal = val;
                                let titleAttr = '';
                                if (typeof val === 'string' && val.length > 24) {
                                    displayVal = val.substring(0, 8) + '...' + val.substring(val.length - 8);
                                    titleAttr = `title="${val}"`;
                                }
                                decodedHtml += `<span class="decoded-item" ${titleAttr} style="cursor: help;"><span class="decoded-key">${key}:</span> <span class="decoded-val">${displayVal}</span></span>`;
                            }
                            decodedHtml += '</div>';
                        } else {
                            if (cat === 'other') {
                                decodedHtml = '<span style="color: var(--text-muted); font-style: italic; font-size: 0.8rem;">Undecoded raw LoRa frame</span>';
                            } else {
                                decodedHtml = '<span style="color: var(--text-muted); font-style: italic; font-size: 0.8rem;">Raw packet (no decoded parameters)</span>';
                            }
                        }

                        const rowClass = `class="cat-${cat} ${pkt.filtered ? 'filtered' : ''}"`;
                        let stationType = '';
                        if (pkt.is_tx) {
                            const protoClass = cat === 'weatherxm' ? 'wxm' : cat;
                            stationType = `<div style="display: flex; align-items: center; gap: 4px;"><span class="badge-proto ${protoClass}">${pkt.station_type || 'Gateway'}</span> <span class="badge-direction tx" title="Transmitted by Gateway">TX</span></div>`;
                        } else {
                            if (cat === 'weatherxm') {
                                const stName = pkt.station_type || 'WS1300';
                                stationType = `<span class="badge-proto wxm">${stName}</span>`;
                            } else if (cat === 'meshtastic') {
                                stationType = `<span class="badge-proto meshtastic">Meshtastic</span>`;
                            } else if (cat === 'meshcore') {
                                stationType = `<span class="badge-proto meshcore">MeshCore</span>`;
                            } else {
                                const stName = (pkt.station_type && pkt.station_type !== '-' && pkt.station_type !== 'Unknown') ? pkt.station_type : 'Other';
                                stationType = `<span class="badge-proto other">${stName}</span>`;
                            }
                        }

                        const stationId = pkt.station_id !== null && pkt.station_id !== undefined ? `<span style="color: var(--secondary); font-weight: 600;">${pkt.station_id}</span>` : '<span style="color: var(--text-muted); font-style: italic;">-</span>';
                        const protoVer = pkt.protocol_version !== null && pkt.protocol_version !== undefined ? `<span style="color: var(--accent-green); white-space: nowrap;">${pkt.protocol_version}</span>` : '<span style="color: var(--text-muted); font-style: italic;">-</span>';

                        let slotBadge = '';
                        if (pkt.channel_slot === 0 && !pkt.is_tx) {
                            slotBadge = '<span class="badge-slot base" style="font-size: 0.65rem; padding: 0.05rem 0.3rem;">Base (0)</span>';
                        } else if (pkt.channel_slot > 0 && pkt.channel_slot <= 8) {
                            slotBadge = `<span class="badge-slot channel" style="font-size: 0.65rem; padding: 0.05rem 0.3rem;">Ch ${pkt.channel_slot - 1}</span>`;
                        } else if (pkt.is_tx) {
                            slotBadge = '<span class="badge-slot channel" style="font-size: 0.65rem; padding: 0.05rem 0.3rem; background: rgba(0, 242, 254, 0.15); border-color: rgba(0, 242, 254, 0.3); color: #00f2fe;">TX</span>';
                        }
                        const freqLabel = pkt.freq_str ? `<div style="font-family: monospace; font-size: 0.78rem; font-weight: 700; color: #00d2ff;">${pkt.freq_str}</div>` : '';
                        const specsTitle = pkt.specs_str ? `title="${pkt.specs_str}"` : '';
                        const slotStr = `<div ${specsTitle} style="cursor: help; display: flex; flex-direction: column; gap: 2px;">${freqLabel}<div>${slotBadge}</div></div>`;

                        let typeStr = '';
                        const t = pkt.packet_type ? pkt.packet_type.toLowerCase() : '';
                        if (cat === 'meshcore' || t.includes('meshcore')) {
                            typeStr = `<span class="badge-type meshcore">${pkt.packet_type ? pkt.packet_type.toUpperCase() : 'MESHCORE'}</span>`;
                        } else if (cat === 'meshtastic' || t.includes('mesh') || t.includes('telem') || t.includes('nodeinfo') || t.includes('pos') || t.includes('dev')) {
                            typeStr = `<span class="badge-type meshtastic">${pkt.packet_type ? pkt.packet_type.toUpperCase() : 'MESHTASTIC'}</span>`;
                        } else if (cat === 'other') {
                            const label = (pkt.packet_type && pkt.packet_type !== 'Unknown') ? pkt.packet_type.toUpperCase() : 'UNKNOWN';
                            typeStr = `<span class="badge-type other">${label}</span>`;
                        } else {
                            if (t === 'boot') {
                                typeStr = '<span class="badge-type boot">BOOT</span>';
                            } else if (t === 'adv') {
                                typeStr = '<span class="badge-type adv">ADV</span>';
                            } else if (t === 'sensors') {
                                typeStr = '<span class="badge-type sensors">SENSORS</span>';
                            } else if (t === 'gnss') {
                                typeStr = '<span class="badge-type gnss">GNSS</span>';
                            } else if (t === 'ack') {
                                typeStr = '<span class="badge-type ack">ACK</span>';
                            } else if (t === 'cmd') {
                                typeStr = '<span class="badge-type cmd">CMD</span>';
                            } else if (t === 'time_sync' || t === 'time sync') {
                                typeStr = '<span class="badge-type timesync">TIME_SYNC</span>';
                            } else if (t === 'pair') {
                                typeStr = '<span class="badge-type pair">PAIR</span>';
                            } else if (pkt.packet_type) {
                                typeStr = `<span class="badge-type wxm">${pkt.packet_type.toUpperCase()}</span>`;
                            } else {
                                typeStr = '<span class="badge-type wxm">P2P</span>';
                            }
                        }

                        let signalCell = '';
                        if (pkt.is_tx) {
                            const txPwr = pkt.tx_power_dbm !== undefined && pkt.tx_power_dbm !== null ? pkt.tx_power_dbm : (pkt.rssi || 10);
                            signalCell = `
                                <div style="display: flex; flex-direction: column; gap: 2px; font-size: 0.76rem; line-height: 1.25;">
                                    <div style="white-space: nowrap; font-family: monospace;">
                                        <span class="badge-direction tx">TX OUT</span>
                                    </div>
                                    <div style="white-space: nowrap; font-family: monospace;">
                                        <span style="color: var(--text-muted); font-size: 0.68rem;">Power:</span> <span style="color: #00f5a0; font-weight: 600;">+${txPwr} dBm</span>
                                    </div>
                                    <div style="font-size: 0.68rem; color: #00d2ff; font-family: monospace;">
                                        ${pkt.specs_str || ''}
                                    </div>
                                </div>
                            `;
                        } else {
                            // Signal Quality / Health calculation (0-100%)
                            const rssiNum = Number(pkt.rssi);
                            const snrNum = Number(pkt.snr);
                            // SNR: -7.5 dB (SF7 demod floor) -> +10 dB (clean saturation)
                            const snrScore = Math.max(0, Math.min(100, Math.round(((snrNum - (-7.5)) / (10 - (-7.5))) * 100)));
                            // RSSI: -122 dBm (sensitivity floor) -> -60 dBm (strong bench)
                            const rssiScore = Math.max(0, Math.min(100, Math.round(((rssiNum - (-122)) / (-60 - (-122))) * 100)));
                            let healthScore = Math.round(0.70 * snrScore + 0.30 * rssiScore);
                            if (rssiNum > -30) {
                                healthScore = Math.min(healthScore, 50); // Warning for LNA saturation
                            }
                            let healthColor = '#00f5a0';
                            let healthLabel = 'Excellent';
                            if (healthScore >= 80) {
                                healthColor = '#00f5a0';
                                healthLabel = 'Excellent';
                            } else if (healthScore >= 60) {
                                healthColor = '#00d2ff';
                                healthLabel = 'Good';
                            } else if (healthScore >= 40) {
                                healthColor = '#ffc107';
                                healthLabel = 'Fair';
                            } else if (healthScore >= 20) {
                                healthColor = '#ff9800';
                                healthLabel = 'Weak';
                            } else {
                                healthColor = '#ff4b4b';
                                healthLabel = 'Poor';
                            }
                            const snrSign = snrNum > 0 ? '+' : '';
                            signalCell = `
                                <div style="display: flex; flex-direction: column; gap: 2px; font-size: 0.76rem; line-height: 1.25;">
                                    <div style="white-space: nowrap; font-family: monospace;">
                                        <span style="color: var(--text-muted); font-size: 0.68rem;">RSSI:</span> <span style="color: #00f5a0; font-weight: 600;">${rssiNum} dBm</span>
                                    </div>
                                    <div style="white-space: nowrap; font-family: monospace;">
                                        <span style="color: var(--text-muted); font-size: 0.68rem;">SNR:</span> <span style="color: #00d2ff; font-weight: 600;">${snrSign}${snrNum} dB</span>
                                    </div>
                                    <div style="display: flex; align-items: center; gap: 4px; font-size: 0.68rem; margin-top: 1px;">
                                        <span style="color: ${healthColor}; font-weight: 700;">${healthScore}%</span>
                                        <span style="color: var(--text-muted); font-size: 0.64rem;">(${healthLabel})</span>
                                    </div>
                                </div>
                            `;
                        }

                        return `
                            <tr ${rowClass}>
                                <td>${timeStr}</td>
                                <td>${signalCell}</td>
                                <td>${stationType}</td>
                                <td>${stationId}</td>
                                <td>${slotStr}</td>
                                <td>${typeStr}</td>
                                <td>${protoVer}</td>
                                <td>${decodedHtml}</td>
                                <td class="font-mono" style="font-size: 0.8rem;"><span class="payload-val">${pkt.data}</span></td>
                            </tr>
                        `;
                    }).join('');
                })
                .catch(err => {
                    // Surface the failure in the table itself. This used to log to
                    // the console only, so a malformed /packet_log response looked
                    // identical to "no packets received" - the table just sat there.
                    console.error('Packets fetch failed:', err);
                    const tbody = document.getElementById('packet-tbody');
                    if (tbody) {
                        tbody.innerHTML = `<tr><td colspan="9" style="text-align: center; color: var(--accent-red); padding: 1.5rem;">Could not read the packet log from the gateway (${err.message}). The radio may still be receiving - check the frame counter above.</td></tr>`;
                    }
                });
        }

        // SD Card & Connections Helpers
        let sdRefreshInFlight = null;
        let g_lastSdDiag = null;
        let g_sdFormatTimer = null;

        function setSdFilesystemStatus(loggerData, diagnosticData) {
            const status = document.getElementById('sd-status-val');
            const note = document.getElementById('sd-refresh-note');
            if (!status || !note) return;

            const isEjected = Boolean((loggerData && loggerData.ejected) || (diagnosticData && diagnosticData.ejected));
            const isMounted = !isEjected && Boolean((loggerData && loggerData.mounted) || (!loggerData && diagnosticData && diagnosticData.mounted));
            const isUnusable = !isEjected && Boolean((diagnosticData && diagnosticData.unusable) || (loggerData && loggerData.card_unusable));
            const isPresent = (diagnosticData && diagnosticData.present !== undefined)
                ? Boolean(diagnosticData.present)
                : Boolean(loggerData && loggerData.card_present);
            const fsType = (diagnosticData && diagnosticData.fs_type) || (loggerData && loggerData.card_fs_type) || '';
            const statusReason = (diagnosticData && diagnosticData.status_reason) || (loggerData && loggerData.card_status_reason) || '';

            const ejectBtn = document.getElementById('sd-eject-btn');
            const mountBtn = document.getElementById('sd-mount-btn');
            const testBtn = document.getElementById('sd-test-btn');
            if (ejectBtn) ejectBtn.style.display = isMounted ? 'inline-block' : 'none';
            if (mountBtn) mountBtn.style.display = (!isMounted && (isEjected || isPresent)) ? 'inline-block' : 'none';
            if (testBtn) testBtn.disabled = !isMounted;

            if (isEjected) {
                status.textContent = isPresent ? 'READY TO EJECT (SAFE TO REMOVE)' : 'EJECTED (CARD REMOVED)';
                status.style.color = '#ffb100';
                note.textContent = isPresent
                    ? 'Filesystem unmounted and SD writes paused. Safe to remove the microSD card now, or click "Mount SD Card" to resume logging.'
                    : 'Card safely ejected and removed. Re-insert a microSD card to automatically remount.';
            } else if ((loggerData && loggerData.mounted) ||
                (!loggerData && diagnosticData && diagnosticData.mounted)) {
                status.textContent = 'MOUNTED';
                status.style.color = '#00f5a0';
                note.textContent = 'Card detected and FAT filesystem mounted.';
            } else if (isUnusable) {
                status.textContent = fsType ? `UNUSABLE (${fsType})` : 'CARD UNFORMATTED / UNSUPPORTED';
                status.style.color = '#ff416c';
                note.textContent = statusReason
                    ? `${statusReason} Click "Format SD Card" to format as FAT32.`
                    : 'Card detected, but filesystem is unformatted or unsupported. Click "Format SD Card" to format as FAT32.';
            } else if (isPresent) {
                status.textContent = 'CARD PRESENT / FAT NOT MOUNTED';
                status.style.color = '#ffb100';
                note.textContent = statusReason || 'Hardware is detected, but no writable FAT filesystem is mounted.';
            } else if ((diagnosticData && diagnosticData.present === false) || (loggerData && loggerData.card_present === false && loggerData.mounted === false)) {
                status.textContent = 'NOT DETECTED';
                status.style.color = 'var(--text-muted)';
                note.textContent = 'No microSD card is inserted in the slot.';
            } else if (loggerData && loggerData.mounted === false) {
                status.textContent = 'NOT DETECTED';
                status.style.color = 'var(--text-muted)';
                note.textContent = 'No microSD card is currently mounted.';
            } else {
                status.textContent = 'STATUS UNAVAILABLE';
                status.style.color = 'var(--text-muted)';
                note.textContent = 'The gateway did not return SD status.';
            }
        }

        function updateSdLoggerStatus(diagnosticData = null) {
            if (g_isScreenless !== false) return;
            return fetch('/sd/status')
                .then(res => {
                    if (!res.ok) throw new Error('Could not read logger status');
                    return res.json();
                })
                .then(data => {
                    setSdFilesystemStatus(data, diagnosticData);

                    const health = document.getElementById('sd-logger-health');
                    const counts = document.getElementById('sd-logger-counts');
                    const lastSuccess = document.getElementById('sd-logger-last-success');
                    const error = document.getElementById('sd-logger-error');
                    
                    const isEjected = Boolean(data.ejected || (diagnosticData && diagnosticData.ejected));
                    const isUnusable = !isEjected && Boolean((diagnosticData && diagnosticData.unusable) || data.card_unusable);
                    const isCardAbsent = !isUnusable && (diagnosticData
                        ? (diagnosticData.present === false)
                        : (!data.mounted && !data.card_present && !data.last_success_timestamp));
                    const activeError = !isCardAbsent && Boolean(data.last_error) &&
                        Number(data.last_error_timestamp || 0) >= Number(data.last_success_timestamp || 0);

                    if (health) {
                        if (isEjected) {
                            health.textContent = 'EJECTED (SAFE TO REMOVE)';
                            health.style.color = '#ffb100';
                        } else if (!data.writer_running) {
                            health.textContent = 'STOPPED';
                            health.style.color = 'var(--text-muted)';
                        } else if (isUnusable) {
                            health.textContent = 'FORMAT REQUIRED (LOGGING PAUSED)';
                            health.style.color = '#ff416c';
                        } else if (isCardAbsent) {
                            health.textContent = 'IDLE (NO SD CARD)';
                            health.style.color = 'var(--text-muted)';
                        } else if (activeError) {
                            health.textContent = 'WRITE ERROR';
                            health.style.color = '#ff416c';
                        } else if (!data.mounted && ((diagnosticData && diagnosticData.present) || data.card_present)) {
                            health.textContent = 'WAITING FOR FAT';
                            health.style.color = '#ffb100';
                        } else if (!data.mounted) {
                            health.textContent = 'IDLE';
                            health.style.color = 'var(--text-muted)';
                        } else if (data.last_error) {
                            health.textContent = 'RECOVERED';
                            health.style.color = '#ffb100';
                        } else {
                            health.textContent = 'READY';
                            health.style.color = '#00f5a0';
                        }
                    }

                    if (counts) {

                        if (isEjected) {
                            counts.textContent = 'SD logging paused — card is ejected and safe to remove. Click "Mount SD Card" or re-insert the card to resume.';
                        } else if (isUnusable) {
                            const fsLabel = (diagnosticData && diagnosticData.fs_type) || data.card_fs_type || 'Unformatted';
                            counts.textContent = `SD logging paused — card is ${fsLabel}. Format as FAT32 to enable logging.`;
                        } else if (isCardAbsent) {
                            counts.textContent = 'SD logging paused (no card)';
                        } else {
                            counts.textContent = `Weather: ${data.weather_rows_written}/${data.weather_rows_failed} failed | Metadata: ${data.metadata_rows_written}/${data.metadata_rows_failed} failed | Retries: ${data.retry_count} | Queue: ${data.pending_records}`;
                        }
                    }

                    if (lastSuccess) {
                        if (data.last_success_timestamp) {
                            const when = new Date(data.last_success_timestamp * 1000).toLocaleString();
                            lastSuccess.textContent = `Last successful write: ${when}`;
                        } else if (isUnusable) {
                            lastSuccess.textContent = 'Last successful write: none (card requires FAT32 formatting)';
                        } else if (isCardAbsent) {
                            lastSuccess.textContent = 'Last successful write: none (no card inserted)';
                        } else {
                            lastSuccess.textContent = 'Last successful write: none since boot';
                        }
                    }

                    if (error) {

                        if (isUnusable) {
                            const reason = (diagnosticData && diagnosticData.status_reason) || data.card_status_reason || data.last_error;
                            error.textContent = reason
                                ? `Card unusable: ${reason}`
                                : 'Card cannot be mounted until formatted as FAT32.';
                            error.style.display = 'block';
                        } else if (activeError && data.last_error) {
                            const when = data.last_error_timestamp
                                ? new Date(data.last_error_timestamp * 1000).toLocaleString()
                                : 'unknown time';
                            error.textContent = `Last SD logging error (${when}): ${data.last_error}`;
                            error.style.display = 'block';
                        } else {
                            error.textContent = '';
                            error.style.display = 'none';
                        }
                    }
                    return data;
                })
                .catch(err => {
                    setSdFilesystemStatus(null, diagnosticData);
                    const health = document.getElementById('sd-logger-health');
                    const error = document.getElementById('sd-logger-error');
                    if (health) {
                        health.textContent = 'STATUS UNAVAILABLE';
                        health.style.color = 'var(--text-muted)';
                    }
                    if (error) {
                        error.textContent = '';
                        error.style.display = 'none';
                    }
                    return null;
                });
        }

        function setSdFileMessage(message, color = 'var(--text-muted)') {
            const tbody = document.getElementById('sd-file-tbody');
            if (!tbody) return;
            const row = document.createElement('tr');
            const cell = document.createElement('td');
            cell.colSpan = 3;
            cell.style.cssText = `text-align: center; color: ${color}; padding: 1.2rem;`;
            cell.textContent = message;
            row.appendChild(cell);
            tbody.replaceChildren(row);
        }

        function updateSdFileList(diagnosticData = null) {
            if (diagnosticData && diagnosticData.ejected) {
                setSdFileMessage('SD card is ejected and safe to remove. Click "Mount SD Card" or re-insert the card to access files.', '#ffb100');
                return Promise.resolve();
            }
            if (diagnosticData && diagnosticData.present === false) {
                setSdFileMessage('No microSD card inserted.');
                return Promise.resolve();
            }
            if (diagnosticData && diagnosticData.unusable) {
                const fsLabel = diagnosticData.fs_type ? ` (${diagnosticData.fs_type})` : '';
                setSdFileMessage(`Card is unformatted or unsupported${fsLabel}. Click "Format SD Card" above to initialize as FAT32.`, '#ffb100');
                return Promise.resolve();
            }
            return fetch('/sd/list')
                .then(async res => {
                    if (!res.ok) {
                        let message = `SD file list failed (HTTP ${res.status})`;
                        try {
                            const body = await res.json();
                            if (body.error) message = body.error;
                        } catch (_) {}
                        throw new Error(message);
                    }
                    return res.json();
                })
                .then(files => {
                    const tbody = document.getElementById('sd-file-tbody');
                    if (files.length === 0) {
                        setSdFileMessage('No files found on SD card.');
                        return;
                    }

                    tbody.innerHTML = '';
                    files.forEach(f => {
                        const tr = document.createElement('tr');
                        tr.style.borderBottom = '1px solid rgba(255,255,255,0.05)';

                        const sizeKB = (f.size / 1024).toFixed(2);
                        const nameCell = document.createElement('td');
                        nameCell.style.cssText = 'padding: 0.6rem 0.5rem; font-family: monospace;';
                        nameCell.textContent = f.name;

                        const sizeCell = document.createElement('td');
                        sizeCell.style.cssText = 'padding: 0.6rem 0.5rem;';
                        sizeCell.textContent = sizeKB + ' KB';

                        const actionCell = document.createElement('td');
                        actionCell.style.cssText = 'padding: 0.6rem 0.5rem; text-align: right; display: flex; gap: 0.5rem; justify-content: flex-end; flex-wrap: wrap;';
                        const makeButton = (label, color, background, border, handler) => {
                            const button = document.createElement('button');
                            button.type = 'button';
                            button.textContent = label;
                            button.style.cssText = `background: ${background}; border: 1px solid ${border}; color: ${color}; padding: 0.25rem 0.75rem; border-radius: 6px; font-size: 0.75rem; font-weight: 600; cursor: pointer; transition: 0.2s;`;
                            button.addEventListener('click', handler);
                            return button;
                        };
                        if (String(f.name).toLowerCase().endsWith('.csv')) {
                            actionCell.appendChild(makeButton('View', '#00f5a0', 'rgba(0,245,160,0.12)', 'rgba(0,245,160,0.4)', () => viewSdFile(f.name)));
                        }
                        actionCell.appendChild(makeButton('Download', '#00d2ff', 'rgba(0,210,255,0.15)', 'rgba(0,210,255,0.4)', () => downloadSdFile(f.name)));
                        actionCell.appendChild(makeButton('Delete', '#ff416c', 'rgba(255,65,108,0.15)', 'rgba(255,65,108,0.4)', () => deleteSdFile(f.name)));
                        tr.append(nameCell, sizeCell, actionCell);
                        tbody.appendChild(tr);
                    });
                })
                .catch(err => {
                    setSdFileMessage('No mounted FAT32 microSD card available.', 'var(--text-muted)');
                });
        }

        function updateSdCardInfo() {
            if (g_isScreenless !== false) return Promise.resolve();
            if (sdRefreshInFlight) return sdRefreshInFlight;

            const button = document.getElementById('sd-refresh-btn');
            const status = document.getElementById('sd-status-val');
            const note = document.getElementById('sd-refresh-note');
            if (button) {
                button.disabled = true;
                button.textContent = 'Refreshing...';
            }
            if (status) {
                status.textContent = 'CHECKING';
                status.style.color = 'var(--text-muted)';
            }
            if (note) note.textContent = 'Checking the card, filesystem, logger, diagnostics, and files…';
            setSdFileMessage('Refreshing card contents…');

            // Diagnostics runs first because it verifies the hardware and may mount
            // FAT on demand. Status and file listing must be read afterwards so all
            // three sections describe the same filesystem state.
            sdRefreshInFlight = (async () => {
                const diagnosticData = await runSdDiagnostics();
                await Promise.all([
                    updateSdLoggerStatus(diagnosticData),
                    updateSdFileList(diagnosticData)
                ]);
            })().finally(() => {
                if (button) {
                    button.disabled = false;
                    button.textContent = 'Refresh';
                }
                sdRefreshInFlight = null;
            });
            return sdRefreshInFlight;
        }

        function runSdDiagnostics() {
            if (g_isScreenless !== false) return Promise.resolve(null);
            const diagPanel = document.getElementById('sd-diag-panel');
            const diagContent = document.getElementById('sd-diag-content');
            const diagParts = document.getElementById('sd-diag-partitions');
            
            if (!diagPanel || !diagContent || !diagParts) return Promise.resolve(null);
            
            diagPanel.style.display = 'none';
            diagContent.innerHTML = '<div style="grid-column: 1/-1; text-align: center; padding: 1rem;"><span style="color: var(--text-muted);">Running deep SD Card diagnostics...</span></div>';
            diagParts.innerHTML = '';

            return fetch('/sd/diagnose')
                .then(async res => {
                    if (!res.ok) {
                        const detail = (await res.text()).trim();
                        throw new Error(detail || `HTTP ${res.status}`);
                    }
                    return res.json();
                })
                .then(data => {
                    g_lastSdDiag = data;
                    if (data.present === false || data.ejected) {
                        diagPanel.style.display = 'none';
                        diagContent.innerHTML = '';
                        diagParts.innerHTML = '';
                        return data;
                    }

                    diagPanel.style.display = 'block';
                    const isMounted = data.mounted
                        ? '<span style="color: #00f5a0; font-weight: 700;">Mounted (FAT)</span>'
                        : (data.unusable
                            ? `<span style="color: #ff416c; font-weight: 700;">Unusable (${data.fs_type || 'Requires Format'})</span>`
                            : '<span style="color: #ffb100; font-weight: 700;">Unmounted (Raw Card Mode)</span>');
                    let vfsHtml = '';
                    if (data.vfs_total_gb !== undefined) {
                        vfsHtml = `
                            <div><strong style="color: var(--text-muted);">FS Total Space:</strong> <span style="color: #fff;">${data.vfs_total_gb.toFixed(2)} GB</span></div>
                            <div><strong style="color: var(--text-muted);">FS Used Space:</strong> <span style="color: #ff416c; font-weight: 600;">${data.vfs_used_gb.toFixed(2)} GB</span></div>
                            <div><strong style="color: var(--text-muted);">FS Free Space:</strong> <span style="color: #00f5a0; font-weight: 600;">${data.vfs_free_gb.toFixed(2)} GB</span></div>
                        `;
                    }
                    diagContent.innerHTML = `
                        <div><strong style="color: var(--text-muted);">Card Name:</strong> <span style="font-family: monospace; color: #fff;">${data.name || 'Unknown'}</span></div>
                        <div><strong style="color: var(--text-muted);">Capacity:</strong> <span style="color: #fff;">${(data.capacity_gb || 0).toFixed(2)} GB</span></div>
                        <div><strong style="color: var(--text-muted);">Max Frequency:</strong> <span style="color: #fff;">${data.speed_mhz || 0} MHz</span></div>
                        <div><strong style="color: var(--text-muted);">VFS Status:</strong> ${isMounted}</div>
                        ${vfsHtml}
                    `;

                    const actionNoticeHtml = data.unusable ? `
                        <div style="margin-top: 0.75rem; background: rgba(255, 65, 108, 0.1); border: 1px solid rgba(255, 65, 108, 0.35); padding: 0.8rem; border-radius: 8px; color: #ffb100; font-size: 0.8rem; line-height: 1.4;">
                            <strong style="color: #ff416c;">ACTION REQUIRED:</strong> ${data.status_reason || 'This microSD card does not have a valid FAT16/FAT32 filesystem.'}<br>
                            Please click <strong>"Format SD Card"</strong> above to initialize this ${(data.capacity_gb || 0).toFixed(0)}GB card as FAT32 so WeatherLogger can record telemetry.
                        </div>` : '';

                    if (data.mbr_error) {
                        diagParts.innerHTML = `
                            <div style="color: #ff416c; font-size: 0.8rem; margin-top: 0.5rem;">
                                <strong>MBR Probe Warning:</strong> ${data.mbr_error}
                            </div>${actionNoticeHtml}`;
                    } else if (data.partitions && data.partitions.length > 0) {
                        let html = `<div style="font-weight: 600; margin-bottom: 0.5rem; color: #fff;">Partition Layout (${data.partition_count} partitions found, MBR Sig: ${data.mbr_signature}):</div>`;
                        html += '<div style="display: flex; flex-direction: column; gap: 0.4rem;">';
                        data.partitions.forEach(p => {
                            let typeStyle = 'color: #ffb100; font-weight: bold;';
                            if (p.type === '0x83' || p.type === '0x07' || p.type === '0xEE') {
                                typeStyle = 'color: #ff416c; font-weight: bold; background: rgba(255, 65, 108, 0.1); padding: 0.1rem 0.3rem; border-radius: 4px;';
                            } else if (p.type === '0x0C' || p.type === '0x0B' || p.type === '0x06' || p.type === '0x04' || p.type === '0x0E') {
                                typeStyle = 'color: #00f5a0; font-weight: bold;';
                            }
                            html += `
                                <div style="display: flex; justify-content: space-between; align-items: center; background: rgba(255,255,255,0.02); padding: 0.4rem 0.6rem; border-radius: 6px; border: 1px solid rgba(255,255,255,0.03);">
                                    <span>Partition #${p.id} (sectors: ${p.sectors}):</span>
                                    <span>Size: <strong style="color: #fff;">${(p.size_gb || 0).toFixed(2)} GB</strong> | Type: <span style="${typeStyle}">${p.desc} (${p.type})</span></span>
                                </div>`;
                        });
                        html += '</div>';
                        html += actionNoticeHtml;
                        diagParts.innerHTML = html;
                    } else {
                        diagParts.innerHTML = `
                            <div style="color: var(--text-muted); font-size: 0.8rem; margin-top: 0.5rem; background: rgba(255,255,255,0.01); padding: 0.5rem; border-radius: 6px; text-align: center;">
                                No MBR partition table found (Sig: ${data.mbr_signature || '0x0000'}) or card is blank/unformatted.
                            </div>${actionNoticeHtml}`;
                    }
                    return data;
                })
                .catch(err => {
                    diagPanel.style.display = 'none';
                    return null;
                });
        }

        function downloadSdFile(name) {
            window.open(`/sd/download?file=${encodeURIComponent(name)}`, '_blank', 'noopener');
        }

        function viewSdFile(name) {
            window.open(`/sd/view?file=${encodeURIComponent(name)}`, '_blank', 'noopener');
        }

        function deleteSdFile(name) {
            if (!confirm(`Are you sure you want to delete ${name}? This cannot be undone.`)) {
                return;
            }
            fetch(`/sd/delete?file=${encodeURIComponent(name)}`, { method: 'POST' })
                .then(res => {
                    if (!res.ok) throw new Error("Delete failed");
                    return res.json();
                })
                .then(data => {
                    updateSdCardInfo();
                })
                .catch(err => {
                    alert(`Failed to delete ${name}: ${err.message}`);
                });
        }

        function showSdFormatModal(estSec, capGb) {
            const modal = document.getElementById('sd-format-modal');
            if (!modal) return;
            const spinner = document.getElementById('sd-format-spinner');
            if (spinner) spinner.style.display = 'block';
            const title = document.getElementById('sd-format-modal-title');
            if (title) title.textContent = (capGb && capGb > 0) ? `Formatting ${(capGb).toFixed(0)}GB SD Card` : 'Formatting SD Card';
            
            const totalDurationSec = estSec || 150;
            const estTimeStr = formatEtaCountdown(totalDurationSec);
            const msg = document.getElementById('sd-format-modal-msg');
            if (msg) {
                msg.innerHTML = `Formatting in progress... Estimated time: <strong>~${estTimeStr}</strong>.<br>` +
                                `Please do not turn off or unplug the device.`;
            }

            const progressContainer = document.getElementById('sd-format-progress-container');
            const progressBar = document.getElementById('sd-format-progress-bar');
            const etaEl = document.getElementById('sd-format-eta');
            if (progressContainer) progressContainer.style.display = 'block';
            if (progressBar) {
                progressBar.style.width = '3%';
                progressBar.style.background = 'linear-gradient(90deg, #ff416c 0%, #ff4b2b 25%, #00f5a0 50%, #ff416c 75%, #ff4b2b 100%)';
                progressBar.style.backgroundSize = '200% 100%';
                progressBar.style.animation = 'progress-shimmer 2s linear infinite';
            }
            if (etaEl) {
                etaEl.style.display = 'block';
                etaEl.style.color = '#00f5a0';
                etaEl.textContent = `ETA ~${estTimeStr} remaining (0%)`;
            }

            const statusEl = document.getElementById('sd-format-modal-status');
            if (statusEl) statusEl.style.display = 'none';

            if (g_sdFormatTimer) clearInterval(g_sdFormatTimer);
            modal.style.display = 'flex';
        }

        function updateSdFormatModal(success, msg) {
            if (g_sdFormatTimer) {
                clearInterval(g_sdFormatTimer);
                g_sdFormatTimer = null;
            }
            const spinner = document.getElementById('sd-format-spinner');
            if (spinner) spinner.style.display = 'none';
            const title = document.getElementById('sd-format-modal-title');
            if (title) title.textContent = success ? 'Format Complete' : 'Format Failed';
            const msgEl = document.getElementById('sd-format-modal-msg');
            if (msgEl) msgEl.textContent = msg;

            const progressBar = document.getElementById('sd-format-progress-bar');
            const etaEl = document.getElementById('sd-format-eta');
            if (progressBar) {
                progressBar.style.animation = 'none';
                if (success) {
                    progressBar.style.width = '100%';
                    progressBar.style.background = '#00f5a0';
                } else {
                    progressBar.style.background = '#ff416c';
                }
            }
            if (etaEl) {
                etaEl.textContent = success ? 'Done (100%)' : 'Format Aborted';
                etaEl.style.color = success ? '#00f5a0' : '#ff416c';
            }

            const statusEl = document.getElementById('sd-format-modal-status');
            if (statusEl) {
                statusEl.style.display = 'block';
                statusEl.style.background = success ? 'rgba(74, 222, 128, 0.15)' : 'rgba(255, 65, 108, 0.15)';
                statusEl.style.color = success ? '#4ade80' : '#ff416c';
                statusEl.style.border = success ? '1px solid rgba(74, 222, 128, 0.3)' : '1px solid rgba(255, 65, 108, 0.3)';
                statusEl.textContent = success ? 'Success' : 'Error';
            }
        }

        function hideSdFormatModal() {
            if (g_sdFormatTimer) {
                clearInterval(g_sdFormatTimer);
                g_sdFormatTimer = null;
            }
            const modal = document.getElementById('sd-format-modal');
            if (modal) modal.style.display = 'none';
            const progressContainer = document.getElementById('sd-format-progress-container');
            const etaEl = document.getElementById('sd-format-eta');
            const progressBar = document.getElementById('sd-format-progress-bar');
            if (progressContainer) progressContainer.style.display = 'none';
            if (etaEl) etaEl.style.display = 'none';
            if (progressBar) {
                progressBar.style.width = '0%';
                progressBar.style.animation = 'progress-shimmer 2s linear infinite';
                progressBar.style.background = 'linear-gradient(90deg, #ff416c 0%, #ff4b2b 25%, #00f5a0 50%, #ff416c 75%, #ff4b2b 100%)';
            }
        }

        function formatSdCard() {
            const capGb = (g_lastSdDiag && g_lastSdDiag.capacity_gb) ? g_lastSdDiag.capacity_gb : (window.g_sdCapacityGb || 0);
            // Fast DMA FAT32 format time: ~10-20s for 32-64GB cards
            const estSec = capGb > 0 ? Math.max(10, Math.round(6 + capGb * 0.22)) : 15;
            const estTimeStr = formatEtaCountdown(estSec);
            const sizeNote = capGb > 0 ? ` (${(capGb).toFixed(0)}GB card)` : '';

            if (!confirm(`CRITICAL WARNING: Are you sure you want to format the SD Card${sizeNote}?\n\nThis will permanently erase ALL files on the card!\n\nEstimated time: ~${estTimeStr}.\nPlease do not turn off or unplug the device.`)) {
                return;
            }
            showSdFormatModal(estSec, capGb);
            const startTime = Date.now();
            fetch('/sd/format', { method: 'POST' })
                .then(res => {
                    if (!res.ok) throw new Error("Format failed");
                    return res.json();
                })
                .then(data => {
                    if (data && data.async) {
                        const progressBar = document.getElementById('sd-format-progress-bar');
                        const etaEl = document.getElementById('sd-format-eta');
                        let pollInFlight = false;
                        if (g_sdFormatTimer) clearInterval(g_sdFormatTimer);
                        g_sdFormatTimer = setInterval(() => {
                            if (pollInFlight) return;
                            pollInFlight = true;
                            fetch('/sd/status')
                                .then(r => r.ok ? r.json() : null)
                                .then(st => {
                                    pollInFlight = false;
                                    if (!st) return;
                                    const pct = Math.max(3, Math.min(99, st.format_progress || 3));
                                    const elapsed = ((Date.now() - startTime) / 1000).toFixed(0);
                                    if (st.formatting) {
                                        if (progressBar) progressBar.style.width = `${pct}%`;
                                        if (etaEl) etaEl.textContent = `Formatting FAT32... ${pct}% (${elapsed}s elapsed)`;
                                        return;
                                    }
                                    clearInterval(g_sdFormatTimer);
                                    g_sdFormatTimer = null;
                                    if (st.mounted || (st.format_progress === 100 && !st.card_unusable)) {
                                        updateSdFormatModal(true, `SD Card formatted and mounted in ${elapsed}s!`);
                                        setTimeout(() => {
                                            hideSdFormatModal();
                                            updateSdCardInfo();
                                        }, 2000);
                                    } else {
                                        updateSdFormatModal(false, st.last_error || st.card_status_reason || "Formatting failed");
                                        setTimeout(() => {
                                            hideSdFormatModal();
                                            updateSdCardInfo();
                                        }, 4000);
                                    }
                                })
                                .catch(() => { pollInFlight = false; });
                        }, 400);
                        return;
                    }
                    updateSdFormatModal(true, "SD Card formatted successfully!");
                    setTimeout(() => {
                        hideSdFormatModal();
                        updateSdCardInfo();
                    }, 2500);
                })
                .catch(err => {
                    updateSdFormatModal(false, `Formatting failed: ${err.message}`);
                    setTimeout(() => {
                        hideSdFormatModal();
                        updateSdCardInfo();
                    }, 4000);
                });
        }

        function testSdCard() {
            const btn = document.getElementById('sd-test-btn');
            if (btn) {
                btn.disabled = true;
                btn.textContent = "Testing...";
            }
            fetch('/sd/test')
                .then(res => {
                    if (!res.ok) throw new Error("Test request failed");
                    return res.json();
                })
                .then(data => {
                    if (data.status === "ok") {
                        alert(`SD Card Test Success: ${data.message}`);
                    } else {
                        alert(`SD Card Test Failed: ${data.message}`);
                    }
                })
                .catch(err => {
                    alert(`SD Card Test Error: ${err.message}`);
                })
                .finally(() => {
                    if (btn) {
                        btn.disabled = false;
                        btn.textContent = "Test SD Card (W/R)";
                    }
                    updateSdCardInfo();
                });
        }

        function ejectSdCard() {
            const ejectBtn = document.getElementById('sd-eject-btn');
            const status = document.getElementById('sd-status-val');
            const note = document.getElementById('sd-refresh-note');
            if (ejectBtn) {
                ejectBtn.disabled = true;
                ejectBtn.textContent = 'Ejecting...';
            }
            if (status) {
                status.textContent = 'EJECTING...';
                status.style.color = '#ffb100';
            }
            if (note) {
                note.textContent = 'Flushing and unmounting FAT32 filesystem...';
            }
            fetch('/sd/eject', { method: 'POST' })
                .then(async res => {
                    if (!res.ok) {
                        const txt = await res.text();
                        throw new Error(txt || `HTTP ${res.status}`);
                    }
                    return res.json();
                })
                .then(() => {
                    updateStats();
                    return updateSdCardInfo();
                })
                .catch(err => {
                    alert('Failed to eject SD card: ' + err.message);
                })
                .finally(() => {
                    if (ejectBtn) {
                        ejectBtn.disabled = false;
                        ejectBtn.textContent = 'Eject SD Card';
                    }
                });
        }

        function mountSdCard() {
            const mountBtn = document.getElementById('sd-mount-btn');
            const status = document.getElementById('sd-status-val');
            const note = document.getElementById('sd-refresh-note');
            if (mountBtn) {
                mountBtn.disabled = true;
                mountBtn.textContent = 'Mounting...';
            }
            if (status) {
                status.textContent = 'MOUNTING...';
                status.style.color = '#00d2ff';
            }
            if (note) {
                note.textContent = 'Probing SPI bus and mounting FAT32 filesystem...';
            }
            fetch('/sd/mount', { method: 'POST' })
                .then(async res => {
                    if (!res.ok) {
                        const txt = await res.text();
                        throw new Error(txt || `HTTP ${res.status}`);
                    }
                    return res.json();
                })
                .then(data => {
                    if (data && data.mounted === false) {
                        alert(data.message || 'Could not mount SD card. Make sure a FAT32 microSD card is inserted.');
                    }
                    updateStats();
                    return updateSdCardInfo();
                })
                .catch(err => {
                    alert('Failed to mount SD card: ' + err.message);
                })
                .finally(() => {
                    if (mountBtn) {
                        mountBtn.disabled = false;
                        mountBtn.textContent = 'Mount SD Card';
                    }
                });
        }

        function toggleWifiRadio(enable) {
            if (!enable && !confirm("Warning: Disabling Wi-Fi will disconnect you from this Web Console dashboard!\n\nYou will need to reboot or connect via serial/physical button to re-enable Wi-Fi. Do you want to proceed?")) {
                document.getElementById('wifi-toggle-btn').checked = true;
                return;
            }
            fetch(`/wifi_toggle?enable=${enable}`, { method: 'POST' })
                .then(res => res.json())
                .then(data => {
                    updateStats();
                })
                .catch(err => {
                    alert(`Failed to update Wi-Fi state: ${err.message}`);
                });
        }

        function toggleMqttBtn(enable) {
            if (g_edition !== 'open' && !enable) {
                alert("Disabling WeatherXM Cloud upload is only possible in OPEN edition.\n\nIn WXM edition, cloud publishing is permanently enabled to ensure data continuity.");
                const btn = document.getElementById('mqtt-toggle-btn');
                if (btn) btn.checked = true;
                return;
            }
            const val = enable ? '1' : '0';
            fetch(`/toggle_mqtt?enable=${val}`, { method: 'POST' })
                .then(res => {
                    if (!res.ok) {
                        return res.json().then(j => { throw new Error(j.error || `HTTP ${res.status}`); });
                    }
                    return res.json();
                })
                .then(data => {
                    updateStats();
                })
                .catch(err => {
                    alert(`Failed to update WeatherXM Cloud state: ${err.message}`);
                    updateStats();
                });
        }

        async function toggleWxmP2P(enable) {
            const p2pToggle = document.getElementById('wxm-p2p-toggle-btn');
            if (!enable) {
                const confirmed = confirm("Warning: Disabling WeatherXM LoRa P2P will stop the gateway from receiving and submitting weather station telemetry.\n\nThrough this protocol, the station is also configured and managed from the gateway (downlink settings, sensor intervals, reboot, and GPS requests). Are you sure you want to disable it?");
                if (!confirmed) {
                    if (p2pToggle) p2pToggle.checked = true;
                    return;
                }
                await setPairState('off');
            } else {
                const targetState = (g_activePairSource && g_activePairSource !== 'off') 
                    ? g_activePairSource 
                    : (g_hasLocalProfile ? 'local' : (g_pairProfiles.cloud.ready ? 'cloud' : 'auto'));
                const mode = (targetState === 'auto' || targetState === 'scan') ? 'scan' : targetState;
                try {
                    await fetch(`/set_pair_mode?mode=${mode}`, { method: 'POST' });
                    updateStats();
                } catch (err) {
                    console.error('Failed to enable WeatherXM LoRa P2P:', err);
                }
            }
        }

        //
        // Meshtastic environment telemetry beacon.
        // State lives in device NVS and is read back from
        // /api/v1/meshtastic/config - never cached in the browser.
        //
		function updateMeshtasticSchedule(intervalSec) {
			const note = document.getElementById('mesh-schedule-note');
			const sec = Number(intervalSec);
			if (!note || !Number.isFinite(sec) || sec <= 0) return;
			const minutes = Math.round(sec / 60);
			note.textContent = `Schedule: Environment ${minutes} min (1×I) · NodeInfo ${minutes * 2} min (2×I) · Device ${minutes * 3} min (3×I) · Position ${minutes * 6} min (6×I). One packet per safe radio window; channel activity detection defers when busy.`;
		}

        function updateMeshIntervalFloodWarning(elementId, intervalSec, isOff) {
            const el = document.getElementById(elementId);
            if (!el) return;
            const sec = Number(intervalSec);
            if (!isOff && Number.isFinite(sec) && sec > 0 && sec < 3600) {
                el.style.display = 'block';
            } else {
                el.style.display = 'none';
            }
        }

        function onStationMeshtasticIntervalChange(val) {
            const min = Number(val);
            updateMeshIntervalFloodWarning('sta-mesh-interval-flood-warning', min * 60, min === 0);
            if (document.getElementById('sta-mesh-send-checkbox') && document.getElementById('sta-mesh-send-checkbox').checked) {
                toggleStationMeshtasticQueue(true);
            }
        }

        function onStationMeshcoreIntervalChange(val) {
            const min = Number(val);
            updateMeshIntervalFloodWarning('sta-meshcore-interval-flood-warning', min * 60, min === 0);
            if (document.getElementById('sta-meshcore-send-checkbox') && document.getElementById('sta-meshcore-send-checkbox').checked) {
                toggleStationMeshcoreQueue(true);
            }
        }

        const MESHTASTIC_PRESET_PARAMS = {
            0: { bw: 250, sf: 11, cr: 5 }, // LongFast
            1: { bw: 125, sf: 12, cr: 8 }, // LongSlow
            2: { bw: 250, sf: 9,  cr: 5 }, // MediumFast
            3: { bw: 250, sf: 10, cr: 5 }, // MediumSlow
            4: { bw: 250, sf: 7,  cr: 5 }, // ShortFast
            5: { bw: 250, sf: 8,  cr: 5 }, // ShortSlow
            6: { bw: 125, sf: 11, cr: 8 }, // LongModerate
            7: { bw: 500, sf: 7,  cr: 5 }  // ShortTurbo
        };

        const MESHCORE_PROFILE_PARAMS = {
            4: { bw: 62,  sf: 8,  cr: 8 }, // EU/UK Narrow
            0: { bw: 250, sf: 10, cr: 5 }, // Long Range
            1: { bw: 250, sf: 8,  cr: 5 }, // Medium
            2: { bw: 125, sf: 11, cr: 5 }, // Narrow Extreme
            3: { bw: 250, sf: 7,  cr: 5 }  // Fast
        };

        const MESH_REGION_FREQS = {
            868: 869525000,
            915: 906875000,
            923: 923875000,
            916: 919875000,
            865: 865875000
        };

        const MESHCORE_REGION_FREQS = {
            868: 869618000,
            915: 915000000,
            923: 923000000,
            916: 915000000,
            865: 865000000
        };

        function updateMeshtasticFieldsFromPreset(force) {
            const customToggle = document.getElementById('meshtastic-custom-toggle');
            if (customToggle && customToggle.checked && !force) return;

            const presetSel = document.getElementById('meshtastic-preset');
            const regionSel = document.getElementById('meshtastic-gw-region');
            const presetVal = parseInt(presetSel ? presetSel.value : '0', 10);
            const regionVal = parseInt(regionSel ? regionSel.value : '868', 10);

            const params = MESHTASTIC_PRESET_PARAMS[presetVal] || MESHTASTIC_PRESET_PARAMS[0];
            const freq = MESH_REGION_FREQS[regionVal] || 869525000;

            const fEl = document.getElementById('mesh-custom-freq');
            const bwEl = document.getElementById('mesh-custom-bw');
            const sfEl = document.getElementById('mesh-custom-sf');
            const crEl = document.getElementById('mesh-custom-cr');
            const pwrEl = document.getElementById('mesh-custom-tx-pwr');

            if (fEl) fEl.value = freq;
            if (bwEl) bwEl.value = params.bw;
            if (sfEl) sfEl.value = params.sf;
            if (crEl) crEl.value = params.cr;
            if (pwrEl) pwrEl.value = '14';
        }

        function toggleMeshtasticCustomMode(isCustom) {
            const customToggle = document.getElementById('meshtastic-custom-toggle');
            if (customToggle) customToggle.checked = isCustom;

            const fEl = document.getElementById('mesh-custom-freq');
            const bwEl = document.getElementById('mesh-custom-bw');
            const sfEl = document.getElementById('mesh-custom-sf');
            const crEl = document.getElementById('mesh-custom-cr');
            const pwrEl = document.getElementById('mesh-custom-tx-pwr');
            const saveBtn = document.getElementById('mesh-save-rf-btn');

            if (fEl) fEl.disabled = !isCustom;
            if (bwEl) bwEl.disabled = !isCustom;
            if (sfEl) sfEl.disabled = !isCustom;
            if (crEl) crEl.disabled = !isCustom;
            if (pwrEl) pwrEl.disabled = !isCustom;
            if (saveBtn) saveBtn.disabled = !isCustom;

            if (!isCustom) {
                updateMeshtasticFieldsFromPreset(true);
                const presetSel = document.getElementById('meshtastic-preset');
                const presetVal = presetSel ? presetSel.value : '0';
                setMeshtasticPreset(presetVal);
            }
        }

        function onMeshtasticPresetChange(val) {
            updateMeshtasticFieldsFromPreset(true);
            const customToggle = document.getElementById('meshtastic-custom-toggle');
            if (customToggle && !customToggle.checked) {
                setMeshtasticPreset(val);
            }
        }

        function updateMeshcoreFieldsFromProfile(force) {
            const customToggle = document.getElementById('meshcore-custom-toggle');
            if (customToggle && customToggle.checked && !force) return;

            const profileSel = document.getElementById('meshcore-gw-profile');
            const regionSel = document.getElementById('meshcore-gw-region');
            const profileVal = parseInt(profileSel ? profileSel.value : '4', 10);
            const regionVal = parseInt(regionSel ? regionSel.value : '868', 10);

            const params = MESHCORE_PROFILE_PARAMS[profileVal] || MESHCORE_PROFILE_PARAMS[4];
            const freq = MESHCORE_REGION_FREQS[regionVal] || 869618000;

            const fEl = document.getElementById('meshcore-gw-custom-freq');
            const bwEl = document.getElementById('meshcore-gw-custom-bw');
            const sfEl = document.getElementById('meshcore-gw-custom-sf');
            const crEl = document.getElementById('meshcore-gw-custom-cr');
            const pwrEl = document.getElementById('meshcore-gw-custom-tx-pwr');

            if (fEl) fEl.value = freq;
            if (bwEl) bwEl.value = params.bw;
            if (sfEl) sfEl.value = params.sf;
            if (crEl) crEl.value = params.cr;
            if (pwrEl) pwrEl.value = '14';
        }

        function toggleMeshcoreCustomMode(isCustom) {
            const customToggle = document.getElementById('meshcore-custom-toggle');
            if (customToggle) customToggle.checked = isCustom;

            const fEl = document.getElementById('meshcore-gw-custom-freq');
            const bwEl = document.getElementById('meshcore-gw-custom-bw');
            const sfEl = document.getElementById('meshcore-gw-custom-sf');
            const crEl = document.getElementById('meshcore-gw-custom-cr');
            const pwrEl = document.getElementById('meshcore-gw-custom-tx-pwr');
            const saveBtn = document.getElementById('meshcore-save-rf-btn');

            if (fEl) fEl.disabled = !isCustom;
            if (bwEl) bwEl.disabled = !isCustom;
            if (sfEl) sfEl.disabled = !isCustom;
            if (crEl) crEl.disabled = !isCustom;
            if (pwrEl) pwrEl.disabled = !isCustom;
            if (saveBtn) saveBtn.disabled = !isCustom;

            if (!isCustom) {
                updateMeshcoreFieldsFromProfile(true);
                const profileSel = document.getElementById('meshcore-gw-profile');
                const profileVal = profileSel ? profileSel.value : '4';
                setMeshcoreGwProfile(profileVal);
            }
        }

        function onMeshcoreProfileChange(val) {
            updateMeshcoreFieldsFromProfile(true);
            const customToggle = document.getElementById('meshcore-custom-toggle');
            if (customToggle && !customToggle.checked) {
                setMeshcoreGwProfile(val);
            }
        }

        function applyMeshtasticConfig(cfg) {
            const toggle = document.getElementById('meshtastic-toggle-btn');
            const detail = document.getElementById('meshtastic-detail');
            if (!toggle || !detail) return;

            toggle.checked = !!cfg.enabled;
            detail.style.display = cfg.enabled ? 'block' : 'none';
            updateAdaptiveWeatherPanelState();
            updateMeshtasticPanelState();

            const intervalSel = document.getElementById('meshtastic-interval');
            if (intervalSel && document.activeElement !== intervalSel) {
                intervalSel.value = String(cfg.interval_sec);
            }
			updateMeshtasticSchedule(cfg.interval_sec);
            updateMeshIntervalFloodWarning('mesh-interval-flood-warning', cfg.interval_sec);

            const gwRegSel = document.getElementById('meshtastic-gw-region');
            if (gwRegSel && cfg.freq_region && document.activeElement !== gwRegSel) {
                gwRegSel.value = String(cfg.freq_region);
            }

            const hopSel = document.getElementById('meshtastic-hop-limit');
            if (hopSel && document.activeElement !== hopSel) {
                hopSel.value = String(cfg.hop_limit !== undefined ? cfg.hop_limit : 3);
            }

            const pwrBtn = document.getElementById('meshtastic-pwr-toggle-btn');
            if (pwrBtn) {
                pwrBtn.checked = !!cfg.tx_power_high;
            }

            const isCustom = (cfg.preset === 255);
            const customToggle = document.getElementById('meshtastic-custom-toggle');
            if (customToggle && document.activeElement !== customToggle) {
                customToggle.checked = isCustom;
            }

            const presetSel = document.getElementById('meshtastic-preset');
            if (presetSel && document.activeElement !== presetSel && cfg.preset !== undefined && !isCustom) {
                presetSel.value = String(cfg.preset);
            }

            const fEl = document.getElementById('mesh-custom-freq');
            const bwEl = document.getElementById('mesh-custom-bw');
            const sfEl = document.getElementById('mesh-custom-sf');
            const crEl = document.getElementById('mesh-custom-cr');
            const pwrEl = document.getElementById('mesh-custom-tx-pwr');
            const saveBtn = document.getElementById('mesh-save-rf-btn');

            if (fEl) fEl.disabled = !isCustom;
            if (bwEl) bwEl.disabled = !isCustom;
            if (sfEl) sfEl.disabled = !isCustom;
            if (crEl) crEl.disabled = !isCustom;
            if (pwrEl) pwrEl.disabled = !isCustom;
            if (saveBtn) saveBtn.disabled = !isCustom;

            if (isCustom) {
                if (fEl && document.activeElement !== fEl && cfg.custom_freq_hz !== undefined) fEl.value = cfg.custom_freq_hz || '';
                if (bwEl && document.activeElement !== bwEl && cfg.custom_bw_khz !== undefined) bwEl.value = String(cfg.custom_bw_khz);
                if (sfEl && document.activeElement !== sfEl && cfg.custom_sf !== undefined) sfEl.value = String(cfg.custom_sf);
                if (crEl && document.activeElement !== crEl && cfg.custom_cr !== undefined) crEl.value = String(cfg.custom_cr);
                if (pwrEl && document.activeElement !== pwrEl && cfg.custom_tx_power_dbm !== undefined) pwrEl.value = cfg.custom_tx_power_dbm || '';
            } else {
                updateMeshtasticFieldsFromPreset(false);
            }

            const customNameChk = document.getElementById('mesh-custom-name-chk');
            const customLongInput = document.getElementById('mesh-custom-long-name');
            const customShortInput = document.getElementById('mesh-custom-short-name');
            const saveNameBtn = document.getElementById('mesh-save-name-btn');

            const isCustomName = !!cfg.custom_name_enabled;
            if (customNameChk && document.activeElement !== customNameChk) {
                customNameChk.checked = isCustomName;
            }
            if (customLongInput) {
                customLongInput.disabled = !isCustomName;
                customLongInput.style.opacity = isCustomName ? '1' : '0.7';
                if (document.activeElement !== customLongInput) {
                    customLongInput.value = isCustomName ? (cfg.custom_long_name || cfg.long_name || '') : (cfg.default_long_name || cfg.long_name || '');
                }
            }
            if (customShortInput) {
                customShortInput.disabled = !isCustomName;
                customShortInput.style.opacity = isCustomName ? '1' : '0.7';
                if (document.activeElement !== customShortInput) {
                    customShortInput.value = isCustomName ? (cfg.custom_short_name || cfg.short_name || '') : (cfg.default_short_name || cfg.short_name || '');
                }
            }
            if (saveNameBtn) {
                saveNameBtn.disabled = !isCustomName;
                saveNameBtn.style.opacity = isCustomName ? '1' : '0.5';
            }

            const idEl = document.getElementById('mesh-node-id');
            if (idEl) idEl.textContent = cfg.node_id || '-';
            const numEl = document.getElementById('mesh-node-num');
            if (numEl) numEl.textContent = cfg.node_num ? `(${cfg.node_num})` : '';

            const hopEl = document.getElementById('mesh-hop-limit');
            if (hopEl) hopEl.textContent = (cfg.hop_limit !== undefined) ? cfg.hop_limit : '3';

            const freqEl = document.getElementById('mesh-freq');
            if (freqEl) freqEl.textContent = cfg.freq_hz ? `${(cfg.freq_hz / 1e6).toFixed(3)} MHz` : 'unmapped';

            const sentEl = document.getElementById('mesh-sent-count');
            if (sentEl) sentEl.textContent = cfg.sent_count;

            const lastEl = document.getElementById('mesh-last-sent');
            if (lastEl) {
                // 0xFFFFFFFF is the "never transmitted" sentinel.
                lastEl.textContent = (cfg.seconds_since_last === 4294967295)
                    ? 'never'
                    : `${cfg.seconds_since_last}s ago`;
            }

            const warn = document.getElementById('mesh-region-warning');
            if (warn) warn.style.display = (cfg.enabled && !cfg.freq_hz) ? 'block' : 'none';
        }

        function toggleMeshtasticCustomName(enable) {
            const longInput = document.getElementById('mesh-custom-long-name');
            const shortInput = document.getElementById('mesh-custom-short-name');
            const saveBtn = document.getElementById('mesh-save-name-btn');
            if (longInput) {
                longInput.disabled = !enable;
                longInput.style.opacity = enable ? '1' : '0.7';
            }
            if (shortInput) {
                shortInput.disabled = !enable;
                shortInput.style.opacity = enable ? '1' : '0.7';
            }
            if (saveBtn) {
                saveBtn.disabled = !enable;
                saveBtn.style.opacity = enable ? '1' : '0.5';
            }
            if (enable) {
                if (longInput) longInput.focus();
            } else {
                fetch('/api/v1/meshtastic/config?custom_name_enabled=false', { method: 'POST' })
                    .then(res => res.json())
                    .then(applyMeshtasticConfig)
                    .catch(err => {
                        alert(`Failed to revert custom name: ${err.message}`);
                        refreshMeshtastic();
                    });
            }
        }

        function saveMeshtasticCustomName() {
            const longInput = document.getElementById('mesh-custom-long-name');
            const shortInput = document.getElementById('mesh-custom-short-name');
            const lName = longInput ? longInput.value.trim().slice(0, 23) : '';
            const sName = shortInput ? shortInput.value.trim().slice(0, 4) : '';
            const saveBtn = document.getElementById('mesh-save-name-btn');
            if (saveBtn) {
                saveBtn.textContent = 'Saving...';
                saveBtn.disabled = true;
            }
            fetch(`/api/v1/meshtastic/config?custom_name_enabled=true&long_name=${encodeURIComponent(lName)}&short_name=${encodeURIComponent(sName)}`, { method: 'POST' })
                .then(res => res.json())
                .then(cfg => {
                    applyMeshtasticConfig(cfg);
                    if (saveBtn) {
                        saveBtn.textContent = 'Saved!';
                        setTimeout(() => {
                            if (saveBtn) {
                                saveBtn.textContent = 'Save';
                                saveBtn.disabled = !document.getElementById('mesh-custom-name-chk')?.checked;
                            }
                        }, 1200);
                    }
                })
                .catch(err => {
                    alert(`Failed to save custom name: ${err.message}`);
                    if (saveBtn) {
                        saveBtn.textContent = 'Save';
                        saveBtn.disabled = false;
                    }
                    refreshMeshtastic();
                });
        }

        function refreshMeshtastic() {
            fetch('/api/v1/meshtastic/config')
                .then(res => res.json())
                .then(applyMeshtasticConfig)
                .catch(err => console.log('Meshtastic config unavailable:', err.message));
        }

        function toggleMeshtastic(enable) {
            updateAdaptiveWeatherPanelState();
            updateMeshtasticPanelState();
            fetch(`/api/v1/meshtastic/config?enabled=${enable ? 'true' : 'false'}`, { method: 'POST' })
                .then(res => res.json())
                .then(applyMeshtasticConfig)
                .catch(err => {
                    alert(`Failed to update Meshtastic beacon: ${err.message}`);
                    refreshMeshtastic();
                });
        }

        function setMeshtasticInterval(sec) {
            updateMeshIntervalFloodWarning('mesh-interval-flood-warning', sec);
            fetch(`/api/v1/meshtastic/config?interval=${sec}`, { method: 'POST' })
                .then(res => res.json())
                .then(applyMeshtasticConfig)
                .catch(err => {
                    alert(`Failed to set Meshtastic interval: ${err.message}`);
                    refreshMeshtastic();
                });
        }

        function setMeshtasticHopLimit(hop) {
            fetch(`/api/v1/meshtastic/config?hop_limit=${hop}`, { method: 'POST' })
                .then(res => res.json())
                .then(applyMeshtasticConfig)
                .catch(err => {
                    alert(`Failed to set Meshtastic hop limit: ${err.message}`);
                    refreshMeshtastic();
                });
        }

        function setMeshtasticPower(high) {
            fetch(`/api/v1/meshtastic/config?tx_power_high=${high ? 'true' : 'false'}`, { method: 'POST' })
                .then(res => res.json())
                .then(applyMeshtasticConfig)
                .catch(err => {
                    alert(`Failed to set Meshtastic transmit power: ${err.message}`);
                    refreshMeshtastic();
                });
        }

        function setMeshtasticPreset(preset) {
            fetch(`/api/v1/meshtastic/config?preset=${preset}&freq_hz=0&bw_khz=0&sf=0&cr=0&tx_power_dbm=0`, { method: 'POST' })
                .then(res => res.json())
                .then(applyMeshtasticConfig)
                .catch(err => {
                    alert(`Failed to set Meshtastic preset: ${err.message}`);
                    refreshMeshtastic();
                });
        }

        function saveMeshtasticCustomRf() {
            const freq = document.getElementById('mesh-custom-freq').value || '0';
            const bw = document.getElementById('mesh-custom-bw').value || '0';
            const sf = document.getElementById('mesh-custom-sf').value || '0';
            const cr = document.getElementById('mesh-custom-cr').value || '0';
            const pwr = document.getElementById('mesh-custom-tx-pwr').value || '0';
            fetch(`/api/v1/meshtastic/config?preset=255&freq_hz=${freq}&bw_khz=${bw}&sf=${sf}&cr=${cr}&tx_power_dbm=${pwr}`, { method: 'POST' })
                .then(res => res.json())
                .then(applyMeshtasticConfig)
                .then(() => alert('Meshtastic custom RF settings saved.'))
                .catch(err => {
                    alert(`Failed to save custom RF settings: ${err.message}`);
                    refreshMeshtastic();
                });
        }

        function meshtasticSendNow() {
            const statusEl = document.getElementById('mesh-send-status');
            if (statusEl) statusEl.textContent = 'Queued - transmits at the next safe radio window...';

            fetch('/api/v1/meshtastic/send_telemetry', { method: 'POST' })
                .then(res => res.json())
                .then(data => {
                    const before = data.sent_count;
                    let tries = 0;
                    const poll = setInterval(() => {
                        tries++;
                        fetch('/api/v1/meshtastic/config')
                            .then(r => r.json())
                            .then(cfg => {
                                applyMeshtasticConfig(cfg);
                                if (cfg.sent_count > before) {
                                    clearInterval(poll);
                                    if (statusEl) statusEl.textContent = 'Transmitted.';
                                } else if (tries >= 30) {
                                    clearInterval(poll);
                                    if (statusEl) statusEl.textContent =
                                        'Not sent yet - check the serial log (no station observation, or region unmapped).';
                                }
                            });
                    }, 2000);
                })
                .catch(err => {
                    if (statusEl) statusEl.textContent = `Failed: ${err.message}`;
                });
        }

        function toggleStationMeshtasticQueue(enabled) {
            const intervalEl = document.getElementById('sta-mesh-target-interval');
            const interval = intervalEl ? intervalEl.value : '0';
            const statusEl = document.getElementById('sta-mesh-command-status');
            const chk = document.getElementById('sta-mesh-send-checkbox');

            if (enabled) {
                if (statusEl) {
                    statusEl.textContent = 'Queuing...';
                    statusEl.style.color = '#ffca28';
                }
                fetch(`/ws1300_station_meshtastic?interval=${interval}`, { method: 'POST' })
                    .then(res => {
                        if (!res.ok) throw new Error('Gateway rejected the command');
                        return res.json();
                    })
                    .then(data => {
                        if (statusEl) {
                            statusEl.textContent = '⏳ Armed (awaiting communication)';
                            statusEl.style.color = '#ffca28';
                        }
                        updateStats();
                    })
                    .catch(err => {
                        if (chk) chk.checked = false;
                        if (statusEl) {
                            statusEl.textContent = `Failed: ${err.message}`;
                            statusEl.style.color = '#ff416c';
                        }
                    });
            } else {
                if (statusEl) {
                    statusEl.textContent = 'Cancelling...';
                    statusEl.style.color = 'var(--text-muted)';
                }
                fetch('/ws1300_station_meshtastic?cancel=1', { method: 'POST' })
                    .then(() => {
                        if (statusEl) statusEl.textContent = '';
                        updateStats();
                    })
                    .catch(() => {
                        updateStats();
                    });
            }
        }

        function applyMeshcoreConfig(cfg) {
            if (!cfg) return;
            const fwdBtn = document.getElementById('meshcore-fwd-toggle-btn');
            if (fwdBtn && cfg.forwarding_enabled !== undefined) {
                fwdBtn.checked = !!cfg.forwarding_enabled;
            }

            const gwToggle = document.getElementById('meshcore-gw-toggle-btn');
            const gwDetail = document.getElementById('meshcore-gw-detail');
            if (gwToggle && gwDetail) {
                gwToggle.checked = !!cfg.gw_enabled;
                gwDetail.style.display = cfg.gw_enabled ? 'block' : 'none';
            }
            updateAdaptiveWeatherPanelState();
            updateMeshcorePanelState();

            const gwRegSel = document.getElementById('meshcore-gw-region');
            if (gwRegSel && cfg.freq_region && document.activeElement !== gwRegSel) {
                gwRegSel.value = String(cfg.freq_region);
            }

            const gwIntervalSel = document.getElementById('meshcore-gw-interval');
            if (gwIntervalSel && document.activeElement !== gwIntervalSel && cfg.gw_interval_sec !== undefined) {
                gwIntervalSel.value = String(cfg.gw_interval_sec);
            }
            if (cfg.gw_interval_sec !== undefined) {
                updateMeshIntervalFloodWarning('meshcore-gw-interval-flood-warning', cfg.gw_interval_sec);
            }

            const isCustom = (cfg.gw_profile === 255);
            const customToggle = document.getElementById('meshcore-custom-toggle');
            if (customToggle && document.activeElement !== customToggle) {
                customToggle.checked = isCustom;
            }

            const gwProfileSel = document.getElementById('meshcore-gw-profile');
            if (gwProfileSel && document.activeElement !== gwProfileSel && cfg.gw_profile !== undefined && !isCustom) {
                gwProfileSel.value = String(cfg.gw_profile);
            }

            const gwCFreq = document.getElementById('meshcore-gw-custom-freq');
            const gwCBw = document.getElementById('meshcore-gw-custom-bw');
            const gwCSf = document.getElementById('meshcore-gw-custom-sf');
            const gwCCr = document.getElementById('meshcore-gw-custom-cr');
            const gwCPwr = document.getElementById('meshcore-gw-custom-tx-pwr');
            const gwSaveBtn = document.getElementById('meshcore-save-rf-btn');

            if (gwCFreq) gwCFreq.disabled = !isCustom;
            if (gwCBw) gwCBw.disabled = !isCustom;
            if (gwCSf) gwCSf.disabled = !isCustom;
            if (gwCCr) gwCCr.disabled = !isCustom;
            if (gwCPwr) gwCPwr.disabled = !isCustom;
            if (gwSaveBtn) gwSaveBtn.disabled = !isCustom;

            if (isCustom) {
                if (gwCFreq && document.activeElement !== gwCFreq && cfg.gw_custom_freq_hz !== undefined) gwCFreq.value = cfg.gw_custom_freq_hz || '';
                if (gwCBw && document.activeElement !== gwCBw && cfg.gw_custom_bw_khz !== undefined) gwCBw.value = String(cfg.gw_custom_bw_khz);
                if (gwCSf && document.activeElement !== gwCSf && cfg.gw_custom_sf !== undefined) gwCSf.value = String(cfg.gw_custom_sf);
                if (gwCCr && document.activeElement !== gwCCr && cfg.gw_custom_cr !== undefined) gwCCr.value = String(cfg.gw_custom_cr);
                if (gwCPwr && document.activeElement !== gwCPwr && cfg.gw_custom_tx_power_dbm !== undefined) gwCPwr.value = cfg.gw_custom_tx_power_dbm || '';
            } else {
                updateMeshcoreFieldsFromProfile(false);
            }

            const gwPwrBtn = document.getElementById('meshcore-gw-pwr-toggle-btn');
            if (gwPwrBtn && cfg.gw_tx_power_high !== undefined) {
                gwPwrBtn.checked = !!cfg.gw_tx_power_high;
            }

            const mcCustomNameChk = document.getElementById('meshcore-custom-name-chk');
            const mcCustomLongInput = document.getElementById('meshcore-custom-long-name');
            const mcCustomShortInput = document.getElementById('meshcore-custom-short-name');
            const mcSaveNameBtn = document.getElementById('meshcore-save-name-btn');

            const isMcCustomName = !!cfg.custom_name_enabled;
            if (mcCustomNameChk && document.activeElement !== mcCustomNameChk) {
                mcCustomNameChk.checked = isMcCustomName;
            }
            if (mcCustomLongInput) {
                mcCustomLongInput.disabled = !isMcCustomName;
                mcCustomLongInput.style.opacity = isMcCustomName ? '1' : '0.7';
                if (document.activeElement !== mcCustomLongInput) {
                    mcCustomLongInput.value = isMcCustomName ? (cfg.custom_long_name || cfg.gw_long_name || '') : (cfg.default_long_name || cfg.gw_long_name || '');
                }
            }
            if (mcCustomShortInput) {
                mcCustomShortInput.disabled = !isMcCustomName;
                mcCustomShortInput.style.opacity = isMcCustomName ? '1' : '0.7';
                if (document.activeElement !== mcCustomShortInput) {
                    mcCustomShortInput.value = isMcCustomName ? (cfg.custom_short_name || cfg.gw_short_name || '') : (cfg.default_short_name || cfg.gw_short_name || '');
                }
            }
            if (mcSaveNameBtn) {
                mcSaveNameBtn.disabled = !isMcCustomName;
                mcSaveNameBtn.style.opacity = isMcCustomName ? '1' : '0.5';
            }

            const idEl = document.getElementById('meshcore-gw-node-id');
            if (idEl) idEl.textContent = cfg.gw_node_id || '-';
            const numEl = document.getElementById('meshcore-gw-node-num');
            if (numEl) numEl.textContent = cfg.gw_node_num ? `(${cfg.gw_node_num})` : '';

            const freqEl = document.getElementById('meshcore-gw-freq');
            if (freqEl) freqEl.textContent = cfg.freq_hz ? `${(cfg.freq_hz / 1e6).toFixed(3)} MHz` : 'unmapped';

            const profNameEl = document.getElementById('meshcore-gw-profile-name');
            if (profNameEl) {
                const profileNames = ['Long Range (SF10/250k)', 'Medium (SF8/250k)', 'Narrow Extreme (SF11/125k)', 'Fast (SF7/250k)', 'EU/UK Narrow (SF8/62.5k)'];
                if (cfg.gw_profile === 255) {
                    profNameEl.textContent = 'Custom RF';
                } else {
                    profNameEl.textContent = profileNames[cfg.gw_profile !== undefined ? cfg.gw_profile : 4] || 'EU/UK Narrow (SF8/62.5k)';
                }
            }

            const sentEl = document.getElementById('meshcore-gw-sent-count');
            if (sentEl) sentEl.textContent = cfg.gw_sent_count !== undefined ? cfg.gw_sent_count : 0;

            const lastEl = document.getElementById('meshcore-gw-last-sent');
            if (lastEl) {
                lastEl.textContent = (cfg.gw_seconds_since_last === 4294967295 || cfg.gw_seconds_since_last === undefined)
                    ? 'never'
                    : `${cfg.gw_seconds_since_last}s ago`;
            }

            const warn = document.getElementById('meshcore-gw-region-warning');
            if (warn) warn.style.display = (cfg.gw_enabled && !cfg.freq_hz) ? 'block' : 'none';
        }

        function toggleMeshcoreCustomName(enable) {
            const longInput = document.getElementById('meshcore-custom-long-name');
            const shortInput = document.getElementById('meshcore-custom-short-name');
            const saveBtn = document.getElementById('meshcore-save-name-btn');
            if (longInput) {
                longInput.disabled = !enable;
                longInput.style.opacity = enable ? '1' : '0.7';
            }
            if (shortInput) {
                shortInput.disabled = !enable;
                shortInput.style.opacity = enable ? '1' : '0.7';
            }
            if (saveBtn) {
                saveBtn.disabled = !enable;
                saveBtn.style.opacity = enable ? '1' : '0.5';
            }
            if (enable) {
                if (longInput) longInput.focus();
            } else {
                fetch('/api/v1/meshcore/config?custom_name_enabled=false', { method: 'POST' })
                    .then(res => res.json())
                    .then(applyMeshcoreConfig)
                    .catch(err => {
                        alert(`Failed to revert custom name: ${err.message}`);
                        refreshMeshcore();
                    });
            }
        }

        function saveMeshcoreCustomName() {
            const longInput = document.getElementById('meshcore-custom-long-name');
            const shortInput = document.getElementById('meshcore-custom-short-name');
            const lName = longInput ? longInput.value.trim().slice(0, 23) : '';
            const sName = shortInput ? shortInput.value.trim().slice(0, 4) : '';
            const saveBtn = document.getElementById('meshcore-save-name-btn');
            if (saveBtn) {
                saveBtn.textContent = 'Saving...';
                saveBtn.disabled = true;
            }
            fetch(`/api/v1/meshcore/config?custom_name_enabled=true&long_name=${encodeURIComponent(lName)}&short_name=${encodeURIComponent(sName)}`, { method: 'POST' })
                .then(res => res.json())
                .then(cfg => {
                    applyMeshcoreConfig(cfg);
                    if (saveBtn) {
                        saveBtn.textContent = 'Saved!';
                        setTimeout(() => {
                            if (saveBtn) {
                                saveBtn.textContent = 'Save';
                                saveBtn.disabled = !document.getElementById('meshcore-custom-name-chk')?.checked;
                            }
                        }, 1200);
                    }
                })
                .catch(err => {
                    alert(`Failed to save custom name: ${err.message}`);
                    if (saveBtn) {
                        saveBtn.textContent = 'Save';
                        saveBtn.disabled = false;
                    }
                    refreshMeshcore();
                });
        }

        function refreshMeshcore() {
            fetch('/api/v1/meshcore/config')
                .then(res => res.json())
                .then(applyMeshcoreConfig)
                .catch(err => console.log('Meshcore config unavailable:', err.message));
        }

        function toggleMeshcoreGw(enable) {
            updateAdaptiveWeatherPanelState();
            updateMeshcorePanelState();
            fetch(`/api/v1/meshcore/config?gw_enabled=${enable ? 'true' : 'false'}`, { method: 'POST' })
                .then(res => res.json())
                .then(applyMeshcoreConfig)
                .catch(err => {
                    alert(`Failed to update MeshCore gateway beacon: ${err.message}`);
                    refreshMeshcore();
                });
        }

        function setMeshcoreGwInterval(sec) {
            updateMeshIntervalFloodWarning('meshcore-gw-interval-flood-warning', sec);
            fetch(`/api/v1/meshcore/config?gw_interval=${sec}`, { method: 'POST' })
                .then(res => res.json())
                .then(applyMeshcoreConfig)
                .catch(err => {
                    alert(`Failed to update MeshCore interval: ${err.message}`);
                    refreshMeshcore();
                });
        }

        function setMeshcoreGwProfile(profileIdx) {
            fetch(`/api/v1/meshcore/config?gw_profile=${profileIdx}&gw_freq_hz=0&gw_bw_khz=0&gw_sf=0&gw_cr=0&gw_tx_power_dbm=0`, { method: 'POST' })
                .then(res => res.json())
                .then(applyMeshcoreConfig)
                .catch(err => {
                    alert(`Failed to update MeshCore RF profile: ${err.message}`);
                    refreshMeshcore();
                });
        }

        function saveMeshcoreGwCustomRf() {
            const freq = document.getElementById('meshcore-gw-custom-freq').value || '0';
            const bw = document.getElementById('meshcore-gw-custom-bw').value || '0';
            const sf = document.getElementById('meshcore-gw-custom-sf').value || '0';
            const cr = document.getElementById('meshcore-gw-custom-cr').value || '0';
            const pwr = document.getElementById('meshcore-gw-custom-tx-pwr').value || '0';
            fetch(`/api/v1/meshcore/config?gw_profile=255&gw_freq_hz=${freq}&gw_bw_khz=${bw}&gw_sf=${sf}&gw_cr=${cr}&gw_tx_power_dbm=${pwr}`, { method: 'POST' })
                .then(res => res.json())
                .then(applyMeshcoreConfig)
                .then(() => alert('MeshCore custom RF settings saved.'))
                .catch(err => {
                    alert(`Failed to save custom RF settings: ${err.message}`);
                    refreshMeshcore();
                });
        }

        function setMeshcoreGwPower(high) {
            fetch(`/api/v1/meshcore/config?gw_tx_power_high=${high ? 'true' : 'false'}`, { method: 'POST' })
                .then(res => res.json())
                .then(applyMeshcoreConfig)
                .catch(err => {
                    alert(`Failed to update MeshCore TX power: ${err.message}`);
                    refreshMeshcore();
                });
        }

        function meshcoreGwSendNow() {
            const statusEl = document.getElementById('meshcore-gw-send-status');
            if (statusEl) {
                statusEl.textContent = 'Queued - awaiting safe radio window…';
                statusEl.style.color = '#ffca28';
            }

            let before = 0;
            const sentEl = document.getElementById('meshcore-gw-sent-count');
            if (sentEl) before = parseInt(sentEl.textContent, 10) || 0;

            fetch('/api/v1/meshcore/send_telemetry', { method: 'POST' })
                .then(res => res.json())
                .then(() => {
                    let tries = 0;
                    const poll = setInterval(() => {
                        tries++;
                        fetch('/api/v1/meshcore/config')
                            .then(r => r.json())
                            .then(cfg => {
                                applyMeshcoreConfig(cfg);
                                if (cfg.gw_sent_count > before) {
                                    clearInterval(poll);
                                    if (statusEl) {
                                        statusEl.textContent = 'Transmitted.';
                                        statusEl.style.color = '#00d2ff';
                                    }
                                } else if (tries >= 30) {
                                    clearInterval(poll);
                                    if (statusEl) {
                                        statusEl.textContent = 'Not sent yet - check serial log (no station obs or region unmapped).';
                                        statusEl.style.color = 'var(--text-muted)';
                                    }
                                }
                            });
                    }, 2000);
                })
                .catch(err => {
                    if (statusEl) {
                        statusEl.textContent = `Failed: ${err.message}`;
                        statusEl.style.color = '#ff416c';
                    }
                });
        }

        function toggleMeshcoreForwarding(enable) {
            updateAdaptiveWeatherPanelState();
            updateMeshcorePanelState();
            fetch(`/api/v1/meshcore/config?forwarding=${enable ? 'true' : 'false'}`, { method: 'POST' })
                .then(res => res.json())
                .then(applyMeshcoreConfig)
                .catch(err => {
                    alert(`Failed to update MeshCore forwarding: ${err.message}`);
                    refreshMeshcore();
                });
        }

        function toggleStationMeshcoreQueue(enabled) {
            const intervalEl = document.getElementById('sta-meshcore-target-interval');
            const interval = intervalEl ? intervalEl.value : '0';
            const statusEl = document.getElementById('sta-meshcore-command-status');
            const chk = document.getElementById('sta-meshcore-send-checkbox');

            if (enabled) {
                if (statusEl) {
                    statusEl.textContent = 'Queuing...';
                    statusEl.style.color = '#ffca28';
                }
                fetch(`/ws1300_station_meshcore?interval=${interval}`, { method: 'POST' })
                    .then(res => {
                        if (!res.ok) throw new Error('Gateway rejected the command');
                        return res.json();
                    })
                    .then(data => {
                        if (statusEl) {
                            statusEl.textContent = '⏳ Armed (awaiting communication)';
                            statusEl.style.color = '#ffca28';
                        }
                        updateStats();
                    })
                    .catch(err => {
                        if (chk) chk.checked = false;
                        if (statusEl) {
                            statusEl.textContent = `Failed: ${err.message}`;
                            statusEl.style.color = '#ff416c';
                        }
                    });
            } else {
                if (statusEl) {
                    statusEl.textContent = 'Cancelling...';
                    statusEl.style.color = 'var(--text-muted)';
                }
                fetch('/ws1300_station_meshcore?cancel=1', { method: 'POST' })
                    .then(() => {
                        if (statusEl) statusEl.textContent = '';
                        updateStats();
                    })
                    .catch(() => {
                        updateStats();
                    });
            }
        }

        function decodeTriggerReasons(mask) {
            if (!mask) return '-';
            const reasons = [];
            if (mask & (1 << 0)) reasons.push('TEMP');
            if (mask & (1 << 1)) reasons.push('HUM');
            if (mask & (1 << 2)) reasons.push('PRESS');
            if (mask & (1 << 3)) reasons.push('WIND_SPEED');
            if (mask & (1 << 4)) reasons.push('WIND_GUST');
            if (mask & (1 << 5)) reasons.push('WIND_DIR');
            if (mask & (1 << 6)) reasons.push('PRECIP_CML');
            if (mask & (1 << 7)) reasons.push('PRECIP_RATE');
            if (mask & (1 << 8)) reasons.push('ILLUM');
            if (mask & (1 << 9)) reasons.push('SOLAR_RAD');
            if (mask & (1 << 10)) reasons.push('UV');
            if (mask & (1 << 11)) reasons.push('RAIN_START');
            if (mask & (1 << 12)) reasons.push('RAIN_STOP');
            if (mask & (1 << 13)) reasons.push('BASELINE');
            return reasons.length > 0 ? reasons.join(', ') : `0x${mask.toString(16)}`;
        }

        let g_meshtasticIsOpen = false; // Closed by default
        let g_meshcoreIsOpen = false;   // Closed by default
        let g_adaptiveWeatherIsOpen = false; // Closed by default anyway
        let g_isD1Mode = false;
        let g_isWs1300 = false;
        let g_policyStationPending = false;

        function isMeshActiveOnGateway() {
            const mtToggle = document.getElementById('meshtastic-toggle-btn');
            const mcToggle = document.getElementById('meshcore-gw-toggle-btn');
            const mcFwdToggle = document.getElementById('meshcore-fwd-toggle-btn');
            return !!((mtToggle && mtToggle.checked) || (mcToggle && mcToggle.checked) || (mcFwdToggle && mcFwdToggle.checked));
        }

        function updateAdaptiveWeatherPanelState() {
            const panel = document.getElementById('adaptive-weather-panel');
            const header = document.getElementById('adaptive-weather-header');
            const content = document.getElementById('adaptive-weather-content');
            const chevron = document.getElementById('adaptive-weather-chevron');
            const badge = document.getElementById('wx-policy-station-badge');
            const banner = document.getElementById('adaptive-weather-disabled-banner');
            const inner = document.getElementById('adaptive-weather-inner');
            if (!panel || !content) return;

            const meshActive = isMeshActiveOnGateway();

            // 1. Accordion display: closed by default, opened/closed manually by user
            if (g_adaptiveWeatherIsOpen) {
                content.style.display = 'block';
                if (chevron) chevron.style.transform = 'rotate(0deg)';
                if (header) header.style.marginBottom = '0.5rem';
                panel.style.padding = '1.5rem';
            } else {
                content.style.display = 'none';
                if (chevron) chevron.style.transform = 'rotate(-90deg)';
                if (header) header.style.marginBottom = '0';
                panel.style.padding = '1rem 1.5rem';
            }

            // 2. Panel enablement: disabled when mesh features are disabled
            if (!meshActive) {
                if (banner) banner.style.display = 'flex';
                if (inner) {
                    inner.style.opacity = '0.45';
                    inner.style.pointerEvents = 'none';
                    const controls = inner.querySelectorAll('input, select, button');
                    controls.forEach(el => el.disabled = true);
                }
                if (badge) {
                    badge.textContent = 'Mesh Disabled';
                    badge.style.background = 'rgba(255, 255, 255, 0.08)';
                    badge.style.color = 'var(--text-muted)';
                }
            } else {
                if (banner) banner.style.display = 'none';
                if (inner) {
                    inner.style.opacity = '1';
                    inner.style.pointerEvents = 'auto';
                    const controls = inner.querySelectorAll('input, select, button');
                    controls.forEach(el => el.disabled = false);
                }
                if (badge) {
                    if (g_isWs1300) {
                        if (g_policyStationPending) {
                            badge.textContent = '⏳ Station Sync Pending';
                            badge.style.background = 'rgba(255, 193, 7, 0.15)';
                            badge.style.color = '#ffc107';
                        } else {
                            badge.textContent = 'Station Synced';
                            badge.style.background = 'rgba(40, 167, 69, 0.15)';
                            badge.style.color = '#28a745';
                        }
                    } else {
                        badge.textContent = 'Gateway Mesh Active';
                        badge.style.background = 'rgba(0, 210, 255, 0.15)';
                        badge.style.color = '#00d2ff';
                    }
                }
            }
        }

        function toggleAdaptiveWeatherManual() {
            g_adaptiveWeatherIsOpen = !g_adaptiveWeatherIsOpen;
            updateAdaptiveWeatherPanelState();
        }

        function updateMeshtasticPanelState() {
            const panel = document.getElementById('meshtastic-panel');
            const header = document.getElementById('meshtastic-header');
            const content = document.getElementById('meshtastic-content');
            const chevron = document.getElementById('meshtastic-chevron');
            const badge = document.getElementById('meshtastic-header-badge');
            if (!panel || !content) return;

            if (g_meshtasticIsOpen) {
                content.style.display = 'block';
                if (chevron) chevron.style.transform = 'rotate(0deg)';
                if (header) header.style.marginBottom = '0.5rem';
                panel.style.padding = '1.5rem';
            } else {
                content.style.display = 'none';
                if (chevron) chevron.style.transform = 'rotate(-90deg)';
                if (header) header.style.marginBottom = '0';
                panel.style.padding = '1rem 1.5rem';
            }

            const mtToggle = document.getElementById('meshtastic-toggle-btn');
            const isEnabled = mtToggle ? mtToggle.checked : false;
            if (badge) {
                badge.textContent = isEnabled ? 'Active' : 'Disabled';
                badge.style.background = isEnabled ? 'rgba(0, 210, 255, 0.15)' : 'rgba(255, 255, 255, 0.08)';
                badge.style.color = isEnabled ? '#00d2ff' : 'var(--text-muted)';
            }
        }

        function toggleMeshtasticPanelManual() {
            g_meshtasticIsOpen = !g_meshtasticIsOpen;
            updateMeshtasticPanelState();
        }

        function updateMeshcorePanelState() {
            const panel = document.getElementById('meshcore-panel');
            const header = document.getElementById('meshcore-header');
            const content = document.getElementById('meshcore-content');
            const chevron = document.getElementById('meshcore-chevron');
            const badge = document.getElementById('meshcore-header-badge');
            if (!panel || !content) return;

            if (g_meshcoreIsOpen) {
                content.style.display = 'block';
                if (chevron) chevron.style.transform = 'rotate(0deg)';
                if (header) header.style.marginBottom = '0.5rem';
                panel.style.padding = '1.5rem';
            } else {
                content.style.display = 'none';
                if (chevron) chevron.style.transform = 'rotate(-90deg)';
                if (header) header.style.marginBottom = '0';
                panel.style.padding = '1rem 1.5rem';
            }

            const mcToggle = document.getElementById('meshcore-gw-toggle-btn');
            const mcFwdToggle = document.getElementById('meshcore-fwd-toggle-btn');
            const isEnabled = !!((mcToggle && mcToggle.checked) || (mcFwdToggle && mcFwdToggle.checked));
            if (badge) {
                badge.textContent = isEnabled ? 'Active' : 'Disabled';
                badge.style.background = isEnabled ? 'rgba(192, 132, 252, 0.15)' : 'rgba(255, 255, 255, 0.08)';
                badge.style.color = isEnabled ? '#c084fc' : 'var(--text-muted)';
            }
        }

        function toggleMeshcorePanelManual() {
            g_meshcoreIsOpen = !g_meshcoreIsOpen;
            updateMeshcorePanelState();
        }

        let g_isWeatherPolicyEditing = false;
        function applyWeatherPolicy(policy) {
            if (!policy) return;
            g_policyStationPending = !!policy.station_pending;
            const verEl = document.getElementById('wx-policy-ver');
            if (verEl) verEl.textContent = `v${policy.version || 1}`;
            const crcEl = document.getElementById('wx-policy-crc');
            if (crcEl) crcEl.textContent = policy.crc16_hex || '0x0000';
            const cdDisp = document.getElementById('wx-policy-cooldown-display');
            if (cdDisp) cdDisp.textContent = `${Math.round((policy.cooldown_sec || 300) / 60)} min`;
            const bgDisp = document.getElementById('wx-policy-budget-display');
            if (bgDisp) bgDisp.textContent = `${policy.budget_per_hour || 6} / hr`;

            if (policy.mt_diag) {
                const elEvents = document.getElementById('wx-diag-mt-events');
                if (elEvents) elEvents.textContent = `${policy.mt_diag.events_this_hour} / ${policy.budget_per_hour || 6}`;
                const elCd = document.getElementById('wx-diag-mt-cd-level');
                if (elCd) elCd.textContent = `Level ${policy.mt_diag.cooldown_level} (${policy.mt_diag.cooldown_level === 0 ? 'Normal' : 'Throttle'})`;
                const elReasons = document.getElementById('wx-diag-mt-reasons');
                if (elReasons) elReasons.textContent = decodeTriggerReasons(policy.mt_diag.last_reasons);
            }
            if (policy.mc_diag) {
                const elEvents = document.getElementById('wx-diag-mc-events');
                if (elEvents) elEvents.textContent = `${policy.mc_diag.events_this_hour} / ${policy.budget_per_hour || 6}`;
                const elCd = document.getElementById('wx-diag-mc-cd-level');
                if (elCd) elCd.textContent = `Level ${policy.mc_diag.cooldown_level} (${policy.mc_diag.cooldown_level === 0 ? 'Normal' : 'Throttle'})`;
                const elReasons = document.getElementById('wx-diag-mc-reasons');
                if (elReasons) elReasons.textContent = decodeTriggerReasons(policy.mc_diag.last_reasons);
            }

            if (g_isWeatherPolicyEditing) return;

            const setVal = (id, val) => { const el = document.getElementById(id); if (el && val !== undefined) el.value = val; };
            const setChk = (id, val) => { const el = document.getElementById(id); if (el && val !== undefined) el.checked = !!val; };

            setVal('wx-input-cooldown', policy.cooldown_sec);
            setVal('wx-input-budget', policy.budget_per_hour);
            setVal('wx-input-calm-gate', policy.calm_gate_ms);
            setVal('wx-input-rain-on', policy.rain_on_mmh);
            setVal('wx-input-rain-off', policy.rain_off_mmh);
            setChk('wx-input-rain-state', policy.rain_state_changes);

            if (policy.temp) { setChk('wx-en-temp', policy.temp.enabled); setVal('wx-th-temp', policy.temp.delta_threshold); }
            if (policy.humidity) { setChk('wx-en-hum', policy.humidity.enabled); setVal('wx-th-hum', policy.humidity.delta_threshold); }
            if (policy.pressure) { setChk('wx-en-press', policy.pressure.enabled); setVal('wx-th-press', policy.pressure.delta_threshold); }
            if (policy.wind_speed) { setChk('wx-en-wind_speed', policy.wind_speed.enabled); setVal('wx-th-wind_speed', policy.wind_speed.delta_threshold); }
            if (policy.wind_gust) { setChk('wx-en-wind_gust', policy.wind_gust.enabled); setVal('wx-th-wind_gust', policy.wind_gust.delta_threshold); }
            if (policy.wind_dir) { setChk('wx-en-wind_dir', policy.wind_dir.enabled); setVal('wx-th-wind_dir', policy.wind_dir.delta_threshold); }
            if (policy.precip_cml) { setChk('wx-en-precip_cml', policy.precip_cml.enabled); setVal('wx-th-precip_cml', policy.precip_cml.delta_threshold); }
            if (policy.precip_rate) { setChk('wx-en-precip_rate', policy.precip_rate.enabled); setVal('wx-th-precip_rate', policy.precip_rate.delta_threshold); }
            if (policy.illuminance) { setChk('wx-en-illum', policy.illuminance.enabled); setVal('wx-th-illum', policy.illuminance.delta_threshold); }
            if (policy.solar_radiation) { setChk('wx-en-solar_rad', policy.solar_radiation.enabled); setVal('wx-th-solar_rad', policy.solar_radiation.delta_threshold); }
            if (policy.uv_index) { setChk('wx-en-uv', policy.uv_index.enabled); setVal('wx-th-uv', policy.uv_index.delta_threshold); }
            updateAdaptiveWeatherPanelState();
        }

        function refreshWeatherPolicy() {
            fetch('/api/v1/weather_policy')
                .then(res => res.json())
                .then(applyWeatherPolicy)
                .catch(() => {});
        }

        function saveWeatherPolicy() {
            const getVal = id => { const el = document.getElementById(id); return el ? el.value : ''; };
            const getChk = id => { const el = document.getElementById(id); return el ? (el.checked ? 'true' : 'false') : 'false'; };

            const params = new URLSearchParams({
                cooldown_sec: getVal('wx-input-cooldown'),
                budget_per_hour: getVal('wx-input-budget'),
                calm_gate_ms: getVal('wx-input-calm-gate'),
                rain_on_mmh: getVal('wx-input-rain-on'),
                rain_off_mmh: getVal('wx-input-rain-off'),
                rain_state_changes: getChk('wx-input-rain-state'),
                temp_en: getChk('wx-en-temp'), temp_th: getVal('wx-th-temp'),
                hum_en: getChk('wx-en-hum'), hum_th: getVal('wx-th-hum'),
                press_en: getChk('wx-en-press'), press_th: getVal('wx-th-press'),
                wind_speed_en: getChk('wx-en-wind_speed'), wind_speed_th: getVal('wx-th-wind_speed'),
                wind_gust_en: getChk('wx-en-wind_gust'), wind_gust_th: getVal('wx-th-wind_gust'),
                wind_dir_en: getChk('wx-en-wind_dir'), wind_dir_th: getVal('wx-th-wind_dir'),
                precip_cml_en: getChk('wx-en-precip_cml'), precip_cml_th: getVal('wx-th-precip_cml'),
                precip_rate_en: getChk('wx-en-precip_rate'), precip_rate_th: getVal('wx-th-precip_rate'),
                illum_en: getChk('wx-en-illum'), illum_th: getVal('wx-th-illum'),
                solar_rad_en: getChk('wx-en-solar_rad'), solar_rad_th: getVal('wx-th-solar_rad'),
                uv_en: getChk('wx-en-uv'), uv_th: getVal('wx-th-uv')
            });

            fetch(`/api/v1/weather_policy?${params.toString()}`, { method: 'POST' })
                .then(res => {
                    if (!res.ok) throw new Error(`HTTP ${res.status}`);
                    return res.json();
                })
                .then(data => {
                    g_isWeatherPolicyEditing = false;
                    applyWeatherPolicy(data);
                    alert('Adaptive weather policy saved successfully.');
                })
                .catch(err => {
                    alert(`Failed to save weather policy: ${err.message}`);
                });
        }

        function resetWeatherPolicyDefaults() {
            if (!confirm('Reset all adaptive weather policy thresholds to default values?')) return;
            fetch('/api/v1/weather_policy?reset_defaults=1', { method: 'POST' })
                .then(res => res.json())
                .then(data => {
                    g_isWeatherPolicyEditing = false;
                    applyWeatherPolicy(data);
                    alert('Weather policy reset to defaults.');
                })
                .catch(err => {
                    alert(`Failed to reset policy defaults: ${err.message}`);
                });
        }

        function resetWeatherPolicyBaseline() {
            fetch('/api/v1/weather_policy?reset_baseline=1', { method: 'POST' })
                .then(res => res.json())
                .then(data => {
                    applyWeatherPolicy(data);
                    alert('Adaptive weather baseline snapshot reset.');
                })
                .catch(err => {
                    alert(`Failed to reset baseline: ${err.message}`);
                });
        }

        let historyCharts = {};
        let historyTimeRange = null;
        let isSyncingHistoryZoom = false;
        const histSync = (typeof uPlot !== 'undefined' && uPlot.sync) ? uPlot.sync("histWeather") : { key: "histWeather" };

        function degToCardinal(deg) {
            if (deg == null || isNaN(deg)) return "--";
            const dirs = ["N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"];
            const norm = ((deg % 360) + 360) % 360;
            return dirs[Math.round(norm / 22.5) % 16];
        }

        function calcDewPointC(tempC, rh) {
            if (tempC == null || rh == null || isNaN(tempC) || isNaN(rh) || rh <= 0) return null;
            const a = 17.625, b = 243.04;
            const c = Math.log(Math.max(1, rh) / 100.0) + (a * tempC) / (b + tempC);
            return parseFloat(((b * c) / (a - c)).toFixed(1));
        }

        function calcFeelsLikeC(tempC, windMs, rh) {
            if (tempC == null || isNaN(tempC)) return null;
            if (tempC <= 10.0 && windMs != null && !isNaN(windMs)) {
                const vKmh = windMs * 3.6;
                if (vKmh > 4.8) {
                    const vPow = Math.pow(vKmh, 0.16);
                    return parseFloat((13.12 + 0.6215 * tempC - 11.37 * vPow + 0.3965 * tempC * vPow).toFixed(1));
                }
                return parseFloat(tempC.toFixed(1));
            }
            if (tempC > 26.7 && rh != null && !isNaN(rh)) {
                const hi = -8.78469475556 + 1.61139411 * tempC + 2.33854883889 * rh
                    - 0.14611605 * tempC * rh - 0.012308094 * tempC * tempC - 0.0164248277778 * rh * rh
                    + 0.002211732 * tempC * tempC * rh + 0.00072546 * tempC * rh * rh - 0.000003582 * tempC * tempC * rh * rh;
                return parseFloat(hi.toFixed(1));
            }
            return parseFloat(tempC.toFixed(1));
        }

        function calcSeriesStats(arr, decimals = 1) {
            if (!arr || !arr.length) return null;
            let min = Infinity, max = -Infinity, sum = 0, count = 0, first = null, last = null;
            for (let i = 0; i < arr.length; i++) {
                const v = arr[i];
                if (v != null && !isNaN(v)) {
                    if (v < min) min = v;
                    if (v > max) max = v;
                    sum += v;
                    count++;
                    if (first === null) first = v;
                    last = v;
                }
            }
            if (!count) return null;
            return {
                min: min.toFixed(decimals),
                max: max.toFixed(decimals),
                avg: (sum / count).toFixed(decimals),
                first: first,
                last: last,
                count: count
            };
        }

        function calcCircularWindMean(dirs, speeds) {
            if (!dirs || !dirs.length) return null;
            let sinSum = 0, cosSum = 0, count = 0;
            for (let i = 0; i < dirs.length; i++) {
                const d = dirs[i];
                if (d == null || isNaN(d)) continue;
                const spd = (speeds && speeds[i] != null && !isNaN(speeds[i])) ? Math.max(0.15, speeds[i]) : 1.0;
                const rad = (d * Math.PI) / 180.0;
                sinSum += Math.sin(rad) * spd;
                cosSum += Math.cos(rad) * spd;
                count++;
            }
            if (!count || (Math.abs(sinSum) < 1e-6 && Math.abs(cosSum) < 1e-6)) return null;
            const deg = Math.round(((Math.atan2(sinSum, cosSum) * 180.0) / Math.PI + 360.0) % 360.0);
            return { deg: deg, cardinal: degToCardinal(deg) };
        }

        function formatHistLegendTime(u, ts) {
            if (ts == null || isNaN(ts)) return "--/-- --:--";
            const d = new Date(ts * 1000);
            const mo = String(d.getMonth() + 1).padStart(2, '0');
            const da = String(d.getDate()).padStart(2, '0');
            const hh = String(d.getHours()).padStart(2, '0');
            const mm = String(d.getMinutes()).padStart(2, '0');
            return `${mo}/${da} ${hh}:${mm}`;
        }

        function syncHistoryScaleX(sourcePlot, min, max) {
            if (isSyncingHistoryZoom) return;
            isSyncingHistoryZoom = true;
            try {
                Object.values(historyCharts).forEach(plot => {
                    if (plot && plot !== sourcePlot) {
                        plot.setScale('x', { min: min, max: max });
                    }
                });
            } finally {
                isSyncingHistoryZoom = false;
            }
        }

        function resetHistoryZoom() {
            if (!historyTimeRange) return;
            isSyncingHistoryZoom = true;
            try {
                Object.values(historyCharts).forEach(plot => {
                    if (plot) {
                        plot.setScale('x', { min: historyTimeRange[0], max: historyTimeRange[1] });
                    }
                });
            } finally {
                isSyncingHistoryZoom = false;
            }
        }

        function drawWindDirectionRibbon(u) {
            const ctx = u.ctx;
            const { left, top, width, height } = u.bbox;
            if (width <= 40 || height <= 60) return;

            const pxRatio = (typeof uPlot !== 'undefined' && uPlot.pxRatio) ? uPlot.pxRatio : (window.devicePixelRatio || 1);
            const bandH = Math.round(34 * pxRatio);

            ctx.save();
            ctx.beginPath();
            ctx.rect(left, top, width, bandH + 2 * pxRatio);
            ctx.clip();

            // Subtle ribbon backdrop & bottom divider
            ctx.fillStyle = "rgba(168, 85, 247, 0.08)";
            ctx.fillRect(left, top, width, bandH);
            ctx.strokeStyle = "rgba(168, 85, 247, 0.24)";
            ctx.lineWidth = Math.max(1, 0.8 * pxRatio);
            ctx.beginPath();
            ctx.moveTo(left, top + bandH);
            ctx.lineTo(left + width, top + bandH);
            ctx.stroke();

            const times = u.data[0];
            const speeds = u.data[1];
            const dirs = u.data[3];
            if (!times || !times.length || !dirs) {
                ctx.restore();
                return;
            }

            const xMin = u.scales.x.min != null ? u.scales.x.min : times[0];
            const xMax = u.scales.x.max != null ? u.scales.x.max : times[times.length - 1];
            const spanSec = Math.max(300, xMax - xMin);

            // Choose bucket interval (default 1 hour = 3600s for 24h view; adapts on zoom or narrow screens)
            const cssWidth = width / pxRatio;
            const maxSlots = Math.max(4, Math.floor((cssWidth - 36) / 30));
            const candidateSteps = [900, 1800, 3600, 7200, 10800, 14400];
            let stepSec = 3600;
            for (let s of candidateSteps) {
                if (spanSec / s <= maxSlots) {
                    stepSec = s;
                    break;
                }
                stepSec = s;
            }

            // Small badge on far left of ribbon indicating bucket window
            const stepLabel = stepSec >= 3600 ? `${stepSec / 3600}H` : `${stepSec / 60}M`;
            ctx.font = `700 ${Math.round(8.5 * pxRatio)}px -apple-system, BlinkMacSystemFont, sans-serif`;
            ctx.fillStyle = "rgba(216, 180, 254, 0.72)";
            ctx.textAlign = "left";
            ctx.textBaseline = "middle";
            ctx.fillText(stepLabel, left + 5 * pxRatio, top + bandH * 0.5);

            const firstBucket = Math.floor(xMin / stepSec) * stepSec;
            let lastDrawnX = left + 26 * pxRatio;
            const minGapPx = 26 * pxRatio;

            for (let bStart = firstBucket; bStart <= xMax; bStart += stepSec) {
                const bEnd = bStart + stepSec;
                let sinSum = 0, cosSum = 0, spdSum = 0, tSum = 0, cnt = 0;
                for (let i = 0; i < times.length; i++) {
                    const t = times[i];
                    if (t < bStart || t >= bEnd || t < xMin || t > xMax) continue;
                    const d = dirs[i];
                    if (d == null || isNaN(d)) continue;
                    const s = (speeds && speeds[i] != null && !isNaN(speeds[i])) ? speeds[i] : 0.5;
                    const w = Math.max(0.15, s);
                    const rad = (d * Math.PI) / 180.0;
                    sinSum += Math.sin(rad) * w;
                    cosSum += Math.cos(rad) * w;
                    spdSum += s;
                    tSum += t;
                    cnt++;
                }
                if (!cnt) continue;

                const centerT = tSum / cnt;
                const cx = u.valToPos(centerT, 'x', true);
                if (cx < lastDrawnX || cx > left + width - 14 * pxRatio) continue;
                lastDrawnX = cx + minGapPx;

                const avgDeg = ((Math.atan2(sinSum, cosSum) * 180.0) / Math.PI + 360.0) % 360.0;
                const avgSpd = spdSum / cnt;
                const cardinal = degToCardinal(avgDeg);

                // Meteorological wind direction avgDeg is where wind blows FROM.
                // Flow vector points TOWARD (avgDeg + 180) deg on compass (0=N/Up, 90=E/Right).
                const flowCompass = (avgDeg + 180.0) % 360.0;
                const angleRad = ((flowCompass - 90.0) * Math.PI) / 180.0;

                const arrowCy = top + 11 * pxRatio;
                const arrowLen = 6.2 * pxRatio;
                const headLen = 4.2 * pxRatio;

                ctx.save();
                ctx.translate(cx, arrowCy);
                ctx.rotate(angleRad);

                const alpha = avgSpd < 0.2 ? 0.55 : Math.min(1.0, 0.72 + (avgSpd / 12.0) * 0.28);
                ctx.strokeStyle = `rgba(216, 180, 254, ${alpha.toFixed(2)})`;
                ctx.fillStyle = `rgba(216, 180, 254, ${alpha.toFixed(2)})`;
                ctx.lineWidth = 1.65 * pxRatio;
                ctx.lineCap = "round";
                ctx.lineJoin = "round";

                // Arrow shaft
                ctx.beginPath();
                ctx.moveTo(-arrowLen * 0.75, 0);
                ctx.lineTo(arrowLen * 0.75, 0);
                ctx.stroke();

                // Arrowhead at +X tip
                ctx.beginPath();
                ctx.moveTo(arrowLen * 0.85, 0);
                ctx.lineTo(arrowLen * 0.85 - headLen, -headLen * 0.68);
                ctx.lineTo(arrowLen * 0.85 - headLen, headLen * 0.68);
                ctx.closePath();
                ctx.fill();
                ctx.restore();

                // Cardinal label below arrow
                ctx.font = `700 ${Math.round(9 * pxRatio)}px -apple-system, BlinkMacSystemFont, sans-serif`;
                ctx.fillStyle = avgSpd >= 3.0 ? "#f3e8ff" : "#d8b4fe";
                ctx.textAlign = "center";
                ctx.textBaseline = "middle";
                ctx.fillText(cardinal, cx, top + 25.5 * pxRatio);
            }

            ctx.restore();
        }

        window.g_units = window.g_units || {
            temp_id: 0,
            wind_id: 0,
            precip_id: 0,
            press_id: 0,
            temp: 'C',
            wind: 'm/s',
            precip: 'mm',
            press: 'hPa'
        };

        function msToBft(ms) {
            if (ms == null || isNaN(ms)) return null;
            if (ms < 0.5) return 0;
            if (ms < 1.6) return 1;
            if (ms < 3.4) return 2;
            if (ms < 5.5) return 3;
            if (ms < 8.0) return 4;
            if (ms < 10.8) return 5;
            if (ms < 13.9) return 6;
            if (ms < 17.2) return 7;
            if (ms < 20.8) return 8;
            if (ms < 24.5) return 9;
            if (ms < 28.5) return 10;
            if (ms < 32.7) return 11;
            return 12;
        }

        function convertTempFromC(valC) {
            if (valC == null || isNaN(valC)) return null;
            const u = window.g_units || {};
            if (u.temp_id === 1 || u.temp === 'F') {
                return valC * 1.8 + 32.0;
            }
            return valC;
        }

        function convertWindFromMs(valMs) {
            if (valMs == null || isNaN(valMs)) return null;
            const u = window.g_units || {};
            switch (Number(u.wind_id)) {
                case 1: return valMs * 3.6;        // km/h
                case 2: return valMs * 2.236936;   // mph
                case 3: return valMs * 3.28084;    // ft/s
                case 4: return msToBft(valMs);     // bft
                case 5: return valMs * 1.943844;   // kn
                case 0:
                default: return valMs;             // m/s
            }
        }

        function convertPrecipFromMm(valMm) {
            if (valMm == null || isNaN(valMm)) return null;
            const u = window.g_units || {};
            if (u.precip_id === 1 || u.precip === 'in') {
                return valMm * 0.03937008;
            }
            return valMm;
        }

        function convertPressFromHpa(valHpa) {
            if (valHpa == null || isNaN(valHpa)) return null;
            const u = window.g_units || {};
            switch (Number(u.press_id)) {
                case 1: return valHpa * 0.7500616;  // mmHg
                case 2: return valHpa * 0.02952998; // inHg
                case 0:
                default: return valHpa;             // hPa
            }
        }

        function convertArr(arr, fn, decimals = 2) {
            if (!Array.isArray(arr)) return arr;
            const factor = Math.pow(10, decimals);
            return arr.map(v => {
                if (v == null || isNaN(v)) return null;
                const conv = fn(v);
                return conv == null ? null : Math.round(conv * factor) / factor;
            });
        }

        function setDisplayUnits() {
            const tempEl = document.getElementById('unit-temp-select');
            const windEl = document.getElementById('unit-wind-select');
            const precipEl = document.getElementById('unit-precip-select');
            const pressEl = document.getElementById('unit-press-select');
            const statusEl = document.getElementById('unit-sync-status');

            const params = new URLSearchParams();
            if (tempEl) params.set('temp', tempEl.value);
            if (windEl) params.set('wind', windEl.value);
            if (precipEl) params.set('precip', precipEl.value);
            if (pressEl) params.set('press', pressEl.value);

            if (statusEl) {
                statusEl.textContent = 'Syncing with LCD...';
                statusEl.style.color = 'var(--accent-blue)';
            }

            fetch('/set_units?' + params.toString(), { method: 'POST' })
                .then(res => res.json())
                .then(resp => {
                    if (resp && resp.status === 'ok') {
                        window.g_units = {
                            temp_id: resp.temp_id,
                            wind_id: resp.wind_id,
                            precip_id: resp.precip_id,
                            press_id: resp.press_id,
                            temp: resp.temp,
                            wind: resp.wind,
                            precip: resp.precip,
                            press: resp.press
                        };
                        window.g_unitsSignature = `${resp.temp_id}:${resp.wind_id}:${resp.precip_id}:${resp.press_id}`;
                        if (statusEl) {
                            statusEl.textContent = `Synced (°${resp.temp}, ${resp.wind}, ${resp.precip}, ${resp.press})`;
                            statusEl.style.color = 'var(--accent-green)';
                        }
                        updateStats();
                        if (window.g_lastLiveTrendsData) {
                            renderLiveChartData(window.g_lastLiveTrendsData, true);
                        } else {
                            updateLiveChart();
                        }
                        if (window.g_lastHistoryData && document.getElementById('page-history').style.display === 'block') {
                            renderHistoryChartsData(window.g_lastHistoryData);
                        }
                    } else if (statusEl) {
                        statusEl.textContent = 'Sync failed';
                        statusEl.style.color = 'var(--accent-red)';
                    }
                })
                .catch(() => {
                    if (statusEl) {
                        statusEl.textContent = 'Connection error';
                        statusEl.style.color = 'var(--accent-red)';
                    }
                });
        }

        function initHistoryCharts() {
            fetch('/history')
                .then(res => res.json())
                .then(historyData => {
                    window.g_lastHistoryData = historyData;
                    renderHistoryChartsData(historyData);
                });
        }

        function renderHistoryChartsData(historyData) {
            Object.values(historyCharts).forEach(c => {
                if (c && typeof c.destroy === 'function') c.destroy();
            });
            historyCharts = {};

            if (!historyData || !historyData.time || historyData.time.length === 0) {
                document.getElementById('history-container').innerHTML = `
                    <div style="text-align: center; color: var(--text-muted); padding: 3rem;">
                        No historical data aggregated yet. Wait for a 5-minute interval boundary...
                    </div>`;
                return;
            }

            const n = historyData.time.length;
            historyTimeRange = [historyData.time[0], historyData.time[n - 1]];

            const u = window.g_units || {};
            const uTemp = u.temp || 'C';
            const uWind = u.wind || 'm/s';
            const uPrecip = u.precip || 'mm';
            const uPress = u.press || 'hPa';
            const windDec = (uWind === 'bft') ? 0 : 1;
            const pressDec = (uPress === 'inHg') ? 2 : 1;
            const precipRateDec = (uPrecip === 'in') ? 2 : 1;
            const precipCmlDec = (uPrecip === 'in') ? 2 : 2;

            // Compute derived series in SI first, then convert all series to active units
            const feelsLikeArrC = [];
            const dewPointArrC = [];
            const historyPrecipCmlMm = [];
            const sanitizedPrecipRateMm = [];
            let runningCmlMm = 0;
            let wetIntervals = 0;

            for (let i = 0; i < n; i++) {
                const t = historyData.temp ? historyData.temp[i] : null;
                const rh = historyData.hum ? historyData.hum[i] : null;
                const w = historyData.wind ? historyData.wind[i] : null;

                feelsLikeArrC.push(calcFeelsLikeC(t, w, rh));
                dewPointArrC.push(calcDewPointC(t, rh));

                const hasAccum = !!(historyData.precip_accum && historyData.precip_accum[i] != null && !isNaN(historyData.precip_accum[i]));
                let rawRate = (historyData.precip && historyData.precip[i] != null && !isNaN(historyData.precip[i])) ? historyData.precip[i] : null;
                if (rawRate != null && (rawRate < 0 || rawRate > 300)) {
                    rawRate = (hasAccum && historyData.precip_accum[i] >= 0 && historyData.precip_accum[i] <= 25)
                        ? (historyData.precip_accum[i] * 12)
                        : 0;
                }
                sanitizedPrecipRateMm.push(rawRate);

                let delta = hasAccum ? historyData.precip_accum[i] : null;
                if (delta == null && rawRate != null) {
                    delta = rawRate * (5 / 60);
                }
                if (delta != null && !isNaN(delta) && delta > 0) {
                    runningCmlMm += delta;
                    wetIntervals++;
                }
                historyPrecipCmlMm.push(parseFloat(runningCmlMm.toFixed(3)));
            }

            const tempArr = convertArr(historyData.temp, convertTempFromC, 1);
            const feelsLikeArr = convertArr(feelsLikeArrC, convertTempFromC, 1);
            const dewPointArr = convertArr(dewPointArrC, convertTempFromC, 1);
            const windArr = convertArr(historyData.wind, convertWindFromMs, windDec);
            const gustArr = convertArr(historyData.gust, convertWindFromMs, windDec);
            const pressArr = convertArr(historyData.press, convertPressFromHpa, pressDec);
            const precipRateArr = convertArr(sanitizedPrecipRateMm, convertPrecipFromMm, precipRateDec);
            const historyPrecipCml = convertArr(historyPrecipCmlMm, convertPrecipFromMm, precipCmlDec);

            // Compute Min / Avg / Max summary stats for every chart card in active units
            const stTemp = calcSeriesStats(tempArr, 1);
            const stFeels = calcSeriesStats(feelsLikeArr, 1);
            const stHum = calcSeriesStats(historyData.hum, 0);

            const stWind = calcSeriesStats(windArr, windDec);
            const stGust = calcSeriesStats(gustArr, windDec);
            const stDir = calcCircularWindMean(historyData.wind_dir, historyData.wind);

            const stPress = calcSeriesStats(pressArr, pressDec);
            const stSolar = calcSeriesStats(historyData.solar, 0);
            const stDew = calcSeriesStats(dewPointArr, 1);

            // 3-hour barometric trend (last 36 five-minute intervals)
            let pressTrendStr = "";
            if (pressArr && n >= 6) {
                const lookback = Math.max(0, n - 36);
                const pNow = pressArr[n - 1];
                let pPrev = null;
                for (let k = lookback; k < n - 1; k++) {
                    if (pressArr[k] != null && !isNaN(pressArr[k])) {
                        pPrev = pressArr[k];
                        break;
                    }
                }
                if (pNow != null && pPrev != null) {
                    const dp = pNow - pPrev;
                    const thresh = (uPress === 'inHg') ? 0.015 : (uPress === 'mmHg' ? 0.38 : 0.5);
                    const arrow = dp > thresh ? "↗" : (dp < -thresh ? "↘" : "→");
                    pressTrendStr = ` · ${arrow} ${dp >= 0 ? '+' : ''}${dp.toFixed(pressDec)}`;
                }
            }

            const stRate = calcSeriesStats(precipRateArr, precipRateDec);
            const totalRainVal = convertPrecipFromMm(runningCmlMm);
            const totalRain = (totalRainVal != null ? totalRainVal : 0).toFixed(precipCmlDec);
            const wetMinutes = wetIntervals * 5;
            const wetTimeStr = wetMinutes >= 60 ? `${Math.floor(wetMinutes / 60)}h ${wetMinutes % 60}m` : `${wetMinutes}m`;

            const pill = (color, label, text) =>
                `<span class="hist-stat-pill"><span class="dot" style="background:${color};"></span>${label}: <strong>${text}</strong></span>`;

            const tempBadges = [
                stTemp ? pill("#ff3b30", "Temp", `${stTemp.min}° / ${stTemp.avg}° / ${stTemp.max}°${uTemp}`) : "",
                stFeels ? pill("#ff9f0a", "Feels", `${stFeels.min}°–${stFeels.max}°${uTemp}`) : "",
                stHum ? pill("#34c759", "Hum", `${stHum.min}% / ${stHum.avg}% / ${stHum.max}%`) : ""
            ].join("");

            const windArrowDeg = stDir ? ((stDir.deg + 180) % 360) : 0;
            const windBadges = [
                stWind ? pill("#00e5ff", "Avg / Max", `${stWind.avg} / ${stWind.max} ${uWind}`) : "",
                stGust ? pill("#ffd600", "Peak Gust", `${stGust.max} ${uWind}`) : "",
                stDir ? pill("#c084fc", "Prevailing", `<span style="display:inline-block;transform:rotate(${windArrowDeg}deg);font-weight:700;">↑</span> ${stDir.cardinal} (${stDir.deg}°)`) : ""
            ].join("");

            const envBadges = [
                stPress ? pill("#af52de", "Press", `${stPress.min}–${stPress.max} ${uPress}${pressTrendStr}`) : "",
                stSolar ? pill("#ff9100", "Solar Peak / Avg", `${stSolar.max} / ${stSolar.avg} W/m²`) : "",
                stDew ? pill("#38bdf8", "Dew Pt", `${stDew.min}° / ${stDew.avg}° / ${stDew.max}°${uTemp}`) : ""
            ].join("");

            const precipBadges = [
                pill("#00d2ff", "24h Total", `${totalRain} ${uPrecip}`),
                stRate ? pill("#0088ff", "Peak Rate", `${stRate.max} ${uPrecip}/h`) : "",
                pill("#64748b", "Wet Time", wetTimeStr)
            ].join("");

            const container = document.getElementById('history-container');
            container.innerHTML = `
                <div class="grid-charts">
                    <div class="card" style="background: var(--card-bg); border: 1px solid var(--card-border); border-radius: 16px; padding: 1.15rem; backdrop-filter: var(--glass-blur);">
                        <div class="hist-card-header">
                            <div class="hist-card-title-row">
                                <h3>Temperature, Feels Like &amp; Humidity</h3>
                            </div>
                            <div class="hist-stats-strip">${tempBadges}</div>
                        </div>
                        <div id="chart-hist-temp" style="width: 100%;"></div>
                    </div>
                    <div class="card" style="background: var(--card-bg); border: 1px solid var(--card-border); border-radius: 16px; padding: 1.15rem; backdrop-filter: var(--glass-blur);">
                        <div class="hist-card-header">
                            <div class="hist-card-title-row">
                                <h3>Wind Trends &amp; Hourly Prevailing Direction</h3>
                            </div>
                            <div class="hist-stats-strip">${windBadges}</div>
                        </div>
                        <div id="chart-hist-wind" style="width: 100%;"></div>
                    </div>
                    <div class="card" style="background: var(--card-bg); border: 1px solid var(--card-border); border-radius: 16px; padding: 1.15rem; backdrop-filter: var(--glass-blur);">
                        <div class="hist-card-header">
                            <div class="hist-card-title-row">
                                <h3>Atmospheric, Solar &amp; Dew Point</h3>
                            </div>
                            <div class="hist-stats-strip">${envBadges}</div>
                        </div>
                        <div id="chart-hist-env" style="width: 100%;"></div>
                    </div>
                    <div class="card" style="background: var(--card-bg); border: 1px solid var(--card-border); border-radius: 16px; padding: 1.15rem; backdrop-filter: var(--glass-blur);">
                        <div class="hist-card-header">
                            <div class="hist-card-title-row">
                                <h3>Precipitation (Rate &amp; Cumulative)</h3>
                            </div>
                            <div class="hist-stats-strip">${precipBadges}</div>
                        </div>
                        <div id="chart-hist-precip" style="width: 100%;"></div>
                    </div>
                </div>`;

            const cols = window.innerWidth < 850 ? 1 : 2;
            const cardWidth = Math.floor((container.clientWidth - (cols > 1 ? 16 : 0)) / cols) - 38;
            const width = Math.max(280, cardWidth);

            const sharedCursor = {
                sync: { key: histSync.key, setSeries: false },
                drag: { x: true, y: false },
                points: { size: 5.5, width: 1.5 }
            };

            const sharedHooks = {
                setScale: [
                    (u, key) => {
                        if (key === 'x') {
                            syncHistoryScaleX(u, u.scales.x.min, u.scales.x.max);
                        }
                    }
                ]
            };

            const timeSeriesCfg = {
                label: "Time",
                value: formatHistLegendTime
            };

            // 1. Temp / Feels Like / Hum Chart
            const dataTemp = [historyData.time, tempArr, feelsLikeArr, historyData.hum];
            const optsTemp = {
                width: width,
                height: 245,
                cursor: sharedCursor,
                hooks: sharedHooks,
                scales: {
                    y: {
                        range: (u, dataMin, dataMax) => {
                            if (dataMin == null || dataMax == null) return uTemp === 'F' ? [32, 86] : [0, 30];
                            let pad = Math.max(1.5, (dataMax - dataMin) * 0.16);
                            return [Math.floor(dataMin - pad), Math.ceil(dataMax + pad)];
                        }
                    },
                    y2: { range: [0, 100] }
                },
                axes: [
                    { stroke: "#8c9cb2", grid: { stroke: "rgba(255,255,255,0.035)" } },
                    { label: `Temp / Feels °${uTemp}`, stroke: "#ff3b30", size: 48, grid: { stroke: "rgba(255,255,255,0.035)" } },
                    { scale: "y2", label: "Hum %", stroke: "#34c759", size: 44, side: 1, grid: { show: false } }
                ],
                series: [
                    timeSeriesCfg,
                    {
                        label: "Temp",
                        stroke: "#ff3b30",
                        fill: "rgba(255, 59, 48, 0.07)",
                        width: 2,
                        points: { show: false },
                        value: (u, v) => v == null ? "  --  " : v.toFixed(1) + `°${uTemp}`
                    },
                    {
                        label: "Feels",
                        stroke: "#ff9f0a",
                        dash: [5, 3],
                        width: 1.75,
                        points: { show: false },
                        value: (u, v) => v == null ? "  --  " : v.toFixed(1) + `°${uTemp}`
                    },
                    {
                        scale: "y2",
                        label: "Hum",
                        stroke: "#34c759",
                        width: 1.75,
                        points: { show: false },
                        value: (u, v) => v == null ? " -- " : Math.round(v) + "%"
                    }
                ]
            };
            historyCharts.temp = new uPlot(optsTemp, dataTemp, document.getElementById('chart-hist-temp'));

            // 2. Wind / Gust / Hourly Prevailing Direction Ribbon Chart
            const dataWind = [historyData.time, windArr, gustArr, historyData.wind_dir];
            const optsWind = {
                width: width,
                height: 245,
                cursor: sharedCursor,
                hooks: {
                    setScale: sharedHooks.setScale,
                    draw: [drawWindDirectionRibbon]
                },
                scales: {
                    // Extra top headroom (1.35x) so wind gust peaks stay cleanly below the top hourly direction ribbon
                    y: { range: (u, dataMin, dataMax) => [0, Math.max(8, Math.ceil((dataMax || 0) * 1.35))] },
                    y2: { range: [0, 360] }
                },
                axes: [
                    { stroke: "#8c9cb2", grid: { stroke: "rgba(255,255,255,0.035)" } },
                    { label: `Wind ${uWind}`, stroke: "#00e5ff", size: 48, grid: { stroke: "rgba(255,255,255,0.035)" } },
                    {
                        scale: "y2",
                        label: "Dir",
                        stroke: "#c084fc",
                        size: 44,
                        side: 1,
                        splits: () => [0, 90, 180, 270, 360],
                        values: (u, splits) => splits.map(v => v === 0 || v === 360 ? "N" : v === 90 ? "E" : v === 180 ? "S" : "W"),
                        grid: { show: false }
                    }
                ],
                series: [
                    timeSeriesCfg,
                    {
                        label: "Speed",
                        stroke: "#00e5ff",
                        fill: "rgba(0, 229, 255, 0.07)",
                        width: 2,
                        points: { show: false },
                        value: (u, v) => v == null ? "  --  " : v.toFixed(windDec) + ` ${uWind}`
                    },
                    {
                        label: "Gust",
                        stroke: "#ffd600",
                        width: 1.5,
                        points: { show: false },
                        value: (u, v) => v == null ? "  --  " : v.toFixed(windDec) + ` ${uWind}`
                    },
                    {
                        scale: "y2",
                        label: "Dir",
                        stroke: "#c084fc",
                        width: 0,
                        points: { show: true, size: 3.2, fill: "rgba(192, 132, 252, 0.32)", stroke: "rgba(192, 132, 252, 0.45)" },
                        paths: () => null,
                        value: (u, v) => v == null ? "   --   " : `${Math.round(v)}° ${degToCardinal(v)}`
                    }
                ]
            };
            historyCharts.wind = new uPlot(optsWind, dataWind, document.getElementById('chart-hist-wind'));

            // 3. Atmospheric Pressure / Solar / Dew Point Chart
            const dataEnv = [historyData.time, pressArr, historyData.solar, dewPointArr];
            const optsEnv = {
                width: width,
                height: 245,
                cursor: sharedCursor,
                hooks: sharedHooks,
                scales: {
                    y: {
                        range: (u, dataMin, dataMax) => {
                            if (dataMin == null || dataMax == null) {
                                return uPress === 'inHg' ? [28.9, 30.7] : (uPress === 'mmHg' ? [735, 780] : [980, 1040]);
                            }
                            if (uPress === 'inHg') {
                                let pad = Math.max(0.12, (dataMax - dataMin) * 0.25);
                                return [parseFloat((dataMin - pad).toFixed(2)), parseFloat((dataMax + pad).toFixed(2))];
                            }
                            let pad = Math.max(uPress === 'mmHg' ? 3 : 4, (dataMax - dataMin) * 0.25);
                            return [Math.floor(dataMin - pad), Math.ceil(dataMax + pad)];
                        }
                    },
                    y2: {
                        range: (u, dataMin, dataMax) => [0, Math.max(200, Math.ceil((dataMax || 0) * 1.12))]
                    },
                    y3: {
                        range: (u, dataMin, dataMax) => {
                            if (dataMin == null || dataMax == null) return uTemp === 'F' ? [20, 77] : [-5, 25];
                            let pad = Math.max(2, (dataMax - dataMin) * 0.2);
                            return [Math.floor(dataMin - pad), Math.ceil(dataMax + pad)];
                        }
                    }
                },
                axes: [
                    { stroke: "#8c9cb2", grid: { stroke: "rgba(255,255,255,0.035)" } },
                    { label: `Pressure ${uPress}`, stroke: "#af52de", size: 52, grid: { stroke: "rgba(255,255,255,0.035)" } },
                    { scale: "y2", label: "Solar W/m²", stroke: "#ff9100", size: 48, side: 1, grid: { show: false } }
                ],
                series: [
                    timeSeriesCfg,
                    {
                        label: "Press",
                        stroke: "#af52de",
                        width: 2,
                        points: { show: false },
                        value: (u, v) => v == null ? "   --   " : v.toFixed(pressDec) + ` ${uPress}`
                    },
                    {
                        scale: "y2",
                        label: "Solar",
                        stroke: "#ff9100",
                        fill: "rgba(255, 145, 0, 0.08)",
                        width: 1.75,
                        points: { show: false },
                        value: (u, v) => v == null ? "  --  " : Math.round(v) + " W/m²"
                    },
                    {
                        scale: "y3",
                        label: "Dew Pt",
                        stroke: "#38bdf8",
                        dash: [5, 3],
                        width: 1.75,
                        points: { show: false },
                        value: (u, v) => v == null ? "  --  " : v.toFixed(1) + `°${uTemp}`
                    }
                ]
            };
            historyCharts.env = new uPlot(optsEnv, dataEnv, document.getElementById('chart-hist-env'));

            // 4. Precipitation (Rate & Cumulative) Chart
            const dataPrecip = [historyData.time, precipRateArr, historyPrecipCml];
            const optsPrecip = {
                width: width,
                height: 245,
                cursor: sharedCursor,
                hooks: sharedHooks,
                scales: {
                    y: {
                        range: (u, dataMin, dataMax) => {
                            const minTop = (uPrecip === 'in') ? 0.25 : 5;
                            const top = Math.max(minTop, (dataMax || 0) * 1.25);
                            return [0, uPrecip === 'in' ? parseFloat(top.toFixed(2)) : Math.ceil(top)];
                        }
                    },
                    y2: {
                        range: (u, dataMin, dataMax) => {
                            const minTop = (uPrecip === 'in') ? 0.1 : 2;
                            const top = Math.max(minTop, (dataMax || 0) * 1.2);
                            return [0, uPrecip === 'in' ? parseFloat(top.toFixed(2)) : Math.ceil(top)];
                        }
                    }
                },
                axes: [
                    { stroke: "#8c9cb2", grid: { stroke: "rgba(255,255,255,0.035)" } },
                    { label: `Rate ${uPrecip}/h`, stroke: "#0088ff", size: 48, grid: { stroke: "rgba(255,255,255,0.035)" } },
                    { scale: "y2", label: `24h Rain ${uPrecip}`, stroke: "#00d2ff", size: 48, side: 1, grid: { show: false } }
                ],
                series: [
                    timeSeriesCfg,
                    {
                        label: "Rate",
                        stroke: "#0088ff",
                        width: 2,
                        points: { show: false },
                        value: (u, v) => v == null ? "  --  " : v.toFixed(precipRateDec) + ` ${uPrecip}/h`
                    },
                    {
                        scale: "y2",
                        label: "24h Rain",
                        stroke: "#00d2ff",
                        fill: "rgba(0, 210, 255, 0.08)",
                        width: 2,
                        points: { show: false },
                        value: (u, v) => v == null ? "  --  " : v.toFixed(precipCmlDec) + ` ${uPrecip}`
                    }
                ]
            };
            historyCharts.precip = new uPlot(optsPrecip, dataPrecip, document.getElementById('chart-hist-precip'));

            resizeHistoryCharts();
        }

        function resizeHistoryCharts() {
            const container = document.getElementById('history-container');
            if (container && historyCharts.temp) {
                const cols = window.innerWidth < 850 ? 1 : 2;
                const cardWidth = Math.floor((container.clientWidth - (cols > 1 ? 16 : 0)) / cols) - 38;
                const width = Math.max(280, cardWidth);
                if (historyCharts.temp) historyCharts.temp.setSize({ width: width, height: 235 });
                if (historyCharts.wind) historyCharts.wind.setSize({ width: width, height: 235 });
                if (historyCharts.env) historyCharts.env.setSize({ width: width, height: 235 });
                if (historyCharts.precip) historyCharts.precip.setSize({ width: width, height: 235 });
            }
        }

        window.addEventListener('resize', () => {
            if (historyCharts.temp && document.getElementById('page-history').style.display === 'block') {
                resizeHistoryCharts();
            }
        });

        setInterval(updateStats, 20000);
        setInterval(updatePackets, 3000);
        setInterval(tickScanEta, 1000);
        updateAdaptiveWeatherPanelState();
        updateMeshtasticPanelState();
        updateMeshcorePanelState();
        updateStats();
        updatePackets();
        updateLiveChart();

        setInterval(() => {
            if (document.getElementById('page-dashboard').style.display === 'block') {
                updateLiveChart();
            }
        }, 20000);

        setInterval(() => {
            if (document.getElementById('page-history').style.display === 'block') {
                initHistoryCharts();
            }
        }, 300000);

        function showPage(pageId) {
            document.querySelectorAll('.page-view').forEach(p => p.style.display = 'none');
            document.querySelectorAll('.nav-link').forEach(l => l.classList.remove('active'));
            document.getElementById('page-' + pageId).style.display = 'block';
            document.getElementById('nav-' + pageId).classList.add('active');
            if (pageId === 'history') {
                initHistoryCharts();
            }
        }

        // OTA Handlers
        const fileInput = document.getElementById('ota-file');
        const otaStatus = document.getElementById('ota-status');
        const progressContainer = document.getElementById('progress-container');
        const progressFill = document.getElementById('progress-fill');

        fileInput.addEventListener('change', (e) => {
            const file = e.target.files[0];
            if (!file) return;

            otaStatus.textContent = "Uploading firmware (0%)...";
            otaStatus.style.color = "var(--text-main)";
            progressContainer.style.display = "block";
            progressFill.style.width = "0%";

            let currentProgress = 0;
            let targetProgress = 0;

            const progressTimer = setInterval(() => {
                if (currentProgress < targetProgress) {
                    currentProgress += Math.max(0.5, (targetProgress - currentProgress) * 0.2);
                    if (currentProgress > 98) currentProgress = 98;
                    progressFill.style.width = currentProgress.toFixed(1) + "%";
                    otaStatus.textContent = `Uploading: ${Math.round(currentProgress)}%`;
                }
            }, 50);

            const xhr = new XMLHttpRequest();
            xhr.open('POST', '/ota', true);

            xhr.upload.onprogress = (event) => {
                if (event.lengthComputable) {
                    targetProgress = (event.loaded / event.total) * 100;
                    if (targetProgress >= 100) {
                        targetProgress = 98;
                    }
                }
            };

            xhr.onload = () => {
                clearInterval(progressTimer);
                if (xhr.status === 200) {
                    progressFill.style.width = "100%";
                    otaStatus.textContent = "Success! Device is rebooting...";
                    otaStatus.style.color = "var(--accent-green)";
                    setTimeout(() => {
                        window.location.reload();
                    }, 5000);
                } else {
                    otaStatus.textContent = "Update failed: " + xhr.responseText;
                    otaStatus.style.color = "var(--accent-red)";
                }
            };

            xhr.onerror = () => {
                clearInterval(progressTimer);
                otaStatus.textContent = "Connection error during update.";
                otaStatus.style.color = "var(--accent-red)";
            };

            xhr.send(file);
        });

        let uplotInstance = null;
        function renderLiveChartData(liveData, recreate = false) {
            const container = document.getElementById('chart-container');
            if (!container) return;
            const legendWidth = container.clientWidth > 650 ? 250 : 0;
            const width = Math.max(300, container.clientWidth - legendWidth);

            const u = window.g_units || {};
            const uTemp = u.temp || 'C';
            const uWind = u.wind || 'm/s';
            const uPrecip = u.precip || 'mm';
            const uPress = u.press || 'hPa';
            const windDec = (uWind === 'bft') ? 0 : 1;
            const pressDec = (uPress === 'inHg') ? 2 : 1;
            const precipDec = (uPrecip === 'in') ? 2 : 1;

            const defaultRaw = {
                time: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23].map(h => Math.floor(Date.now()/1000) - (24 - h)*3600),
                temp: [15.5, 15.0, 14.8, 14.2, 13.8, 13.5, 13.2, 14.0, 15.2, 17.0, 18.5, 20.0, 21.5, 22.0, 22.2, 21.8, 20.5, 19.2, 18.0, 17.2, 16.8, 16.5, 16.0, 15.8],
                wind_speed: [2.5, 2.8, 3.0, 2.4, 2.0, 1.8, 2.2, 3.5, 4.0, 4.8, 5.5, 6.0, 6.2, 5.8, 5.0, 4.5, 4.2, 3.8, 3.2, 2.8, 2.5, 2.2, 2.0, 2.4],
                wind_dir: [180, 190, 200, 210, 220, 230, 240, 250, 260, 270, 280, 290, 300, 310, 320, 330, 340, 350, 360, 10, 20, 30, 40, 50],
                wind_gust: [4.5, 5.0, 5.5, 4.8, 3.8, 3.5, 4.0, 6.2, 7.5, 8.8, 9.8, 11.0, 11.5, 10.5, 9.2, 8.0, 7.8, 6.8, 5.8, 5.2, 4.8, 4.2, 3.8, 4.5],
                humidity: [85, 88, 90, 92, 95, 96, 94, 88, 80, 72, 65, 58, 52, 50, 48, 52, 58, 65, 72, 78, 82, 84, 85, 86],
                pressure: [1012.5, 1012.2, 1011.8, 1011.5, 1011.0, 1010.8, 1011.2, 1011.5, 1012.0, 1012.5, 1012.8, 1013.2, 1013.5, 1013.2, 1012.8, 1012.5, 1012.2, 1012.5, 1012.8, 1013.0, 1013.2, 1013.5, 1013.8, 1013.5],
                solar_rad: [0, 0, 0, 0, 0, 0, 50, 150, 300, 500, 650, 800, 900, 850, 750, 600, 450, 250, 100, 10, 0, 0, 0, 0],
                precip_rate: [0, 0, 0, 0.5, 1.2, 2.5, 1.8, 0.8, 0.2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
            };

            const src = (liveData && liveData.time && liveData.time.length > 0) ? liveData : defaultRaw;
            const finalData = [
                src.time,
                convertArr(src.temp, convertTempFromC, 1),
                convertArr(src.wind_speed, convertWindFromMs, windDec),
                src.wind_dir,
                convertArr(src.wind_gust, convertWindFromMs, windDec),
                src.humidity,
                convertArr(src.pressure, convertPressFromHpa, pressDec),
                src.solar_rad,
                convertArr(src.precip_rate, convertPrecipFromMm, precipDec)
            ];

            const opts = {
                width: width,
                height: 280,
                legend: {
                    show: true,
                },
                scales: {
                    y2: {
                        range: (u, dataMin, dataMax) => [0, Math.max(10, Math.ceil((dataMax || 0) * 1.15))],
                    },
                    humidity: {
                        range: [0, 100],
                    },
                    pressure: {
                        range: (u, dataMin, dataMax) => {
                            if (dataMin == null || dataMax == null) {
                                return uPress === 'inHg' ? [28.9, 30.7] : (uPress === 'mmHg' ? [735, 780] : [980, 1040]);
                            }
                            if (uPress === 'inHg') {
                                let pad = Math.max(0.15, (dataMax - dataMin) * 0.25);
                                return [parseFloat((dataMin - pad).toFixed(2)), parseFloat((dataMax + pad).toFixed(2))];
                            }
                            let pad = Math.max(5, (dataMax - dataMin) * 0.25);
                            return [Math.floor(dataMin - pad), Math.ceil(dataMax + pad)];
                        },
                    },
                    solar_rad: {
                        range: (u, dataMin, dataMax) => [0, Math.max(200, Math.ceil((dataMax || 0) * 1.1))],
                    },
                    precip_rate: {
                        range: (u, dataMin, dataMax) => {
                            const minTop = (uPrecip === 'in') ? 0.25 : 5;
                            const top = Math.max(minTop, (dataMax || 0) * 1.25);
                            return [0, uPrecip === 'in' ? parseFloat(top.toFixed(2)) : Math.ceil(top)];
                        },
                    }
                },
                axes: [
                    {
                        stroke: "#8c9cb2",
                        grid: { stroke: "rgba(255, 255, 255, 0.05)" }
                    },
                    {
                        label: `Temp °${uTemp}`,
                        stroke: "#ff3b30",
                        grid: { stroke: "rgba(255, 255, 255, 0.05)" }
                    },
                    {
                        scale: 'y2',
                        label: `Wind ${uWind}`,
                        stroke: "#00e5ff",
                        side: 1,
                        grid: {
                            show: false,
                        }
                    }
                ],
                series: [
                    {},
                    {
                        label: `Temp °${uTemp}`,
                        stroke: "#ff3b30",
                        width: 2,
                    },
                    {
                        label: `Wind Speed ${uWind}`,
                        stroke: "#00e5ff",
                        scale: 'y2',
                        width: 2,
                    },
                    {
                        label: "Wind dir °",
                        stroke: "#00f5a0",
                        auto: false,
                        points: { show: false },
                        paths: (u, seriesIdx, fromIdx, toIdx) => {
                            let series = u.series[seriesIdx];
                            let times = u.data[0];
                            let speeds = u.data[2];
                            let dirs = u.data[3];

                            u.ctx.save();
                            u.ctx.beginPath();

                            for (let i = fromIdx; i <= toIdx; i++) {
                                if (speeds[i] == null) continue;

                                let xPx = u.valToPos(times[i], 'x', true);
                                let yPx = u.valToPos(speeds[i], 'y2', true);

                                let len = 15;
                                let theta = (dirs[i] - 90) * (Math.PI / 180);
                                let dx = len * Math.cos(theta);
                                let dy = len * Math.sin(theta);

                                u.ctx.moveTo(xPx, yPx);
                                u.ctx.lineTo(xPx + dx, yPx + dy);
                            }

                            u.ctx.strokeStyle = series.stroke();
                            u.ctx.stroke();
                            u.ctx.restore();
                        },
                    },
                    {
                        label: `Wind Gust ${uWind}`,
                        stroke: "#ffd600",
                        scale: 'y2',
                        width: 2,
                    },
                    {
                        label: "Humidity %",
                        stroke: "#34c759",
                        scale: 'humidity',
                        width: 2,
                    },
                    {
                        label: `Pressure ${uPress}`,
                        stroke: "#af52de",
                        scale: 'pressure',
                        width: 2,
                    },
                    {
                        label: "Solar Rad W/m²",
                        stroke: "#ff9100",
                        scale: 'solar_rad',
                        width: 2,
                    },
                    {
                        label: `Precip Rate ${uPrecip}/h`,
                        stroke: "#0088ff",
                        scale: 'precip_rate',
                        width: 2,
                    }
                ]
            };

            if (recreate && uplotInstance) {
                uplotInstance.destroy();
                uplotInstance = null;
            }

            if (uplotInstance) {
                uplotInstance.setSize({ width: width, height: 280 });
                uplotInstance.setData(finalData);
            } else {
                uplotInstance = new uPlot(opts, finalData, container);
            }
        }

        function updateLiveChart() {
            fetch('/live_trends')
                .then(res => res.json())
                .then(liveData => {
                    window.g_lastLiveTrendsData = liveData;
                    renderLiveChartData(liveData, false);
                })
                .catch(err => {
                    console.error("Error fetching live trends:", err);
                    renderLiveChartData(null, false);
                });
        }

        window.addEventListener('resize', () => {
            if (uplotInstance && document.getElementById('page-dashboard').style.display === 'block') {
                const container = document.getElementById('chart-container');
                if (container) {
                    const width = Math.max(320, Math.min(container.clientWidth - 20, 800));
                    uplotInstance.setSize({ width: width, height: 400 });
                }
            }
        });
    </script>
</body>
</html>
)html";

#ifndef CONFIG_SCREENLESS
// The SD CSV viewer intentionally does all parsing and chart rendering in the
// browser. The gateway only serves this static page, the CSV bytes, and the
// already-bundled uPlot assets, keeping ESP32 CPU and RAM use negligible.
static const char g_sd_csv_viewer_html[] = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>WeatherXM SD CSV Viewer</title>
    <link rel="stylesheet" href="/uPlot.min.css">
    <script src="/uPlot.iife.min.js"></script>
    <style>
        :root {
            color-scheme: dark;
            --bg: #0b0f19;
            --panel: #141c29;
            --panel-2: #101722;
            --card-bg: rgba(255, 255, 255, 0.03);
            --card-border: rgba(255, 255, 255, 0.08);
            --glass-blur: blur(12px);
            --border: #2a3647;
            --text: #f1f3f9;
            --muted: #8c9cb2;
            --cyan: #00d2ff;
            --green: #00f5a0;
            --yellow: #ffcf33;
            --red: #ff416c;
        }
        * { box-sizing: border-box; }
        body {
            margin: 0;
            min-width: 320px;
            background: radial-gradient(circle at top left, #102637 0, var(--bg) 36rem);
            color: var(--text);
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
        }
        .page { width: min(1500px, 100%); margin: 0 auto; padding: 1.25rem; }
        .topbar { display: flex; gap: 1rem; justify-content: space-between; align-items: flex-start; flex-wrap: wrap; }
        h1 { margin: 0; color: var(--cyan); font-size: clamp(1.35rem, 3vw, 2rem); }
        #filename { margin-top: .35rem; color: var(--muted); font: .9rem ui-monospace, monospace; overflow-wrap: anywhere; }
        .actions { display: flex; flex-wrap: wrap; align-items: center; gap: .55rem; }
        button, .button {
            appearance: none;
            border: 1px solid #376276;
            border-radius: 8px;
            padding: .55rem .85rem;
            background: rgba(0, 210, 255, .12);
            color: var(--cyan);
            cursor: pointer;
            font-family: inherit;
            font-size: .85rem;
            font-weight: 600;
            text-decoration: none;
        }
        button:hover, .button:hover { background: rgba(0, 210, 255, .23); }
        .sync-badge {
            font-size: 0.72rem;
            color: var(--muted);
            background: rgba(255, 255, 255, 0.04);
            border: 1px solid rgba(255, 255, 255, 0.08);
            padding: 0.35rem 0.65rem;
            border-radius: 999px;
        }
        .status { margin: 1rem 0; padding: .8rem 1rem; border: 1px solid var(--border); border-radius: 10px; background: var(--panel-2); color: var(--muted); }
        .status.error { border-color: rgba(255, 65, 108, .55); color: var(--red); }
        .summary { display: flex; flex-wrap: wrap; flex-wrap: balance; gap: .6rem; margin: 1rem 0; }
        .chip { padding: .5rem .75rem; border: 1px solid var(--border); border-radius: 999px; background: var(--panel); color: var(--muted); font-size: .84rem; }
        .chip strong { color: var(--text); }
        .charts { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 1rem; margin: 1rem 0; }
        .chart-card {
            min-width: 0;
            padding: 1.15rem;
            border: 1px solid var(--card-border);
            border-radius: 16px;
            background: var(--card-bg);
            backdrop-filter: var(--glass-blur);
        }
        .hist-card-header {
            display: flex;
            flex-direction: column;
            gap: 0.4rem;
            margin-bottom: 0.55rem;
        }
        .hist-card-title-row {
            display: flex;
            align-items: center;
            justify-content: space-between;
            gap: 0.5rem;
            flex-wrap: wrap;
        }
        .hist-card-title-row h3 {
            margin: 0;
            font-size: 0.98rem;
            font-weight: 700;
            letter-spacing: -0.01em;
            color: var(--text);
        }
        .hist-stats-strip {
            display: flex;
            flex-wrap: wrap;
            flex-wrap: balance;
            gap: 0.35rem 0.5rem;
            align-items: center;
            min-height: 22px;
        }
        .hist-stat-pill {
            display: inline-flex;
            align-items: center;
            gap: 0.3rem;
            font-size: 0.69rem;
            line-height: 1.2;
            padding: 0.16rem 0.45rem;
            border-radius: 6px;
            background: rgba(255, 255, 255, 0.035);
            border: 1px solid rgba(255, 255, 255, 0.07);
            color: var(--muted);
            font-variant-numeric: tabular-nums;
            white-space: nowrap;
        }
        .hist-stat-pill .dot {
            width: 7px;
            height: 7px;
            border-radius: 50%;
            flex-shrink: 0;
        }
        .hist-stat-pill strong {
            color: var(--text);
            font-weight: 600;
        }
        #charts .uplot { font-family: inherit; }
        #charts .uplot canvas { display: block; }
        #charts .uplot .u-legend {
            display: flex !important;
            flex-wrap: nowrap !important;
            align-items: center !important;
            justify-content: flex-start !important;
            gap: 0.45rem !important;
            font-size: 0.72rem !important;
            font-variant-numeric: tabular-nums !important;
            padding: 0.2rem 0.3rem !important;
            margin: 0.25rem 0 0 0 !important;
            height: 26px !important;
            min-height: 26px !important;
            max-height: 26px !important;
            white-space: nowrap !important;
            overflow-x: auto !important;
            overflow-y: hidden !important;
            scrollbar-width: none !important;
        }
        #charts .uplot .u-legend::-webkit-scrollbar {
            display: none !important;
        }
        #charts .uplot .u-legend .u-series {
            display: inline-flex !important;
            align-items: center !important;
            flex-shrink: 0 !important;
            padding: 0 !important;
            margin: 0 !important;
        }
        #charts .uplot .u-legend .u-series th,
        #charts .uplot .u-legend .u-series td {
            padding: 0 2px !important;
            white-space: nowrap !important;
        }
        #charts .uplot .u-legend .u-marker {
            width: 8px !important;
            height: 8px !important;
            border-radius: 2px !important;
            margin-right: 3px !important;
        }
        #charts .uplot .u-legend .u-value {
            display: inline-block !important;
            min-width: 5.2ch !important;
            text-align: left !important;
            font-variant-numeric: tabular-nums !important;
            color: var(--text) !important;
            font-weight: 600 !important;
        }
        #charts .uplot .u-legend .u-series:first-child .u-value {
            min-width: 9.6ch !important;
            color: var(--cyan) !important;
        }
        .table-panel { border: 1px solid var(--border); border-radius: 12px; background: var(--panel); overflow: hidden; }
        .table-tools { display: flex; align-items: center; gap: .7rem; flex-wrap: wrap; padding: .8rem; border-bottom: 1px solid var(--border); }
        input[type="search"] { flex: 1 1 260px; min-width: 0; padding: .6rem .75rem; border: 1px solid var(--border); border-radius: 8px; background: #090e16; color: var(--text); }
        label { color: var(--muted); font-size: .85rem; }
        .table-wrap { max-height: 68vh; overflow: auto; }
        table { width: 100%; border-collapse: separate; border-spacing: 0; font-size: .78rem; }
        th { position: sticky; top: 0; z-index: 2; padding: .65rem .6rem; background: #1c2736; color: var(--cyan); text-align: left; white-space: nowrap; border-bottom: 1px solid #3b4a5d; }
        td { padding: .55rem .6rem; white-space: nowrap; border-bottom: 1px solid rgba(255, 255, 255, .055); }
        tbody tr:nth-child(even) { background: rgba(255, 255, 255, .025); }
        tbody tr:hover { background: rgba(0, 210, 255, .08); }
        td.number { text-align: right; font-variant-numeric: tabular-nums; }
        td.empty { color: #566579; }
        td.quality-ok { color: var(--green); font-weight: 700; }
        td.quality-warn { color: var(--yellow); background: rgba(255, 207, 51, .08); font-weight: 700; }
        tr.quality-warn-row td:first-child { box-shadow: inset 3px 0 var(--yellow); }
        .hidden { display: none !important; }
        @media (max-width: 850px) { .charts { grid-template-columns: 1fr; } .page { padding: .8rem; } }
    </style>
</head>
<body>
    <main class="page">
        <div class="topbar">
            <div>
                <h1>SD CSV Viewer</h1>
                <div id="filename">Loading…</div>
            </div>
            <div class="actions">
                <span id="sync-badge" class="sync-badge hidden">Crosshair &amp; Zoom Linked</span>
                <button type="button" id="reset-zoom-button" class="hidden">Reset Zoom</button>
                <button type="button" id="close-button">Close tab</button>
                <a class="button" id="download-button" href="#">Download CSV</a>
            </div>
        </div>
        <div id="status" class="status">Reading CSV from the SD card…</div>
        <section id="summary" class="summary hidden"></section>
        <section id="charts" class="charts hidden" aria-label="Weather charts"></section>
        <section id="table-panel" class="table-panel hidden">
            <div class="table-tools">
                <input id="search" type="search" placeholder="Filter rows…" aria-label="Filter CSV rows">
                <label><input id="newest-first" type="checkbox"> Newest first</label>
                <span id="visible-count" class="chip"></span>
            </div>
            <div class="table-wrap">
                <table>
                    <thead id="table-head"></thead>
                    <tbody id="table-body"></tbody>
                </table>
            </div>
        </section>
    </main>
    <script>
        'use strict';
        const query = new URLSearchParams(location.search);
        const fileName = query.get('file') || '';
        const statusEl = document.getElementById('status');
        const summaryEl = document.getElementById('summary');
        const chartsEl = document.getElementById('charts');
        const tablePanel = document.getElementById('table-panel');
        const searchEl = document.getElementById('search');
        const newestFirstEl = document.getElementById('newest-first');
        const visibleCountEl = document.getElementById('visible-count');
        const resetZoomBtn = document.getElementById('reset-zoom-button');
        const syncBadgeEl = document.getElementById('sync-badge');

        let csvCharts = {};
        let csvTimeRange = null;
        let isSyncingCsvZoom = false;
        let headers = [];
        let sourceRows = [];
        let activeUnits = {
            temp_id: 0,
            wind_id: 0,
            precip_id: 0,
            press_id: 0,
            temp: 'C',
            wind: 'm/s',
            precip: 'mm',
            press: 'hPa'
        };

        document.getElementById('filename').textContent = fileName || 'No CSV file selected';
        document.getElementById('download-button').href = '/sd/download?file=' + encodeURIComponent(fileName);
        document.getElementById('close-button').addEventListener('click', () => {
            window.close();
            setTimeout(() => history.back(), 50);
        });
        resetZoomBtn.addEventListener('click', resetCsvZoom);

        function parseCsv(text) {
            const parsed = [];
            let row = [];
            let field = '';
            let quoted = false;
            for (let i = 0; i < text.length; i++) {
                const ch = text[i];
                if (quoted) {
                    if (ch === '"' && text[i + 1] === '"') {
                        field += '"';
                        i++;
                    } else if (ch === '"') {
                        quoted = false;
                    } else {
                        field += ch;
                    }
                } else if (ch === '"') {
                    quoted = true;
                } else if (ch === ',') {
                    row.push(field);
                    field = '';
                } else if (ch === '\n') {
                    row.push(field.endsWith('\r') ? field.slice(0, -1) : field);
                    if (row.some(value => value !== '')) parsed.push(row);
                    row = [];
                    field = '';
                } else {
                    field += ch;
                }
            }
            if (field !== '' || row.length) {
                row.push(field.endsWith('\r') ? field.slice(0, -1) : field);
                if (row.some(value => value !== '')) parsed.push(row);
            }
            if (parsed.length) parsed[0][0] = parsed[0][0].replace(/^\uFEFF/, '');
            return parsed;
        }

        function numericValue(value) {
            if (value == null || value.trim() === '') return null;
            const number = Number(value);
            return Number.isFinite(number) ? number : null;
        }

        function rowHasWarning(row) {
            const flagIndex = headers.findIndex(name => name === 'QualityFlags' || name === 'QualityFlagsHex');
            if (flagIndex < 0) return false;
            const value = String(row[flagIndex] || '').trim().toUpperCase();
            return value !== '' && value !== 'OK' && value !== '0' && value !== '0X00000000';
        }

        function formatTableHeader(header) {
            const uTemp = activeUnits.temp || 'C';
            const uWind = activeUnits.wind || 'm/s';
            const uPrecip = activeUnits.precip || 'mm';
            const uPress = activeUnits.press || 'hPa';
            if (header === 'Temp_Avg_C') return `Temp_Avg_${uTemp}`;
            if (header === 'WindSpeed_Avg_m_s') return `WindSpeed_Avg_${uWind}`;
            if (header === 'WindGust_Max_m_s') return `WindGust_Max_${uWind}`;
            if (header === 'PrecipRate_Avg_mm_h') return `PrecipRate_Avg_${uPrecip}/h`;
            if (header === 'PrecipAccum_mm') return `PrecipAccum_${uPrecip}`;
            if (header === 'Pressure_Avg_hPa') return `Pressure_Avg_${uPress}`;
            return header;
        }

        function formatTableCell(header, rawValue) {
            if (rawValue == null || rawValue === '') return '';
            const num = numericValue(rawValue);
            if (num === null) return String(rawValue);
            const uWind = activeUnits.wind || 'm/s';
            const uPrecip = activeUnits.precip || 'mm';
            const uPress = activeUnits.press || 'hPa';
            const windDec = (uWind === 'bft') ? 0 : 1;
            const pressDec = (uPress === 'inHg') ? 2 : 1;
            const precipRateDec = (uPrecip === 'in') ? 2 : 1;
            if (header === 'Temp_Avg_C') {
                const c = convertTempFromC(num);
                return c == null ? String(rawValue) : c.toFixed(1);
            }
            if (header === 'WindSpeed_Avg_m_s' || header === 'WindGust_Max_m_s') {
                const w = convertWindFromMs(num);
                return w == null ? String(rawValue) : w.toFixed(windDec);
            }
            if (header === 'PrecipRate_Avg_mm_h') {
                const p = convertPrecipFromMm(num);
                return p == null ? String(rawValue) : p.toFixed(precipRateDec);
            }
            if (header === 'PrecipAccum_mm') {
                const p = convertPrecipFromMm(num);
                return p == null ? String(rawValue) : p.toFixed(2);
            }
            if (header === 'Pressure_Avg_hPa') {
                const pr = convertPressFromHpa(num);
                return pr == null ? String(rawValue) : pr.toFixed(pressDec);
            }
            return String(rawValue);
        }

        function renderTable() {
            const needle = searchEl.value.trim().toLowerCase();
            let rows = sourceRows.filter(row => !needle || row.some(value => String(value).toLowerCase().includes(needle)));
            if (newestFirstEl.checked) rows = rows.slice().reverse();

            const headRow = document.createElement('tr');
            headers.forEach(header => {
                const th = document.createElement('th');
                th.textContent = formatTableHeader(header);
                headRow.appendChild(th);
            });
            document.getElementById('table-head').replaceChildren(headRow);

            const fragment = document.createDocumentFragment();
            rows.forEach(row => {
                const tr = document.createElement('tr');
                const warning = rowHasWarning(row);
                if (warning) tr.classList.add('quality-warn-row');
                headers.forEach((header, index) => {
                    const td = document.createElement('td');
                    const rawValue = row[index] == null ? '' : String(row[index]);
                    const value = formatTableCell(header, rawValue);
                    td.textContent = value || '—';
                    if (value === '') td.classList.add('empty');
                    if (numericValue(rawValue) !== null) td.classList.add('number');
                    if (header === 'QualityFlags' || header === 'QualityFlagsHex') {
                        td.classList.add(warning ? 'quality-warn' : 'quality-ok');
                    }
                    tr.appendChild(td);
                });
                fragment.appendChild(tr);
            });
            document.getElementById('table-body').replaceChildren(fragment);
            visibleCountEl.textContent = rows.length + ' of ' + sourceRows.length + ' rows';
        }

        function columnValues(name) {
            const index = headers.indexOf(name);
            return index < 0 ? null : sourceRows.map(row => numericValue(row[index]));
        }

        const csvSync = (typeof uPlot !== 'undefined' && uPlot.sync) ? uPlot.sync('csvWeather') : { key: 'csvWeather' };
        const CARDINALS_16 = ["N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"];

        function degToCardinal(deg) {
            if (deg == null || isNaN(deg)) return "--";
            const idx = Math.round((((deg % 360) + 360) % 360) / 22.5) % 16;
            return CARDINALS_16[idx];
        }

        function calcCsvDewPoint(t, rh) {
            if (t == null || rh == null || rh <= 0) return null;
            const a = 17.625, b = 243.04;
            const c = Math.log(Math.max(1, rh) / 100.0) + (a * t) / (b + t);
            return parseFloat(((b * c) / (a - c)).toFixed(1));
        }

        function calcCsvFeelsLike(t, w, rh) {
            if (t == null) return null;
            if (t <= 10.0 && w != null) {
                const vKmh = w * 3.6;
                if (vKmh > 4.8) {
                    const vPow = Math.pow(vKmh, 0.16);
                    return parseFloat((13.12 + 0.6215 * t - 11.37 * vPow + 0.3965 * t * vPow).toFixed(1));
                }
                return parseFloat(t.toFixed(1));
            }
            if (t > 26.7 && rh != null) {
                const hi = -8.78469475556 + 1.61139411 * t + 2.33854883889 * rh
                    - 0.14611605 * t * rh - 0.012308094 * t * t - 0.0164248277778 * rh * rh
                    + 0.002211732 * t * t * rh + 0.00072546 * t * rh * rh - 0.000003582 * t * t * rh * rh;
                return parseFloat(hi.toFixed(1));
            }
            return parseFloat(t.toFixed(1));
        }

        function msToBft(ms) {
            if (ms == null || isNaN(ms)) return null;
            if (ms < 0.5) return 0;
            if (ms < 1.6) return 1;
            if (ms < 3.4) return 2;
            if (ms < 5.5) return 3;
            if (ms < 8.0) return 4;
            if (ms < 10.8) return 5;
            if (ms < 13.9) return 6;
            if (ms < 17.2) return 7;
            if (ms < 20.8) return 8;
            if (ms < 24.5) return 9;
            if (ms < 28.5) return 10;
            if (ms < 32.7) return 11;
            return 12;
        }

        function convertTempFromC(valC) {
            if (valC == null || isNaN(valC)) return null;
            if (activeUnits.temp_id === 1 || activeUnits.temp === 'F') {
                return valC * 1.8 + 32.0;
            }
            return valC;
        }

        function convertWindFromMs(valMs) {
            if (valMs == null || isNaN(valMs)) return null;
            switch (Number(activeUnits.wind_id)) {
                case 1: return valMs * 3.6;
                case 2: return valMs * 2.236936;
                case 3: return valMs * 3.28084;
                case 4: return msToBft(valMs);
                case 5: return valMs * 1.943844;
                case 0:
                default: return valMs;
            }
        }

        function convertPrecipFromMm(valMm) {
            if (valMm == null || isNaN(valMm)) return null;
            if (activeUnits.precip_id === 1 || activeUnits.precip === 'in') {
                return valMm * 0.03937008;
            }
            return valMm;
        }

        function convertPressFromHpa(valHpa) {
            if (valHpa == null || isNaN(valHpa)) return null;
            switch (Number(activeUnits.press_id)) {
                case 1: return valHpa * 0.7500616;
                case 2: return valHpa * 0.02952998;
                case 0:
                default: return valHpa;
            }
        }

        function convertArr(arr, fn, decimals = 2) {
            if (!Array.isArray(arr)) return arr;
            const factor = Math.pow(10, decimals);
            return arr.map(v => {
                if (v == null || isNaN(v)) return null;
                const conv = fn(v);
                return conv == null ? null : Math.round(conv * factor) / factor;
            });
        }

        function calcSeriesStats(arr, decimals = 1) {
            if (!arr || !arr.length) return null;
            let min = Infinity, max = -Infinity, sum = 0, count = 0;
            for (let i = 0; i < arr.length; i++) {
                const v = arr[i];
                if (v != null && !isNaN(v)) {
                    if (v < min) min = v;
                    if (v > max) max = v;
                    sum += v;
                    count++;
                }
            }
            if (!count) return null;
            return {
                min: min.toFixed(decimals),
                max: max.toFixed(decimals),
                avg: (sum / count).toFixed(decimals),
                count: count
            };
        }

        function calcCircularWindMean(dirs, speeds) {
            if (!dirs || !dirs.length) return null;
            let sinSum = 0, cosSum = 0, count = 0;
            for (let i = 0; i < dirs.length; i++) {
                const d = dirs[i];
                if (d == null || isNaN(d)) continue;
                const spd = (speeds && speeds[i] != null && !isNaN(speeds[i])) ? Math.max(0.15, speeds[i]) : 1.0;
                const rad = (d * Math.PI) / 180.0;
                sinSum += Math.sin(rad) * spd;
                cosSum += Math.cos(rad) * spd;
                count++;
            }
            if (!count || (Math.abs(sinSum) < 1e-6 && Math.abs(cosSum) < 1e-6)) return null;
            const deg = Math.round(((Math.atan2(sinSum, cosSum) * 180.0) / Math.PI + 360.0) % 360.0);
            return { deg: deg, cardinal: degToCardinal(deg) };
        }

        function formatCsvLegendTime(u, ts) {
            if (ts == null || isNaN(ts)) return "--/-- --:--";
            const d = new Date(ts * 1000);
            const mo = String(d.getMonth() + 1).padStart(2, '0');
            const da = String(d.getDate()).padStart(2, '0');
            const hh = String(d.getHours()).padStart(2, '0');
            const mm = String(d.getMinutes()).padStart(2, '0');
            return `${mo}/${da} ${hh}:${mm}`;
        }

        function syncCsvScaleX(sourcePlot, min, max) {
            if (isSyncingCsvZoom) return;
            isSyncingCsvZoom = true;
            try {
                Object.values(csvCharts).forEach(plot => {
                    if (plot && plot !== sourcePlot) {
                        plot.setScale('x', { min: min, max: max });
                    }
                });
            } finally {
                isSyncingCsvZoom = false;
            }
        }

        function resetCsvZoom() {
            if (!csvTimeRange) return;
            isSyncingCsvZoom = true;
            try {
                Object.values(csvCharts).forEach(plot => {
                    if (plot) {
                        plot.setScale('x', { min: csvTimeRange[0], max: csvTimeRange[1] });
                    }
                });
            } finally {
                isSyncingCsvZoom = false;
            }
        }

        function drawWindDirectionRibbon(u) {
            const ctx = u.ctx;
            const { left, top, width, height } = u.bbox;
            if (width <= 40 || height <= 60) return;

            const pxRatio = (typeof uPlot !== 'undefined' && uPlot.pxRatio) ? uPlot.pxRatio : (window.devicePixelRatio || 1);
            const bandH = Math.round(34 * pxRatio);

            ctx.save();
            ctx.beginPath();
            ctx.rect(left, top, width, bandH + 2 * pxRatio);
            ctx.clip();

            ctx.fillStyle = "rgba(168, 85, 247, 0.08)";
            ctx.fillRect(left, top, width, bandH);
            ctx.strokeStyle = "rgba(168, 85, 247, 0.24)";
            ctx.lineWidth = Math.max(1, 0.8 * pxRatio);
            ctx.beginPath();
            ctx.moveTo(left, top + bandH);
            ctx.lineTo(left + width, top + bandH);
            ctx.stroke();

            const times = u.data[0];
            const speeds = u.data[1];
            const dirs = u.data[3];
            if (!times || !times.length || !dirs) {
                ctx.restore();
                return;
            }

            const xMin = u.scales.x.min != null ? u.scales.x.min : times[0];
            const xMax = u.scales.x.max != null ? u.scales.x.max : times[times.length - 1];
            const spanSec = Math.max(300, xMax - xMin);

            const cssWidth = width / pxRatio;
            const maxSlots = Math.max(4, Math.floor((cssWidth - 36) / 30));
            const candidateSteps = [900, 1800, 3600, 7200, 10800, 14400];
            let stepSec = 3600;
            for (const s of candidateSteps) {
                if (spanSec / s <= maxSlots) {
                    stepSec = s;
                    break;
                }
                stepSec = s;
            }

            const stepLabel = stepSec >= 3600 ? `${stepSec / 3600}H` : `${stepSec / 60}M`;
            ctx.font = `700 ${Math.round(8.5 * pxRatio)}px -apple-system, BlinkMacSystemFont, sans-serif`;
            ctx.fillStyle = "rgba(216, 180, 254, 0.72)";
            ctx.textAlign = "left";
            ctx.textBaseline = "middle";
            ctx.fillText(stepLabel, left + 5 * pxRatio, top + bandH * 0.5);

            const firstBucket = Math.floor(xMin / stepSec) * stepSec;
            let lastDrawnX = left + 26 * pxRatio;
            const minGapPx = 26 * pxRatio;

            for (let bStart = firstBucket; bStart <= xMax; bStart += stepSec) {
                const bEnd = bStart + stepSec;
                let sinSum = 0, cosSum = 0, spdSum = 0, tSum = 0, cnt = 0;
                for (let i = 0; i < times.length; i++) {
                    const t = times[i];
                    if (t < bStart || t >= bEnd || t < xMin || t > xMax) continue;
                    const d = dirs[i];
                    if (d == null || isNaN(d)) continue;
                    const s = (speeds && speeds[i] != null && !isNaN(speeds[i])) ? speeds[i] : 0.5;
                    const w = Math.max(0.15, s);
                    const rad = (d * Math.PI) / 180.0;
                    sinSum += Math.sin(rad) * w;
                    cosSum += Math.cos(rad) * w;
                    spdSum += s;
                    tSum += t;
                    cnt++;
                }
                if (!cnt) continue;

                const centerT = tSum / cnt;
                const cx = u.valToPos(centerT, 'x', true);
                if (cx < lastDrawnX || cx > left + width - 14 * pxRatio) continue;
                lastDrawnX = cx + minGapPx;

                const avgDeg = ((Math.atan2(sinSum, cosSum) * 180.0) / Math.PI + 360.0) % 360.0;
                const avgSpd = spdSum / cnt;
                const cardinal = degToCardinal(avgDeg);

                const flowCompass = (avgDeg + 180.0) % 360.0;
                const angleRad = ((flowCompass - 90.0) * Math.PI) / 180.0;

                const arrowCy = top + 11 * pxRatio;
                const arrowLen = 6.2 * pxRatio;
                const headLen = 4.2 * pxRatio;

                ctx.save();
                ctx.translate(cx, arrowCy);
                ctx.rotate(angleRad);

                const alpha = avgSpd < 0.2 ? 0.55 : Math.min(1.0, 0.72 + (avgSpd / 12.0) * 0.28);
                ctx.strokeStyle = `rgba(216, 180, 254, ${alpha.toFixed(2)})`;
                ctx.fillStyle = `rgba(216, 180, 254, ${alpha.toFixed(2)})`;
                ctx.lineWidth = 1.65 * pxRatio;
                ctx.lineCap = "round";
                ctx.lineJoin = "round";

                ctx.beginPath();
                ctx.moveTo(-arrowLen * 0.75, 0);
                ctx.lineTo(arrowLen * 0.75, 0);
                ctx.stroke();

                ctx.beginPath();
                ctx.moveTo(arrowLen * 0.85, 0);
                ctx.lineTo(arrowLen * 0.85 - headLen, -headLen * 0.68);
                ctx.lineTo(arrowLen * 0.85 - headLen, headLen * 0.68);
                ctx.closePath();
                ctx.fill();
                ctx.restore();

                ctx.font = `700 ${Math.round(9 * pxRatio)}px -apple-system, BlinkMacSystemFont, sans-serif`;
                ctx.fillStyle = avgSpd >= 3.0 ? "#f3e8ff" : "#d8b4fe";
                ctx.textAlign = "center";
                ctx.textBaseline = "middle";
                ctx.fillText(cardinal, cx, top + 25.5 * pxRatio);
            }

            ctx.restore();
        }

        function resizeCsvCharts() {
            if (!chartsEl || chartsEl.classList.contains('hidden')) return;
            const cols = window.innerWidth < 850 ? 1 : 2;
            const cardWidth = Math.floor((chartsEl.clientWidth - (cols > 1 ? 16 : 0)) / cols) - 38;
            const width = Math.max(280, cardWidth);
            Object.values(csvCharts).forEach(plot => {
                if (plot) plot.setSize({ width: width, height: 235 });
            });
        }

        function renderWeatherCharts() {
            Object.values(csvCharts).forEach(c => {
                if (c && typeof c.destroy === 'function') c.destroy();
            });
            csvCharts = {};

            const timestampIndex = headers.indexOf('UnixTimestamp');
            if (timestampIndex < 0 || sourceRows.length < 2 || typeof uPlot === 'undefined') return false;
            const ordered = sourceRows
                .map(row => ({ row, timestamp: numericValue(row[timestampIndex]) }))
                .filter(item => item.timestamp !== null)
                .sort((a, b) => a.timestamp - b.timestamp);
            if (ordered.length < 2) return false;
            sourceRows = ordered.map(item => item.row);
            const xValues = ordered.map(item => item.timestamp);

            const tempValsC = columnValues('Temp_Avg_C');
            const humVals = columnValues('Hum_Avg_pct');
            const windValsMs = columnValues('WindSpeed_Avg_m_s');
            const gustValsMs = columnValues('WindGust_Max_m_s');
            const windDirVals = columnValues('WindDir_Prevailing_deg');
            const precipRateValsMm = columnValues('PrecipRate_Avg_mm_h');
            const precipAccumValsMm = columnValues('PrecipAccum_mm');
            const pressValsHpa = columnValues('Pressure_Avg_hPa');
            const solarVals = columnValues('SolarRad_Avg_W_m2');

            const hasAnyWeather = [tempValsC, humVals, windValsMs, precipRateValsMm, pressValsHpa, solarVals]
                .some(arr => arr && arr.some(v => v !== null));
            if (!hasAnyWeather) return false;

            const n = xValues.length;
            csvTimeRange = [xValues[0], xValues[n - 1]];

            const uTemp = activeUnits.temp || 'C';
            const uWind = activeUnits.wind || 'm/s';
            const uPrecip = activeUnits.precip || 'mm';
            const uPress = activeUnits.press || 'hPa';
            const windDec = (uWind === 'bft') ? 0 : 1;
            const pressDec = (uPress === 'inHg') ? 2 : 1;
            const precipRateDec = (uPrecip === 'in') ? 2 : 1;
            const precipCmlDec = 2;

            const feelsLikeArrC = [];
            const dewPointArrC = [];
            const precipCmlMm = [];
            const sanitizedPrecipRateMm = [];
            let runningCmlMm = 0;
            let wetIntervals = 0;

            for (let i = 0; i < n; i++) {
                const t = tempValsC ? tempValsC[i] : null;
                const rh = humVals ? humVals[i] : null;
                const w = windValsMs ? windValsMs[i] : null;

                feelsLikeArrC.push(calcCsvFeelsLike(t, w, rh));
                dewPointArrC.push(calcCsvDewPoint(t, rh));

                const hasAccum = !!(precipAccumValsMm && precipAccumValsMm[i] != null && !isNaN(precipAccumValsMm[i]));
                let rawRate = (precipRateValsMm && precipRateValsMm[i] != null && !isNaN(precipRateValsMm[i])) ? precipRateValsMm[i] : null;
                if (rawRate != null && (rawRate < 0 || rawRate > 300)) {
                    rawRate = (hasAccum && precipAccumValsMm[i] >= 0 && precipAccumValsMm[i] <= 25)
                        ? (precipAccumValsMm[i] * 12)
                        : 0;
                }
                sanitizedPrecipRateMm.push(rawRate);

                let delta = hasAccum ? precipAccumValsMm[i] : null;
                if (delta == null && rawRate != null) {
                    delta = rawRate * (5 / 60);
                }
                if (delta != null && !isNaN(delta) && delta > 0) {
                    runningCmlMm += delta;
                    wetIntervals++;
                }
                precipCmlMm.push(parseFloat(runningCmlMm.toFixed(3)));
            }

            const tempArr = convertArr(tempValsC || new Array(n).fill(null), convertTempFromC, 1);
            const feelsLikeArr = convertArr(feelsLikeArrC, convertTempFromC, 1);
            const dewPointArr = convertArr(dewPointArrC, convertTempFromC, 1);
            const humArr = humVals || new Array(n).fill(null);
            const windArr = convertArr(windValsMs || new Array(n).fill(null), convertWindFromMs, windDec);
            const gustArr = convertArr(gustValsMs || new Array(n).fill(null), convertWindFromMs, windDec);
            const dirArr = windDirVals || new Array(n).fill(null);
            const pressArr = convertArr(pressValsHpa || new Array(n).fill(null), convertPressFromHpa, pressDec);
            const solarArr = solarVals || new Array(n).fill(null);
            const precipRateArr = convertArr(sanitizedPrecipRateMm, convertPrecipFromMm, precipRateDec);
            const precipCmlArr = convertArr(precipCmlMm, convertPrecipFromMm, precipCmlDec);

            const stTemp = calcSeriesStats(tempArr, 1);
            const stFeels = calcSeriesStats(feelsLikeArr, 1);
            const stHum = calcSeriesStats(humArr, 0);

            const stWind = calcSeriesStats(windArr, windDec);
            const stGust = calcSeriesStats(gustArr, windDec);
            const stDir = calcCircularWindMean(dirArr, windValsMs);

            const stPress = calcSeriesStats(pressArr, pressDec);
            const stSolar = calcSeriesStats(solarArr, 0);
            const stDew = calcSeriesStats(dewPointArr, 1);

            let pressTrendStr = "";
            if (pressArr && n >= 6) {
                const lookback = Math.max(0, n - 36);
                const pNow = pressArr[n - 1];
                let pPrev = null;
                for (let k = lookback; k < n - 1; k++) {
                    if (pressArr[k] != null && !isNaN(pressArr[k])) {
                        pPrev = pressArr[k];
                        break;
                    }
                }
                if (pNow != null && pPrev != null) {
                    const dp = pNow - pPrev;
                    const thresh = (uPress === 'inHg') ? 0.015 : (uPress === 'mmHg' ? 0.38 : 0.5);
                    const arrow = dp > thresh ? "↗" : (dp < -thresh ? "↘" : "→");
                    pressTrendStr = ` · ${arrow} ${dp >= 0 ? '+' : ''}${dp.toFixed(pressDec)}`;
                }
            }

            const stRate = calcSeriesStats(precipRateArr, precipRateDec);
            const totalRainVal = convertPrecipFromMm(runningCmlMm);
            const totalRain = (totalRainVal != null ? totalRainVal : 0).toFixed(precipCmlDec);
            const wetMinutes = wetIntervals * 5;
            const wetTimeStr = wetMinutes >= 60 ? `${Math.floor(wetMinutes / 60)}h ${wetMinutes % 60}m` : `${wetMinutes}m`;

            const pill = (color, label, text) =>
                `<span class="hist-stat-pill"><span class="dot" style="background:${color};"></span>${label}: <strong>${text}</strong></span>`;

            const tempBadges = [
                stTemp ? pill("#ff3b30", "Temp", `${stTemp.min}° / ${stTemp.avg}° / ${stTemp.max}°${uTemp}`) : "",
                stFeels ? pill("#ff9f0a", "Feels", `${stFeels.min}°–${stFeels.max}°${uTemp}`) : "",
                stHum ? pill("#34c759", "Hum", `${stHum.min}% / ${stHum.avg}% / ${stHum.max}%`) : ""
            ].join("");

            const windArrowDeg = stDir ? ((stDir.deg + 180) % 360) : 0;
            const windBadges = [
                stWind ? pill("#00e5ff", "Avg / Max", `${stWind.avg} / ${stWind.max} ${uWind}`) : "",
                stGust ? pill("#ffd600", "Peak Gust", `${stGust.max} ${uWind}`) : "",
                stDir ? pill("#c084fc", "Prevailing", `<span style="display:inline-block;transform:rotate(${windArrowDeg}deg);font-weight:700;">↑</span> ${stDir.cardinal} (${stDir.deg}°)`) : ""
            ].join("");

            const envBadges = [
                stPress ? pill("#af52de", "Press", `${stPress.min}–${stPress.max} ${uPress}${pressTrendStr}`) : "",
                stSolar ? pill("#ff9100", "Solar Peak / Avg", `${stSolar.max} / ${stSolar.avg} W/m²`) : "",
                stDew ? pill("#38bdf8", "Dew Pt", `${stDew.min}° / ${stDew.avg}° / ${stDew.max}°${uTemp}`) : ""
            ].join("");

            const precipBadges = [
                pill("#00d2ff", "Total Rain", `${totalRain} ${uPrecip}`),
                stRate ? pill("#0088ff", "Peak Rate", `${stRate.max} ${uPrecip}/h`) : "",
                pill("#64748b", "Wet Time", wetTimeStr)
            ].join("");

            // Participate in layout before measuring clientWidth
            chartsEl.classList.remove('hidden');
            resetZoomBtn.classList.remove('hidden');
            syncBadgeEl.classList.remove('hidden');

            chartsEl.innerHTML = `
                <article class="chart-card">
                    <div class="hist-card-header">
                        <div class="hist-card-title-row">
                            <h3>Temperature, Feels Like &amp; Humidity</h3>
                        </div>
                        <div class="hist-stats-strip">${tempBadges}</div>
                    </div>
                    <div id="csv-chart-temp"></div>
                </article>
                <article class="chart-card">
                    <div class="hist-card-header">
                        <div class="hist-card-title-row">
                            <h3>Wind Trends &amp; Hourly Prevailing Direction</h3>
                        </div>
                        <div class="hist-stats-strip">${windBadges}</div>
                    </div>
                    <div id="csv-chart-wind"></div>
                </article>
                <article class="chart-card">
                    <div class="hist-card-header">
                        <div class="hist-card-title-row">
                            <h3>Atmospheric, Solar &amp; Dew Point</h3>
                        </div>
                        <div class="hist-stats-strip">${envBadges}</div>
                    </div>
                    <div id="csv-chart-env"></div>
                </article>
                <article class="chart-card">
                    <div class="hist-card-header">
                        <div class="hist-card-title-row">
                            <h3>Precipitation (Rate &amp; Cumulative)</h3>
                        </div>
                        <div class="hist-stats-strip">${precipBadges}</div>
                    </div>
                    <div id="csv-chart-precip"></div>
                </article>`;

            const cols = window.innerWidth < 850 ? 1 : 2;
            const cardWidth = Math.floor((chartsEl.clientWidth - (cols > 1 ? 16 : 0)) / cols) - 38;
            const width = Math.max(280, cardWidth);

            const sharedCursor = {
                sync: { key: csvSync.key, setSeries: false },
                drag: { x: true, y: false },
                points: { size: 5.5, width: 1.5 }
            };

            const sharedHooks = {
                setScale: [
                    (u, key) => {
                        if (key === 'x') {
                            syncCsvScaleX(u, u.scales.x.min, u.scales.x.max);
                        }
                    }
                ]
            };

            const timeSeriesCfg = {
                label: "Time",
                value: formatCsvLegendTime
            };

            // 1. Temperature, Feels Like & Humidity
            csvCharts.temp = new uPlot({
                width: width,
                height: 235,
                cursor: sharedCursor,
                hooks: sharedHooks,
                scales: {
                    y: {
                        range: (u, dataMin, dataMax) => {
                            if (dataMin == null || dataMax == null) return uTemp === 'F' ? [32, 86] : [0, 30];
                            const pad = Math.max(1.5, (dataMax - dataMin) * 0.16);
                            return [Math.floor(dataMin - pad), Math.ceil(dataMax + pad)];
                        }
                    },
                    y2: { range: [0, 100] }
                },
                axes: [
                    { stroke: "#8c9cb2", grid: { stroke: "rgba(255,255,255,0.035)" } },
                    { label: `Temp / Feels °${uTemp}`, stroke: "#ff3b30", size: 48, grid: { stroke: "rgba(255,255,255,0.035)" } },
                    { scale: "y2", label: "Hum %", stroke: "#34c759", size: 44, side: 1, grid: { show: false } }
                ],
                series: [
                    timeSeriesCfg,
                    {
                        label: "Temp",
                        stroke: "#ff3b30",
                        fill: "rgba(255, 59, 48, 0.07)",
                        width: 2,
                        points: { show: false },
                        value: (u, v) => v == null ? "  --  " : v.toFixed(1) + `°${uTemp}`
                    },
                    {
                        label: "Feels",
                        stroke: "#ff9f0a",
                        dash: [5, 3],
                        width: 1.75,
                        points: { show: false },
                        value: (u, v) => v == null ? "  --  " : v.toFixed(1) + `°${uTemp}`
                    },
                    {
                        scale: "y2",
                        label: "Hum",
                        stroke: "#34c759",
                        width: 1.75,
                        points: { show: false },
                        value: (u, v) => v == null ? " -- " : Math.round(v) + "%"
                    }
                ]
            }, [xValues, tempArr, feelsLikeArr, humArr], document.getElementById('csv-chart-temp'));

            // 2. Wind Trends & Hourly Prevailing Direction Ribbon
            csvCharts.wind = new uPlot({
                width: width,
                height: 235,
                cursor: sharedCursor,
                hooks: {
                    setScale: sharedHooks.setScale,
                    draw: [drawWindDirectionRibbon]
                },
                scales: {
                    y: { range: (u, dataMin, dataMax) => [0, Math.max(8, Math.ceil((dataMax || 0) * 1.35))] },
                    y2: { range: [0, 360] }
                },
                axes: [
                    { stroke: "#8c9cb2", grid: { stroke: "rgba(255,255,255,0.035)" } },
                    { label: `Wind ${uWind}`, stroke: "#00e5ff", size: 48, grid: { stroke: "rgba(255,255,255,0.035)" } },
                    {
                        scale: "y2",
                        label: "Dir",
                        stroke: "#c084fc",
                        size: 44,
                        side: 1,
                        splits: () => [0, 90, 180, 270, 360],
                        values: (u, splits) => splits.map(v => v === 0 || v === 360 ? "N" : v === 90 ? "E" : v === 180 ? "S" : "W"),
                        grid: { show: false }
                    }
                ],
                series: [
                    timeSeriesCfg,
                    {
                        label: "Speed",
                        stroke: "#00e5ff",
                        fill: "rgba(0, 229, 255, 0.07)",
                        width: 2,
                        points: { show: false },
                        value: (u, v) => v == null ? "  --  " : v.toFixed(windDec) + ` ${uWind}`
                    },
                    {
                        label: "Gust",
                        stroke: "#ffd600",
                        width: 1.5,
                        points: { show: false },
                        value: (u, v) => v == null ? "  --  " : v.toFixed(windDec) + ` ${uWind}`
                    },
                    {
                        scale: "y2",
                        label: "Dir",
                        stroke: "#c084fc",
                        width: 0,
                        points: { show: true, size: 3.2, fill: "rgba(192, 132, 252, 0.32)", stroke: "rgba(192, 132, 252, 0.45)" },
                        paths: () => null,
                        value: (u, v) => v == null ? "   --   " : `${Math.round(v)}° ${degToCardinal(v)}`
                    }
                ]
            }, [xValues, windArr, gustArr, dirArr], document.getElementById('csv-chart-wind'));

            // 3. Atmospheric, Solar & Dew Point
            csvCharts.env = new uPlot({
                width: width,
                height: 235,
                cursor: sharedCursor,
                hooks: sharedHooks,
                scales: {
                    y: {
                        range: (u, dataMin, dataMax) => {
                            if (dataMin == null || dataMax == null) {
                                return uPress === 'inHg' ? [28.9, 30.7] : (uPress === 'mmHg' ? [735, 780] : [980, 1040]);
                            }
                            if (uPress === 'inHg') {
                                const pad = Math.max(0.12, (dataMax - dataMin) * 0.25);
                                return [parseFloat((dataMin - pad).toFixed(2)), parseFloat((dataMax + pad).toFixed(2))];
                            }
                            const pad = Math.max(uPress === 'mmHg' ? 3 : 4, (dataMax - dataMin) * 0.25);
                            return [Math.floor(dataMin - pad), Math.ceil(dataMax + pad)];
                        }
                    },
                    y2: {
                        range: (u, dataMin, dataMax) => [0, Math.max(200, Math.ceil((dataMax || 0) * 1.12))]
                    },
                    y3: {
                        range: (u, dataMin, dataMax) => {
                            if (dataMin == null || dataMax == null) return uTemp === 'F' ? [20, 77] : [-5, 25];
                            const pad = Math.max(2, (dataMax - dataMin) * 0.2);
                            return [Math.floor(dataMin - pad), Math.ceil(dataMax + pad)];
                        }
                    }
                },
                axes: [
                    { stroke: "#8c9cb2", grid: { stroke: "rgba(255,255,255,0.035)" } },
                    { label: `Pressure ${uPress}`, stroke: "#af52de", size: 52, grid: { stroke: "rgba(255,255,255,0.035)" } },
                    { scale: "y2", label: "Solar W/m²", stroke: "#ff9100", size: 48, side: 1, grid: { show: false } }
                ],
                series: [
                    timeSeriesCfg,
                    {
                        label: "Press",
                        stroke: "#af52de",
                        width: 2,
                        points: { show: false },
                        value: (u, v) => v == null ? "   --   " : v.toFixed(pressDec) + ` ${uPress}`
                    },
                    {
                        scale: "y2",
                        label: "Solar",
                        stroke: "#ff9100",
                        fill: "rgba(255, 145, 0, 0.08)",
                        width: 1.75,
                        points: { show: false },
                        value: (u, v) => v == null ? "  --  " : Math.round(v) + " W/m²"
                    },
                    {
                        scale: "y3",
                        label: "Dew Pt",
                        stroke: "#38bdf8",
                        dash: [5, 3],
                        width: 1.75,
                        points: { show: false },
                        value: (u, v) => v == null ? "  --  " : v.toFixed(1) + `°${uTemp}`
                    }
                ]
            }, [xValues, pressArr, solarArr, dewPointArr], document.getElementById('csv-chart-env'));

            // 4. Precipitation (Rate & Cumulative)
            csvCharts.precip = new uPlot({
                width: width,
                height: 235,
                cursor: sharedCursor,
                hooks: sharedHooks,
                scales: {
                    y: {
                        range: (u, dataMin, dataMax) => {
                            const minTop = (uPrecip === 'in') ? 0.25 : 5;
                            const top = Math.max(minTop, (dataMax || 0) * 1.25);
                            return [0, uPrecip === 'in' ? parseFloat(top.toFixed(2)) : Math.ceil(top)];
                        }
                    },
                    y2: {
                        range: (u, dataMin, dataMax) => {
                            const minTop = (uPrecip === 'in') ? 0.1 : 2;
                            const top = Math.max(minTop, (dataMax || 0) * 1.2);
                            return [0, uPrecip === 'in' ? parseFloat(top.toFixed(2)) : Math.ceil(top)];
                        }
                    }
                },
                axes: [
                    { stroke: "#8c9cb2", grid: { stroke: "rgba(255,255,255,0.035)" } },
                    { label: `Rate ${uPrecip}/h`, stroke: "#0088ff", size: 48, grid: { stroke: "rgba(255,255,255,0.035)" } },
                    { scale: "y2", label: `Total Rain ${uPrecip}`, stroke: "#00d2ff", size: 48, side: 1, grid: { show: false } }
                ],
                series: [
                    timeSeriesCfg,
                    {
                        label: "Rate",
                        stroke: "#0088ff",
                        width: 2,
                        points: { show: false },
                        value: (u, v) => v == null ? "  --  " : v.toFixed(precipRateDec) + ` ${uPrecip}/h`
                    },
                    {
                        scale: "y2",
                        label: "Total Rain",
                        stroke: "#00d2ff",
                        fill: "rgba(0, 210, 255, 0.08)",
                        width: 2,
                        points: { show: false },
                        value: (u, v) => v == null ? "  --  " : v.toFixed(precipCmlDec) + ` ${uPrecip}`
                    }
                ]
            }, [xValues, precipRateArr, precipCmlArr], document.getElementById('csv-chart-precip'));

            resizeCsvCharts();
            return true;
        }

        let isWeatherCharted = false;

        function renderSummaryChips() {
            summaryEl.innerHTML = '';
            const values = [
                ['Rows', sourceRows.length],
                ['Columns', headers.length],
                ['Type', isWeatherCharted ? 'Weather history' : 'CSV / daily metadata'],
                ['Units', `°${activeUnits.temp} · ${activeUnits.wind} · ${activeUnits.precip} · ${activeUnits.press}`],
                ['Processing', 'In this browser']
            ];
            values.forEach(([label, value]) => {
                const chip = document.createElement('span');
                chip.className = 'chip';
                const strong = document.createElement('strong');
                strong.textContent = label + ': ';
                chip.append(strong, document.createTextNode(String(value)));
                summaryEl.appendChild(chip);
            });
        }

        async function pollCsvUnits() {
            try {
                const r = await fetch('/api/v1/units', { cache: 'no-store' });
                if (!r.ok) return;
                const u = await r.json();
                if (!u || u.status !== 'ok') return;
                if (u.temp_id !== activeUnits.temp_id ||
                    u.wind_id !== activeUnits.wind_id ||
                    u.precip_id !== activeUnits.precip_id ||
                    u.press_id !== activeUnits.press_id) {
                    activeUnits = {
                        temp_id: u.temp_id,
                        wind_id: u.wind_id,
                        precip_id: u.precip_id,
                        press_id: u.press_id,
                        temp: u.temp || 'C',
                        wind: u.wind || 'm/s',
                        precip: u.precip || 'mm',
                        press: u.press || 'hPa'
                    };
                    if (sourceRows.length) {
                        isWeatherCharted = renderWeatherCharts();
                        renderSummaryChips();
                        renderTable();
                    }
                }
            } catch (e) {}
        }

        async function loadCsv() {
            if (!fileName || !fileName.toLowerCase().endsWith('.csv')) {
                throw new Error('Select a CSV file from the SD Card section.');
            }
            const [csvResp] = await Promise.all([
                fetch('/sd/download?file=' + encodeURIComponent(fileName), { cache: 'no-store' }),
                fetch('/api/v1/units', { cache: 'no-store' })
                    .then(r => r.ok ? r.json() : null)
                    .then(u => {
                        if (u && u.status === 'ok') {
                            activeUnits = {
                                temp_id: u.temp_id,
                                wind_id: u.wind_id,
                                precip_id: u.precip_id,
                                press_id: u.press_id,
                                temp: u.temp || 'C',
                                wind: u.wind || 'm/s',
                                precip: u.precip || 'mm',
                                press: u.press || 'hPa'
                            };
                        }
                    })
                    .catch(() => {})
            ]);
            if (!csvResp.ok) {
                const detail = (await csvResp.text()).trim();
                throw new Error(detail || ('CSV request failed with HTTP ' + csvResp.status));
            }
            const parsed = parseCsv(await csvResp.text());
            if (!parsed.length) throw new Error('The CSV file is empty.');
            headers = parsed[0].map((header, index) => header.trim() || ('Column ' + (index + 1)));
            sourceRows = parsed.slice(1).map(row => {
                const normalized = new Array(headers.length).fill('');
                for (let i = 0; i < Math.min(row.length, headers.length); i++) normalized[i] = row[i];
                return normalized;
            });

            isWeatherCharted = renderWeatherCharts();
            renderSummaryChips();
            summaryEl.classList.remove('hidden');
            tablePanel.classList.remove('hidden');
            statusEl.classList.add('hidden');
            renderTable();
            setInterval(pollCsvUnits, 3000);
        }

        searchEl.addEventListener('input', renderTable);
        newestFirstEl.addEventListener('change', renderTable);
        let resizeTimer;
        window.addEventListener('resize', () => {
            clearTimeout(resizeTimer);
            resizeTimer = setTimeout(resizeCsvCharts, 100);
        });

        loadCsv().catch(error => {
            statusEl.classList.remove('hidden');
            statusEl.classList.add('error');
            statusEl.textContent = 'Unable to view CSV: ' + error.message;
        });
    </script>
</body>
</html>
)html";
#endif

// Factored weather observation structure for /status, /api/v1/observations, etc.
struct WeatherObservationData {
  bool has_weather = false;
  uint32_t timestamp = 0;
  float temp = 0.0f;
  float feels_like = 0.0f;
  float humidity = 0.0f;
  float precip_rate = 0.0f;
  float precip_cml = NAN;
  float precip_daily = NAN;
  float precip_24h = NAN;
  float pressure = 0.0f;
  float wind_speed = 0.0f;
  float wind_gust = 0.0f;
  float wind_dir = 0.0f;
  float irradiance = 0.0f;
  uint8_t uv = 0;
  const char *wind_dir_cardinal = "";

  const char *unit_temp = "°C";
  const char *unit_wind = "m/s";
  const char *unit_precip = "mm";
  const char *unit_press = "hPa";

  char pressure_hpa_json[24];
  char temp_json[24];
  char feels_like_json[24];
  char humidity_json[24];
  char precip_rate_json[24];
  char precip_cml_json[24];
  char precip_daily_json[24];
  char precip_24h_json[24];
  char pressure_json[24];
  char wind_speed_json[24];
  char wind_gust_json[24];
  char wind_dir_json[24];
  char irradiance_json[24];
};

static WeatherObservationData extract_weather_data(const WsFields::FieldPack *pack, bool is_pack_valid) {
  WeatherObservationData obs;

  const auto *t_desc = Units::get_temp_unit_desc(nullptr, Settings::get_temp_unit());
  if (t_desc != nullptr) obs.unit_temp = t_desc->symbol;
  const auto *w_desc = Units::get_wind_speed_unit_desc(nullptr, Settings::get_wind_speed_unit());
  if (w_desc != nullptr) obs.unit_wind = w_desc->symbol;
  const auto *p_desc = Units::get_precip_unit_desc(nullptr, Settings::get_precip_unit());
  if (p_desc != nullptr) obs.unit_precip = p_desc->symbol;
  const auto *pr_desc = Units::get_press_unit_desc(nullptr, Settings::get_press_unit());
  if (pr_desc != nullptr) obs.unit_press = pr_desc->symbol;

  float raw_press_sensor = Pressure::get_latest_val();

  if (is_pack_valid && pack != nullptr) {
    obs.timestamp = pack->ts;
    float raw_temp = 0, raw_hum = 0, raw_wind_speed = 0, raw_wind_gust = 0, raw_wind_dir = 0, raw_irr = 0, raw_precip_rate = 0, raw_press = 0;
    uint8_t raw_uv = 0;

    bool temp_avail = WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(pack), WsFields::FieldType::WEATHER_TEMP, NULL, &raw_temp) == WsCommon::RetRes::OK;
    bool hum_avail = WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(pack), WsFields::FieldType::WEATHER_HUM, NULL, &raw_hum) == WsCommon::RetRes::OK;
    bool wind_speed_avail = WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(pack), WsFields::FieldType::WEATHER_WIND_SPEED, NULL, &raw_wind_speed) == WsCommon::RetRes::OK;
    bool wind_gust_avail = WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(pack), WsFields::FieldType::WEATHER_WIND_GUST, NULL, &raw_wind_gust) == WsCommon::RetRes::OK;
    bool wind_dir_avail = WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(pack), WsFields::FieldType::WEATHER_WIND_DIR, NULL, &raw_wind_dir) == WsCommon::RetRes::OK;
    bool irr_avail = WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(pack), WsFields::FieldType::WEATHER_SOLAR_RAD, NULL, &raw_irr) == WsCommon::RetRes::OK;
    bool uv_avail = WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(pack), WsFields::FieldType::WEATHER_UV_INDEX, NULL, &raw_uv) == WsCommon::RetRes::OK;
    bool precip_avail = WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(pack), WsFields::FieldType::WEATHER_PRECIP_RATE, NULL, &raw_precip_rate) == WsCommon::RetRes::OK;
    bool press_avail = WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(pack), WsFields::FieldType::WEATHER_PRESSURE, NULL, &raw_press) == WsCommon::RetRes::OK;

    if (!precip_avail) {
      raw_precip_rate = WsData::get_precip_rate();
      precip_avail = true;
    }
    if (!press_avail) {
      raw_press = raw_press_sensor;
      press_avail = (raw_press != -1.0f);
    }

    if (temp_avail) {
      obs.has_weather = true;
      obs.temp = Units::convert_temp(raw_temp, Settings::get_temp_unit());
      if (hum_avail) {
        obs.humidity = raw_hum;
        if (wind_speed_avail) {
          float raw_feels = Units::calculate_feels_like(raw_temp, raw_wind_speed, raw_hum);
          obs.feels_like = Units::convert_temp(raw_feels, Settings::get_temp_unit());
        } else {
          obs.feels_like = obs.temp;
        }
      } else {
        obs.feels_like = obs.temp;
      }
    }

    if (wind_speed_avail) {
      obs.wind_speed = Units::convert_wind_speed(raw_wind_speed, Settings::get_wind_speed_unit());
    }
    if (wind_gust_avail) {
      obs.wind_gust = Units::convert_wind_speed(raw_wind_gust, Settings::get_wind_speed_unit());
    }
    if (wind_dir_avail) {
      obs.wind_dir = raw_wind_dir;
      obs.wind_dir_cardinal = std::isfinite(raw_wind_dir) ? Units::wind_dir_to_cardinal((uint16_t)raw_wind_dir) : "";
    }
    if (irr_avail) {
      obs.irradiance = raw_irr;
    }
    if (uv_avail) {
      obs.uv = raw_uv;
    }
    if (precip_avail) {
      obs.precip_rate = Units::convert_precip(raw_precip_rate, Settings::get_precip_unit());
    }

    float raw_precip_cml = NAN;
    bool precip_cml_avail = WsFields::get_field_val_from_pack(const_cast<WsFields::FieldPack*>(pack), WsFields::FieldType::WEATHER_PRECIP_CML, NULL, &raw_precip_cml) == WsCommon::RetRes::OK;
    if (precip_cml_avail && std::isfinite(raw_precip_cml)) {
      obs.precip_cml = Units::convert_precip(raw_precip_cml, Settings::get_precip_unit());
    }

    obs.precip_daily = Units::convert_precip(WsData::get_daily_precip(), Settings::get_precip_unit());

    time_t now_ts = (obs.timestamp > 0) ? obs.timestamp : time(NULL);
    if (now_ts > 0) {
      time_t t_24h_ago = now_ts - 86400;
      auto aggs = WeatherLogger::get_aggregates();
      float p24 = 0.0f;
      bool has_p24 = false;
      for (const auto &agg : aggs) {
        if (agg.valid && agg.timestamp >= t_24h_ago && !isnan(agg.precip_accum)) {
          p24 += agg.precip_accum;
          has_p24 = true;
        }
      }
      if (has_p24) {
        obs.precip_24h = Units::convert_precip(p24, Settings::get_precip_unit());
      }
    }

    if (press_avail) {
      obs.pressure = Units::convert_press(raw_press, Settings::get_press_unit());
    }
  } else {
    if (raw_press_sensor != -1.0f) {
      obs.pressure = Units::convert_press(raw_press_sensor, Settings::get_press_unit());
    }
  }

  format_json_float(obs.pressure_hpa_json, sizeof(obs.pressure_hpa_json),
                    raw_press_sensor, 2);
  format_json_float(obs.temp_json, sizeof(obs.temp_json), obs.temp, 2);
  format_json_float(obs.feels_like_json, sizeof(obs.feels_like_json),
                    obs.feels_like, 2);
  format_json_float(obs.humidity_json, sizeof(obs.humidity_json), obs.humidity, 1);
  format_json_float(obs.precip_rate_json, sizeof(obs.precip_rate_json),
                    obs.precip_rate, 2);
  format_json_float(obs.precip_cml_json, sizeof(obs.precip_cml_json),
                    obs.precip_cml, 2);
  format_json_float(obs.precip_daily_json, sizeof(obs.precip_daily_json),
                    obs.precip_daily, 2);
  format_json_float(obs.precip_24h_json, sizeof(obs.precip_24h_json),
                    obs.precip_24h, 2);
  format_json_float(obs.pressure_json, sizeof(obs.pressure_json), obs.pressure, 2);
  format_json_float(obs.wind_speed_json, sizeof(obs.wind_speed_json),
                    obs.wind_speed, 2);
  format_json_float(obs.wind_gust_json, sizeof(obs.wind_gust_json),
                    obs.wind_gust, 2);
  format_json_float(obs.wind_dir_json, sizeof(obs.wind_dir_json), obs.wind_dir, 1);
  format_json_float(obs.irradiance_json, sizeof(obs.irradiance_json),
                    obs.irradiance, 2);

  return obs;
}

static WeatherObservationData extract_current_weather_data() {
  WsFields::FieldPack w_pack = {};
  bool has_pack = StateSniffer::get_latest_weather_pack(&w_pack);
  return extract_weather_data(has_pack ? &w_pack : nullptr, has_pack);
}

// GET /api/v1/info handler
static esp_err_t get_info_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();

  char json_buf[1536];

  const char *wifi_state_str = "DISCONNECTED";
  switch (Wifi::get_status()) {
  case Wifi::WifiStatus::CONNECTED:
    wifi_state_str = "CONNECTED";
    break;
  case Wifi::WifiStatus::CONNECTING:
    wifi_state_str = "CONNECTING";
    break;
  case Wifi::WifiStatus::CONNECTED_NO_IP:
    wifi_state_str = "CONNECTED_NO_IP";
    break;
  case Wifi::WifiStatus::CONNECTED_NO_INTERNET:
    wifi_state_str = "CONNECTED_NO_INTERNET";
    break;
  case Wifi::WifiStatus::CONNECTION_FAILED:
    wifi_state_str = "CONNECTION_FAILED";
    break;
  case Wifi::WifiStatus::DISCONNECTING:
    wifi_state_str = "DISCONNECTING";
    break;
  default:
    break;
  }

  int station_type = 0;
  if (WsHandler::get_pair_state() == WsHandler::PairState::PAIRED) {
    if (WsHandler::get_cur_station() != nullptr) {
      if (WsHandler::get_cur_station()->TYPE == WsType::WS_TYPE_1001) {
        station_type = 1001;
      } else if (WsHandler::get_cur_station()->TYPE == WsType::WS_TYPE_1300) {
        station_type = 1300;
      }
    } else {
      WsType active_t = (WsHandler::get_active_pair_source() == PairSource::PAIR_SOURCE_CLOUD)
                        ? Settings::get_cloud_ws_type()
                        : Settings::get_ws_type();
      if (active_t == WsType::WS_TYPE_1001) {
        station_type = 1001;
      } else if (active_t == WsType::WS_TYPE_1300) {
        station_type = 1300;
      }
    }
  } else if (WsHandler::is_scan_active() || Settings::get_pair_mode() == PairMode::PAIR_MODE_SCAN) {
    if (WsHandler::get_scan_profile() == WsType::WS_TYPE_1001) {
      station_type = 1001;
    } else if (WsHandler::get_scan_profile() == WsType::WS_TYPE_1300) {
      station_type = 1300;
    } else {
      station_type = 0;
    }
  } else if (WsHandler::g_receiver_profile == WsType::WS_TYPE_1001) {
    station_type = 1001;
  } else if (WsHandler::g_receiver_profile == WsType::WS_TYPE_1300) {
    station_type = 1300;
  }

  const char *freq_region_str = "-";
  const WsFreqDesc *freq_desc = WsHandler::get_freq_desc(NULL, Settings::get_freq_region());
  if (freq_desc != NULL) {
    freq_region_str = freq_desc->symbol;
  }

  char mac_str[20] = {0};
  Utils::get_mac_addr_str(mac_str, sizeof(mac_str));

  const char *u_temp = "°C";
  const char *u_wind = "m/s";
  const char *u_precip = "mm";
  const char *u_press = "hPa";
  const auto *t_desc = Units::get_temp_unit_desc(nullptr, Settings::get_temp_unit());
  if (t_desc != nullptr) u_temp = t_desc->symbol;
  const auto *w_desc = Units::get_wind_speed_unit_desc(nullptr, Settings::get_wind_speed_unit());
  if (w_desc != nullptr) u_wind = w_desc->symbol;
  const auto *p_desc = Units::get_precip_unit_desc(nullptr, Settings::get_precip_unit());
  if (p_desc != nullptr) u_precip = p_desc->symbol;
  const auto *pr_desc = Units::get_press_unit_desc(nullptr, Settings::get_press_unit());
  if (pr_desc != nullptr) u_press = pr_desc->symbol;

  const char *gw_id = MdnsDiscovery::get_stable_gateway_id();
  int64_t paired_id = (WsHandler::get_active_pair_source() == PairSource::PAIR_SOURCE_CLOUD)
                      ? Settings::get_cloud_ws_id()
                      : Settings::get_ws_id();
  uint8_t station_fw = WsHandler::g_ws1300_station_fw_ver;

  int len = snprintf(
      json_buf, sizeof(json_buf),
      "{"
      "\"id\":\"%s\","
      "\"gateway_id\":\"%s\","
      "\"model\":\"%s\","
      "\"hardware\":\"%s\","
      "\"firmware\":\"%s\","
      "\"api_version\":\"1.0.0\","
      "\"device_name\":\"%s\","
      "\"mac\":\"%s\","
      "\"uptime_sec\":%lu,"
      "\"edition\":\"%s\","
      "\"freq_region\":\"%s\","
      "\"station\":{"
        "\"is_paired\":%s,"
        "\"pair_mode\":\"%s\","
        "\"paired_id\":%lld,"
        "\"station_type\":%d,"
        "\"station_fw_version\":%u,"
        "\"packet_reception\":%u"
      "},"
      "\"wifi\":{"
        "\"status\":\"%s\","
        "\"ssid\":\"%s\","
        "\"rssi\":%d,"
        "\"ip\":\"%s\""
      "},"
      "\"mesh\":{"
        "\"meshtastic_enabled\":%s,"
        "\"meshcore_enabled\":%s"
      "},"
      "\"units\":{"
        "\"temp\":\"%s\","
        "\"wind\":\"%s\","
        "\"precip\":\"%s\","
        "\"press\":\"%s\""
      "}"
      "}",
      gw_id, gw_id,
      CONFIG_HW_NAME, CONFIG_HW_NAME,
      APP_VERSION,
      Settings::get_device_name(),
      mac_str,
      (unsigned long)Utils::get_tick_sec(),
      Settings::is_open_edition() ? "open" : "wxm",
      freq_region_str,
      (WsHandler::get_pair_state() == WsHandler::PairState::PAIRED) ? "true" : "false",
      Utils::pair_mode_to_str(Settings::get_pair_mode()),
      (long long)paired_id,
      station_type,
      station_fw,
      WsHandler::get_packet_reception(),
      wifi_state_str,
      Wifi::get_ssid() ? Wifi::get_ssid() : "",
      Wifi::get_rssi(),
      Wifi::get_ip_str(),
      Settings::get_mesh_env_telemetry_enabled() ? "true" : "false",
      Settings::get_meshcore_gw_telemetry_enabled() ? "true" : "false",
      u_temp, u_wind, u_precip, u_press
  );

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json_buf, len);
}

// GET /api/v1/observations (and /api/v1/observation) handler
static esp_err_t get_observations_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();

  char query[64] = {0};
  bool full_buffer = false;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char val[16] = {0};
    if (httpd_query_key_value(query, "buffer", val, sizeof(val)) == ESP_OK) {
      if (strcmp(val, "full") == 0) {
        full_buffer = true;
      }
    }
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  if (!full_buffer) {
    WeatherObservationData wx = extract_current_weather_data();
    char buf[1024];
    int len = snprintf(
        buf, sizeof(buf),
        "{"
        "\"timestamp\":%lu,"
        "\"has_weather\":%s,"
        "\"temp\":%s,"
        "\"feels_like\":%s,"
        "\"humidity\":%s,"
        "\"pressure\":%s,"
        "\"wind_speed\":%s,"
        "\"wind_gust\":%s,"
        "\"wind_dir_deg\":%s,"
        "\"wind_dir_cardinal\":\"%s\","
        "\"precip_rate\":%s,"
        "\"precip_daily\":%s,"
        "\"precip_cml\":%s,"
        "\"precip_24h\":%s,"
        "\"uv\":%u,"
        "\"irradiance\":%s,"
        "\"unit_temp\":\"%s\","
        "\"unit_wind\":\"%s\","
        "\"unit_precip\":\"%s\","
        "\"unit_press\":\"%s\","
        "\"units\":{"
          "\"temp\":\"%s\","
          "\"wind\":\"%s\","
          "\"precip\":\"%s\","
          "\"press\":\"%s\""
        "}"
        "}",
        (unsigned long)wx.timestamp,
        wx.has_weather ? "true" : "false",
        wx.temp_json,
        wx.feels_like_json,
        wx.humidity_json,
        wx.pressure_json,
        wx.wind_speed_json,
        wx.wind_gust_json,
        wx.wind_dir_json,
        wx.wind_dir_cardinal,
        wx.precip_rate_json,
        wx.precip_daily_json,
        wx.precip_cml_json,
        wx.precip_24h_json,
        wx.uv,
        wx.irradiance_json,
        wx.unit_temp, wx.unit_wind, wx.unit_precip, wx.unit_press,
        wx.unit_temp, wx.unit_wind, wx.unit_precip, wx.unit_press
    );
    return httpd_resp_send(req, buf, len);
  }

  // Full 15-minute rolling raw buffer
  auto raw_buf = WeatherLogger::get_raw_buffer();
  char header[48];
  snprintf(header, sizeof(header), "{\"count\":%u,\"buffer\":[", (unsigned int)raw_buf.size());
  httpd_resp_send_chunk(req, header, -1);

  for (size_t i = 0; i < raw_buf.size(); ++i) {
    WeatherObservationData wx = extract_weather_data(&raw_buf[i], true);
    char item[512];
    snprintf(
        item, sizeof(item),
        "{\"timestamp\":%lu,\"temp\":%s,\"feels_like\":%s,\"humidity\":%s,"
        "\"pressure\":%s,\"wind_speed\":%s,\"wind_gust\":%s,\"wind_dir_deg\":%s,"
        "\"wind_dir_cardinal\":\"%s\",\"precip_rate\":%s,\"precip_daily\":%s,\"precip_cml\":%s,\"precip_24h\":%s,\"uv\":%u,\"irradiance\":%s}%s",
        (unsigned long)raw_buf[i].ts,
        wx.temp_json,
        wx.feels_like_json,
        wx.humidity_json,
        wx.pressure_json,
        wx.wind_speed_json,
        wx.wind_gust_json,
        wx.wind_dir_json,
        wx.wind_dir_cardinal,
        wx.precip_rate_json,
        wx.precip_daily_json,
        wx.precip_cml_json,
        wx.precip_24h_json,
        wx.uv,
        wx.irradiance_json,
        (i == raw_buf.size() - 1) ? "" : ","
    );
    httpd_resp_send_chunk(req, item, -1);
  }

  httpd_resp_send_chunk(req, "]}", -1);
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// GET / status handler
static esp_err_t get_status_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();
  // Headroom matters here: a truncated /status is malformed JSON, which does not
  // fail loudly - it takes the entire dashboard down with a parse error while the
  // gateway looks perfectly healthy. Long SSIDs and device names push the tail of
  // this object around, so keep well clear of the limit and check the result.
  char json_buf[6144];

  const char *wifi_state_str = "DISCONNECTED";
  switch (Wifi::get_status()) {
  case Wifi::WifiStatus::CONNECTED:
    wifi_state_str = "CONNECTED";
    break;
  case Wifi::WifiStatus::CONNECTING:
    wifi_state_str = "CONNECTING";
    break;
  case Wifi::WifiStatus::CONNECTED_NO_IP:
    wifi_state_str = "CONNECTED_NO_IP";
    break;
  case Wifi::WifiStatus::CONNECTED_NO_INTERNET:
    wifi_state_str = "CONNECTED_NO_INTERNET";
    break;
  case Wifi::WifiStatus::CONNECTION_FAILED:
    wifi_state_str = "CONNECTION_FAILED";
    break;
  case Wifi::WifiStatus::DISCONNECTING:
    wifi_state_str = "DISCONNECTING";
    break;
  default:
    break;
  }

  const char *mqtt_state_str = "DISCONNECTED";
  switch (Tb::get_status()) {
  case Tb::MqttStatus::CONNECTED:
    mqtt_state_str = "CONNECTED";
    break;
  case Tb::MqttStatus::CONNECTING:
    mqtt_state_str = "CONNECTING";
    break;
  case Tb::MqttStatus::CONNECTION_FAILED:
    mqtt_state_str = "CONNECTION_FAILED";
    break;
  case Tb::MqttStatus::DISCONNECTING:
    mqtt_state_str = "DISCONNECTING";
    break;
  default:
    break;
  }

  int station_type = 0;
  if (WsHandler::get_pair_state() == WsHandler::PairState::PAIRED) {
    if (WsHandler::get_cur_station() != nullptr) {
      if (WsHandler::get_cur_station()->TYPE == WsType::WS_TYPE_1001) {
        station_type = 1001;
      } else if (WsHandler::get_cur_station()->TYPE == WsType::WS_TYPE_1300) {
        station_type = 1300;
      }
    } else {
      WsType active_t = (WsHandler::get_active_pair_source() == PairSource::PAIR_SOURCE_CLOUD)
                        ? Settings::get_cloud_ws_type()
                        : Settings::get_ws_type();
      if (active_t == WsType::WS_TYPE_1001) {
        station_type = 1001;
      } else if (active_t == WsType::WS_TYPE_1300) {
        station_type = 1300;
      }
    }
  } else if (WsHandler::is_scan_active() || Settings::get_pair_mode() == PairMode::PAIR_MODE_SCAN) {
    if (WsHandler::get_scan_profile() == WsType::WS_TYPE_1001) {
      station_type = 1001;
    } else if (WsHandler::get_scan_profile() == WsType::WS_TYPE_1300) {
      station_type = 1300;
    } else {
      station_type = 0;
    }
  } else if (WsHandler::g_receiver_profile == WsType::WS_TYPE_1001) {
    station_type = 1001;
  } else if (WsHandler::g_receiver_profile == WsType::WS_TYPE_1300) {
    station_type = 1300;
  } else {
    station_type = 0;
  }

#ifdef CONFIG_SCREENLESS
  bool is_screenless = true;
  bool lora_link_ok = KissClient::g_nrf52_uart_ok;
#else
  bool is_screenless = false;
  bool lora_link_ok = true; // SPI connection is successful
#endif

  uint32_t listen_freq_hz = 0;
  char listen_desc[32] = "-";
  WsHandler::get_listen_info(&listen_freq_hz, listen_desc, sizeof(listen_desc));

  uint32_t region_base_hz = 0;
  const WsFreqDesc *freq_desc =
      WsHandler::get_freq_desc(NULL, Settings::get_freq_region());
  if (freq_desc != NULL) {
    region_base_hz = freq_desc->freq_hz;
  }

  WeatherObservationData wx = extract_current_weather_data();

  int64_t c_ws_id = Settings::get_cloud_ws_id();
  int64_t l_ws_id = Settings::get_ws_id();
  bool c_ws_valid = Settings::get_cloud_ws_valid();
  bool cloud_profile_ready = Settings::cloud_ws_profile_ready();
  bool local_profile_ready = Settings::local_ws_profile_ready();
  bool local_profile_present = l_ws_id >= 0;
  int local_ws_type = Settings::get_ws_type() == WsType::WS_TYPE_1300 ? 1300 :
      (Settings::get_ws_type() == WsType::WS_TYPE_1001 ? 1001 : 0);
  int cloud_ws_type = Settings::get_cloud_ws_type() == WsType::WS_TYPE_1300 ? 1300 :
      (Settings::get_cloud_ws_type() == WsType::WS_TYPE_1001 ? 1001 : 0);
  bool is_paired = (WsHandler::get_pair_state() == WsHandler::PairState::PAIRED);
  bool c_ws_match = (local_profile_ready && c_ws_valid && l_ws_id == c_ws_id);
  PairSource active_source = WsHandler::get_active_pair_source();
  PairSource selected_source = Settings::get_pair_source();
  const char *active_source_str =
      active_source == PairSource::PAIR_SOURCE_CLOUD ? "cloud" :
      active_source == PairSource::PAIR_SOURCE_LOCAL ? "local" : "none";
  const char *selected_source_str =
      selected_source == PairSource::PAIR_SOURCE_CLOUD ? "cloud" :
      selected_source == PairSource::PAIR_SOURCE_LOCAL ? "local" : "none";

  uint8_t local_sn[WS_SN_LEN] = {};
  uint8_t cloud_sn[WS_SN_LEN] = {};
  char local_sn_hex[WS_SN_LEN * 2 + 1] = "";
  char cloud_sn_hex[WS_SN_LEN * 2 + 1] = "";
  Settings::get_ws_sn(local_sn, sizeof(local_sn));
  Settings::get_cloud_ws_sn(cloud_sn, sizeof(cloud_sn));
  Utils::buff_to_hex_str(local_sn, sizeof(local_sn), local_sn_hex, sizeof(local_sn_hex));
  Utils::buff_to_hex_str(cloud_sn, sizeof(cloud_sn), cloud_sn_hex, sizeof(cloud_sn_hex));

  uint8_t local_pk[WS_PUB_KEY_LEN] = {};
  uint8_t cloud_pk[WS_PUB_KEY_LEN] = {};
  Settings::get_ws_pubkey(local_pk, sizeof(local_pk));
  Settings::get_cloud_ws_pubkey(cloud_pk, sizeof(cloud_pk));
  bool local_has_pubkey = false;
  bool cloud_has_pubkey = false;
  for (size_t i = 0; i < WS_PUB_KEY_LEN; i++) {
    if (local_pk[i] != 0x00 && local_pk[i] != 0xFF) local_has_pubkey = true;
    if (cloud_pk[i] != 0x00 && cloud_pk[i] != 0xFF) cloud_has_pubkey = true;
  }
  bool local_sn_synthesized = (local_sn[4] == 0xAA && local_sn[5] == 0x55 &&
                               local_sn[6] == 0xAA && local_sn[7] == 0x55);
  bool cloud_sn_synthesized = (cloud_sn[4] == 0xAA && cloud_sn[5] == 0x55 &&
                               cloud_sn[6] == 0xAA && cloud_sn[7] == 0x55);

  WsHandler::ActivityStatus activity_status = WsHandler::get_activity_status();
  // This proves that traffic matching the configured target is being decoded.
  // It does not prove which gateway a WS1300 accepts responses from; keep the
  // legacy active_link field as an API alias and expose the precise name too.
  bool active_link = is_paired &&
      activity_status != WsHandler::ActivityStatus::NONE &&
      activity_status != WsHandler::ActivityStatus::INACTIVE;
  bool uplink_enabled = is_paired &&
      Settings::get_pair_mode() != PairMode::PAIR_MODE_OFF;
  bool uplink_active = uplink_enabled && active_link;

  const char *operation_mode = "listen";
  if (WsHandler::g_ws1300_kick_armed) {
    operation_mode = "repair";
  } else if (WsHandler::is_scan_active()) {
    operation_mode = "scan";
  } else if (Settings::get_pair_mode() == PairMode::PAIR_MODE_OFF) {
    operation_mode =
        (WsHandler::g_manual_listen_slot >= 0 || WsHandler::g_sniff_mode_enabled)
            ? "listen"
            : "off";
  } else if (!is_paired && WsHandler::g_manual_listen_slot >= 0) {
    operation_mode = "listen";
  } else if (Settings::get_pair_mode() == PairMode::PAIR_MODE_CLOUD) {
    operation_mode = "cloud";
  } else if (Settings::get_pair_mode() == PairMode::PAIR_MODE_LOCAL) {
    operation_mode = "local";
  }

  const char *ownership_status = "unverified";
  uint32_t paired_id = WsHandler::get_paired_id();
  bool nvs_confirmed = Settings::is_station_ownership_confirmed() &&
                       Settings::get_station_ownership_id() == paired_id;
  bool ownership_target_matches =
      (WsHandler::g_ownership_challenge_station_id == paired_id) || nvs_confirmed;
  if (Settings::get_pair_mode() == PairMode::PAIR_MODE_OFF) {
    ownership_status = "receive_only";
  } else if (WsHandler::g_ownership_confirmed && (WsHandler::g_ownership_challenge_station_id == paired_id || WsHandler::g_ownership_challenge_station_id == 0)) {
    ownership_status = "confirmed";
  } else if ((WsHandler::g_ownership_challenge_armed ||
              WsHandler::g_ownership_challenge_pending) &&
             (WsHandler::g_ownership_challenge_station_id == paired_id || WsHandler::g_ownership_challenge_station_id == 0)) {
    uint32_t ownership_now_sec = Utils::get_tick_sec();
    bool challenge_timed_out =
        WsHandler::g_ownership_challenge_started_sec != 0 &&
        ownership_now_sec - WsHandler::g_ownership_challenge_started_sec > 180;
    ownership_status = challenge_timed_out ? (nvs_confirmed ? "confirmed" : "failed") : "pending";
    if (challenge_timed_out) {
      WsHandler::g_ownership_challenge_pending = false;
    }
  } else if (nvs_confirmed) {
    ownership_status = "confirmed";
  } else if (WsHandler::g_ownership_challenge_started_sec != 0 &&
             (WsHandler::g_ownership_challenge_station_id == paired_id)) {
    ownership_status = "failed";
  }

  // Recently seen stations buffer. Serializing them here
  // never touches NVS; Cloud and Local remain the only persistent identities.
  WsHandler::RecentStation recent[RECENT_STATIONS_CAPACITY] = {};
  size_t recent_count = WsHandler::get_recent_stations(recent, RECENT_STATIONS_CAPACITY);
  char recent_json[1536] = "[";
  size_t recent_len = 1;
  uint32_t now_tick_sec = Utils::get_tick_sec();
  for (size_t i = 0; i < recent_count; i++) {
    char sn_hex[WS_SN_LEN * 2 + 1] = "";
    if (recent[i].has_sn) {
      Utils::buff_to_hex_str(recent[i].sn, sizeof(recent[i].sn),
                             sn_hex, sizeof(sn_hex));
    }
    char pubkey_hex[WS_PUB_KEY_LEN * 2 + 1] = "";
    if (recent[i].has_pubkey) {
      Utils::buff_to_hex_str(recent[i].pubkey, sizeof(recent[i].pubkey),
                             pubkey_hex, sizeof(pubkey_hex));
    }
    const char *packet_type = "DATA";
    if (recent[i].type == WsType::WS_TYPE_1300) {
      switch ((Ws1300::StaPacketType)recent[i].packet_type) {
      case Ws1300::StaPacketType::ADV: packet_type = "ADV"; break;
      case Ws1300::StaPacketType::BOOT: packet_type = "BOOT"; break;
      case Ws1300::StaPacketType::SENSORS: packet_type = "SENSOR"; break;
      case Ws1300::StaPacketType::GNSS: packet_type = "GNSS"; break;
      default: packet_type = "UNKNOWN"; break;
      }
    }
    uint32_t age_sec = now_tick_sec >= recent[i].last_seen_tick_sec
        ? now_tick_sec - recent[i].last_seen_tick_sec : 0;
    int written = snprintf(
        recent_json + recent_len, sizeof(recent_json) - recent_len,
        "%s{\"id\":%lu,\"type\":%d,\"sn\":\"%s\",\"has_sn\":%s,"
        "\"has_pubkey\":%s,\"pubkey\":\"%s\","
        "\"pairable\":%s,\"packet_type\":\"%s\",\"age_sec\":%lu,"
        "\"rssi\":%d,\"snr\":%d,\"channel_slot\":%u}",
        i == 0 ? "" : ",", (unsigned long)recent[i].id,
        recent[i].type == WsType::WS_TYPE_1300 ? 1300 : 1001,
        sn_hex, recent[i].has_sn ? "true" : "false",
        recent[i].has_pubkey ? "true" : "false", pubkey_hex,
        recent[i].pairable ? "true" : "false", packet_type,
        (unsigned long)age_sec, (int)recent[i].rssi, (int)recent[i].snr,
        (unsigned int)recent[i].channel_slot);
    if (written < 0 || (size_t)written >= sizeof(recent_json) - recent_len) {
      break;
    }
    recent_len += (size_t)written;
  }
  if (recent_len < sizeof(recent_json) - 1) {
    recent_json[recent_len++] = ']';
    recent_json[recent_len] = '\0';
  } else {
    snprintf(recent_json, sizeof(recent_json), "[]");
  }

  uint8_t scan_total_slots = 9;
  if (WsHandler::get_scan_profile() == WsType::WS_TYPE_1300) {
    scan_total_slots = 8;
  } else if (WsHandler::get_scan_profile() == WsType::WS_TYPE_1001) {
    scan_total_slots = 1;
  }

  const uint32_t uptime_sec = Utils::get_tick_sec();

  AdaptivePublish::WeatherChangePolicy wx_policy = Settings::get_weather_policy();
  uint16_t wx_policy_crc = AdaptivePublish::compute_crc16(wx_policy);
  MeshTelemetry::EvaluatorStatus mt_status = MeshTelemetry::get_evaluator_status(MeshTelemetry::MeshProtocol::MESHTASTIC);
  MeshTelemetry::EvaluatorStatus mc_status = MeshTelemetry::get_evaluator_status(MeshTelemetry::MeshProtocol::MESHCORE);

  bsp_sdcard_status_info_t sd_info = {};
#ifndef CONFIG_SCREENLESS
  bsp_sdcard_get_status_info(&sd_info);
  const bool sd_ejected = WeatherLogger::get_sd_status().ejected;
#else
  const bool sd_ejected = false;
#endif

  int status_len = snprintf(
      json_buf, sizeof(json_buf),
      "{\"wifi_status\":\"%s\",\"wifi_ssid\":\"%s\",\"wifi_rssi\":%d,"
      "\"uptime_sec\":%lu,"
      "\"mqtt_status\":\"%s\",\"mqtt_success_count\":%lu,\"mqtt_fail_count\":%lu,"
      "\"packet_reception\":%d,\"activity_status\":\"%s\",\"activity_duration_sec\":%lu,"
      "\"paired_id\":%lu,\"pressure_hpa\":%s,\"fw_version\":\"%s\",\"is_paired\":%s,\"edition\":\"%s\",\"is_open_edition\":%s,"
      "\"sniff_mode\":%s,\"autopair_enabled\":%s,\"cloud_pairing_enabled\":%s,"
      "\"pair_mode\":\"%s\",\"pair_source\":\"%s\",\"active_source\":\"%s\","
      "\"operation_mode\":\"%s\",\"pairing_state\":\"%s\",\"ownership_status\":\"%s\",\"ownership_packets_seen\":%u,"
      "\"ownership_confirmed_id\":%lu,\"ownership_confirmed_ts\":%lu,"
      "\"scan_target_id\":%lu,\"scan_target_slot\":%d,"
      "\"active_link\":%s,\"target_packets_seen\":%s,"
      "\"uplink_enabled\":%s,\"uplink_active\":%s,\"local_pair_sync_pending\":%s,"
      "\"local_ws_id\":%lld,\"local_ws_sn\":\"%s\",\"local_ws_type\":%d,\"local_ws_present\":%s,"
      "\"local_ws_valid\":%s,\"local_profile_ready\":%s,\"local_has_pubkey\":%s,\"local_sn_synthesized\":%s,"
      "\"cloud_ws_id\":%lld,\"cloud_ws_sn\":\"%s\",\"cloud_ws_type\":%d,\"cloud_ws_valid\":%s,\"cloud_profile_ready\":%s,\"cloud_ws_match\":%s,\"cloud_has_pubkey\":%s,\"cloud_sn_synthesized\":%s,"
      "\"fallback_phase\":\"%s\",\"fallback_sec_remaining\":%lu,"
      "\"scan_active\":%s,\"scan_slot\":%d,\"scan_total_slots\":%u,\"scan_eta_sec\":%lu,\"scan_total_sec\":%lu,\"scan_profile\":%d,"
      "\"scan_completed\":%s,\"scan_cycle\":%u,\"scan_phase\":\"%s\","
      "\"station_type\":%d,\"freq_region\":%d,\"mqtt_disabled\":%s,\"lora_link_ok\":%s,\"is_screenless\":%s,"
      "\"sd_mounted\":%s,\"sd_present\":%s,\"sd_ejected\":%s,\"sd_unusable\":%s,\"sd_capacity_gb\":%.2f,\"sd_fs_type\":\"%s\",\"sd_status_reason\":\"%s\","
      "\"listen_freq_hz\":%lu,\"listen_desc\":\"%s\",\"manual_slot\":%d,"
      "\"rx_packet_total\":%lu,"
      "\"region_base_hz\":%lu,\"tx_power_override\":%d,"
      "\"tx_interval_override\":%d,\"station_fw_ver\":%u,\"station_meshtastic_interval_min\":%u,\"station_meshtastic_preset\":%u,\"station_meshtastic_hop_limit\":%u,"
      "\"station_meshtastic_custom_freq_hz\":%lu,\"station_meshtastic_custom_bw_khz\":%u,\"station_meshtastic_custom_sf\":%u,\"station_meshtastic_custom_cr\":%u,\"station_meshtastic_custom_tx_power_dbm\":%d,"
      "\"station_meshtastic_command_pending\":%s,\"ws1300_reboot_pending\":%s,\"ws1300_gps_req_pending\":%s,\"ws1300_kick_armed\":%s,"
      "\"mesh_forwarding_enabled\":%s,\"station_meshcore_interval_min\":%u,\"station_meshcore_profile_idx\":%u,"
      "\"station_meshcore_custom_freq_hz\":%lu,\"station_meshcore_custom_bw_khz\":%u,\"station_meshcore_custom_sf\":%u,\"station_meshcore_custom_cr\":%u,\"station_meshcore_custom_tx_power_dbm\":%d,"
      "\"station_meshcore_command_pending\":%s,"
      "\"policy_ver\":%u,\"policy_crc\":%u,\"policy_crc_hex\":\"0x%04X\",\"station_policy_pending\":%s,"
      "\"mt_events_hour\":%u,\"mt_cooldown_lvl\":%u,\"mt_last_reasons\":%lu,"
      "\"mc_events_hour\":%u,\"mc_cooldown_lvl\":%u,\"mc_last_reasons\":%lu,"
      "\"dev_name\":\"%s\","
      "\"recent_stations\":%s,"
      "\"units\":{\"temp_id\":%d,\"wind_id\":%d,\"precip_id\":%d,\"press_id\":%d,\"temp\":\"%s\",\"wind\":\"%s\",\"precip\":\"%s\",\"press\":\"%s\"},"
      "\"weather\":{"
      "\"has_weather\":%s,\"temp\":%s,\"feels_like\":%s,\"humidity\":%s,"
      "\"precip_rate\":%s,\"precip_daily\":%s,\"precip_cml\":%s,\"precip_24h\":%s,\"pressure\":%s,\"wind_speed\":%s,\"wind_gust\":%s,"
      "\"wind_dir_deg\":%s,\"wind_dir_cardinal\":\"%s\",\"uv\":%u,\"irradiance\":%s,"
      "\"unit_temp\":\"%s\",\"unit_wind\":\"%s\",\"unit_precip\":\"%s\",\"unit_press\":\"%s\""
      "}}",
      wifi_state_str, Wifi::get_ssid() ? Wifi::get_ssid() : "",
      Wifi::get_rssi(), (unsigned long)uptime_sec, mqtt_state_str,
      (unsigned long)g_tb_success_count,
      (unsigned long)g_tb_fail_count, WsHandler::get_packet_reception(),
      WsHandler::get_activity_status_title(WsHandler::get_activity_status()),
      (unsigned long)WsHandler::get_activity_status_duration_sec(),
      (unsigned long)WsHandler::get_paired_id(), wx.pressure_hpa_json,
      APP_VERSION, is_paired ? "true" : "false",
      Settings::is_open_edition() ? "open" : "wxm",
      Settings::is_open_edition() ? "true" : "false",
      WsHandler::g_sniff_mode_enabled ? "true" : "false",
      WsHandler::g_auto_pair_enabled ? "true" : "false",
      Settings::get_cloud_pairing_enabled() ? "true" : "false",
      Utils::pair_mode_to_str(Settings::get_pair_mode()),
      selected_source_str, active_source_str,
      operation_mode, WsHandler::get_pairing_state_name(), ownership_status,
      (unsigned int)WsHandler::g_ownership_challenge_packets_seen,
      (unsigned long)Settings::get_station_ownership_id(),
      (unsigned long)Settings::get_station_ownership_timestamp(),
      (unsigned long)WsHandler::g_scan_target_id,
      (int)WsHandler::g_scan_target_slot,
      active_link ? "true" : "false",
      active_link ? "true" : "false",
      uplink_enabled ? "true" : "false",
      uplink_active ? "true" : "false",
      Settings::get_local_pair_sync_pending() ? "true" : "false",
      (long long)l_ws_id, local_sn_hex, local_ws_type,
      local_profile_present ? "true" : "false",
      local_profile_ready ? "true" : "false",
      local_profile_ready ? "true" : "false",
      local_has_pubkey ? "true" : "false",
      local_sn_synthesized ? "true" : "false",
      (long long)c_ws_id, cloud_sn_hex, cloud_ws_type, c_ws_valid ? "true" : "false",
      cloud_profile_ready ? "true" : "false", c_ws_match ? "true" : "false",
      cloud_has_pubkey ? "true" : "false",
      cloud_sn_synthesized ? "true" : "false",
      WsHandler::get_fallback_phase_name(),
      (unsigned long)WsHandler::get_fallback_seconds_remaining(),
      WsHandler::is_scan_active() ? "true" : "false",
      (int)WsHandler::get_sniff_slot(),
      (unsigned int)scan_total_slots,
      (unsigned long)WsHandler::get_scan_eta_sec(),
      (unsigned long)WsHandler::get_scan_total_cycle_sec(),
      (int)WsHandler::get_scan_profile(),
      WsHandler::is_scan_completed() ? "true" : "false",
      (unsigned int)WsHandler::get_scan_cycle_count(),
      (WsHandler::get_scan_phase() == WsHandler::ScanPhase::WS1001) ? "ws1001" : "ws1300",
      station_type, (int)Settings::get_freq_region(), Settings::get_mqtt_disabled() ? "true" : "false",
      lora_link_ok ? "true" : "false", is_screenless ? "true" : "false",
      sd_info.mounted ? "true" : "false", sd_info.present ? "true" : "false", sd_ejected ? "true" : "false",
      sd_info.unusable ? "true" : "false", sd_info.capacity_gb,
      sd_info.fs_type, sd_info.reason,
      (unsigned long)listen_freq_hz, listen_desc,
      (int)WsHandler::g_manual_listen_slot,
      (unsigned long)_rx_packet_total,
      (unsigned long)region_base_hz,
      (int)WsHandler::g_ws1300_tx_power_override,
      (int)WsHandler::g_ws1300_tx_interval_override_sec,
      (unsigned int)WsHandler::g_ws1300_station_fw_ver,
      (unsigned int)WsHandler::g_ws1300_meshtastic_interval_min,
      (unsigned int)WsHandler::g_ws1300_meshtastic_preset,
      (unsigned int)WsHandler::g_ws1300_meshtastic_hop_limit,
      (unsigned long)WsHandler::g_ws1300_meshtastic_custom_freq_hz,
      (unsigned int)WsHandler::g_ws1300_meshtastic_custom_bw_khz,
      (unsigned int)WsHandler::g_ws1300_meshtastic_custom_sf,
      (unsigned int)WsHandler::g_ws1300_meshtastic_custom_cr,
      (int)WsHandler::g_ws1300_meshtastic_custom_tx_power_dbm,
      WsHandler::g_ws1300_meshtastic_changed ? "true" : "false",
      WsHandler::g_ws1300_reboot_requested ? "true" : "false",
      WsHandler::g_ws1300_gps_req_forced ? "true" : "false",
      WsHandler::g_ws1300_kick_armed ? "true" : "false",
      Settings::get_mesh_multi_station_forwarding_enabled() ? "true" : "false",
      (unsigned int)WsHandler::g_ws1300_meshcore_interval_min,
      (unsigned int)WsHandler::g_ws1300_meshcore_profile_idx,
      (unsigned long)WsHandler::g_ws1300_meshcore_custom_freq_hz,
      (unsigned int)WsHandler::g_ws1300_meshcore_custom_bw_khz,
      (unsigned int)WsHandler::g_ws1300_meshcore_custom_sf,
      (unsigned int)WsHandler::g_ws1300_meshcore_custom_cr,
      (int)WsHandler::g_ws1300_meshcore_custom_tx_power_dbm,
      WsHandler::g_ws1300_meshcore_changed ? "true" : "false",
      (unsigned int)wx_policy.version, (unsigned int)wx_policy_crc, (unsigned int)wx_policy_crc,
      WsHandler::g_ws1300_weather_policy_changed ? "true" : "false",
      (unsigned int)mt_status.events_this_hour, (unsigned int)mt_status.cooldown_level, (unsigned long)mt_status.last_reasons,
      (unsigned int)mc_status.events_this_hour, (unsigned int)mc_status.cooldown_level, (unsigned long)mc_status.last_reasons,
      Settings::get_device_name() ? Settings::get_device_name() : "",
      recent_json,
      (int)Settings::get_temp_unit(),
      (int)Settings::get_wind_speed_unit(),
      (int)Settings::get_precip_unit(),
      (int)Settings::get_press_unit(),
      wx.unit_temp, wx.unit_wind, wx.unit_precip, wx.unit_press,
      wx.has_weather ? "true" : "false",
      wx.temp_json, wx.feels_like_json, wx.humidity_json,
      wx.precip_rate_json, wx.precip_daily_json, wx.precip_cml_json, wx.precip_24h_json, wx.pressure_json, wx.wind_speed_json, wx.wind_gust_json,
      wx.wind_dir_json, wx.wind_dir_cardinal, (unsigned int)wx.uv, wx.irradiance_json,
      wx.unit_temp, wx.unit_wind, wx.unit_precip, wx.unit_press);

  if (status_len < 0 || (size_t)status_len >= sizeof(json_buf)) {
    // Sending a truncated object would break every consumer of /status with a
    // parse error that points nowhere. Fail loudly instead.
    ESP_LOGE(TAG, "/status truncated: needs %d bytes, buffer is %u",
             status_len, (unsigned)sizeof(json_buf));
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control",
                     "no-store, no-cache, must-revalidate, max-age=0");
  httpd_resp_send(req, json_buf, status_len);
  return ESP_OK;
}

// GET /packet_log handler
static esp_err_t get_packet_log_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();
  // Return last 20 packets in JSON list format
  size_t cap = 32768;
  char *json_buf = (char *)malloc(cap);
  if (!json_buf) {
    httpd_resp_send_500(req);
    return ESP_ERR_NO_MEM;
  }

  // Entry scratch buffer. Must hold the hex payload (2 * 256 + 1) plus the
  // decoded field blob (1024) plus the JSON scaffolding. The old 1600 byte
  // stack buffer was smaller than its own worst-case inputs.
  const size_t entry_cap = 2048;
  char *entry = (char *)malloc(entry_cap);
  if (!entry) {
    free(json_buf);
    httpd_resp_send_500(req);
    return ESP_ERR_NO_MEM;
  }

  strcpy(json_buf, "[");
  size_t len = 1;
  bool first_entry = true;

  if (_packet_log_mutex != NULL &&
      xSemaphoreTake(_packet_log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    int idx = (_packet_log_head - _packet_log_count + PACKET_LOG_CAPACITY) % PACKET_LOG_CAPACITY;
    for (int i = 0; i < _packet_log_count; i++) {
      LoggedPacket &pkt = _packet_log[idx];

      char hex_payload[1024];
      for (int k = 0; k < pkt.length; k++) {
        sprintf(hex_payload + (k * 2), "%02x", pkt.payload[k]);
      }
      hex_payload[pkt.length * 2] = '\0';

      char decoded_str[1024] = "{}";
      WsBase::MultiStation *active_station = NULL;
      uint32_t station_id_val = 0;
      bool has_station_id = false;
      uint8_t fw_version_val = 0;
      bool has_fw_version = false;
      bool decoded_ok = false;
      char s_type_str[64] = "";
      char s_id_str[32] = "null";
      char s_ver_str[48] = "null";
      char p_type_str[64] = "Unknown";

      {
        WsCommon::Packet rx_pkt = {.pl = pkt.payload,
                                   .pl_len = (uint8_t)pkt.length};
        WsFields::FieldPack f_pack = {};
        WsGps::GpsData g_data = {};
        decoded_ok = false;

        // Always attribute via full validate/verify/decode, regardless of
        // sniff mode - bare packet_decoder calls are too permissive and
        // mislabel foreign packets (e.g. WS1001 neighbors shown as WS1300
        // with garbage IDs)
        WsBase::MultiStation *matched = NULL;
        if (WsHandler::try_sniff_decode(&rx_pkt, &matched, &f_pack, &g_data)) {
          active_station = matched;
          decoded_ok = true;
        }

        if (decoded_ok) {
          if (WsFields::get_field_val_from_pack<uint32_t>(
                  &f_pack, WsFields::FieldType::META_ID, NULL,
                  &station_id_val) == WsCommon::RetRes::OK) {
            has_station_id = true;
          }
          if (WsFields::get_field_val_from_pack<uint8_t>(
                  &f_pack, WsFields::FieldType::META_FW_VER, NULL,
                  &fw_version_val) == WsCommon::RetRes::OK) {
            has_fw_version = true;
          }

          size_t dec_written = snprintf(decoded_str, sizeof(decoded_str), "{");
          bool dec_first = true;
          for (int f_idx = 0; f_idx < f_pack.field_count; f_idx++) {
            const WsFields::Field &f = f_pack.fields[f_idx];
            if (f.decode_status == WsCommon::RetRes::OK && f.info != nullptr) {
              char val_str[64];
              if (f.info->type == WsFields::FieldType::META_FW_VER) {
                if (active_station != nullptr && active_station->TYPE == WsType::WS_TYPE_1300) {
                  snprintf(val_str, sizeof(val_str), "\"v%u (0x%02X)\"", f.value.val_u8, f.value.val_u8);
                } else {
                  uint8_t minor = f.value.val_u8 >> 5;
                  uint8_t patch = f.value.val_u8 & 0x1F;
                  snprintf(val_str, sizeof(val_str), "\"1.%u.%u (0x%02X)\"", minor, patch, f.value.val_u8);
                }
              } else if (f.info->data_type == WsFields::ValueType::FLOAT) {
                // JSON has no NaN or infinity literals. Faulty stations can
                // still return decode-OK for a raw float whose bit pattern is
                // non-finite (observed as "-nan" for precipitation). Preserve
                // the field and packet diagnosis, but serialize its value as
                // JSON null so one bad sensor cannot break the whole log.
                if (std::isfinite(f.value.val_float)) {
                  snprintf(val_str, sizeof(val_str), "%.2f",
                           f.value.val_float);
                } else {
                  snprintf(val_str, sizeof(val_str), "null");
                }
              } else if (f.info->data_type == WsFields::ValueType::INT) {
                snprintf(val_str, sizeof(val_str), "%d", f.value.val_int);
              } else if (f.info->data_type == WsFields::ValueType::U8) {
                snprintf(val_str, sizeof(val_str), "%u", f.value.val_u8);
              } else if (f.info->data_type == WsFields::ValueType::U16) {
                snprintf(val_str, sizeof(val_str), "%u", f.value.val_u16);
              } else if (f.info->data_type == WsFields::ValueType::U32) {
                snprintf(val_str, sizeof(val_str), "%lu",
                         (unsigned long)f.value.val_u32);
              } else if (f.info->data_type == WsFields::ValueType::BOOL) {
                snprintf(val_str, sizeof(val_str), "%s",
                         f.value.val_bool ? "true" : "false");
              } else {
                continue;
              }
              size_t req_len = snprintf(
                  decoded_str + dec_written, sizeof(decoded_str) - dec_written,
                  "%s\"%s\":%s", dec_first ? "" : ",", f.info->name, val_str);
              if (dec_written + req_len < sizeof(decoded_str)) {
                dec_written += req_len;
                dec_first = false;
              } else {
                break;
              }
            }
          }
          if (g_data.valid) {
            for (int f_idx = 0; f_idx < g_data.field_pack.field_count;
                 f_idx++) {
              const WsFields::Field &f = g_data.field_pack.fields[f_idx];
              if (f.decode_status == WsCommon::RetRes::OK &&
                  f.info != nullptr) {
                char val_str[64];
                if (f.info->data_type == WsFields::ValueType::FLOAT) {
                  if (std::isfinite(f.value.val_float)) {
                    snprintf(val_str, sizeof(val_str), "%.6f",
                             f.value.val_float);
                  } else {
                    snprintf(val_str, sizeof(val_str), "null");
                  }
                } else if (f.info->data_type == WsFields::ValueType::INT) {
                  snprintf(val_str, sizeof(val_str), "%d", f.value.val_int);
                } else if (f.info->data_type == WsFields::ValueType::U8) {
                  snprintf(val_str, sizeof(val_str), "%u", f.value.val_u8);
                } else if (f.info->data_type == WsFields::ValueType::U16) {
                  snprintf(val_str, sizeof(val_str), "%u", f.value.val_u16);
                } else if (f.info->data_type == WsFields::ValueType::U32) {
                  snprintf(val_str, sizeof(val_str), "%lu",
                           (unsigned long)f.value.val_u32);
                } else if (f.info->data_type == WsFields::ValueType::BOOL) {
                  snprintf(val_str, sizeof(val_str), "%s",
                           f.value.val_bool ? "true" : "false");
                } else {
                  continue;
                }
                size_t req_len =
                    snprintf(decoded_str + dec_written,
                             sizeof(decoded_str) - dec_written, "%s\"%s\":%s",
                             dec_first ? "" : ",", f.info->name, val_str);
                if (dec_written + req_len < sizeof(decoded_str)) {
                  dec_written += req_len;
                  dec_first = false;
                } else {
                  break;
                }
              }
            }
          }
          if (active_station != nullptr && active_station->TYPE == WsType::WS_TYPE_1300) {
            WsCommon::Packet rx_pkt = {.pl = pkt.payload, .pl_len = (uint8_t)pkt.length};
            Ws1300::StaPacketType p_type = Ws1300Helpers::get_packet_type(&rx_pkt);
            if (p_type == Ws1300::StaPacketType::ADV && pkt.length >= sizeof(Ws1300::ByteBuffer::StaAdv)) {
              Ws1300::ByteBuffer::StaAdv* adv = (Ws1300::ByteBuffer::StaAdv*)pkt.payload;
              char sn_hex[21] = "";
              char pubkey_hex[129] = "";
              for (int k = 0; k < sizeof(adv->sn); k++) {
                sprintf(sn_hex + (k * 2), "%02x", adv->sn[k]);
              }
              for (int k = 0; k < sizeof(adv->pub_key); k++) {
                sprintf(pubkey_hex + (k * 2), "%02x", adv->pub_key[k]);
              }
              char extra_fields[512] = "";
              snprintf(extra_fields, sizeof(extra_fields),
                ",\"Hardware Version\":%u,\"Serial Number\":\"%s\",\"Public Key\":\"%s\",\"Ext Battery Connected\":%s,\"Int Battery Inserted\":%s,\"GPS Acquiring\":%s",
                adv->header.hw_ver,
                sn_hex,
                pubkey_hex,
                adv->header.aux_byte_2.ext_batt ? "true" : "false",
                adv->header.aux_byte_2.int_batt ? "true" : "false",
                adv->header.aux_byte_2.acquiring_gps ? "true" : "false"
              );
              size_t req_len = strlen(extra_fields);
              if (dec_written + req_len < sizeof(decoded_str)) {
                strcpy(decoded_str + dec_written, extra_fields);
                dec_written += req_len;
              }
            } else if (p_type == Ws1300::StaPacketType::BOOT && pkt.length >= sizeof(Ws1300::ByteBuffer::StaBoot)) {
              Ws1300::ByteBuffer::StaBoot* boot = (Ws1300::ByteBuffer::StaBoot*)pkt.payload;
              char sn_hex[21] = "";
              char pubkey_hex[129] = "";
              for (int k = 0; k < sizeof(boot->sn); k++) {
                sprintf(sn_hex + (k * 2), "%02x", boot->sn[k]);
              }
              for (int k = 0; k < sizeof(boot->pub_key); k++) {
                sprintf(pubkey_hex + (k * 2), "%02x", boot->pub_key[k]);
              }
              char extra_fields[512] = "";
              snprintf(extra_fields, sizeof(extra_fields),
                ",\"Hardware Version\":%u,\"Serial Number\":\"%s\",\"Public Key\":\"%s\",\"Ext Battery Connected\":%s,\"Int Battery Inserted\":%s,\"GPS Acquiring\":%s,\"FO Version\":%u,\"Boot Reason\":%u",
                boot->header.hw_ver,
                sn_hex,
                pubkey_hex,
                boot->header.aux_byte_2.ext_batt ? "true" : "false",
                boot->header.aux_byte_2.int_batt ? "true" : "false",
                boot->header.aux_byte_2.acquiring_gps ? "true" : "false",
                boot->fo_ver,
                boot->boot_reason
              );
              size_t req_len = strlen(extra_fields);
              if (dec_written + req_len < sizeof(decoded_str)) {
                strcpy(decoded_str + dec_written, extra_fields);
                dec_written += req_len;
              }
            }
          }
          snprintf(decoded_str + dec_written, sizeof(decoded_str) - dec_written,
                   "}");

          if (active_station != NULL) {
            snprintf(s_type_str, sizeof(s_type_str), "%s", active_station->NAME);
            if (has_station_id) {
              snprintf(s_id_str, sizeof(s_id_str), "\"0x%08lX\"",
                       (unsigned long)station_id_val);
            }
            if (has_fw_version) {
              if (active_station->TYPE == WsType::WS_TYPE_1300) {
                snprintf(s_ver_str, sizeof(s_ver_str), "\"v%u (0x%02X)\"", fw_version_val, fw_version_val);
              } else {
                uint8_t minor = fw_version_val >> 5;
                uint8_t patch = fw_version_val & 0x1F;
                snprintf(s_ver_str, sizeof(s_ver_str), "\"1.%u.%u (0x%02X)\"", minor, patch, fw_version_val);
              }
            }

            if (active_station->TYPE == WsType::WS_TYPE_1001) {
              snprintf(p_type_str, sizeof(p_type_str), "%s", "Sensors");
            } else if (active_station->TYPE == WsType::WS_TYPE_1300) {
              WsCommon::Packet rx_pkt = {.pl = pkt.payload, .pl_len = (uint8_t)pkt.length};
              Ws1300::StaPacketType p_type = Ws1300Helpers::get_packet_type(&rx_pkt);
              if (p_type == Ws1300::StaPacketType::BOOT) {
                snprintf(p_type_str, sizeof(p_type_str), "%s", "Boot");
              } else if (p_type == Ws1300::StaPacketType::ADV) {
                snprintf(p_type_str, sizeof(p_type_str), "%s", "Adv");
              } else if (p_type == Ws1300::StaPacketType::SENSORS) {
                snprintf(p_type_str, sizeof(p_type_str), "%s", "Sensors");
              } else if (p_type == Ws1300::StaPacketType::GNSS) {
                snprintf(p_type_str, sizeof(p_type_str), "%s", "GNSS");
              } else {
                snprintf(p_type_str, sizeof(p_type_str), "%s", "Unknown");
              }
            }
          }
        }
      }

      if (!decoded_ok && pkt.length >= sizeof(Ws1300::ByteBuffer::RespHeader)) {
        const Ws1300::ByteBuffer::RespHeader *resp_hdr =
            (const Ws1300::ByteBuffer::RespHeader *)pkt.payload;
        if (resp_hdr->type == Ws1300::RespPacketType::ACK &&
            pkt.length == sizeof(Ws1300::ByteBuffer::RespAck)) {
          decoded_ok = true;
          station_id_val = Ws1300::ByteBuffer::be_to_le(resp_hdr->station_id);
          has_station_id = true;
          uint16_t p_id = Ws1300::ByteBuffer::be_to_le(resp_hdr->packet_id);
          snprintf(s_type_str, sizeof(s_type_str), "WS1300");
          snprintf(s_id_str, sizeof(s_id_str), "\"0x%08lX\"", (unsigned long)station_id_val);
          snprintf(s_ver_str, sizeof(s_ver_str), "\"P2P\"");
          snprintf(p_type_str, sizeof(p_type_str), "ACK");
          snprintf(decoded_str, sizeof(decoded_str),
                   "{\"Packet Type\":\"ACK\",\"Station ID\":\"0x%08lX\",\"Acked Packet ID\":%u}",
                   (unsigned long)station_id_val, (unsigned int)p_id);
        } else if (resp_hdr->type == Ws1300::RespPacketType::TIME_SYNC &&
                   pkt.length == sizeof(Ws1300::ByteBuffer::RespTimeSync)) {
          decoded_ok = true;
          const Ws1300::ByteBuffer::RespTimeSync *ts_resp =
              (const Ws1300::ByteBuffer::RespTimeSync *)pkt.payload;
          station_id_val = Ws1300::ByteBuffer::be_to_le(resp_hdr->station_id);
          has_station_id = true;
          uint16_t p_id = Ws1300::ByteBuffer::be_to_le(resp_hdr->packet_id);
          uint32_t sync_ts = Ws1300::ByteBuffer::be_to_le(ts_resp->ts);
          snprintf(s_type_str, sizeof(s_type_str), "WS1300");
          snprintf(s_id_str, sizeof(s_id_str), "\"0x%08lX\"", (unsigned long)station_id_val);
          snprintf(s_ver_str, sizeof(s_ver_str), "\"P2P\"");
          snprintf(p_type_str, sizeof(p_type_str), "TIME_SYNC");
          snprintf(decoded_str, sizeof(decoded_str),
                   "{\"Packet Type\":\"TIME_SYNC\",\"Station ID\":\"0x%08lX\",\"Acked Packet ID\":%u,\"Synced Timestamp\":%lu}",
                   (unsigned long)station_id_val, (unsigned int)p_id, (unsigned long)sync_ts);
        } else if (resp_hdr->type == Ws1300::RespPacketType::PAIR &&
                   pkt.length == sizeof(Ws1300::ByteBuffer::RespPair)) {
          decoded_ok = true;
          station_id_val = Ws1300::ByteBuffer::be_to_le(resp_hdr->station_id);
          has_station_id = true;
          uint16_t p_id = Ws1300::ByteBuffer::be_to_le(resp_hdr->packet_id);
          snprintf(s_type_str, sizeof(s_type_str), "WS1300");
          snprintf(s_id_str, sizeof(s_id_str), "\"0x%08lX\"", (unsigned long)station_id_val);
          snprintf(s_ver_str, sizeof(s_ver_str), "\"P2P\"");
          snprintf(p_type_str, sizeof(p_type_str), "PAIR");
          snprintf(decoded_str, sizeof(decoded_str),
                   "{\"Packet Type\":\"PAIR\",\"Station ID\":\"0x%08lX\",\"Acked Packet ID\":%u}",
                   (unsigned long)station_id_val, (unsigned int)p_id);
        } else if (resp_hdr->type == Ws1300::RespPacketType::CMD) {
          station_id_val = Ws1300::ByteBuffer::be_to_le(resp_hdr->station_id);
          uint16_t p_id = Ws1300::ByteBuffer::be_to_le(resp_hdr->packet_id);
          if (pkt.length >= sizeof(Ws1300::ByteBuffer::RespCmdV3Header) &&
              pkt.payload[sizeof(Ws1300::ByteBuffer::RespHeader)] == Ws1300::ByteBuffer::RESP_CMD_V3_FORMAT_VER) {
            decoded_ok = true;
            has_station_id = true;
            const Ws1300::ByteBuffer::RespCmdV3Header *v3_hdr =
                (const Ws1300::ByteBuffer::RespCmdV3Header *)pkt.payload;
            snprintf(s_type_str, sizeof(s_type_str), "WS1300");
            snprintf(s_id_str, sizeof(s_id_str), "\"0x%08lX\"", (unsigned long)station_id_val);
            snprintf(s_ver_str, sizeof(s_ver_str), "\"P2P v3\"");
            snprintf(p_type_str, sizeof(p_type_str), "CMD");
            snprintf(decoded_str, sizeof(decoded_str),
                     "{\"Packet Type\":\"CMD V3\",\"Station ID\":\"0x%08lX\",\"Acked Packet ID\":%u,\"Cmd Seq\":%u,\"TLV Len\":%u}",
                     (unsigned long)station_id_val, (unsigned int)p_id, (unsigned int)v3_hdr->cmd_seq, (unsigned int)v3_hdr->tlv_len);
          } else if (pkt.length == sizeof(Ws1300::ByteBuffer::RespCmdV2)) {
            decoded_ok = true;
            has_station_id = true;
            const Ws1300::ByteBuffer::RespCmdV2 *cmd_v2 =
                (const Ws1300::ByteBuffer::RespCmdV2 *)pkt.payload;
            snprintf(s_type_str, sizeof(s_type_str), "WS1300");
            snprintf(s_id_str, sizeof(s_id_str), "\"0x%08lX\"", (unsigned long)station_id_val);
            snprintf(s_ver_str, sizeof(s_ver_str), "\"P2P v2\"");
            snprintf(p_type_str, sizeof(p_type_str), "CMD");
            snprintf(decoded_str, sizeof(decoded_str),
                     "{\"Packet Type\":\"CMD V2\",\"Station ID\":\"0x%08lX\",\"Acked Packet ID\":%u,\"Acquire GPS\":%s,\"Reboot\":%s,\"TX Power\":%u,\"Mesh Interval\":%u}",
                     (unsigned long)station_id_val, (unsigned int)p_id,
                     cmd_v2->acquire_gps ? "true" : "false",
                     cmd_v2->reboot ? "true" : "false",
                     (unsigned int)cmd_v2->tx_pwr,
                     (unsigned int)cmd_v2->meshtastic_interval_min);
          } else if (pkt.length == sizeof(Ws1300::ByteBuffer::RespCmdV1)) {
            decoded_ok = true;
            has_station_id = true;
            const Ws1300::ByteBuffer::RespCmdV1 *cmd_v1 =
                (const Ws1300::ByteBuffer::RespCmdV1 *)pkt.payload;
            snprintf(s_type_str, sizeof(s_type_str), "WS1300");
            snprintf(s_id_str, sizeof(s_id_str), "\"0x%08lX\"", (unsigned long)station_id_val);
            snprintf(s_ver_str, sizeof(s_ver_str), "\"P2P v1\"");
            snprintf(p_type_str, sizeof(p_type_str), "CMD");
            snprintf(decoded_str, sizeof(decoded_str),
                     "{\"Packet Type\":\"CMD V1\",\"Station ID\":\"0x%08lX\",\"Acked Packet ID\":%u,\"Acquire GPS\":%s,\"Reboot\":%s,\"TX Power\":%u}",
                     (unsigned long)station_id_val, (unsigned int)p_id,
                     cmd_v1->acquire_gps ? "true" : "false",
                     cmd_v1->reboot ? "true" : "false",
                     (unsigned int)cmd_v1->tx_pwr);
          }
        }
      }

      // Try WeatherXM WS1001 Downlink ACK Decode
      if (!decoded_ok && (pkt.length == sizeof(Ws1001::AckPacket) ||
                          pkt.length == sizeof(Ws1001::AckPacket) + 16)) {
        const Ws1001::AckPacket *ack = (const Ws1001::AckPacket *)pkt.payload;
        if (ack->size == sizeof(Ws1001::AckPacket)) {
          decoded_ok = true;
          snprintf(s_type_str, sizeof(s_type_str), "WS1001");
          snprintf(s_ver_str, sizeof(s_ver_str), "\"P2P\"");
          snprintf(p_type_str, sizeof(p_type_str), "ACK");
          snprintf(decoded_str, sizeof(decoded_str),
                   "{\"Packet Type\":\"ACK\",\"Interval\":%u,\"Reboot\":%s,\"Req GPS\":%s,\"Power\":%u,\"Req Command\":%s}",
                   (unsigned int)ack->aux_byte_1.interval,
                   ack->aux_byte_1.reboot ? "true" : "false",
                   ack->aux_byte_1.req_gps ? "true" : "false",
                   (unsigned int)ack->aux_byte_2.power,
                   ack->aux_byte_2.req_command ? "true" : "false");
        }
      }

      if (!decoded_ok) {
        // Try Meshtastic frame decode
        MeshFrame::DecodedFrame mesh_dec = {};
        if (MeshFrame::decode_frame(pkt.payload, pkt.length, MeshFrame::DEFAULT_PSK, ws_aes_ecb, &mesh_dec)) {
          decoded_ok = true;
          snprintf(s_type_str, sizeof(s_type_str), "Meshtastic");
          snprintf(s_id_str, sizeof(s_id_str), "\"!%08lX\"", (unsigned long)mesh_dec.from);
          snprintf(s_ver_str, sizeof(s_ver_str), "\"Mesh V2\"");

          size_t dec_w = snprintf(decoded_str, sizeof(decoded_str), "{");
          bool df = true;

          auto append_f = [&](const char *name, float val) {
            if (std::isfinite(val)) {
              int r = snprintf(decoded_str + dec_w, sizeof(decoded_str) - dec_w,
                               "%s\"%s\":%.2f", df ? "" : ",", name, val);
              if (r > 0 && dec_w + (size_t)r < sizeof(decoded_str)) {
                dec_w += (size_t)r;
                df = false;
              }
            }
          };

          auto append_u = [&](const char *name, uint32_t val) {
            int r = snprintf(decoded_str + dec_w, sizeof(decoded_str) - dec_w,
                             "%s\"%s\":%lu", df ? "" : ",", name, (unsigned long)val);
            if (r > 0 && dec_w + (size_t)r < sizeof(decoded_str)) {
              dec_w += (size_t)r;
              df = false;
            }
          };

          auto append_s = [&](const char *name, const char *val) {
            if (val != nullptr && val[0] != '\0') {
              int r = snprintf(decoded_str + dec_w, sizeof(decoded_str) - dec_w,
                               "%s\"%s\":\"%s\"", df ? "" : ",", name, val);
              if (r > 0 && dec_w + (size_t)r < sizeof(decoded_str)) {
                dec_w += (size_t)r;
                df = false;
              }
            }
          };

          if (mesh_dec.has_env) {
            snprintf(p_type_str, sizeof(p_type_str), "MESH: TELEM");
            if (mesh_dec.env.has_temperature) append_f("Temperature", mesh_dec.env.temperature);
            if (mesh_dec.env.has_humidity) append_f("Humidity", mesh_dec.env.humidity);
            if (mesh_dec.env.has_pressure) append_f("Pressure", mesh_dec.env.pressure);
            if (mesh_dec.env.has_lux) append_f("Illuminance", mesh_dec.env.lux);
            if (mesh_dec.env.has_wind_direction) append_u("Wind Direction", mesh_dec.env.wind_direction);
            if (mesh_dec.env.has_wind_speed) append_f("Wind Speed", mesh_dec.env.wind_speed);
            if (mesh_dec.env.has_wind_gust) append_f("Wind Gust", mesh_dec.env.wind_gust);
            if (mesh_dec.env.has_rainfall_1h) append_f("Rainfall 1h", mesh_dec.env.rainfall_1h);
            if (mesh_dec.env.has_rainfall_24h) append_f("Rainfall 24h", mesh_dec.env.rainfall_24h);
            append_u("Hop Limit", mesh_dec.hop_limit);
            append_u("Channel Hash", mesh_dec.channel_hash);
          } else if (mesh_dec.has_device_metrics) {
            snprintf(p_type_str, sizeof(p_type_str), "MESH: DEV");
            if (mesh_dec.device.has_battery_level) append_u("Battery Level", mesh_dec.device.battery_level);
            if (mesh_dec.device.has_voltage) append_f("Voltage", mesh_dec.device.voltage);
            if (mesh_dec.device.has_uptime) append_u("Uptime Seconds", mesh_dec.device.uptime_seconds);
            append_u("Hop Limit", mesh_dec.hop_limit);
            append_u("Channel Hash", mesh_dec.channel_hash);
          } else if (mesh_dec.has_nodeinfo) {
            snprintf(p_type_str, sizeof(p_type_str), "MESH: NODEINFO");
            append_s("Node ID", mesh_dec.user_id);
            append_s("Long Name", mesh_dec.long_name);
            append_s("Short Name", mesh_dec.short_name);
            append_u("HW Model", mesh_dec.hw_model);
            append_u("Role", mesh_dec.role);
          } else if (mesh_dec.has_position) {
            snprintf(p_type_str, sizeof(p_type_str), "MESH: POS");
            append_f("Latitude", mesh_dec.position.lat);
            append_f("Longitude", mesh_dec.position.lon);
            if (mesh_dec.position.has_altitude) append_u("Altitude", (uint32_t)mesh_dec.position.altitude);
            if (mesh_dec.position.time > 0) append_u("Timestamp", mesh_dec.position.time);
          } else if (mesh_dec.has_text) {
            snprintf(p_type_str, sizeof(p_type_str), "MESH: TEXT");
            append_s("Message", mesh_dec.text);
          } else {
            snprintf(p_type_str, sizeof(p_type_str), "MESH: APP %u", (unsigned int)mesh_dec.portnum);
            append_u("Portnum", mesh_dec.portnum);
            append_u("Hop Limit", mesh_dec.hop_limit);
            append_u("Channel Hash", mesh_dec.channel_hash);
          }
          snprintf(decoded_str + dec_w, sizeof(decoded_str) - dec_w, "}");
        }
      }

      if (!decoded_ok) {
        // Try MeshCore frame decode
        MeshCoreFrame::DecodedGrpData mc_dec = {};
        if (MeshCoreFrame::decode_grp_data_frame(
                pkt.payload, pkt.length, MeshCoreFrame::DEFAULT_PSK,
                ws_meshcore_aes_ecb, ws_meshcore_hmac_sha256, &mc_dec)) {
          decoded_ok = true;
          snprintf(s_type_str, sizeof(s_type_str), "MeshCore");
          snprintf(s_ver_str, sizeof(s_ver_str), "\"MeshCore v1\"");

          if (mc_dec.type == MeshCoreFrame::GrpPayloadType::SIGNED_STASENSORS_V1) {
            snprintf(p_type_str, sizeof(p_type_str), "MESHCORE: SIGNED");
            WsCommon::Packet inner_pkt = {.pl = mc_dec.signed_sta_sensors,
                                          .pl_len = (uint8_t)mc_dec.signed_sta_sensors_len};
            WsFields::FieldPack f_pack = {};
            WsGps::GpsData g_data = {};
            if (Ws1300::packet_decoder(&inner_pkt, &f_pack, &g_data) == WsCommon::RetRes::OK) {
              if (WsFields::get_field_val_from_pack<uint32_t>(
                      &f_pack, WsFields::FieldType::META_ID, NULL,
                      &station_id_val) == WsCommon::RetRes::OK) {
                snprintf(s_id_str, sizeof(s_id_str), "\"0x%08lX\"", (unsigned long)station_id_val);
              }
              size_t dec_w = snprintf(decoded_str, sizeof(decoded_str), "{");
              bool dec_first = true;
              for (int f_idx = 0; f_idx < f_pack.field_count; f_idx++) {
                const WsFields::Field &f = f_pack.fields[f_idx];
                if (f.decode_status == WsCommon::RetRes::OK && f.info != nullptr) {
                  char val_str[64];
                  if (f.info->data_type == WsFields::ValueType::FLOAT) {
                    if (std::isfinite(f.value.val_float)) {
                      snprintf(val_str, sizeof(val_str), "%.2f", f.value.val_float);
                    } else {
                      snprintf(val_str, sizeof(val_str), "null");
                    }
                  } else if (f.info->data_type == WsFields::ValueType::INT) {
                    snprintf(val_str, sizeof(val_str), "%d", f.value.val_int);
                  } else if (f.info->data_type == WsFields::ValueType::U8) {
                    snprintf(val_str, sizeof(val_str), "%u", f.value.val_u8);
                  } else if (f.info->data_type == WsFields::ValueType::U16) {
                    snprintf(val_str, sizeof(val_str), "%u", f.value.val_u16);
                  } else if (f.info->data_type == WsFields::ValueType::U32) {
                    snprintf(val_str, sizeof(val_str), "%lu", (unsigned long)f.value.val_u32);
                  } else if (f.info->data_type == WsFields::ValueType::BOOL) {
                    snprintf(val_str, sizeof(val_str), "%s", f.value.val_bool ? "true" : "false");
                  } else {
                    continue;
                  }
                  size_t req_len = snprintf(
                      decoded_str + dec_w, sizeof(decoded_str) - dec_w,
                      "%s\"%s\":%s", dec_first ? "" : ",", f.info->name, val_str);
                  if (dec_w + req_len < sizeof(decoded_str)) {
                    dec_w += req_len;
                    dec_first = false;
                  }
                }
              }
              snprintf(decoded_str + dec_w, sizeof(decoded_str) - dec_w, "}");
            }
          } else if (mc_dec.type == MeshCoreFrame::GrpPayloadType::LEGACY_OBSERVATION) {
            snprintf(p_type_str, sizeof(p_type_str), "MESHCORE: OBS");
            snprintf(s_id_str, sizeof(s_id_str), "\"0x%08lX\"", (unsigned long)mc_dec.legacy_obs.station_id);

            size_t dec_w = snprintf(decoded_str, sizeof(decoded_str), "{");
            bool df = true;
            auto append_mc_f = [&](const char *name, float val) {
              int r = snprintf(decoded_str + dec_w, sizeof(decoded_str) - dec_w,
                               "%s\"%s\":%.2f", df ? "" : ",", name, val);
              if (r > 0 && dec_w + (size_t)r < sizeof(decoded_str)) {
                dec_w += (size_t)r;
                df = false;
              }
            };
            auto append_mc_u = [&](const char *name, uint32_t val) {
              int r = snprintf(decoded_str + dec_w, sizeof(decoded_str) - dec_w,
                               "%s\"%s\":%lu", df ? "" : ",", name, (unsigned long)val);
              if (r > 0 && dec_w + (size_t)r < sizeof(decoded_str)) {
                dec_w += (size_t)r;
                df = false;
              }
            };
            uint16_t mask = mc_dec.legacy_obs.valid_mask;
            if (mask & (1 << 0)) append_mc_f("Temperature", (float)mc_dec.legacy_obs.temperature / 10.0f);
            if (mask & (1 << 1)) append_mc_u("Humidity", mc_dec.legacy_obs.humidity);
            if (mask & (1 << 2)) append_mc_f("Pressure", (float)mc_dec.legacy_obs.pressure / 10.0f);
            if (mask & (1 << 3)) append_mc_f("Wind Speed", (float)mc_dec.legacy_obs.wind_speed / 10.0f);
            if (mask & (1 << 4)) append_mc_f("Wind Gust", (float)mc_dec.legacy_obs.wind_gust / 10.0f);
            if (mask & (1 << 5)) append_mc_u("Wind Direction", mc_dec.legacy_obs.wind_dir);
            if (mask & (1 << 6)) append_mc_f("Precipitation", (float)mc_dec.legacy_obs.precip_cml / 10.0f);
            if (mask & (1 << 7)) append_mc_f("Solar Irradiance", (float)mc_dec.legacy_obs.solar_rad / 10.0f);
            if (mask & (1 << 8)) append_mc_f("UV Index", (float)mc_dec.legacy_obs.uv_index / 10.0f);
            if (mask & (1 << 9)) append_mc_f("Battery Voltage", (float)mc_dec.legacy_obs.battery_mv / 1000.0f);
            snprintf(decoded_str + dec_w, sizeof(decoded_str) - dec_w, "}");
          }
        }
      }

      const char *category_str = "other";
      if (decoded_ok) {
        if (strncmp(s_type_str, "Meshtastic", 10) == 0) {
          category_str = "meshtastic";
        } else if (strncmp(s_type_str, "MeshCore", 8) == 0) {
          category_str = "meshcore";
        } else if (active_station != NULL || strncmp(s_type_str, "WS", 2) == 0) {
          category_str = "weatherxm";
        }
      }

      bool dynamic_filtered = pkt.filtered;
      if (pkt.is_tx) {
        dynamic_filtered = false;
        if (strcmp(category_str, "other") == 0) {
          if (strncmp(p_type_str, "MESHCORE", 8) == 0) {
            category_str = "meshcore";
          } else if (strncmp(p_type_str, "MESH", 4) == 0) {
            category_str = "meshtastic";
          }
        }
        if (s_type_str[0] == '\0') {
          snprintf(s_type_str, sizeof(s_type_str), "Gateway");
        }
      } else if (decoded_ok) {
        WsType current_profile = WsHandler::g_receiver_profile;
        if (current_profile != WsType::WS_TYPE_ALL) {
          if (active_station != NULL) {
            dynamic_filtered = (active_station->TYPE != current_profile);
          } else {
            dynamic_filtered = false;
          }
        } else {
          dynamic_filtered = false;
        }
      } else {
        dynamic_filtered = true;
      }

      uint32_t f_hz = pkt.freq_hz;
      if (f_hz == 0) {
        WsHandler::get_listen_info(&f_hz, nullptr, 0);
      }
      char freq_str[32] = "";
      if (f_hz > 0) {
        snprintf(freq_str, sizeof(freq_str), "%.3f MHz", (double)f_hz / 1000000.0);
      } else {
        snprintf(freq_str, sizeof(freq_str), "---");
      }
      char specs_str[64] = "";
      uint8_t sf = pkt.sf ? pkt.sf : 7;
      uint32_t bw_khz = pkt.bw_hz ? (pkt.bw_hz / 1000) : 125;
      uint8_t cr = pkt.cr ? (pkt.cr > 4 ? pkt.cr : pkt.cr + 4) : 5;
      snprintf(specs_str, sizeof(specs_str), "SF%u \xc2\xb7 BW %lukHz \xc2\xb7 CR 4/%u", sf, (unsigned long)bw_khz, cr);

      // The separator is emitted BEFORE the entry, never baked into it. The
      // old code appended a trailing comma to every entry except the last, so
      // whenever an entry was dropped for lack of room the response ended
      // "...},]" - invalid JSON. The browser then rejected the whole payload
      // and the sniffer table silently stayed empty, with the only clue a
      // console error. Same class of failure if snprintf truncated an entry.
      int written = snprintf(entry, entry_cap,
               "{\"time\":%lu,\"rssi\":%d,\"snr\":%d,\"filtered\":%s,\"is_tx\":%s,\"tx_power_dbm\":%d,\"category\":\"%s\",\"data\":"
               "\"%s\",\"decoded\":%s,"
               "\"station_type\":\"%s\",\"station_id\":%s,\"protocol_version\":"
               "%s,\"channel_slot\":%u,\"freq_hz\":%lu,\"freq_str\":\"%s\",\"specs_str\":\"%s\",\"packet_type\":\"%s\"}",
               (unsigned long)pkt.timestamp_sec, pkt.rssi, pkt.snr,
               dynamic_filtered ? "true" : "false",
               pkt.is_tx ? "true" : "false",
               pkt.tx_power_dbm,
               category_str,
               hex_payload, decoded_str,
               s_type_str, s_id_str, s_ver_str,
               pkt.channel_slot, (unsigned long)f_hz, freq_str, specs_str, p_type_str);

      // snprintf returns the length it WANTED to write. Anything >= the buffer
      // was truncated and would be malformed JSON - drop it whole.
      if (written < 0 || (size_t)written >= entry_cap) {
        ESP_LOGW(TAG, "packet_log: entry %d needs %d bytes, dropping", i, written);
        idx = (idx + 1) % PACKET_LOG_CAPACITY;
        continue;
      }

      // +2 leaves room for the separator and the closing bracket.
      if (len + (size_t)written + 2 >= cap) {
        ESP_LOGW(TAG, "packet_log: response buffer full at entry %d", i);
        idx = (idx + 1) % PACKET_LOG_CAPACITY;
        continue;
      }

      if (!first_entry) {
        json_buf[len++] = ',';
      }
      memcpy(json_buf + len, entry, (size_t)written);
      len += (size_t)written;
      json_buf[len] = '\0';
      first_entry = false;

      idx = (idx + 1) % PACKET_LOG_CAPACITY;
    }
    xSemaphoreGive(_packet_log_mutex);
  }
  free(entry);

  json_buf[len++] = ']';
  json_buf[len] = '\0';

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control",
                     "no-store, no-cache, must-revalidate, max-age=0");
  httpd_resp_send(req, json_buf, len);
  free(json_buf);
  return ESP_OK;
}

extern "C" {
    extern const uint8_t _binary_uPlot_iife_min_js_start[];
    extern const uint8_t _binary_uPlot_iife_min_js_end[];
    extern const uint8_t _binary_uPlot_min_css_start[];
    extern const uint8_t _binary_uPlot_min_css_end[];
}

// GET /littlefs file handler
static esp_err_t get_littlefs_file_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();
  
  const char *uri = req->uri;
  
  // Serve pre-embedded static assets directly from flash .rodata to eliminate flash I/O latency
  if (strcmp(uri, "/uPlot.iife.min.js") == 0) {
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=31536000, public");
    httpd_resp_send(req, (const char *)_binary_uPlot_iife_min_js_start,
                    _binary_uPlot_iife_min_js_end - _binary_uPlot_iife_min_js_start);
    return ESP_OK;
  }
  if (strcmp(uri, "/uPlot.min.css") == 0) {
    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=31536000, public");
    httpd_resp_send(req, (const char *)_binary_uPlot_min_css_start,
                    _binary_uPlot_min_css_end - _binary_uPlot_min_css_start);
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Requested LittleFS file: %s", uri);
  
  char filepath[128];
  snprintf(filepath, sizeof(filepath), "/littlefs%s", uri);
  
  FILE *f = fopen(filepath, "r");
  if (f == NULL) {
    ESP_LOGE(TAG, "File not found: %s", filepath);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_FAIL;
  }
  
  if (strstr(uri, ".css") != NULL) {
    httpd_resp_set_type(req, "text/css");
  } else if (strstr(uri, ".js") != NULL) {
    httpd_resp_set_type(req, "application/javascript");
  } else if (strstr(uri, ".html") != NULL) {
    httpd_resp_set_type(req, "text/html");
  } else {
    httpd_resp_set_type(req, "application/octet-stream");
  }
  
  httpd_resp_set_hdr(req, "Cache-Control", "max-age=31536000, public");
  
  char chunk[4096];
  size_t read_bytes;
  while ((read_bytes = fread(chunk, 1, sizeof(chunk), f)) > 0) {
    if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
      fclose(f);
      ESP_LOGE(TAG, "Failed to send chunk");
      return ESP_FAIL;
    }
  }
  
  httpd_resp_send_chunk(req, NULL, 0);
  fclose(f);
  return ESP_OK;
}

// GET / index handler
static esp_err_t get_root_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();
  httpd_resp_set_type(req, "text/html; charset=utf-8");

  char accept_encoding[64] = {0};
  if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", accept_encoding, sizeof(accept_encoding)) == ESP_OK &&
      strstr(accept_encoding, "gzip") != NULL) {
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *)g_dashboard_html_gz, g_dashboard_html_gz_len);
  } else {
    httpd_resp_send(req, g_dashboard_html, strlen(g_dashboard_html));
  }
  return ESP_OK;
}

// LLM & AI Agent Documentation adhering to the llms.txt standard
static const char g_llms_txt[] = R"llm(# WeatherXM Gateway (WG1200) Local HTTP API

> WeatherXM local weather gateway (WG1200) providing real-time and historical weather telemetry, LoRa RF diagnostics, station pairing & downlink controls, Meshtastic/MeshCore mesh broadcasting, and microSD CSV logging over local REST HTTP endpoints and Zeroconf mDNS.

## Agent Operating Guidelines

- **Base URL & Authentication**: Plain HTTP on port `80` (`http://<GATEWAY_IP>`). No authentication, cookies, or API keys are required.
- **CORS**: `Access-Control-Allow-Origin: *` is set on `/llms.txt`, `/api/v1/*`, `/history`, `/live_trends`, and `/sd/*`.
- **Control Parameters**: `POST` endpoints accept parameters via **URL query string** (e.g., `POST /set_pair_mode?mode=scan&type=1300`). `/api/v1/weather_policy` also accepts a JSON `POST` body.
- **Units & Null Handling**: Weather observations in `/api/v1/observations`, `/status`, `/api/v1/history`, and `/live_trends` are returned in the gateway's active user units (reported in the `units` object). If no weather packet has been received yet (`"has_weather": false`) or a sensor reading is invalid, numeric fields are serialized as `null`.
- **Asynchronous WS1300 Downlink Commands**: The WS1300 weather station wakes to transmit every ~20 seconds and listens only briefly after each uplink. Station-targeted commands (`/remote_reboot`, `/request_gps`, `/ws1300_kick_station`, `/ws1300_station_meshtastic`, `/ws1300_station_meshcore`, and weather policy sync) are **queued/armed** on the gateway (`*_pending: true` in `/status`) and delivered automatically on the next station check-in.

## Zeroconf / mDNS Discovery

- **Service Type**: `_weatherxm._tcp`
- **Port**: `80`
- **Domain**: `local.`
- **Advertised TXT Records**:
  - `id`: Stable unique gateway identifier (e.g. `"WXM-12345678"`)
  - `gateway_id`: Alias for `id`
  - `model`: Gateway hardware model name (e.g. `"WXM-WG1200"`)
  - `api_version`: SemVer API version (`"1.0.0"`)
  - `sw_version`: Firmware version string (e.g. `"0.8.22"`)

## Core Telemetry, History & RF Diagnostic Endpoints (`GET`)

### 1. Gateway Info (`GET /api/v1/info`)
Lightweight gateway identity, Wi-Fi state, paired station summary, mesh toggles, and active measurement units:
```json
{
  "id": "WXM-12345678",
  "gateway_id": "WXM-12345678",
  "model": "WXM-WG1200",
  "hardware": "WXM-WG1200",
  "firmware": "0.8.22",
  "api_version": "1.0.0",
  "device_name": "WeatherXM Gateway",
  "mac": "24:DC:C3:XX:XX:XX",
  "uptime_sec": 86400,
  "edition": "wxm",
  "freq_region": "EU868",
  "station": {
    "is_paired": true,
    "pair_mode": "CLOUD",
    "paired_id": 142857,
    "station_type": 1300,
    "station_fw_version": 18,
    "packet_reception": 98
  },
  "wifi": {
    "status": "CONNECTED",
    "ssid": "MyWiFi",
    "rssi": -58,
    "ip": "192.168.1.100"
  },
  "mesh": {
    "meshtastic_enabled": false,
    "meshcore_enabled": false
  },
  "units": {
    "temp": "°C",
    "wind": "m/s",
    "precip": "mm",
    "press": "hPa"
  }
}
```

### 2. Real-Time Weather Observations (`GET /api/v1/observations` or `/api/v1/observation`)
Returns the latest decoded weather observation:
```json
{
  "timestamp": 1726358400,
  "has_weather": true,
  "temp": 21.50,
  "feels_like": 21.50,
  "humidity": 55.0,
  "pressure": 1013.25,
  "wind_speed": 3.20,
  "wind_gust": 4.50,
  "wind_dir_deg": 180.0,
  "wind_dir_cardinal": "S",
  "precip_rate": 0.00,
  "precip_daily": 0.00,
  "precip_cml": 12.50,
  "precip_24h": 2.40,
  "uv": 3,
  "irradiance": 450.00,
  "unit_temp": "°C",
  "unit_wind": "m/s",
  "unit_precip": "mm",
  "unit_press": "hPa",
  "units": { "temp": "°C", "wind": "m/s", "precip": "mm", "press": "hPa" }
}
```
- **15-Minute Rolling Raw Buffer (`GET /api/v1/observations?buffer=full`)**: Streams all raw observations from the last 15 minutes (~20s resolution) via HTTP chunked transfer:
  ```json
  {
    "count": 45,
    "buffer": [
      {
        "timestamp": 1726357500,
        "temp": 21.40,
        "feels_like": 21.40,
        "humidity": 55.0,
        "pressure": 1013.20,
        "wind_speed": 2.90,
        "wind_gust": 4.10,
        "wind_dir_deg": 175.0,
        "wind_dir_cardinal": "S",
        "precip_rate": 0.00,
        "precip_daily": 0.00,
        "precip_cml": 12.50,
        "precip_24h": 2.40,
        "uv": 3,
        "irradiance": 440.00
      }
    ]
  }
  ```

### 3. 24-Hour Historical Time Series (`GET /api/v1/history` or `/history`)
Returns up to 288 5-minute aggregated buckets over the last 24 hours in **columnar parallel-array format** (all arrays have identical length; missing readings are `null`):
```json
{
  "time": [1726272000, 1726272300],
  "temp": [18.2, 18.4],
  "hum": [68.0, 67.5],
  "wind": [2.1, 2.3],
  "gust": [3.5, 4.1],
  "wind_dir": [210.0, 215.0],
  "precip": [0.00, 0.00],
  "precip_accum": [1.20, 1.20],
  "press": [1012.8, 1012.9],
  "solar": [120.0, 135.0]
}
```

### 4. High-Resolution 15-Minute Live Trends (`GET /live_trends`)
Returns the 15-minute rolling buffer (~20s cadence) as parallel arrays:
```json
{
  "time": [1726357500, 1726357520],
  "temp": [21.4, 21.5],
  "wind_speed": [2.9, 3.2],
  "wind_dir": [175.0, 180.0],
  "wind_gust": [4.1, 4.5],
  "humidity": [55.0, 55.0],
  "pressure": [1013.2, 1013.3],
  "solar_rad": [440.0, 450.0],
  "precip_rate": [0.00, 0.00]
}
```

### 5. Comprehensive Gateway & RF Status (`GET /status`)
Single-call diagnostic payload combining system health, Wi-Fi, MQTT uplink, pairing & ownership state machine, discovered stations, radio tuning, Mesh/Adaptive policy state, microSD status, units, and the current `.weather` snapshot:
- **System & Connectivity**: `wifi_status`, `wifi_ssid`, `wifi_rssi`, `uptime_sec`, `fw_version`, `dev_name`, `edition` (`"wxm"`|`"open"`), `is_open_edition`, `is_screenless`, `mqtt_status`, `mqtt_success_count`, `mqtt_fail_count`, `mqtt_disabled`.
- **Pairing & Link Health**: `is_paired`, `paired_id`, `station_type` (`1300`|`1001`|`0`), `station_fw_ver`, `pair_mode` (`"OFF"`|`"CLOUD"`|`"LOCAL"`|`"SCAN"`), `pair_source`, `active_source`, `operation_mode`, `pairing_state`, `ownership_status`, `active_link`, `lora_link_ok`, `packet_reception` (0–100%), `activity_status`, `activity_duration_sec`, `rx_packet_total`.
- **Profiles & Scanner**: `local_ws_id`, `local_ws_sn`, `local_ws_type`, `local_profile_ready`, `cloud_ws_id`, `cloud_ws_sn`, `cloud_ws_type`, `cloud_profile_ready`, `cloud_ws_match`, `fallback_phase`, `fallback_sec_remaining`, `scan_active`, `scan_slot`, `scan_total_slots`, `scan_eta_sec`, `scan_profile`, `scan_phase`, `recent_stations` (array of `{id, type, sn, has_sn, has_pubkey, pubkey, pairable, packet_type, age_sec, rssi, snr, channel_slot}`).
- **Radio & Pending WS1300 Downlink Commands**: `freq_region` (`868`|`915`|`923`|`916`|`865`), `region_base_hz`, `listen_freq_hz`, `listen_desc`, `manual_slot`, `sniff_mode`, `tx_power_override`, `tx_interval_override`, `ws1300_reboot_pending`, `ws1300_gps_req_pending`, `ws1300_kick_armed`.
- **Mesh & Adaptive Policy**: `station_meshtastic_interval_min`, `station_meshtastic_preset`, `station_meshtastic_hop_limit`, `station_meshtastic_command_pending`, `mesh_forwarding_enabled`, `station_meshcore_interval_min`, `station_meshcore_profile_idx`, `station_meshcore_command_pending`, `policy_ver`, `policy_crc`, `policy_crc_hex`, `station_policy_pending`, `mt_events_hour`, `mt_cooldown_lvl`, `mt_last_reasons`, `mc_events_hour`, `mc_cooldown_lvl`, `mc_last_reasons`.
- **microSD & Weather**: `sd_mounted`, `sd_present`, `sd_ejected`, `sd_unusable`, `sd_capacity_gb`, `sd_fs_type`, `sd_status_reason`, `units`, `pressure_hpa` (always hPa), and `weather` (`has_weather`, `temp`, `feels_like`, `humidity`, `precip_rate`, `precip_daily`, `precip_cml`, `precip_24h`, `pressure`, `wind_speed`, `wind_gust`, `wind_dir_deg`, `wind_dir_cardinal`, `uv`, `irradiance`, `unit_temp`, `unit_wind`, `unit_precip`, `unit_press`).

### 6. Live LoRa Packet Log (`GET /packet_log`)
Returns a JSON array of the last 20 demodulated or transmitted LoRa packets:
```json
[
  {
    "time": 1726358400,
    "rssi": -72,
    "snr": 9,
    "filtered": false,
    "is_tx": false,
    "tx_power_dbm": 0,
    "category": "PAIRED",
    "data": "a301...",
    "decoded": { "Temperature": 21.50, "Humidity": 55.00 },
    "station_type": "WS1300",
    "station_id": "0x00022E09",
    "protocol_version": "v18 (0x12)",
    "channel_slot": 3,
    "freq_hz": 868100000,
    "freq_str": "868.10 MHz",
    "specs_str": "SF9 · BW 125kHz · CR 4/5",
    "packet_type": "Sensors"
  }
]
```
- `category` values: `"PAIRED"`, `"UNPAIRED"`, `"NEIGHBOR"`, `"GW_TX"`, `"MESH_TX"`, `"MESHCORE_TX"`, `"MESH_RX"`, `"MESHCORE_RX"`.

## Configuration & Control Endpoints (`GET` / `POST`)

### Measurement Units
- `GET /api/v1/units` — Returns active unit IDs and symbols (`temp`, `wind`, `precip`, `press`).
- `POST /api/v1/units` (alias `POST /set_units`) — Updates display and API telemetry units via query params:
  - `temp`: `C` | `F` (or `0` | `1`)
  - `wind`: `m/s` | `km/h` | `mph` | `ft/s` | `bft` | `kn` (or `0`..`5`)
  - `precip`: `mm` | `in` (or `0` | `1`)
  - `press`: `hPa` | `mmHg` | `inHg` (or `0`..`2`)
  - Example: `curl -X POST "http://<GATEWAY_IP>/api/v1/units?temp=C&wind=km/h&precip=mm&press=hPa"`

### Pairing & Station Management
- `POST /set_pair_mode?mode=off|cloud|local|scan[&id=0x...&slot=0..8&type=1300|1001|all]` — Switches gateway pairing mode:
  - `cloud`: Follow WeatherXM Cloud station assignment (falls back to scan/local if unconfigured).
  - `local`: Lock onto the saved local station profile.
  - `scan`: Actively sweep channels to discover and auto-pair a station (optionally filtered by `id`, `slot`, and `type`).
  - `off`: Disable pairing.
- `POST /pair_local?id=0x...&type=1300|1001[&slot=0..8]` — Saves a discovered station (from `/status` `recent_stations`) into LOCAL NVS and switches immediately to `LOCAL` mode.
- `POST /copy_cloud_to_local` — Copies the provisioned CLOUD station profile into LOCAL NVS and activates `LOCAL` mode.
- `POST /unpair` — Unpairs the current station and resets receiver profile to `ALL`.
- `POST /force_pair?id=0x...[&sn=<20-hex>&pubkey=<128-hex>]` — Diagnostic direct LOCAL profile creation for a WS1300 station ID without waiting for an over-the-air `BOOT`/`ADV` handshake.
- `POST /clear_local` — Clears the saved LOCAL NVS station profile (`ws_id`, `ws_sn`, `ws_pubkey`) and switches to `CLOUD` if a Cloud profile is configured (or unpairs if not).
- `POST /clear_recent` — Clears the `recent_stations` discovery list.
- `POST /set_cloud_pairing?enable=1|0` — Enables or disables cloud pairing mode.
- `POST /toggle_autopair[?enable=1|0]` — Enables or disables automatic scan pairing.

### Radio, Sniffer & WS1300 Downlink Controls
- `POST /set_region?region=868|915|923|916|865` — Sets LoRa ISM frequency region (`868`=EU868, `915`=US915, `923`=AS923, `916`=IL916, `865`=IN865) and retunes the radio.
- `POST /toggle_sniff` — Toggles RF sniffer mode (`{"sniff_mode": true|false}`) to decode all heard stations.
- `POST /set_station?type=1300|1001|0` — Sets receiver profile when unpaired (`1300`=WS1300, `1001`=WS1001, `0`=all). Use `?scope=1300|1001|0` to change scan scope.
- `POST /set_channel?slot=-1..8` — Locks receiver to a specific channel slot (`-1`=auto, `0`=WS1001 base, `1..8`=WS1300 ch 0–7).
- `POST /set_tx_power?dbm=0..22` — Overrides paired WS1300 TX power (`0`=auto adaptive algorithm, or fixed dBm).
- `POST /set_tx_interval?sec=0|20..240` — Overrides paired WS1300 uplink interval in multiples of 20s (`0`=default 20s).
- `POST /remote_reboot?enable=1|0` — Arms (`1`) or cancels (`0`) a remote reboot command for the paired WS1300 (delivered on next check-in).
- `POST /request_gps` — Arms an immediate GNSS fix acquisition command for the paired WS1300 (delivered on next check-in).
- `POST /ws1300_kick_station[?id=0x...&cancel=1]` — Arms a one-shot LoRa reboot for a sniffed WS1300 paired to this gateway's serial so it emits a pairable `BOOT` frame.
- `POST /toggle_mqtt` — Toggles WeatherXM MQTT cloud telemetry (Open Edition firmware only; returns `403` on WXM edition).

### Meshtastic, MeshCore & Adaptive Weather Policy
- `GET /api/v1/meshtastic/config` — Returns gateway & station Meshtastic configuration, regional presets, and adaptive evaluator counters.
- `POST /api/v1/meshtastic/config?enabled=true|false&interval=60..3600&preset=0..8&hop_limit=0..7&multi_fwd=true|false[&freq_hz=...&bw_khz=...&sf=7..12&cr=5..8&tx_power=-9..22]` — Configures gateway Meshtastic broadcasting (`preset=8` enables custom LoRa parameters).
- `POST /api/v1/meshtastic/send_telemetry` (alias `/api/v1/mesh/send_telemetry`, accepts `GET` or `POST`, optional `?node_info=true&position=true`) — Immediately transmits a Meshtastic telemetry burst over LoRa.
- `POST /ws1300_station_meshtastic?interval=0..60[&cancel=1]` — Queues a command to set the autonomous Meshtastic beacon interval in minutes (`0`=off) directly on a paired WS1300 station (requires WS1300 FW v15+).
- `GET /api/v1/meshcore/config` — Returns gateway & station MeshCore configuration, regional profiles, and adaptive evaluator counters.
- `POST /api/v1/meshcore/config?gw_enabled=true|false&gw_interval=60..3600&profile=0..10[&freq_hz=...&bw_khz=...&sf=7..12&cr=5..8&tx_power=-9..22]` — Configures gateway MeshCore broadcasting (`profile=10` enables custom LoRa parameters).
- `POST /api/v1/meshcore/send_telemetry` (accepts `GET` or `POST`) — Immediately transmits a MeshCore `GRP_DATA` weather frame.
- `POST /ws1300_station_meshcore?interval=0..60[&cancel=1]` — Queues a command to set the autonomous MeshCore beacon interval in minutes (`0`=off) on a paired WS1300 station (requires WS1300 FW v16+).
- `GET /api/v1/weather_policy` & `POST /api/v1/weather_policy` — Reads or updates the adaptive weather-change trigger policy used for event-driven mesh broadcasts (supports `?reset=1` to restore defaults, `?cancel_sync=1` to disarm station sync, or query/JSON threshold fields).

### microSD Card Logging (Display Gateways)
- `GET /sd/status` — Returns card presence, mount state, filesystem type, capacity, free space, active daily CSV filename, and logged row count.
- `GET /sd/list` — Lists daily `.csv` log files (`{"mounted":true,"files":[{"name":"2026-10-24.csv","size":14200}]}`).
- `GET /sd/download?file=<name.csv>[&preview=1]` — Downloads a daily CSV weather log (`preview=1` streams up to 64 KB as `text/plain`).
- `GET /sd/view?file=<name.csv>` — Interactive HTML/uPlot chart viewer for a CSV log file.
- `POST /sd/mount` & `POST /sd/eject` — Remounts or safely unmounts the microSD card for removal.
- `POST /sd/delete?file=<name.csv>` — Deletes a specific CSV file.
- `POST /sd/format` — Formats the microSD card as FAT32 (destructive).
- `GET /sd/diagnose` & `GET /sd/test` — Runs SD bus pin diagnostics or a read/write verification test.

## Home Assistant Integration

### Auto-Discovery (Recommended)
Home Assistant automatically discovers `_weatherxm._tcp.local.` via Zeroconf. Confirm setup in **Settings > Devices & Services**.

### Manual REST Sensor Configuration (`configuration.yaml`)
```yaml
rest:
  - resource: http://<GATEWAY_IP>/api/v1/observations
    scan_interval: 20
    sensor:
      - name: "Weather Station Temperature"
        value_template: "{{ value_json.temp }}"
        unit_of_measurement: "°C"
        device_class: temperature
        state_class: measurement
      - name: "Weather Station Feels Like"
        value_template: "{{ value_json.feels_like }}"
        unit_of_measurement: "°C"
        device_class: temperature
        state_class: measurement
      - name: "Weather Station Humidity"
        value_template: "{{ value_json.humidity }}"
        unit_of_measurement: "%"
        device_class: humidity
        state_class: measurement
      - name: "Weather Station Pressure"
        value_template: "{{ value_json.pressure }}"
        unit_of_measurement: "hPa"
        device_class: atmospheric_pressure
        state_class: measurement
      - name: "Weather Station Wind Speed"
        value_template: "{{ value_json.wind_speed }}"
        unit_of_measurement: "m/s"
        device_class: wind_speed
        state_class: measurement
      - name: "Weather Station Wind Gust"
        value_template: "{{ value_json.wind_gust }}"
        unit_of_measurement: "m/s"
        device_class: wind_speed
        state_class: measurement
      - name: "Weather Station Wind Direction"
        value_template: "{{ value_json.wind_dir_deg }}"
        unit_of_measurement: "°"
        state_class: measurement
      - name: "Weather Station Rain Rate"
        value_template: "{{ value_json.precip_rate }}"
        unit_of_measurement: "mm/h"
        device_class: precipitation_intensity
        state_class: measurement
      - name: "Weather Station Rain Daily"
        value_template: "{{ value_json.precip_daily }}"
        unit_of_measurement: "mm"
        device_class: precipitation
        state_class: total
      - name: "Weather Station Rain Total"
        value_template: "{{ value_json.precip_cml }}"
        unit_of_measurement: "mm"
        device_class: precipitation
        state_class: total_increasing
      - name: "Weather Station Solar Radiation"
        value_template: "{{ value_json.irradiance }}"
        unit_of_measurement: "W/m²"
        device_class: irradiance
        state_class: measurement
      - name: "Weather Station UV Index"
        value_template: "{{ value_json.uv }}"
        state_class: measurement
```
)llm";

// GET /llms.txt and /llms-full.txt handler
static esp_err_t get_llms_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, g_llms_txt, strlen(g_llms_txt));
  return ESP_OK;
}


// POST /ota update handler
static esp_err_t post_ota_handler(httpd_req_t *req) {
  if (!Submit::try_lock_ota()) {
    ESP_LOGW(TAG, "Rejecting Web OTA: another OTA operation is already in progress");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "OTA already in progress");
    return ESP_FAIL;
  }
  struct WebOtaLockGuard {
    ~WebOtaLockGuard() { Submit::unlock_ota(); }
  } web_ota_lock_guard;

  lv_port_show_ota_screen();

  esp_ota_handle_t update_handle = 0;
  const esp_partition_t *update_partition =
      esp_ota_get_next_update_partition(NULL);

  if (update_partition == NULL) {
    ESP_LOGE(TAG, "No update partition found");
    lv_port_resume();
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "No update partition");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Writing OTA update to partition %s (content_len: %d bytes)",
           update_partition->label, req->content_len);

  esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
    lv_port_resume();
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        esp_err_to_name(err));
    return ESP_FAIL;
  }

  char *rx_buf = (char *)malloc(4096);
  if (!rx_buf) {
    ESP_LOGE(TAG, "Failed to allocate 4KB OTA buffer");
    esp_ota_abort(update_handle);
    lv_port_resume();
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory Allocation Failed");
    return ESP_FAIL;
  }

  int total_written = 0;
  int remaining = req->content_len;
  bool is_first_chunk = true;
  int timeout_retries = 0;
  static constexpr int MAX_OTA_SOCK_TIMEOUT_RETRIES = 6;

  while (remaining > 0 || req->content_len == 0) {
    int to_read = (remaining > 0) ? MIN(remaining, 4096) : 4096;
    int received = httpd_req_recv(req, rx_buf, to_read);

    if (received <= 0) {
      if (received == HTTPD_SOCK_ERR_TIMEOUT) {
        if (++timeout_retries <= MAX_OTA_SOCK_TIMEOUT_RETRIES) {
          vTaskDelay(pdMS_TO_TICKS(10));
          continue;
        }
        ESP_LOGE(TAG, "OTA upload timed out after %d retries (%d bytes written)",
                 timeout_retries, total_written);
      }
      if (received == 0 && req->content_len == 0 && total_written > 0) {
        break;
      }
      ESP_LOGE(TAG, "File receive failed (%d), written so far: %d", received, total_written);
      free(rx_buf);
      esp_ota_abort(update_handle);
      lv_port_resume();
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Connection Lost");
      return ESP_FAIL;
    }
    timeout_retries = 0;

    if (is_first_chunk) {
      is_first_chunk = false;
      uint8_t magic = (uint8_t)rx_buf[0];
      ESP_LOGI(TAG, "OTA First byte: 0x%02X (expected 0xE9)", magic);
      if (magic != 0xE9) {
        ESP_LOGE(TAG, "Invalid OTA binary header magic: 0x%02X (expected 0xE9)", magic);
        free(rx_buf);
        esp_ota_abort(update_handle);
        lv_port_resume();
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid .bin (Header Magic)");
        return ESP_FAIL;
      }
    }

    err = esp_ota_write(update_handle, rx_buf, received);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_write failed (%s) after %d bytes", esp_err_to_name(err), total_written);
      free(rx_buf);
      esp_ota_abort(update_handle);
      lv_port_resume();
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
      return ESP_FAIL;
    }

    total_written += received;
    if (remaining > 0) {
      remaining -= received;
    }

    vTaskDelay(1);
  }

  free(rx_buf);
  ESP_LOGI(TAG, "OTA upload loop complete. Total written: %d bytes (content_len: %d)",
           total_written, (int)req->content_len);

  if (total_written < 100000) {
    ESP_LOGE(TAG, "OTA payload too small (%d bytes), aborting", total_written);
    esp_ota_abort(update_handle);
    lv_port_resume();
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File Too Small");
    return ESP_FAIL;
  }

  err = esp_ota_end(update_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_end failed (%s) [written: %d bytes]", esp_err_to_name(err), total_written);
    lv_port_resume();
    char err_msg[64];
    snprintf(err_msg, sizeof(err_msg), "End Failed (%s)", esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, err_msg);
    return ESP_FAIL;
  }

  err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)",
             esp_err_to_name(err));
    lv_port_resume();
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Set Boot Partition Failed");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "OTA Update complete. Rebooting...");
  httpd_resp_sendstr(req, "OK");

  vTaskDelay(pdMS_TO_TICKS(1000));
  Utils::safe_restart(Utils::RestartReason::WEB_OTA_UPDATE);
  return ESP_OK;
}

static esp_err_t post_set_region_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();
  char query[256] = {0};
  char region_str[32] = {0};
  esp_err_t query_err = httpd_req_get_url_query_str(req, query, sizeof(query));
  
  if (query_err == ESP_OK) {
    if (httpd_query_key_value(query, "region", region_str, sizeof(region_str)) != ESP_OK) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Region parameter missing");
      return ESP_FAIL;
    }
  } else {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No query parameters");
    return ESP_FAIL;
  }
  
  int region_val = atoi(region_str);
  if (region_val == 868 || region_val == 915 || region_val == 923 || region_val == 916 || region_val == 865) {
    Settings::set_freq_region((WsFreqRegion)region_val);
    Settings::commit_blocking(); // Save to NVS
    
    // Trigger radio retuning
    WsHandler::apply_station_change(WsHandler::g_receiver_profile);
    
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"region\":%d}", region_val);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
  } else {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid region value");
    return ESP_FAIL;
  }
}

static esp_err_t post_toggle_sniff_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();
  WsHandler::g_sniff_mode_enabled = !WsHandler::g_sniff_mode_enabled;
  ESP_LOGI(TAG, "Sniff Mode Toggled: %s",
           WsHandler::g_sniff_mode_enabled ? "ENABLED" : "DISABLED");

  // Note: sniff mode does not touch the receiver profile or the current
  // station. On a paired gateway the radio stays on the paired frequency and
  // sniffing only decodes other stations heard there; the frequency sweep
  // happens only when unpaired (handled by retune inside the notify below).
  WsHandler::notify_sniff_mode_toggled();

  char resp[128];
  snprintf(resp, sizeof(resp), "{\"sniff_mode\":%s}",
           WsHandler::g_sniff_mode_enabled ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

// Legacy endpoints (/toggle_autopair, /set_cloud_pairing, /unpair) mutate the
// individual flags directly; recompute and persist the pair mode from them so
// the persisted mode never contradicts the runtime state they created.
static void sync_pair_mode_from_flags(void) {
  PairMode mode;
  if (Settings::get_cloud_pairing_enabled()) {
    mode = PairMode::PAIR_MODE_CLOUD;
  } else if (WsHandler::get_pair_state() == WsHandler::PairState::PAIRED) {
    mode = PairMode::PAIR_MODE_LOCAL;
  } else if (WsHandler::g_auto_pair_enabled) {
    mode = PairMode::PAIR_MODE_SCAN;
  } else {
    mode = PairMode::PAIR_MODE_OFF;
  }
  Settings::set_pair_mode(mode);
  Settings::commit();
}

static esp_err_t post_set_pair_mode_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();

  char query[96] = {0};
  char mode_str[16] = {0};
  char id_str[16] = {0};
  char slot_str[8] = {0};
  char type_str[16] = {0};
  PairMode mode = PairMode::PAIR_MODE_NONE;
  uint32_t scan_target_id = 0;
  int8_t scan_target_slot = -1;
  WsType scan_profile = WsType::WS_TYPE_NONE;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    if (httpd_query_key_value(query, "mode", mode_str, sizeof(mode_str)) == ESP_OK) {
      mode = Utils::pair_mode_from_str(mode_str);
    }
    if (httpd_query_key_value(query, "id", id_str, sizeof(id_str)) == ESP_OK) {
      scan_target_id = (uint32_t)strtoul(id_str, NULL, 0);
    }
    if (httpd_query_key_value(query, "slot", slot_str, sizeof(slot_str)) == ESP_OK) {
      int parsed_slot = atoi(slot_str);
      if (parsed_slot < 0 || parsed_slot > 8) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Invalid slot (expected 0..8)");
        return ESP_FAIL;
      }
      scan_target_slot = (int8_t)parsed_slot;
    }
    if (httpd_query_key_value(query, "type", type_str, sizeof(type_str)) == ESP_OK) {
      int parsed_type = atoi(type_str);
      if (parsed_type == 1001 || strcasecmp(type_str, "ws1001") == 0) {
        scan_profile = WsType::WS_TYPE_1001;
      } else if (parsed_type == 1300 || strcasecmp(type_str, "ws1300") == 0) {
        scan_profile = WsType::WS_TYPE_1300;
      } else if (parsed_type == 0 || strcasecmp(type_str, "all") == 0 || strcasecmp(type_str, "combo") == 0) {
        scan_profile = WsType::WS_TYPE_ALL;
      }
    }
  }

  if (mode == PairMode::PAIR_MODE_NONE) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Invalid mode (expected off|cloud|local|scan)");
    return ESP_FAIL;
  }

  RetResult ret = WsHandler::apply_pair_mode(
      mode, scan_target_id, scan_target_slot, scan_profile);
  if (ret == RET_INVALID_STATE && mode == PairMode::PAIR_MODE_LOCAL && Settings::cloud_ws_profile_ready()) {
    // If local was not ready but cloud profile exists, copy cloud to local
    Settings::copy_cloud_profile_to_local();
    ret = WsHandler::apply_pair_mode(
        mode, scan_target_id, scan_target_slot, scan_profile);
  }
  if (ret == RET_INVALID_STATE) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "LOCAL requires a complete saved local profile (run SCAN first)");
    return ESP_FAIL;
  } else if (ret != RET_OK) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  // Manual mode selection via Web UI locks the fallback state machine so the
  // gateway stays on the user's chosen target without timing out after 300s.
  WsHandler::set_fallback_phase(WsHandler::AutoFallbackPhase::PHASE_MANUAL);

  ESP_LOGI(TAG, "Pair mode set via Web: %s (scan profile: %d)",
           Utils::pair_mode_to_str(mode), (int)WsHandler::g_scan_profile);

  char resp[200];
  const char *scan_p_str = (WsHandler::g_scan_profile == WsType::WS_TYPE_1001) ? "1001"
                         : (WsHandler::g_scan_profile == WsType::WS_TYPE_1300 ? "1300" : "all");
  snprintf(resp, sizeof(resp),
           "{\"pair_mode\":\"%s\",\"scan_target_id\":\"0x%08lX\",\"scan_profile\":\"%s\"}",
           Utils::pair_mode_to_str(mode), (unsigned long)WsHandler::g_scan_target_id,
           scan_p_str);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

// POST /ws1300_kick_station[?cancel=1]
// Arms a one-shot LoRa CMD-reboot for the next sniffed WS1300 packet. The
// station's protocol has no unpair command; rebooting it makes it transmit a
// pairable STA_BOOT. Delivery is HMAC-gated: only a station paired to THIS
// gateway's serial accepts the command. Use with the gateway in SCAN mode to
// exercise the full over-the-air pairing chain.
static esp_err_t post_ws1300_kick_station_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();

  bool arm = true;
  uint32_t target_id = 0; // 0 = first WS1300 heard
  char query[96] = {0};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char val_str[16] = {0};
    if (httpd_query_key_value(query, "cancel", val_str, sizeof(val_str)) == ESP_OK &&
        atoi(val_str) != 0) {
      arm = false;
    }
    // Optional station-ID filter, hex (0x...) or decimal
    if (httpd_query_key_value(query, "id", val_str, sizeof(val_str)) == ESP_OK) {
      target_id = (uint32_t)strtoul(val_str, NULL, 0);
    }
  }

  if (WsHandler::set_ws1300_kick_armed(arm, target_id) != RET_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Nothing is listening (enable sniffer or pair first)");
    return ESP_FAIL;
  }

  char resp[128];
  snprintf(resp, sizeof(resp), "{\"ws1300_kick_armed\":%s,\"target_id\":\"0x%08lX\"}",
           WsHandler::g_ws1300_kick_armed ? "true" : "false",
           (unsigned long)WsHandler::g_ws1300_kick_target_id);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

static esp_err_t post_toggle_autopair_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();
  
  bool enable_val = WsHandler::g_auto_pair_enabled; // Default to preserving current state
  bool has_param = false;
  char query[64] = {0};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char val_str[8] = {0};
    if (httpd_query_key_value(query, "enable", val_str, sizeof(val_str)) == ESP_OK) {
      enable_val = (atoi(val_str) != 0);
      has_param = true;
    }
  }
  
  if (!has_param) {
    int total_len = req->content_len;
    if (total_len > 0 && total_len < 32) {
      char body[32] = {0};
      int received = httpd_req_recv(req, body, total_len);
      if (received > 0) {
        body[received] = '\0';
        char *p = strchr(body, ':');
        enable_val = (atoi(p != NULL ? p + 1 : body) != 0);
        has_param = true;
      }
    }
  }

  // Only flip if absolutely no valid parameter was supplied
  if (!has_param) {
    enable_val = !WsHandler::g_auto_pair_enabled;
  }

  WsHandler::g_auto_pair_enabled = enable_val;
  ESP_LOGI(TAG, "Auto-Pair set to: %s",
           WsHandler::g_auto_pair_enabled ? "ENABLED" : "DISABLED");
  sync_pair_mode_from_flags();

  char resp[128];
  snprintf(resp, sizeof(resp), "{\"autopair_enabled\":%s}",
           WsHandler::g_auto_pair_enabled ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

static esp_err_t post_set_cloud_pairing_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();
  
  bool enable_val = Settings::get_cloud_pairing_enabled(); // Default to current state (enable cloud pairing)
  bool has_param = false;
  char query[64] = {0};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char val_str[8] = {0};
    if (httpd_query_key_value(query, "enable", val_str, sizeof(val_str)) == ESP_OK) {
      enable_val = (atoi(val_str) != 0);
      has_param = true;
    }
  }
  
  if (!has_param) {
    int total_len = req->content_len;
    if (total_len > 0 && total_len < 32) {
      char body[32] = {0};
      int received = httpd_req_recv(req, body, total_len);
      if (received > 0) {
        body[received] = '\0';
        char *p = strchr(body, ':');
        enable_val = (atoi(p != NULL ? p + 1 : body) != 0);
        has_param = true;
      }
    }
  }

  // Only flip if absolutely no valid parameter was supplied
  if (!has_param) {
    enable_val = !Settings::get_cloud_pairing_enabled(); // Toggle
  }

  Settings::set_cloud_pairing_enabled(enable_val);
  sync_pair_mode_from_flags();
  Settings::commit_blocking();
  ESP_LOGI(TAG, "Cloud Pairing set to: %s",
           Settings::get_cloud_pairing_enabled() ? "ENABLED" : "DISABLED");

  char resp[128];
  snprintf(resp, sizeof(resp), "{\"cloud_pairing_enabled\":%s}",
           Settings::get_cloud_pairing_enabled() ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

static esp_err_t post_toggle_mqtt_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();

  if (!Settings::is_open_edition()) {
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    const char *err_resp = "{\"error\":\"WeatherXM MQTT server cannot be disabled in WXM edition\",\"mqtt_disabled\":false}";
    httpd_resp_send(req, err_resp, strlen(err_resp));
    return ESP_OK;
  }

  bool is_disabled = !Settings::get_mqtt_disabled();
  Settings::set_mqtt_disabled(is_disabled);
  Settings::commit_blocking();

  ESP_LOGI(TAG, "MQTT Disabled state changed to: %s",
           is_disabled ? "DISABLED" : "ENABLED");

  if (is_disabled) {
    Tb::stop();
  } else {
    Tb::start();
  }

  char resp[128];
  snprintf(resp, sizeof(resp), "{\"mqtt_disabled\":%s}",
           is_disabled ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

// POST /set_channel handler - manual listen channel override (diagnostics).
// slot: -1 = AUTO (normal behavior), 0 = WS1001 base, 1-8 = WS1300 ch 0-7
static esp_err_t post_set_channel_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();

  int slot_val = -128;
  char query[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char slot_str[8] = {0};
    if (httpd_query_key_value(query, "slot", slot_str, sizeof(slot_str)) == ESP_OK) {
      slot_val = atoi(slot_str);
    }
  } else {
    int total_len = req->content_len;
    if (total_len > 0 && total_len < 32) {
      char body[32] = {0};
      int received = httpd_req_recv(req, body, total_len);
      if (received > 0) {
        body[received] = '\0';
        char *p = strchr(body, ':');
        slot_val = atoi(p != NULL ? p + 1 : body);
      }
    }
  }

  if (slot_val < -1 || slot_val > 8 ||
      WsHandler::set_manual_listen_slot((int8_t)slot_val) != RET_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid slot (-1..8)");
    return ESP_FAIL;
  }

  uint32_t listen_freq_hz = 0;
  char listen_desc[32] = "-";
  WsHandler::get_listen_info(&listen_freq_hz, listen_desc, sizeof(listen_desc));

  char resp[160];
  snprintf(resp, sizeof(resp),
           "{\"status\":\"ok\",\"manual_slot\":%d,\"listen_freq_hz\":%lu,"
           "\"listen_desc\":\"%s\"}",
           slot_val, (unsigned long)listen_freq_hz, listen_desc);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

// POST /remote_reboot - arm/disarm a remote reboot of the paired WS1300 over
// LoRa. enable=1 (default) arms it, enable=0 cancels a pending one. The
// station is request/response only (it initiates every exchange), so this
// cannot be pushed immediately: it stays armed - reported via /status as
// "ws1300_reboot_pending" - until actually delivered on the station's next
// packet exchange, at which point the gateway clears it automatically.
static esp_err_t post_remote_reboot_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();

  bool enable = true;
  char query[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char val_str[8] = {0};
    if (httpd_query_key_value(query, "enable", val_str, sizeof(val_str)) == ESP_OK) {
      enable = (atoi(val_str) != 0);
    }
  }

  // Set the reboot requested flag directly so that the operation always succeeds at the API layer.
  // When a WS1300 station eventually connects or if it is already paired, it will pick up this command.
  WsHandler::g_ws1300_reboot_requested = enable;
  ESP_LOGW(TAG, "Remote reboot requested via Web: %s", enable ? "ARMED" : "CANCELLED");

  char resp[96];
  snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"reboot_pending\":%s}",
           WsHandler::g_ws1300_reboot_requested ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

// POST /request_gps - arms an immediate GPS acquisition command for the next check-in
static esp_err_t post_request_gps_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();

  WsHandler::trigger_gps_request();

  char resp[96];
  snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"gps_request_armed\":true}");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

// POST /copy_cloud_to_local - copies cloud assignment profile to local pairing
static esp_err_t post_copy_cloud_to_local_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();

  RetResult ret = Settings::copy_cloud_profile_to_local();
  if (ret != RET_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No valid cloud profile to copy");
    return ESP_FAIL;
  }

  WsHandler::apply_pair_mode(PairMode::PAIR_MODE_LOCAL);
  WsHandler::set_fallback_phase(WsHandler::AutoFallbackPhase::PHASE_MANUAL);

  char resp[96];
  snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"copied\":true}");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

// POST /set_tx_power - manual TX power override for the paired WS1300.
// dbm: 0 = auto (gateway's adaptive ack-loss algorithm), else a fixed dBm
// value (clamped to the station's supported range).
static esp_err_t post_set_tx_power_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();

  int dbm_val = 0;
  char query[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char val_str[8] = {0};
    if (httpd_query_key_value(query, "dbm", val_str, sizeof(val_str)) == ESP_OK) {
      dbm_val = atoi(val_str);
    }
  }

  WsHandler::set_ws1300_tx_power_override((int16_t)dbm_val);

  char resp[96];
  snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"tx_power_override\":%d}",
           (int)WsHandler::g_ws1300_tx_power_override);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

// POST /set_tx_interval - manual TX interval override for the paired
// WS1300. sec: 0 = auto (fixed 20s), else desired interval in seconds
// (clamped to [20,240], rounded down to a multiple of 20).
static esp_err_t post_set_tx_interval_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();

  int sec_val = 0;
  char query[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char val_str[8] = {0};
    if (httpd_query_key_value(query, "sec", val_str, sizeof(val_str)) == ESP_OK) {
      sec_val = atoi(val_str);
    }
  }

  WsHandler::set_ws1300_tx_interval_override((int16_t)sec_val);

  char resp[96];
  snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"tx_interval_override\":%d}",
           (int)WsHandler::g_ws1300_tx_interval_override_sec);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

// POST /ws1300_station_meshtastic - station Meshtastic interval configuration (WS1300 v15+)
// params: interval (0 = off, else minutes)
static esp_err_t post_ws1300_station_meshtastic_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();

  WsBase::MultiStation *station = WsHandler::get_cur_station();
  if (WsHandler::get_pair_state() != WsHandler::PairState::PAIRED ||
      station == nullptr || station->TYPE != WsType::WS_TYPE_1300 ||
      Settings::get_pair_mode() == PairMode::PAIR_MODE_OFF) {
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req,
        "{\"status\":\"error\",\"message\":\"An active paired WS1300 is required\"}");
    return ESP_FAIL;
  }

  int interval_min = WsHandler::g_ws1300_meshtastic_interval_min;

  char query[256];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char cancel_str[16] = {0};
    if (httpd_query_key_value(query, "cancel", cancel_str, sizeof(cancel_str)) == ESP_OK && atoi(cancel_str) == 1) {
      WsHandler::g_ws1300_meshtastic_changed = false;
      char resp[128];
      snprintf(resp, sizeof(resp),
               "{\"status\":\"ok\",\"station_meshtastic_interval_min\":%u,\"pending\":false}",
               (unsigned int)WsHandler::g_ws1300_meshtastic_interval_min);
      httpd_resp_set_type(req, "application/json");
      httpd_resp_send(req, resp, strlen(resp));
      return ESP_OK;
    }
    char val_str[32] = {0};
    if (httpd_query_key_value(query, "interval", val_str, sizeof(val_str)) == ESP_OK) {
      interval_min = atoi(val_str);
    }
  }

  if (WsHandler::set_ws1300_station_meshtastic_interval((uint8_t)interval_min) != RET_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Invalid Meshtastic interval");
    return ESP_FAIL;
  }

  char resp[128];
  snprintf(resp, sizeof(resp),
           "{\"status\":\"ok\",\"station_meshtastic_interval_min\":%u,\"pending\":true}",
           (unsigned int)WsHandler::g_ws1300_meshtastic_interval_min);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

// POST /ws1300_station_meshcore - station MeshCore interval configuration (WS1300 v16+)
// params: interval (0 = off, else minutes), cancel (1 = disarm pending command)
static esp_err_t post_ws1300_station_meshcore_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();

  WsBase::MultiStation *station = WsHandler::get_cur_station();
  if (WsHandler::get_pair_state() != WsHandler::PairState::PAIRED ||
      station == nullptr || station->TYPE != WsType::WS_TYPE_1300 ||
      Settings::get_pair_mode() == PairMode::PAIR_MODE_OFF) {
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req,
        "{\"status\":\"error\",\"message\":\"An active paired WS1300 is required\"}");
    return ESP_FAIL;
  }

  int interval_min = WsHandler::g_ws1300_meshcore_interval_min;

  char query[256];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char cancel_str[16] = {0};
    if (httpd_query_key_value(query, "cancel", cancel_str, sizeof(cancel_str)) == ESP_OK && atoi(cancel_str) == 1) {
      WsHandler::g_ws1300_meshcore_changed = false;
      char resp[128];
      snprintf(resp, sizeof(resp),
               "{\"status\":\"ok\",\"station_meshcore_interval_min\":%u,\"pending\":false}",
               (unsigned int)WsHandler::g_ws1300_meshcore_interval_min);
      httpd_resp_set_type(req, "application/json");
      httpd_resp_send(req, resp, strlen(resp));
      return ESP_OK;
    }
    char val_str[32] = {0};
    if (httpd_query_key_value(query, "interval", val_str, sizeof(val_str)) == ESP_OK) {
      interval_min = atoi(val_str);
    }
  }

  if (WsHandler::set_ws1300_station_meshcore_interval((uint8_t)interval_min) != RET_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Invalid MeshCore interval");
    return ESP_FAIL;
  }

  char resp[128];
  snprintf(resp, sizeof(resp),
           "{\"status\":\"ok\",\"station_meshcore_interval_min\":%u,\"pending\":true}",
           (unsigned int)WsHandler::g_ws1300_meshcore_interval_min);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

static esp_err_t post_unpair_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();
  ESP_LOGI(TAG, "Unpairing weather station requested via Web.");
  WsHandler::unpair();

  WsHandler::g_receiver_profile = WsType::WS_TYPE_ALL;
  WsHandler::apply_station_change(WsType::WS_TYPE_ALL);
  ESP_LOGI(TAG, "Receiver Profile auto-set to ALL on unpair (transiently)");
  sync_pair_mode_from_flags();

  char resp[128];
  snprintf(resp, sizeof(resp), "{\"status\":\"unpaired\"}");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

// Diagnostic-only: create and select a LOCAL profile directly, bypassing the
// ADV/BOOT dance entirely. The cloud assignment remains unchanged.
// Valid because the WS1300 wire protocol only checks the 4-byte station ID for
// non-ADV packets (SENSORS/GNSS/BOOT), and every RESP's HMAC is keyed on this
// gateway's own serial only - the stored SN/pubkey are never used to build or
// verify ACK/CMD/TIME_SYNC. A real SN is only needed to know which channel to
// listen on (last SN byte mod 8); if omitted a placeholder is used and the
// existing base/channel probe finds the station the same way it always does.
static esp_err_t post_force_pair_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();
  char query[256] = {0};
  char val_str[128] = {0};

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "id", val_str, sizeof(val_str)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id parameter required");
    return ESP_FAIL;
  }
  uint32_t station_id = (uint32_t)strtoul(val_str, NULL, 0);

  uint8_t ws_sn[WS_SN_LEN];
  memset(ws_sn, 0xFF, sizeof(ws_sn));
  if (httpd_query_key_value(query, "sn", val_str, sizeof(val_str)) == ESP_OK) {
    if (Utils::hex_str_to_byte_array(val_str, ws_sn, sizeof(ws_sn)) != RetResult::RET_OK) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "sn must be 20 hex chars");
      return ESP_FAIL;
    }
  } else {
    // No real serial known: encode the station ID's low byte as the channel
    // selector so at least a targeted guess is possible, same convention as
    // the numeric ID itself (last SN byte mod 8 picks the channel).
    ws_sn[sizeof(ws_sn) - 1] = (uint8_t)(station_id & 0xFF);
  }

  uint8_t ws_pubkey[WS_PUB_KEY_LEN];
  memset(ws_pubkey, 0xFF, sizeof(ws_pubkey));
  if (httpd_query_key_value(query, "pubkey", val_str, sizeof(val_str)) == ESP_OK) {
    if (Utils::hex_str_to_byte_array(val_str, ws_pubkey, sizeof(ws_pubkey)) != RetResult::RET_OK) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pubkey must be 128 hex chars");
      return ESP_FAIL;
    }
  }

  Settings::set_ws_type(WsType::WS_TYPE_1300);
  Settings::set_ws_pubkey(ws_pubkey, sizeof(ws_pubkey));
  Settings::set_ws_sn(ws_sn, sizeof(ws_sn));
  Settings::set_ws_id((int64_t)station_id);
  Settings::set_pair_mode(PairMode::PAIR_MODE_LOCAL);
  Settings::commit_blocking();

  ESP_LOGW(TAG, "Force-paired (diagnostic) to station ID 0x%08lX", (unsigned long)station_id);

  char resp[128];
  snprintf(resp, sizeof(resp), "{\"force_paired_id\":\"0x%08lX\"}", (unsigned long)station_id);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

// User-triggered action: select a detected station and push it to LOCAL NVS,
// switching the gateway to LOCAL pairing mode immediately without touching CLOUD.
static esp_err_t post_pair_local_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();
  char query[128] = {0};
  char val_str[64] = {0};

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "id", val_str, sizeof(val_str)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id parameter required");
    return ESP_FAIL;
  }
  uint32_t station_id = (uint32_t)strtoul(val_str, NULL, 0);

  int station_type = 1300;
  if (httpd_query_key_value(query, "type", val_str, sizeof(val_str)) == ESP_OK) {
    station_type = atoi(val_str);
    if (strcasecmp(val_str, "ws1001") == 0) station_type = 1001;
    else if (strcasecmp(val_str, "ws1300") == 0) station_type = 1300;
  }

  int target_slot = -1;
  if (httpd_query_key_value(query, "slot", val_str, sizeof(val_str)) == ESP_OK) {
    target_slot = atoi(val_str);
  }

  // Look up in recent stations
  WsHandler::RecentStation recent[RECENT_STATIONS_CAPACITY] = {};
  size_t recent_count = WsHandler::get_recent_stations(recent, RECENT_STATIONS_CAPACITY);
  uint8_t ws_sn[WS_SN_LEN] = {0};
  uint8_t ws_pubkey[WS_PUB_KEY_LEN] = {0};
  bool has_sn = false;
  bool has_pubkey = false;

  for (size_t i = 0; i < recent_count; i++) {
    if (recent[i].id == station_id) {
      if (recent[i].type == WsType::WS_TYPE_1001) {
        station_type = 1001;
      } else if (recent[i].type == WsType::WS_TYPE_1300) {
        station_type = 1300;
      }
      if (recent[i].has_sn) {
        memcpy(ws_sn, recent[i].sn, sizeof(ws_sn));
        has_sn = true;
      }
      if (recent[i].has_pubkey) {
        memcpy(ws_pubkey, recent[i].pubkey, sizeof(ws_pubkey));
        has_pubkey = true;
      }
      if (target_slot < 0 && recent[i].channel_slot <= 8) {
        target_slot = recent[i].channel_slot;
      }
      break;
    }
  }

  WsType ws_t = (station_type == 1001) ? WsType::WS_TYPE_1001 : WsType::WS_TYPE_1300;
  Settings::set_ws_type(ws_t);
  Settings::set_ws_id((int64_t)station_id);

  if (ws_t == WsType::WS_TYPE_1300) {
    if (!has_sn) {
      // Synthesize serial matching profile_id_matches_sn and target slot
      ws_sn[0] = (uint8_t)((station_id >> 24) & 0xFF);
      ws_sn[1] = (uint8_t)((station_id >> 16) & 0xFF);
      ws_sn[2] = (uint8_t)((station_id >> 8) & 0xFF);
      ws_sn[3] = (uint8_t)(station_id & 0xFF);
      ws_sn[4] = 0xAA;
      ws_sn[5] = 0x55;
      ws_sn[6] = 0xAA;
      ws_sn[7] = 0x55;
      ws_sn[8] = 0x00;
      ws_sn[9] = (target_slot > 0) ? (uint8_t)(target_slot - 1) : 0;
    }
    if (!has_pubkey) {
      memset(ws_pubkey, 0xFF, sizeof(ws_pubkey));
    }
    Settings::set_ws_sn(ws_sn, sizeof(ws_sn));
    Settings::set_ws_pubkey(ws_pubkey, sizeof(ws_pubkey));
  } else {
    memset(ws_sn, 0, sizeof(ws_sn));
    Settings::set_ws_sn(ws_sn, sizeof(ws_sn));
  }

  Settings::set_pair_source(PairSource::PAIR_SOURCE_LOCAL);
  Settings::set_pair_mode(PairMode::PAIR_MODE_LOCAL);
  Settings::commit_blocking();

  RetResult ret = WsHandler::apply_pair_mode(PairMode::PAIR_MODE_LOCAL, station_id, (int8_t)target_slot, ws_t);
  WsHandler::set_fallback_phase(WsHandler::AutoFallbackPhase::PHASE_MANUAL);
  ESP_LOGI(TAG, "Pair to Local NVS: station ID 0x%08lX (type %d, slot %d) ret=%d",
           (unsigned long)station_id, station_type, target_slot, (int)ret);

  char resp[128];
  snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"id\":\"0x%08lX\",\"type\":%d}",
           (unsigned long)station_id, station_type);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

static esp_err_t post_clear_recent_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();
  WsHandler::clear_recent_stations();
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"status\":\"ok\"}", 15);
  return ESP_OK;
}

static esp_err_t post_clear_local_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();
  Settings::clear_local_ws_profile();
  Settings::commit_blocking();

  if (Settings::cloud_ws_profile_ready()) {
    WsHandler::apply_pair_mode(PairMode::PAIR_MODE_CLOUD, (uint32_t)Settings::get_cloud_ws_id());
    WsHandler::set_fallback_phase(WsHandler::AutoFallbackPhase::PHASE_MANUAL);
    ESP_LOGI(TAG, "Cleared Local NVS profile; switched active target to Cloud (0x%08X)",
             (unsigned int)Settings::get_cloud_ws_id());
  } else {
    WsHandler::unpair();
    ESP_LOGI(TAG, "Cleared Local NVS profile; no Cloud profile ready -> unpaired");
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"status\":\"ok\"}", 15);
  return ESP_OK;
}

static esp_err_t post_set_station_handler(httpd_req_t *req) {
  g_last_web_request_ts = Utils::get_tick_sec();
  char query[64] = {0};
  esp_err_t query_err = httpd_req_get_url_query_str(req, query, sizeof(query));

  if (query_err == ESP_OK) {
    char scope_str[16] = {0};
    if (httpd_query_key_value(query, "scope", scope_str, sizeof(scope_str)) == ESP_OK) {
      int scope_val = atoi(scope_str);
      WsType scan_t = WsType::WS_TYPE_ALL;
      if (scope_val == 1001) scan_t = WsType::WS_TYPE_1001;
      else if (scope_val == 1300) scan_t = WsType::WS_TYPE_1300;
      WsHandler::g_scan_profile = scan_t;
      if (WsHandler::is_scan_active() || Settings::get_pair_mode() == PairMode::PAIR_MODE_SCAN) {
        WsHandler::apply_pair_mode(PairMode::PAIR_MODE_SCAN, 0, -1, scan_t);
      }
      ESP_LOGI(TAG, "Scan scope updated to %d", scope_val);
      char resp[128];
      snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"scan_scope\":%d}", scope_val);
      httpd_resp_set_type(req, "application/json");
      httpd_resp_send(req, resp, strlen(resp));
      return ESP_OK;
    }
  }

  int station_type_val = -1;
  if (query_err == ESP_OK) {
    char type_str[16] = {0};
    if (httpd_query_key_value(query, "type", type_str, sizeof(type_str)) ==
        ESP_OK) {
      station_type_val = atoi(type_str);
    }
  } else {
    int total_len = req->content_len;
    if (total_len > 0 && total_len < 64) {
      char body[64] = {0};
      int received = httpd_req_recv(req, body, total_len);
      if (received > 0) {
        body[received] = '\0';
        if (strstr(body, "type") != NULL) {
          char *p = strchr(body, ':');
          if (p != NULL) {
            station_type_val = atoi(p + 1);
          }
        } else {
          station_type_val = atoi(body);
        }
      }
    }
  }

  if (station_type_val == 1001 || station_type_val == 1300 || station_type_val == 0) {
    WsType ws_type;
    if (station_type_val == 1001) {
      ws_type = WsType::WS_TYPE_1001;
    } else if (station_type_val == 1300) {
      ws_type = WsType::WS_TYPE_1300;
    } else {
      ws_type = WsType::WS_TYPE_ALL;
    }
    WsType prev_profile = WsHandler::g_receiver_profile;
    WsHandler::g_receiver_profile = ws_type;
    WsCommon::RetRes res = WsCommon::RetRes::OK;
    bool is_paired = (WsHandler::get_pair_state() == WsHandler::PairState::PAIRED);
    if (!is_paired) {
      res = WsHandler::apply_station_change(ws_type);
      if (res != WsCommon::RetRes::OK) {
        WsHandler::g_receiver_profile = prev_profile;
      }
    }
    if (res == WsCommon::RetRes::OK) {
      if (!is_paired) {
        Settings::set_ws_type(ws_type);
        Settings::commit_blocking();
      }
      ESP_LOGI(TAG, "Station type / receiver profile changed successfully to %d",
               station_type_val);
      char resp[128];
      snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"station_type\":%d}",
               station_type_val);
      httpd_resp_set_type(req, "application/json");
      httpd_resp_send(req, resp, strlen(resp));
      return ESP_OK;
    } else {
      ESP_LOGE(TAG, "apply_station_change returned error %d", (int)res);
    }
  }

  httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid station type");
  return ESP_FAIL;
}


static esp_err_t get_live_trends_handler(httpd_req_t *req) {
    auto raw_buf = WeatherLogger::get_raw_buffer();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    httpd_resp_send_chunk(req, "{\"time\":[", -1);
    for (size_t i = 0; i < raw_buf.size(); ++i) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%u%s", raw_buf[i].ts, (i == raw_buf.size() - 1) ? "" : ",");
        httpd_resp_send_chunk(req, buf, -1);
    }
    
    httpd_resp_send_chunk(req, "],\"temp\":[", -1);
    for (size_t i = 0; i < raw_buf.size(); ++i) {
        char buf[32];
        float val = 0.0f;
        if (WsFields::get_field_val_from_pack(&raw_buf[i], WsFields::FieldType::WEATHER_TEMP, NULL, &val) != WsCommon::RetRes::OK) {
            snprintf(buf, sizeof(buf), "null%s", (i == raw_buf.size() - 1) ? "" : ",");
        } else {
            snprintf(buf, sizeof(buf), "%.2f%s", val, (i == raw_buf.size() - 1) ? "" : ",");
        }
        httpd_resp_send_chunk(req, buf, -1);
    }
    
    httpd_resp_send_chunk(req, "],\"wind_speed\":[", -1);
    for (size_t i = 0; i < raw_buf.size(); ++i) {
        char buf[32];
        float val = 0.0f;
        if (WsFields::get_field_val_from_pack(&raw_buf[i], WsFields::FieldType::WEATHER_WIND_SPEED, NULL, &val) != WsCommon::RetRes::OK) {
            snprintf(buf, sizeof(buf), "null%s", (i == raw_buf.size() - 1) ? "" : ",");
        } else {
            snprintf(buf, sizeof(buf), "%.2f%s", val, (i == raw_buf.size() - 1) ? "" : ",");
        }
        httpd_resp_send_chunk(req, buf, -1);
    }
    
    httpd_resp_send_chunk(req, "],\"wind_dir\":[", -1);
    for (size_t i = 0; i < raw_buf.size(); ++i) {
        char buf[32];
        uint16_t val = 0;
        if (WsFields::get_field_val_from_pack(&raw_buf[i], WsFields::FieldType::WEATHER_WIND_DIR, NULL, &val) != WsCommon::RetRes::OK) {
            snprintf(buf, sizeof(buf), "null%s", (i == raw_buf.size() - 1) ? "" : ",");
        } else {
            snprintf(buf, sizeof(buf), "%u%s", val, (i == raw_buf.size() - 1) ? "" : ",");
        }
        httpd_resp_send_chunk(req, buf, -1);
    }

    httpd_resp_send_chunk(req, "],\"wind_gust\":[", -1);
    for (size_t i = 0; i < raw_buf.size(); ++i) {
        char buf[32];
        float val = 0.0f;
        if (WsFields::get_field_val_from_pack(&raw_buf[i], WsFields::FieldType::WEATHER_WIND_GUST, NULL, &val) != WsCommon::RetRes::OK) {
            snprintf(buf, sizeof(buf), "null%s", (i == raw_buf.size() - 1) ? "" : ",");
        } else {
            snprintf(buf, sizeof(buf), "%.2f%s", val, (i == raw_buf.size() - 1) ? "" : ",");
        }
        httpd_resp_send_chunk(req, buf, -1);
    }

    httpd_resp_send_chunk(req, "],\"humidity\":[", -1);
    for (size_t i = 0; i < raw_buf.size(); ++i) {
        char buf[32];
        float val = 0.0f;
        if (WsFields::get_field_val_from_pack(&raw_buf[i], WsFields::FieldType::WEATHER_HUM, NULL, &val) != WsCommon::RetRes::OK) {
            snprintf(buf, sizeof(buf), "null%s", (i == raw_buf.size() - 1) ? "" : ",");
        } else {
            snprintf(buf, sizeof(buf), "%.2f%s", val, (i == raw_buf.size() - 1) ? "" : ",");
        }
        httpd_resp_send_chunk(req, buf, -1);
    }

    httpd_resp_send_chunk(req, "],\"pressure\":[", -1);
    for (size_t i = 0; i < raw_buf.size(); ++i) {
        char buf[32];
        float val = 0.0f;
        if (WsFields::get_field_val_from_pack(&raw_buf[i], WsFields::FieldType::WEATHER_PRESSURE, NULL, &val) != WsCommon::RetRes::OK) {
            snprintf(buf, sizeof(buf), "null%s", (i == raw_buf.size() - 1) ? "" : ",");
        } else {
            snprintf(buf, sizeof(buf), "%.2f%s", val, (i == raw_buf.size() - 1) ? "" : ",");
        }
        httpd_resp_send_chunk(req, buf, -1);
    }

    httpd_resp_send_chunk(req, "],\"solar_rad\":[", -1);
    for (size_t i = 0; i < raw_buf.size(); ++i) {
        char buf[32];
        float val = 0.0f;
        if (WsFields::get_field_val_from_pack(&raw_buf[i], WsFields::FieldType::WEATHER_SOLAR_RAD, NULL, &val) != WsCommon::RetRes::OK) {
            snprintf(buf, sizeof(buf), "null%s", (i == raw_buf.size() - 1) ? "" : ",");
        } else {
            snprintf(buf, sizeof(buf), "%.2f%s", val, (i == raw_buf.size() - 1) ? "" : ",");
        }
        httpd_resp_send_chunk(req, buf, -1);
    }

    httpd_resp_send_chunk(req, "],\"precip_rate\":[", -1);
    for (size_t i = 0; i < raw_buf.size(); ++i) {
        char buf[32];
        float val = 0.0f;
        if (WsFields::get_field_val_from_pack(&raw_buf[i], WsFields::FieldType::WEATHER_PRECIP_RATE, NULL, &val) != WsCommon::RetRes::OK) {
            snprintf(buf, sizeof(buf), "null%s", (i == raw_buf.size() - 1) ? "" : ",");
        } else {
            snprintf(buf, sizeof(buf), "%.2f%s", val, (i == raw_buf.size() - 1) ? "" : ",");
        }
        httpd_resp_send_chunk(req, buf, -1);
    }
    
    httpd_resp_send_chunk(req, "]}", -1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}


static esp_err_t get_history_handler(httpd_req_t *req) {
    auto aggs = WeatherLogger::get_aggregates();
    httpd_resp_set_type(req, "application/json");
    
    httpd_resp_send_chunk(req, "{\"time\":[", -1);
    for (size_t i = 0; i < aggs.size(); ++i) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld%s", (long long)aggs[i].timestamp, (i == aggs.size() - 1) ? "" : ",");
        httpd_resp_send_chunk(req, buf, -1);
    }
    
    httpd_resp_send_chunk(req, "],\"temp\":[", -1);
    for (size_t i = 0; i < aggs.size(); ++i) {
        char buf[32];
        if (std::isnan(aggs[i].temp_avg)) {
            snprintf(buf, sizeof(buf), "null%s", (i == aggs.size() - 1) ? "" : ",");
        } else {
            snprintf(buf, sizeof(buf), "%.2f%s", aggs[i].temp_avg, (i == aggs.size() - 1) ? "" : ",");
        }
        httpd_resp_send_chunk(req, buf, -1);
    }
    
    httpd_resp_send_chunk(req, "],\"hum\":[", -1);
    for (size_t i = 0; i < aggs.size(); ++i) {
        char buf[32];
        if (std::isnan(aggs[i].hum_avg)) {
            snprintf(buf, sizeof(buf), "null%s", (i == aggs.size() - 1) ? "" : ",");
        } else {
            snprintf(buf, sizeof(buf), "%.1f%s", aggs[i].hum_avg, (i == aggs.size() - 1) ? "" : ",");
        }
        httpd_resp_send_chunk(req, buf, -1);
    }
    
    httpd_resp_send_chunk(req, "],\"wind\":[", -1);
    for (size_t i = 0; i < aggs.size(); ++i) {
        char buf[32];
        if (std::isnan(aggs[i].wind_speed_avg)) {
            snprintf(buf, sizeof(buf), "null%s", (i == aggs.size() - 1) ? "" : ",");
        } else {
            snprintf(buf, sizeof(buf), "%.2f%s", aggs[i].wind_speed_avg, (i == aggs.size() - 1) ? "" : ",");
        }
        httpd_resp_send_chunk(req, buf, -1);
    }
    
    httpd_resp_send_chunk(req, "],\"gust\":[", -1);
    for (size_t i = 0; i < aggs.size(); ++i) {
        char buf[32];
        if (std::isnan(aggs[i].wind_gust_max)) {
            snprintf(buf, sizeof(buf), "null%s", (i == aggs.size() - 1) ? "" : ",");
        } else {
            snprintf(buf, sizeof(buf), "%.2f%s", aggs[i].wind_gust_max, (i == aggs.size() - 1) ? "" : ",");
        }
        httpd_resp_send_chunk(req, buf, -1);
    }
    
    httpd_resp_send_chunk(req, "],\"wind_dir\":[", -1);
    for (size_t i = 0; i < aggs.size(); ++i) {
        char buf[32];
        if (std::isnan(aggs[i].wind_dir_prevailing)) {
            snprintf(buf, sizeof(buf), "null%s", (i == aggs.size() - 1) ? "" : ",");
        } else {
            snprintf(buf, sizeof(buf), "%.1f%s", aggs[i].wind_dir_prevailing, (i == aggs.size() - 1) ? "" : ",");
        }
        httpd_resp_send_chunk(req, buf, -1);
    }
    
    httpd_resp_send_chunk(req, "],\"precip\":[", -1);
    for (size_t i = 0; i < aggs.size(); ++i) {
        char buf[32];
        if (std::isnan(aggs[i].precip_avg)) {
            snprintf(buf, sizeof(buf), "null%s", (i == aggs.size() - 1) ? "" : ",");
        } else {
            snprintf(buf, sizeof(buf), "%.2f%s", aggs[i].precip_avg, (i == aggs.size() - 1) ? "" : ",");
        }
        httpd_resp_send_chunk(req, buf, -1);
    }
    
    httpd_resp_send_chunk(req, "],\"precip_accum\":[", -1);
    for (size_t i = 0; i < aggs.size(); ++i) {
        char buf[32];
        if (std::isnan(aggs[i].precip_accum)) {
            snprintf(buf, sizeof(buf), "0.00%s", (i == aggs.size() - 1) ? "" : ",");
        } else {
            snprintf(buf, sizeof(buf), "%.2f%s", aggs[i].precip_accum, (i == aggs.size() - 1) ? "" : ",");
        }
        httpd_resp_send_chunk(req, buf, -1);
    }
    
    httpd_resp_send_chunk(req, "],\"press\":[", -1);
    for (size_t i = 0; i < aggs.size(); ++i) {
        char buf[32];
        if (std::isnan(aggs[i].pressure_avg)) {
            snprintf(buf, sizeof(buf), "null%s", (i == aggs.size() - 1) ? "" : ",");
        } else {
            snprintf(buf, sizeof(buf), "%.2f%s", aggs[i].pressure_avg, (i == aggs.size() - 1) ? "" : ",");
        }
        httpd_resp_send_chunk(req, buf, -1);
    }
    
    httpd_resp_send_chunk(req, "],\"solar\":[", -1);
    for (size_t i = 0; i < aggs.size(); ++i) {
        char buf[32];
        if (std::isnan(aggs[i].solar_rad_avg)) {
            snprintf(buf, sizeof(buf), "null%s", (i == aggs.size() - 1) ? "" : ",");
        } else {
            snprintf(buf, sizeof(buf), "%.2f%s", aggs[i].solar_rad_avg, (i == aggs.size() - 1) ? "" : ",");
        }
        httpd_resp_send_chunk(req, buf, -1);
    }
    
    httpd_resp_send_chunk(req, "]}", -1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

#ifndef CONFIG_SCREENLESS
static std::string escape_json_string(const std::string &input) {
    std::string output;
    output.reserve(input.size() + 8);
    for (const char ch : input) {
        switch (ch) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) >= 0x20) output += ch;
            break;
        }
    }
    return output;
}

static bool is_safe_sd_filename(const char *filename) {
    if (filename == nullptr || filename[0] == '\0' || strlen(filename) >= 128 ||
        strstr(filename, "..") != nullptr) {
        return false;
    }
    for (const unsigned char *cursor =
             reinterpret_cast<const unsigned char *>(filename);
         *cursor != '\0'; ++cursor) {
        if (*cursor < 0x20 || *cursor == 0x7f || *cursor == '/' ||
            *cursor == '\\' || *cursor == '"') {
            return false;
        }
    }
    return true;
}

static bool has_csv_extension(const char *filename) {
    if (filename == nullptr) return false;
    const size_t length = strlen(filename);
    if (length < 4 || filename[length - 4] != '.') return false;
    const char c1 = filename[length - 3] | 0x20;
    const char c2 = filename[length - 2] | 0x20;
    const char c3 = filename[length - 1] | 0x20;
    return c1 == 'c' && c2 == 's' && c3 == 'v';
}

static esp_err_t get_sd_status_handler(httpd_req_t *req) {
    const SdLoggerStatus status = WeatherLogger::get_sd_status();
    const bool mounted = WeatherLogger::is_sd_mounted();
    const std::string escaped_error = escape_json_string(status.last_error);
    const std::string escaped_fs_type = escape_json_string(status.card_fs_type);
    const std::string escaped_reason = escape_json_string(status.card_status_reason);
    char response[1536];
    const int response_len = snprintf(
        response, sizeof(response),
        "{\"mounted\":%s,\"card_present\":%s,\"ejected\":%s,\"card_unusable\":%s,"
        "\"formatting\":%s,\"format_progress\":%u,"
        "\"card_capacity_gb\":%.2f,\"card_fs_type\":\"%s\",\"card_status_reason\":\"%s\","
        "\"writer_running\":%s,\"pending_records\":%u,"
        "\"weather_rows_written\":%lu,\"metadata_rows_written\":%lu,"
        "\"weather_rows_failed\":%lu,\"metadata_rows_failed\":%lu,"
        "\"retry_count\":%lu,\"last_success_timestamp\":%lld,"
        "\"last_error_timestamp\":%lld,\"mount_backoff_active\":%s,"
        "\"next_mount_attempt_timestamp\":%lld,\"last_error\":\"%s\"}",
        mounted ? "true" : "false",
        status.card_present ? "true" : "false",
        status.ejected ? "true" : "false",
        status.card_unusable ? "true" : "false",
        status.formatting ? "true" : "false",
        (unsigned int)status.format_progress,
        status.card_capacity_gb,
        escaped_fs_type.c_str(),
        escaped_reason.c_str(),
        status.writer_running ? "true" : "false",
        static_cast<unsigned int>(status.pending_records),
        (unsigned long)status.weather_rows_written,
        (unsigned long)status.metadata_rows_written,
        (unsigned long)status.weather_rows_failed,
        (unsigned long)status.metadata_rows_failed,
        (unsigned long)status.retry_count,
        (long long)status.last_success_timestamp,
        (long long)status.last_error_timestamp,
        status.mount_backoff_active ? "true" : "false",
        (long long)status.next_mount_attempt_timestamp,
        escaped_error.c_str());

    if (response_len < 0 || static_cast<size_t>(response_len) >= sizeof(response)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "SD status response too large");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, response, response_len);
}

static esp_err_t get_sd_list_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    if (!WeatherLogger::is_sd_mounted()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"error\":\"SD card is not mounted\"}");
        return ESP_OK;
    }
    auto files = WeatherLogger::list_sd_files();
    httpd_resp_send_chunk(req, "[", -1);
    for (size_t i = 0; i < files.size(); ++i) {
        const std::string entry =
            "{\"name\":\"" + escape_json_string(files[i].first) +
            "\",\"size\":" + std::to_string(files[i].second) + "}" +
            ((i == files.size() - 1) ? "" : ",");
        httpd_resp_send_chunk(req, entry.c_str(), entry.size());
    }
    httpd_resp_send_chunk(req, "]", -1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t get_sd_download_handler(httpd_req_t *req) {
    char query[256];
    char filename[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "file", filename, sizeof(filename)) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Filename parameter missing");
            return ESP_FAIL;
        }
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No query parameters");
        return ESP_FAIL;
    }
    if (!is_safe_sd_filename(filename)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }
    
    bsp_spi_lock();
    std::string filepath = "/sdcard/" + std::string(filename);
    FILE* f = fopen(filepath.c_str(), "r");
    if (f == nullptr) {
        bsp_spi_unlock();
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "text/csv");
    char header_val[192];
    snprintf(header_val, sizeof(header_val), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", header_val);
    
    char chunk[1024];
    size_t read_bytes;
    esp_err_t send_status = ESP_OK;
    while ((read_bytes = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
            send_status = ESP_FAIL;
            break;
        }
    }
    fclose(f);
    bsp_spi_unlock();

    if (send_status == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    } else {
        return ESP_FAIL;
    }
}

static esp_err_t get_sd_view_handler(httpd_req_t *req) {
    char query[256];
    char filename[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "file", filename, sizeof(filename)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "CSV filename parameter missing");
        return ESP_FAIL;
    }
    if (!is_safe_sd_filename(filename) || !has_csv_extension(filename)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "A valid CSV filename is required");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, g_sd_csv_viewer_html,
                           strlen(g_sd_csv_viewer_html));
}

static esp_err_t get_sd_diagnose_handler(httpd_req_t *req) {
    const SdLoggerStatus sd_st = WeatherLogger::get_sd_status();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (sd_st.formatting) {
        httpd_resp_sendstr(req, "{\"mounted\":false,\"present\":true,\"formatting\":true}");
        return ESP_OK;
    }
    if (sd_st.ejected) {
        const bool present = bsp_sdcard_is_inserted();
        char resp[192];
        snprintf(resp, sizeof(resp),
                 "{\"mounted\":false,\"present\":%s,\"ejected\":true,\"unusable\":false,\"status_reason\":\"Ejected (safe to remove)\"}",
                 present ? "true" : "false");
        httpd_resp_sendstr(req, resp);
        return ESP_OK;
    }

    char *diagnose_json = (char *)malloc(1024);
    if (!diagnose_json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    
    bsp_spi_lock();
    esp_err_t err = bsp_sdcard_diagnose(diagnose_json, 1024);
    bsp_spi_unlock();

    if (err == ESP_OK) {
        httpd_resp_sendstr(req, diagnose_json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Diagnostics failed");
    }
    
    free(diagnose_json);
    return ESP_OK;
}

static esp_err_t get_sd_test_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (WeatherLogger::get_sd_status().formatting) {
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"SD card is currently formatting\"}");
        return ESP_OK;
    }

    bsp_spi_lock();
    // Check if mounted
    struct stat st;
    if (stat("/sdcard", &st) != 0 || !S_ISDIR(st.st_mode)) {
        bsp_spi_unlock();
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"SD card not mounted\"}");
        return ESP_OK;
    }

    const char* filepath = "/sdcard/wxm_test.txt";
    const char* test_data = "WeatherXM SD Card Write and Read Test: SUCCESS!";

    // Write file
    FILE* f_write = fopen(filepath, "w");
    if (f_write == nullptr) {
        bsp_spi_unlock();
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to open file for writing\"}");
        return ESP_OK;
    }
    size_t written = fwrite(test_data, 1, strlen(test_data), f_write);
    fclose(f_write);

    if (written != strlen(test_data)) {
        bsp_spi_unlock();
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to write all bytes to file\"}");
        return ESP_OK;
    }

    // Read file
    FILE* f_read = fopen(filepath, "r");
    if (f_read == nullptr) {
        bsp_spi_unlock();
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to open file for reading\"}");
        return ESP_OK;
    }
    char read_buf[128] = {0};
    size_t read_bytes = fread(read_buf, 1, sizeof(read_buf) - 1, f_read);
    fclose(f_read);

    bsp_spi_unlock();

    if (read_bytes == 0) {
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to read data from file\"}");
        return ESP_OK;
    }

    if (strcmp(read_buf, test_data) != 0) {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "{\"status\":\"error\",\"message\":\"Data mismatch! Sent: '%s', Received: '%s'\"}", test_data, read_buf);
        httpd_resp_sendstr(req, err_msg);
        return ESP_OK;
    }

    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Write/Read verified!\"}");
    return ESP_OK;
}

static esp_err_t post_sd_eject_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (!WeatherLogger::eject_sd()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot eject SD card while formatting is in progress");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "{\"status\":\"ejected\",\"ejected\":true,\"mounted\":false}");
    return ESP_OK;
}

static esp_err_t post_sd_mount_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    bool mounted = WeatherLogger::mount_sd();
    const SdLoggerStatus st = WeatherLogger::get_sd_status();
    const std::string reason = escape_json_string(st.card_status_reason);
    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"%s\",\"mounted\":%s,\"ejected\":%s,\"card_present\":%s,\"message\":\"%s\"}",
             mounted ? "mounted" : "unmounted",
             mounted ? "true" : "false",
             st.ejected ? "true" : "false",
             st.card_present ? "true" : "false",
             reason.c_str());
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t post_sd_format_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    bool started = WeatherLogger::start_format_sd_async();
    if (started) {
        httpd_resp_sendstr(req, "{\"status\":\"started\",\"async\":true}");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start SD format task");
    }
    return ESP_OK;
}

static esp_err_t post_sd_delete_handler(httpd_req_t *req) {
    char query[256];
    char filename[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "file", filename, sizeof(filename)) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Filename parameter missing");
            return ESP_FAIL;
        }
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No query parameters");
        return ESP_FAIL;
    }
    if (!is_safe_sd_filename(filename)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }
    
    bool success = WeatherLogger::delete_sd_file(filename);
    httpd_resp_set_type(req, "application/json");
    if (success) {
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
    }
    return ESP_OK;
}
#endif // !CONFIG_SCREENLESS

static esp_err_t post_wifi_toggle_handler(httpd_req_t *req) {
    char query[256];
    char enable_str[16] = {0};
    bool enable = true;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "enable", enable_str, sizeof(enable_str)) == ESP_OK) {
            if (strcmp(enable_str, "false") == 0) {
                enable = false;
            }
        }
    }
    
    if (enable) {
        Wifi::connect(NULL, NULL, true);
    } else {
        Wifi::disconnect();
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static void url_decode_inplace(char *dst, size_t max_len, const char *src) {
    if (!dst || max_len == 0) return;
    size_t d = 0;
    while (*src && d + 1 < max_len) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[d++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[d++] = ' ';
            src++;
        } else {
            dst[d++] = *src++;
        }
    }
    dst[d] = '\0';
}

static esp_err_t mesh_telemetry_send_handler(httpd_req_t *req) {
    g_last_web_request_ts = (uint32_t)time(NULL);

    // Queue only. The transmit itself happens inside the packet handler task,
    // at the next safe radio window - blocking an HTTP worker for a full
    // station cycle would stall the whole web UI. Poll /api/v1/meshtastic/config
    // and watch sent_count to confirm it went out.
    MeshTelemetry::request_send(0);

    uint32_t sent_count = MeshTelemetry::get_sent_count();
    uint32_t since = MeshTelemetry::get_seconds_since_last_send();

    char resp[192];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"queued\":true,\"sent_count\":%u,\"seconds_since_last\":%u}",
             (unsigned)sent_count, (unsigned)since);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t mesh_telemetry_config_handler(httpd_req_t *req) {
    g_last_web_request_ts = (uint32_t)time(NULL);
    char query[256] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[32] = {0};
        if (httpd_query_key_value(query, "region", param, sizeof(param)) == ESP_OK) {
            int r = atoi(param);
            if (r == 868 || r == 915 || r == 923 || r == 916 || r == 865) {
                Settings::set_freq_region((WsFreqRegion)r);
                WsHandler::apply_station_change(WsHandler::g_receiver_profile);
                WsHandler::g_ws1300_meshtastic_changed = true;
                WsHandler::g_ws1300_meshcore_changed = true;
            }
        }
        if (httpd_query_key_value(query, "enabled", param, sizeof(param)) == ESP_OK) {
            bool en = (strcmp(param, "true") == 0 || strcmp(param, "1") == 0);
            Settings::set_mesh_env_telemetry_enabled(en);
        }
        if (httpd_query_key_value(query, "interval", param, sizeof(param)) == ESP_OK) {
            uint32_t interval = (uint32_t)atoi(param);
            if (interval >= 60) {
                Settings::set_mesh_env_telemetry_interval_sec(interval);
            }
        }
        if (httpd_query_key_value(query, "hop_limit", param, sizeof(param)) == ESP_OK) {
            uint8_t hop = (uint8_t)atoi(param);
            if (hop >= 1 && hop <= 5) {
                Settings::set_mesh_env_telemetry_hop_limit(hop);
            }
        }
        if (httpd_query_key_value(query, "tx_power_high", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "power_high", param, sizeof(param)) == ESP_OK) {
            bool high = (strcmp(param, "true") == 0 || strcmp(param, "1") == 0);
            Settings::set_mesh_tx_power_high(high);
        }
        if (httpd_query_key_value(query, "custom_name_enabled", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "cname_en", param, sizeof(param)) == ESP_OK) {
            bool c_en = (strcmp(param, "true") == 0 || strcmp(param, "1") == 0);
            Settings::set_mesh_custom_name_enabled(c_en);
            MeshTelemetry::update_names();
            WsHandler::g_ws1300_meshtastic_changed = true;
        }
        char name_param[64] = {0};
        if (httpd_query_key_value(query, "long_name", name_param, sizeof(name_param)) == ESP_OK ||
            httpd_query_key_value(query, "custom_long_name", name_param, sizeof(name_param)) == ESP_OK ||
            httpd_query_key_value(query, "custom_name", name_param, sizeof(name_param)) == ESP_OK) {
            char decoded[32] = {0};
            url_decode_inplace(decoded, sizeof(decoded), name_param);
            if (strlen(decoded) > 23) decoded[23] = '\0';
            Settings::set_mesh_custom_name(decoded);
            MeshTelemetry::update_names();
            WsHandler::g_ws1300_meshtastic_changed = true;
        }
        char short_param[32] = {0};
        if (httpd_query_key_value(query, "short_name", short_param, sizeof(short_param)) == ESP_OK ||
            httpd_query_key_value(query, "custom_short_name", short_param, sizeof(short_param)) == ESP_OK) {
            char decoded[16] = {0};
            url_decode_inplace(decoded, sizeof(decoded), short_param);
            if (strlen(decoded) > 4) decoded[4] = '\0';
            Settings::set_mesh_custom_short_name(decoded);
            MeshTelemetry::update_names();
            WsHandler::g_ws1300_meshtastic_changed = true;
        }
        if (httpd_query_key_value(query, "preset", param, sizeof(param)) == ESP_OK) {
            uint8_t preset = (uint8_t)atoi(param);
            Settings::set_mesh_modem_preset(preset);
            if (preset != 255) {
                Settings::set_mesh_freq_hz(0);
                Settings::set_mesh_bw_khz(0);
                Settings::set_mesh_sf(0);
                Settings::set_mesh_cr(0);
                Settings::set_mesh_tx_power_dbm(0);
            }
            WsHandler::g_ws1300_meshtastic_changed = true;
        }
        if (httpd_query_key_value(query, "freq_hz", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "freq", param, sizeof(param)) == ESP_OK) {
            uint32_t freq = (uint32_t)strtoul(param, NULL, 10);
            Settings::set_mesh_freq_hz(freq);
            WsHandler::g_ws1300_meshtastic_changed = true;
        }
        if (httpd_query_key_value(query, "bw_khz", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "bw", param, sizeof(param)) == ESP_OK) {
            uint16_t bw = (uint16_t)atoi(param);
            Settings::set_mesh_bw_khz(bw);
            WsHandler::g_ws1300_meshtastic_changed = true;
        }
        if (httpd_query_key_value(query, "sf", param, sizeof(param)) == ESP_OK) {
            uint8_t sf = (uint8_t)atoi(param);
            Settings::set_mesh_sf(sf);
            WsHandler::g_ws1300_meshtastic_changed = true;
        }
        if (httpd_query_key_value(query, "cr", param, sizeof(param)) == ESP_OK) {
            uint8_t cr = (uint8_t)atoi(param);
            Settings::set_mesh_cr(cr);
            WsHandler::g_ws1300_meshtastic_changed = true;
        }
        if (httpd_query_key_value(query, "tx_power_dbm", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "tx_power", param, sizeof(param)) == ESP_OK) {
            int8_t pwr = (int8_t)atoi(param);
            Settings::set_mesh_tx_power_dbm(pwr);
            WsHandler::g_ws1300_meshtastic_changed = true;
        }
    }

    bool enabled = Settings::get_mesh_env_telemetry_enabled();
    bool pwr_high = Settings::get_mesh_tx_power_high();
    uint32_t interval = Settings::get_mesh_env_telemetry_interval_sec();
    uint32_t sent_count = MeshTelemetry::get_sent_count();
    uint32_t since = MeshTelemetry::get_seconds_since_last_send();
    uint8_t preset = Settings::get_mesh_modem_preset();
    uint32_t custom_freq = Settings::get_mesh_freq_hz();
    uint16_t custom_bw = Settings::get_mesh_bw_khz();
    uint8_t custom_sf = Settings::get_mesh_sf();
    uint8_t custom_cr = Settings::get_mesh_cr();
    int8_t custom_tx_pwr = Settings::get_mesh_tx_power_dbm();
    int pwr_dbm = (preset == 255 && custom_tx_pwr > 0) ? custom_tx_pwr : (pwr_high ? (Settings::get_freq_region() == WS_FREQ_REGION_EU868 ? 22 : 14) : 10);

    char def_long[32] = {0};
    char def_short[8] = {0};
    MeshTelemetry::get_default_long_name(def_long, sizeof(def_long));
    MeshTelemetry::get_default_short_name(def_short, sizeof(def_short));
    bool custom_name_en = Settings::get_mesh_custom_name_enabled();
    const char *custom_long = Settings::get_mesh_custom_name();
    const char *custom_short = Settings::get_mesh_custom_short_name();

    char resp[1024];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"enabled\":%s,\"interval_sec\":%u,\"sent_count\":%u,"
             "\"seconds_since_last\":%u,\"node_id\":\"%s\",\"node_num\":%u,"
             "\"long_name\":\"%s\",\"short_name\":\"%s\","
             "\"default_long_name\":\"%s\",\"default_short_name\":\"%s\","
             "\"custom_name_enabled\":%s,\"custom_long_name\":\"%s\",\"custom_short_name\":\"%s\","
             "\"freq_region\":%d,\"freq_hz\":%u,\"preset\":%u,\"custom_freq_hz\":%u,"
             "\"custom_bw_khz\":%u,\"custom_sf\":%u,\"custom_cr\":%u,\"custom_tx_power_dbm\":%d,"
             "\"channel\":\"LongFast\",\"hop_limit\":%u,"
             "\"tx_power_high\":%s,\"tx_power_dbm\":%d}",
             enabled ? "true" : "false", (unsigned)interval, (unsigned)sent_count, (unsigned)since,
             MeshTelemetry::get_node_id_str(), (unsigned)MeshTelemetry::get_node_num(),
             MeshTelemetry::get_long_name(), MeshTelemetry::get_short_name(),
             def_long, def_short,
             custom_name_en ? "true" : "false", custom_long ? custom_long : "", custom_short ? custom_short : "",
             (int)Settings::get_freq_region(),
             (unsigned)MeshTelemetry::get_mesh_freq_hz(),
             (unsigned)preset, (unsigned)custom_freq, (unsigned)custom_bw, (unsigned)custom_sf, (unsigned)custom_cr, (int)custom_tx_pwr,
             (unsigned)MeshTelemetry::get_hop_limit(),
             pwr_high ? "true" : "false", pwr_dbm);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t meshcore_telemetry_send_handler(httpd_req_t *req) {
    g_last_web_request_ts = (uint32_t)time(NULL);

    MeshTelemetry::request_meshcore_send(0);

    uint32_t sent_count = MeshTelemetry::get_meshcore_sent_count();
    uint32_t since = MeshTelemetry::get_meshcore_seconds_since_last_send();

    char resp[192];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"queued\":true,\"gw_sent_count\":%u,\"gw_seconds_since_last\":%u}",
             (unsigned)sent_count, (unsigned)since);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t meshcore_config_handler(httpd_req_t *req) {
    g_last_web_request_ts = (uint32_t)time(NULL);
    char query[256] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[32] = {0};
        if (httpd_query_key_value(query, "region", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "gw_region", param, sizeof(param)) == ESP_OK) {
            int r = atoi(param);
            if (r == 868 || r == 915 || r == 923 || r == 916 || r == 865) {
                Settings::set_freq_region((WsFreqRegion)r);
                WsHandler::apply_station_change(WsHandler::g_receiver_profile);
                WsHandler::g_ws1300_meshtastic_changed = true;
                WsHandler::g_ws1300_meshcore_changed = true;
            }
        }
        if (httpd_query_key_value(query, "forwarding", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "forwarding_enabled", param, sizeof(param)) == ESP_OK) {
            bool fwd = (strcmp(param, "true") == 0 || strcmp(param, "1") == 0);
            Settings::set_mesh_multi_station_forwarding_enabled(fwd);
        }
        if (httpd_query_key_value(query, "gw_enabled", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "enabled", param, sizeof(param)) == ESP_OK) {
            bool en = (strcmp(param, "true") == 0 || strcmp(param, "1") == 0);
            Settings::set_meshcore_gw_telemetry_enabled(en);
        }
        if (httpd_query_key_value(query, "gw_interval", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "interval", param, sizeof(param)) == ESP_OK) {
            uint32_t interval = (uint32_t)atoi(param);
            if (interval >= 60) {
                Settings::set_meshcore_gw_telemetry_interval_sec(interval);
            }
        }
        if (httpd_query_key_value(query, "gw_profile", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "profile", param, sizeof(param)) == ESP_OK) {
            int prof = atoi(param);
            if ((prof >= 0 && prof <= 4) || prof == 255) {
                Settings::set_meshcore_gw_profile_idx((uint8_t)prof);
                if (prof != 255) {
                    Settings::set_meshcore_gw_freq_hz(0);
                    Settings::set_meshcore_gw_bw_khz(0);
                    Settings::set_meshcore_gw_sf(0);
                    Settings::set_meshcore_gw_cr(0);
                    Settings::set_meshcore_gw_tx_power_dbm(0);
                }
                WsHandler::g_ws1300_meshcore_changed = true;
            }
        }
        if (httpd_query_key_value(query, "gw_tx_power_high", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "tx_power_high", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "power_high", param, sizeof(param)) == ESP_OK) {
            bool high = (strcmp(param, "true") == 0 || strcmp(param, "1") == 0);
            Settings::set_meshcore_gw_tx_power_high(high);
        }
        if (httpd_query_key_value(query, "gw_freq_hz", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "gw_freq", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "freq_hz", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "freq", param, sizeof(param)) == ESP_OK) {
            uint32_t freq = (uint32_t)strtoul(param, NULL, 10);
            Settings::set_meshcore_gw_freq_hz(freq);
            WsHandler::g_ws1300_meshcore_changed = true;
        }
        if (httpd_query_key_value(query, "gw_bw_khz", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "gw_bw", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "bw_khz", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "bw", param, sizeof(param)) == ESP_OK) {
            uint16_t bw = (uint16_t)atoi(param);
            Settings::set_meshcore_gw_bw_khz(bw);
            WsHandler::g_ws1300_meshcore_changed = true;
        }
        if (httpd_query_key_value(query, "gw_sf", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "sf", param, sizeof(param)) == ESP_OK) {
            uint8_t sf = (uint8_t)atoi(param);
            Settings::set_meshcore_gw_sf(sf);
            WsHandler::g_ws1300_meshcore_changed = true;
        }
        if (httpd_query_key_value(query, "gw_cr", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "cr", param, sizeof(param)) == ESP_OK) {
            uint8_t cr = (uint8_t)atoi(param);
            Settings::set_meshcore_gw_cr(cr);
            WsHandler::g_ws1300_meshcore_changed = true;
        }
        if (httpd_query_key_value(query, "gw_tx_power_dbm", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "gw_tx_power", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "tx_power_dbm", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "tx_power", param, sizeof(param)) == ESP_OK) {
            int8_t pwr = (int8_t)atoi(param);
            Settings::set_meshcore_gw_tx_power_dbm(pwr);
            WsHandler::g_ws1300_meshcore_changed = true;
        }
        if (httpd_query_key_value(query, "custom_name_enabled", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "cname_en", param, sizeof(param)) == ESP_OK) {
            bool c_en = (strcmp(param, "true") == 0 || strcmp(param, "1") == 0);
            Settings::set_mesh_custom_name_enabled(c_en);
            MeshTelemetry::update_names();
            WsHandler::g_ws1300_meshcore_changed = true;
        }
        char name_param[64] = {0};
        if (httpd_query_key_value(query, "long_name", name_param, sizeof(name_param)) == ESP_OK ||
            httpd_query_key_value(query, "custom_long_name", name_param, sizeof(name_param)) == ESP_OK ||
            httpd_query_key_value(query, "custom_name", name_param, sizeof(name_param)) == ESP_OK ||
            httpd_query_key_value(query, "gw_long_name", name_param, sizeof(name_param)) == ESP_OK) {
            char decoded[32] = {0};
            url_decode_inplace(decoded, sizeof(decoded), name_param);
            if (strlen(decoded) > 23) decoded[23] = '\0';
            Settings::set_mesh_custom_name(decoded);
            MeshTelemetry::update_names();
            WsHandler::g_ws1300_meshcore_changed = true;
        }
        char short_param[32] = {0};
        if (httpd_query_key_value(query, "short_name", short_param, sizeof(short_param)) == ESP_OK ||
            httpd_query_key_value(query, "custom_short_name", short_param, sizeof(short_param)) == ESP_OK ||
            httpd_query_key_value(query, "gw_short_name", short_param, sizeof(short_param)) == ESP_OK) {
            char decoded[16] = {0};
            url_decode_inplace(decoded, sizeof(decoded), short_param);
            if (strlen(decoded) > 4) decoded[4] = '\0';
            Settings::set_mesh_custom_short_name(decoded);
            MeshTelemetry::update_names();
            WsHandler::g_ws1300_meshcore_changed = true;
        }
        if (httpd_query_key_value(query, "radio_mode", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "gw_radio_mode", param, sizeof(param)) == ESP_OK) {
            if (strcmp(param, "meshcore") == 0 || strcmp(param, "1") == 0) {
                Settings::set_gateway_radio_mode(Settings::GatewayRadioMode::MESHCORE_GATEWAY);
            } else if (strcmp(param, "wxm") == 0 || strcmp(param, "0") == 0) {
                Settings::set_gateway_radio_mode(Settings::GatewayRadioMode::WXM_GATEWAY);
            }
            WsHandler::retune_for_current_state();
            WsHandler::sx126x_rx_cont();
        }
    }

    bool forwarding_enabled = Settings::get_mesh_multi_station_forwarding_enabled();
    bool gw_enabled = Settings::get_meshcore_gw_telemetry_enabled();
    bool gw_pwr_high = Settings::get_meshcore_gw_tx_power_high();
    uint32_t gw_interval = Settings::get_meshcore_gw_telemetry_interval_sec();
    uint8_t gw_prof = Settings::get_meshcore_gw_profile_idx();
    uint32_t gw_sent = MeshTelemetry::get_meshcore_sent_count();
    uint32_t gw_since = MeshTelemetry::get_meshcore_seconds_since_last_send();
    uint32_t freq_hz = MeshTelemetry::get_meshcore_freq_hz();
    uint32_t gw_custom_freq = Settings::get_meshcore_gw_freq_hz();
    uint16_t gw_custom_bw = Settings::get_meshcore_gw_bw_khz();
    uint8_t gw_custom_sf = Settings::get_meshcore_gw_sf();
    uint8_t gw_custom_cr = Settings::get_meshcore_gw_cr();
    int8_t gw_custom_pwr = Settings::get_meshcore_gw_tx_power_dbm();
    int pwr_dbm = (gw_prof == 255 && gw_custom_pwr > 0) ? gw_custom_pwr : (gw_pwr_high ? (Settings::get_freq_region() == WS_FREQ_REGION_EU868 ? 22 : 14) : 2);
    Settings::GatewayRadioMode radio_mode = Settings::get_gateway_radio_mode();

    char def_long[32] = {0};
    char def_short[8] = {0};
    MeshTelemetry::get_default_long_name(def_long, sizeof(def_long));
    MeshTelemetry::get_default_short_name(def_short, sizeof(def_short));
    bool custom_name_en = Settings::get_mesh_custom_name_enabled();
    const char *custom_long = Settings::get_mesh_custom_name();
    const char *custom_short = Settings::get_mesh_custom_short_name();

    char resp[1152];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"radio_mode\":\"%s\",\"gw_radio_mode\":%u,"
             "\"forwarding_enabled\":%s,\"multi_station_forwarding\":%s,"
             "\"gw_enabled\":%s,\"gw_interval_sec\":%u,\"gw_profile\":%u,\"gw_sent_count\":%u,"
             "\"gw_seconds_since_last\":%u,\"gw_node_id\":\"%s\",\"gw_node_num\":%u,"
             "\"gw_long_name\":\"%s\",\"gw_short_name\":\"%s\","
             "\"default_long_name\":\"%s\",\"default_short_name\":\"%s\","
             "\"custom_name_enabled\":%s,\"custom_long_name\":\"%s\",\"custom_short_name\":\"%s\","
             "\"freq_region\":%d,\"freq_hz\":%u,\"gw_custom_freq_hz\":%u,\"gw_custom_bw_khz\":%u,"
             "\"gw_custom_sf\":%u,\"gw_custom_cr\":%u,\"gw_custom_tx_power_dbm\":%d,"
             "\"gw_tx_power_high\":%s,\"gw_tx_power_dbm\":%d}",
             (radio_mode == Settings::GatewayRadioMode::MESHCORE_GATEWAY) ? "meshcore" : "wxm",
             (unsigned)radio_mode,
             forwarding_enabled ? "true" : "false",
             forwarding_enabled ? "true" : "false",
             gw_enabled ? "true" : "false", (unsigned)gw_interval, (unsigned)gw_prof,
             (unsigned)gw_sent, (unsigned)gw_since,
             MeshTelemetry::get_node_id_str(), (unsigned)MeshTelemetry::get_node_num(),
             MeshTelemetry::get_long_name(), MeshTelemetry::get_short_name(),
             def_long, def_short,
             custom_name_en ? "true" : "false", custom_long ? custom_long : "", custom_short ? custom_short : "",
             (int)Settings::get_freq_region(),
             (unsigned)freq_hz, (unsigned)gw_custom_freq, (unsigned)gw_custom_bw,
             (unsigned)gw_custom_sf, (unsigned)gw_custom_cr, (int)gw_custom_pwr,
             gw_pwr_high ? "true" : "false", pwr_dbm);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t weather_policy_handler(httpd_req_t *req) {
    g_last_web_request_ts = Utils::get_tick_sec();

    AdaptivePublish::WeatherChangePolicy policy = Settings::get_weather_policy();

    char query[1024];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[64];

        if (httpd_query_key_value(query, "reset_defaults", param, sizeof(param)) == ESP_OK) {
            policy = AdaptivePublish::default_policy();
            Settings::set_weather_policy(policy);
        }

        if (httpd_query_key_value(query, "reset_baseline", param, sizeof(param)) == ESP_OK) {
            MeshTelemetry::reset_baseline();
        }

        if (httpd_query_key_value(query, "cooldown_sec", param, sizeof(param)) == ESP_OK) {
            policy.event_cooldown_sec = (uint16_t)atoi(param);
        }
        if (httpd_query_key_value(query, "budget_per_hour", param, sizeof(param)) == ESP_OK) {
            policy.event_budget_per_hour = (uint8_t)atoi(param);
        }
        if (httpd_query_key_value(query, "calm_gate_ms", param, sizeof(param)) == ESP_OK) {
            policy.wind_dir_calm_gate_ms = (float)atof(param);
        }
        if (httpd_query_key_value(query, "rain_on_mmh", param, sizeof(param)) == ESP_OK) {
            policy.rain_on_threshold_mmh = (float)atof(param);
        }
        if (httpd_query_key_value(query, "rain_off_mmh", param, sizeof(param)) == ESP_OK) {
            policy.rain_off_threshold_mmh = (float)atof(param);
        }
        if (httpd_query_key_value(query, "rain_state_changes", param, sizeof(param)) == ESP_OK) {
            policy.rain_state_changes = (strcmp(param, "true") == 0 || strcmp(param, "1") == 0);
        }

        auto parse_metric = [&](const char* en_key, const char* th_key, AdaptivePublish::WeatherVar v) {
            if (httpd_query_key_value(query, en_key, param, sizeof(param)) == ESP_OK) {
                bool en = (strcmp(param, "true") == 0 || strcmp(param, "1") == 0);
                if (en) {
                    policy.enabled_mask |= (1u << (uint8_t)v);
                } else {
                    policy.enabled_mask &= ~(1u << (uint8_t)v);
                }
            }
            if (httpd_query_key_value(query, th_key, param, sizeof(param)) == ESP_OK) {
                policy.thresholds[(uint8_t)v] = (float)atof(param);
            }
        };

        parse_metric("temp_en", "temp_th", AdaptivePublish::VAR_TEMPERATURE);
        parse_metric("hum_en", "hum_th", AdaptivePublish::VAR_HUMIDITY);
        parse_metric("press_en", "press_th", AdaptivePublish::VAR_PRESSURE);
        parse_metric("wind_speed_en", "wind_speed_th", AdaptivePublish::VAR_WIND_SPEED);
        parse_metric("wind_gust_en", "wind_gust_th", AdaptivePublish::VAR_WIND_GUST);
        parse_metric("wind_dir_en", "wind_dir_th", AdaptivePublish::VAR_WIND_DIRECTION);
        parse_metric("precip_cml_en", "precip_cml_th", AdaptivePublish::VAR_PRECIP_CUMULATIVE);
        parse_metric("precip_rate_en", "precip_rate_th", AdaptivePublish::VAR_PRECIP_RATE);
        parse_metric("illum_en", "illum_th", AdaptivePublish::VAR_ILLUMINANCE);
        parse_metric("solar_rad_en", "solar_rad_th", AdaptivePublish::VAR_SOLAR_RADIATION);
        parse_metric("uv_en", "uv_th", AdaptivePublish::VAR_UV_INDEX);

        if (AdaptivePublish::validate_policy(policy)) {
            Settings::set_weather_policy(policy);
        } else {
            ESP_LOGW(TAG, "Invalid weather policy submitted via web/API");
        }
    }

    uint16_t crc16 = AdaptivePublish::compute_crc16(policy);
    MeshTelemetry::EvaluatorStatus mt_status = MeshTelemetry::get_evaluator_status(MeshTelemetry::MeshProtocol::MESHTASTIC);
    MeshTelemetry::EvaluatorStatus mc_status = MeshTelemetry::get_evaluator_status(MeshTelemetry::MeshProtocol::MESHCORE);

    auto is_en = [&](AdaptivePublish::WeatherVar v) {
        return (policy.enabled_mask & (1u << (uint8_t)v)) != 0 ? "true" : "false";
    };

    char resp[1536];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"version\":%u,\"crc16\":%u,\"crc16_hex\":\"0x%04X\","
             "\"station_pending\":%s,"
             "\"cooldown_sec\":%u,\"budget_per_hour\":%u,"
             "\"calm_gate_ms\":%.2f,\"rain_on_mmh\":%.2f,\"rain_off_mmh\":%.2f,\"rain_state_changes\":%s,"
             "\"temp\":{\"enabled\":%s,\"delta_threshold\":%.2f},"
             "\"humidity\":{\"enabled\":%s,\"delta_threshold\":%.2f},"
             "\"pressure\":{\"enabled\":%s,\"delta_threshold\":%.2f},"
             "\"wind_speed\":{\"enabled\":%s,\"delta_threshold\":%.2f},"
             "\"wind_gust\":{\"enabled\":%s,\"delta_threshold\":%.2f},"
             "\"wind_dir\":{\"enabled\":%s,\"delta_threshold\":%.2f},"
             "\"precip_cml\":{\"enabled\":%s,\"delta_threshold\":%.2f},"
             "\"precip_rate\":{\"enabled\":%s,\"delta_threshold\":%.2f},"
             "\"illuminance\":{\"enabled\":%s,\"delta_threshold\":%.2f},"
             "\"solar_radiation\":{\"enabled\":%s,\"delta_threshold\":%.2f},"
             "\"uv_index\":{\"enabled\":%s,\"delta_threshold\":%.2f},"
             "\"mt_diag\":{\"events_this_hour\":%u,\"cooldown_level\":%u,\"last_reasons\":%lu},"
             "\"mc_diag\":{\"events_this_hour\":%u,\"cooldown_level\":%u,\"last_reasons\":%lu}}",
             (unsigned)policy.version, (unsigned)crc16, (unsigned)crc16,
             WsHandler::g_ws1300_weather_policy_changed ? "true" : "false",
             (unsigned)policy.event_cooldown_sec, (unsigned)policy.event_budget_per_hour,
             policy.wind_dir_calm_gate_ms, policy.rain_on_threshold_mmh, policy.rain_off_threshold_mmh,
             policy.rain_state_changes ? "true" : "false",
             is_en(AdaptivePublish::VAR_TEMPERATURE), policy.thresholds[AdaptivePublish::VAR_TEMPERATURE],
             is_en(AdaptivePublish::VAR_HUMIDITY), policy.thresholds[AdaptivePublish::VAR_HUMIDITY],
             is_en(AdaptivePublish::VAR_PRESSURE), policy.thresholds[AdaptivePublish::VAR_PRESSURE],
             is_en(AdaptivePublish::VAR_WIND_SPEED), policy.thresholds[AdaptivePublish::VAR_WIND_SPEED],
             is_en(AdaptivePublish::VAR_WIND_GUST), policy.thresholds[AdaptivePublish::VAR_WIND_GUST],
             is_en(AdaptivePublish::VAR_WIND_DIRECTION), policy.thresholds[AdaptivePublish::VAR_WIND_DIRECTION],
             is_en(AdaptivePublish::VAR_PRECIP_CUMULATIVE), policy.thresholds[AdaptivePublish::VAR_PRECIP_CUMULATIVE],
             is_en(AdaptivePublish::VAR_PRECIP_RATE), policy.thresholds[AdaptivePublish::VAR_PRECIP_RATE],
             is_en(AdaptivePublish::VAR_ILLUMINANCE), policy.thresholds[AdaptivePublish::VAR_ILLUMINANCE],
             is_en(AdaptivePublish::VAR_SOLAR_RADIATION), policy.thresholds[AdaptivePublish::VAR_SOLAR_RADIATION],
             is_en(AdaptivePublish::VAR_UV_INDEX), policy.thresholds[AdaptivePublish::VAR_UV_INDEX],
             (unsigned)mt_status.events_this_hour, (unsigned)mt_status.cooldown_level, (unsigned long)mt_status.last_reasons,
             (unsigned)mc_status.events_this_hour, (unsigned)mc_status.cooldown_level, (unsigned long)mc_status.last_reasons);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

esp_err_t units_config_handler(httpd_req_t *req) {
  bool modified = false;
  char buf[256] = {0};
  if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
    char param[32] = {0};

    if (httpd_query_key_value(buf, "temp", param, sizeof(param)) == ESP_OK) {
      int v = -1;
      if (strcasecmp(param, "C") == 0 || strcmp(param, "0") == 0) v = 0;
      else if (strcasecmp(param, "F") == 0 || strcmp(param, "1") == 0) v = 1;
      if (v >= 0 && v <= 1) {
        Settings::set_temp_unit(static_cast<TempUnit>(v));
        modified = true;
      }
    }

    if (httpd_query_key_value(buf, "wind", param, sizeof(param)) == ESP_OK) {
      int v = -1;
      if (strcasecmp(param, "m/s") == 0 || strcasecmp(param, "ms") == 0 || strcmp(param, "0") == 0) v = 0;
      else if (strcasecmp(param, "km/h") == 0 || strcasecmp(param, "kmh") == 0 || strcmp(param, "1") == 0) v = 1;
      else if (strcasecmp(param, "mph") == 0 || strcmp(param, "2") == 0) v = 2;
      else if (strcasecmp(param, "ft/s") == 0 || strcasecmp(param, "fts") == 0 || strcmp(param, "3") == 0) v = 3;
      else if (strcasecmp(param, "bft") == 0 || strcmp(param, "4") == 0) v = 4;
      else if (strcasecmp(param, "kn") == 0 || strcmp(param, "5") == 0) v = 5;
      if (v >= 0 && v <= 5) {
        Settings::set_wind_speed_unit(static_cast<WindSpeedUnit>(v));
        modified = true;
      }
    }

    if (httpd_query_key_value(buf, "precip", param, sizeof(param)) == ESP_OK) {
      int v = -1;
      if (strcasecmp(param, "mm") == 0 || strcmp(param, "0") == 0) v = 0;
      else if (strcasecmp(param, "in") == 0 || strcmp(param, "1") == 0) v = 1;
      if (v >= 0 && v <= 1) {
        Settings::set_precip_unit(static_cast<PrecipUnit>(v));
        modified = true;
      }
    }

    if (httpd_query_key_value(buf, "press", param, sizeof(param)) == ESP_OK) {
      int v = -1;
      if (strcasecmp(param, "hPa") == 0 || strcmp(param, "0") == 0) v = 0;
      else if (strcasecmp(param, "mmHg") == 0 || strcmp(param, "1") == 0) v = 1;
      else if (strcasecmp(param, "inHg") == 0 || strcmp(param, "2") == 0) v = 2;
      if (v >= 0 && v <= 2) {
        Settings::set_press_unit(static_cast<PressureUnit>(v));
        modified = true;
      }
    }
  }

  if (modified) {
    Settings::commit();
#ifndef CONFIG_SCREENLESS
    StateSniffer::publish_ws_content_update();
#endif
    ESP_LOGI(TAG, "Display units updated via Web UI -> temp=%d wind=%d precip=%d press=%d",
             (int)Settings::get_temp_unit(),
             (int)Settings::get_wind_speed_unit(),
             (int)Settings::get_precip_unit(),
             (int)Settings::get_press_unit());
  }

  const char *u_temp = (Settings::get_temp_unit() == TEMP_UNIT_F) ? "F" : "C";
  const char *u_wind = "m/s";
  switch (Settings::get_wind_speed_unit()) {
    case WIND_SPEED_UNIT_KM_H:     u_wind = "km/h"; break;
    case WIND_SPEED_UNIT_MI_H:     u_wind = "mph";  break;
    case WIND_SPEED_UNIT_FT_S:     u_wind = "ft/s"; break;
    case WIND_SPEED_UNIT_BEAUFORT: u_wind = "bft";  break;
    case WIND_SPEED_UNIT_KNOTS:    u_wind = "kn";   break;
    case WIND_SPEED_UNIT_M_S:
    default:                       u_wind = "m/s";  break;
  }
  const char *u_precip = (Settings::get_precip_unit() == PRECIP_UNIT_IN) ? "in" : "mm";
  const char *u_press = "hPa";
  switch (Settings::get_press_unit()) {
    case PRESSURE_UNIT_MM_HG: u_press = "mmHg"; break;
    case PRESSURE_UNIT_IN_HG: u_press = "inHg"; break;
    case PRESSURE_UNIT_HPA:
    default:                  u_press = "hPa";  break;
  }

  char resp[256];
  snprintf(resp, sizeof(resp),
           "{\"status\":\"ok\",\"temp_id\":%d,\"wind_id\":%d,\"precip_id\":%d,\"press_id\":%d,"
           "\"temp\":\"%s\",\"wind\":\"%s\",\"precip\":\"%s\",\"press\":\"%s\"}",
           (int)Settings::get_temp_unit(),
           (int)Settings::get_wind_speed_unit(),
           (int)Settings::get_precip_unit(),
           (int)Settings::get_press_unit(),
           u_temp, u_wind, u_precip, u_press);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, resp);
  return ESP_OK;
}

esp_err_t start() {
  if (_server_handle != NULL) {
    return ESP_OK;
  }

  if (_packet_log_mutex == NULL) {
    _packet_log_mutex = xSemaphoreCreateMutex();
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.max_uri_handlers = 84;
  config.lru_purge_enable = true;
  config.task_priority = 6;
  config.stack_size = 16384;
  config.max_open_sockets = 7;
  config.backlog_conn = 8;
  config.recv_wait_timeout = 10;
  config.send_wait_timeout = 10;

  printf("Starting web server on port 80...\n");
  ESP_LOGI(TAG, "Starting web server on port 80...");
  esp_err_t err = httpd_start(&_server_handle, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server (%s)", esp_err_to_name(err));
    return err;
  }

  httpd_uri_t uri_root = {.uri = "/",
                          .method = HTTP_GET,
                          .handler = get_root_handler,
                          .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_root);

  httpd_uri_t uri_uplot_js = {.uri = "/uPlot.iife.min.js",
                              .method = HTTP_GET,
                              .handler = get_littlefs_file_handler,
                              .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_uplot_js);

  httpd_uri_t uri_uplot_css = {.uri = "/uPlot.min.css",
                               .method = HTTP_GET,
                               .handler = get_littlefs_file_handler,
                               .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_uplot_css);

  httpd_uri_t uri_status = {.uri = "/status",
                            .method = HTTP_GET,
                            .handler = get_status_handler,
                            .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_status);

  httpd_uri_t uri_llms_txt = {.uri = "/llms.txt",
                              .method = HTTP_GET,
                              .handler = get_llms_handler,
                              .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_llms_txt);

  httpd_uri_t uri_llms_full_txt = {.uri = "/llms-full.txt",
                                   .method = HTTP_GET,
                                   .handler = get_llms_handler,
                                   .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_llms_full_txt);

  httpd_uri_t uri_packet_log = {.uri = "/packet_log",
                                .method = HTTP_GET,
                                .handler = get_packet_log_handler,
                                .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_packet_log);

  httpd_uri_t uri_ota = {.uri = "/ota",
                         .method = HTTP_POST,
                         .handler = post_ota_handler,
                         .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_ota);

  httpd_uri_t uri_toggle_sniff = {.uri = "/toggle_sniff",
                                  .method = HTTP_POST,
                                  .handler = post_toggle_sniff_handler,
                                  .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_toggle_sniff);

  httpd_uri_t uri_toggle_autopair = {.uri = "/toggle_autopair",
                                      .method = HTTP_POST,
                                      .handler = post_toggle_autopair_handler,
                                      .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_toggle_autopair);

  httpd_uri_t uri_ws1300_kick = {.uri = "/ws1300_kick_station",
                                 .method = HTTP_POST,
                                 .handler = post_ws1300_kick_station_handler,
                                 .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_ws1300_kick);

  httpd_uri_t uri_set_pair_mode = {.uri = "/set_pair_mode",
                                   .method = HTTP_POST,
                                   .handler = post_set_pair_mode_handler,
                                   .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_set_pair_mode);

  httpd_uri_t uri_set_cloud_pairing = {.uri = "/set_cloud_pairing",
                                        .method = HTTP_POST,
                                        .handler = post_set_cloud_pairing_handler,
                                        .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_set_cloud_pairing);

  httpd_uri_t uri_set_region = {.uri = "/set_region",
                                .method = HTTP_POST,
                                .handler = post_set_region_handler,
                                .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_set_region);

  httpd_uri_t uri_set_station = {.uri = "/set_station",
                                 .method = HTTP_POST,
                                 .handler = post_set_station_handler,
                                 .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_set_station);

  httpd_uri_t uri_set_channel = {.uri = "/set_channel",
                                 .method = HTTP_POST,
                                 .handler = post_set_channel_handler,
                                 .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_set_channel);

  httpd_uri_t uri_remote_reboot = {.uri = "/remote_reboot",
                                   .method = HTTP_POST,
                                   .handler = post_remote_reboot_handler,
                                   .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_remote_reboot);

  httpd_uri_t uri_request_gps = {.uri = "/request_gps",
                                 .method = HTTP_POST,
                                 .handler = post_request_gps_handler,
                                 .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_request_gps);

  httpd_uri_t uri_copy_cloud_to_local = {.uri = "/copy_cloud_to_local",
                                         .method = HTTP_POST,
                                         .handler = post_copy_cloud_to_local_handler,
                                         .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_copy_cloud_to_local);

  httpd_uri_t uri_set_tx_power = {.uri = "/set_tx_power",
                                  .method = HTTP_POST,
                                  .handler = post_set_tx_power_handler,
                                  .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_set_tx_power);

  httpd_uri_t uri_set_tx_interval = {.uri = "/set_tx_interval",
                                     .method = HTTP_POST,
                                     .handler = post_set_tx_interval_handler,
                                     .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_set_tx_interval);

  httpd_uri_t uri_ws1300_station_meshtastic = {.uri = "/ws1300_station_meshtastic",
                                              .method = HTTP_POST,
                                              .handler = post_ws1300_station_meshtastic_handler,
                                              .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_ws1300_station_meshtastic);

  httpd_uri_t uri_toggle_mqtt = {.uri = "/toggle_mqtt",
                                 .method = HTTP_POST,
                                 .handler = post_toggle_mqtt_handler,
                                 .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_toggle_mqtt);

  httpd_uri_t uri_unpair = {.uri = "/unpair",
                            .method = HTTP_POST,
                            .handler = post_unpair_handler,
                            .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_unpair);

  httpd_uri_t uri_force_pair = {.uri = "/force_pair",
                                .method = HTTP_POST,
                                .handler = post_force_pair_handler,
                                .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_force_pair);

  httpd_uri_t uri_pair_local = {.uri = "/pair_local",
                                .method = HTTP_POST,
                                .handler = post_pair_local_handler,
                                .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_pair_local);

  httpd_uri_t uri_clear_recent = {.uri = "/clear_recent",
                                  .method = HTTP_POST,
                                  .handler = post_clear_recent_handler,
                                  .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_clear_recent);

  httpd_uri_t uri_clear_local = {.uri = "/clear_local",
                                 .method = HTTP_POST,
                                 .handler = post_clear_local_handler,
                                 .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_clear_local);

  httpd_uri_t uri_history = {.uri = "/history",
                             .method = HTTP_GET,
                             .handler = get_history_handler,
                             .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_history);

  httpd_uri_t uri_live_trends = {.uri = "/live_trends",
                                 .method = HTTP_GET,
                                 .handler = get_live_trends_handler,
                                 .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_live_trends);

#ifndef CONFIG_SCREENLESS
  httpd_uri_t uri_sd_status = {.uri = "/sd/status",
                               .method = HTTP_GET,
                               .handler = get_sd_status_handler,
                               .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_sd_status);

  httpd_uri_t uri_sd_list = {.uri = "/sd/list",
                             .method = HTTP_GET,
                             .handler = get_sd_list_handler,
                             .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_sd_list);

  httpd_uri_t uri_sd_download = {.uri = "/sd/download",
                                 .method = HTTP_GET,
                                 .handler = get_sd_download_handler,
                                 .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_sd_download);

  httpd_uri_t uri_sd_view = {.uri = "/sd/view",
                             .method = HTTP_GET,
                             .handler = get_sd_view_handler,
                             .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_sd_view);

  httpd_uri_t uri_sd_format = {.uri = "/sd/format",
                               .method = HTTP_POST,
                               .handler = post_sd_format_handler,
                               .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_sd_format);

  httpd_uri_t uri_sd_eject = {.uri = "/sd/eject",
                              .method = HTTP_POST,
                              .handler = post_sd_eject_handler,
                              .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_sd_eject);

  httpd_uri_t uri_sd_mount = {.uri = "/sd/mount",
                              .method = HTTP_POST,
                              .handler = post_sd_mount_handler,
                              .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_sd_mount);

  httpd_uri_t uri_sd_diagnose = {.uri = "/sd/diagnose",
                                 .method = HTTP_GET,
                                 .handler = get_sd_diagnose_handler,
                                 .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_sd_diagnose);

  httpd_uri_t uri_sd_test = {.uri = "/sd/test",
                             .method = HTTP_GET,
                             .handler = get_sd_test_handler,
                             .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_sd_test);

  httpd_uri_t uri_sd_delete = {.uri = "/sd/delete",
                               .method = HTTP_POST,
                               .handler = post_sd_delete_handler,
                               .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_sd_delete);
#endif

  httpd_uri_t uri_wifi_toggle = {.uri = "/wifi_toggle",
                                 .method = HTTP_POST,
                                 .handler = post_wifi_toggle_handler,
                                 .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_wifi_toggle);

  httpd_uri_t uri_mesh_send_telem = {.uri = "/api/v1/meshtastic/send_telemetry",
                                      .method = HTTP_GET,
                                      .handler = mesh_telemetry_send_handler,
                                      .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_mesh_send_telem);

  httpd_uri_t uri_mesh_send_telem_post = {.uri = "/api/v1/meshtastic/send_telemetry",
                                           .method = HTTP_POST,
                                           .handler = mesh_telemetry_send_handler,
                                           .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_mesh_send_telem_post);

  httpd_uri_t uri_mesh_send_alias = {.uri = "/api/v1/mesh/send_telemetry",
                                      .method = HTTP_GET,
                                      .handler = mesh_telemetry_send_handler,
                                      .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_mesh_send_alias);

  httpd_uri_t uri_mesh_send_alias_post = {.uri = "/api/v1/mesh/send_telemetry",
                                           .method = HTTP_POST,
                                           .handler = mesh_telemetry_send_handler,
                                           .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_mesh_send_alias_post);

  httpd_uri_t uri_mesh_config_get = {.uri = "/api/v1/meshtastic/config",
                                      .method = HTTP_GET,
                                      .handler = mesh_telemetry_config_handler,
                                      .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_mesh_config_get);

  httpd_uri_t uri_mesh_config_post = {.uri = "/api/v1/meshtastic/config",
                                       .method = HTTP_POST,
                                       .handler = mesh_telemetry_config_handler,
                                       .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_mesh_config_post);

  httpd_uri_t uri_ws1300_station_meshcore = {.uri = "/ws1300_station_meshcore",
                                             .method = HTTP_POST,
                                             .handler = post_ws1300_station_meshcore_handler,
                                             .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_ws1300_station_meshcore);

  httpd_uri_t uri_meshcore_config_get = {.uri = "/api/v1/meshcore/config",
                                         .method = HTTP_GET,
                                         .handler = meshcore_config_handler,
                                         .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_meshcore_config_get);

  httpd_uri_t uri_meshcore_config_post = {.uri = "/api/v1/meshcore/config",
                                          .method = HTTP_POST,
                                          .handler = meshcore_config_handler,
                                          .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_meshcore_config_post);

  httpd_uri_t uri_meshcore_send_telem_get = {.uri = "/api/v1/meshcore/send_telemetry",
                                            .method = HTTP_GET,
                                            .handler = meshcore_telemetry_send_handler,
                                            .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_meshcore_send_telem_get);

  httpd_uri_t uri_meshcore_send_telem_post = {.uri = "/api/v1/meshcore/send_telemetry",
                                             .method = HTTP_POST,
                                             .handler = meshcore_telemetry_send_handler,
                                             .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_meshcore_send_telem_post);

  httpd_uri_t uri_weather_policy_get = {.uri = "/api/v1/weather_policy",
                                       .method = HTTP_GET,
                                       .handler = weather_policy_handler,
                                       .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_weather_policy_get);

  httpd_uri_t uri_weather_policy_post = {.uri = "/api/v1/weather_policy",
                                         .method = HTTP_POST,
                                         .handler = weather_policy_handler,
                                         .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_weather_policy_post);

  httpd_uri_t uri_set_units_post = {.uri = "/set_units",
                                    .method = HTTP_POST,
                                    .handler = units_config_handler,
                                    .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_set_units_post);

  httpd_uri_t uri_api_units_get = {.uri = "/api/v1/units",
                                   .method = HTTP_GET,
                                   .handler = units_config_handler,
                                   .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_api_units_get);

  httpd_uri_t uri_api_units_post = {.uri = "/api/v1/units",
                                    .method = HTTP_POST,
                                    .handler = units_config_handler,
                                    .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_api_units_post);

  httpd_uri_t uri_api_info = {.uri = "/api/v1/info",
                              .method = HTTP_GET,
                              .handler = get_info_handler,
                              .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_api_info);

  httpd_uri_t uri_api_observations = {.uri = "/api/v1/observations",
                                      .method = HTTP_GET,
                                      .handler = get_observations_handler,
                                      .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_api_observations);

  httpd_uri_t uri_api_observation = {.uri = "/api/v1/observation",
                                     .method = HTTP_GET,
                                     .handler = get_observations_handler,
                                     .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_api_observation);

  httpd_uri_t uri_api_history = {.uri = "/api/v1/history",
                                 .method = HTTP_GET,
                                 .handler = get_history_handler,
                                 .user_ctx = NULL};
  httpd_register_uri_handler(_server_handle, &uri_api_history);

  return ESP_OK;
}

void stop() {
  if (_server_handle != NULL) {
    httpd_stop(_server_handle);
    _server_handle = NULL;
    ESP_LOGI(TAG, "Stopped web server.");
  }
}

} // namespace WebServer
