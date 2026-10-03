#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <curl/curl.h>

#include "net.h"
#include "irc.h"
#include "config.h"
#include "util.h"
#include "i18n.h"
#include "minijson.h"
#include <psp2/rtc.h>

#define TCACHE_PATH DATA_DIR "/trcache.txt"
#define USAGE_PATH  DATA_DIR "/usage.txt"

long g_usage_today, g_usage_total, g_usage_requests;
static char usage_date[12];

enum { JOB_TR_IN, JOB_TR_OUT, JOB_UPLOAD, JOB_FETCH };

typedef struct {
	int      type;
	int      sidx;
	uint32_t uid;
	uint32_t mid;
	char    *text;
	char     lang[8];
} Job;

#define MAX_JOBS 64

static Job             jobs[MAX_JOBS];
static int             njobs;
static pthread_mutex_t qlock;
static pthread_cond_t  qcond;
static pthread_t       worker;
static int             have_ca;

char     g_net_busy[96];
int      g_upload_done;
char     g_upload_result[256];
int      g_upload_sidx;
uint32_t g_upload_uid;
int            g_fetch_done;
unsigned char *g_fetch_buf;
size_t         g_fetch_len;
char           g_fetch_err[160];

/* ---------------- translation cache ---------------- */

#define TCACHE 256
typedef struct { uint32_t h; char lang[8]; char *src; char *res; } TEntry;
static TEntry tcache[TCACHE];
static int    tcache_next;

static const char *tcache_get(const char *lang, const char *src)
{
	uint32_t h = hash_str(src);
	for (int i = 0; i < TCACHE; i++)
		if (tcache[i].src && tcache[i].h == h && !strcmp(tcache[i].lang, lang) && !strcmp(tcache[i].src, src))
			return tcache[i].res;
	return NULL;
}

static void tcache_put_mem(const char *lang, const char *src, const char *res);

static void flatten(char *s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\n' || *s == '\r')
			*s = ' ';
}

/* Translations are kept on the memory card so the same text is never paid twice. */
static void tcache_put(const char *lang, const char *src, const char *res)
{
	tcache_put_mem(lang, src, res);
	FILE *f = fopen(TCACHE_PATH, "a");
	if (!f)
		return;
	char *a = str_dup(src), *b = str_dup(res);
	if (a && b) {
		flatten(a);
		flatten(b);
		fprintf(f, "%s\t%s\t%s\n", lang, a, b);
	}
	free(a);
	free(b);
	fclose(f);
}

static void tcache_load(void)
{
	FILE *f = fopen(TCACHE_PATH, "rb");
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	long from = size > 96 * 1024 ? size - 96 * 1024 : 0;
	fseek(f, from, SEEK_SET);
	char *buf = malloc(size - from + 1);
	if (!buf) {
		fclose(f);
		return;
	}
	long n = fread(buf, 1, size - from, f);
	fclose(f);
	buf[n] = 0;
	char *p = buf;
	if (from) {
		char *nl = strchr(p, '\n');
		p = nl ? nl + 1 : p + n;
	}
	while (*p) {
		char *nl = strchr(p, '\n');
		if (nl) *nl = 0;
		char *t1 = strchr(p, '\t');
		char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
		if (t1 && t2) {
			*t1 = *t2 = 0;
			tcache_put_mem(p, t1 + 1, t2 + 1);
		}
		if (!nl) break;
		p = nl + 1;
	}
	/* keep the file bounded: rewrite the newest part */
	if (from) {
		f = fopen(TCACHE_PATH, "wb");
		if (f) {
			for (int i = 0; i < TCACHE; i++) {
				TEntry *e = &tcache[(tcache_next + i) % TCACHE];
				if (e->src)
					fprintf(f, "%s\t%s\t%s\n", e->lang, e->src, e->res);
			}
			fclose(f);
		}
	}
	free(buf);
}

static void today(char out[12])
{
	SceDateTime t;
	sceRtcGetCurrentClockLocalTime(&t);
	snprintf(out, 12, "%04d-%02d-%02d", t.year, t.month, t.day);
}

static void usage_load(void)
{
	FILE *f = fopen(USAGE_PATH, "r");
	if (!f)
		return;
	char d[12] = "";
	if (fscanf(f, "%11s %ld %ld %ld", d, &g_usage_today, &g_usage_total, &g_usage_requests) >= 3)
		str_copy(usage_date, d, sizeof(usage_date));
	fclose(f);
	char now[12];
	today(now);
	if (strcmp(now, usage_date))
		g_usage_today = 0;
}

