#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <psp2/rtc.h>
#include <psp2/kernel/processmgr.h>

#include "util.h"

void str_copy(char *dst, const char *src, size_t size)
{
	if (!size)
		return;
	if (!src)
		src = "";
	size_t n = strlen(src);
	if (n > size - 1) {
		n = size - 1;
		while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
			n--;
	}
	memmove(dst, src, n);
	dst[n] = 0;
}

void str_trim(char *s)
{
	char *p = s;
	while (*p && isspace((unsigned char)*p))
		p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);
	size_t n = strlen(s);
	while (n && isspace((unsigned char)s[n - 1]))
		s[--n] = 0;
}

int str_icmp(const char *a, const char *b)
{
	while (*a && *b) {
		int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
		if (d)
			return d;
		a++, b++;
	}
	return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static int is_nick_char(char c)
{
	return c && (isalnum((unsigned char)c) || strchr("[]\\`_^{|}-", c));
}

int str_icontains_word(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	if (!n)
		return 0;
	for (const char *p = hay; *p; p++) {
		if (strncasecmp(p, needle, n))
			continue;
		int left_ok = (p == hay) || !is_nick_char(p[-1]);
		int right_ok = !is_nick_char(p[n]);
		if (left_ok && right_ok)
			return 1;
	}
	return 0;
}

int str_icontains(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	for (; *hay; hay++)
		if (!strncasecmp(hay, needle, n))
			return 1;
	return n == 0;
}

char *str_dup(const char *s)
{
	size_t n = strlen(s ? s : "");
	char *d = malloc(n + 1);
	if (d)
		memcpy(d, s ? s : "", n + 1);
	return d;
}

void irc_strip_format(char *s)
{
	char *r = s, *w = s;
	while (*r) {
		unsigned char c = *r;
		if (c == 0x03) {             /* color: \x03[N[N]][,N[N]] */
			r++;
			if (isdigit((unsigned char)*r)) { r++; if (isdigit((unsigned char)*r)) r++; }
			if (*r == ',' && isdigit((unsigned char)r[1])) {
				r += 2;
				if (isdigit((unsigned char)*r)) r++;
			}
			continue;
		}
		if (c == 0x04) {             /* hex color \x04RRGGBB[,RRGGBB] */
			r++;
			for (int i = 0; i < 6 && isxdigit((unsigned char)*r); i++) r++;
			if (*r == ',' && isxdigit((unsigned char)r[1])) {
				r++;
				for (int i = 0; i < 6 && isxdigit((unsigned char)*r); i++) r++;
			}
			continue;
		}
		if (c == 0x02 || c == 0x0f || c == 0x16 || c == 0x1d || c == 0x1e || c == 0x1f || c == 0x11) {
			r++;
			continue;
		}
		if (c < 0x20 && c != '\t') {
			r++;
			continue;
		}
		*w++ = *r++;
	}
	*w = 0;
}

int utf8_decode(const char *s, uint32_t *cp)
{
	const unsigned char *u = (const unsigned char *)s;
	if (u[0] < 0x80) { *cp = u[0]; return 1; }
	if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) {
		*cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F);
		return 2;
	}
	if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
		*cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
		return 3;
	}
	if ((u[0] & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 && (u[3] & 0xC0) == 0x80) {
		*cp = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F);
		return 4;
	}
	/* invalid byte: treat as Latin-1 (common on old IRC networks) */
	*cp = u[0];
	return 1;
}

int utf8_to_utf16(const char *in, uint16_t *out, int max)
{
	int n = 0;
	while (*in && n < max - 1) {
		uint32_t cp;
		in += utf8_decode(in, &cp);
		if (cp >= 0x10000) {
			if (n >= max - 2)
				break;
			cp -= 0x10000;
			out[n++] = 0xD800 | (cp >> 10);
			out[n++] = 0xDC00 | (cp & 0x3FF);
		} else {
			out[n++] = (uint16_t)cp;
		}
	}
	out[n] = 0;
	return n;
}

int utf16_to_utf8(const uint16_t *in, char *out, int max)
{
	int n = 0;
	while (*in) {
		uint32_t cp = *in++;
		if (cp >= 0xD800 && cp <= 0xDBFF && *in >= 0xDC00 && *in <= 0xDFFF)
			cp = 0x10000 + ((cp - 0xD800) << 10) + (*in++ - 0xDC00);
		char buf[4];
		int len;
		if (cp < 0x80) { buf[0] = cp; len = 1; }
		else if (cp < 0x800) { buf[0] = 0xC0 | (cp >> 6); buf[1] = 0x80 | (cp & 0x3F); len = 2; }
		else if (cp < 0x10000) { buf[0] = 0xE0 | (cp >> 12); buf[1] = 0x80 | ((cp >> 6) & 0x3F); buf[2] = 0x80 | (cp & 0x3F); len = 3; }
		else { buf[0] = 0xF0 | (cp >> 18); buf[1] = 0x80 | ((cp >> 12) & 0x3F); buf[2] = 0x80 | ((cp >> 6) & 0x3F); buf[3] = 0x80 | (cp & 0x3F); len = 4; }
		if (n + len >= max)
			break;
		memcpy(out + n, buf, len);
		n += len;
	}
	out[n] = 0;
	return n;
}

