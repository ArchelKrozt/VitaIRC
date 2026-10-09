#ifndef VITAIRC_HTTPC_H
#define VITAIRC_HTTPC_H

#include <stddef.h>
#include <curl/curl.h>

/* Small blocking HTTP client (libcurl) for the worker threads. */

typedef struct { char *data; size_t len, max; } HBuf;

typedef struct {
	struct curl_slist *headers;
	const char *body;          /* POST body, NULL = GET */
	const char *user_agent;    /* NULL = VitaIRC */
	size_t      max;           /* largest body accepted */
	long        timeout;       /* seconds */
	int         head;          /* HEAD request: no body */
} HReq;

typedef struct {
	long code;
	char ctype[96];            /* Content-Type, lowercase, without parameters */
	long length;               /* Content-Length, -1 when unknown */
	char final_url[512];       /* after redirects */
} HRes;

void httpc_init(void);
/* Returns 0 on a 2xx answer. out->data is malloc'd (may hold an error page). */
int  httpc(const char *url, const HReq *req, HBuf *out, HRes *res, char *err, int errlen);

#endif