static void usage_add(long tokens)
{
	char now[12];
	today(now);
	if (strcmp(now, usage_date)) {
		g_usage_today = 0;
		str_copy(usage_date, now, sizeof(usage_date));
	}
	g_usage_today += tokens;
	g_usage_total += tokens;
	g_usage_requests++;
	FILE *f = fopen(USAGE_PATH, "w");
	if (f) {
		fprintf(f, "%s %ld %ld %ld\n", usage_date, g_usage_today, g_usage_total, g_usage_requests);
		fclose(f);
	}
}

static void tcache_put_mem(const char *lang, const char *src, const char *res)
{
	TEntry *e = &tcache[tcache_next];
	tcache_next = (tcache_next + 1) % TCACHE;
	free(e->src);
	free(e->res);
	e->h = hash_str(src);
	str_copy(e->lang, lang, sizeof(e->lang));
	e->src = str_dup(src);
	e->res = str_dup(res);
}

/* Same rules as the panel: skip numbers/symbols, URLs and commands. */
int tr_should_skip(const char *t)
{
	while (*t == ' ')
		t++;
	if (strlen(t) < 2)
		return 1;
	if (!strncasecmp(t, "http://", 7) || !strncasecmp(t, "https://", 8))
		return 1;
	if (t[0] == '/' || t[0] == '!')
		return 1;
	for (const char *p = t; *p; p++)
		if (isalpha((unsigned char)*p) || ((unsigned char)*p) >= 0x80)
			return 0;
	return 1;
}

/* ---------------- HTTP helpers ---------------- */

typedef struct { char *data; size_t len; } Buf;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *ud)
{
	Buf *b = ud;
	size_t n = size * nmemb;
	if (b->len + n > 12 * 1024 * 1024)
		return 0;
	char *nd = realloc(b->data, b->len + n + 1);
	if (!nd)
		return 0;
	b->data = nd;
	memcpy(b->data + b->len, ptr, n);
	b->len += n;
	b->data[b->len] = 0;
	return n;
}

static CURL *http_new(Buf *out, long timeout)
{
	CURL *c = curl_easy_init();
	if (!c)
		return NULL;
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "VitaIRC/" APP_VERSION);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	if (have_ca) {
		curl_easy_setopt(c, CURLOPT_CAINFO, CA_PATH);
	} else {
		curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
	}
	return c;
}

static const char *lang_english_name(const char *code)
{
	static const char *codes[] = { "es", "en", "pt", "fr", "de", "it", "ja", "ru", "zh", "ko" };
	static const char *names[] = { "Spanish", "English", "Portuguese", "French", "German",
	                               "Italian", "Japanese", "Russian", "Chinese", "Korean" };
	for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
		if (!strcmp(codes[i], code))
			return names[i];
	return "Spanish";
}

