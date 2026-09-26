#ifndef HTTP_HANDLER_H
#define HTTP_HANDLER_H

#include "const.h"
#include "struct.h"

namespace HttpsClient
{

	/** Response callback */
	typedef void (*RespCb) (int status, const char* data, size_t data_len, void* ctx);

	enum class ReqType
	{
		NONE = 0,
		GET,
		POST
	};

	struct Req
	{
		ReqType type = ReqType::GET;
		char url[HTTPS_CLIENT_URL_SIZE];
		RespCb resp_cb = NULL;
		void* ctx = NULL;
	};

	void init();
	void request(Req* req);
}

#endif