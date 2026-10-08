#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <curl/curl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>

#include "booru.h"
#include "irc.h"
#include "net.h"
#include "config.h"
#include "util.h"
#include "i18n.h"
#include "minijson.h"
#include "imgutil.h"

#define FAV_PATH    DATA_DIR "/favorites.txt"
#define FAV_THUMBS  DATA_DIR "/favthumbs"
#define CACHE_DIR   DATA_DIR "/cache"
#define PICTURE_DIR "ux0:picture/VitaIRC"
#define PAGE_SIZE   40

BPost *g_bres;
int    g_bres_n;
int    g_bres_more;
int    g_bres_loading;
char   g_bres_err[160];
int    g_bres_engine;
char   g_bres_query[128];
BPost *g_bfav;
int    g_bfav_n;

/* search being paged: page number (Safebooru/Danbooru) or cursor (Sankaku) */
static int  s_gen;
static int  s_page;
static char s_cursor[128];

static int  have_ca;
static char sk_token[700];
static char sk_token_user[64];       /* account the token belongs to */
static uint64_t sk_token_until;

const char *booru_engine_name(int e)
{
	switch (e) {
	case BE_DANBOORU: return "Danbooru";
	case BE_SANKAKU:  return "Sankaku";
	default:          return "Safebooru";
	}
}

/* ---------------- content filter ---------------- */

/* Never shown, whatever the settings. */
static const char *block_always[] = {
	"loli", "shota", "lolicon", "shotacon", "toddlercon", "child_abuse", "underage",
};
/* Also hidden on questionable/explicit posts. */
static const char *block_mature[] = {
	"child", "children", "female_child", "male_child", "kid", "kids", "toddler", "baby", "young",
	"younger", "aged_down", "teenage", "elementary_school_student", "middle_school_student", "school_child",
};

static int has_tag(const char *tags, const char *t)
{
	size_t n = strlen(t);
	for (const char *p = tags; (p = strstr(p, t)); p += n) {
		int left = p == tags || p[-1] == ' ';
		int right = p[n] == ' ' || p[n] == 0;
		if (left && right)
			return 1;
	}
	return 0;
}

static int post_allowed(char rating, const char *all_tags, int adult)
{
	if (!adult && rating != 'g')
		return 0;
	for (unsigned i = 0; i < sizeof(block_always) / sizeof(block_always[0]); i++)
		if (has_tag(all_tags, block_always[i]))
			return 0;
	if (rating == 'q' || rating == 'e')
		for (unsigned i = 0; i < sizeof(block_mature) / sizeof(block_mature[0]); i++)
			if (has_tag(all_tags, block_mature[i]))
				return 0;
	return 1;
}

static char rating_code(const char *r)
{
	if (!r || !r[0]) return 'q';
	if (!strcmp(r, "general") || !strcmp(r, "safe") || !strcmp(r, "g") || !strcmp(r, "s_sankaku")) return 'g';
	if (!strcmp(r, "sensitive") || !strcmp(r, "s")) return 's';
	if (!strcmp(r, "questionable") || !strcmp(r, "q")) return 'q';
	return 'e';
}

/* ---------------- HTTP ---------------- */

typedef struct { char *data; size_t len, max; } HBuf;

static size_t hb_write(void *ptr, size_t size, size_t n, void *ud)
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

/* GET (or POST when body is set). Returns 0 on success (2xx). */
static int http(const char *url, struct curl_slist *hdr, const char *body, HBuf *out, size_t max,
                char *err, int errlen, long *code_out)
{
	memset(out, 0, sizeof(*out));
	out->max = max;
	CURL *c = curl_easy_init();
	if (!c) {
		snprintf(err, errlen, T("Error interno (JSON)"));
		return -1;
	}
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, hb_write);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 90L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "VitaIRC/" APP_VERSION " (PlayStation Vita)");
	if (hdr)
		curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
	if (body)
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
	if (have_ca) {
		curl_easy_setopt(c, CURLOPT_CAINFO, CA_PATH);
	} else {
		curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
	}
	CURLcode rc = curl_easy_perform(c);
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	if (code_out)
		*code_out = code;
	if (rc != CURLE_OK) {
		snprintf(err, errlen, T("Red: %s"), curl_easy_strerror(rc));
		return -1;
	}
	if (code >= 300) {
		snprintf(err, errlen, "HTTP %ld", code);
		return -1;
	}
	return 0;
}