void utf8_truncate(char *s, size_t maxbytes)
{
	size_t n = strlen(s);
	if (n <= maxbytes)
		return;
	n = maxbytes;
	while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
		n--;
	s[n] = 0;
}

char *base64_encode(const unsigned char *data, size_t len)
{
	static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t olen = 4 * ((len + 2) / 3);
	char *out = malloc(olen + 1);
	if (!out)
		return NULL;
	size_t i, j = 0;
	for (i = 0; i + 2 < len; i += 3) {
		uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
		out[j++] = tbl[(v >> 18) & 63];
		out[j++] = tbl[(v >> 12) & 63];
		out[j++] = tbl[(v >> 6) & 63];
		out[j++] = tbl[v & 63];
	}
	if (i < len) {
		uint32_t v = data[i] << 16;
		if (i + 1 < len)
			v |= data[i + 1] << 8;
		out[j++] = tbl[(v >> 18) & 63];
		out[j++] = tbl[(v >> 12) & 63];
		out[j++] = (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
		out[j++] = '=';
	}
	out[j] = 0;
	return out;
}

uint32_t hash_str(const char *s)
{
	uint32_t h = 2166136261u;
	while (*s) {
		h ^= (unsigned char)*s++;
		h *= 16777619u;
	}
	return h;
}

void time_hhmm(char out[6])
{
	SceDateTime t;
	sceRtcGetCurrentClockLocalTime(&t);
	snprintf(out, 6, "%02d:%02d", t.hour % 100, t.minute % 100);
}

uint64_t now_ms(void)
{
	return sceKernelGetProcessTimeWide() / 1000;
}

static long days_from_civil(int y, int m, int d)
{
	y -= m <= 2;
	long era = (y >= 0 ? y : y - 399) / 400;
	long yoe = y - era * 400;
	long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static long dt_minutes(const SceDateTime *t)
{
	return days_from_civil(t->year, t->month, t->day) * 1440 + t->hour * 60 + t->minute;
}

static void civil_from_days(long z, int *y, int *m, int *d)
{
	z += 719468;
	long era = (z >= 0 ? z : z - 146096) / 146097;
	long doe = z - era * 146097;
	long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	long mp = (5 * doy + 2) / 153;
	*d = doy - (153 * mp + 2) / 5 + 1;
	*m = mp < 10 ? mp + 3 : mp - 9;
	*y = yoe + era * 400 + (*m <= 2);
}

/* Minutes to add to UTC to get local time (refreshed once a minute). */
static long utc_offset_min(void)
{
	static long offset;
	static uint64_t at;
	uint64_t now = now_ms();
	if (!at || now - at > 60000) {
		SceDateTime loc, utc;
		sceRtcGetCurrentClockLocalTime(&loc);
		sceRtcGetCurrentClock(&utc, 0);
		long d = dt_minutes(&loc) - dt_minutes(&utc);
		/* round to a quarter hour: both clocks may straddle a minute */
		offset = (d >= 0 ? d + 7 : d - 7) / 15 * 15;
		at = now;
	}
	return offset;
}

int64_t unix_ms_now(void)
{
	SceDateTime u;
	sceRtcGetCurrentClock(&u, 0);
	return ((int64_t)days_from_civil(u.year, u.month, u.day) * 86400 + u.hour * 3600 + u.minute * 60 + u.second) * 1000
	       + u.microsecond / 1000;
}

int iso_to_unix_ms(const char *iso, int64_t *out)
{
	int Y, M, D, h, m, sec = 0, n = 0;
	if (sscanf(iso, "%4d-%2d-%2dT%2d:%2d%n", &Y, &M, &D, &h, &m, &n) != 5)
		return -1;
	const char *p = iso + n;
	int ms = 0;
	if (*p == ':') {
		p++;
		sec = atoi(p);
		while (isdigit((unsigned char)*p)) p++;
		if (*p == '.') {
			int scale = 100;
			for (p++; isdigit((unsigned char)*p); p++) {
				ms += (*p - '0') * scale;
				scale /= 10;
			}
		}
	}
	*out = ((int64_t)days_from_civil(Y, M, D) * 86400 + h * 3600 + m * 60 + sec) * 1000 + ms;
	return 0;
}

void unix_ms_to_iso(int64_t ms, char out[32])
{
	int64_t secs = ms / 1000;
	long days = (long)(secs / 86400);
	long rem = (long)(secs % 86400);
	int y, mo, d;
	civil_from_days(days, &y, &mo, &d);
	snprintf(out, 32, "%04d-%02d-%02dT%02ld:%02ld:%02ld.%03dZ", y, mo, d, rem / 3600, rem / 60 % 60, rem % 60, (int)(ms % 1000));
}

static int64_t local_minutes(int64_t ms)
{
	return ms / 60000 + utc_offset_min();
}

void ts_local_hhmm(int64_t ms, char out[6])
{
	long mins = (long)(((local_minutes(ms) % 1440) + 1440) % 1440);
	snprintf(out, 6, "%02ld:%02ld", mins / 60, mins % 60);
}

int ts_local_ymd(int64_t ms)
{
	int64_t lm = local_minutes(ms);
	long days = (long)(lm >= 0 ? lm / 1440 : (lm - 1439) / 1440);
	int y, m, d;
	civil_from_days(days, &y, &m, &d);
	return y * 10000 + m * 100 + d;
}

int ts_local_weekday(int64_t ms)
{
	int64_t lm = local_minutes(ms);
	long days = (long)(lm >= 0 ? lm / 1440 : (lm - 1439) / 1440);
	return (int)(((days % 7) + 11) % 7);   /* 1970-01-01 was a Thursday */
}

int64_t local_to_unix_ms(int ymd, int hour, int minute)
{
	long days = days_from_civil(ymd / 10000, ymd / 100 % 100, ymd % 100);
	return ((int64_t)days * 1440 + hour * 60 + minute - utc_offset_min()) * 60000;
}

int ymd_diff_days(int a, int b)
{
	return (int)(days_from_civil(b / 10000, b / 100 % 100, b % 100) - days_from_civil(a / 10000, a / 100 % 100, a % 100));
}

int iso_to_local_hhmm(const char *iso, char out[6])
{
	int Y, M, D, h, m;
	if (sscanf(iso, "%4d-%2d-%2dT%2d:%2d", &Y, &M, &D, &h, &m) != 5)
		return -1;
	SceDateTime loc, utc;
	sceRtcGetCurrentClockLocalTime(&loc);
	sceRtcGetCurrentClock(&utc, 0);
	long offset = dt_minutes(&loc) - dt_minutes(&utc);
	long t = days_from_civil(Y, M, D) * 1440 + h * 60 + m + offset;
	long mins = ((t % 1440) + 1440) % 1440;
	snprintf(out, 6, "%02ld:%02ld", mins / 60, mins % 60);
	return 0;
}

int irc_strip_format_spans(char *s, FmtSpan *spans, int max)
{
	char *r = s, *w = s;
	int n = 0;
	FmtSpan cur = { 0, -1, -1, 0, 0, 0 };
	int changed = 0;
	while (*r) {
		unsigned char c = *r;
		if (c == 0x03) {
			r++;
			if (isdigit((unsigned char)*r)) {
				int fg = *r++ - '0';
				if (isdigit((unsigned char)*r)) fg = fg * 10 + (*r++ - '0');
				cur.fg = fg < 16 ? fg : -1;
				if (*r == ',' && isdigit((unsigned char)r[1])) {
					r++;
					int bg = *r++ - '0';
					if (isdigit((unsigned char)*r)) bg = bg * 10 + (*r++ - '0');
					cur.bg = bg < 16 ? bg : -1;
				}
			} else {
				cur.fg = cur.bg = -1;
			}
			changed = 1;
			continue;
		}
		if (c == 0x04) {          /* hex colors: drop, keep default color */
			r++;
			for (int i = 0; i < 6 && isxdigit((unsigned char)*r); i++) r++;
			if (*r == ',' && isxdigit((unsigned char)r[1])) {
				r++;
				for (int i = 0; i < 6 && isxdigit((unsigned char)*r); i++) r++;
			}
			continue;
		}
		if (c == 0x02) { cur.bold = !cur.bold; changed = 1; r++; continue; }
		if (c == 0x1f) { cur.underline = !cur.underline; changed = 1; r++; continue; }
		if (c == 0x1d) { cur.italic = !cur.italic; changed = 1; r++; continue; }
		if (c == 0x0f) { cur.fg = cur.bg = -1; cur.bold = cur.underline = cur.italic = 0; changed = 1; r++; continue; }
		if (c == 0x16 || c == 0x1e || c == 0x11 || (c < 0x20 && c != '\t')) { r++; continue; }
		if (changed) {
			cur.off = (uint16_t)(w - s);
			if (n > 0 && spans[n - 1].off == cur.off)
				spans[n - 1] = cur;
			else if (n < max)
				spans[n++] = cur;
			changed = 0;
		}
		*w++ = *r++;
	}
	*w = 0;
	/* a single "reset" span at the start carries no information */
	if (n == 1 && spans[0].fg < 0 && spans[0].bg < 0 && !spans[0].bold && !spans[0].underline && !spans[0].italic)
		n = 0;
	return n;
}

int glob_imatch(const char *p, const char *s)
{
	while (*p) {
		if (*p == '*') {
			while (*p == '*') p++;
			if (!*p) return 1;
			for (; *s; s++)
				if (glob_imatch(p, s)) return 1;
			return 0;
		}
		if (!*s) return 0;
		if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s))
			return 0;
		p++, s++;
	}
	return !*s;
}
