#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <psp2/kernel/threadmgr.h>

#include "irc.h"
#include "net.h"
#include "history.h"
#include "util.h"
#include "i18n.h"

#define CTCP_VERSION "VitaIRC " APP_VERSION " (PlayStation Vita)"

Server          g_servers[MAX_SERVERS];
int             g_nservers;
pthread_mutex_t g_lock;
volatile int    g_dirty = 1;
volatile int    g_app_quit;

int             g_view_sidx = -1;
uint32_t        g_view_uid;
int             g_focus_req;
int             g_focus_sidx;
uint32_t        g_focus_uid;
char            g_toast[200];
uint64_t        g_toast_until;

int             g_invite_pending;
int             g_invite_sidx;
char            g_invite_chan[64];
volatile int    g_beep_req;

#define FLOOD_MAX    10    /* private messages per nick ... */
#define FLOOD_WINDOW 60    /* ... per this many seconds (same as the old daemon) */

static uint32_t g_next_uid = 1;
static char     g_line_time[6];   /* server-time of the line being handled */

#define LOCK()   pthread_mutex_lock(&g_lock)
#define UNLOCK() pthread_mutex_unlock(&g_lock)

void ui_toast(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_toast, sizeof(g_toast), fmt, ap);
	va_end(ap);
	utf8_truncate(g_toast, sizeof(g_toast) - 1);
	g_toast_until = now_ms() + 4000;
	g_dirty = 1;
}

/* ------------------------------------------------------------------ */
/* Channels and messages                                              */
/* ------------------------------------------------------------------ */

static int is_channel_name(const char *n)
{
	return n[0] == '#' || n[0] == '&' || n[0] == '!' || n[0] == '+';
}

static void msg_free(Msg *m)
{
	free(m->text);
	free(m->trans);
	free(m->lay_breaks);
	free(m->spans);
	memset(m, 0, sizeof(*m));
}

Chan *irc_find_chan(Server *s, const char *name)
{
	for (int i = 0; i < s->nchans; i++)
		if (!str_icmp(s->chans[i]->name, name))
			return s->chans[i];
	return NULL;
}

Chan *irc_find_chan_uid(Server *s, uint32_t uid)
{
	for (int i = 0; i < s->nchans; i++)
		if (s->chans[i]->uid == uid)
			return s->chans[i];
	return NULL;
}

Chan *irc_get_chan(Server *s, const char *name, int type)
{
	Chan *c = irc_find_chan(s, name);
	if (c)
		return c;
	if (s->nchans >= MAX_CHANS)
		return s->chans[0];
	c = calloc(1, sizeof(Chan));
	if (!c)
		return s->chans[0];
	c->uid = g_next_uid++;
	c->type = type;
	str_copy(c->name, name, sizeof(c->name));
	s->chans[s->nchans++] = c;
	if (type != CH_STATUS) {
		char saved[6];
		memcpy(saved, g_line_time, sizeof(saved));
		g_line_time[0] = 0;
		hist_load(s, c);
		memcpy(g_line_time, saved, sizeof(saved));
		session_save();
	}
	g_dirty = 1;
	return c;
}

int irc_server_by_host(const char *host)
{
	for (int i = 0; i < g_cfg.nservers; i++)
		if (!g_cfg.servers[i].deleted && !str_icmp(g_cfg.servers[i].host, host))
			return i;
	return -1;
}

void irc_close_chan(Server *s, Chan *c)
{
	if (c->type == CH_STATUS)
		return;
	for (int i = 0; i < s->nchans; i++) {
		if (s->chans[i] != c)
			continue;
		memmove(&s->chans[i], &s->chans[i + 1], (s->nchans - i - 1) * sizeof(Chan *));
		s->nchans--;
		break;
	}
	for (int i = 0; i < MAX_MSGS; i++)
		msg_free(&c->msgs[i]);
	hist_close(c);
	free(c->users);
	free(c);
	session_save();
	g_dirty = 1;
}

Msg *irc_msg_at(Chan *c, int i)
{
	if (i < 0 || i >= c->count)
		return NULL;
	return &c->msgs[(c->head + i) % MAX_MSGS];
}

Msg *irc_find_msg(Chan *c, uint32_t id)
{
	for (int i = c->count - 1; i >= 0; i--) {
		Msg *m = irc_msg_at(c, i);
		if (m->id == id)
			return m;
		if (m->id < id)
			break;
	}
	return NULL;
}

Msg *irc_add_msg(Server *s, Chan *c, int type, const char *nick, const char *text, int self)
{
	int pos;
	if (c->count < MAX_MSGS) {
		pos = (c->head + c->count) % MAX_MSGS;
		c->count++;
	} else {
		pos = c->head;
		msg_free(&c->msgs[pos]);
		c->head = (c->head + 1) % MAX_MSGS;
	}
	Msg *m = &c->msgs[pos];
	memset(m, 0, sizeof(*m));
	m->id = ++c->next_id;
	m->type = type;
	m->self = self;
	if (g_line_time[0])
		memcpy(m->time, g_line_time, sizeof(m->time));
	else
		time_hhmm(m->time);
	str_copy(m->nick, nick ? nick : "", sizeof(m->nick));
	m->text = str_dup(text ? text : "");
	FmtSpan spans[24];
	int ns = irc_strip_format_spans(m->text, spans, 24);
	utf8_truncate(m->text, 1000);
	if (ns > 0 && (m->spans = malloc(ns * sizeof(FmtSpan)))) {
		memcpy(m->spans, spans, ns * sizeof(FmtSpan));
		m->nspans = ns;
	}

	int visible = (g_view_sidx == s->idx && g_view_uid == c->uid);
	int chatty = (type == MT_MSG || type == MT_ACTION || type == MT_NOTICE);
	hist_log(s, c, m);
	if (g_loading_history) {
		if (chatty && !self && c->type != CH_STATUS && irc_is_highlight(s, m->text))
			m->highlight = 1;
	} else if (chatty && !self) {
		if (c->type != CH_STATUS && irc_is_highlight(s, m->text))
			m->highlight = 1;
		if ((m->highlight || c->type == CH_QUERY) && g_cfg.sound)
			g_beep_req = 1;
		if (!visible) {
			c->unread++;
			if (m->highlight || c->type == CH_QUERY) {
				c->mention++;
				if (c->type == CH_QUERY)
					ui_toast(T("PM de %s: %s"), m->nick, m->text);
				else
					ui_toast(T("%s en %s: %s"), m->nick, c->name, m->text);
			}
		}
		if (c->trans_in && !tr_should_skip(m->text)) {
			m->trans_state = TR_PENDING;
			tr_request_in(s->idx, c->uid, m->id, m->text, irc_chan_lang(c));
		}
	}
	g_dirty = 1;
	return m;
}

