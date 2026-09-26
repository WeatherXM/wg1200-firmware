#ifndef ADAPTIVE_PUBLISH_H
#define ADAPTIVE_PUBLISH_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

namespace AdaptivePublish
{
    static constexpr uint8_t POLICY_VERSION = 1;

    /* ---- Variables -------------------------------------------------- */
    /* Order is wire-visible: it indexes enabled_mask and the TLV. Append only. */
    enum WeatherVar : uint8_t {
        VAR_TEMPERATURE = 0,       // degC
        VAR_HUMIDITY,              // %RH
        VAR_PRESSURE,              // hPa
        VAR_WIND_SPEED,            // m/s
        VAR_WIND_GUST,             // m/s
        VAR_WIND_DIRECTION,        // deg (0..359)
        VAR_PRECIP_CUMULATIVE,     // mm
        VAR_PRECIP_RATE,           // mm/h
        VAR_ILLUMINANCE,           // lux
        VAR_SOLAR_RADIATION,       // W/m2
        VAR_UV_INDEX,              // index
        VAR_COUNT                  // = 11
    };

    enum CompareKind : uint8_t {
        KIND_LINEAR = 0,
        KIND_CIRCULAR,             // shortest angle, calm-gated
        KIND_ACCUM                 // monotonic delta, rebase on negative
    };

    struct VarMeta {
        const char *name;
        const char *unit;
        CompareKind kind;
        float default_threshold;
        bool default_enabled;
    };

    static const VarMeta VAR_INFO[VAR_COUNT] = {
        { "Temperature",         "°C",   KIND_LINEAR,     2.0f,     true  },
        { "Humidity",            "%RH",  KIND_LINEAR,    10.0f,     true  },
        { "Pressure",            "hPa",  KIND_LINEAR,     3.0f,     true  },
        { "Wind speed",          "m/s",  KIND_LINEAR,     3.0f,     true  },
        { "Wind gust",           "m/s",  KIND_LINEAR,     5.0f,     true  },
        { "Wind direction",      "°",    KIND_CIRCULAR,  45.0f,     true  },
        { "Rain accumulation",   "mm",   KIND_ACCUM,      0.5f,     true  },
        { "Rain rate",           "mm/h", KIND_LINEAR,     2.0f,     true  },
        { "Illuminance",         "lux",  KIND_LINEAR, 25000.0f,     false },
        { "Solar radiation",     "W/m²", KIND_LINEAR,   200.0f,     false },
        { "UV index",            "UV",   KIND_LINEAR,     2.0f,     false },
    };

    /* ---- Reasons (wire-visible in diagnostics & status) ------------- */
    enum ReasonBit : uint16_t {
        REASON_NONE         = 0,
        REASON_HEARTBEAT    = (1u << 0),
        /* bits 1..11: per-variable delta threshold crossing (1u << (1 + var)) */
        REASON_RAIN_STARTED = (1u << 12),
        REASON_RAIN_STOPPED = (1u << 13),
        REASON_MANUAL       = (1u << 14),
        REASON_EVENT_MASK   = 0x7FFE   /* all event reasons excluding HEARTBEAT */
    };

    inline constexpr uint16_t reason_for(WeatherVar v) {
        return (uint16_t)(1u << (1 + (uint8_t)v));
    }

    /* ---- Weather snapshot ------------------------------------------- */
    struct WeatherSnapshot {
        uint16_t valid_mask;                // bit v set if values[v] is present
        float    values[VAR_COUNT];

        inline bool has(WeatherVar v) const {
            return (valid_mask & (1u << (uint8_t)v)) != 0;
        }

        inline void set(WeatherVar v, float val) {
            valid_mask |= (1u << (uint8_t)v);
            values[(uint8_t)v] = val;
        }

        inline void clear(WeatherVar v) {
            valid_mask &= ~(1u << (uint8_t)v);
            values[(uint8_t)v] = 0.0f;
        }

        inline void clear_all() {
            valid_mask = 0;
            memset(values, 0, sizeof(values));
        }
    };