static struct curl_slist *sankaku_headers(int image)
{
	struct curl_slist *h = NULL;
	if (image) {
		h = curl_slist_append(h, "Referer: https://sankaku.app/");
		h = curl_slist_append(h, "Accept: image/webp,image/*,*/*;q=0.8");
	} else {
		h = curl_slist_append(h, "Accept: application/vnd.sankaku.api+json;v=2");
		h = curl_slist_append(h, "Origin: https://sankaku.app");
	}
	h = curl_slist_append(h, "User-Agent: Mozilla/5.0");
	if (sk_token[0]) {
		char a[760];
		snprintf(a, sizeof(a), "Authorization: %s", sk_token);
		h = curl_slist_append(h, a);
	}
	return h;
}

/* Logs in when an account is set (the token lasts about an hour). */
static void sankaku_login(void)
{
	char user[64], pass[128];
	pthread_mutex_lock(&g_lock);
	str_copy(user, g_cfg.sankaku_user, sizeof(user));
	str_copy(pass, g_cfg.sankaku_pass, sizeof(pass));
	pthread_mutex_unlock(&g_lock);
	if (!user[0] || !pass[0]) {
		sk_token[0] = 0;
		return;
	}
	if (sk_token[0] && now_ms() < sk_token_until && !strcmp(user, sk_token_user))
		return;
	str_copy(sk_token_user, user, sizeof(sk_token_user));
	char *body = NULL;
	size_t blen = 0;
	mj_append_raw(&body, &blen, "{\"login\":");
	mj_append_str(&body, &blen, user);
	mj_append_raw(&body, &blen, ",\"password\":");
	mj_append_str(&body, &blen, pass);
	mj_append_raw(&body, &blen, "}");
	sk_token[0] = 0;
	struct curl_slist *h = sankaku_headers(0);
	h = curl_slist_append(h, "Content-Type: application/json");
	HBuf out;
	char err[160];
	if (body && http("https://sankakuapi.com/auth/token", h, body, &out, 64 * 1024, err, sizeof(err), NULL) == 0) {
		char *tok = mj_str(mj_get(out.data, "access_token"));
		if (tok) {
			snprintf(sk_token, sizeof(sk_token), "Bearer %s", tok);
			sk_token_until = now_ms() + 55 * 60 * 1000ULL;
		}
		free(tok);
	}
	if (!sk_token[0]) {
		pthread_mutex_lock(&g_lock);
		ui_toast(T("Sankaku: no se pudo iniciar sesión, se busca sin cuenta"));
		pthread_mutex_unlock(&g_lock);
	}
	curl_slist_free_all(h);
	free(body);
	free(out.data);
}

/* ---------------- parsing ---------------- */

static void set_str(char *dst, size_t n, const char *val)
{
	char *s = mj_scalar(val);
	str_copy(dst, s ? s : "", n);
	free(s);
}

static long get_long(const char *val)
{
	char *s = mj_scalar(val);
	long v = s ? atol(s) : 0;
	free(s);
	return v;
}

static int get_bool(const char *val)
{
	char *s = mj_scalar(val);
	int v = s && (!strcmp(s, "true") || !strcmp(s, "True") || !strcmp(s, "1"));
	free(s);
	return v;
}

static int bad_ext(const char *ext)
{
	return !strcmp(ext, "mp4") || !strcmp(ext, "webm") || !strcmp(ext, "zip") || !strcmp(ext, "swf") ||
	       !strcmp(ext, "avif") || !ext[0];
}

static void url_ext(const char *url, char *ext, int n)
{
	char path[512];
	str_copy(path, url, sizeof(path));
	path[strcspn(path, "?#")] = 0;
	const char *dot = strrchr(path, '.');
	const char *slash = strrchr(path, '/');
	str_copy(ext, dot && dot > slash ? dot + 1 : "", n);
	for (char *p = ext; *p; p++)
		*p = tolower((unsigned char)*p);
}