const char *irc_chan_lang(Chan *c)
{
	return c->trans_lang[0] ? c->trans_lang : g_cfg.lang_in;
}

int irc_is_ignored(const char *nick)
{
	if (!nick || !nick[0] || !g_cfg.ignore[0])
		return 0;
	char list[sizeof(g_cfg.ignore)];
	str_copy(list, g_cfg.ignore, sizeof(list));
	char *save = NULL;
	for (char *t = strtok_r(list, ", ", &save); t; t = strtok_r(NULL, ", ", &save))
		if (glob_imatch(t, nick))
			return 1;
	return 0;
}

int irc_is_highlight(Server *s, const char *text)
{
	const char *me = s->nick[0] ? s->nick : s->cfg.nick;
	if (me[0] && str_icontains_word(text, me))
		return 1;
	if (!g_cfg.highlight[0])
		return 0;
	char list[sizeof(g_cfg.highlight)];
	str_copy(list, g_cfg.highlight, sizeof(list));
	char *save = NULL;
	for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
		str_trim(t);
		if (t[0] && str_icontains_word(text, t))
			return 1;
	}
	return 0;
}

char irc_my_prefix(Server *s, Chan *c)
{
	for (int i = 0; i < c->nusers; i++)
		if (!str_icmp(c->users[i].nick, s->nick))
			return c->users[i].prefix;
	return 0;
}

/* Private-message flood protection: returns 1 when the message must be dropped. */
static int pm_flooding(Server *s, const char *nick)
{
	uint64_t now = now_ms();
	int slot = -1, oldest = 0;
	for (int i = 0; i < 16; i++) {
		if (!str_icmp(s->flood[i].nick, nick)) { slot = i; break; }
		if (s->flood[i].reset_at < s->flood[oldest].reset_at) oldest = i;
	}
	if (slot < 0) {
		slot = oldest;
		str_copy(s->flood[slot].nick, nick, sizeof(s->flood[slot].nick));
		s->flood[slot].count = 0;
		s->flood[slot].reset_at = 0;
	}
	if (now > s->flood[slot].reset_at) {
		s->flood[slot].count = 0;
		s->flood[slot].warned = 0;
		s->flood[slot].reset_at = now + FLOOD_WINDOW * 1000ULL;
	}
	if (++s->flood[slot].count <= FLOOD_MAX)
		return 0;
	if (!s->flood[slot].warned) {
		s->flood[slot].warned = 1;
		char b[160];
		snprintf(b, sizeof(b), T("Flood de %s: ignorando sus privados por un minuto"), nick);
		irc_add_msg(s, s->chans[0], MT_ERROR, NULL, b, 0);
	}
	return 1;
}

static void ignore_list_edit(const char *nick, int add)
{
	char out[sizeof(g_cfg.ignore)] = "";
	char list[sizeof(g_cfg.ignore)];
	str_copy(list, g_cfg.ignore, sizeof(list));
	char *save = NULL;
	for (char *t = strtok_r(list, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
		if (!str_icmp(t, nick))
			continue;
		if (out[0]) strncat(out, ",", sizeof(out) - strlen(out) - 1);
		strncat(out, t, sizeof(out) - strlen(out) - 1);
	}
	if (add) {
		if (out[0]) strncat(out, ",", sizeof(out) - strlen(out) - 1);
		strncat(out, nick, sizeof(out) - strlen(out) - 1);
	}
	str_copy(g_cfg.ignore, out, sizeof(g_cfg.ignore));
	config_save();
}

static Msg *status_msg(Server *s, int type, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return irc_add_msg(s, s->chans[0], type, NULL, buf, 0);
}

/* Window to show replies the user asked for (WHOIS, errors...) */
static Chan *reply_chan(Server *s)
{
	if (g_view_sidx == s->idx) {
		Chan *c = irc_find_chan_uid(s, g_view_uid);
		if (c)
			return c;
	}
	return s->chans[0];
}

/* ------------------------------------------------------------------ */
/* Users                                                              */
/* ------------------------------------------------------------------ */

static int prefix_rank(Server *s, char p)
{
	if (!p)
		return 99;
	char *q = strchr(s->prefixes, p);
	return q ? (int)(q - s->prefixes) : 50;
}

static Server *g_sort_server;
static int user_cmp(const void *a, const void *b)
{
	const ChanUser *x = a, *y = b;
	int d = prefix_rank(g_sort_server, x->prefix) - prefix_rank(g_sort_server, y->prefix);
	return d ? d : str_icmp(x->nick, y->nick);
}

void irc_sort_users(Server *s, Chan *c)
{
	g_sort_server = s;
	qsort(c->users, c->nusers, sizeof(ChanUser), user_cmp);
}

static ChanUser *find_user(Chan *c, const char *nick)
{
	for (int i = 0; i < c->nusers; i++)
		if (!str_icmp(c->users[i].nick, nick))
			return &c->users[i];
	return NULL;
}

static void add_user(Server *s, Chan *c, const char *entry)
{
	char prefix = 0;
	/* multi-prefix: keep the highest-ranked prefix */
	while (*entry && strchr(s->prefixes, *entry)) {
		if (!prefix || prefix_rank(s, *entry) < prefix_rank(s, prefix))
			prefix = *entry;
		entry++;
	}
	char nick[40];
	str_copy(nick, entry, sizeof(nick));
	char *bang = strchr(nick, '!');   /* userhost-in-names */
	if (bang)
		*bang = 0;
	if (!nick[0])
		return;
	ChanUser *u = find_user(c, nick);
	if (u) {
		u->prefix = prefix;
		return;
	}
	if (c->nusers == c->cap_users) {
		int ncap = c->cap_users ? c->cap_users * 2 : 32;
		ChanUser *nu = realloc(c->users, ncap * sizeof(ChanUser));
		if (!nu)
			return;
		c->users = nu;
		c->cap_users = ncap;
	}
	str_copy(c->users[c->nusers].nick, nick, sizeof(c->users[0].nick));
	c->users[c->nusers].prefix = prefix;
	c->nusers++;
}

static int remove_user(Chan *c, const char *nick)
{
	ChanUser *u = find_user(c, nick);
	if (!u)
		return 0;
	int i = u - c->users;
	memmove(&c->users[i], &c->users[i + 1], (c->nusers - i - 1) * sizeof(ChanUser));
	c->nusers--;
	return 1;
}

/* ------------------------------------------------------------------ */
/* Outgoing                                                           */
/* ------------------------------------------------------------------ */

void irc_send_raw(Server *s, const char *fmt, ...)
{
	char buf[600];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(buf) - 3)
		n = sizeof(buf) - 3;
	/* never allow embedded line breaks to inject extra commands */
	for (int i = 0; i < n; i++)
		if (buf[i] == '\r' || buf[i] == '\n')
			buf[i] = ' ';
	buf[n++] = '\r';
	buf[n++] = '\n';
	buf[n] = 0;
	if (s->outq_n >= MAX_OUTQ)
		return;
	s->outq[s->outq_n++] = str_dup(buf);
}