    /* ---- Change policy ---------------------------------------------- */
    struct WeatherChangePolicy {
        uint8_t  version;                   // = POLICY_VERSION (1)
        uint16_t enabled_mask;              // bit v enables change detection for VAR v
        bool     rain_state_changes;        // report rain start / stop transitions
        uint16_t event_cooldown_sec;        // minimum time between event publishes (default 300)
        uint8_t  event_budget_per_hour;     // max event packets per hour (default 6, 0 = unlimited)
        float    thresholds[VAR_COUNT];
        float    wind_dir_calm_gate_ms;     // suppress wind dir events below this speed (default 3.0 m/s)
        float    rain_on_threshold_mmh;     // rain start threshold (default 0.2 mm/h)
        float    rain_off_threshold_mmh;    // rain stop threshold (default 0.0 mm/h)
    };

    /* ---- Publisher state (per transport, RAM only) ------------------ */
    struct PublisherState {
        bool            baseline_valid;
        WeatherSnapshot baseline;
        int64_t         last_success_ms;
        int64_t         last_heartbeat_ms;
        int64_t         last_event_ms;
        uint16_t        pending_reasons;
        bool            rain_active;
        uint8_t         events_this_hour;
        int64_t         hour_window_start_ms;
        uint8_t         escalation_level;   // 0 = 1x, 1 = 2x, 2 = 4x cooldown
    };

    /* ---- Decision returned by evaluate() ---------------------------- */
    struct PublishDecision {
        bool            publish;
        uint16_t        reason_mask;
        WeatherSnapshot observation;        // snapshot decision was made on
    };

    /* ---- Packed wire representation for TLV & canonical CRC (31 bytes) */
#pragma pack(push, 1)
    struct WeatherPolicyWire {
        uint8_t  version;                   // 0: 1
        uint16_t enabled_mask;              // 1: BE
        uint8_t  flags;                     // 3: bit 0 = rain_state_changes
        uint8_t  event_cooldown_min;        // 4: minutes (1..60)
        uint8_t  event_budget_per_hour;     // 5: max events/hr (0 = unlimited)
        uint16_t temp_delta_x10;            // 6: degC * 10 (BE)
        uint8_t  humidity_delta;            // 8: %RH
        uint16_t pressure_delta_x10;        // 9: hPa * 10 (BE)
        uint16_t wind_speed_delta_x10;      // 11: m/s * 10 (BE)
        uint16_t wind_gust_delta_x10;       // 13: m/s * 10 (BE)
        uint16_t wind_direction_delta_deg;  // 15: deg (BE)
        uint16_t precip_accum_delta_x100;   // 17: mm * 100 (BE)
        uint16_t precip_rate_delta_x10;     // 19: mm/h * 10 (BE)
        uint32_t illuminance_delta_lux;     // 21: lux (BE)
        uint16_t solar_delta_x10;           // 25: W/m2 * 10 (BE)
        uint8_t  uv_delta_x10;              // 27: UV * 10
        uint8_t  wind_dir_calm_gate_x10;    // 28: m/s * 10
        uint8_t  rain_on_threshold_x10;     // 29: mm/h * 10
        uint8_t  rain_off_threshold_x10;    // 30: mm/h * 10
    };
#pragma pack(pop)

    /* ---- Endian helpers for wire struct ----------------------------- */
    inline uint16_t host_to_be16(uint16_t v) {
        return (uint16_t)((v >> 8) | (v << 8));
    }
    inline uint16_t be16_to_host(uint16_t v) {
        return host_to_be16(v);
    }
    inline uint32_t host_to_be32(uint32_t v) {
        return ((v >> 24) & 0x000000FF) |
               ((v >> 8)  & 0x0000FF00) |
               ((v << 8)  & 0x00FF0000) |
               ((v << 24) & 0xFF000000);
    }
    inline uint32_t be32_to_host(uint32_t v) {
        return host_to_be32(v);
    }

    /* ---- CRC16 (Standard ANSI / Modbus poly 0xA001) ----------------- */
    inline uint16_t compute_crc16(const uint8_t *data, size_t len) {
        uint16_t crc = 0xFFFF;
        for (size_t i = 0; i < len; i++) {
            crc ^= (uint16_t)data[i];
            for (int b = 0; b < 8; b++) {
                if (crc & 0x0001) {
                    crc = (crc >> 1) ^ 0xA001;
                } else {
                    crc = crc >> 1;
                }
            }
        }
        return crc;
    }

