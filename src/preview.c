#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <curl/curl.h>

#include "preview.h"
#include "irc.h"
#include "util.h"
#include "i18n.h"
#include "minijson.h"
#include "imgutil.h"
#include "httpc.h"

#define PV_MAX      40
#define PV_THUMB_W  320
#define PV_THUMB_H  240
#define PAGE_MAX    (768 * 1024)
#define IMAGE_MAX   (8 * 1024 * 1024)
#define PAGE_UA     "Mozilla/5.0 (compatible; VitaIRC/" APP_VERSION "; link preview)"

static Preview         pv[PV_MAX];
static vita2d_texture *dead[PV_MAX];   /* evicted textures, freed before the next frame */
static int             ndead;
static int             changed;

/* ---------------- URL helpers ---------------- */

static void url_path_ext(const char *url, char *ext, int n)
{
	char path[300];
	str_copy(path, url, sizeof(path));
	path[strcspn(path, "?#")] = 0;
	const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
	str_copy(ext, dot && slash && dot > slash ? dot + 1 : "", n);
	for (char *p = ext; *p; p++)
		*p = tolower((unsigned char)*p);
}

static const char *url_host(const char *url, char *host, int n)
{
	const char *h = strstr(url, "://");
	h = h ? h + 3 : url;
	int k = 0;
	while (h[k] && h[k] != '/' && h[k] != '?' && h[k] != '#' && h[k] != ':' && k < n - 1) {
		host[k] = tolower((unsigned char)h[k]);
		k++;
	}
	host[k] = 0;
	return !strncmp(host, "www.", 4) ? host + 4 : host;
}

/* imgur.com/<id> -> its picture (albums and galleries stay pages) */
static int imgur_page_id(const char *url, char *id, int n)
{
	char host[96];
	const char *h = url_host(url, host, sizeof(host));
	if (strcmp(h, "imgur.com") && strcmp(h, "m.imgur.com"))
		return 0;
	const char *p = strstr(url, "imgur.com/") + 10;
	if (!*p || !strncmp(p, "a/", 2) || !strncmp(p, "gallery/", 8) || !strncmp(p, "t/", 2) || !strncmp(p, "user/", 5))
		return 0;
	str_copy(id, p, n);
	id[strcspn(id, "/?#.")] = 0;
	return id[0] != 0;
}

int pv_image_url(const char *url, char *direct, int n)
{
	char ext[8], id[64];
	url_path_ext(url, ext, sizeof(ext));
	if (!strcmp(ext, "png") || !strcmp(ext, "jpg") || !strcmp(ext, "jpeg") || !strcmp(ext, "webp")) {
		str_copy(direct, url, n);
		return 1;
	}
	if (imgur_page_id(url, id, sizeof(id))) {
		snprintf(direct, n, "https://i.imgur.com/%s.jpg", id);
		return 1;
	}
	return 0;
}

int pv_kind_of(const char *url)
{
	char ext[8], id[64];
	url_path_ext(url, ext, sizeof(ext));
	if (!strcmp(ext, "png") || !strcmp(ext, "jpg") || !strcmp(ext, "jpeg") || !strcmp(ext, "webp") || !strcmp(ext, "gif"))
		return PK_IMAGE;
	if (!strcmp(ext, "mp4") || !strcmp(ext, "m4v") || !strcmp(ext, "mov") || !strcmp(ext, "webm") || !strcmp(ext, "gifv"))
		return PK_VIDEO;
	if (imgur_page_id(url, id, sizeof(id)))
		return PK_IMAGE;
	return PK_PAGE;
}

void pv_video_url(const char *url, char *out, int n)
{
	char ext[8];
	url_path_ext(url, ext, sizeof(ext));
	str_copy(out, url, n);
	if (!strcmp(ext, "gifv")) {             /* Imgur's .gifv is an MP4 */
		char *dot = strrchr(out, '.');
		if (dot && (size_t)(dot - out) + 5 < (size_t)n)
			strcpy(dot, ".mp4");
	}
}

/* Resolves a link found in a page against the page's address. */
static void resolve(const char *base, const char *ref, char *out, int n)
{
	if (!strncmp(ref, "http://", 7) || !strncmp(ref, "https://", 8)) {
		str_copy(out, ref, n);
	} else if (!strncmp(ref, "//", 2)) {
		snprintf(out, n, "https:%s", ref);
	} else {
		char origin[300];
		str_copy(origin, base, sizeof(origin));
		char *p = strstr(origin, "://");
		char *slash = p ? strchr(p + 3, '/') : NULL;
		if (ref[0] == '/') {
			if (slash) *slash = 0;
			snprintf(out, n, "%s%s", origin, ref);
		} else {
			char *last = strrchr(origin, '/');
			if (last && slash && last >= slash)
				last[1] = 0;
			snprintf(out, n, "%s%s%s", origin, last && slash && last >= slash ? "" : "/", ref);
		}
	}
}