/* Splits long text at UTF-8 boundaries so each PRIVMSG fits in 512 bytes. */
static void send_chunks(Server *s, Chan *c, const char *text, int action)
{
	const int maxc = 380;
	const char *p = text;
	while (*p) {
		int n = strlen(p);
		if (n > maxc) {
			n = maxc;
			while (n > 0 && ((unsigned char)p[n] & 0xC0) == 0x80)
				n--;
			for (int k = n; k > maxc - 60; k--)
				if (p[k] == ' ') { n = k; break; }
		}
		char chunk[400];
		memcpy(chunk, p, n);
		chunk[n] = 0;
		if (action)
			irc_send_raw(s, "PRIVMSG %s :\x01" "ACTION %s\x01", c->name, chunk);
		else
			irc_send_raw(s, "PRIVMSG %s :%s", c->name, chunk);
		irc_add_msg(s, c, action ? MT_ACTION : MT_MSG, s->nick, chunk, 1);
		p += n;
		while (*p == ' ')
			p++;
	}
}

void irc_send_privmsg(int sidx, uint32_t chan_uid, const char *text)
{
	LOCK();
	Server *s = &g_servers[sidx];
	Chan *c = irc_find_chan_uid(s, chan_uid);
	if (c && c->type != CH_STATUS) {
		if (s->state != SS_ONLINE)
			irc_add_msg(s, c, MT_ERROR, NULL, T("No conectado: el mensaje no se envió."), 0);
		else
			send_chunks(s, c, text, 0);
	}
	UNLOCK();
}

static void show_help(Server *s, Chan *c)
{
	static const char *lines[] = {
		"Comandos disponibles:",
		"/join #canal [clave]  /part [motivo]  /close",
		"/msg nick texto  /query nick  /me acción  /notice destino texto",
		"/nick nuevo  /topic [texto]  /whois nick  /list",
		"/connect  /disconnect  /quote COMANDO CRUDO  /clear",
		"/away [motivo]  /back  /ignore [nick]  /unignore nick",
		"/op /deop /voice /devoice nick  /kick nick [motivo]  /ban nick  /kickban nick",
		"Cualquier otro /COMANDO se envía tal cual al servidor.",
	};
	for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
		irc_add_msg(s, c, MT_INFO, NULL, T(lines[i]), 0);
}