    /* ---- Wire encode / decode --------------------------------------- */
    inline void encode_wire(const WeatherChangePolicy &p, WeatherPolicyWire *out) {
        if (!out) return;
        memset(out, 0, sizeof(*out));
        out->version = p.version ? p.version : POLICY_VERSION;
        out->enabled_mask = host_to_be16(p.enabled_mask);
        out->flags = p.rain_state_changes ? 0x01 : 0x00;

        uint16_t cd_min = (uint16_t)((p.event_cooldown_sec + 59) / 60);
        if (cd_min < 1) cd_min = 1;
        if (cd_min > 60) cd_min = 60;
        out->event_cooldown_min = (uint8_t)cd_min;
        out->event_budget_per_hour = p.event_budget_per_hour;

        out->temp_delta_x10 = host_to_be16((uint16_t)(roundf(p.thresholds[VAR_TEMPERATURE] * 10.0f)));
        out->humidity_delta = (uint8_t)(roundf(p.thresholds[VAR_HUMIDITY]));
        out->pressure_delta_x10 = host_to_be16((uint16_t)(roundf(p.thresholds[VAR_PRESSURE] * 10.0f)));
        out->wind_speed_delta_x10 = host_to_be16((uint16_t)(roundf(p.thresholds[VAR_WIND_SPEED] * 10.0f)));
        out->wind_gust_delta_x10 = host_to_be16((uint16_t)(roundf(p.thresholds[VAR_WIND_GUST] * 10.0f)));
        out->wind_direction_delta_deg = host_to_be16((uint16_t)(roundf(p.thresholds[VAR_WIND_DIRECTION])));
        out->precip_accum_delta_x100 = host_to_be16((uint16_t)(roundf(p.thresholds[VAR_PRECIP_CUMULATIVE] * 100.0f)));
        out->precip_rate_delta_x10 = host_to_be16((uint16_t)(roundf(p.thresholds[VAR_PRECIP_RATE] * 10.0f)));
        out->illuminance_delta_lux = host_to_be32((uint32_t)(roundf(p.thresholds[VAR_ILLUMINANCE])));
        out->solar_delta_x10 = host_to_be16((uint16_t)(roundf(p.thresholds[VAR_SOLAR_RADIATION] * 10.0f)));
        out->uv_delta_x10 = (uint8_t)(roundf(p.thresholds[VAR_UV_INDEX] * 10.0f));

        out->wind_dir_calm_gate_x10 = (uint8_t)(roundf(p.wind_dir_calm_gate_ms * 10.0f));
        out->rain_on_threshold_x10 = (uint8_t)(roundf(p.rain_on_threshold_mmh * 10.0f));
        out->rain_off_threshold_x10 = (uint8_t)(roundf(p.rain_off_threshold_mmh * 10.0f));
    }

    inline bool decode_wire(const WeatherPolicyWire &wire, WeatherChangePolicy *out) {
        if (!out) return false;
        if (wire.version != POLICY_VERSION) return false;

        out->version = wire.version;
        out->enabled_mask = be16_to_host(wire.enabled_mask);
        out->rain_state_changes = (wire.flags & 0x01) != 0;

        uint8_t cd_min = wire.event_cooldown_min;
        if (cd_min < 1) cd_min = 1;
        if (cd_min > 60) cd_min = 60;
        out->event_cooldown_sec = (uint16_t)cd_min * 60;
        out->event_budget_per_hour = wire.event_budget_per_hour;

        out->thresholds[VAR_TEMPERATURE] = (float)be16_to_host(wire.temp_delta_x10) / 10.0f;
        out->thresholds[VAR_HUMIDITY] = (float)wire.humidity_delta;
        out->thresholds[VAR_PRESSURE] = (float)be16_to_host(wire.pressure_delta_x10) / 10.0f;
        out->thresholds[VAR_WIND_SPEED] = (float)be16_to_host(wire.wind_speed_delta_x10) / 10.0f;
        out->thresholds[VAR_WIND_GUST] = (float)be16_to_host(wire.wind_gust_delta_x10) / 10.0f;
        out->thresholds[VAR_WIND_DIRECTION] = (float)be16_to_host(wire.wind_direction_delta_deg);
        out->thresholds[VAR_PRECIP_CUMULATIVE] = (float)be16_to_host(wire.precip_accum_delta_x100) / 100.0f;
        out->thresholds[VAR_PRECIP_RATE] = (float)be16_to_host(wire.precip_rate_delta_x10) / 10.0f;
        out->thresholds[VAR_ILLUMINANCE] = (float)be32_to_host(wire.illuminance_delta_lux);
        out->thresholds[VAR_SOLAR_RADIATION] = (float)be16_to_host(wire.solar_delta_x10) / 10.0f;
        out->thresholds[VAR_UV_INDEX] = (float)wire.uv_delta_x10 / 10.0f;

        out->wind_dir_calm_gate_ms = (float)wire.wind_dir_calm_gate_x10 / 10.0f;
        out->rain_on_threshold_mmh = (float)wire.rain_on_threshold_x10 / 10.0f;
        out->rain_off_threshold_mmh = (float)wire.rain_off_threshold_x10 / 10.0f;

        return true;
    }