/* ---------------- HTML ---------------- */

static const char *stristr(const char *h, const char *end, const char *n)
{
	size_t k = strlen(n);
	for (; h + k <= end; h++)
		if (!strncasecmp(h, n, k))
			return h;
	return NULL;
}

/* Value of attribute `name` inside <tag ...> (between p and end). */
static int attr(const char *p, const char *end, const char *name, char *out, int n)
{
	size_t k = strlen(name);
	for (const char *q = p; q + k < end; q++) {
		if (q == p || strncasecmp(q, name, k) || !isspace((unsigned char)q[-1]))
			continue;
		const char *v = q + k;
		while (v < end && isspace((unsigned char)*v)) v++;
		if (v >= end || *v != '=')
			continue;
		v++;
		while (v < end && isspace((unsigned char)*v)) v++;
		char quote = (*v == '"' || *v == '\'') ? *v++ : 0;
		int len = 0;
		while (v < end && (quote ? *v != quote : !isspace((unsigned char)*v) && *v != '>') && len < n - 1)
			out[len++] = *v++;
		out[len] = 0;
		return 1;
	}
	return 0;
}

static int put_utf8(char *o, unsigned cp)
{
	if (cp < 0x80) { o[0] = cp; return 1; }
	if (cp < 0x800) { o[0] = 0xC0 | (cp >> 6); o[1] = 0x80 | (cp & 0x3F); return 2; }
	if (cp < 0x10000) { o[0] = 0xE0 | (cp >> 12); o[1] = 0x80 | ((cp >> 6) & 0x3F); o[2] = 0x80 | (cp & 0x3F); return 3; }
	o[0] = 0xF0 | (cp >> 18); o[1] = 0x80 | ((cp >> 12) & 0x3F); o[2] = 0x80 | ((cp >> 6) & 0x3F); o[3] = 0x80 | (cp & 0x3F);
	return 4;
}

/* &amp; &#39; &#x27; ... and runs of whitespace -> one space */
static void html_text(char *s)
{
	static const struct { const char *name; unsigned cp; } ents[] = {
		{ "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", ' ' },
		{ "hellip", 0x2026 }, { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "laquo", 0xAB }, { "raquo", 0xBB },
	};
	char *w = s;
	int space = 0;
	for (char *r = s; *r; ) {
		unsigned cp = 0;
		int used = 0;
		if (*r == '&') {
			char *semi = strchr(r, ';');
			if (semi && semi - r < 10) {
				if (r[1] == '#')
					cp = (r[2] == 'x' || r[2] == 'X') ? strtoul(r + 3, NULL, 16) : strtoul(r + 2, NULL, 10);
				else
					for (unsigned i = 0; i < sizeof(ents) / sizeof(ents[0]); i++)
						if ((size_t)(semi - r - 1) == strlen(ents[i].name) && !strncmp(r + 1, ents[i].name, semi - r - 1))
							cp = ents[i].cp;
				if (cp)
					used = semi - r + 1;
			}
		}
		if (used) {
			r += used;
		} else {
			cp = (unsigned char)*r++;
			if (cp >= 0x80) {          /* copy UTF-8 bytes as they are */
				*w++ = cp;
				space = 0;
				continue;
			}
		}
		if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') {
			if (!space && w != s)
				*w++ = ' ';
			space = 1;
			continue;
		}
		space = 0;
		w += put_utf8(w, cp);       /* entities never expand past their source */
	}
	while (w > s && w[-1] == ' ')
		w--;
	*w = 0;
}