/* Parses one page; returns posts kept (malloc'd array in *out). */
static int parse_page(int engine, const char *json, int adult, BPost **out, int *raw_count, char *cursor, int clen)
{
	const char *arr = engine == BE_SANKAKU ? mj_get(json, "data") : json;
	cursor[0] = 0;
	if (engine == BE_SANKAKU)
		set_str(cursor, clen, mj_get(mj_get(json, "meta"), "next"));
	int cap = 0, n = 0;
	*out = NULL;
	*raw_count = 0;
	for (int i = 0; arr; i++) {
		const char *it = mj_at(arr, i);
		if (!it)
			break;
		(*raw_count)++;
		BPost p;
		memset(&p, 0, sizeof(p));
		p.engine = engine;
		char *tags = NULL, rating[24] = "";
		set_str(p.id, sizeof(p.id), mj_get(it, "id"));
		if (engine == BE_SAFEBOORU) {
			set_str(rating, sizeof(rating), mj_get(it, "rating"));
			set_str(p.thumb_url, sizeof(p.thumb_url), mj_get(it, "preview_url"));
			set_str(p.file_url, sizeof(p.file_url), mj_get(it, "file_url"));
			if (get_bool(mj_get(it, "sample")))
				set_str(p.view_url, sizeof(p.view_url), mj_get(it, "sample_url"));
			p.w = get_long(mj_get(it, "width"));
			p.h = get_long(mj_get(it, "height"));
			tags = mj_scalar(mj_get(it, "tags"));
			url_ext(p.file_url, p.ext, sizeof(p.ext));
			p.rating = rating_code(rating);
		} else if (engine == BE_DANBOORU) {
			set_str(rating, sizeof(rating), mj_get(it, "rating"));
			set_str(p.thumb_url, sizeof(p.thumb_url), mj_get(it, "preview_file_url"));
			set_str(p.view_url, sizeof(p.view_url), mj_get(it, "large_file_url"));
			set_str(p.file_url, sizeof(p.file_url), mj_get(it, "file_url"));
			set_str(p.ext, sizeof(p.ext), mj_get(it, "file_ext"));
			p.w = get_long(mj_get(it, "image_width"));
			p.h = get_long(mj_get(it, "image_height"));
			p.file_size = get_long(mj_get(it, "file_size"));
			tags = mj_scalar(mj_get(it, "tag_string"));
			p.rating = rating_code(rating);
		} else {
			set_str(rating, sizeof(rating), mj_get(it, "rating"));
			if (get_bool(mj_get(it, "is_premium")) || get_bool(mj_get(it, "redirect_to_signup")))
				continue;
			set_str(p.view_url, sizeof(p.view_url), mj_get(it, "sample_url"));
			set_str(p.file_url, sizeof(p.file_url), mj_get(it, "file_url"));
			set_str(p.ext, sizeof(p.ext), mj_get(it, "file_ext"));
			str_copy(p.thumb_url, p.view_url, sizeof(p.thumb_url));   /* the preview is AVIF */
			p.w = get_long(mj_get(it, "width"));
			p.h = get_long(mj_get(it, "height"));
			p.file_size = get_long(mj_get(it, "file_size"));
			/* Sankaku: s = safe */
			p.rating = !strcmp(rating, "s") ? 'g' : rating_code(rating);
			size_t tl = 0;
			const char *ta = mj_get(it, "tags");
			for (int k = 0; ta; k++) {
				const char *t = mj_at(ta, k);
				if (!t)
					break;
				char *name = mj_str(mj_get(t, "tagName"));
				if (!name) name = mj_str(mj_get(t, "name_en"));
				if (!name) name = mj_str(mj_get(t, "name"));
				if (name) {
					for (char *q = name; *q; q++)
						if (*q == ' ') *q = '_';
					if (tl) mj_append_raw(&tags, &tl, " ");
					mj_append_raw(&tags, &tl, name);
				}
				free(name);
			}
		}
		for (char *q = p.ext; *q; q++)
			*q = tolower((unsigned char)*q);
		if (!p.view_url[0])
			str_copy(p.view_url, p.file_url, sizeof(p.view_url));
		int keep = p.id[0] && p.file_url[0] && p.thumb_url[0] && !bad_ext(p.ext) &&
		           post_allowed(p.rating, tags ? tags : "", adult);
		if (keep) {
			str_copy(p.tags, tags ? tags : "", sizeof(p.tags));
			for (char *q = p.tags; *q; q++)
				if (*q == '\t' || *q == '\n') *q = ' ';
			if (n == cap) {
				cap = cap ? cap * 2 : 32;
				BPost *nb = realloc(*out, cap * sizeof(BPost));
				if (!nb) {
					free(tags);
					break;
				}
				*out = nb;
			}
			(*out)[n++] = p;
		}
		free(tags);
	}
	return n;
}

static void search_url(int engine, const char *query, int adult, int page, const char *cursor, char *url, int n)
{
	char q[200];
	const char *safe = engine == BE_SAFEBOORU ? " rating:general" : engine == BE_DANBOORU ? " rating:g" : " rating:safe";
	snprintf(q, sizeof(q), "%s%s", query, adult ? "" : safe);
	str_trim(q);
	char *e = curl_easy_escape(NULL, q, 0);
	if (engine == BE_SAFEBOORU)
		snprintf(url, n, "https://safebooru.org/index.php?page=dapi&s=post&q=index&json=1&limit=%d&pid=%d&tags=%s",
		         PAGE_SIZE, page, e ? e : "");
	else if (engine == BE_DANBOORU)
		snprintf(url, n, "https://danbooru.donmai.us/posts.json?limit=%d&page=%d&tags=%s", PAGE_SIZE, page + 1, e ? e : "");
	else
		snprintf(url, n, "https://sankakuapi.com/v2/posts/keyset?lang=en&limit=%d&tags=%s%s%s",
		         PAGE_SIZE, e ? e : "", cursor[0] ? "&next=" : "", cursor);
	curl_free(e);
}