    inline uint16_t crc16(const WeatherChangePolicy &policy) {
        WeatherPolicyWire wire;
        encode_wire(policy, &wire);
        return compute_crc16(reinterpret_cast<const uint8_t *>(&wire), sizeof(wire));
    }

    inline uint16_t compute_crc16(const WeatherChangePolicy &policy) {
        return crc16(policy);
    }

    inline bool validate_policy(const WeatherChangePolicy &p) {
        if (p.event_cooldown_sec < 60 || p.event_cooldown_sec > 3600) return false;
        if (p.wind_dir_calm_gate_ms < 0.0f) return false;
        if (p.rain_on_threshold_mmh < 0.0f || p.rain_off_threshold_mmh < 0.0f) return false;
        for (uint8_t v = 0; v < VAR_COUNT; v++) {
            if (p.thresholds[v] < 0.0f) return false;
        }
        return true;
    }

    /* ---- Default policy initialisation ------------------------------ */
    inline void default_policy(WeatherChangePolicy *p) {
        if (!p) return;
        memset(p, 0, sizeof(*p));
        p->version                 = POLICY_VERSION;
        p->event_cooldown_sec      = 300;
        p->event_budget_per_hour   = 6;
        p->rain_state_changes      = true;
        p->wind_dir_calm_gate_ms   = 3.0f;
        p->rain_on_threshold_mmh   = 0.2f;
        p->rain_off_threshold_mmh  = 0.0f;

        p->thresholds[VAR_TEMPERATURE]       = 2.0f;      // degC
        p->thresholds[VAR_HUMIDITY]          = 10.0f;     // %RH
        p->thresholds[VAR_PRESSURE]          = 3.0f;      // hPa
        p->thresholds[VAR_WIND_SPEED]        = 3.0f;      // m/s
        p->thresholds[VAR_WIND_GUST]         = 5.0f;      // m/s
        p->thresholds[VAR_WIND_DIRECTION]    = 45.0f;     // deg
        p->thresholds[VAR_PRECIP_CUMULATIVE] = 0.5f;      // mm
        p->thresholds[VAR_PRECIP_RATE]       = 2.0f;      // mm/h
        p->thresholds[VAR_ILLUMINANCE]       = 25000.0f;  // lux
        p->thresholds[VAR_SOLAR_RADIATION]   = 200.0f;    // W/m2
        p->thresholds[VAR_UV_INDEX]          = 2.0f;

        /* Real weather variables enabled by default; diurnal variables disabled */
        p->enabled_mask =
            (1u << VAR_TEMPERATURE)       | (1u << VAR_HUMIDITY) |
            (1u << VAR_PRESSURE)          | (1u << VAR_WIND_SPEED) |
            (1u << VAR_WIND_GUST)         | (1u << VAR_WIND_DIRECTION) |
            (1u << VAR_PRECIP_CUMULATIVE) | (1u << VAR_PRECIP_RATE);
    }

    inline WeatherChangePolicy default_policy() {
        WeatherChangePolicy p;
        default_policy(&p);
        return p;
    }

