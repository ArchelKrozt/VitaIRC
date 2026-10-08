#ifndef VITAIRC_CONFIG_H
#define VITAIRC_CONFIG_H

#define APP_VERSION   "1.1"
#define DATA_DIR      "ux0:data/VitaIRC"
#define CONFIG_PATH   DATA_DIR "/config.ini"
#define CA_PATH       "app0:cacert.pem"

#define MAX_SERVERS   8

/* GitHub repository checked for new releases ("owner/repo"). */
#define UPDATE_REPO   "ArchelKrozt/VitaIRC"

/* Image hosting services */
enum { IMG_LITTERBOX = 0, IMG_IMGBB, IMG_FILEHOST, IMG_IMGUR, IMG_COUNT };

/* Translation services */
enum { TRP_GOOGLE = 0, TRP_OPENAI, TRP_COUNT };

typedef enum {
	AUTH_NONE = 0,
	AUTH_NICKSERV,
	AUTH_SASL,
	AUTH_PASS,
	AUTH_COUNT
} AuthMode;

typedef struct {
	char name[48];
	char host[128];
	int  port;
	int  ssl;
	char nick[32];
	char user[48];       /* account / auth username (defaults to nick) */
	char realname[64];
	char password[128];
	int  auth;           /* AuthMode */
	char channels[512];  /* comma separated, joined after connect */
	int  autoconnect;
	int  deleted;        /* slot freed in this session; dropped on save */
} ServerCfg;

typedef struct {
	ServerCfg servers[MAX_SERVERS];
	int  nservers;

	/* Translation */
	int  tr_provider;    /* TRP_* */
	char openai_key[256];
	char openai_model[48];
	char lang_in[8];     /* translate incoming messages to this language */
	char lang_out[8];    /* translate my messages to this language */

	/* Images */
	int  img_host;       /* IMG_* */
	char imgur_id[64];
	char imgbb_key[80];
	int  imgbb_expire;   /* index into the expiration choices, 0 = never */
	int  litter_time;    /* 0 = 1h, 1 = 12h, 2 = 24h, 3 = 72h */

	/* Updates */
	int  update_check;   /* look for a new release once a day */
	char update_repo[80];
	char update_last[12];/* date of the last automatic check */

	/* misc */
	int  show_joins;
	int  keep_awake;
	int  cpu_mhz;
	int  font_pct;
	int  ui_lang;        /* 0 = system, 1 = Spanish, 2 = English */
	int  sound;          /* beep on PMs and mentions */
	int  irc_colors;     /* render mIRC colors/bold instead of plain text */
	char highlight[256]; /* extra words that count as a mention (comma separated) */
	char ignore[512];    /* ignored nicks, wildcards allowed (comma separated) */
} AppConfig;

extern AppConfig g_cfg;

void config_defaults(void);
void config_server_defaults(ServerCfg *s);
void config_load(void);
int  config_save(void);

const char *auth_name(int auth);

/* Supported translation languages (code, display name) */
extern const char *g_lang_codes[];
extern const char *g_lang_names[];
extern const int   g_lang_count;
int lang_index(const char *code);

#endif