/* ---------------- jobs ---------------- */

enum { BJ_SEARCH, BJ_THUMB, BJ_VIEW, BJ_SEND, BJ_SAVE };

typedef struct {
	int      type;
	int      gen;
	int      idx;
	int      sidx;
	uint32_t uid;
	BPost    post;
	char     query[128];
	char     cursor[128];
	int      page;
	int      engine;
} BJob;

#define MAX_BJOBS 24
static BJob            bjobs[MAX_BJOBS];
static int             nbjobs;
static pthread_mutex_t bq;
static pthread_cond_t  bqc;

static void push(const BJob *j)
{
	pthread_mutex_lock(&bq);
	if (nbjobs == MAX_BJOBS) {
		/* drop the oldest thumbnail request */
		int drop = -1;
		for (int i = 0; i < nbjobs && drop < 0; i++)
			if (bjobs[i].type == BJ_THUMB)
				drop = i;
		if (drop < 0) {
			pthread_mutex_unlock(&bq);
			return;
		}
		memmove(&bjobs[drop], &bjobs[drop + 1], (nbjobs - drop - 1) * sizeof(BJob));
		nbjobs--;
	}
	bjobs[nbjobs++] = *j;
	pthread_cond_signal(&bqc);
	pthread_mutex_unlock(&bq);
}

static void busy(const char *s)
{
	pthread_mutex_lock(&g_lock);
	str_copy(g_net_busy, s, sizeof(g_net_busy));
	g_dirty = 1;
	pthread_mutex_unlock(&g_lock);
}

static void run_search(BJob *j)
{
	pthread_mutex_lock(&g_lock);
	int adult = g_cfg.booru_adult;
	pthread_mutex_unlock(&g_lock);
	if (j->engine == BE_SANKAKU)
		sankaku_login();
	char url[600], err[160] = "", cursor[128] = "";
	search_url(j->engine, j->query, adult, j->page, j->cursor, url, sizeof(url));
	struct curl_slist *h = j->engine == BE_SANKAKU ? sankaku_headers(0) : NULL;
	HBuf out;
	long code = 0;
	int rc = http(url, h, NULL, &out, 4 * 1024 * 1024, err, sizeof(err), &code);
	curl_slist_free_all(h);
	BPost *posts = NULL;
	int n = 0, raw = 0;
	if (rc == 0) {
		n = parse_page(j->engine, out.data ? out.data : "[]", adult, &posts, &raw, cursor, sizeof(cursor));
	} else if (out.data) {
		/* Danbooru explains errors (e.g. too many tags) in "message" */
		char *m = mj_str(mj_get(out.data, "message"));
		if (!m)
			m = mj_str(mj_get(out.data, "error"));
		if (m)
			snprintf(err, sizeof(err), "%s: %.120s", booru_engine_name(j->engine), m);
		free(m);
	}
	free(out.data);

	pthread_mutex_lock(&g_lock);
	if (j->gen == s_gen) {
		for (int i = 0; i < n && g_bres_n < BOORU_MAX_RESULTS; i++)
			g_bres[g_bres_n++] = posts[i];
		if (rc == 0) {
			s_page = j->page + 1;
			str_copy(s_cursor, cursor, sizeof(s_cursor));
			g_bres_more = raw > 0 && g_bres_n < BOORU_MAX_RESULTS &&
			              (j->engine == BE_SANKAKU ? cursor[0] != 0 : raw >= PAGE_SIZE);
			g_bres_err[0] = 0;
			/* everything on the page was filtered out: keep paging */
			if (!n && g_bres_more && !g_bres_n) {
				BJob k = *j;
				k.page = s_page;
				str_copy(k.cursor, s_cursor, sizeof(k.cursor));
				push(&k);
				pthread_mutex_unlock(&g_lock);
				free(posts);
				return;
			}
		} else {
			str_copy(g_bres_err, err, sizeof(g_bres_err));
			g_bres_more = 0;
		}
		g_bres_loading = 0;
		g_dirty = 1;
	}
	pthread_mutex_unlock(&g_lock);
	free(posts);
}

