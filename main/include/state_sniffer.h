#ifndef STATE_SNIFFER_H
#define STATE_SNIFFER_H

#include "ws/include/ws_fields.h"

namespace StateSniffer
{
	void init();
	void on_enter();
	void leave();
	bool get_latest_weather_pack(WsFields::FieldPack* field_pack);
}

#endif