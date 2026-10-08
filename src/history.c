#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>

#include "history.h"
#include "config.h"
#include "util.h"
#include "i18n.h"

#define LOG_DIR       DATA_DIR "/logs"
#define SESSION_PATH  DATA_DIR "/session.txt"
#define TAIL_BYTES    (48 * 1024)
#define MAX_LOG_BYTES (512 * 1024)

int g_loading_history;
static int restoring;

static void safe_name(const char *in, char *out, int n)
{
	int k = 0;
	for (; *in && k < n - 1; in++) {
		unsigned char c = *in;
		out[k++] = (isalnum(c) || c == '#' || c == '-' || c == '_' || c == '.') ? tolower(c) : '_';
	}
	out[k] = 0;
	if (!k)
		str_copy(out, "_", n);
}

static void log_path(Server *s, Chan *c, char *out, int n)
{
	char srv[64], ch[80];
	safe_name(s->cfg.host[0] ? s->cfg.host : g_cfg.servers[s->idx].host, srv, sizeof(srv));
	safe_name(c->name, ch, sizeof(ch));
	snprintf(out, n, LOG_DIR "/%s/%s.log", srv, ch);
}

static void log_dir(Server *s, char *out, int n)
{
	char srv[64];
	safe_name(s->cfg.host[0] ? s->cfg.host : g_cfg.servers[s->idx].host, srv, sizeof(srv));
	snprintf(out, n, LOG_DIR "/%s", srv);
}

static int loggable(int type)
{
	return type == MT_MSG || type == MT_ACTION || type == MT_NOTICE || type == MT_TOPIC ||
	       type == MT_KICK || type == MT_NICK;
}

void hist_log(Server *s, Chan *c, Msg *m)
{
	if (g_loading_history || c->type == CH_STATUS || !loggable(m->type))
		return;
	if (!c->log) {
		char dir[128], path[256];
		sceIoMkdir(LOG_DIR, 0777);
		log_dir(s, dir, sizeof(dir));
		sceIoMkdir(dir, 0777);
		log_path(s, c, path, sizeof(path));
		c->log = fopen(path, "a");
		if (!c->log)
			return;
	}
	/* one line per message; tabs/newlines inside the text are flattened.
	 * date, time, type, self, nick, text, ms since 1970, msgid */
	char text[1100];
	str_copy(text, m->text, sizeof(text));
	for (char *p = text; *p; p++)
		if (*p == '\t' || *p == '\n' || *p == '\r')
			*p = ' ';
	int ymd = ts_local_ymd(m->ts);
	fprintf(c->log, "%04d-%02d-%02d\t%s\t%d\t%d\t%s\t%s\t%lld\t%s\n", ymd / 10000, ymd / 100 % 100, ymd % 100,
	        m->time, m->type, m->self, m->nick, text, (long long)m->ts, m->msgid ? m->msgid : "");
	fflush(c->log);
}

void hist_close(Chan *c)
{
	if (c->log) {
		fclose(c->log);
		c->log = NULL;
	}
}

/* Keeps log files bounded: rewrites only the newest part when too big. */
static void trim_file(const char *path, long size)
{
	if (size <= MAX_LOG_BYTES)
		return;
	FILE *f = fopen(path, "rb");
	if (!f)
		return;
	long keep = MAX_LOG_BYTES / 2;
	char *buf = malloc(keep);
	if (!buf) {
		fclose(f);
		return;
	}
	fseek(f, size - keep, SEEK_SET);
	long n = fread(buf, 1, keep, f);
	fclose(f);
	char *start = memchr(buf, '\n', n);
	start = start ? start + 1 : buf;
	f = fopen(path, "wb");
	if (f) {
		fwrite(start, 1, n - (start - buf), f);
		fclose(f);
	}
	free(buf);
}