/* Downloads an image of a post (Sankaku needs a Referer). */
static int fetch_image(const BPost *p, const char *url, size_t max, HBuf *out, char *err, int errlen)
{
	struct curl_slist *h = p->engine == BE_SANKAKU ? sankaku_headers(1) : NULL;
	int rc = http(url, h, NULL, out, max, err, errlen, NULL);
	curl_slist_free_all(h);
	if (rc != 0) {
		free(out->data);
		out->data = NULL;
	}
	return rc;
}

static void run_thumb(BJob *j)
{
	HBuf out;
	char err[160];
	int rc = fetch_image(&j->post, j->post.thumb_url, 4 * 1024 * 1024, &out, err, sizeof(err));
	pthread_mutex_lock(&g_lock);
	BPost *p = (j->gen == s_gen && j->idx < g_bres_n && !strcmp(g_bres[j->idx].id, j->post.id)) ? &g_bres[j->idx] : NULL;
	if (p && rc == 0) {
		p->raw = (unsigned char *)out.data;
		p->rawlen = out.len;
		p->tstate = TS_RAW;
		out.data = NULL;
	} else if (p) {
		p->tstate = TS_ERROR;
	}
	g_dirty = 1;
	pthread_mutex_unlock(&g_lock);
	free(out.data);
}

static void favorites_save(void);

/* Sankaku links expire after about an hour: look the post up again. */
static int sankaku_expired(const BPost *p)
{
	const char *e = strstr(p->view_url, "expires=");
	return e && atoll(e + 8) < unix_ms_now() / 1000 + 120;
}

static int sankaku_refresh(BPost *p, char *err, int errlen)
{
	sankaku_login();
	char q[48], url[600];
	snprintf(q, sizeof(q), "id:%s", p->id);
	char *e = curl_easy_escape(NULL, q, 0);
	snprintf(url, sizeof(url), "https://sankakuapi.com/v2/posts/keyset?lang=en&limit=1&tags=%s", e ? e : "");
	curl_free(e);
	struct curl_slist *h = sankaku_headers(0);
	HBuf out;
	int rc = http(url, h, NULL, &out, 1024 * 1024, err, errlen, NULL);
	curl_slist_free_all(h);
	BPost *found = NULL;
	int raw = 0;
	char cursor[128];
	int n = rc == 0 ? parse_page(BE_SANKAKU, out.data ? out.data : "{}", 1, &found, &raw, cursor, sizeof(cursor)) : 0;
	free(out.data);
	if (!n) {
		free(found);
		if (rc == 0)
			snprintf(err, errlen, T("Sankaku: la imagen ya no está disponible"));
		return -1;
	}
	str_copy(p->thumb_url, found[0].thumb_url, sizeof(p->thumb_url));
	str_copy(p->view_url, found[0].view_url, sizeof(p->view_url));
	str_copy(p->file_url, found[0].file_url, sizeof(p->file_url));
	free(found);
	/* keep the fresh links in the lists too */
	pthread_mutex_lock(&g_lock);
	int fav = 0;
	for (int k = 0; k < 2; k++) {
		BPost *list = k ? g_bfav : g_bres;
		int nl = k ? g_bfav_n : g_bres_n;
		for (int i = 0; i < nl; i++)
			if (list[i].engine == BE_SANKAKU && !strcmp(list[i].id, p->id)) {
				str_copy(list[i].thumb_url, p->thumb_url, sizeof(list[i].thumb_url));
				str_copy(list[i].view_url, p->view_url, sizeof(list[i].view_url));
				str_copy(list[i].file_url, p->file_url, sizeof(list[i].file_url));
				fav |= k;
			}
	}
	if (fav)
		favorites_save();
	pthread_mutex_unlock(&g_lock);
	return 0;
}

static void run_view(BJob *j)
{
	char err[160] = "";
	HBuf out = {0};
	int rc = 0;
	if (j->post.engine == BE_SANKAKU && sankaku_expired(&j->post))
		rc = sankaku_refresh(&j->post, err, sizeof(err));
	if (rc == 0)
		rc = fetch_image(&j->post, j->post.view_url, 12 * 1024 * 1024, &out, err, sizeof(err));
	pthread_mutex_lock(&g_lock);
	free(g_fetch_buf);
	g_fetch_buf = rc == 0 ? (unsigned char *)out.data : NULL;
	g_fetch_len = rc == 0 ? out.len : 0;
	str_copy(g_fetch_err, err, sizeof(g_fetch_err));
	g_fetch_done = rc == 0 ? 1 : -1;
	g_dirty = 1;
	pthread_mutex_unlock(&g_lock);
	if (rc != 0)
		free(out.data);
}

static int write_file(const char *path, const void *data, size_t len)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	size_t w = fwrite(data, 1, len, f);
	fclose(f);
	return w == len ? 0 : -1;
}

