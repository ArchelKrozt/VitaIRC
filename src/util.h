#ifndef VITAIRC_UTIL_H
#define VITAIRC_UTIL_H

#include <stddef.h>
#include <stdint.h>

void   str_copy(char *dst, const char *src, size_t size);
void   str_trim(char *s);
int    str_icmp(const char *a, const char *b);
int    str_icontains_word(const char *hay, const char *needle);
int    str_icontains(const char *hay, const char *needle);
char  *str_dup(const char *s);

/* Removes mIRC color/format control codes in place. */
void   irc_strip_format(char *s);

/* Formatting run inside a stripped message (offset into the plain text). */
typedef struct {
	uint16_t off;
	int8_t   fg, bg;        /* mIRC color index, -1 = default */
	uint8_t  bold, underline, italic;
} FmtSpan;
/* Like irc_strip_format, but records where colors/bold/underline change. */
int    irc_strip_format_spans(char *s, FmtSpan *spans, int max);

/* Case-insensitive glob match supporting * and ? */
int    glob_imatch(const char *pat, const char *str);

/* UTF-8 helpers */
int    utf8_decode(const char *s, uint32_t *cp);   /* returns bytes consumed (>=1) */
int    utf8_to_utf16(const char *in, uint16_t *out, int max);
int    utf16_to_utf8(const uint16_t *in, char *out, int max);
void   utf8_truncate(char *s, size_t maxbytes);

/* base64 encoding; returns malloc'd string */
char  *base64_encode(const unsigned char *data, size_t len);

uint32_t hash_str(const char *s);

/* "HH:MM" local time */
void   time_hhmm(char out[6]);
/* IRCv3 server-time ("2026-10-03T18:13:00.000Z") -> local "HH:MM". Returns 0 on success. */
int    iso_to_local_hhmm(const char *iso, char out[6]);
uint64_t now_ms(void);

#endif
