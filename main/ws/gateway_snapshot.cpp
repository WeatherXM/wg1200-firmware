#include "ws/include/gateway_snapshot.h"
#include "ws/include/ws_data.h"
#include "ws/include/ws_fields.h"
#include "pressure.h"
#include <math.h>

namespace GatewaySnapshot
{
    bool build(AdaptivePublish::WeatherSnapshot *out)
    {
        if (!out) return false;
        out->clear_all();

        WsFields::FieldPack pack;
        if (WsData::get_last_packet(&pack) != WsCommon::RetRes::OK) {
            return false;
        }

        float f = NAN;
        uint16_t u16 = 0;
        uint32_t u32 = 0;
        uint8_t u8 = 0;

        // Temperature (degC)
        f = NAN;
        if (WsFields::get_field_val_from_pack<float>(&pack, WsFields::FieldType::WEATHER_TEMP, NULL, &f) == WsCommon::RetRes::OK && !isnan(f)) {
            out->set(AdaptivePublish::VAR_TEMPERATURE, f);
        }

        // Humidity (%RH)
        f = NAN;
        if (WsFields::get_field_val_from_pack<float>(&pack, WsFields::FieldType::WEATHER_HUM, NULL, &f) == WsCommon::RetRes::OK && !isnan(f)) {
            out->set(AdaptivePublish::VAR_HUMIDITY, f);
        }

        // Pressure (hPa) with onboard BMP3xx fallback for WS1001
        f = NAN;
        bool has_press = false;
        if (WsFields::get_field_val_from_pack<float>(&pack, WsFields::FieldType::WEATHER_PRESSURE, NULL, &f) == WsCommon::RetRes::OK && !isnan(f) && f > 0.0f) {
            out->set(AdaptivePublish::VAR_PRESSURE, f);
            has_press = true;
        }
        if (!has_press) {
            float onboard_p = Pressure::get_latest_val();
            if (!isnan(onboard_p) && onboard_p > 0.0f) {
                out->set(AdaptivePublish::VAR_PRESSURE, onboard_p);
            }
        }

        // Wind Speed (m/s)
        f = NAN;
        if (WsFields::get_field_val_from_pack<float>(&pack, WsFields::FieldType::WEATHER_WIND_SPEED, NULL, &f) == WsCommon::RetRes::OK && !isnan(f)) {
            out->set(AdaptivePublish::VAR_WIND_SPEED, f);
        }

        // Wind Gust (m/s)
        f = NAN;
        if (WsFields::get_field_val_from_pack<float>(&pack, WsFields::FieldType::WEATHER_WIND_GUST, NULL, &f) == WsCommon::RetRes::OK && !isnan(f)) {
            out->set(AdaptivePublish::VAR_WIND_GUST, f);
        }

        // Wind Direction (deg)
        u16 = 0;
        if (WsFields::get_field_val_from_pack<uint16_t>(&pack, WsFields::FieldType::WEATHER_WIND_DIR, NULL, &u16) == WsCommon::RetRes::OK) {
            out->set(AdaptivePublish::VAR_WIND_DIRECTION, (float)u16);
        }

        // Cumulative Precipitation (mm)
        f = NAN;
        if (WsFields::get_field_val_from_pack<float>(&pack, WsFields::FieldType::WEATHER_PRECIP_CML, NULL, &f) == WsCommon::RetRes::OK && !isnan(f)) {
            out->set(AdaptivePublish::VAR_PRECIP_CUMULATIVE, f);
        }

        // Precipitation Rate (mm/h)
        f = NAN;
        if (WsFields::get_field_val_from_pack<float>(&pack, WsFields::FieldType::WEATHER_PRECIP_RATE, NULL, &f) == WsCommon::RetRes::OK && !isnan(f)) {
            out->set(AdaptivePublish::VAR_PRECIP_RATE, f);
        }

        // Illuminance (lux)
        u32 = 0;
        if (WsFields::get_field_val_from_pack<uint32_t>(&pack, WsFields::FieldType::WEATHER_ILLUMINANCE, NULL, &u32) == WsCommon::RetRes::OK) {
            if (u32 >= 1500000u || u32 == 0x00FFFFFFu || u32 == 0x007FFFFFu || u32 == 0x0000FFFFu) {
                out->set(AdaptivePublish::VAR_ILLUMINANCE, 0.0f);
            } else {
                out->set(AdaptivePublish::VAR_ILLUMINANCE, (float)u32 / 10.0f);
            }
        }

        // Solar Radiation (W/m2)
        f = NAN;
        if (WsFields::get_field_val_from_pack<float>(&pack, WsFields::FieldType::WEATHER_SOLAR_RAD, NULL, &f) == WsCommon::RetRes::OK && !isnan(f)) {
            out->set(AdaptivePublish::VAR_SOLAR_RADIATION, f);
        }

        // UV Index
        u8 = 0;
        if (WsFields::get_field_val_from_pack<uint8_t>(&pack, WsFields::FieldType::WEATHER_UV_INDEX, NULL, &u8) == WsCommon::RetRes::OK) {
            out->set(AdaptivePublish::VAR_UV_INDEX, (float)u8);
        }

        return (out->valid_mask != 0);
    }
}