static void safe_id(const char *id, char *out, int n)
{
	int k = 0;
	for (; *id && k < n - 1; id++)
		out[k++] = isalnum((unsigned char)*id) ? *id : '_';
	out[k] = 0;
}

static void upload_result(int sidx, uint32_t uid, int ok, const char *text)
{
	pthread_mutex_lock(&g_lock);
	g_upload_sidx = sidx;
	g_upload_uid = uid;
	str_copy(g_upload_result, text, sizeof(g_upload_result));
	g_upload_done = ok ? 1 : -1;
	g_dirty = 1;
	pthread_mutex_unlock(&g_lock);
}

static void run_send(BJob *j)
{
	BPost *p = &j->post;
	char err[160] = "";
	if (p->engine != BE_SANKAKU) {
		/* Safebooru / Danbooru links do not expire: send the original directly */
		upload_result(j->sidx, j->uid, 1, p->file_url);
		return;
	}
	if (sankaku_expired(p) && sankaku_refresh(p, err, sizeof(err)) != 0) {
		upload_result(j->sidx, j->uid, 0, err);
		return;
	}
	/* Sankaku links expire within the hour: re-upload the image */
	int original = p->file_size > 0 && p->file_size <= 8 * 1024 * 1024;
	const char *url = original ? p->file_url : p->view_url;
	busy(T("Descargando imagen..."));
	HBuf out;
	if (fetch_image(p, url, 40 * 1024 * 1024, &out, err, sizeof(err)) != 0) {
		upload_result(j->sidx, j->uid, 0, err);
		return;
	}
	char ext[8], sid[32], path[128];
	url_ext(url, ext, sizeof(ext));
	safe_id(p->id, sid, sizeof(sid));
	sceIoMkdir(CACHE_DIR, 0777);
	snprintf(path, sizeof(path), CACHE_DIR "/sankaku_%s.%s", sid, ext[0] ? ext : "jpg");
	int ok = write_file(path, out.data, out.len) == 0;
	free(out.data);
	if (!ok) {
		upload_result(j->sidx, j->uid, 0, T("No se pudo guardar el archivo"));
		return;
	}
	img_upload_request(j->sidx, j->uid, path);
}

static void run_save(BJob *j)
{
	BPost *p = &j->post;
	char err[160] = "";
	if (p->engine == BE_SANKAKU && sankaku_expired(p) && sankaku_refresh(p, err, sizeof(err)) != 0) {
		pthread_mutex_lock(&g_lock);
		ui_toast(T("No se pudo guardar: %s"), err);
		pthread_mutex_unlock(&g_lock);
		return;
	}
	busy(T("Descargando imagen..."));
	HBuf out;
	if (fetch_image(p, p->file_url, 40 * 1024 * 1024, &out, err, sizeof(err)) != 0) {
		pthread_mutex_lock(&g_lock);
		ui_toast(T("No se pudo guardar: %s"), err);
		pthread_mutex_unlock(&g_lock);
		return;
	}
	sceIoMkdir("ux0:picture", 0777);
	sceIoMkdir(PICTURE_DIR, 0777);
	char sid[32], path[160], ext[8];
	safe_id(p->id, sid, sizeof(sid));
	int ok;
	if (img_is_webp((unsigned char *)out.data, out.len)) {
		/* the Photos app does not read WebP */
		int w, h;
		unsigned char *px = webp_decode_rgba((unsigned char *)out.data, out.len, 4096, &w, &h);
		snprintf(path, sizeof(path), PICTURE_DIR "/%s_%s.jpg", booru_engine_name(p->engine), sid);
		ok = px && rgba_save_jpeg(px, w, h, w * 4, path, 92) == 0;
		free(px);
	} else {
		url_ext(p->file_url, ext, sizeof(ext));
		snprintf(path, sizeof(path), PICTURE_DIR "/%s_%s.%s", booru_engine_name(p->engine), sid, ext[0] ? ext : "jpg");
		ok = write_file(path, out.data, out.len) == 0;
	}
	free(out.data);
	pthread_mutex_lock(&g_lock);
	if (ok)
		ui_toast(T("Guardada en %s"), path);
	else
		ui_toast(T("No se pudo guardar: %s"), T("No se pudo guardar el archivo"));
	pthread_mutex_unlock(&g_lock);
}