void irc_user_input(int sidx, uint32_t chan_uid, const char *input)
{
	char text[1100];
	str_copy(text, input, sizeof(text));
	str_trim(text);
	if (!text[0])
		return;

	LOCK();
	Server *s = &g_servers[sidx];
	Chan *c = irc_find_chan_uid(s, chan_uid);
	if (!c)
		c = s->chans[0];

	if (text[0] != '/' || text[1] == '/') {
		const char *body = text[0] == '/' ? text + 1 : text;
		if (c->type == CH_STATUS) {
			irc_add_msg(s, c, MT_ERROR, NULL, T("Esta es la ventana del servidor. Usa /join #canal o /help."), 0);
		} else if (s->state != SS_ONLINE) {
			irc_add_msg(s, c, MT_ERROR, NULL, T("No conectado: el mensaje no se envió."), 0);
		} else if (c->trans_out && g_cfg.openai_key[0] && !tr_should_skip(body)) {
			tr_request_out(sidx, c->uid, body);
		} else {
			send_chunks(s, c, body, 0);
		}
		UNLOCK();
		return;
	}

	char cmd[32] = {0};
	char *args = text + 1;
	int i = 0;
	while (*args && *args != ' ' && i < 31)
		cmd[i++] = toupper((unsigned char)*args++);
	while (*args == ' ')
		args++;

	if (!strcmp(cmd, "CONNECT")) {
		UNLOCK();
		irc_connect(sidx);
		return;
	}
	if (!strcmp(cmd, "DISCONNECT") || !strcmp(cmd, "QUIT")) {
		UNLOCK();
		irc_disconnect(sidx, args[0] ? args : NULL);
		return;
	}
	if (!strcmp(cmd, "HELP")) {
		show_help(s, c);
		UNLOCK();
		return;
	}
	if (!strcmp(cmd, "CLEAR")) {
		for (int k = 0; k < MAX_MSGS; k++)
			msg_free(&c->msgs[k]);
		c->head = c->count = 0;
		c->scroll = 0;
		g_dirty = 1;
		UNLOCK();
		return;
	}
	if (!strcmp(cmd, "QUERY") && args[0]) {
		char nick[40];
		sscanf(args, "%39s", nick);
		Chan *q = irc_get_chan(s, nick, CH_QUERY);
		g_focus_req = 1; g_focus_sidx = sidx; g_focus_uid = q->uid;
		const char *rest = strchr(args, ' ');
		if (rest && s->state == SS_ONLINE)
			send_chunks(s, q, rest + 1, 0);
		UNLOCK();
		return;
	}
	if (!strcmp(cmd, "IGNORE") || !strcmp(cmd, "UNIGNORE")) {
		char nick[40] = "";
		sscanf(args, "%39s", nick);
		if (nick[0]) {
			ignore_list_edit(nick, cmd[0] == 'I');
			char b[120];
			snprintf(b, sizeof(b), cmd[0] == 'I' ? T("Ignorando a %s") : T("Ya no ignoras a %s"), nick);
			irc_add_msg(s, c, MT_INFO, NULL, b, 0);
		} else {
			char b[600];
			snprintf(b, sizeof(b), T("Ignorados: %s"), g_cfg.ignore[0] ? g_cfg.ignore : T("(ninguno)"));
			irc_add_msg(s, c, MT_INFO, NULL, b, 0);
		}
		UNLOCK();
		return;
	}
	if (!strcmp(cmd, "CLOSE")) {
		if (c->type == CH_CHANNEL && c->joined && s->state == SS_ONLINE)
			irc_send_raw(s, "PART %s :VitaIRC", c->name);
		irc_close_chan(s, c);
		UNLOCK();
		return;
	}

	if (s->state != SS_ONLINE) {
		irc_add_msg(s, c, MT_ERROR, NULL, T("No conectado. Usa /connect."), 0);
		UNLOCK();
		return;
	}

	char who[64] = "";
	sscanf(args, "%63s", who);
	const char *rest = strchr(args, ' ');
	rest = rest ? rest + 1 : "";

	if (!strcmp(cmd, "AWAY")) {
		if (args[0])
			irc_send_raw(s, "AWAY :%s", args);
		else
			irc_send_raw(s, "AWAY");
	} else if (!strcmp(cmd, "BACK")) {
		irc_send_raw(s, "AWAY");
	} else if ((!strcmp(cmd, "OP") || !strcmp(cmd, "DEOP") || !strcmp(cmd, "VOICE") || !strcmp(cmd, "DEVOICE")) && who[0] && c->type == CH_CHANNEL) {
		char sign = cmd[0] == 'D' ? '-' : '+';
		char mode = strstr(cmd, "OP") ? 'o' : 'v';
		irc_send_raw(s, "MODE %s %c%c %s", c->name, sign, mode, who);
	} else if (!strcmp(cmd, "KICK") && who[0] && c->type == CH_CHANNEL) {
		irc_send_raw(s, "KICK %s %s :%s", c->name, who, rest[0] ? rest : "VitaIRC");
	} else if ((!strcmp(cmd, "BAN") || !strcmp(cmd, "KICKBAN")) && who[0] && c->type == CH_CHANNEL) {
		irc_send_raw(s, "MODE %s +b %s!*@*", c->name, who);
		if (!strcmp(cmd, "KICKBAN"))
			irc_send_raw(s, "KICK %s %s :%s", c->name, who, rest[0] ? rest : "VitaIRC");
	} else if (!strcmp(cmd, "JOIN") || !strcmp(cmd, "J")) {
		char name[64];
		if (sscanf(args, "%63s", name) == 1) {
			if (!is_channel_name(name)) {
				memmove(name + 1, name, strlen(name) + 1);
				name[0] = '#';
				name[63] = 0;
			}
			const char *key = strchr(args, ' ');
			irc_send_raw(s, "JOIN %s%s%s", name, key ? " " : "", key ? key + 1 : "");
			str_copy(s->pending_focus, name, sizeof(s->pending_focus));
		}
	} else if (!strcmp(cmd, "PART") || !strcmp(cmd, "LEAVE")) {
		if (c->type == CH_CHANNEL)
			irc_send_raw(s, "PART %s :%s", c->name, args[0] ? args : "VitaIRC");
	} else if (!strcmp(cmd, "ME")) {
		if (c->type != CH_STATUS && args[0])
			send_chunks(s, c, args, 1);
	} else if (!strcmp(cmd, "MSG") || !strcmp(cmd, "NOTICE")) {
		char target[64];
		const char *rest = strchr(args, ' ');
		if (sscanf(args, "%63s", target) == 1 && rest) {
			rest++;
			if (!strcmp(cmd, "NOTICE")) {
				irc_send_raw(s, "NOTICE %s :%s", target, rest);
				irc_add_msg(s, c, MT_NOTICE, s->nick, rest, 1);
			} else {
				Chan *t = is_channel_name(target) ? irc_find_chan(s, target) : irc_get_chan(s, target, CH_QUERY);
				if (t)
					send_chunks(s, t, rest, 0);
				else
					irc_send_raw(s, "PRIVMSG %s :%s", target, rest);
			}
		}
	} else if (!strcmp(cmd, "NICK")) {
		if (args[0])
			irc_send_raw(s, "NICK %s", args);
	} else if (!strcmp(cmd, "TOPIC")) {
		if (c->type == CH_CHANNEL) {
			if (args[0])
				irc_send_raw(s, "TOPIC %s :%s", c->name, args);
			else
				irc_send_raw(s, "TOPIC %s", c->name);
		}
	} else if (!strcmp(cmd, "WHOIS")) {
		if (args[0])
			irc_send_raw(s, "WHOIS %s", args);
	} else if (!strcmp(cmd, "LIST")) {
		free(s->listed);
		s->listed = NULL;
		s->nlisted = 0;
		s->listing = 1;
		irc_send_raw(s, "LIST");
	} else if (!strcmp(cmd, "QUOTE") || !strcmp(cmd, "RAW")) {
		if (args[0])
			irc_send_raw(s, "%s", args);
	} else {
		irc_send_raw(s, "%s%s%s", cmd, args[0] ? " " : "", args);
	}
	UNLOCK();
}

/* ------------------------------------------------------------------ */
/* Incoming                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
	char *nick;
	char *cmd;
	char *p[16];
	int   np;
} Line;

static void parse_line(char *buf, Line *l)
{
	memset(l, 0, sizeof(*l));
	char *p = buf;
	g_line_time[0] = 0;
	if (*p == '@') {                 /* IRCv3 tags: only server-time is used */
		char *end = strchr(p, ' ');
		if (!end) return;
		*end = 0;
		char *t = strstr(p, "time=");
		if (t && (t == p + 1 || t[-1] == ';'))
			iso_to_local_hhmm(t + 5, g_line_time);
		p = end + 1;
		while (*p == ' ') p++;
	}
	if (*p == ':') {
		l->nick = ++p;
		p = strchr(p, ' ');
		if (!p) return;
		*p++ = 0;
		char *bang = strchr(l->nick, '!');
		if (bang) *bang = 0;
		char *at = strchr(l->nick, '@');
		if (at) *at = 0;
		while (*p == ' ') p++;
	}
	l->cmd = p;
	p = strchr(p, ' ');
	if (!p) return;
	*p++ = 0;
	while (*p && l->np < 16) {
		while (*p == ' ') p++;
		if (!*p) break;
		if (*p == ':') {
			l->p[l->np++] = p + 1;
			break;
		}
		l->p[l->np++] = p;
		p = strchr(p, ' ');
		if (!p) break;
		*p++ = 0;
	}
}

static int is_me(Server *s, const char *nick)
{
	return nick && !str_icmp(nick, s->nick);
}

