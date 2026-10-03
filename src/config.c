#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/rng.h>

#include "config.h"
#include "util.h"
#include "i18n.h"

AppConfig g_cfg;

const char *g_lang_codes[] = { "es", "en", "pt", "fr", "de", "it", "ja", "ru", "zh", "ko" };
const char *g_lang_names[] = { "Español", "English", "Português", "Français", "Deutsch",
                               "Italiano", "Japanese", "Russian", "Chinese", "Korean" };
const int g_lang_count = sizeof(g_lang_codes) / sizeof(g_lang_codes[0]);

int lang_index(const char *code)
{
	for (int i = 0; i < g_lang_count; i++)
		if (!strcmp(g_lang_codes[i], code))
			return i;
	return 0;
}

const char *auth_name(int auth)
{
	switch (auth) {
	case AUTH_NICKSERV: return "NickServ IDENTIFY";
	case AUTH_SASL:     return "SASL PLAIN";
	case AUTH_PASS:     return "Server PASS";
	default:            return T("Automática");
	}
}

void config_server_defaults(ServerCfg *s)
{
	unsigned int r = 0;
	sceKernelGetRandomNumber(&r, sizeof(r));
	memset(s, 0, sizeof(*s));
	strcpy(s->name, "Libera.Chat");
	strcpy(s->host, "irc.libera.chat");
	s->port = 6697;
	s->ssl = 1;
	snprintf(s->nick, sizeof(s->nick), "Vita%04u", r % 10000);
	strcpy(s->realname, "VitaIRC user");
	strcpy(s->channels, "#vitasdk");
	s->auth = AUTH_NONE;
	s->autoconnect = 1;
}

void config_defaults(void)
{
	memset(&g_cfg, 0, sizeof(g_cfg));
	strcpy(g_cfg.openai_model, "gpt-4o-mini");
	strcpy(g_cfg.lang_in, "es");
	strcpy(g_cfg.lang_out, "en");
	g_cfg.show_joins = 1;
	g_cfg.keep_awake = 1;
	g_cfg.cpu_mhz = 333;
	g_cfg.font_pct = 100;
	g_cfg.sound = 1;
	g_cfg.irc_colors = 1;
}

#define SETS(dst, v) str_copy(dst, v, sizeof(dst))

static void apply_general(const char *k, const char *v)
{
	if (!strcmp(k, "openai_key"))        SETS(g_cfg.openai_key, v);
	else if (!strcmp(k, "openai_model")) SETS(g_cfg.openai_model, v);
	else if (!strcmp(k, "lang_in"))      SETS(g_cfg.lang_in, v);
	else if (!strcmp(k, "lang_out"))     SETS(g_cfg.lang_out, v);
	else if (!strcmp(k, "imgur_client_id")) SETS(g_cfg.imgur_id, v);
	else if (!strcmp(k, "show_joins"))   g_cfg.show_joins = atoi(v);
	else if (!strcmp(k, "keep_awake"))   g_cfg.keep_awake = atoi(v);
	else if (!strcmp(k, "cpu_mhz"))      g_cfg.cpu_mhz = atoi(v);
	else if (!strcmp(k, "font_pct"))     g_cfg.font_pct = atoi(v);
	else if (!strcmp(k, "ui_lang"))      g_cfg.ui_lang = atoi(v);
	else if (!strcmp(k, "sound"))        g_cfg.sound = atoi(v);
	else if (!strcmp(k, "irc_colors"))   g_cfg.irc_colors = atoi(v);
	else if (!strcmp(k, "highlight"))    SETS(g_cfg.highlight, v);
	else if (!strcmp(k, "ignore"))       SETS(g_cfg.ignore, v);
}