/* Returns malloc'd translation, NULL on error (err filled), "" when it should be skipped. */
static char *openai_translate(const char *text, const char *lang, int outgoing, char *err, int errlen)
{
	char key[256], model[48];
	pthread_mutex_lock(&g_lock);
	str_copy(key, g_cfg.openai_key, sizeof(key));
	str_copy(model, g_cfg.openai_model, sizeof(model));
	pthread_mutex_unlock(&g_lock);
	if (!key[0]) {
		snprintf(err, errlen, T("Falta la API key de OpenAI (Ajustes)"));
		return NULL;
	}
	const char *ln = lang_english_name(lang);
	char sys[600];
	if (outgoing)
		snprintf(sys, sizeof(sys),
		         "You are a translator. Translate the user's IRC chat message to %s. Rules:\n"
		         "- Reply ONLY with the translation, no quotes or explanations.\n"
		         "- Keep nicknames, URLs, emoticons and IRC slang natural.\n"
		         "- If the message is already in %s, reply with the exact original text.", ln, ln);
	else
		snprintf(sys, sizeof(sys),
		         "You are a translator. Translate IRC chat messages to %s. Rules:\n"
		         "- Reply ONLY with the translation, no quotes or explanations.\n"
		         "- If the message is already in %s, reply with the exact original text.\n"
		         "- If the message is a number, URL, IRC command, or completely untranslatable, reply exactly: [SKIP]",
		         ln, ln);

	char *body = NULL;
	size_t blen = 0;
	mj_append_raw(&body, &blen, "{\"model\":");
	mj_append_str(&body, &blen, model[0] ? model : "gpt-4o-mini");
	mj_append_raw(&body, &blen, ",\"max_completion_tokens\":400,\"messages\":[{\"role\":\"system\",\"content\":");
	mj_append_str(&body, &blen, sys);
	mj_append_raw(&body, &blen, "},{\"role\":\"user\",\"content\":");
	mj_append_str(&body, &blen, text);
	mj_append_raw(&body, &blen, "}]}");
	if (!body) {
		snprintf(err, errlen, T("Error interno (JSON)"));
		return NULL;
	}

	Buf out = {0};
	CURL *c = http_new(&out, 30);
	char auth[300];
	snprintf(auth, sizeof(auth), "Authorization: Bearer %s", key);
	struct curl_slist *hdr = NULL;
	hdr = curl_slist_append(hdr, "Content-Type: application/json");
	hdr = curl_slist_append(hdr, auth);
	curl_easy_setopt(c, CURLOPT_URL, "https://api.openai.com/v1/chat/completions");
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
	curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
	CURLcode rc = curl_easy_perform(c);
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(hdr);
	curl_easy_cleanup(c);
	free(body);

	char *result = NULL;
	if (rc != CURLE_OK) {
		snprintf(err, errlen, T("Red: %s"), curl_easy_strerror(rc));
	} else if (code == 401) {
		snprintf(err, errlen, T("OpenAI: API key inválida"));
	} else if (code == 429) {
		snprintf(err, errlen, T("OpenAI: límite de uso alcanzado"));
	} else {
		const char *r = out.data;
		const char *tok = mj_get(mj_get(r, "usage"), "total_tokens");
		if (tok)
			usage_add(atol(tok));
		char *content = mj_str(mj_get(mj_get(mj_at(mj_get(r, "choices"), 0), "message"), "content"));
		if (content) {
			result = content;
			str_trim(result);
			if (!strcmp(result, "[SKIP]"))
				result[0] = 0;
		} else {
			char *emsg = mj_str(mj_get(mj_get(r, "error"), "message"));
			snprintf(err, errlen, "OpenAI %ld: %.120s", code, emsg ? emsg : T("respuesta inválida"));
			free(emsg);
		}
	}
	free(out.data);
	return result;
}

static char *imgur_upload(const char *path, char *err, int errlen)
{
	char cid[64];
	pthread_mutex_lock(&g_lock);
	str_copy(cid, g_cfg.imgur_id, sizeof(cid));
	pthread_mutex_unlock(&g_lock);
	if (!cid[0]) {
		snprintf(err, errlen, T("Falta el Client-ID de Imgur (Ajustes)"));
		return NULL;
	}
	FILE *f = fopen(path, "rb");
	if (!f) {
		snprintf(err, errlen, T("No se pudo abrir el archivo"));
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || size > 10 * 1024 * 1024) {
		fclose(f);
		snprintf(err, errlen, T("La imagen debe pesar menos de 10 MB"));
		return NULL;
	}
	unsigned char *data = malloc(size);
	if (!data || fread(data, 1, size, f) != (size_t)size) {
		fclose(f);
		free(data);
		snprintf(err, errlen, T("Error leyendo el archivo"));
		return NULL;
	}
	fclose(f);

	const char *fname = strrchr(path, '/');
	fname = fname ? fname + 1 : path;

	Buf out = {0};
	CURL *c = http_new(&out, 120);
	curl_mime *mime = curl_mime_init(c);
	curl_mimepart *part = curl_mime_addpart(mime);
	curl_mime_name(part, "image");
	curl_mime_data(part, (const char *)data, size);
	curl_mime_filename(part, fname);
	part = curl_mime_addpart(mime);
	curl_mime_name(part, "type");
	curl_mime_data(part, "file", CURL_ZERO_TERMINATED);

	char auth[100];
	snprintf(auth, sizeof(auth), "Authorization: Client-ID %s", cid);
	struct curl_slist *hdr = curl_slist_append(NULL, auth);
	curl_easy_setopt(c, CURLOPT_URL, "https://api.imgur.com/3/image");
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
	curl_easy_setopt(c, CURLOPT_MIMEPOST, mime);
	CURLcode rc = curl_easy_perform(c);
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_mime_free(mime);
	curl_slist_free_all(hdr);
	curl_easy_cleanup(c);
	free(data);

	char *link = NULL;
	if (rc != CURLE_OK) {
		snprintf(err, errlen, T("Red: %s"), curl_easy_strerror(rc));
	} else {
		const char *d = mj_get(out.data, "data");
		char *l = mj_str(mj_get(d, "link"));
		if (l && code < 300) {
			link = l;
		} else {
			free(l);
			const char *e = mj_get(d, "error");
			char *em = mj_str(e);
			if (!em)
				em = mj_str(mj_get(e, "message"));
			snprintf(err, errlen, "Imgur %ld: %.120s", code, em ? em : T("error desconocido"));
			free(em);
		}
	}
	free(out.data);
	return link;
}