static void do_autojoin(Server *s)
{
	char list[512];
	str_copy(list, s->cfg.channels, sizeof(list));
	char *save = NULL;
	for (char *t = strtok_r(list, ", ", &save); t; t = strtok_r(NULL, ", ", &save))
		irc_send_raw(s, "JOIN %s", t);
	/* re-join channels that were open before a reconnect */
	for (int i = 0; i < s->nchans; i++) {
		Chan *c = s->chans[i];
		if (c->type == CH_CHANNEL && !c->joined && !str_icontains_word(s->cfg.channels, c->name))
			irc_send_raw(s, "JOIN %s", c->name);
	}
}

static void handle_ctcp(Server *s, Line *l, const char *target, char *text)
{
	if (irc_is_ignored(l->nick))
		return;
	text++;
	char *end = strchr(text, '\x01');
	if (end) *end = 0;
	char *arg = strchr(text, ' ');
	if (arg) *arg++ = 0;

	if (!strcmp(text, "ACTION")) {
		int priv = is_me(s, target);
		Chan *c = irc_get_chan(s, priv ? l->nick : target, priv ? CH_QUERY : CH_CHANNEL);
		irc_add_msg(s, c, MT_ACTION, l->nick, arg ? arg : "", 0);
	} else if (!strcmp(text, "VERSION")) {
		irc_send_raw(s, "NOTICE %s :\x01VERSION %s\x01", l->nick, CTCP_VERSION);
	} else if (!strcmp(text, "PING")) {
		irc_send_raw(s, "NOTICE %s :\x01PING %s\x01", l->nick, arg ? arg : "");
	} else if (!strcmp(text, "TIME")) {
		char t[6];
		time_hhmm(t);
		irc_send_raw(s, "NOTICE %s :\x01TIME %s\x01", l->nick, t);
	}
}

