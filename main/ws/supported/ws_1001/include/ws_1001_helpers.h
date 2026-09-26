#ifndef WS_1001_HELPERS_H
#define WS_1001_HELPERS_H

#include "ws/supported/ws_1001/include/ws_1001.h"

namespace Ws1001Helpers
{
	extern const int POWER_LEVEL_MAX;
	extern const int POWER_LEVEL_MIN;

	Ws1001::PacketType get_packet_type(WsCommon::Packet* pkt);
	WsCommon::RetRes packet_validate(WsCommon::Packet* pkt);
	WsCommon::RetRes packet_verify(WsCommon::Packet* pkt);
	bool is_gps_data_valid(WsCommon::Packet* pkt);
	void calc_sta_status(WsCommon::Packet* pkt, WsBase::StaCtx* sta_ctx);
	bool gen_ack_base(WsCommon::Packet* pkt, uint8_t req_power, bool req_gps, uint8_t req_interval, bool req_reboot, uint8_t *out);
	void gen_ack_sig(WsCommon::Packet* pkt, WsCommon::Packet* ack_pkt, uint8_t *sig, uint16_t sig_len);
	WsCommon::RetRes get_gps_parameters(WsCommon::Packet* pkt, float* gps_lat,	float* gps_lon, bool* gps_valid,
								uint32_t* gps_ts, float* gps_speed_kn, float* gps_dir);
}

#endif