static unsigned char *http_get(const char *url, size_t *len, char *err, int errlen)
{
	Buf out = {0};
	CURL *c = http_new(&out, 60);
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
	CURLcode rc = curl_easy_perform(c);
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	if (rc != CURLE_OK || code >= 400 || !out.len) {
		if (rc != CURLE_OK)
			snprintf(err, errlen, T("Red: %s"), curl_easy_strerror(rc));
		else
			snprintf(err, errlen, "HTTP %ld", code);
		free(out.data);
		return NULL;
	}
	*len = out.len;
	return (unsigned char *)out.data;
}

/* ---------------- worker ---------------- */

static void set_busy(const char *s)
{
	pthread_mutex_lock(&g_lock);
	str_copy(g_net_busy, s, sizeof(g_net_busy));
	g_dirty = 1;
	pthread_mutex_unlock(&g_lock);
}

static void finish_in(Job *j, const char *res)
{
	pthread_mutex_lock(&g_lock);
	Server *s = &g_servers[j->sidx];
	Chan *c = irc_find_chan_uid(s, j->uid);
	Msg *m = c ? irc_find_msg(c, j->mid) : NULL;
	if (m) {
		/* show only when it actually differs from the original */
		if (res && res[0] && strcmp(res, m->text)) {
			free(m->trans);
			m->trans = str_dup(res);
			m->trans_state = TR_DONE;
		} else {
			m->trans_state = TR_SKIP;
		}
		m->lay_w = 0;   /* force re-layout */
		g_dirty = 1;
	}
	pthread_mutex_unlock(&g_lock);
}

static void run_job(Job *j)
{
	char err[200] = {0};
	if (j->type == JOB_TR_IN) {
		const char *cached = tcache_get(j->lang, j->text);
		if (cached) {
			finish_in(j, cached);
			return;
		}
		set_busy(T("Traduciendo..."));
		char *r = openai_translate(j->text, j->lang, 0, err, sizeof(err));
		if (r) {
			tcache_put(j->lang, j->text, r);
			finish_in(j, r);
			free(r);
		} else {
			finish_in(j, NULL);
			pthread_mutex_lock(&g_lock);
			ui_toast(T("Traducción: %s"), err);
			pthread_mutex_unlock(&g_lock);
		}
	} else if (j->type == JOB_TR_OUT) {
		set_busy(T("Traduciendo mensaje..."));
		char *r = openai_translate(j->text, j->lang, 1, err, sizeof(err));
		if (r && r[0]) {
			irc_send_privmsg(j->sidx, j->uid, r);
		} else {
			pthread_mutex_lock(&g_lock);
			ui_toast(T("No se pudo traducir (%s). Enviado sin traducir."), err[0] ? err : T("vacío"));
			pthread_mutex_unlock(&g_lock);
			irc_send_privmsg(j->sidx, j->uid, j->text);
		}
		free(r);
	} else if (j->type == JOB_FETCH) {
		set_busy(T("Descargando imagen..."));
		size_t len = 0;
		unsigned char *data = http_get(j->text, &len, err, sizeof(err));
		pthread_mutex_lock(&g_lock);
		free(g_fetch_buf);
		g_fetch_buf = data;
		g_fetch_len = len;
		str_copy(g_fetch_err, err, sizeof(g_fetch_err));
		g_fetch_done = data ? 1 : -1;
		g_dirty = 1;
		pthread_mutex_unlock(&g_lock);
	} else if (j->type == JOB_UPLOAD) {
		set_busy(T("Subiendo imagen a Imgur..."));
		char *link = imgur_upload(j->text, err, sizeof(err));
		pthread_mutex_lock(&g_lock);
		g_upload_sidx = j->sidx;
		g_upload_uid = j->uid;
		if (link) {
			str_copy(g_upload_result, link, sizeof(g_upload_result));
			g_upload_done = 1;
		} else {
			str_copy(g_upload_result, err, sizeof(g_upload_result));
			g_upload_done = -1;
		}
		g_dirty = 1;
		pthread_mutex_unlock(&g_lock);
		free(link);
	}
	set_busy("");
}