static void handle_line(Server *s, char *raw)
{
	Line l;
	parse_line(raw, &l);
	if (!l.cmd || !l.cmd[0])
		return;
	const char *cmd = l.cmd;
	const char *last = l.np ? l.p[l.np - 1] : "";

	if (!strcmp(cmd, "PING")) {
		irc_send_raw(s, "PONG :%s", last);
		return;
	}
	if (!strcmp(cmd, "PONG"))
		return;

	if (!strcmp(cmd, "PRIVMSG") && l.np >= 2 && l.nick) {
		const char *target = l.p[0];
		char *text = l.p[1];
		if (text[0] == '\x01') {
			handle_ctcp(s, &l, target, text);
			return;
		}
		int priv = is_me(s, target);
		if (irc_is_ignored(l.nick) || (priv && pm_flooding(s, l.nick)))
			return;
		Chan *c = irc_get_chan(s, priv ? l.nick : target, priv ? CH_QUERY : CH_CHANNEL);
		irc_add_msg(s, c, MT_MSG, l.nick, text, 0);
		return;
	}

	if (!strcmp(cmd, "NOTICE") && l.np >= 2) {
		const char *target = l.p[0];
		if (l.nick && (irc_is_ignored(l.nick) || (is_me(s, target) && s->state == SS_ONLINE && !strchr(l.nick, '.') && pm_flooding(s, l.nick))))
			return;
		char *text = l.p[1];
		if (text[0] == '\x01')       /* CTCP replies */
			text++;
		Chan *c;
		if (!l.nick || strchr(l.nick, '.') || s->state != SS_ONLINE)
			c = s->chans[0];
		else if (is_channel_name(target))
			c = irc_get_chan(s, target, CH_CHANNEL);
		else if (!(c = irc_find_chan(s, l.nick)))
			c = reply_chan(s);
		char *end = strchr(text, '\x01');
		if (end) *end = 0;
		irc_add_msg(s, c, MT_NOTICE, l.nick ? l.nick : "", text, 0);
		return;
	}

	if (!strcmp(cmd, "JOIN") && l.np >= 1 && l.nick) {
		const char *name = l.p[0];
		Chan *c = irc_get_chan(s, name, CH_CHANNEL);
		if (is_me(s, l.nick)) {
			c->joined = 1;
			c->nusers = 0;
			irc_add_msg(s, c, MT_JOIN, NULL, T("Te uniste al canal"), 0);
			if (!str_icmp(s->pending_focus, name)) {
				s->pending_focus[0] = 0;
				g_focus_req = 1; g_focus_sidx = s->idx; g_focus_uid = c->uid;
			}
		} else {
			add_user(s, c, l.nick);
			irc_sort_users(s, c);
			if (g_cfg.show_joins) {
				char b[128];
				snprintf(b, sizeof(b), T("%s se unió"), l.nick);
				irc_add_msg(s, c, MT_JOIN, NULL, b, 0);
			}
		}
		return;
	}

	if (!strcmp(cmd, "PART") && l.np >= 1 && l.nick) {
		Chan *c = irc_find_chan(s, l.p[0]);
		if (!c) return;
		char b[512];
		if (is_me(s, l.nick)) {
			c->joined = 0;
			c->nusers = 0;
			irc_add_msg(s, c, MT_PART, NULL, T("Saliste del canal"), 0);
		} else {
			remove_user(c, l.nick);
			if (g_cfg.show_joins) {
				snprintf(b, sizeof(b), T("%s salió%s%s"), l.nick, l.np > 1 ? ": " : "", l.np > 1 ? l.p[1] : "");
				irc_add_msg(s, c, MT_PART, NULL, b, 0);
			}
		}
		return;
	}

	if (!strcmp(cmd, "QUIT") && l.nick) {
		char b[512];
		snprintf(b, sizeof(b), T("%s se desconectó%s%s"), l.nick, l.np ? ": " : "", l.np ? l.p[0] : "");
		for (int i = 0; i < s->nchans; i++) {
			Chan *c = s->chans[i];
			int had = remove_user(c, l.nick);
			int is_query = c->type == CH_QUERY && !str_icmp(c->name, l.nick);
			if ((had && g_cfg.show_joins) || is_query)
				irc_add_msg(s, c, MT_QUIT, NULL, b, 0);
		}
		return;
	}

	if (!strcmp(cmd, "KICK") && l.np >= 2) {
		Chan *c = irc_find_chan(s, l.p[0]);
		if (!c) return;
		char b[512];
		snprintf(b, sizeof(b), T("%s fue expulsado por %s: %s"), l.p[1], l.nick ? l.nick : "?", l.np > 2 ? l.p[2] : "");
		if (is_me(s, l.p[1])) {
			c->joined = 0;
			c->nusers = 0;
			irc_add_msg(s, c, MT_ERROR, NULL, b, 0);
			ui_toast(T("Fuiste expulsado de %s"), c->name);
		} else {
			remove_user(c, l.p[1]);
			irc_add_msg(s, c, MT_KICK, NULL, b, 0);
		}
		return;
	}

	if (!strcmp(cmd, "NICK") && l.np >= 1 && l.nick) {
		const char *nn = l.p[0];
		char b[160];
		snprintf(b, sizeof(b), T("%s ahora es %s"), l.nick, nn);
		int me = is_me(s, l.nick);
		if (me)
			str_copy(s->nick, nn, sizeof(s->nick));
		for (int i = 0; i < s->nchans; i++) {
			Chan *c = s->chans[i];
			ChanUser *u = find_user(c, l.nick);
			if (u) {
				str_copy(u->nick, nn, sizeof(u->nick));
				irc_add_msg(s, c, MT_NICK, NULL, b, 0);
			} else if (c->type == CH_QUERY && !str_icmp(c->name, l.nick)) {
				str_copy(c->name, nn, sizeof(c->name));
				irc_add_msg(s, c, MT_NICK, NULL, b, 0);
			}
		}
		if (me)
			irc_add_msg(s, s->chans[0], MT_NICK, NULL, b, 0);
		return;
	}

	if (!strcmp(cmd, "TOPIC") && l.np >= 2) {
		Chan *c = irc_find_chan(s, l.p[0]);
		if (!c) return;
		str_copy(c->topic, l.p[1], sizeof(c->topic));
		irc_strip_format(c->topic);
		char b[520];
		snprintf(b, sizeof(b), T("%s cambió el tema: %s"), l.nick ? l.nick : "?", l.p[1]);
		irc_add_msg(s, c, MT_TOPIC, NULL, b, 0);
		return;
	}

	if (!strcmp(cmd, "MODE") && l.np >= 2) {
		Chan *c = is_channel_name(l.p[0]) ? irc_find_chan(s, l.p[0]) : NULL;
		char b[400] = {0};
		for (int i = 1; i < l.np; i++) {
			strncat(b, l.p[i], sizeof(b) - strlen(b) - 2);
			strcat(b, " ");
		}
		char line[520];
		snprintf(line, sizeof(line), T("%s establece modo %s"), l.nick ? l.nick : "?", b);
		irc_add_msg(s, c ? c : s->chans[0], MT_MODE, NULL, line, 0);
		if (c)
			irc_send_raw(s, "NAMES %s", c->name);   /* refresh prefixes */
		return;
	}

	if (!strcmp(cmd, "CAP") && l.np >= 3) {
		const char *sub = l.p[1];
		if (!strcmp(sub, "LS")) {
			int more = l.np >= 4 && !strcmp(l.p[2], "*");
			size_t n = strlen(s->caps_ls);
			snprintf(s->caps_ls + n, sizeof(s->caps_ls) - n, "%s ", last);
			if (more)
				return;
			/* request what we know how to use */
			static const char *wanted[] = { "server-time", "znc.in/server-time-iso", "multi-prefix", "away-notify", "sasl" };
			char req[200] = "";
			for (unsigned i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
				if (!strcmp(wanted[i], "sasl") && !(s->cfg.auth == AUTH_SASL && s->cfg.password[0]))
					continue;
				size_t wl = strlen(wanted[i]);
				for (const char *q = s->caps_ls; (q = strstr(q, wanted[i])); q += wl) {
					int start = q == s->caps_ls || q[-1] == ' ';
					int stop = q[wl] == ' ' || q[wl] == '=' || q[wl] == 0;
					if (start && stop) {
						if (!strcmp(wanted[i], "znc.in/server-time-iso") && strstr(req, "server-time "))
							break;
						strcat(req, wanted[i]);
						strcat(req, " ");
						break;
					}
				}
			}
			str_trim(req);
			if (req[0])
				irc_send_raw(s, "CAP REQ :%s", req);
			else
				irc_send_raw(s, "CAP END");
		} else if (!strcmp(sub, "ACK")) {
			if (str_icontains_word(last, "sasl") || strstr(last, "sasl")) {
				s->cap_sasl = 1;
				irc_send_raw(s, "AUTHENTICATE PLAIN");
			} else if (s->state != SS_ONLINE) {
				irc_send_raw(s, "CAP END");
			}
		} else if (!strcmp(sub, "NAK")) {
			if (s->state != SS_ONLINE)
				irc_send_raw(s, "CAP END");
		}
		return;
	}
	if (!strcmp(cmd, "AWAY") && l.nick) {
		for (int i = 0; i < s->nchans; i++) {
			ChanUser *u = find_user(s->chans[i], l.nick);
			if (u)
				u->away = l.np > 0 && l.p[0][0];
		}
		g_dirty = 1;
		return;
	}
	if (!strcmp(cmd, "INVITE") && l.np >= 2 && l.nick) {
		if (irc_is_ignored(l.nick))
			return;
		g_invite_pending = 1;
		g_invite_sidx = s->idx;
		str_copy(g_invite_chan, l.p[1], sizeof(g_invite_chan));
		char b[200];
		snprintf(b, sizeof(b), T("%s te invitó a %s. Usa /join %s"), l.nick, l.p[1], l.p[1]);
		irc_add_msg(s, reply_chan(s), MT_INFO, NULL, b, 0);
		ui_toast(T("%s te invitó a %s"), l.nick, l.p[1]);
		return;
	}
	if (!strcmp(cmd, "AUTHENTICATE") && l.np >= 1 && !strcmp(l.p[0], "+")) {
		const char *user = s->cfg.user[0] ? s->cfg.user : s->cfg.nick;
		unsigned char buf[400];
		int n = snprintf((char *)buf, sizeof(buf), "%s%c%s%c%s", user, 0, user, 0, s->cfg.password);
		char *b64 = base64_encode(buf, n);
		if (b64) {
			irc_send_raw(s, "AUTHENTICATE %s", b64);
			free(b64);
		}
		return;
	}

	if (!strcmp(cmd, "ERROR")) {
		status_msg(s, MT_ERROR, T("Servidor: %s"), last);
		return;
	}

	if (!(isdigit((unsigned char)cmd[0]) && strlen(cmd) == 3)) {
		return;
	}

	/* ---------------- numerics ---------------- */
	int num = atoi(cmd);
	switch (num) {
	case 1:
		s->state = SS_ONLINE;
		if (l.np >= 1)
			str_copy(s->nick, l.p[0], sizeof(s->nick));
		status_msg(s, MT_SERVER, T("Conectado a %s como %s"), s->cfg.host, s->nick);
		if (s->cfg.auth == AUTH_NICKSERV && s->cfg.password[0]) {
			const char *user = s->cfg.user[0] ? s->cfg.user : s->cfg.nick;
			irc_send_raw(s, "PRIVMSG NickServ :IDENTIFY %s %s", user, s->cfg.password);
			s->join_at = now_ms() + 2200;
		} else {
			s->join_at = now_ms() + 300;
		}
		ui_toast(T("%s: conectado"), s->cfg.name);
		return;
	case 5:
		for (int i = 1; i < l.np - 1; i++) {
			if (!strncmp(l.p[i], "PREFIX=(", 8)) {
				char *close = strchr(l.p[i], ')');
				if (close)
					str_copy(s->prefixes, close + 1, sizeof(s->prefixes));
			}
		}
		return;
	case 4:
		return;
	case 2: case 3: case 265: case 266: case 375: case 376:
		status_msg(s, MT_SERVER, "%s", last);
		return;
	case 372: {
		const char *t = last;
		if (t[0] == '-' && t[1] == ' ')
			t += 2;
		status_msg(s, MT_SERVER, "%s", t[0] ? t : " ");
		return;
	}
	case 332:
		if (l.np >= 3) {
			Chan *c = irc_get_chan(s, l.p[1], CH_CHANNEL);
			str_copy(c->topic, l.p[2], sizeof(c->topic));
			irc_strip_format(c->topic);
			char b[520];
			snprintf(b, sizeof(b), T("Tema: %s"), l.p[2]);
			irc_add_msg(s, c, MT_TOPIC, NULL, b, 0);
		}
		return;
	case 333:
		return;
	case 353:
		if (l.np >= 4) {
			Chan *c = irc_find_chan(s, l.p[2]);
			if (!c) return;
			if (!c->names_building) {
				c->nusers = 0;
				c->names_building = 1;
			}
			char names[1024];
			str_copy(names, l.p[3], sizeof(names));
			char *save = NULL;
			for (char *t = strtok_r(names, " ", &save); t; t = strtok_r(NULL, " ", &save))
				add_user(s, c, t);
		}
		return;
	case 366:
		if (l.np >= 2) {
			Chan *c = irc_find_chan(s, l.p[1]);
			if (c) {
				c->names_building = 0;
				irc_sort_users(s, c);
				g_dirty = 1;
			}
		}
		return;
	case 321:
		return;
	case 322:
		if (l.np >= 3) {
			if (!s->listed)
				s->listed = calloc(MAX_LISTED, sizeof(ListedChan));
			if (!s->listed)
				return;
			ListedChan *e = NULL;
			int users = atoi(l.p[2]);
			if (s->nlisted < MAX_LISTED) {
				e = &s->listed[s->nlisted++];
			} else {          /* keep the biggest channels */
				int mi = 0;
				for (int i = 1; i < s->nlisted; i++)
					if (s->listed[i].users < s->listed[mi].users) mi = i;
				if (s->listed[mi].users < users)
					e = &s->listed[mi];
			}
			if (e) {
				str_copy(e->name, l.p[1], sizeof(e->name));
				e->users = users;
				str_copy(e->topic, l.np >= 4 ? l.p[3] : "", sizeof(e->topic));
				irc_strip_format(e->topic);
			}
			s->listing = 1;
			g_dirty = 1;
		}
		return;
	case 323:
		s->listing = 2;
		g_dirty = 1;
		return;
	case 433:
		if (s->state != SS_ONLINE) {
			size_t n = strlen(s->nick);
			if (n < sizeof(s->nick) - 1) {
				s->nick[n] = '_';
				s->nick[n + 1] = 0;
			}
			irc_send_raw(s, "NICK %s", s->nick);
			status_msg(s, MT_ERROR, T("Nick en uso, probando %s"), s->nick);
		} else {
			irc_add_msg(s, reply_chan(s), MT_ERROR, NULL, T("Ese nick ya está en uso"), 0);
		}
		return;
	case 900:
		status_msg(s, MT_SERVER, "%s", last);
		return;
	case 305:
		s->away = 0;
		irc_add_msg(s, reply_chan(s), MT_SERVER, NULL, T("Ya no estás ausente"), 0);
		return;
	case 306:
		s->away = 1;
		irc_add_msg(s, reply_chan(s), MT_SERVER, NULL, T("Ahora estás marcado como ausente"), 0);
		return;
	case 903:
		status_msg(s, MT_SERVER, T("Autenticación SASL correcta"));
		irc_send_raw(s, "CAP END");
		return;
	case 902: case 904: case 905: case 906: case 907:
		status_msg(s, MT_ERROR, T("SASL falló: %s"), last);
		irc_send_raw(s, "CAP END");
		return;
	default:
		break;
	}

	/* everything else: show the human-readable part */
	char b[800] = {0};
	for (int i = 1; i < l.np; i++) {
		strncat(b, l.p[i], sizeof(b) - strlen(b) - 2);
		if (i < l.np - 1)
			strcat(b, " ");
	}
	int to_view = (num >= 300 && num < 400 && num != 353) || (num >= 400 && num < 600);
	Chan *c = to_view ? reply_chan(s) : s->chans[0];
	irc_add_msg(s, c, num >= 400 ? MT_ERROR : MT_SERVER, NULL, b, 0);
}

