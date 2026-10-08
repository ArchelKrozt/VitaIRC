#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minijson.h"

static const char *ws(const char *p)
{
	while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	return p;
}

static const char *skip_string(const char *p)
{
	p++;    /* opening quote */
	while (*p && *p != '"') {
		if (*p == '\\' && p[1])
			p++;
		p++;
	}
	return *p ? p + 1 : NULL;
}

/* Returns the pointer right after the value starting at p. */
static const char *skip_value(const char *p)
{
	p = ws(p);
	if (!p || !*p)
		return NULL;
	if (*p == '"')
		return skip_string(p);
	if (*p == '{' || *p == '[') {
		int depth = 0;
		while (*p) {
			if (*p == '"') {
				p = skip_string(p);
				if (!p)
					return NULL;
				continue;
			}
			if (*p == '{' || *p == '[')
				depth++;
			else if (*p == '}' || *p == ']') {
				depth--;
				if (depth == 0)
					return p + 1;
			}
			p++;
		}
		return NULL;
	}
	/* number, true, false, null */
	while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t')
		p++;
	return p;
}

const char *mj_get(const char *obj, const char *key)
{
	const char *p = ws(obj);
	if (!p || *p != '{')
		return NULL;
	p = ws(p + 1);
	size_t klen = strlen(key);
	while (p && *p == '"') {
		const char *kstart = p + 1;
		const char *kend = skip_string(p);
		if (!kend)
			return NULL;
		int match = (size_t)(kend - 1 - kstart) == klen && !strncmp(kstart, key, klen);
		p = ws(kend);
		if (!p || *p != ':')
			return NULL;
		p = ws(p + 1);
		if (match)
			return p;
		p = ws(skip_value(p));
		if (!p || *p != ',')
			return NULL;
		p = ws(p + 1);
	}
	return NULL;
}

const char *mj_at(const char *arr, int idx)
{
	const char *p = ws(arr);
	if (!p || *p != '[')
		return NULL;
	p = ws(p + 1);
	for (int i = 0; p && *p && *p != ']'; i++) {
		if (i == idx)
			return p;
		p = ws(skip_value(p));
		if (!p || *p != ',')
			return NULL;
		p = ws(p + 1);
	}
	return NULL;
}

static int put_utf8(char *o, unsigned cp)
{
	if (cp < 0x80) { o[0] = cp; return 1; }
	if (cp < 0x800) { o[0] = 0xC0 | (cp >> 6); o[1] = 0x80 | (cp & 0x3F); return 2; }
	if (cp < 0x10000) { o[0] = 0xE0 | (cp >> 12); o[1] = 0x80 | ((cp >> 6) & 0x3F); o[2] = 0x80 | (cp & 0x3F); return 3; }
	o[0] = 0xF0 | (cp >> 18); o[1] = 0x80 | ((cp >> 12) & 0x3F); o[2] = 0x80 | ((cp >> 6) & 0x3F); o[3] = 0x80 | (cp & 0x3F);
	return 4;
}

static unsigned hex4(const char *p)
{
	unsigned v = 0;
	for (int i = 0; i < 4; i++) {
		char c = p[i];
		v <<= 4;
		if (c >= '0' && c <= '9') v |= c - '0';
		else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
		else return 0xFFFFFFFF;
	}
	return v;
}

char *mj_str(const char *val)
{
	const char *p = ws(val);
	if (!p || *p != '"')
		return NULL;
	const char *end = skip_string(p);
	if (!end)
		return NULL;
	char *out = malloc(end - p + 1);
	if (!out)
		return NULL;
	int n = 0;
	for (p++; *p && *p != '"'; p++) {
		if (*p != '\\') {
			out[n++] = *p;
			continue;
		}
		p++;
		switch (*p) {
		case 'n': out[n++] = '\n'; break;
		case 't': out[n++] = '\t'; break;
		case 'r': out[n++] = '\r'; break;
		case 'b': out[n++] = '\b'; break;
		case 'f': out[n++] = '\f'; break;
		case 'u': {
			unsigned cp = hex4(p + 1);
			if (cp == 0xFFFFFFFF) break;
			p += 4;
			if (cp >= 0xD800 && cp <= 0xDBFF && p[1] == '\\' && p[2] == 'u') {
				unsigned lo = hex4(p + 3);
				if (lo >= 0xDC00 && lo <= 0xDFFF) {
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
					p += 6;
				}
			}
			n += put_utf8(out + n, cp);
			break;
		}
		default: out[n++] = *p; break;   /* \" \\ \/ */
		}
	}
	out[n] = 0;
	return out;
}

char *mj_scalar(const char *val)
{
	const char *p = ws(val);
	if (!p || !*p)
		return NULL;
	if (*p == '"')
		return mj_str(p);
	if (*p == '{' || *p == '[' || !strncmp(p, "null", 4))
		return NULL;
	const char *e = skip_value(p);
	if (!e || e == p)
		return NULL;
	char *out = malloc(e - p + 1);
	if (!out)
		return NULL;
	memcpy(out, p, e - p);
	out[e - p] = 0;
	return out;
}

void mj_append_raw(char **buf, size_t *len, const char *s)
{
	size_t n = strlen(s);
	char *nb = realloc(*buf, *len + n + 1);
	if (!nb)
		return;
	memcpy(nb + *len, s, n + 1);
	*buf = nb;
	*len += n;
}

void mj_append_str(char **buf, size_t *len, const char *s)
{
	size_t cap = strlen(s) * 6 + 3;
	char *tmp = malloc(cap);
	if (!tmp)
		return;
	size_t n = 0;
	tmp[n++] = '"';
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '"':  tmp[n++] = '\\'; tmp[n++] = '"'; break;
		case '\\': tmp[n++] = '\\'; tmp[n++] = '\\'; break;
		case '\n': tmp[n++] = '\\'; tmp[n++] = 'n'; break;
		case '\r': tmp[n++] = '\\'; tmp[n++] = 'r'; break;
		case '\t': tmp[n++] = '\\'; tmp[n++] = 't'; break;
		default:
			if (*p < 0x20)
				n += sprintf(tmp + n, "\\u%04x", *p);
			else
				tmp[n++] = *p;
		}
	}
	tmp[n++] = '"';
	tmp[n] = 0;
	mj_append_raw(buf, len, tmp);
	free(tmp);
}