    /* ---- Reset baseline --------------------------------------------- */
    inline void reset_baseline(PublisherState *st) {
        if (!st) return;
        st->baseline_valid = false;
        st->pending_reasons = 0;
        st->rain_active = false;
        st->baseline.clear_all();
    }

    /* ---- Pure Evaluator --------------------------------------------- */
    inline PublishDecision evaluate(
        const WeatherSnapshot &current,
        const WeatherChangePolicy &policy,
        PublisherState *st,
        uint32_t heartbeat_sec,
        int64_t now_ms)
    {
        PublishDecision dec;
        dec.publish = false;
        dec.reason_mask = REASON_NONE;
        dec.observation = current;

        if (!st) return dec;

        /* §5.6 Empty observation: mutate nothing, return false */
        if (current.valid_mask == 0) {
            return dec;
        }

        uint16_t new_reasons = 0;

        /* §5.7 Baseline initialisation / reboot */
        if (!st->baseline_valid) {
            new_reasons |= REASON_HEARTBEAT;
        } else {
            /* Check change detection on enabled variables */
            for (uint8_t v = 0; v < VAR_COUNT; v++) {
                WeatherVar var = (WeatherVar)v;
                if (!(policy.enabled_mask & (1u << v))) {
                    continue;
                }
                /* §5.5 Validity rules: both baseline and current must be valid */
                if (!current.has(var) || !st->baseline.has(var)) {
                    continue;
                }

                float curr_val = current.values[v];
                float base_val = st->baseline.values[v];

                switch (VAR_INFO[v].kind) {
                    case KIND_LINEAR: {
                        float delta = fabsf(curr_val - base_val);
                        if (delta >= policy.thresholds[v]) {
                            new_reasons |= reason_for(var);
                        }
                        break;
                    }
                    case KIND_CIRCULAR: {
                        /* Wind direction: calm-gated */
                        float speed = current.has(VAR_WIND_SPEED) ? current.values[VAR_WIND_SPEED] : 0.0f;
                        if (speed >= policy.wind_dir_calm_gate_ms) {
                            float diff = fabsf(curr_val - base_val);
                            while (diff > 360.0f) diff -= 360.0f;
                            if (diff > 180.0f) diff = 360.0f - diff;
                            if (diff >= policy.thresholds[v]) {
                                new_reasons |= reason_for(var);
                            }
                        }
                        break;
                    }
                    case KIND_ACCUM: {
                        /* Cumulative rain: monotonic delta */
                        float delta = curr_val - base_val;
                        if (delta < 0.0f) {
                            /* Immediate baseline rebase without emitting false event */
                            st->baseline.values[v] = curr_val;
                        } else if (delta >= policy.thresholds[v]) {
                            new_reasons |= reason_for(var);
                        }
                        break;
                    }
                }
            }

            /* §5.4 Rain start / stop hysteresis */
            if (policy.rain_state_changes && current.has(VAR_PRECIP_RATE)) {
                float rate = current.values[VAR_PRECIP_RATE];
                if (!st->rain_active && rate >= policy.rain_on_threshold_mmh) {
                    st->rain_active = true;
                    new_reasons |= REASON_RAIN_STARTED;
                } else if (st->rain_active && rate <= policy.rain_off_threshold_mmh) {
                    st->rain_active = false;
                    new_reasons |= REASON_RAIN_STOPPED;
                }
            }
        }

        /* Heartbeat due check */
        if (heartbeat_sec > 0) {
            if (st->last_heartbeat_ms == 0 ||
                (now_ms - st->last_heartbeat_ms >= (int64_t)heartbeat_sec * 1000LL)) {
                new_reasons |= REASON_HEARTBEAT;
            }
        }

        /* Accumulate into pending reasons */
        st->pending_reasons |= new_reasons;

        /* §6 Storm backstop: maintain hourly window with proportional decay */
        if (st->hour_window_start_ms == 0) {
            st->hour_window_start_ms = now_ms;
        }
        int64_t elapsed_window_ms = now_ms - st->hour_window_start_ms;
        if (elapsed_window_ms >= 3600000LL) {
            uint32_t elapsed_hours = (uint32_t)(elapsed_window_ms / 3600000LL);
            st->hour_window_start_ms += (int64_t)elapsed_hours * 3600000LL;
            st->events_this_hour = 0;
            st->escalation_level = (st->escalation_level > elapsed_hours) ? (st->escalation_level - (uint8_t)elapsed_hours) : 0;
        }

        if (st->pending_reasons == 0) {
            return dec;
        }

        /* Rate limiting & budget check: applies ONLY if publication is purely event-driven */
        bool is_event_only = (st->pending_reasons & REASON_HEARTBEAT) == 0;
        if (is_event_only) {
            uint32_t effective_cooldown_sec = ((uint32_t)policy.event_cooldown_sec) << st->escalation_level;
            if (st->last_event_ms > 0 &&
                (now_ms - st->last_event_ms < (int64_t)effective_cooldown_sec * 1000LL)) {
                return dec; // Cooldown active; reasons remain pending
            }

            if (policy.event_budget_per_hour > 0 &&
                st->events_this_hour >= policy.event_budget_per_hour) {
                return dec; // Hourly budget reached; reasons remain pending
            }
        }

        /* Publish accepted */
        dec.publish = true;
        dec.reason_mask = st->pending_reasons;
        return dec;
    }