/* ------------------------------------------------------------------ */
/* Connection thread                                                  */
/* ------------------------------------------------------------------ */

static int flush_outq(Server *s)
{
	char *q[MAX_OUTQ];
	int n;
	LOCK();
	n = s->outq_n;
	memcpy(q, s->outq, n * sizeof(char *));
	s->outq_n = 0;
	UNLOCK();
	int ok = 0;
	for (int i = 0; i < n; i++) {
		if (ok == 0 && conn_write(&s->conn, q[i], strlen(q[i])) < 0)
			ok = -1;
		free(q[i]);
	}
	return ok;
}

static void wait_or_cancel(Server *s, int seconds)
{
	uint64_t until = now_ms() + seconds * 1000ULL;
	while (now_ms() < until && s->want_connect && !g_app_quit)
		sceKernelDelayThread(200 * 1000);
}

static void *server_thread(void *arg)
{
	Server *s = arg;
	static const int backoff_s[] = { 5, 10, 20, 30, 60 };
	int attempt = 0;
	char line[2048];

	while (!g_app_quit) {
		if (!s->want_connect) {
			s->state = SS_OFF;
			sceKernelDelayThread(250 * 1000);
			continue;
		}

		LOCK();
		ServerCfg cfg = s->cfg;
		s->state = SS_CONNECTING;
		status_msg(s, MT_INFO, T("Conectando a %s:%d%s..."), cfg.host, cfg.port, cfg.ssl ? " (SSL)" : "");
		UNLOCK();

		char err[160] = {0};
		if (conn_open(&s->conn, cfg.host, cfg.port, cfg.ssl, err, sizeof(err)) != 0) {
			int wait = backoff_s[attempt < 4 ? attempt : 4];
			attempt++;
			LOCK();
			s->state = SS_WAITING;
			status_msg(s, MT_ERROR, T("%s. Reintento en %d s"), err, wait);
			UNLOCK();
			wait_or_cancel(s, wait);
			continue;
		}
		attempt = 0;

		LOCK();
		s->state = SS_REGISTERING;
		str_copy(s->nick, cfg.nick, sizeof(s->nick));
		strcpy(s->prefixes, "~&@%+");
		s->join_at = 0;
		s->probe = 0;
		if (cfg.ssl && !s->conn.cert_ok)
			status_msg(s, MT_INFO, T("Aviso: el certificado SSL del servidor no se pudo verificar"));
		/* drop anything queued while offline */
		for (int i = 0; i < s->outq_n; i++)
			free(s->outq[i]);
		s->outq_n = 0;
		const char *user = cfg.user[0] ? cfg.user : cfg.nick;
		s->caps_ls[0] = 0;
		s->cap_sasl = 0;
		irc_send_raw(s, "CAP LS 302");
		if (cfg.auth == AUTH_PASS && cfg.password[0])
			irc_send_raw(s, "PASS %s", cfg.password);
		irc_send_raw(s, "NICK %s", cfg.nick);
		char ident[16] = {0};
		int k = 0;
		for (const char *p = user; *p && k < 10; p++)
			if (isalnum((unsigned char)*p))
				ident[k++] = tolower((unsigned char)*p);
		irc_send_raw(s, "USER %s 0 * :%s", k ? ident : "vita", cfg.realname[0] ? cfg.realname : "VitaIRC");
		UNLOCK();

		uint64_t last_rx = now_ms();
		int ping_sent = 0;
		while (!g_app_quit && s->want_connect) {
			if (flush_outq(s) < 0)
				break;
			uint64_t now = now_ms();
			if (s->join_at && now >= s->join_at) {
				LOCK();
				s->join_at = 0;
				do_autojoin(s);
				UNLOCK();
				continue;
			}
			if (s->probe) {
				s->probe = 0;
				LOCK();
				irc_send_raw(s, "PING :probe");
				UNLOCK();
				last_rx = now - 225000;   /* expect an answer within ~15 s */
				ping_sent = 1;
				continue;
			}
			int n = conn_readline(&s->conn, line, sizeof(line), 100);
			if (n == -1)
				break;
			if (n >= 0) {
				last_rx = now_ms();
				ping_sent = 0;
				LOCK();
				handle_line(s, line);
				UNLOCK();
				continue;
			}
			if (now - last_rx > 120000 && !ping_sent) {
				LOCK();
				irc_send_raw(s, "PING :vitairc");
				UNLOCK();
				ping_sent = 1;
			}
			if (now - last_rx > 240000)
				break;
		}
		flush_outq(s);          /* QUIT, if one was queued */
		conn_close(&s->conn);

		LOCK();
		s->state = s->want_connect ? SS_WAITING : SS_OFF;
		s->join_at = 0;
		for (int i = 0; i < s->nchans; i++) {
			s->chans[i]->joined = 0;
			s->chans[i]->nusers = 0;
		}
		status_msg(s, MT_ERROR, T("Desconectado de %s"), cfg.host);
		UNLOCK();
		if (s->want_connect)
			wait_or_cancel(s, 5);
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Public control                                                     */
/* ------------------------------------------------------------------ */

void irc_init(void)
{
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&g_lock, &attr);
	pthread_mutexattr_destroy(&attr);
	memset(g_servers, 0, sizeof(g_servers));
	for (int i = 0; i < MAX_SERVERS; i++) {
		g_servers[i].idx = i;
		g_servers[i].conn.fd = -1;
	}
	g_nservers = 0;
}

void irc_sync_from_config(void)
{
	LOCK();
	for (int i = 0; i < g_cfg.nservers; i++) {
		Server *s = &g_servers[i];
		if (!s->nchans) {
			Chan *st = calloc(1, sizeof(Chan));
			st->uid = g_next_uid++;
			st->type = CH_STATUS;
			s->chans[s->nchans++] = st;
		}
		/* the live connection keeps its snapshot until reconnect */
		if (s->state == SS_OFF || s->state == SS_WAITING)
			s->cfg = g_cfg.servers[i];
		str_copy(s->chans[0]->name, g_cfg.servers[i].name, sizeof(s->chans[0]->name));
	}
	g_nservers = g_cfg.nservers;
	g_dirty = 1;
	UNLOCK();
}

void irc_connect(int sidx)
{
	Server *s = &g_servers[sidx];
	LOCK();
	if (s->state == SS_OFF)
		s->cfg = g_cfg.servers[sidx];
	s->want_connect = 1;
	if (!s->thread_started) {
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 256 * 1024);
		if (pthread_create(&s->thread, &attr, server_thread, s) == 0)
			s->thread_started = 1;
		pthread_attr_destroy(&attr);
	}
	UNLOCK();
}

void irc_disconnect(int sidx, const char *reason)
{
	Server *s = &g_servers[sidx];
	LOCK();
	if (s->state == SS_ONLINE || s->state == SS_REGISTERING)
		irc_send_raw(s, "QUIT :%s", reason ? reason : "VitaIRC " APP_VERSION " - PlayStation Vita");
	s->want_connect = 0;
	UNLOCK();
}

void irc_quit_all(void)
{
	for (int i = 0; i < g_nservers; i++)
		if (g_servers[i].want_connect)
			irc_disconnect(i, NULL);
}

void irc_resume_check(void)
{
	LOCK();
	for (int i = 0; i < g_nservers; i++)
		if (g_servers[i].state == SS_ONLINE || g_servers[i].state == SS_REGISTERING)
			g_servers[i].probe = 1;
	UNLOCK();
}