static void parse_html(const char *html, size_t len, Preview *r, char *img, int imgn)
{
	const char *end = html + len;
	const char *head_end = stristr(html, end, "</head");
	if (head_end)
		end = head_end;
	char tw_title[160] = "", tw_desc[200] = "", desc[200] = "", tw_img[400] = "";
	img[0] = 0;
	for (const char *p = html; (p = stristr(p, end, "<meta")); p += 5) {
		const char *e = memchr(p, '>', end - p);
		if (!e)
			break;
		char key[48] = "", val[400];
		if (!attr(p, e, "property", key, sizeof(key)))
			attr(p, e, "name", key, sizeof(key));
		if (!key[0] || !attr(p, e, "content", val, sizeof(val)))
			continue;
		for (char *k = key; *k; k++)
			*k = tolower((unsigned char)*k);
		if (!strcmp(key, "og:title")) str_copy(r->title, val, sizeof(r->title));
		else if (!strcmp(key, "twitter:title")) str_copy(tw_title, val, sizeof(tw_title));
		else if (!strcmp(key, "og:description")) str_copy(r->desc, val, sizeof(r->desc));
		else if (!strcmp(key, "twitter:description")) str_copy(tw_desc, val, sizeof(tw_desc));
		else if (!strcmp(key, "description")) str_copy(desc, val, sizeof(desc));
		else if (!strcmp(key, "og:site_name")) str_copy(r->site, val, sizeof(r->site));
		else if ((!strcmp(key, "og:image") || !strcmp(key, "og:image:secure_url") || !strcmp(key, "og:image:url")) && !img[0])
			str_copy(img, val, imgn);
		else if ((!strcmp(key, "twitter:image") || !strcmp(key, "twitter:image:src")) && !tw_img[0])
			str_copy(tw_img, val, sizeof(tw_img));
		else if (!strcmp(key, "og:type") && !strncmp(val, "video", 5))
			r->video_site = 1;
	}
	if (!r->title[0])
		str_copy(r->title, tw_title, sizeof(r->title));
	if (!r->title[0]) {
		const char *t = stristr(html, end, "<title");
		const char *gt = t ? memchr(t, '>', end - t) : NULL;
		const char *te = gt ? stristr(gt, end, "</title") : NULL;
		if (te) {
			int n = te - gt - 1 < (int)sizeof(r->title) - 1 ? te - gt - 1 : (int)sizeof(r->title) - 1;
			memcpy(r->title, gt + 1, n);
			r->title[n] = 0;
		}
	}
	if (!r->desc[0])
		str_copy(r->desc, tw_desc[0] ? tw_desc : desc, sizeof(r->desc));
	if (!img[0])
		str_copy(img, tw_img, imgn);
	html_text(r->title);
	html_text(r->desc);
	html_text(r->site);
	html_text(img);
}

/* ---------------- fetching ---------------- */

/* Downloads a picture and keeps a small copy for the chat. */
static void load_thumb(Preview *r, const char *url)
{
	HReq rq = { NULL, NULL, PAGE_UA, IMAGE_MAX, 45, 0 };
	HBuf out;
	char err[160];
	int rc = httpc(url, &rq, &out, NULL, err, sizeof(err));
	if (rc == 0 && out.data) {
		if (img_is_gif((unsigned char *)out.data, out.len))
			r->gif = 1;
		else
			r->rgba = img_decode_thumb((unsigned char *)out.data, out.len, PV_THUMB_W, PV_THUMB_H, &r->tw, &r->th);
	}
	free(out.data);
}

static void fetch_video(Preview *r)
{
	char host[96];
	pv_video_url(r->url, r->media, sizeof(r->media));
	HReq rq = { NULL, NULL, PAGE_UA, 0, 20, 1 };
	HBuf out;
	HRes rs;
	char err[160], ext[8];
	if (httpc(r->media, &rq, &out, &rs, err, sizeof(err)) == 0)
		r->size = rs.length;
	free(out.data);
	url_path_ext(r->media, ext, sizeof(ext));
	r->playable = !strcmp(ext, "mp4") || !strcmp(ext, "m4v") || !strcmp(ext, "mov") ||
	              !strcmp(rs.ctype, "video/mp4") || !strcmp(rs.ctype, "video/quicktime");
	str_copy(r->site, url_host(r->url, host, sizeof(host)), sizeof(r->site));
	const char *name = strrchr(r->media, '/');
	str_copy(r->title, name ? name + 1 : r->media, sizeof(r->title));
	r->title[strcspn(r->title, "?#")] = 0;
	/* Imgur keeps a still picture of each video */
	if (!strcmp(r->site, "i.imgur.com")) {
		char poster[300];
		str_copy(poster, r->media, sizeof(poster));
		char *dot = strrchr(poster, '.');
		if (dot) {
			strcpy(dot, ".jpg");
			load_thumb(r, poster);
		}
	}
	r->state = PS_READY;
}

static int is_youtube(const char *url)
{
	char host[96];
	const char *h = url_host(url, host, sizeof(host));
	return !strcmp(h, "youtube.com") || !strcmp(h, "m.youtube.com") || !strcmp(h, "youtu.be") ||
	       !strcmp(h, "music.youtube.com");
}