    /* ---- Mark success / failure ------------------------------------- */
    inline void mark_success(
        PublisherState *st,
        const PublishDecision &d,
        const WeatherChangePolicy &policy,
        int64_t now_ms)
    {
        if (!st) return;

        uint16_t published_mask = d.reason_mask;
        st->pending_reasons = 0;
        st->last_success_ms = now_ms;
        st->baseline = d.observation;
        st->baseline_valid = true;

        if (published_mask & REASON_HEARTBEAT) {
            st->last_heartbeat_ms = now_ms;
        }

        if (published_mask & REASON_EVENT_MASK) {
            st->last_event_ms = now_ms;
            st->events_this_hour++;
            if (policy.event_budget_per_hour > 0 &&
                st->events_this_hour >= policy.event_budget_per_hour) {
                if (st->escalation_level < 2) {
                    st->escalation_level++;
                }
            }
        }
    }

    inline void mark_failure(PublisherState *st, const PublishDecision &/*d*/) {
        /* Baseline remains unchanged, reasons stay pending */
        (void)st;
    }

    /* ---- Reason formatting for UI & diagnostics -------------------- */
    inline void format_reasons(
        uint16_t mask,
        const WeatherSnapshot &current,
        const WeatherSnapshot &baseline,
        char *buf,
        size_t buf_size)
    {
        if (!buf || buf_size == 0) return;
        buf[0] = '\0';

        if (mask == REASON_NONE) {
            snprintf(buf, buf_size, "None");
            return;
        }

        size_t written = 0;
        auto append = [&](const char *str) {
            if (written > 0 && written + 2 < buf_size) {
                written += snprintf(buf + written, buf_size - written, ", ");
            }
            if (written < buf_size) {
                written += snprintf(buf + written, buf_size - written, "%s", str);
            }
        };

        if (mask & REASON_HEARTBEAT) {
            append("Heartbeat");
        }
        if (mask & REASON_RAIN_STARTED) {
            append("Rain started");
        }
        if (mask & REASON_RAIN_STOPPED) {
            append("Rain stopped");
        }
        if (mask & REASON_MANUAL) {
            append("Manual");
        }

        for (uint8_t v = 0; v < VAR_COUNT; v++) {
            WeatherVar var = (WeatherVar)v;
            if (mask & reason_for(var)) {
                char part[64];
                if (current.has(var) && baseline.has(var)) {
                    float delta = current.values[v] - baseline.values[v];
                    snprintf(part, sizeof(part), "%s %+0.1f %s",
                             VAR_INFO[v].name, delta, VAR_INFO[v].unit);
                } else if (current.has(var)) {
                    snprintf(part, sizeof(part), "%s %0.1f %s",
                             VAR_INFO[v].name, current.values[v], VAR_INFO[v].unit);
                } else {
                    snprintf(part, sizeof(part), "%s", VAR_INFO[v].name);
                }
                append(part);
            }
        }
    }
}

#endif // ADAPTIVE_PUBLISH_H
