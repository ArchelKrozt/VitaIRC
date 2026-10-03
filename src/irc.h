#ifndef VITAIRC_IRC_H
#define VITAIRC_IRC_H

#include <stdint.h>
#include <stdio.h>
#include <pthread.h>

#include "config.h"
#include "conn.h"
#include "util.h"

#define MAX_CHANS     48
#define MAX_MSGS      300
#define MAX_OUTQ      64
#define MAX_LISTED    600

enum {
	MT_MSG = 0, MT_ACTION, MT_NOTICE, MT_JOIN, MT_PART, MT_QUIT, MT_KICK,
	MT_TOPIC, MT_SERVER, MT_ERROR, MT_INFO, MT_NICK, MT_MODE
};

enum { CH_STATUS = 0, CH_CHANNEL, CH_QUERY };

enum { TR_NONE = 0, TR_PENDING, TR_DONE, TR_SKIP };

enum { SS_OFF = 0, SS_CONNECTING, SS_REGISTERING, SS_ONLINE, SS_WAITING };

typedef struct {
	uint32_t id;
	uint8_t  type;
	uint8_t  self;
	uint8_t  highlight;
	uint8_t  trans_state;
	char     time[6];
	char     nick[32];
	char    *text;
	char    *trans;
	FmtSpan *spans;             /* mIRC formatting, NULL when plain */
	uint8_t  nspans;
	/* layout cache (owned by the UI) */
	int      lay_w;
	int      lay_lines;
	uint16_t *lay_breaks;   /* byte offsets of wrapped line starts (text then translation) */
	int      lay_text_lines;
} Msg;

typedef struct {
	char nick[40];
	char prefix;            /* '@', '+', '%', '~', '&' or 0 */
	char away;
} ChanUser;

typedef struct {
	uint32_t uid;
	int      type;
	char     name[64];
	char     topic[400];
	int      joined;
	Msg      msgs[MAX_MSGS];
	int      head, count;
	uint32_t next_id;
	int      unread;
	int      mention;
	ChanUser *users;
	int      nusers, cap_users;
	int      names_building;
	int      trans_in;
	int      trans_out;
	int      scroll;            /* lines scrolled up from the bottom */
	uint32_t ui_seen_id;        /* newest message the UI has laid out */
	FILE    *log;               /* history file, opened on first write */
	char     trans_lang[8];     /* per-channel translation language ("" = global) */
} Chan;

typedef struct {
	char name[64];
	int  users;
	char topic[160];
} ListedChan;

typedef struct Server {
	int        idx;
	ServerCfg  cfg;             /* snapshot used for the connection */
	int        state;
	int        want_connect;
	char       nick[32];
	char       prefixes[8];     /* from ISUPPORT PREFIX, e.g. "~&@%+" */
	Chan      *chans[MAX_CHANS];
	int        nchans;
	char      *outq[MAX_OUTQ];
	int        outq_n;
	int        cert_warned;
	uint64_t   join_at;         /* delayed auto-join (after NickServ) */
	uint64_t   wake_at;
	int        sasl_in_progress;
	int        probe;           /* after resume: verify the link is still alive */
	char       pending_focus[64];
	char       caps_ls[1024];   /* IRCv3 capabilities offered by the server */
	int        cap_sasl;        /* SASL was acknowledged */
	int        away;
	struct { char nick[32]; int count; uint64_t reset_at; int warned; } flood[16];
	/* /LIST results */
	ListedChan *listed;
	int        nlisted;
	int        listing;         /* 1 = loading, 2 = done */
	pthread_t  thread;
	int        thread_started;
	Conn       conn;
} Server;

extern Server          g_servers[MAX_SERVERS];
extern int             g_nservers;
extern pthread_mutex_t g_lock;
extern volatile int    g_dirty;
extern volatile int    g_app_quit;

/* Window currently shown by the UI (written by the UI under g_lock) */
extern int             g_view_sidx;
extern uint32_t        g_view_uid;

/* Request from the network side to show a window (e.g. after /join) */
extern int             g_focus_req;
extern int             g_focus_sidx;
extern uint32_t        g_focus_uid;

/* Notification for the UI (PM / mention on a non-visible window) */
extern char            g_toast[200];
extern uint64_t        g_toast_until;
void   ui_toast(const char *fmt, ...);

/* Invitation waiting for the user's answer */
extern int             g_invite_pending;
extern int             g_invite_sidx;
extern char            g_invite_chan[64];
/* Set when a PM or mention arrives (the UI plays a short sound) */
extern volatile int    g_beep_req;

int    irc_is_ignored(const char *nick);
int    irc_is_highlight(Server *s, const char *text);
const char *irc_chan_lang(Chan *c);
/* My prefix in a channel ('@', '+', ... or 0) */
char   irc_my_prefix(Server *s, Chan *c);

void   irc_init(void);
/* Rebuilds runtime servers from g_cfg (keeps connections whose index persists). */
void   irc_sync_from_config(void);
void   irc_connect(int sidx);
void   irc_disconnect(int sidx, const char *reason);
void   irc_quit_all(void);
void   irc_resume_check(void);

/* Must hold g_lock for the functions below */
Chan  *irc_find_chan(Server *s, const char *name);
Chan  *irc_get_chan(Server *s, const char *name, int type);
Chan  *irc_find_chan_uid(Server *s, uint32_t uid);
void   irc_close_chan(Server *s, Chan *c);
Msg   *irc_add_msg(Server *s, Chan *c, int type, const char *nick, const char *text, int self);
Msg   *irc_msg_at(Chan *c, int i);   /* 0 = oldest */
Msg   *irc_find_msg(Chan *c, uint32_t id);
void   irc_send_raw(Server *s, const char *fmt, ...);
void   irc_sort_users(Server *s, Chan *c);
int    irc_server_by_host(const char *host);

/* Handles user input typed into a window (commands or plain text). Takes g_lock. */
void   irc_user_input(int sidx, uint32_t chan_uid, const char *text);
/* Sends a PRIVMSG and echoes it locally. Takes g_lock. */
void   irc_send_privmsg(int sidx, uint32_t chan_uid, const char *text);

#endif