static void fetch_youtube(Preview *r)
{
	char api[600];
	char *e = curl_easy_escape(NULL, r->url, 0);
	snprintf(api, sizeof(api), "https://www.youtube.com/oembed?format=json&url=%s", e ? e : "");
	curl_free(e);
	HReq rq = { NULL, NULL, NULL, 64 * 1024, 20, 0 };
	HBuf out;
	char err[160];
	if (httpc(api, &rq, &out, NULL, err, sizeof(err)) == 0 && out.data) {
		char *t = mj_str(mj_get(out.data, "title"));
		char *a = mj_str(mj_get(out.data, "author_name"));
		char *th = mj_str(mj_get(out.data, "thumbnail_url"));
		str_copy(r->title, t ? t : "", sizeof(r->title));
		str_copy(r->desc, a ? a : "", sizeof(r->desc));
		str_copy(r->site, "YouTube", sizeof(r->site));
		r->video_site = 1;
		if (th)
			load_thumb(r, th);
		free(t);
		free(a);
		free(th);
	}
	free(out.data);
	r->state = r->title[0] ? PS_READY : PS_FAIL;
}

static void fetch_page(Preview *r)
{
	if (is_youtube(r->url)) {
		fetch_youtube(r);
		return;
	}
	/* a link without extension may still be a picture or a video */
	HReq head = { NULL, NULL, PAGE_UA, 0, 20, 1 };
	HBuf out;
	HRes rs;
	char err[160];
	if (httpc(r->url, &head, &out, &rs, err, sizeof(err)) == 0) {
		free(out.data);
		if (!strncmp(rs.ctype, "image/", 6)) {
			r->kind = PK_IMAGE;
			str_copy(r->media, r->url, sizeof(r->media));
			load_thumb(r, r->url);
			r->state = r->rgba || r->gif ? PS_READY : PS_FAIL;
			return;
		}
		if (!strncmp(rs.ctype, "video/", 6)) {
			r->kind = PK_VIDEO;
			fetch_video(r);
			return;
		}
	} else {
		free(out.data);
	}
	HReq get = { NULL, NULL, PAGE_UA, PAGE_MAX, 25, 0 };
	int rc = httpc(r->url, &get, &out, &rs, err, sizeof(err));
	/* servers that refuse HEAD: the answer itself says what the link is */
	if (rc >= 0 && !strncmp(rs.ctype, "image/", 6)) {
		r->kind = PK_IMAGE;
		str_copy(r->media, rs.final_url[0] ? rs.final_url : r->url, sizeof(r->media));
		if (rc == 0 && out.data && img_is_gif((unsigned char *)out.data, out.len))
			r->gif = 1;
		else if (rc == 0 && out.data)
			r->rgba = img_decode_thumb((unsigned char *)out.data, out.len, PV_THUMB_W, PV_THUMB_H, &r->tw, &r->th);
		else
			load_thumb(r, r->media);       /* bigger than a page: fetch it as a picture */
		free(out.data);
		r->state = r->rgba || r->gif ? PS_READY : PS_FAIL;
		return;
	}
	if (rc >= 0 && !strncmp(rs.ctype, "video/", 6)) {
		free(out.data);
		r->kind = PK_VIDEO;
		fetch_video(r);
		return;
	}
	if (rc < 0 || !out.data || (rs.ctype[0] && strcmp(rs.ctype, "text/html") && strcmp(rs.ctype, "application/xhtml+xml"))) {
		free(out.data);
		r->state = PS_FAIL;
		return;
	}
	char img[400], host[96];
	parse_html(out.data, out.len, r, img, sizeof(img));
	free(out.data);
	if (!r->site[0])
		str_copy(r->site, url_host(rs.final_url[0] ? rs.final_url : r->url, host, sizeof(host)), sizeof(r->site));
	if (img[0]) {
		char abs[400];
		resolve(rs.final_url[0] ? rs.final_url : r->url, img, abs, sizeof(abs));
		load_thumb(r, abs);
	}
	r->state = (r->title[0] || r->rgba) ? PS_READY : PS_FAIL;
}

static void fetch(Preview *r)
{
	if (r->kind == PK_IMAGE) {
		if (!pv_image_url(r->url, r->media, sizeof(r->media)))
			str_copy(r->media, r->url, sizeof(r->media));
		load_thumb(r, r->media);
		r->state = r->rgba || r->gif ? PS_READY : PS_FAIL;
	} else if (r->kind == PK_VIDEO) {
		fetch_video(r);
	} else {
		fetch_page(r);
	}
}

/* ---------------- worker ---------------- */

typedef struct { int slot; uint32_t hash; char url[300]; int kind; } PJob;
#define MAX_PJOBS 16
static PJob            pjobs[MAX_PJOBS];
static int             npjobs;
static pthread_mutex_t pq;
static pthread_cond_t  pqc;