void hist_load(Server *s, Chan *c)
{
	if (c->type == CH_STATUS)
		return;
	char path[256];
	log_path(s, c, path, sizeof(path));
	FILE *f = fopen(path, "rb");
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	long from = size > TAIL_BYTES ? size - TAIL_BYTES : 0;
	fseek(f, from, SEEK_SET);
	char *buf = malloc(size - from + 1);
	if (!buf) {
		fclose(f);
		return;
	}
	long n = fread(buf, 1, size - from, f);
	fclose(f);
	buf[n] = 0;
	trim_file(path, size);

	/* collect line starts, keep the last HIST_LOAD_LINES */
	char *p = buf;
	if (from > 0) {
		char *nl = strchr(p, '\n');
		p = nl ? nl + 1 : p + n;
	}
	char *lines[HIST_LOAD_LINES];
	int count = 0, first = 0;
	while (*p) {
		char *nl = strchr(p, '\n');
		if (nl)
			*nl = 0;
		if (*p) {
			if (count < HIST_LOAD_LINES) {
				lines[count++] = p;
			} else {
				lines[first] = p;
				first = (first + 1) % HIST_LOAD_LINES;
			}
		}
		if (!nl)
			break;
		p = nl + 1;
	}

	char last_date[16] = "", last_time[8] = "";
	int64_t last_ts = 0;
	g_loading_history = 1;
	for (int i = 0; i < count; i++) {
		char *line = lines[(first + i) % HIST_LOAD_LINES];
		char *f_[8];
		int nf = 0;
		f_[nf++] = line;
		for (char *q = line; *q && nf < 8; q++)
			if (*q == '\t') {
				*q = 0;
				f_[nf++] = q + 1;
			}
		if (nf < 6)
			continue;
		/* logs from 1.0 have no timestamp: rebuild it from the date and time */
		int64_t ts = nf > 6 ? strtoll(f_[6], NULL, 10) : 0;
		if (ts <= 0) {
			int Y = 0, M = 0, D = 0, h = 0, mi = 0;
			sscanf(f_[0], "%d-%d-%d", &Y, &M, &D);
			sscanf(f_[1], "%d:%d", &h, &mi);
			ts = local_to_unix_ms(Y * 10000 + M * 100 + D, h, mi);
		}
		g_line_ts = ts;
		str_copy(g_line_msgid, nf > 7 ? f_[7] : "", sizeof(g_line_msgid));
		irc_add_msg(s, c, atoi(f_[2]), f_[4], f_[5], atoi(f_[3]));
		str_copy(last_date, f_[0], sizeof(last_date));
		str_copy(last_time, f_[1], sizeof(last_time));
		last_ts = ts;
	}
	if (count) {
		char sep[96];
		/* "2026-10-03" -> "03/10" */
		snprintf(sep, sizeof(sep), T("--- historial hasta %.2s/%.2s %s ---"),
		         last_date + 8, last_date + 5, last_time);
		g_line_ts = last_ts;
		g_line_msgid[0] = 0;
		irc_add_msg(s, c, MT_INFO, NULL, sep, 0);
	}
	g_line_ts = 0;
	g_line_msgid[0] = 0;
	g_loading_history = 0;
	c->unread = 0;
	c->mention = 0;
	free(buf);
}

/* ------------------------------------------------------------------ */

void session_save(void)
{
	if (restoring)
		return;
	FILE *f = fopen(SESSION_PATH, "w");
	if (!f)
		return;
	for (int i = 0; i < g_nservers; i++) {
		if (g_cfg.servers[i].deleted)
			continue;
		Server *s = &g_servers[i];
		for (int k = 1; k < s->nchans; k++) {
			Chan *c = s->chans[k];
			fprintf(f, "%s\t%d\t%d\t%d\t%s\t%s\t%d\n", g_cfg.servers[i].host, c->type, c->trans_in, c->trans_out,
			        c->name, c->trans_lang, c->muted);
		}
	}
	fclose(f);
}

void session_restore(void)
{
	FILE *f = fopen(SESSION_PATH, "r");
	if (!f)
		return;
	char line[300];
	restoring = 1;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *fld[7];
		int nf = 0;
		fld[nf++] = line;
		for (char *q = line; *q && nf < 7; q++)
			if (*q == '\t') {
				*q = 0;
				fld[nf++] = q + 1;
			}
		if (nf < 5 || !fld[4][0])
			continue;
		int sidx = irc_server_by_host(fld[0]);
		if (sidx < 0)
			continue;
		int type = atoi(fld[1]);
		if (type != CH_CHANNEL && type != CH_QUERY)
			continue;
		Chan *c = irc_get_chan(&g_servers[sidx], fld[4], type);
		c->trans_in = atoi(fld[2]);
		c->trans_out = atoi(fld[3]);
		if (nf > 5)
			str_copy(c->trans_lang, fld[5], sizeof(c->trans_lang));
		if (nf > 6)
			c->muted = atoi(fld[6]);
	}
	restoring = 0;
	fclose(f);
}