static void *worker(void *arg)
{
	(void)arg;
	while (!g_app_quit) {
		pthread_mutex_lock(&bq);
		while (!nbjobs && !g_app_quit)
			pthread_cond_wait(&bqc, &bq);
		if (g_app_quit) {
			pthread_mutex_unlock(&bq);
			break;
		}
		/* what the user asked for goes before thumbnails */
		int pick = 0;
		for (int i = 0; i < nbjobs; i++)
			if (bjobs[i].type != BJ_THUMB) { pick = i; break; }
		BJob j = bjobs[pick];
		memmove(&bjobs[pick], &bjobs[pick + 1], (nbjobs - pick - 1) * sizeof(BJob));
		nbjobs--;
		pthread_mutex_unlock(&bq);

		switch (j.type) {
		case BJ_SEARCH: run_search(&j); break;
		case BJ_THUMB:  run_thumb(&j); break;
		case BJ_VIEW:   run_view(&j); break;
		case BJ_SEND:   run_send(&j); break;
		case BJ_SAVE:   run_save(&j); break;
		}
		if (j.type != BJ_THUMB && j.type != BJ_SEARCH)
			busy("");
	}
	return NULL;
}

/* ---------------- public API ---------------- */

void booru_search(int engine, const char *query)
{
	/* drop pending work of the previous search */
	pthread_mutex_lock(&bq);
	int k = 0;
	for (int i = 0; i < nbjobs; i++)
		if (bjobs[i].type != BJ_THUMB && bjobs[i].type != BJ_SEARCH)
			bjobs[k++] = bjobs[i];
	nbjobs = k;
	pthread_mutex_unlock(&bq);

	for (int i = 0; i < g_bres_n; i++)
		free(g_bres[i].raw);          /* textures are freed by the UI before calling */
	g_bres_n = 0;
	g_bres_more = 0;
	g_bres_err[0] = 0;
	g_bres_loading = 1;
	g_bres_engine = engine;
	str_copy(g_bres_query, query, sizeof(g_bres_query));
	s_gen++;
	s_page = 0;
	s_cursor[0] = 0;
	BJob j;
	memset(&j, 0, sizeof(j));
	j.type = BJ_SEARCH;
	j.gen = s_gen;
	j.engine = engine;
	str_copy(j.query, query, sizeof(j.query));
	push(&j);
	g_dirty = 1;
}

void booru_more(void)
{
	if (!g_bres_more || g_bres_loading)
		return;
	g_bres_loading = 1;
	BJob j;
	memset(&j, 0, sizeof(j));
	j.type = BJ_SEARCH;
	j.gen = s_gen;
	j.engine = g_bres_engine;
	j.page = s_page;
	str_copy(j.query, g_bres_query, sizeof(j.query));
	str_copy(j.cursor, s_cursor, sizeof(j.cursor));
	push(&j);
}

void booru_thumb(int i)
{
	if (i < 0 || i >= g_bres_n || g_bres[i].tstate != TS_NONE)
		return;
	g_bres[i].tstate = TS_LOADING;
	BJob j;
	memset(&j, 0, sizeof(j));
	j.type = BJ_THUMB;
	j.gen = s_gen;
	j.idx = i;
	j.post = g_bres[i];
	j.post.tex = NULL;
	j.post.raw = NULL;
	push(&j);
}

static void post_job(int type, const BPost *p, int sidx, uint32_t uid)
{
	BJob j;
	memset(&j, 0, sizeof(j));
	j.type = type;
	j.post = *p;
	j.post.tex = NULL;
	j.post.raw = NULL;
	j.sidx = sidx;
	j.uid = uid;
	push(&j);
}

void booru_view(const BPost *p)  { post_job(BJ_VIEW, p, 0, 0); }
void booru_save(const BPost *p)  { post_job(BJ_SAVE, p, 0, 0); }
void booru_send(const BPost *p, int sidx, uint32_t uid) { post_job(BJ_SEND, p, sidx, uid); }

void booru_page_url(const BPost *p, char *out, int n)
{
	if (p->engine == BE_DANBOORU)
		snprintf(out, n, "https://danbooru.donmai.us/posts/%s", p->id);
	else if (p->engine == BE_SANKAKU)
		snprintf(out, n, "https://sankaku.app/posts/%s", p->id);
	else
		snprintf(out, n, "https://safebooru.org/index.php?page=post&s=view&id=%s", p->id);
}

/* ---------------- favorites ---------------- */

void booru_fav_thumb_path(const BPost *p, char *out, int n)
{
	char sid[32];
	safe_id(p->id, sid, sizeof(sid));
	snprintf(out, n, FAV_THUMBS "/%d_%s.jpg", p->engine, sid);
}

int booru_is_fav(int engine, const char *id)
{
	for (int i = 0; i < g_bfav_n; i++)
		if (g_bfav[i].engine == engine && !strcmp(g_bfav[i].id, id))
			return 1;
	return 0;
}