static void *worker(void *arg)
{
	(void)arg;
	while (!g_app_quit) {
		pthread_mutex_lock(&pq);
		while (!npjobs && !g_app_quit)
			pthread_cond_wait(&pqc, &pq);
		if (g_app_quit) {
			pthread_mutex_unlock(&pq);
			break;
		}
		PJob j = pjobs[--npjobs];          /* newest first: what is on screen now */
		pthread_mutex_unlock(&pq);

		Preview r;
		memset(&r, 0, sizeof(r));
		str_copy(r.url, j.url, sizeof(r.url));
		r.kind = j.kind;
		r.size = -1;
		fetch(&r);

		pthread_mutex_lock(&g_lock);
		Preview *p = &pv[j.slot];
		if (p->hash == j.hash && !strcmp(p->url, j.url) && p->state == PS_LOADING) {
			if (r.kind != p->kind || r.state == PS_FAIL || r.gif)
				changed = 1;
			free(p->rgba);
			vita2d_texture *tex = p->tex;
			uint64_t used = p->used;
			*p = r;
			p->hash = j.hash;
			p->tex = tex;
			p->used = used;
		} else {
			free(r.rgba);
		}
		g_dirty = 1;
		pthread_mutex_unlock(&g_lock);
	}
	return NULL;
}

static void request(int slot)
{
	pthread_mutex_lock(&pq);
	if (npjobs == MAX_PJOBS) {
		pv[pjobs[0].slot].state = PS_NONE;   /* asked again when shown */
		memmove(&pjobs[0], &pjobs[1], (MAX_PJOBS - 1) * sizeof(PJob));
		npjobs--;
	}
	PJob *j = &pjobs[npjobs++];
	j->slot = slot;
	j->hash = pv[slot].hash;
	j->kind = pv[slot].kind;
	str_copy(j->url, pv[slot].url, sizeof(j->url));
	pv[slot].state = PS_LOADING;
	pthread_cond_signal(&pqc);
	pthread_mutex_unlock(&pq);
}

/* ---------------- cache ---------------- */

static uint32_t url_hash(const char *url)
{
	uint32_t h = hash_str(url);
	return h ? h : 1;
}

Preview *pv_find(const char *url)
{
	uint32_t h = url_hash(url);
	for (int i = 0; i < PV_MAX; i++)
		if (pv[i].hash == h && !strcmp(pv[i].url, url))
			return &pv[i];
	return NULL;
}

Preview *pv_get(const char *url)
{
	Preview *p = pv_find(url);
	uint64_t now = now_ms();
	if (p) {
		p->used = now;
		if (p->state == PS_NONE)
			request(p - pv);
		return p;
	}
	int slot = -1;
	for (int i = 0; i < PV_MAX && slot < 0; i++)
		if (!pv[i].hash)
			slot = i;
	/* full: reuse the least recently shown one that is off screen and idle */
	for (int k = 0; k < PV_MAX && !(slot >= 0 && !pv[slot].hash); k++)
		if (pv[k].state != PS_LOADING && now - pv[k].used > 2000 && (slot < 0 || pv[k].used < pv[slot].used))
			slot = k;
	if (slot < 0)
		return NULL;
	p = &pv[slot];
	if (p->tex && ndead < PV_MAX)
		dead[ndead++] = p->tex;
	free(p->rgba);
	memset(p, 0, sizeof(*p));
	p->hash = url_hash(url);
	str_copy(p->url, url, sizeof(p->url));
	p->kind = pv_kind_of(url);
	p->size = -1;
	p->used = now;
	request(slot);
	return p;
}

int pv_frame(void)
{
	if (ndead) {
		vita2d_wait_rendering_done();
		for (int i = 0; i < ndead; i++)
			vita2d_free_texture(dead[i]);
		ndead = 0;
	}
	int made = 0;
	for (int i = 0; i < PV_MAX && made < 2; i++)
		if (pv[i].rgba && !pv[i].tex) {
			pv[i].tex = tex_from_rgba(pv[i].rgba, pv[i].tw, pv[i].th);
			free(pv[i].rgba);
			pv[i].rgba = NULL;
			made++;
			g_dirty = 1;
		}
	int c = changed;
	changed = 0;
	return c;
}

void pv_init(void)
{
	pthread_mutex_init(&pq, NULL);
	pthread_cond_init(&pqc, NULL);
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	pthread_t th;
	pthread_create(&th, &attr, worker, NULL);
	pthread_attr_destroy(&attr);
}