static void apply_server(ServerCfg *s, const char *k, const char *v)
{
	if (!strcmp(k, "name"))          SETS(s->name, v);
	else if (!strcmp(k, "host"))     SETS(s->host, v);
	else if (!strcmp(k, "port"))     s->port = atoi(v);
	else if (!strcmp(k, "ssl"))      s->ssl = atoi(v);
	else if (!strcmp(k, "nick"))     SETS(s->nick, v);
	else if (!strcmp(k, "user"))     SETS(s->user, v);
	else if (!strcmp(k, "realname")) SETS(s->realname, v);
	else if (!strcmp(k, "password")) SETS(s->password, v);
	else if (!strcmp(k, "auth"))     s->auth = atoi(v);
	else if (!strcmp(k, "channels")) SETS(s->channels, v);
	else if (!strcmp(k, "autoconnect")) s->autoconnect = atoi(v);
}

void config_load(void)
{
	config_defaults();
	sceIoMkdir("ux0:data", 0777);
	sceIoMkdir(DATA_DIR, 0777);

	FILE *f = fopen(CONFIG_PATH, "r");
	if (!f) {
		config_server_defaults(&g_cfg.servers[0]);
		g_cfg.nservers = 1;
		config_save();
		return;
	}

	char line[1024];
	ServerCfg *cur = NULL;
	while (fgets(line, sizeof(line), f)) {
		str_trim(line);
		if (!line[0] || line[0] == ';' || line[0] == '#')
			continue;
		if (line[0] == '[') {
			if (!strncmp(line, "[server]", 8) && g_cfg.nservers < MAX_SERVERS) {
				cur = &g_cfg.servers[g_cfg.nservers++];
				config_server_defaults(cur);
				cur->channels[0] = 0;
			} else {
				cur = NULL;
			}
			continue;
		}
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		char *k = line, *v = eq + 1;
		str_trim(k);
		str_trim(v);
		if (cur)
			apply_server(cur, k, v);
		else
			apply_general(k, v);
	}
	fclose(f);

	if (g_cfg.cpu_mhz != 222 && g_cfg.cpu_mhz != 333 && g_cfg.cpu_mhz != 444)
		g_cfg.cpu_mhz = 333;
	if (g_cfg.font_pct < 70 || g_cfg.font_pct > 150)
		g_cfg.font_pct = 100;
}

int config_save(void)
{
	FILE *f = fopen(CONFIG_PATH, "w");
	if (!f)
		return -1;
	fprintf(f, "; VitaIRC configuration. Can also be edited with VitaShell.\n");
	fprintf(f, "[general]\n");
	fprintf(f, "openai_key=%s\n", g_cfg.openai_key);
	fprintf(f, "openai_model=%s\n", g_cfg.openai_model);
	fprintf(f, "lang_in=%s\n", g_cfg.lang_in);
	fprintf(f, "lang_out=%s\n", g_cfg.lang_out);
	fprintf(f, "imgur_client_id=%s\n", g_cfg.imgur_id);
	fprintf(f, "show_joins=%d\n", g_cfg.show_joins);
	fprintf(f, "keep_awake=%d\n", g_cfg.keep_awake);
	fprintf(f, "cpu_mhz=%d\n", g_cfg.cpu_mhz);
	fprintf(f, "font_pct=%d\n", g_cfg.font_pct);
	fprintf(f, "ui_lang=%d\n", g_cfg.ui_lang);
	fprintf(f, "sound=%d\n", g_cfg.sound);
	fprintf(f, "irc_colors=%d\n", g_cfg.irc_colors);
	fprintf(f, "highlight=%s\n", g_cfg.highlight);
	fprintf(f, "ignore=%s\n", g_cfg.ignore);
	for (int i = 0; i < g_cfg.nservers; i++) {
		ServerCfg *s = &g_cfg.servers[i];
		if (s->deleted)
			continue;
		fprintf(f, "\n[server]\n");
		fprintf(f, "name=%s\nhost=%s\nport=%d\nssl=%d\nnick=%s\nuser=%s\nrealname=%s\n",
		        s->name, s->host, s->port, s->ssl, s->nick, s->user, s->realname);
		fprintf(f, "password=%s\nauth=%d\nchannels=%s\nautoconnect=%d\n",
		        s->password, s->auth, s->channels, s->autoconnect);
	}
	fclose(f);
	return 0;
}
