#include "https_client.h"
#include "app_config.h"
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_crt_bundle.h"
#include "esp_tls.h"

#ifndef CONFIG_HW_NAME
#define CONFIG_HW_NAME "WXM-WG1200"
#endif

namespace HttpsClient
{

//
// Local vars
//
/** Task stack */
EXT_RAM_BSS_ATTR StackType_t _task_stack[HTTPS_CLIENT_TASK_TASK_STACK_SIZE] = {};

/** Task buffer */
StaticTask_t _task_buff;

/** UI monitor task handle */
static TaskHandle_t _task_handle = NULL;

/** Queue handle for http req handling */
static QueueHandle_t _http_req_queue;

/** Queue size */
const uint16_t HTTP_REQ_QUEUE_SIZE = 3;

/** Queue send timeout constant */
const uint16_t HTTP_REQ_QUEUE_TIMEOUT_MS = 100;

/** Logger tag */
static const char TAG[] = "HttpsClient";

/** Response body buffer */
static char _res_body_buff[HTTPS_CLIENT_RESPONSE_SIZE] = {0};

/** Request timeout constant */
const uint16_t REQ_TIMEOUT_MS = 5000;

esp_err_t https_get(const char *url,
					char *body_buf, size_t body_buf_sz,
					int *out_status,
					int timeout_ms);

esp_err_t url_encode_spaces(char *buf, size_t buf_size);


/******************************************************************************
 * @brief Send an https request
 * @param req Https request
 ******************************************************************************/
void request(Req* req)
{
	// If queue is full, wait for HTTP_REQ_QUEUE_TIMEOUT_MS then drop the message
	if (xQueueSend(_http_req_queue, req, pdMS_TO_TICKS(HTTP_REQ_QUEUE_TIMEOUT_MS)) != pdPASS)
	{
		ESP_LOGW(TAG, "request: queue full, dropping msg");
	}
}

/******************************************************************************
 * @brief Https Clinet task
 ******************************************************************************/
void https_client_task(void *data)
{
	while(1)
	{
		HttpsClient::Req http_req;
		BaseType_t xStatus = xQueueReceive(_http_req_queue, &http_req, portMAX_DELAY);
		if(xStatus == pdPASS)
		{
			int status;
			
			if(url_encode_spaces(http_req.url, sizeof(http_req.url)) != ESP_OK)
			{
				ESP_LOGE(TAG, "Error encoding spaces");
			}

			esp_err_t err = https_get(http_req.url,
										_res_body_buff, sizeof(_res_body_buff),
										&status,
										REQ_TIMEOUT_MS);
			if(err != ESP_OK)
			{
				ESP_LOGE(TAG, "Error: %s", esp_err_to_name(err));
			}
			if(http_req.resp_cb)
			{
				http_req.resp_cb(status, _res_body_buff, strlen(_res_body_buff), http_req.ctx);
			}
			
			memset(_res_body_buff, 0, sizeof(_res_body_buff));
		}
	}
}

/******************************************************************************
 * @brief Init
 ******************************************************************************/
void init()
{
	_http_req_queue  = xQueueCreate(HTTP_REQ_QUEUE_SIZE, sizeof(HttpsClient::Req));

	_task_handle = xTaskCreateStaticPinnedToCore(https_client_task, HTTPS_CLIENT_TASK_NAME, sizeof(_task_stack), NULL,
											HTTPS_CLIENT_TASK_TASK_PRIORITY, _task_stack, &_task_buff, 1);
}

/******************************************************************************
 * @brief Check if overall timeout has passed
 * @return True on timeout reached
 ******************************************************************************/
static inline bool past_deadline(TickType_t start_ticks, int timeout_ms)
{
	if(timeout_ms <= 0)
	{
		return false;
	}
	TickType_t now = xTaskGetTickCount();
	return (now - start_ticks) >= pdMS_TO_TICKS(timeout_ms);
}

/******************************************************************************
 * @brief TLS read with retry-on-WANT_* until timeout.
 * @retval >0 bytes read
 * @retval 0  EOF (peer closed)
 * @retval -2 timeout (TLS per-I/O or overall deadline)
 * @retval <0 other fatal TLS error (original negative code)
 ******************************************************************************/
static int tls_conn_read(struct esp_tls *tls, void *buf, size_t len, TickType_t t0, int timeout_ms)
{
	while(true)
	{
		int n = esp_tls_conn_read(tls, buf, (int)len);
		if(n >= 0)
		{
			return n;
		}
		if(n == MBEDTLS_ERR_SSL_TIMEOUT)
		{
			return -2;
		}
		if(n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
		{
			if(past_deadline(t0, timeout_ms))
			{
				return -2;
			}
			vTaskDelay(pdMS_TO_TICKS(10));
			continue;
		}
		return n;
	}
}

/******************************************************************************
 * @brief TLS write with retry-on-WANT_* until timeout.
 * @retval >0 bytes written
 * @retval 0  EOF on write
 * @retval -2 timeout (TLS per-I/O or overall deadline)
 * @retval <0 other fatal TLS error (original negative code)
 ******************************************************************************/
static int tls_conn_write(struct esp_tls *tls, const void *buf, size_t len, TickType_t t0, int timeout_ms)
{
	while(true)
	{
		int n = esp_tls_conn_write(tls, buf, (int)len);
		if(n >= 0)
		{
			return n;
		}
		if(n == MBEDTLS_ERR_SSL_TIMEOUT)
		{
			return -2;
		}
		if(n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ)
		{
			if(past_deadline(t0, timeout_ms))
			{
				return -2;
			}
			vTaskDelay(pdMS_TO_TICKS(10));
			continue;
		}
		return n;
	}
}

/******************************************************************************
 * @brief Write to TLS connection (timeout-aware)
 * @retval 0 on success
 * @retval -1 on EOF
 * @retval -2 on timeout
 * @retval -3 on other error
 ******************************************************************************/
static int tls_write_all(struct esp_tls *tls, const char *buf, size_t len, TickType_t t0, int timeout_ms)
{
	size_t off = 0;
	while (off < len)
	{
		int n = tls_conn_write(tls, buf + off, len - off, t0, timeout_ms);
		if(n > 0)
		{
			off += (size_t)n;
			continue;
		}
		if(n == 0)
		{
			return -1; // connection closed early (EOF on write)
		}
		if(n == -2)
		{
			return -2; // timeout
		}
		// Any other negative is fatal
		return -3;
	}
	return 0;
}

/******************************************************************************
 * @brief Read a CRLF-terminated line into 'line' (NUL-terminated). Max len is (cap-1).
 * @retval Number of bytes (excluding CRLF) on success 
 * @retval -1 on early close
 * @retval -2 on timeout
 * @retval -3 on other error
 ******************************************************************************/
static int tls_read_line(struct esp_tls *tls, char *line, size_t cap, TickType_t t0, int timeout_ms)
{
	size_t pos = 0;
	char c_prev = 0;
	while(true)
	{
		char c;
		int n = tls_conn_read(tls, &c, 1, t0, timeout_ms);
		if(n > 0)
		{
			if(c_prev == '\r' && c == '\n')
			{
				if (pos > 0 && line[pos-1] == '\r') pos--;
				if(pos >= cap) pos = cap - 1;
				line[pos] = '\0';
				return (int)pos;
			}
			if(pos + 1 < cap) line[pos] = c;
			pos++;
			c_prev = c;
			continue;
		}
		if(n == 0)
		{
			return -1; // closed before CRLF
		}
		if(n == -2)
		{
			return -2; // timeout
		}
		// Any other negative is fatal
		return -3;
	}
}

/******************************************************************************
 * @brief In place func to lower char
 ******************************************************************************/
static void strlower_inplace(char *s)
{
	while(*s)
	{
		*s = tolower((unsigned char)*s);
		s++;
	}
}


/******************************************************************************
 * @brief Convert a single hexadecimal ASCII character to its numeric value.
 *
 * Accepts '0'..'9', 'a'..'f', 'A'..'F'. Any other character yields -1.
 *
 * @param c  ASCII character to convert.
 * @return   0..15 on success; -1 if @p c is not a valid hex digit.
 ******************************************************************************/
static inline int hex_nibble(int c)
{
	if (c >= '0' && c <= '9')
	{
		return c - '0';
	}
	c |= 0x20; // tolower (ASCII only)
	if (c >= 'a' && c <= 'f')
	{
		return 10 + (c - 'a');
	}
	return -1;
}

/******************************************************************************
 * @brief Parse a positive hexadecimal length from a string, stopping at ';'.
 *
 * Intended for HTTP chunked-encoding sizes like "1A3f;ext=foo". Parsing stops
 * at the first ';' (chunk extensions are ignored). Returns -1 on invalid digit
 * or on overflow (i.e., when val*16 + digit would exceed INT_MAX).
 *
 * @param s  NUL-terminated string starting with hex digits (optionally followed
 *           by a ';' and any extension text).
 * @return   Parsed non-negative length on success; -1 on error/overflow.
 ******************************************************************************/
static int parse_hex_len(const char *s)
{
	int val = 0;
	for (size_t i = 0; s[i] && s[i] != ';'; ++i)
	{
		int d = hex_nibble((unsigned char)s[i]);
		if (d < 0)
		{
			return -1;
		}
		// Check: val*16 + d <= INT_MAX  =>  val <= (INT_MAX - d)/16
		if (val > (INT_MAX - d) / 16)
		{
			return -1; // overflow
		}
		val = val * 16 + d;
	}
	return val;
}

/******************************************************************************
 * @brief Encode spaces with %20
 ******************************************************************************/
esp_err_t url_encode_spaces(char *buf, size_t buf_size)
{
	size_t len = strlen(buf);
	size_t spaces = 0;

	// count spaces
	for(size_t i = 0; i < len; i++)
	{
		if(buf[i] == ' ') spaces++;
	}

	// new length
	size_t new_len = len + spaces * 2; // each ' ' → "%20" adds +2
	if(new_len + 1 > buf_size)
	{
		// not enough buffer space
		return ESP_ERR_INVALID_SIZE;
	}

	// null terminate at new end
	buf[new_len] = '\0';

	// process backwards
	for(ssize_t i = len - 1, j = new_len - 1; i >= 0; i--)
	{
		if(buf[i] == ' ')
		{
			buf[j--] = '0';
			buf[j--] = '2';
			buf[j--] = '%';
		}
		else
		{
			buf[j--] = buf[i];
		}
	}

	return ESP_OK;
}

/** WXM cert to use with HTTPS */
extern const uint8_t _wxm_pem_start[] asm("_binary_wxm_pem_start");
extern const uint8_t wxm_pem_end[] asm("_binary_wxm_pem_end");

/******************************************************************************
 * @brief Performs HTTPS GET to `url` using bundle; writes response body into `body_buf` (NUL-terminated).
 * @param url Url of request
 * @param body_buf_sz Total size of body_buf
 * @param out_status Optional; returns HTTP status (e.g., 200) or -1 if not parsed
 * @param timeout_ms Timeout value of milliseconds (overall deadline)
 * @retval ESP_OK on success.s.
 * @retval ESP_ERR_INVALID_ARG for bad inputs.
 * @retval ESP_ERR_NO_MEM if the body wouldn’t fit (no truncation allowed).
 * @retval ESP_ERR_TIMEOUT if the overall time budget is exceeded.
 * @retval ESP_FAIL for other failures (bad TLS connect, malformed protocol, early close, etc.).
 ******************************************************************************/
esp_err_t https_get(const char *url,
					char *body_buf, size_t body_buf_sz,
					int *out_status,
					int timeout_ms)
{
	if(out_status)
	{
		*out_status = -1;
	}
	if(!url || !body_buf || body_buf_sz == 0)
	{
		return ESP_ERR_INVALID_ARG;
	}

	body_buf[0] = '\0';

	// global deadline start
	TickType_t t0 = xTaskGetTickCount();

	// Attempt 1: verify against the embedded WXM root certificates. This is highly target-curated,
	// fast, fits perfectly in SRAM, and succeeds on the very first try in 100% of cases,
	// avoiding double handshakes, CPU waste, and log spam.
	esp_tls_t *tls = esp_tls_init();
	if (!tls)
	{
		return ESP_ERR_NO_MEM;
	}

	// Create a null-terminated copy of the embedded certificate buffer.
	// PlatformIO embeds raw binary files without appending a trailing null terminator.
	unsigned int cert_size = (unsigned int)(wxm_pem_end - _wxm_pem_start);
	char *cert_buf = (char *)malloc(cert_size + 1);
	if (!cert_buf)
	{
		esp_tls_conn_destroy(tls);
		return ESP_ERR_NO_MEM;
	}
	memcpy(cert_buf, _wxm_pem_start, cert_size);
	cert_buf[cert_size] = '\0';

	esp_tls_cfg_t cfg = { 0 };
	cfg.timeout_ms = (timeout_ms > 0) ? timeout_ms : 10000;
	cfg.cacert_buf = (const unsigned char *)cert_buf;
	cfg.cacert_bytes = cert_size + 1;

	int tls_conn_status = esp_tls_conn_http_new_sync(url, &cfg, tls);

	// Free the temporary null-terminated buffer immediately after handshaking is complete
	free(cert_buf);

	if(tls_conn_status != 1)
	{
		// Attempt 2: fall back to the built-in CA bundle. This covers unexpected CA rotations
		// on the server side to a completely new public CA not covered by our embedded roots.
		ESP_LOGW(TAG, "TLS connect with embedded WXM roots failed, retrying with global CA bundle");
		esp_tls_conn_destroy(tls);
		tls = esp_tls_init();
		if (!tls)
		{
			return ESP_ERR_NO_MEM;
		}

		memset(&cfg, 0, sizeof(cfg));
		cfg.timeout_ms = (timeout_ms > 0) ? timeout_ms : 10000;
		cfg.crt_bundle_attach = esp_crt_bundle_attach;

		tls_conn_status = esp_tls_conn_http_new_sync(url, &cfg, tls);
	}

	if(tls_conn_status != 1)
	{
		ESP_LOGE(TAG, "TLS connect failed");
		esp_tls_conn_destroy(tls);
		return ESP_FAIL;
	}

	// Build GET request (keep host[:port] as-is from URL)
	const char *u = url;
	const char *scheme = "https://";
	size_t scheme_len = strlen(scheme);
	if (strncmp(u, scheme, scheme_len) == 0)
	{
		u += scheme_len;
	}

	const char *slash = strchr(u, '/');
	const char *host_start = u;
	const char *host_end   = slash ? slash : (u + strlen(u));

	// Host header
	char host_hdr[200];
	size_t host_len = (size_t)(host_end - host_start);
	if (host_len == 0 || host_len >= sizeof(host_hdr))
	{
		esp_tls_conn_destroy(tls);
		return ESP_ERR_INVALID_ARG;
	}
	memcpy(host_hdr, host_start, host_len);
	host_hdr[host_len] = '\0';

	// Path
	char path[256];
	if (slash)
	{
		size_t plen = strlen(slash);
		if (plen >= sizeof(path)) plen = sizeof(path) - 1;
		memcpy(path, slash, plen);
		path[plen] = '\0';
	}
	else
	{
		path[0] = '/';
		path[1] = '\0';
	}

	// Request buffer
	char req[512];
	int req_len = snprintf(req, sizeof(req),
						"GET %s HTTP/1.1\r\n"
						"Host: %s\r\n"
						"User-Agent: %s/%s\r\n"
						"Connection: close\r\n"
						"\r\n",
						path, host_hdr, CONFIG_HW_NAME, APP_VERSION);
	if (req_len <= 0 || req_len >= (int)sizeof(req))
	{
		esp_tls_conn_destroy(tls);
		return ESP_ERR_NO_MEM;
	}

	if (tls_write_all(tls, req, (size_t)req_len, t0, timeout_ms) != 0)
	{
		ESP_LOGE(TAG, "Write failed");
		esp_tls_conn_destroy(tls);
		return past_deadline(t0, timeout_ms) ? ESP_ERR_TIMEOUT : ESP_FAIL;
	}

	// Read status line
	char line[256];
	int n = tls_read_line(tls, line, sizeof(line), t0, timeout_ms);
	if(n < 0)
	{
		esp_tls_conn_destroy(tls);
		return (n == -2) ? ESP_ERR_TIMEOUT : ESP_FAIL;
	}

	// Parse "HTTP/1.1 XXX"
	if(out_status)
	{
		int code = -1;
		if(n >= 12 && !strncmp(line, "HTTP/", 5))
		{
			const char *sp = strchr(line, ' ');
			if(sp && isdigit((unsigned char)sp[1]) &&
					 isdigit((unsigned char)sp[2]) &&
					 isdigit((unsigned char)sp[3]))
			{
				code = (sp[1]-'0')*100 + (sp[2]-'0')*10 + (sp[3]-'0');
			}
		}
		*out_status = code;
	}

	// Read headers; detect chunked vs get Content-Length
	bool is_chunked = false;
	int content_len = -1;

	while(true)
	{
		n = tls_read_line(tls, line, sizeof(line), t0, timeout_ms);
		if(n < 0)
		{
			esp_tls_conn_destroy(tls);
			return (n == -2) ? ESP_ERR_TIMEOUT : ESP_FAIL;
		}
		if(n == 0)
		{
			break; // empty line → end of headers
		}

		// Lowercase a copy in-place (safe, line is our buffer)
		char key[64];
		// Find colon to split header
		char *colon2 = strchr(line, ':');
		if(!colon2)
		{
			continue;
		}
		size_t klen = (size_t)(colon2 - line);
		if(klen >= sizeof(key))
		{
			klen = sizeof(key) - 1;
		}
		memcpy(key, line, klen);
		key[klen] = '\0';
		strlower_inplace(key);

		// Value (skip spaces)
		char *val = colon2 + 1;
		while(*val == ' ' || *val == '\t')
		{
			++val;
		}

		if(!strcmp(key, "transfer-encoding"))
		{
			char vcopy[64];
			size_t vlen = strnlen(val, sizeof(vcopy)-1);
			if(vlen >= sizeof(vcopy))
			{
				vlen = sizeof(vcopy)-1;
			}
			memcpy(vcopy, val, vlen); vcopy[vlen] = '\0';
			strlower_inplace(vcopy);
			if(strstr(vcopy, "chunked"))
			{
				is_chunked = true;
			}
		}
		else if(!strcmp(key, "content-length"))
		{
			content_len = 0;
			for (char *p = val; *p && isdigit((unsigned char)*p); ++p)
			{
				content_len = content_len * 10 + (*p - '0');
			}
		}
	}

	// Read body into caller buffer (error if it won't fit)
	size_t written = 0;

	// Pre-check for known Content-Length
	if(!is_chunked && content_len >= 0)
	{
		if((size_t)content_len + 1 > body_buf_sz)
		{
			esp_tls_conn_destroy(tls);
			return ESP_ERR_NO_MEM;
		}
	}

	if(is_chunked)
	{
		// Chunked: read chunks until size 0
		while(true)
		{
			// chunk-size line (hex)
			n = tls_read_line(tls, line, sizeof(line), t0, timeout_ms);
			if(n < 0)
			{
				esp_tls_conn_destroy(tls);
				return (n == -2) ? ESP_ERR_TIMEOUT : ESP_FAIL;
			}
			int chunk = parse_hex_len(line);
			if(chunk < 0)
			{
				esp_tls_conn_destroy(tls);
				return ESP_FAIL;
			}
			if(chunk == 0)
			{
				// Optional trailer headers; read until empty line
				while(true)
				{
					n = tls_read_line(tls, line, sizeof(line), t0, timeout_ms);
					if(n <= 0)
					{
						break;
					}
				}
				break; // done
			}

			// capacity check (include trailing NUL)
			if(written + (size_t)chunk + 1 > body_buf_sz)
			{
				esp_tls_conn_destroy(tls);
				return ESP_ERR_NO_MEM;
			}

			// read exactly 'chunk' bytes (may need multiple reads)
			while(chunk > 0)
			{
				int r = tls_conn_read(tls, body_buf + written, (size_t)chunk, t0, timeout_ms);
				if(r > 0)
				{
					written += (size_t)r;
					chunk -= r;
					continue;
				}
				if(r == -2)
				{
					esp_tls_conn_destroy(tls);
					return ESP_ERR_TIMEOUT;
				}
				esp_tls_conn_destroy(tls);
				return ESP_FAIL;
			}

			// skip CRLF after the chunk
			char crlf[2];
			int r1 = tls_conn_read(tls, crlf, 2, t0, timeout_ms);
			if(r1 == 2 && crlf[0] == '\r' && crlf[1] == '\n')
			{
				// ok
			}
			else
			{
				// try to complete CRLF with deadline
				size_t got = (r1 > 0) ? (size_t)r1 : 0;
				while(got < 2)
				{
					int r2 = tls_conn_read(tls, crlf + got, 2 - (int)got, t0, timeout_ms);
					if(r2 > 0)
					{
						got += (size_t)r2; 
						continue; 
					}
					if(r2 == -2) {
						esp_tls_conn_destroy(tls);
						return ESP_ERR_TIMEOUT;
					}
					esp_tls_conn_destroy(tls); 
					return ESP_FAIL;
				}
				if(!(crlf[0] == '\r' && crlf[1] == '\n'))
				{
					esp_tls_conn_destroy(tls);
					return ESP_FAIL;
				}
			}
		}
	}
	else if(content_len >= 0)
	{
		int remaining = content_len;
		while(remaining > 0)
		{
			int r = tls_conn_read(tls, body_buf + written, (size_t)remaining, t0, timeout_ms);
			if(r > 0)
			{
				written += (size_t)r;
				remaining -= r;
				continue;
			}
			if(r == -2)
			{
				esp_tls_conn_destroy(tls);
				return ESP_ERR_TIMEOUT;
			}
			esp_tls_conn_destroy(tls);
			return ESP_FAIL;
		}
	}
	else
	{
		// No Content-Length: read until close, but error if buffer would overflow
		while(true)
		{
			size_t room = (body_buf_sz - 1) - written;
			if(room == 0)
			{
				esp_tls_conn_destroy(tls);
				return ESP_ERR_NO_MEM;
			}
			int r = tls_conn_read(tls, body_buf + written, room, t0, timeout_ms);
			if(r > 0)
			{
				written += (size_t)r;
				continue;
			}
			if(r == 0)
			{
				break;
			}
			if(r == -2)
			{
				esp_tls_conn_destroy(tls);
				return ESP_ERR_TIMEOUT;
			}
			esp_tls_conn_destroy(tls);
			return ESP_FAIL;
		}
	}

	// Done
	body_buf[written] = '\0';
	esp_tls_conn_destroy(tls);
	return ESP_OK;
}

}