static void *worker_thread(void *arg)
{
	(void)arg;
	while (!g_app_quit) {
		pthread_mutex_lock(&qlock);
		while (njobs == 0 && !g_app_quit)
			pthread_cond_wait(&qcond, &qlock);
		if (g_app_quit) {
			pthread_mutex_unlock(&qlock);
			break;
		}
		/* user actions (outgoing / uploads) go before background translations */
		int pick = 0;
		for (int i = 0; i < njobs; i++)
			if (jobs[i].type != JOB_TR_IN) { pick = i; break; }
		Job j = jobs[pick];
		memmove(&jobs[pick], &jobs[pick + 1], (njobs - pick - 1) * sizeof(Job));
		njobs--;
		pthread_mutex_unlock(&qlock);

		run_job(&j);
		free(j.text);
	}
	return NULL;
}

static void push_job(Job *j)
{
	pthread_mutex_lock(&qlock);
	if (njobs == MAX_JOBS) {
		/* drop the oldest background translation */
		int drop = -1;
		for (int i = 0; i < njobs; i++)
			if (jobs[i].type == JOB_TR_IN) { drop = i; break; }
		if (drop < 0) {
			pthread_mutex_unlock(&qlock);
			free(j->text);
			return;
		}
		free(jobs[drop].text);
		memmove(&jobs[drop], &jobs[drop + 1], (njobs - drop - 1) * sizeof(Job));
		njobs--;
	}
	jobs[njobs++] = *j;
	pthread_cond_signal(&qcond);
	pthread_mutex_unlock(&qlock);
}

void tr_request_in(int sidx, uint32_t uid, uint32_t mid, const char *text, const char *lang)
{
	Job j = { JOB_TR_IN, sidx, uid, mid, str_dup(text), {0} };
	str_copy(j.lang, lang && lang[0] ? lang : g_cfg.lang_in, sizeof(j.lang));
	push_job(&j);
}

void tr_request_out(int sidx, uint32_t uid, const char *text)
{
	Job j = { JOB_TR_OUT, sidx, uid, 0, str_dup(text), {0} };
	str_copy(j.lang, g_cfg.lang_out, sizeof(j.lang));
	push_job(&j);
}

void img_fetch_request(const char *url)
{
	Job j = { JOB_FETCH, 0, 0, 0, str_dup(url), {0} };
	push_job(&j);
}

void tr_request_one(int sidx, uint32_t uid, uint32_t mid, const char *text, const char *lang)
{
	Job j = { JOB_TR_IN, sidx, uid, mid, str_dup(text), {0} };
	str_copy(j.lang, lang && lang[0] ? lang : g_cfg.lang_in, sizeof(j.lang));
	/* on-demand: put it first in the queue */
	pthread_mutex_lock(&qlock);
	if (njobs < MAX_JOBS) {
		memmove(&jobs[1], &jobs[0], njobs * sizeof(Job));
		jobs[0] = j;
		njobs++;
		pthread_cond_signal(&qcond);
	} else {
		free(j.text);
	}
	pthread_mutex_unlock(&qlock);
}

void img_upload_request(int sidx, uint32_t uid, const char *path)
{
	Job j = { JOB_UPLOAD, sidx, uid, 0, str_dup(path), {0} };
	push_job(&j);
}

void net_init(void)
{
	curl_global_init(CURL_GLOBAL_ALL);
	tcache_load();
	usage_load();
	FILE *f = fopen(CA_PATH, "r");
	if (f) {
		have_ca = 1;
		fclose(f);
	}
	pthread_mutex_init(&qlock, NULL);
	pthread_cond_init(&qcond, NULL);
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	pthread_create(&worker, &attr, worker_thread, NULL);
	pthread_attr_destroy(&attr);
}

void net_shutdown(void)
{
	pthread_mutex_lock(&qlock);
	pthread_cond_broadcast(&qcond);
	pthread_mutex_unlock(&qlock);
}
