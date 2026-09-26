#ifndef WS_1300_HELPERS_H
#define WS_1300_HELPERS_H

#include "ws/supported/ws_1300/include/ws_1300.h"

namespace Ws1300Helpers
{

extern const int POWER_LEVEL_MAX;
extern const int POWER_LEVEL_MIN;

Ws1300::StaPacketType get_packet_type(WsCommon::Packet* pkt);
WsCommon::RetRes packet_validate(WsCommon::Packet* pkt);
WsCommon::RetRes packet_verify(WsCommon::Packet* pkt, u8* pubkey, size_t pubkey_len);
void calc_sta_status(WsCommon::Packet* pkt, WsBase::StaCtx* sta_ctx);
void init_sta_status(Ws1300::ByteBuffer::StaHeader* sta_header);
u16 get_tx_int_sec(Ws1300::ByteBuffer::StaHeader* sta_header);
WsCommon::RetRes gen_resp_hmac(u8* data, int data_len, u8* hmac_buff, int hmac_buff_len);
void build_time_sync_resp(WsCommon::Packet* resp_pkt, Ws1300::ByteBuffer::StaHeader* sta_header, u32 time);
void build_ack_resp(WsCommon::Packet* resp_pkt, Ws1300::ByteBuffer::StaHeader* sta_header);

}

#endif