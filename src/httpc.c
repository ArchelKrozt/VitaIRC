#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "httpc.h"
#include "config.h"
#include "util.h"
#include "i18n.h"

static int have_ca;

void httpc_init(void)
{
	FILE *f = fopen(CA_PATH, "r");
	if (f) {
		have_ca = 1;
		fclose(f);
	}
}

static size_t on_body(void *ptr, size_t size, size_t n, void *ud)
{
	HBuf *b = ud;
	size_t k = size * n;
	if (b->len + k > b->max)
		return 0;
	char *nd = realloc(b->data, b->len + k + 1);
	if (!nd)
		return 0;
	b->data = nd;
	memcpy(b->data + b->len, ptr, k);
	b->len += k;
	b->data[b->len] = 0;
	return k;
}

int httpc(const char *url, const HReq *req, HBuf *out, HRes *res, char *err, int errlen)
{
	HRes local;
	if (!res)
		res = &local;
	memset(res, 0, sizeof(*res));
	res->length = -1;
	memset(out, 0, sizeof(*out));
	out->max = req && req->max ? req->max : 4 * 1024 * 1024;
	CURL *c = curl_easy_init();
	if (!c) {
		snprintf(err, errlen, T("Error interno (JSON)"));
		return -1;
	}
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, req && req->timeout ? req->timeout : 60L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 6L);
	curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(c, CURLOPT_USERAGENT, req && req->user_agent ? req->user_agent : "VitaIRC/" APP_VERSION " (PlayStation Vita)");
	if (req && req->headers)
		curl_easy_setopt(c, CURLOPT_HTTPHEADER, req->headers);
	if (req && req->body)
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, req->body);
	if (req && req->head)
		curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
	if (have_ca) {
		curl_easy_setopt(c, CURLOPT_CAINFO, CA_PATH);
	} else {
		curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
	}
	CURLcode rc = curl_easy_perform(c);
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &res->code);
	char *ct = NULL, *eff = NULL;
	curl_off_t cl = -1;
	curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct);
	curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
	curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
	if (ct) {
		str_copy(res->ctype, ct, sizeof(res->ctype));
		res->ctype[strcspn(res->ctype, "; ")] = 0;
		for (char *p = res->ctype; *p; p++)
			*p = tolower((unsigned char)*p);
	}
	str_copy(res->final_url, eff ? eff : url, sizeof(res->final_url));
	res->length = (long)cl;
	curl_easy_cleanup(c);
	if (rc != CURLE_OK) {
		/* a body bigger than allowed still tells us what the link is */
		if (rc == CURLE_WRITE_ERROR && res->code && res->code < 300) {
			snprintf(err, errlen, T("Archivo demasiado grande"));
			return 1;
		}
		snprintf(err, errlen, T("Red: %s"), curl_easy_strerror(rc));
		return -1;
	}
	if (res->code >= 300) {
		snprintf(err, errlen, "HTTP %ld", res->code);
		return -1;
	}
	return 0;
}