/* engine, id, rating, w, h, ext, size, thumb, view, file, tags */
static void favorites_save(void)
{
	if (!g_bfav_n) {
		sceIoRemove(FAV_PATH);
		return;
	}
	FILE *f = fopen(FAV_PATH, "w");
	if (!f)
		return;
	for (int i = 0; i < g_bfav_n; i++) {
		BPost *p = &g_bfav[i];
		fprintf(f, "%d\t%s\t%c\t%d\t%d\t%s\t%ld\t%s\t%s\t%s\t%s\n", p->engine, p->id, p->rating ? p->rating : 'g',
		        p->w, p->h, p->ext, p->file_size, p->thumb_url, p->view_url, p->file_url, p->tags);
	}
	fclose(f);
}

static void favorites_load(void)
{
	FILE *f = fopen(FAV_PATH, "r");
	if (!f)
		return;
	char line[2400];
	while (g_bfav_n < BOORU_MAX_FAVS && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *fld[11];
		int nf = 0;
		fld[nf++] = line;
		for (char *q = line; *q && nf < 11; q++)
			if (*q == '\t') {
				*q = 0;
				fld[nf++] = q + 1;
			}
		if (nf < 10)
			continue;
		BPost *p = &g_bfav[g_bfav_n];
		memset(p, 0, sizeof(*p));
		p->engine = atoi(fld[0]);
		if (p->engine < 0 || p->engine >= BE_COUNT)
			continue;
		str_copy(p->id, fld[1], sizeof(p->id));
		p->rating = fld[2][0];
		p->w = atoi(fld[3]);
		p->h = atoi(fld[4]);
		str_copy(p->ext, fld[5], sizeof(p->ext));
		p->file_size = atol(fld[6]);
		str_copy(p->thumb_url, fld[7], sizeof(p->thumb_url));
		str_copy(p->view_url, fld[8], sizeof(p->view_url));
		str_copy(p->file_url, fld[9], sizeof(p->file_url));
		str_copy(p->tags, nf > 10 ? fld[10] : "", sizeof(p->tags));
		g_bfav_n++;
	}
	fclose(f);
}

int booru_fav_toggle(const BPost *p, vita2d_texture *thumb)
{
	char path[128];
	booru_fav_thumb_path(p, path, sizeof(path));
	for (int i = 0; i < g_bfav_n; i++) {
		if (g_bfav[i].engine != p->engine || strcmp(g_bfav[i].id, p->id))
			continue;
		if (g_bfav[i].tex) {
			vita2d_wait_rendering_done();
			vita2d_free_texture(g_bfav[i].tex);
		}
		free(g_bfav[i].raw);
		memmove(&g_bfav[i], &g_bfav[i + 1], (g_bfav_n - i - 1) * sizeof(BPost));
		g_bfav_n--;
		sceIoRemove(path);
		favorites_save();
		return 0;
	}
	if (g_bfav_n >= BOORU_MAX_FAVS) {
		ui_toast(T("Tienes demasiados favoritos (máximo %d)"), BOORU_MAX_FAVS);
		return 0;
	}
	memmove(&g_bfav[1], &g_bfav[0], g_bfav_n * sizeof(BPost));   /* newest first */
	g_bfav[0] = *p;
	g_bfav[0].tex = NULL;
	g_bfav[0].raw = NULL;
	g_bfav[0].tstate = TS_NONE;
	g_bfav_n++;
	sceIoMkdir(FAV_THUMBS, 0777);
	if (thumb)
		tex_save_jpeg(thumb, path, 240, 85);
	favorites_save();
	return 1;
}

/* ---------------- init ---------------- */

static void clear_cache(void)
{
	SceUID d = sceIoDopen(CACHE_DIR);
	if (d < 0)
		return;
	SceIoDirent e;
	char path[400];
	while (sceIoDread(d, &e) > 0) {
		snprintf(path, sizeof(path), CACHE_DIR "/%s", e.d_name);
		sceIoRemove(path);
	}
	sceIoDclose(d);
}

void booru_init(void)
{
	FILE *f = fopen(CA_PATH, "r");
	if (f) {
		have_ca = 1;
		fclose(f);
	}
	g_bres = calloc(BOORU_MAX_RESULTS, sizeof(BPost));
	g_bfav = calloc(BOORU_MAX_FAVS, sizeof(BPost));
	g_bres_engine = g_cfg.booru_engine;
	if (g_bfav)
		favorites_load();
	clear_cache();
	pthread_mutex_init(&bq, NULL);
	pthread_cond_init(&bqc, NULL);
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	pthread_t th;
	pthread_create(&th, &attr, worker, NULL);
	pthread_attr_destroy(&attr);
}
