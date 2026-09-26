#ifndef TB_JSON_H
#define TB_JSON_H

#include "ArduinoJson.h"
#include "common.h"
#include "struct.h"
#include "ws/include/ws_data.h"
// #include "gps.h"
#include "const.h"
#include "log.h"

namespace TBJson
{
	RetResult from_ws_aggregate(WsFields::FieldPack *agg_pack, StaticJsonDocument<TB_JSON_DOC_SIZE> *json_doc);
	RetResult append_telemetry_signature(char *data, StaticJsonDocument<TB_JSON_DOC_SIZE> *json_doc, const char *key_prefix);
	RetResult signed_ws_packet_structure(WsFields::FieldPack *agg_pack, char *buff, size_t buff_size);
	RetResult signed_gps_packet_structure(WsGps::GpsData* gps_pack, char *buff, size_t buff_size);
	RetResult add_ipfs_hash_to_values(StaticJsonDocument<TB_JSON_DOC_SIZE> *json_doc, char *hash);

	RetResult from_gps_data(WsGps::GpsData* gps_pack, StaticJsonDocument<TB_JSON_DOC_SIZE> *json_doc);
	RetResult build_one_time_attr(StaticJsonDocument<TB_JSON_DOC_SIZE> *json_doc);
	RetResult build_client_attr(StaticJsonDocument<TB_JSON_DOC_SIZE> *json_doc);
	RetResult build_local_pair_attr(StaticJsonDocument<TB_JSON_DOC_SIZE> *json_doc);
	RetResult append_from_log_entry(const Log::Entry *entry, StaticJsonDocument<TB_JSON_DOC_SIZE> *json_doc);
}


#endif
