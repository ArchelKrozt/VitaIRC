/*
 * VitaIRC - IRC client for PlayStation Vita
 * Native, standalone IRC client with ChatGPT translation and Imgur uploads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/system_param.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/appmgr.h>
#include <psp2/rtc.h>
#include <psp2/rtc.h>
#include <vita2d.h>
#include <psp2/display.h>

#include "config.h"
#include "irc.h"
#include "net.h"
#include "conn.h"
#include "util.h"
#include "i18n.h"
#include "camera.h"
#include "sound.h"
#include "history.h"
#include "i18n.h"

#define SCR_W 960
#define SCR_H 544
#define TOP_H 34
#define BOT_H 34
#define SIDE_W 214

/* ------------------------------------------------------------------ */
/* Theme                                                              */
/* ------------------------------------------------------------------ */
#define C_BG        RGBA8(0x0b, 0x0b, 0x10, 0xff)
#define C_PANEL     RGBA8(0x13, 0x13, 0x1c, 0xff)
#define C_SIDE      RGBA8(0x0e, 0x0e, 0x15, 0xff)
#define C_LINE      RGBA8(0x24, 0x24, 0x32, 0xff)
#define C_SEL       RGBA8(0x1d, 0x1d, 0x2b, 0xff)
#define C_ACCENT    RGBA8(0xf5, 0xe6, 0x42, 0xff)
#define C_TEXT      RGBA8(0xd6, 0xd6, 0xe0, 0xff)
#define C_DIM       RGBA8(0x70, 0x70, 0x84, 0xff)
#define C_FAINT     RGBA8(0x48, 0x48, 0x58, 0xff)
#define C_CHAN      RGBA8(0x7a, 0xaa, 0xde, 0xff)
#define C_PM        RGBA8(0xe8, 0x97, 0x4c, 0xff)
#define C_ERR       RGBA8(0xff, 0x5a, 0x6a, 0xff)
#define C_OK        RGBA8(0x5f, 0xc8, 0x7f, 0xff)
#define C_TRANS     RGBA8(0x63, 0xc5, 0xda, 0xff)
#define C_NOTICE    RGBA8(0xc0, 0x8a, 0xe8, 0xff)
#define C_HL_BG     RGBA8(0x3a, 0x30, 0x0c, 0xff)
#define C_OVERLAY   RGBA8(0x00, 0x00, 0x00, 0xb0)

static const unsigned int nick_colors[] = {
	RGBA8(0xff, 0x7b, 0x7b, 0xff), RGBA8(0x7b, 0xd8, 0x8f, 0xff), RGBA8(0x7b, 0xb4, 0xff, 0xff),
	RGBA8(0xff, 0xc8, 0x6b, 0xff), RGBA8(0xd0, 0x8b, 0xff, 0xff), RGBA8(0x6b, 0xe0, 0xd8, 0xff),
	RGBA8(0xff, 0x9b, 0xd2, 0xff), RGBA8(0xb8, 0xe0, 0x6b, 0xff), RGBA8(0xff, 0xa0, 0x5a, 0xff),
	RGBA8(0x9a, 0xa8, 0xff, 0xff), RGBA8(0x5a, 0xd0, 0xa0, 0xff), RGBA8(0xe8, 0xd0, 0x70, 0xff),
};

static unsigned int nick_color(const char *n)
{
	char low[40];
	int i = 0;
	for (; n[i] && i < 39; i++)
		low[i] = (n[i] >= 'A' && n[i] <= 'Z') ? n[i] + 32 : n[i];
	low[i] = 0;
	return nick_colors[hash_str(low) % (sizeof(nick_colors) / sizeof(nick_colors[0]))];
}

/* ------------------------------------------------------------------ */
/* Text                                                               */
/* ------------------------------------------------------------------ */
static vita2d_pgf *font;
static float fscale = 0.9f;
static int   lh = 21;      /* line height */
static int   asc = 16;     /* baseline offset from line top */
static float cw_cache[0x3000];

static void font_setup(void)
{
	fscale = 0.9f * g_cfg.font_pct / 100.0f;
	int h = vita2d_pgf_text_height(font, fscale, "ÁMgjy|");
	if (h < 10)
		h = (int)(18 * fscale);
	lh = h + 5;
	asc = (int)(h * 0.78f) + 2;
	memset(cw_cache, 0, sizeof(cw_cache));
}

static float char_width(uint32_t cp)
{
	if (cp < 0x3000 && cw_cache[cp] > 0)
		return cw_cache[cp];
	float w;
	if (cp == ' ') {
		w = vita2d_pgf_text_width(font, fscale, "a a") - vita2d_pgf_text_width(font, fscale, "aa");
		if (w <= 0)
			w = 5 * fscale;
	} else {
		char b[5] = {0};
		if (cp < 0x80) b[0] = cp;
		else if (cp < 0x800) { b[0] = 0xC0 | (cp >> 6); b[1] = 0x80 | (cp & 0x3F); }
		else if (cp < 0x10000) { b[0] = 0xE0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 0x3F); b[2] = 0x80 | (cp & 0x3F); }
		else { b[0] = 0xF0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 0x3F); b[2] = 0x80 | ((cp >> 6) & 0x3F); b[3] = 0x80 | (cp & 0x3F); }
		w = vita2d_pgf_text_width(font, fscale, b);
		if (w <= 0)
			w = 8 * fscale;
	}
	if (cp < 0x3000)
		cw_cache[cp] = w;
	return w;
}

static int text_w(const char *s)
{
	float w = 0;
	while (*s) {
		uint32_t cp;
		s += utf8_decode(s, &cp);
		w += char_width(cp);
	}
	return (int)(w + 0.5f);
}

static void draw_text(int x, int top, unsigned int color, const char *s)
{
	vita2d_pgf_draw_text(font, x, top + asc, color, fscale, s);
}

/* Draws text cut to maxw pixels, adding "..." when truncated */
static void draw_text_fit(int x, int top, int maxw, unsigned int color, const char *s)
{
	if (text_w(s) <= maxw) {
		draw_text(x, top, color, s);
		return;
	}
	char buf[512];
	int dots = text_w("...");
	float w = 0;
	int n = 0;
	const char *p = s;
	while (*p && n < (int)sizeof(buf) - 8) {
		uint32_t cp;
		int len = utf8_decode(p, &cp);
		float cw = char_width(cp);
		if (w + cw > maxw - dots)
			break;
		memcpy(buf + n, p, len);
		n += len;
		p += len;
		w += cw;
	}
	strcpy(buf + n, "...");
	draw_text(x, top, color, buf);
}

static void draw_rect_outline(int x, int y, int w, int h, unsigned int c)
{
	vita2d_draw_rectangle(x, y, w, 1, c);
	vita2d_draw_rectangle(x, y + h - 1, w, 1, c);
	vita2d_draw_rectangle(x, y, 1, h, c);
	vita2d_draw_rectangle(x + w - 1, y, 1, h, c);
}

/* PlayStation button glyphs drawn with primitives (the system font lacks them) */
enum { G_CROSS, G_CIRCLE, G_SQUARE, G_TRIANGLE, G_L, G_R, G_START, G_DPAD };

static int draw_glyph(int g, int x, int top)
{
	int s = lh - 2;
	int cx = x + s / 2, cy = top + lh / 2;
	unsigned int col;
	switch (g) {
	case G_CROSS:
		col = RGBA8(0x7c, 0xb2, 0xe8, 0xff);
		for (int t = -1; t <= 0; t++) {
			vita2d_draw_line(cx - s / 3 + t, cy - s / 3, cx + s / 3 + t, cy + s / 3, col);
			vita2d_draw_line(cx + s / 3 + t, cy - s / 3, cx - s / 3 + t, cy + s / 3, col);
		}
		return s + 4;
	case G_CIRCLE:
		col = RGBA8(0xff, 0x66, 0x66, 0xff);
		vita2d_draw_fill_circle(cx, cy, s / 2.6f, col);
		vita2d_draw_fill_circle(cx, cy, s / 2.6f - 2, C_PANEL);
		return s + 4;
	case G_SQUARE:
		col = RGBA8(0xe8, 0x8c, 0xd8, 0xff);
		draw_rect_outline(cx - s / 3, cy - s / 3, 2 * s / 3, 2 * s / 3, col);
		draw_rect_outline(cx - s / 3 + 1, cy - s / 3 + 1, 2 * s / 3 - 2, 2 * s / 3 - 2, col);
		return s + 4;
	case G_TRIANGLE:
		col = RGBA8(0x5f, 0xd8, 0xb0, 0xff);
		for (int t = 0; t < 2; t++) {
			vita2d_draw_line(cx, cy - s / 3 + t, cx - s / 3 + t, cy + s / 4, col);
			vita2d_draw_line(cx, cy - s / 3 + t, cx + s / 3 - t, cy + s / 4, col);
			vita2d_draw_line(cx - s / 3, cy + s / 4 - t, cx + s / 3, cy + s / 4 - t, col);
		}
		return s + 4;
	default: {
		const char *t = g == G_L ? "L" : g == G_R ? "R" : g == G_START ? "START" : "+";
		int w = text_w(t) + 8;
		draw_rect_outline(x, top + 3, w, lh - 6, C_DIM);
		vita2d_pgf_draw_text(font, x + 4, top + asc - 1, C_DIM, fscale * 0.85f, t);
		return w + 4;
	}
	}
}

static int draw_hint(int x, int top, int glyph, const char *label)
{
	x += draw_glyph(glyph, x, top);
	draw_text(x, top, C_DIM, label);
	return x + text_w(label) + 14;
}

/* ------------------------------------------------------------------ */
/* Input                                                              */
/* ------------------------------------------------------------------ */
static unsigned int btn_ok = SCE_CTRL_CROSS, btn_back = SCE_CTRL_CIRCLE;
static int glyph_ok = G_CROSS, glyph_back = G_CIRCLE;
static unsigned int pad_now, pad_prev, pad_pressed, pad_repeat;
static uint64_t pad_hold_since, pad_last_repeat;

static void input_read(void)
{
	SceCtrlData pad;
	sceCtrlPeekBufferPositive(0, &pad, 1);
	pad_prev = pad_now;
	pad_now = pad.buttons;
	/* left stick as d-pad */
	if (pad.ly < 40) pad_now |= SCE_CTRL_UP;
	if (pad.ly > 215) pad_now |= SCE_CTRL_DOWN;
	pad_pressed = pad_now & ~pad_prev;
	pad_repeat = pad_pressed;
	unsigned int dirs = SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT |
	                    SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
	uint64_t now = now_ms();
	if (pad_pressed & dirs)
		pad_hold_since = now;
	if ((pad_now & dirs) && now - pad_hold_since > 350 && now - pad_last_repeat > 70) {
		pad_repeat |= pad_now & dirs;
		pad_last_repeat = now;
	}
}

#define PRESSED(b)  (pad_pressed & (b))
#define REPEAT(b)   (pad_repeat & (b))

/* touch */
static int touch_down, touch_was_down, touch_x, touch_y, touch_sx, touch_sy, touch_moved;
static int touch_tap, touch_tap_x, touch_tap_y, touch_dy;
static int touch_last_y;

static void touch_read(void)
{
	SceTouchData t;
	sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1);
	touch_was_down = touch_down;
	touch_down = t.reportNum > 0;
	touch_tap = 0;
	touch_dy = 0;
	if (touch_down) {
		touch_x = t.report[0].x / 2;
		touch_y = t.report[0].y / 2;
		if (!touch_was_down) {
			touch_sx = touch_x;
			touch_sy = touch_y;
			touch_last_y = touch_y;
			touch_moved = 0;
		} else {
			touch_dy = touch_y - touch_last_y;
			touch_last_y = touch_y;
		}
		if (abs(touch_x - touch_sx) > 12 || abs(touch_y - touch_sy) > 12)
			touch_moved = 1;
	} else if (touch_was_down && !touch_moved) {
		touch_tap = 1;
		touch_tap_x = touch_sx;
		touch_tap_y = touch_sy;
	}
}

/* ------------------------------------------------------------------ */
/* IME (on-screen keyboard)                                           */
/* ------------------------------------------------------------------ */
enum { IME_NONE, IME_CHAT, IME_JOIN, IME_PM, IME_NICK, IME_TOPIC, IME_FIELD, IME_FIELD_INT, IME_AWAY, IME_SEARCH, IME_KICK };

static struct {
	int      active;
	int      kind;
	int      sidx;
	uint32_t uid;
	char    *field;
	int      fieldlen;
	int     *intfield;
	uint16_t title[SCE_IME_DIALOG_MAX_TITLE_LENGTH];
	uint16_t init[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
	uint16_t buf[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
} ime;

static void ime_open(int kind, const char *title, const char *initial, int maxlen, int password, int number)
{
	if (ime.active)
		return;
	SceImeDialogParam p;
	sceImeDialogParamInit(&p);
	utf8_to_utf16(title, ime.title, SCE_IME_DIALOG_MAX_TITLE_LENGTH);
	utf8_to_utf16(initial ? initial : "", ime.init, SCE_IME_DIALOG_MAX_TEXT_LENGTH);
	memset(ime.buf, 0, sizeof(ime.buf));
	p.supportedLanguages = 0;
	p.languagesForced = SCE_FALSE;
	p.type = number ? SCE_IME_TYPE_NUMBER : SCE_IME_TYPE_DEFAULT;
	p.option = (kind == IME_FIELD || kind == IME_JOIN || kind == IME_NICK || kind == IME_PM)
	           ? SCE_IME_OPTION_NO_AUTO_CAPITALIZATION : 0;
	p.textBoxMode = password ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD : SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
	p.title = ime.title;
	p.maxTextLength = maxlen > SCE_IME_DIALOG_MAX_TEXT_LENGTH ? SCE_IME_DIALOG_MAX_TEXT_LENGTH : maxlen;
	p.initialText = ime.init;
	p.inputTextBuffer = ime.buf;
	if (sceImeDialogInit(&p) >= 0) {
		ime.active = 1;
		ime.kind = kind;
	}
}

/* ------------------------------------------------------------------ */
/* UI state                                                           */
/* ------------------------------------------------------------------ */
enum { SCR_CHAT, SCR_SERVERS, SCR_EDIT, SCR_SETTINGS, SCR_USERS, SCR_CHANLIST, SCR_FILES, SCR_HELP, SCR_IMAGE, SCR_CAMERA };

static int screen = SCR_CHAT;
static int list_sel, list_top;
static int app_running = 1;

/* menu overlay */
enum {
	A_WRITE = 1, A_JOIN, A_PM, A_USERS, A_TR_IN, A_TR_OUT, A_UPLOAD, A_LIST, A_NICK, A_TOPIC,
	A_PART, A_CLOSE, A_CONNECT, A_DISCONNECT, A_SERVERS, A_SETTINGS, A_HELP, A_EXIT, A_CLEAR,
	A_QUICK, A_Q_NICK, A_Q_SENT, A_CTX_MENTION, A_CTX_PM, A_CTX_TRANSLATE, A_URL, A_LINKS, A_NOP,
	A_SUB_TRANS, A_SUB_IMG, A_SUB_CHAN, A_CHAN_LANG_MENU, A_CHAN_LANG, A_SEARCH, A_JUMP, A_LAST_MENTION,
	A_AWAY, A_BACK, A_CAMERA, A_CTX_IGNORE, A_U_PM, A_U_MENTION, A_U_WHOIS, A_U_IGNORE, A_U_MODE,
	A_U_KICK, A_U_KICKBAN, A_PRESET, A_INVITE_JOIN
};
typedef struct { char label[160]; int action; int arg; } MenuItem;
static MenuItem menu[32];
static int  nmenu, menu_sel, menu_open, menu_arg;
static char menu_title[160];

/* context of the message / link menus */
#define MAX_CTX_URLS 12
static char     ctx_nick[40];
static uint32_t ctx_msg_id;
static char     ctx_urls[MAX_CTX_URLS][300];
static int      ctx_nurls;
static char     quick_nicks[8][40];

/* what I sent (newest first) and the unsent draft */
#define SENT_MAX 15
static char     sent_hist[SENT_MAX][460];
static int      sent_n;
static char     draft[460];
static int      draft_sidx = -1;
static uint32_t draft_uid;

/* lines drawn in the chat view, for touch hit-testing */
static int      vis_y[64];
static uint32_t vis_id[64];
static int      nvis;

/* image viewer */
static vita2d_texture *iv_tex;
static char     iv_url[300];
static char     iv_upload[512];
static char     iv_err[160];
static int      iv_loading, iv_return;
static float    iv_zoom = 1, iv_dx, iv_dy;

/* message to flash after a search / jump */
static uint32_t jump_uid, jump_id;
static uint64_t jump_until;

/* user picked in the users list */
static char     u_nick[40];

/* camera */
static int      cam_back = 1;

static const unsigned int mirc_pal[16] = {
	RGBA8(0xff, 0xff, 0xff, 0xff), RGBA8(0x8a, 0x8a, 0x8a, 0xff), RGBA8(0x6a, 0x7e, 0xff, 0xff), RGBA8(0x3f, 0xbf, 0x3f, 0xff),
	RGBA8(0xff, 0x55, 0x55, 0xff), RGBA8(0xc0, 0x70, 0x3a, 0xff), RGBA8(0xc0, 0x60, 0xe0, 0xff), RGBA8(0xff, 0xa0, 0x30, 0xff),
	RGBA8(0xff, 0xff, 0x55, 0xff), RGBA8(0x70, 0xff, 0x70, 0xff), RGBA8(0x30, 0xc0, 0xc0, 0xff), RGBA8(0x70, 0xff, 0xff, 0xff),
	RGBA8(0x70, 0x90, 0xff, 0xff), RGBA8(0xff, 0x70, 0xff, 0xff), RGBA8(0x90, 0x90, 0x90, 0xff), RGBA8(0xd0, 0xd0, 0xd0, 0xff),
};

typedef struct { const char *name, *host; int port, ssl; } Preset;
static const Preset presets[] = {
	{ "Libera.Chat", "irc.libera.chat", 6697, 1 },
	{ "OFTC", "irc.oftc.net", 6697, 1 },
	{ "EFnet", "irc.efnet.org", 6697, 1 },
	{ "Rizon", "irc.rizon.net", 6697, 1 },
	{ "DALnet", "irc.dal.net", 6697, 1 },
	{ "hackint", "irc.hackint.org", 6697, 1 },
	{ "IRCnet", "open.ircnet.net", 6667, 0 },
	{ "Undernet", "irc.undernet.org", 6667, 0 },
	{ "QuakeNet", "irc.quakenet.org", 6667, 0 },
};
#define NPRESETS ((int)(sizeof(presets) / sizeof(presets[0])))

/* confirm dialog */
static int  confirm_open, confirm_action, confirm_arg;
static char confirm_text[160];

/* server editor */
static ServerCfg edit_cfg;
static int edit_idx;

/* file browser */
typedef struct { char name[256]; int dir; } FileEnt;
static char     fb_path[512] = "ux0:picture/";
static FileEnt *fb_ents;
static int      fb_n;

/* users snapshot for the user list screen */
static ChanUser *ul_users;
static int       ul_n;

static uint64_t last_frame_ms, last_tick_ms;
static int      side_first;     /* first visible sidebar row */
static int      wifi_ok = 1;

static Server *cur_server(void)
{
	if (g_view_sidx < 0 || g_view_sidx >= g_nservers)
		return NULL;
	return &g_servers[g_view_sidx];
}

static Chan *cur_chan(void)
{
	Server *s = cur_server();
	return s ? irc_find_chan_uid(s, g_view_uid) : NULL;
}

static void set_view(int sidx, uint32_t uid)
{
	g_view_sidx = sidx;
	g_view_uid = uid;
	Chan *c = cur_chan();
	if (c) {
		c->unread = 0;
		c->mention = 0;
	}
	g_dirty = 1;
}

/* Flat list of windows (sidebar order) */
typedef struct { int sidx; Chan *c; } Row;
static Row rows[MAX_SERVERS * MAX_CHANS];
static int nrows;

static void build_rows(void)
{
	nrows = 0;
	for (int i = 0; i < g_nservers; i++) {
		if (g_cfg.servers[i].deleted)
			continue;
		Server *s = &g_servers[i];
		for (int k = 0; k < s->nchans; k++) {
			rows[nrows].sidx = i;
			rows[nrows].c = s->chans[k];
			nrows++;
		}
	}
}

static int cur_row(void)
{
	for (int i = 0; i < nrows; i++)
		if (rows[i].sidx == g_view_sidx && rows[i].c->uid == g_view_uid)
			return i;
	return -1;
}

static void ensure_valid_view(void)
{
	build_rows();
	if (cur_row() >= 0)
		return;
	if (nrows)
		set_view(rows[0].sidx, rows[0].c->uid);
	else
		g_view_sidx = -1;
}

static void cycle_view(int dir)
{
	build_rows();
	if (!nrows)
		return;
	int r = cur_row();
	r = (r + dir + nrows) % nrows;
	set_view(rows[r].sidx, rows[r].c->uid);
}

/* ------------------------------------------------------------------ */
/* Message layout                                                     */
/* ------------------------------------------------------------------ */
#define MAX_BREAKS 80

static int msg_area_x(void) { return SIDE_W + 10; }
static int msg_area_w(void) { return SCR_W - SIDE_W - 22; }

static int time_w(void) { return text_w("00:00") + 10; }

static void msg_prefix(Msg *m, char *out, int n)
{
	switch (m->type) {
	case MT_MSG:    snprintf(out, n, "%s ", m->nick); break;
	case MT_ACTION: snprintf(out, n, "* %s ", m->nick); break;
	case MT_NOTICE: snprintf(out, n, "-%s- ", m->nick[0] ? m->nick : "*"); break;
	case MT_JOIN:   snprintf(out, n, "→ "); break;
	case MT_PART: case MT_QUIT: case MT_KICK: snprintf(out, n, "← "); break;
	case MT_ERROR:  snprintf(out, n, "! "); break;
	default:        snprintf(out, n, "· "); break;
	}
}

static int wrap(const char *t, int first_w, int rest_w, uint16_t *br, int nbr, int base)
{
	int lines = 0;
	const char *p = t;
	if (nbr <= 0)
		return 0;
	br[lines++] = base;
	float w = 0;
	int avail = first_w;
	const char *line_start = t, *last_space = NULL;
	while (*p) {
		uint32_t cp;
		int len = utf8_decode(p, &cp);
		float cw = char_width(cp);
		if (w + cw > avail && p > line_start) {
			const char *brk = (last_space && last_space > line_start) ? last_space + 1 : p;
			if (lines >= nbr)
				break;
			br[lines++] = base + (brk - t);
			line_start = brk;
			last_space = NULL;
			avail = rest_w;
			w = 0;
			for (const char *q = brk; q < p; ) {
				uint32_t c2;
				q += utf8_decode(q, &c2);
				w += char_width(c2);
			}
		}
		if (cp == ' ')
			last_space = p;
		w += cw;
		p += len;
	}
	return lines;
}

static void msg_layout(Msg *m, int width)
{
	if (m->lay_w == width && m->lay_breaks)
		return;
	if (!m->lay_breaks)
		m->lay_breaks = malloc(MAX_BREAKS * sizeof(uint16_t));
	if (!m->lay_breaks) {
		m->lay_lines = 1;
		m->lay_text_lines = 1;
		return;
	}
	char pre[64];
	msg_prefix(m, pre, sizeof(pre));
	int tw = time_w();
	int pw = text_w(pre);
	int n = wrap(m->text, width - tw - pw, width - tw, m->lay_breaks, MAX_BREAKS / 2, 0);
	if (n < 1) n = 1;
	m->lay_text_lines = n;
	if (m->trans_state == TR_DONE && m->trans) {
		int aw = text_w("» ");
		n += wrap(m->trans, width - tw - aw, width - tw - aw, m->lay_breaks + n, MAX_BREAKS - n, 0);
	} else if (m->trans_state == TR_PENDING) {
		n += 1;
	}
	m->lay_lines = n;
	m->lay_w = width;
}

static unsigned int msg_color(Msg *m)
{
	switch (m->type) {
	case MT_MSG: case MT_ACTION: return C_TEXT;
	case MT_NOTICE: return C_NOTICE;
	case MT_JOIN:   return C_OK;
	case MT_PART: case MT_QUIT: case MT_KICK: return RGBA8(0xb0, 0x80, 0x60, 0xff);
	case MT_ERROR:  return C_ERR;
	case MT_TOPIC:  return C_ACCENT;
	default:        return C_DIM;
	}
}

static void draw_segment(int x, int top, unsigned int color, const char *s, int from, int to)
{
	char buf[1100];
	int n = to - from;
	if (n <= 0)
		return;
	if (n > (int)sizeof(buf) - 1)
		n = sizeof(buf) - 1;
	memcpy(buf, s + from, n);
	buf[n] = 0;
	draw_text(x, top, color, buf);
}

/* Draws [from, to) of the message text honoring mIRC colors, bold and underline. */
static void draw_rich(Msg *m, int x, int top, unsigned int def, int from, int to)
{
	if (!m->nspans || !g_cfg.irc_colors) {
		draw_segment(x, top, def, m->text, from, to);
		return;
	}
	char buf[1100];
	int pos = from;
	while (pos < to) {
		FmtSpan st = { 0, -1, -1, 0, 0, 0 };
		int next = to;
		for (int i = 0; i < m->nspans; i++) {
			if (m->spans[i].off <= pos) {
				st = m->spans[i];
			} else {
				if (m->spans[i].off < next)
					next = m->spans[i].off;
				break;
			}
		}
		int n = next - pos;
		if (n > (int)sizeof(buf) - 1)
			n = sizeof(buf) - 1;
		memcpy(buf, m->text + pos, n);
		buf[n] = 0;
		int w = text_w(buf);
		unsigned int col = st.fg >= 0 ? mirc_pal[st.fg] : def;
		if (st.bg >= 0) {
			vita2d_draw_rectangle(x, top + 1, w, lh - 2, st.bg == 1 ? RGBA8(0x10, 0x10, 0x10, 0xff) : mirc_pal[st.bg]);
			if (st.fg < 0)
				col = (st.bg == 0 || st.bg == 8 || st.bg == 9 || st.bg == 11 || st.bg == 15) ? RGBA8(0, 0, 0, 0xff) : def;
		}
		draw_text(x, top, col, buf);
		if (st.bold)
			draw_text(x + 1, top, col, buf);
		if (st.underline)
			vita2d_draw_rectangle(x, top + asc + 2, w, 1, col);
		x += w;
		pos = next;
	}
}

static void draw_msg_line(Msg *m, int line, int x, int top, int width)
{
	int tw = time_w();
	if (jump_id == m->id && now_ms() < jump_until && g_view_uid == jump_uid)
		vita2d_draw_rectangle(x - 4, top, width + 8, lh, RGBA8(0x2a, 0x3a, 0x5a, 0xff));
	if (m->highlight && line < m->lay_text_lines)
		vita2d_draw_rectangle(x - 4, top, width + 8, lh, C_HL_BG);

	if (line < m->lay_text_lines) {
		const char *t = m->text;
		int from = m->lay_breaks ? m->lay_breaks[line] : 0;
		int to = (m->lay_breaks && line + 1 < m->lay_text_lines) ? m->lay_breaks[line + 1] : (int)strlen(t);
		int tx = x + tw;
		if (line == 0) {
			draw_text(x, top, C_FAINT, m->time);
			char pre[64];
			msg_prefix(m, pre, sizeof(pre));
			unsigned int pc = (m->type == MT_MSG || m->type == MT_ACTION)
			                  ? (m->self ? C_ACCENT : nick_color(m->nick)) : msg_color(m);
			draw_text(tx, top, pc, pre);
			tx += text_w(pre);
		}
		draw_rich(m, tx, top, msg_color(m), from, to);
	} else if (m->trans_state == TR_PENDING) {
		draw_text(x + tw, top, C_FAINT, T("» traduciendo..."));
	} else if (m->trans) {
		int li = line - m->lay_text_lines;
		int base = m->lay_text_lines;
		int from = m->lay_breaks[base + li];
		int to = (base + li + 1 < m->lay_lines) ? m->lay_breaks[base + li + 1] : (int)strlen(m->trans);
		int ax = x + tw;
		if (li == 0)
			draw_text(ax, top, C_TRANS, "»");
		draw_segment(ax + text_w("» "), top, C_TRANS, m->trans, from, to);
	}
}

static int chat_top(void) { return TOP_H + 4; }
static int chat_bottom(void) { return SCR_H - BOT_H - 4; }

static int visible_lines(void) { return (chat_bottom() - chat_top()) / lh; }

static int total_lines(Chan *c, int width)
{
	int total = 0;
	for (int i = 0; i < c->count; i++) {
		Msg *m = irc_msg_at(c, i);
		msg_layout(m, width);
		total += m->lay_lines;
	}
	return total;
}

static void clamp_scroll(Chan *c)
{
	int maxs = total_lines(c, msg_area_w()) - visible_lines();
	if (maxs < 0) maxs = 0;
	if (c->scroll > maxs) c->scroll = maxs;
	if (c->scroll < 0) c->scroll = 0;
}

static void jump_to(Chan *c, uint32_t id)
{
	int width = msg_area_w(), after = 0, found = 0, mlines = 1;
	for (int i = c->count - 1; i >= 0; i--) {
		Msg *m = irc_msg_at(c, i);
		msg_layout(m, width);
		if (m->id == id) {
			found = 1;
			mlines = m->lay_lines;
			break;
		}
		after += m->lay_lines;
	}
	if (!found)
		return;
	c->scroll = after - visible_lines() / 2 + mlines / 2;
	clamp_scroll(c);
	jump_uid = c->uid;
	jump_id = id;
	jump_until = now_ms() + 2500;
	g_dirty = 1;
}

static void draw_chat(Chan *c)
{
	int x = msg_area_x(), width = msg_area_w();
	int top = chat_top(), bottom = chat_bottom();

	/* keep the view still while reading history */
	for (int i = c->count - 1; i >= 0; i--) {
		Msg *m = irc_msg_at(c, i);
		if (m->id <= c->ui_seen_id)
			break;
		msg_layout(m, width);
		if (c->scroll > 0)
			c->scroll += m->lay_lines;
	}
	if (c->count)
		c->ui_seen_id = irc_msg_at(c, c->count - 1)->id;
	clamp_scroll(c);

	vita2d_set_clip_rectangle(SIDE_W, top, SCR_W, bottom);
	vita2d_enable_clipping();
	int y = bottom - (bottom - top) % lh;   /* align so lines fit exactly */
	nvis = 0;
	int skip = c->scroll;
	for (int i = c->count - 1; i >= 0 && y > top - lh; i--) {
		Msg *m = irc_msg_at(c, i);
		msg_layout(m, width);
		for (int ln = m->lay_lines - 1; ln >= 0 && y > top - lh; ln--) {
			if (skip > 0) {
				skip--;
				continue;
			}
			y -= lh;
			draw_msg_line(m, ln, x, y, width);
			if (nvis < 64) {
				vis_y[nvis] = y;
				vis_id[nvis] = m->id;
				nvis++;
			}
		}
	}
	vita2d_disable_clipping();

	if (c->scroll > 0) {
		const char *t = T("▼ mensajes nuevos abajo");
		int w = text_w(t) + 20;
		vita2d_draw_rectangle(SCR_W - w - 14, bottom - lh - 6, w, lh + 4, C_SEL);
		draw_text(SCR_W - w - 4, bottom - lh - 4, C_ACCENT, t);
	}
	if (c->count == 0)
		draw_text(x, top + 6, C_FAINT, T("Sin mensajes todavía."));
}

/* ------------------------------------------------------------------ */
/* Frame pieces                                                       */
/* ------------------------------------------------------------------ */
static const char *state_text(int st)
{
	switch (st) {
	case SS_CONNECTING:  return T("conectando");
	case SS_REGISTERING: return T("registrando");
	case SS_ONLINE:      return T("en línea");
	case SS_WAITING:     return T("reintentando");
	default:             return T("desconectado");
	}
}

static unsigned int state_color(int st)
{
	switch (st) {
	case SS_ONLINE:  return C_OK;
	case SS_OFF:     return C_FAINT;
	case SS_WAITING: return C_ERR;
	default:         return C_ACCENT;
	}
}

static void draw_topbar(const char *title, const char *sub)
{
	vita2d_draw_rectangle(0, 0, SCR_W, TOP_H, C_PANEL);
	vita2d_draw_rectangle(0, TOP_H - 1, SCR_W, 1, C_LINE);
	int y = (TOP_H - lh) / 2;
	draw_text(12, y, C_ACCENT, "VitaIRC");
	int x = 12 + text_w("VitaIRC") + 16;

	char clock[6];
	time_hhmm(clock);
	int batt = scePowerGetBatteryLifePercent();
	char right[64];
	snprintf(right, sizeof(right), "%s%s  %d%%%s  %s",
	         g_net_busy[0] ? g_net_busy : "", g_net_busy[0] ? "  " : "",
	         batt, scePowerIsBatteryCharging() ? "+" : "", clock);
	int rw = text_w(right);
	if (!wifi_ok) {
		int ww = text_w(T("SIN WI-FI")) + 12;
		draw_text(SCR_W - 12 - rw - ww, y, C_ERR, T("SIN WI-FI"));
		rw += ww;
	}
	draw_text(SCR_W - 12 - rw, y, g_net_busy[0] ? C_TRANS : C_DIM, right);

	int avail = SCR_W - 24 - rw - x;
	draw_text_fit(x, y, avail, C_TEXT, title);
	if (sub && sub[0]) {
		int tw = text_w(title) + 14;
		if (tw < avail - 40)
			draw_text_fit(x + tw, y, avail - tw, C_DIM, sub);
	}
}

static void draw_sidebar(void)
{
	vita2d_draw_rectangle(0, TOP_H, SIDE_W, SCR_H - TOP_H, C_SIDE);
	vita2d_draw_rectangle(SIDE_W - 1, TOP_H, 1, SCR_H - TOP_H, C_LINE);
	build_rows();
	int rh = lh + 6;
	int top = TOP_H + 4;
	int avail = (SCR_H - BOT_H - 4 - top) / rh;
	int cur = cur_row();
	int first = side_first;
	if (cur >= 0) {
		if (cur < first) first = cur;
		if (cur >= first + avail) first = cur - avail + 1;
	}
	if (first > nrows - avail) first = nrows - avail;
	if (first < 0) first = 0;
	side_first = first;

	vita2d_set_clip_rectangle(0, top, SIDE_W - 1, SCR_H - BOT_H);
	vita2d_enable_clipping();
	for (int i = first; i < nrows && i < first + avail; i++) {
		Row *r = &rows[i];
		Server *s = &g_servers[r->sidx];
		int y = top + (i - first) * rh;
		int selected = (i == cur);
		if (selected) {
			vita2d_draw_rectangle(0, y, SIDE_W - 1, rh, C_SEL);
			vita2d_draw_rectangle(0, y, 3, rh, r->c->type == CH_QUERY ? C_PM : C_ACCENT);
		}
		int ty = y + 3;
		if (r->c->type == CH_STATUS) {
			vita2d_draw_fill_circle(14, y + rh / 2, 4, state_color(s->state));
			draw_text_fit(26, ty, SIDE_W - 60, selected ? C_TEXT : RGBA8(0xb0, 0xb0, 0xc0, 0xff),
			              g_cfg.servers[r->sidx].name);
		} else {
			unsigned int col = r->c->type == CH_QUERY ? C_PM : C_CHAN;
			if (r->c->type == CH_CHANNEL && !r->c->joined)
				col = C_FAINT;
			if (selected)
				col = r->c->type == CH_QUERY ? C_PM : C_TEXT;
			const char *ico = r->c->type == CH_QUERY ? "@ " : "";
			char label[80];
			snprintf(label, sizeof(label), "%s%s", ico, r->c->name);
			draw_text_fit(26, ty, SIDE_W - 72, col, label);
		}
		if (r->c->unread && !selected) {
			char b[8];
			snprintf(b, sizeof(b), "%d", r->c->unread > 99 ? 99 : r->c->unread);
			int bw = text_w(b) + 10;
			unsigned int bc = r->c->mention ? C_ERR : RGBA8(0x2a, 0x3a, 0x5a, 0xff);
			vita2d_draw_rectangle(SIDE_W - bw - 10, y + 4, bw, rh - 8, bc);
			vita2d_pgf_draw_text(font, SIDE_W - bw - 5, y + 3 + asc - 1, C_TEXT, fscale * 0.85f, b);
		}
	}
	vita2d_disable_clipping();
	if (!nrows)
		draw_text(12, top + 6, C_FAINT, T("Sin servidores"));
}

static void draw_bottombar_chat(Chan *c)
{
	int y = SCR_H - BOT_H;
	vita2d_draw_rectangle(SIDE_W, y, SCR_W - SIDE_W, BOT_H, C_PANEL);
	vita2d_draw_rectangle(SIDE_W, y, SCR_W - SIDE_W, 1, C_LINE);
	int ty = y + (BOT_H - lh) / 2;
	int x = SIDE_W + 10;

	/* input box (tap to write) */
	int bw = 262;
	vita2d_draw_rectangle(x, y + 5, bw, BOT_H - 10, C_BG);
	draw_rect_outline(x, y + 5, bw, BOT_H - 10, C_LINE);
	int gx = x + 6 + draw_glyph(glyph_ok, x + 6, ty);
	char ph[80];
	if (c && c->type != CH_STATUS && c->trans_out)
		snprintf(ph, sizeof(ph), T("Escribir (se envía en %s)"), g_cfg.lang_out);
	else if (c && c->type == CH_STATUS)
		snprintf(ph, sizeof(ph), T("Comando (/join #canal)"));
	else
		snprintf(ph, sizeof(ph), T("Escribir mensaje..."));
	draw_text_fit(gx, ty, bw - (gx - x) - 6, C_FAINT, ph);

	x += bw + 14;
	x = draw_hint(x, ty, G_TRIANGLE, T("Menú"));
	if (c && c->type != CH_STATUS)
		x = draw_hint(x, ty, glyph_back, T("Rápido"));
	if (c && c->type != CH_STATUS)
		x = draw_hint(x, ty, G_SQUARE, c->trans_in ? T("Trad: ON") : T("Trad: OFF"));
	x = draw_hint(x, ty, G_L, "");
	x -= 10;
	x = draw_hint(x, ty, G_R, T("Canal"));
}

static void draw_bottombar_hints(int n, const int *glyphs, const char **labels)
{
	int y = SCR_H - BOT_H;
	vita2d_draw_rectangle(0, y, SCR_W, BOT_H, C_PANEL);
	vita2d_draw_rectangle(0, y, SCR_W, 1, C_LINE);
	int ty = y + (BOT_H - lh) / 2;
	int x = 14;
	for (int i = 0; i < n; i++)
		x = draw_hint(x, ty, glyphs[i], labels[i]);
}

static void draw_toast(void)
{
	if (!g_toast[0] || now_ms() > g_toast_until)
		return;
	int w = text_w(g_toast) + 30;
	if (w > SCR_W - 80) w = SCR_W - 80;
	int x = (SCR_W - w) / 2, y = TOP_H + 10;
	vita2d_draw_rectangle(x, y, w, lh + 14, RGBA8(0x22, 0x22, 0x34, 0xf0));
	draw_rect_outline(x, y, w, lh + 14, C_ACCENT);
	draw_text_fit(x + 15, y + 7, w - 30, C_TEXT, g_toast);
}

/* generic list frame; returns the row height */
static int list_rows_visible(int rh)
{
	return (SCR_H - BOT_H - TOP_H - 16) / rh;
}

static void list_clamp(int n, int rh)
{
	int vis = list_rows_visible(rh);
	if (list_sel >= n) list_sel = n - 1;
	if (list_sel < 0) list_sel = 0;
	if (list_sel < list_top) list_top = list_sel;
	if (list_sel >= list_top + vis) list_top = list_sel - vis + 1;
	if (list_top < 0) list_top = 0;
}

static int list_nav(int n)
{
	int rh = lh + 10;
	if (REPEAT(SCE_CTRL_UP)) list_sel--;
	if (REPEAT(SCE_CTRL_DOWN)) list_sel++;
	if (REPEAT(SCE_CTRL_LTRIGGER)) list_sel -= list_rows_visible(rh);
	if (REPEAT(SCE_CTRL_RTRIGGER)) list_sel += list_rows_visible(rh);
	if (n > 0) {
		if (list_sel < 0) list_sel = (pad_pressed & SCE_CTRL_UP) ? n - 1 : 0;
		if (list_sel >= n) list_sel = (pad_pressed & SCE_CTRL_DOWN) ? 0 : n - 1;
	}
	/* touch: tap a row to select it, tap the selected row to activate */
	int activate = 0;
	if (touch_down && touch_moved && touch_dy) {
		static int acc;
		acc += touch_dy;
		while (acc <= -rh) { list_top++; acc += rh; }
		while (acc >= rh) { list_top--; acc -= rh; }
		int vis = list_rows_visible(rh);
		if (list_top > n - vis) list_top = n - vis;
		if (list_top < 0) list_top = 0;
		if (list_sel < list_top) list_sel = list_top;
		if (list_sel >= list_top + vis) list_sel = list_top + vis - 1;
		return 0;
	}
	if (touch_tap && touch_tap_y > TOP_H + 8 && touch_tap_y < SCR_H - BOT_H) {
		int r = list_top + (touch_tap_y - TOP_H - 8) / rh;
		if (r >= 0 && r < n) {
			if (r == list_sel) activate = 1;
			list_sel = r;
		}
	}
	list_clamp(n, rh);
	return activate;
}

static void draw_list_row(int i, int y, int rh, int selected)
{
	(void)i;
	if (selected) {
		vita2d_draw_rectangle(10, y, SCR_W - 20, rh - 2, C_SEL);
		vita2d_draw_rectangle(10, y, 3, rh - 2, C_ACCENT);
	}
}

/* ------------------------------------------------------------------ */
/* Menu                                                               */
/* ------------------------------------------------------------------ */
static void menu_add_arg(const char *label, int action, int arg)
{
	if (nmenu >= (int)(sizeof(menu) / sizeof(menu[0])))
		return;
	str_copy(menu[nmenu].label, label, sizeof(menu[0].label));
	menu[nmenu].action = action;
	menu[nmenu].arg = arg;
	nmenu++;
}

static void menu_add(const char *label, int action)
{
	menu_add_arg(label, action, 0);
}

static void menu_begin(const char *title)
{
	nmenu = 0;
	menu_sel = 0;
	str_copy(menu_title, title, sizeof(menu_title));
}

/* ---------------- links ---------------- */
static int extract_urls(const char *text, char out[][300], int max, int n)
{
	const char *p = text;
	while (n < max && (p = strstr(p, "http"))) {
		if (strncmp(p, "http://", 7) && strncmp(p, "https://", 8)) {
			p += 4;
			continue;
		}
		int len = strcspn(p, " \t\"'<>");
		while (len > 0 && strchr(".,;:!?)]", p[len - 1]))
			len--;
		if (len > 10 && len < 299) {
			char u[300];
			memcpy(u, p, len);
			u[len] = 0;
			int dup = 0;
			for (int i = 0; i < n; i++)
				if (!strcmp(out[i], u)) dup = 1;
			if (!dup)
				str_copy(out[n++], u, 300);
		}
		p += len > 0 ? len : 4;
	}
	return n;
}

/* Returns 1 and a direct image URL when the link can be shown in the viewer. */
static int image_url(const char *url, char *direct, int n)
{
	const char *host = strstr(url, "://");
	host = host ? host + 3 : url;
	char path[300];
	str_copy(path, url, sizeof(path));
	path[strcspn(path, "?#")] = 0;
	const char *ext = strrchr(path, '.');
	if (ext && ext > strrchr(path, '/') && (!strcasecmp(ext, ".png") || !strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg"))) {
		str_copy(direct, url, n);
		return 1;
	}
	/* imgur.com/<id>  ->  i.imgur.com/<id>.jpg (albums and galleries open in the browser) */
	if (!strncmp(host, "imgur.com/", 10) || !strncmp(host, "www.imgur.com/", 14) || !strncmp(host, "m.imgur.com/", 12)) {
		const char *id = strchr(host, '/') + 1;
		if (!strncmp(id, "a/", 2) || !strncmp(id, "gallery/", 8) || !strncmp(id, "t/", 2) || !*id)
			return 0;
		char idb[64];
		str_copy(idb, id, sizeof(idb));
		idb[strcspn(idb, "/?#.")] = 0;
		snprintf(direct, n, "https://i.imgur.com/%s.jpg", idb);
		return 1;
	}
	return 0;
}

static void open_browser(const char *url)
{
	sceAppMgrLaunchAppByUri(0xFFFFF, url);
}

static void open_menu(void)
{
	Server *s = cur_server();
	Chan *c = cur_chan();
	menu_begin(T("Menú"));
	char b[160];
	int is_chan = c && c->type == CH_CHANNEL;
	int is_win = c && c->type != CH_STATUS;
	if (s) {
		menu_add(T("Escribir mensaje"), A_WRITE);
		if (is_win)
			menu_add(T("Respuestas rápidas (nicks y enviados)"), A_QUICK);
		menu_add(T("Unirse a un canal..."), A_JOIN);
		menu_add(T("Mensaje privado..."), A_PM);
		if (is_chan) {
			snprintf(b, sizeof(b), T("Usuarios del canal (%d)"), c->nusers);
			menu_add(b, A_USERS);
		}
		if (is_win) {
			menu_add(T("Traducción..."), A_SUB_TRANS);
			menu_add(T("Imágenes (Imgur, cámara, enlaces)..."), A_SUB_IMG);
			menu_add(T("Buscar en el canal..."), A_SEARCH);
			menu_add(T("Ir a la última mención"), A_LAST_MENTION);
		}
		menu_add(T("Explorar canales del servidor (/list)"), A_LIST);
		if (is_win)
			menu_add(T("Canal (tema, salir, cerrar)..."), A_SUB_CHAN);
		else
			menu_add(T("Limpiar ventana"), A_CLEAR);
		menu_add(T("Cambiar nick..."), A_NICK);
		if (s->away)
			menu_add(T("Volver (quitar ausente)"), A_BACK);
		else
			menu_add(T("Marcar como ausente..."), A_AWAY);
		if (s->want_connect)
			snprintf(b, sizeof(b), T("Desconectar de %s"), g_cfg.servers[s->idx].name);
		else
			snprintf(b, sizeof(b), T("Conectar a %s"), g_cfg.servers[s->idx].name);
		menu_add(b, s->want_connect ? A_DISCONNECT : A_CONNECT);
	}
	menu_add(T("Servidores..."), A_SERVERS);
	menu_add(T("Ajustes (ChatGPT, Imgur...)"), A_SETTINGS);
	menu_add(T("Ayuda y controles"), A_HELP);
	menu_add(T("Salir de VitaIRC"), A_EXIT);
	menu_open = 1;
}

static void open_sub_trans(void)
{
	Chan *c = cur_chan();
	if (!c)
		return;
	char b[160];
	menu_begin(T("Traducción"));
	snprintf(b, sizeof(b), T("Traducir mensajes recibidos → %s: %s"), irc_chan_lang(c), c->trans_in ? "ON" : "OFF");
	menu_add(b, A_TR_IN);
	snprintf(b, sizeof(b), T("Traducir mis mensajes → %s: %s"), g_cfg.lang_out, c->trans_out ? "ON" : "OFF");
	menu_add(b, A_TR_OUT);
	snprintf(b, sizeof(b), T("Idioma de traducción de este canal: %s"),
	         c->trans_lang[0] ? g_lang_names[lang_index(c->trans_lang)] : T("el de Ajustes"));
	menu_add(b, A_CHAN_LANG_MENU);
	menu_open = 1;
}

static void open_sub_img(void)
{
	menu_begin(T("Imágenes"));
	menu_add(T("Subir imagen a Imgur..."), A_UPLOAD);
	menu_add(T("Tomar foto con la cámara y subirla"), A_CAMERA);
	menu_add(T("Enlaces e imágenes del canal"), A_LINKS);
	menu_open = 1;
}

static void open_sub_chan(void)
{
	Chan *c = cur_chan();
	if (!c)
		return;
	menu_begin(c->name);
	if (c->type == CH_CHANNEL)
		menu_add(T("Cambiar tema del canal..."), A_TOPIC);
	if (c->type == CH_CHANNEL && c->joined)
		menu_add(T("Salir del canal"), A_PART);
	menu_add(T("Cerrar ventana"), A_CLOSE);
	menu_add(T("Limpiar ventana"), A_CLEAR);
	menu_open = 1;
}

static void open_chan_lang_menu(void)
{
	menu_begin(T("Idioma de traducción de este canal"));
	menu_add_arg(T("El de Ajustes (global)"), A_CHAN_LANG, -1);
	for (int i = 0; i < g_lang_count; i++)
		menu_add_arg(g_lang_names[i], A_CHAN_LANG, i);
	menu_open = 1;
}

static void open_user_menu(const char *nick)
{
	Server *s = cur_server();
	Chan *c = cur_chan();
	if (!s)
		return;
	str_copy(u_nick, nick, sizeof(u_nick));
	char b[120];
	menu_begin(nick);
	menu_add(T("Mensaje privado"), A_U_PM);
	menu_add(T("Mencionar"), A_U_MENTION);
	menu_add("WHOIS", A_U_WHOIS);
	menu_add(irc_is_ignored(nick) ? T("Dejar de ignorar") : T("Ignorar"), A_U_IGNORE);
	char me = c && c->type == CH_CHANNEL ? irc_my_prefix(s, c) : 0;
	if (me && strchr("~&@%", me)) {
		ChanUser *u = NULL;
		for (int i = 0; i < c->nusers; i++)
			if (!str_icmp(c->users[i].nick, nick)) u = &c->users[i];
		int has_op = u && u->prefix && strchr("~&@", u->prefix);
		int has_voice = u && u->prefix == '+';
		snprintf(b, sizeof(b), has_op ? T("Quitar op a %s") : T("Dar op a %s"), nick);
		menu_add_arg(b, A_U_MODE, has_op ? 1 : 0);
		snprintf(b, sizeof(b), has_voice ? T("Quitar voz a %s") : T("Dar voz a %s"), nick);
		menu_add_arg(b, A_U_MODE, has_voice ? 3 : 2);
		menu_add(T("Expulsar (kick)..."), A_U_KICK);
		menu_add(T("Banear y expulsar"), A_U_KICKBAN);
	}
	menu_open = 1;
}

static void open_presets(void)
{
	menu_begin(T("Añadir servidor"));
	for (int i = 0; i < NPRESETS; i++) {
		char b[120];
		snprintf(b, sizeof(b), "%s  (%s:%d%s)", presets[i].name, presets[i].host, presets[i].port, presets[i].ssl ? " SSL" : "");
		menu_add_arg(b, A_PRESET, i);
	}
	menu_add_arg(T("Otro servidor (manual)..."), A_PRESET, -1);
	menu_open = 1;
}

static void search_results(const char *q)
{
	Chan *c = cur_chan();
	if (!c || !q[0])
		return;
	char title[160];
	snprintf(title, sizeof(title), T("Resultados para \"%s\""), q);
	menu_begin(title);
	for (int i = c->count - 1; i >= 0 && nmenu < 30; i--) {
		Msg *m = irc_msg_at(c, i);
		if (!str_icontains(m->text, q) && !(m->trans && str_icontains(m->trans, q)))
			continue;
		char b[200];
		snprintf(b, sizeof(b), "%s %s%s%s", m->time, m->nick, m->nick[0] ? ": " : "", m->text);
		menu_add_arg(b, A_JUMP, (int)m->id);
	}
	if (!nmenu) {
		ui_toast(T("Sin resultados para \"%s\""), q);
		return;
	}
	menu_open = 1;
}

static void ask_confirm(const char *text, int action, int arg)
{
	str_copy(confirm_text, text, sizeof(confirm_text));
	confirm_action = action;
	confirm_arg = arg;
	confirm_open = 1;
}

static void open_chat_ime(const char *initial)
{
	Server *s = cur_server();
	Chan *c = cur_chan();
	if (!s || !c)
		return;
	char title[128];
	if (c->type == CH_STATUS)
		snprintf(title, sizeof(title), T("%s: comando"), g_cfg.servers[s->idx].name);
	else if (c->trans_out)
		snprintf(title, sizeof(title), T("%s (se traducirá a %s)"), c->name, g_cfg.lang_out);
	else
		snprintf(title, sizeof(title), T("Mensaje para %s"), c->name);
	ime.sidx = s->idx;
	ime.uid = c->uid;
	if ((!initial || !initial[0]) && draft[0] && draft_sidx == s->idx && draft_uid == c->uid)
		initial = draft;
	ime_open(IME_CHAT, title, initial, 450, 0, 0);
}

static void open_quick(void)
{
	Server *s = cur_server();
	Chan *c = cur_chan();
	if (!s || !c)
		return;
	menu_begin(T("Respuestas rápidas"));
	menu_add(T("Escribir mensaje nuevo"), A_WRITE);
	int nn = 0;
	if (c->type != CH_STATUS) {
		for (int i = c->count - 1; i >= 0 && nn < 6; i--) {
			Msg *m = irc_msg_at(c, i);
			if ((m->type != MT_MSG && m->type != MT_ACTION) || m->self || !m->nick[0])
				continue;
			int dup = 0;
			for (int k = 0; k < nn; k++)
				if (!str_icmp(quick_nicks[k], m->nick)) dup = 1;
			if (dup)
				continue;
			str_copy(quick_nicks[nn], m->nick, sizeof(quick_nicks[0]));
			char b[80];
			snprintf(b, sizeof(b), T("Responder a %s"), m->nick);
			menu_add_arg(b, A_Q_NICK, nn);
			nn++;
		}
	}
	if (sent_n) {
		menu_add(T("— Enviados recientemente —"), A_NOP);
		for (int i = 0; i < sent_n && i < 8; i++)
			menu_add_arg(sent_hist[i], A_Q_SENT, i);
	}
	menu_open = 1;
}

static void open_links(void)
{
	Chan *c = cur_chan();
	if (!c)
		return;
	menu_begin(T("Enlaces e imágenes del canal"));
	ctx_nurls = 0;
	for (int i = c->count - 1; i >= 0 && ctx_nurls < MAX_CTX_URLS; i--) {
		Msg *m = irc_msg_at(c, i);
		int before = ctx_nurls;
		ctx_nurls = extract_urls(m->text, ctx_urls, MAX_CTX_URLS, ctx_nurls);
		for (int k = before; k < ctx_nurls; k++) {
			char direct[300], b[200];
			int img = image_url(ctx_urls[k], direct, sizeof(direct));
			snprintf(b, sizeof(b), "%s %s: %s", img ? "[img]" : "[web]", m->nick[0] ? m->nick : "*", ctx_urls[k]);
			menu_add_arg(b, A_URL, k);
		}
	}
	if (!ctx_nurls) {
		ui_toast(T("No hay enlaces en los mensajes recientes"));
		return;
	}
	menu_open = 1;
}

static void open_msg_menu(Chan *c, uint32_t id)
{
	Msg *m = irc_find_msg(c, id);
	if (!m)
		return;
	char title[160];
	snprintf(title, sizeof(title), "%s%s%s", m->nick, m->nick[0] ? ": " : "", m->text);
	menu_begin(title);
	ctx_msg_id = id;
	str_copy(ctx_nick, m->nick, sizeof(ctx_nick));
	ctx_nurls = extract_urls(m->text, ctx_urls, MAX_CTX_URLS, 0);
	for (int k = 0; k < ctx_nurls && k < 4; k++) {
		char direct[300], b[200];
		int img = image_url(ctx_urls[k], direct, sizeof(direct));
		snprintf(b, sizeof(b), "%s %s", img ? T("Ver imagen:") : T("Abrir en el navegador:"), ctx_urls[k]);
		menu_add_arg(b, A_URL, k);
	}
	int chatty = m->type == MT_MSG || m->type == MT_ACTION || m->type == MT_NOTICE;
	if (chatty && m->nick[0] && !m->self) {
		char b[80];
		snprintf(b, sizeof(b), T("Responder a %s"), m->nick);
		menu_add(b, A_CTX_MENTION);
		snprintf(b, sizeof(b), T("Mensaje privado a %s"), m->nick);
		menu_add(b, A_CTX_PM);
		snprintf(b, sizeof(b), irc_is_ignored(m->nick) ? T("Dejar de ignorar a %s") : T("Ignorar a %s"), m->nick);
		menu_add(b, A_CTX_IGNORE);
	}
	if (chatty && m->trans_state != TR_DONE && !tr_should_skip(m->text))
		menu_add(T("Traducir este mensaje"), A_CTX_TRANSLATE);
	if (nmenu)
		menu_open = 1;
}

/* ---------------- image viewer ---------------- */
static void iv_free(void)
{
	if (iv_tex) {
		vita2d_wait_rendering_done();
		vita2d_free_texture(iv_tex);
		iv_tex = NULL;
	}
}

static void iv_decode(const unsigned char *buf, size_t len)
{
	iv_free();
	iv_err[0] = 0;
	if (len > 8 && buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G')
		iv_tex = vita2d_load_PNG_buffer(buf);
	else if (len > 3 && buf[0] == 0xFF && buf[1] == 0xD8)
		iv_tex = vita2d_load_JPEG_buffer(buf, len);
	else if (len > 4 && !memcmp(buf, "GIF8", 4)) {
		str_copy(iv_err, T("Los GIF no se pueden mostrar aquí. Ábrelo en el navegador."), sizeof(iv_err));
		return;
	} else {
		str_copy(iv_err, T("Formato no soportado (solo PNG y JPG)."), sizeof(iv_err));
		return;
	}
	if (!iv_tex)
		str_copy(iv_err, T("No se pudo abrir la imagen (¿demasiado grande?)."), sizeof(iv_err));
	iv_zoom = 1;
	iv_dx = iv_dy = 0;
}

static void open_viewer_url(const char *direct, const char *orig)
{
	iv_free();
	str_copy(iv_url, orig ? orig : direct, sizeof(iv_url));
	iv_upload[0] = 0;
	iv_err[0] = 0;
	iv_loading = 1;
	iv_return = screen;
	img_fetch_request(direct);
	screen = SCR_IMAGE;
}

static void open_viewer_file(const char *path)
{
	iv_free();
	str_copy(iv_url, path, sizeof(iv_url));
	str_copy(iv_upload, path, sizeof(iv_upload));
	iv_loading = 0;
	iv_return = SCR_FILES;
	screen = SCR_IMAGE;
	FILE *f = fopen(path, "rb");
	if (!f) {
		str_copy(iv_err, T("No se pudo abrir el archivo"), sizeof(iv_err));
		return;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *buf = (n > 0 && n < 16 * 1024 * 1024) ? malloc(n) : NULL;
	if (buf && fread(buf, 1, n, f) == (size_t)n)
		iv_decode(buf, n);
	else
		str_copy(iv_err, T("No se pudo leer el archivo"), sizeof(iv_err));
	free(buf);
	fclose(f);
}

static void image_input(void)
{
	if (PRESSED(btn_back)) {
		iv_free();
		iv_loading = 0;
		screen = iv_return;
		return;
	}
	if (PRESSED(SCE_CTRL_TRIANGLE) && !iv_upload[0])
		open_browser(iv_url);
	if (PRESSED(btn_ok) && iv_upload[0]) {
		Server *s = cur_server();
		Chan *c = cur_chan();
		if (s && c) {
			img_upload_request(s->idx, c->uid, iv_upload);
			const char *name = strrchr(iv_upload, '/');
			ui_toast(T("Subiendo %s..."), name ? name + 1 : iv_upload);
		}
		iv_free();
		screen = SCR_CHAT;
		return;
	}
	if (REPEAT(SCE_CTRL_RTRIGGER)) iv_zoom *= 1.25f;
	if (REPEAT(SCE_CTRL_LTRIGGER)) iv_zoom /= 1.25f;
	if (iv_zoom < 0.25f) iv_zoom = 0.25f;
	if (iv_zoom > 8) iv_zoom = 8;
	if (PRESSED(SCE_CTRL_START) || PRESSED(SCE_CTRL_SELECT)) { iv_zoom = 1; iv_dx = iv_dy = 0; }
	float step = 24;
	if (pad_now & SCE_CTRL_LEFT) iv_dx += step;
	if (pad_now & SCE_CTRL_RIGHT) iv_dx -= step;
	if (pad_now & SCE_CTRL_UP) iv_dy += step;
	if (pad_now & SCE_CTRL_DOWN) iv_dy -= step;
	static int lx = -1, ly = -1;
	if (touch_down) {
		if (lx >= 0) {
			iv_dx += touch_x - lx;
			iv_dy += touch_y - ly;
		}
		lx = touch_x;
		ly = touch_y;
	} else {
		lx = ly = -1;
	}
	if (pad_now)
		g_dirty = 1;
}

static void take_photo(void)
{
	char dir[] = DATA_DIR "/photos";
	sceIoMkdir(dir, 0777);
	SceDateTime t;
	sceRtcGetCurrentClockLocalTime(&t);
	char path[128];
	snprintf(path, sizeof(path), "%s/foto_%04d%02d%02d_%02d%02d%02d.jpg", dir, t.year, t.month, t.day, t.hour, t.minute, t.second);
	int ok = cam_save_jpeg(path) == 0;
	cam_close();
	if (!ok) {
		ui_toast(T("No se pudo guardar la foto"));
		screen = SCR_CHAT;
		return;
	}
	open_viewer_file(path);
	iv_return = SCR_CHAT;
}

static void camera_input(void)
{
	if (PRESSED(btn_back)) {
		cam_close();
		screen = SCR_CHAT;
		return;
	}
	if (PRESSED(SCE_CTRL_SQUARE)) {
		cam_back = !cam_back;
		if (cam_open(cam_back) < 0) {
			ui_toast(T("No se pudo abrir la cámara"));
			screen = SCR_CHAT;
			return;
		}
	}
	if (PRESSED(btn_ok) || touch_tap)
		take_photo();
}

static void draw_camera_screen(void)
{
	vita2d_draw_rectangle(0, TOP_H, SCR_W, SCR_H - TOP_H, RGBA8(0, 0, 0, 0xff));
	draw_topbar(T("Cámara"), cam_back ? T("trasera") : T("frontal"));
	cam_update();
	vita2d_texture *tx = cam_texture();
	if (tx) {
		float h = SCR_H - TOP_H - BOT_H;
		float sc = h / CAM_H;
		float w = CAM_W * sc;
		vita2d_draw_texture_scale(tx, (SCR_W - w) / 2, TOP_H, sc, sc);
	}
	const int g[] = { glyph_ok, G_SQUARE, glyph_back };
	const char *l[] = { T("Tomar foto"), T("Cambiar cámara"), T("Cancelar") };
	draw_bottombar_hints(3, g, l);
	g_dirty = 1;   /* live preview */
}

static void draw_image_screen(void)
{
	vita2d_draw_rectangle(0, TOP_H, SCR_W, SCR_H - TOP_H, RGBA8(0x05, 0x05, 0x08, 0xff));
	draw_topbar(iv_upload[0] ? T("Vista previa") : T("Imagen"), iv_url);
	int top = TOP_H, h = SCR_H - TOP_H - BOT_H;
	if (iv_tex) {
		float w = vita2d_texture_get_width(iv_tex), ih = vita2d_texture_get_height(iv_tex);
		float fit = SCR_W / w < h / ih ? SCR_W / w : h / ih;
		if (fit > 2) fit = 2;
		float sc = fit * iv_zoom;
		float dw = w * sc, dh = ih * sc;
		float x = (SCR_W - dw) / 2 + iv_dx, y = top + (h - dh) / 2 + iv_dy;
		vita2d_set_clip_rectangle(0, top, SCR_W, top + h);
		vita2d_enable_clipping();
		vita2d_draw_texture_scale(iv_tex, x, y, sc, sc);
		vita2d_disable_clipping();
		char info[48];
		snprintf(info, sizeof(info), "%dx%d  %d%%", (int)w, (int)ih, (int)(sc * 100 + 0.5f));
		draw_text(SCR_W - 14 - text_w(info), SCR_H - BOT_H - lh - 6, C_DIM, info);
	} else if (iv_loading) {
		draw_text((SCR_W - text_w(T("Cargando imagen..."))) / 2, top + h / 2 - lh, C_DIM, T("Cargando imagen..."));
	} else if (iv_err[0]) {
		draw_text_fit(30, top + h / 2 - lh, SCR_W - 60, C_ERR, iv_err);
	}
	if (iv_upload[0]) {
		const int g[] = { glyph_ok, G_L, G_R, glyph_back };
		const char *l[] = { T("Subir a Imgur"), "", "Zoom", T("Volver") };
		draw_bottombar_hints(4, g, l);
	} else {
		const int g[] = { G_L, G_R, G_DPAD, G_TRIANGLE, glyph_back };
		const char *l[] = { "", "Zoom", T("Mover"), T("Abrir en el navegador"), T("Cerrar") };
		draw_bottombar_hints(5, g, l);
	}
}

static void fb_load(void);
static void users_snapshot(void);
static void start_edit_server(int idx);
static void open_quick(void);
static void open_links(void);
static void open_viewer_url(const char *direct, const char *orig);
static void open_viewer_file(const char *path);
static void open_sub_trans(void);
static void open_sub_img(void);
static void open_sub_chan(void);
static void open_chan_lang_menu(void);
static void open_user_menu(const char *nick);
static void open_presets(void);
static void search_results(const char *q);
static void jump_to(Chan *c, uint32_t id);

static void do_action(int a)
{
	Server *s = cur_server();
	Chan *c = cur_chan();
	switch (a) {
	case A_WRITE: open_chat_ime(""); break;
	case A_JOIN:
		if (s) { ime.sidx = s->idx; ime_open(IME_JOIN, T("Unirse a canal (ej: #vitasdk)"), "#", 64, 0, 0); }
		break;
	case A_PM:
		if (s) { ime.sidx = s->idx; ime_open(IME_PM, T("Nick para mensaje privado"), "", 32, 0, 0); }
		break;
	case A_NICK:
		if (s) { ime.sidx = s->idx; ime_open(IME_NICK, T("Nuevo nick"), s->nick, 30, 0, 0); }
		break;
	case A_TOPIC:
		if (s && c) { ime.sidx = s->idx; ime.uid = c->uid; ime_open(IME_TOPIC, T("Nuevo tema del canal"), c->topic, 390, 0, 0); }
		break;
	case A_USERS:
		users_snapshot();
		screen = SCR_USERS;
		list_sel = list_top = 0;
		break;
	case A_TR_IN:
		if (c) {
			if (!g_cfg.openai_key[0]) {
				ui_toast(T("Configura tu API key de OpenAI en Ajustes"));
				break;
			}
			c->trans_in = !c->trans_in;
			if (c->trans_in) {
				/* translate the last messages already on screen */
				int done = 0;
				for (int i = c->count - 1; i >= 0 && done < 12; i--) {
					Msg *m = irc_msg_at(c, i);
					if ((m->type == MT_MSG || m->type == MT_ACTION || m->type == MT_NOTICE) && !m->self &&
					    m->trans_state == TR_NONE && !tr_should_skip(m->text)) {
						m->trans_state = TR_PENDING;
						m->lay_w = 0;
						tr_request_in(s->idx, c->uid, m->id, m->text, irc_chan_lang(c));
						done++;
					}
				}
			}
			ui_toast(T("Traducción de recibidos: %s"), c->trans_in ? T("activada") : T("desactivada"));
		}
		break;
	case A_TR_OUT:
		if (c) {
			if (!g_cfg.openai_key[0]) {
				ui_toast(T("Configura tu API key de OpenAI en Ajustes"));
				break;
			}
			c->trans_out = !c->trans_out;
			ui_toast(T("Tus mensajes %s"), c->trans_out ? T("se traducirán con ChatGPT") : T("se envían sin traducir"));
		}
		break;
	case A_UPLOAD:
		if (!g_cfg.imgur_id[0]) {
			ui_toast(T("Configura tu Client-ID de Imgur en Ajustes"));
			break;
		}
		screen = SCR_FILES;
		fb_load();
		break;
	case A_LIST:
		if (s) {
			screen = SCR_CHANLIST;
			list_sel = list_top = 0;
			if (s->listing == 0 || !s->nlisted)
				irc_user_input(s->idx, c ? c->uid : 0, "/list");
		}
		break;
	case A_PART:
		if (s && c) irc_user_input(s->idx, c->uid, "/part");
		break;
	case A_CLOSE:
		if (s && c) {
			irc_user_input(s->idx, c->uid, "/close");
			ensure_valid_view();
		}
		break;
	case A_CLEAR:
		if (s && c) irc_user_input(s->idx, c->uid, "/clear");
		break;
	case A_CONNECT:
		if (s) irc_connect(s->idx);
		break;
	case A_DISCONNECT:
		if (s) irc_disconnect(s->idx, NULL);
		break;
	case A_SERVERS:
		screen = SCR_SERVERS;
		list_sel = list_top = 0;
		break;
	case A_SETTINGS:
		screen = SCR_SETTINGS;
		list_sel = list_top = 0;
		break;
	case A_HELP:
		screen = SCR_HELP;
		break;
	case A_EXIT:
		ask_confirm(T("¿Salir de VitaIRC? Se cerrarán las conexiones."), A_EXIT, 0);
		break;
	case A_QUICK:
		open_quick();
		break;
	case A_Q_NICK: {
		char b[64];
		snprintf(b, sizeof(b), "%s: ", quick_nicks[menu_arg]);
		open_chat_ime(b);
		break;
	}
	case A_Q_SENT:
		open_chat_ime(sent_hist[menu_arg]);
		break;
	case A_CTX_MENTION: {
		char b[64];
		snprintf(b, sizeof(b), "%s: ", ctx_nick);
		open_chat_ime(b);
		break;
	}
	case A_CTX_PM:
		if (s) {
			char b[64];
			snprintf(b, sizeof(b), "/query %s", ctx_nick);
			irc_user_input(s->idx, c ? c->uid : 0, b);
		}
		break;
	case A_CTX_TRANSLATE:
		if (s && c) {
			Msg *m = irc_find_msg(c, ctx_msg_id);
			if (!g_cfg.openai_key[0]) {
				ui_toast(T("Configura tu API key de OpenAI en Ajustes"));
			} else if (m) {
				m->trans_state = TR_PENDING;
				m->lay_w = 0;
				tr_request_one(s->idx, c->uid, m->id, m->text, irc_chan_lang(c));
			}
		}
		break;
	case A_URL: {
		char direct[300];
		if (image_url(ctx_urls[menu_arg], direct, sizeof(direct)))
			open_viewer_url(direct, ctx_urls[menu_arg]);
		else
			open_browser(ctx_urls[menu_arg]);
		break;
	}
	case A_LINKS:
		open_links();
		break;
	case A_SUB_TRANS: open_sub_trans(); break;
	case A_SUB_IMG: open_sub_img(); break;
	case A_SUB_CHAN: open_sub_chan(); break;
	case A_CHAN_LANG_MENU: open_chan_lang_menu(); break;
	case A_CHAN_LANG:
		if (c) {
			if (menu_arg < 0)
				c->trans_lang[0] = 0;
			else
				str_copy(c->trans_lang, g_lang_codes[menu_arg], sizeof(c->trans_lang));
			session_save();
			ui_toast(T("%s se traducirá a %s"), c->name, g_lang_names[lang_index(irc_chan_lang(c))]);
		}
		break;
	case A_SEARCH:
		if (s && c) {
			ime.sidx = s->idx;
			ime.uid = c->uid;
			ime_open(IME_SEARCH, T("Buscar en el canal"), "", 60, 0, 0);
		}
		break;
	case A_JUMP:
		if (c) {
			screen = SCR_CHAT;
			jump_to(c, (uint32_t)menu_arg);
		}
		break;
	case A_LAST_MENTION:
		if (c) {
			for (int i = c->count - 1; i >= 0; i--) {
				Msg *m = irc_msg_at(c, i);
				if (m->highlight) {
					jump_to(c, m->id);
					return;
				}
			}
			ui_toast(T("No hay menciones en este canal"));
		}
		break;
	case A_AWAY:
		if (s) {
			ime.sidx = s->idx;
			ime_open(IME_AWAY, T("Motivo de ausencia"), T("Ausente"), 120, 0, 0);
		}
		break;
	case A_BACK:
		if (s)
			irc_user_input(s->idx, c ? c->uid : 0, "/back");
		break;
	case A_CAMERA:
		if (!g_cfg.imgur_id[0])
			ui_toast(T("Configura tu Client-ID de Imgur en Ajustes"));
		else if (cam_open(cam_back) == 0)
			screen = SCR_CAMERA;
		else
			ui_toast(T("No se pudo abrir la cámara"));
		break;
	case A_CTX_IGNORE:
	case A_U_IGNORE:
		if (s) {
			const char *n = a == A_CTX_IGNORE ? ctx_nick : u_nick;
			char b[80];
			snprintf(b, sizeof(b), irc_is_ignored(n) ? "/unignore %s" : "/ignore %s", n);
			irc_user_input(s->idx, c ? c->uid : 0, b);
			if (screen == SCR_USERS)
				screen = SCR_CHAT;
		}
		break;
	case A_U_PM:
		if (s) {
			char b[64];
			snprintf(b, sizeof(b), "/query %s", u_nick);
			irc_user_input(s->idx, c ? c->uid : 0, b);
			screen = SCR_CHAT;
		}
		break;
	case A_U_MENTION: {
		char b[64];
		snprintf(b, sizeof(b), "%s: ", u_nick);
		screen = SCR_CHAT;
		open_chat_ime(b);
		break;
	}
	case A_U_WHOIS:
		if (s) {
			char b[64];
			snprintf(b, sizeof(b), "/whois %s", u_nick);
			irc_user_input(s->idx, c ? c->uid : 0, b);
			screen = SCR_CHAT;
		}
		break;
	case A_U_MODE:
		if (s && c) {
			static const char *cmds[] = { "/op", "/deop", "/voice", "/devoice" };
			char b[80];
			snprintf(b, sizeof(b), "%s %s", cmds[menu_arg], u_nick);
			irc_user_input(s->idx, c->uid, b);
			screen = SCR_CHAT;
		}
		break;
	case A_U_KICK:
		if (s && c) {
			ime.sidx = s->idx;
			ime.uid = c->uid;
			char t[96];
			snprintf(t, sizeof(t), T("Motivo para expulsar a %s"), u_nick);
			ime_open(IME_KICK, t, "", 120, 0, 0);
		}
		break;
	case A_U_KICKBAN:
		if (s && c) {
			char b[80];
			snprintf(b, sizeof(b), "/kickban %s", u_nick);
			irc_user_input(s->idx, c->uid, b);
			screen = SCR_CHAT;
		}
		break;
	case A_PRESET:
		start_edit_server(-1);
		if (menu_arg >= 0) {
			const Preset *p = &presets[menu_arg];
			str_copy(edit_cfg.name, p->name, sizeof(edit_cfg.name));
			str_copy(edit_cfg.host, p->host, sizeof(edit_cfg.host));
			edit_cfg.port = p->port;
			edit_cfg.ssl = p->ssl;
		}
		break;
	}
}


static void draw_menu(void)
{
	vita2d_draw_rectangle(0, 0, SCR_W, SCR_H, C_OVERLAY);
	int rh = lh + 2;
	int w = 640, h = nmenu * rh + lh + 30;
	if (h > SCR_H - 20) h = SCR_H - 20;
	int x = (SCR_W - w) / 2, y = (SCR_H - h) / 2;
	vita2d_draw_rectangle(x, y, w, h, C_PANEL);
	draw_rect_outline(x, y, w, h, C_LINE);
	draw_text_fit(x + 16, y + 8, w - 32, C_ACCENT, menu_title);
	int ly = y + lh + 18;
	int vis = (h - lh - 26) / rh;
	int first = menu_sel >= vis ? menu_sel - vis + 1 : 0;
	for (int i = first; i < nmenu && i < first + vis; i++) {
		int ry = ly + (i - first) * rh;
		if (i == menu_sel) {
			vita2d_draw_rectangle(x + 6, ry, w - 12, rh, C_SEL);
			vita2d_draw_rectangle(x + 6, ry, 3, rh, C_ACCENT);
		}
		unsigned int col = menu[i].action == A_EXIT ? C_ERR : menu[i].action == A_NOP ? C_FAINT :
		                   (i == menu_sel ? C_TEXT : RGBA8(0xa8, 0xa8, 0xb8, 0xff));
		draw_text_fit(x + 20, ry + 1, w - 40, col, menu[i].label);
	}
	if (first > 0)
		draw_text(x + w - 30, y + 8, C_DIM, "▲");
	if (first + vis < nmenu)
		draw_text(x + w - 30, y + h - lh - 4, C_ACCENT, "▼");
}

static void menu_input(void)
{
	if (REPEAT(SCE_CTRL_UP)) menu_sel = (menu_sel - 1 + nmenu) % nmenu;
	if (REPEAT(SCE_CTRL_DOWN)) menu_sel = (menu_sel + 1) % nmenu;
	int activate = PRESSED(btn_ok);
	if (touch_tap) {
		int rh = lh + 2;
		int w = 640, h = nmenu * rh + lh + 30;
		if (h > SCR_H - 20) h = SCR_H - 20;
		int x = (SCR_W - w) / 2, y = (SCR_H - h) / 2;
		int vis = (h - lh - 26) / rh;
		int first = menu_sel >= vis ? menu_sel - vis + 1 : 0;
		if (touch_tap_x < x || touch_tap_x > x + w || touch_tap_y < y || touch_tap_y > y + h) {
			menu_open = 0;
			return;
		}
		int r = first + (touch_tap_y - (y + lh + 18)) / rh;
		if (touch_tap_y >= y + lh + 18 && r >= 0 && r < nmenu) {
			menu_sel = r;
			activate = 1;
		}
	}
	if (activate && menu[menu_sel].action == A_NOP)
		activate = 0;
	if (activate) {
		menu_open = 0;
		menu_arg = menu[menu_sel].arg;
		do_action(menu[menu_sel].action);
	} else if (PRESSED(btn_back) || PRESSED(SCE_CTRL_TRIANGLE)) {
		menu_open = 0;
	}
}

static void draw_confirm(void)
{
	vita2d_draw_rectangle(0, 0, SCR_W, SCR_H, C_OVERLAY);
	int w = 600, h = lh * 3 + 40;
	int x = (SCR_W - w) / 2, y = (SCR_H - h) / 2;
	vita2d_draw_rectangle(x, y, w, h, C_PANEL);
	draw_rect_outline(x, y, w, h, C_ACCENT);
	draw_text_fit(x + 20, y + 16, w - 40, C_TEXT, confirm_text);
	int hx = x + 20, hy = y + h - lh - 14;
	hx = draw_hint(hx, hy, glyph_ok, T("Sí"));
	draw_hint(hx, hy, glyph_back, "No");
}

static void confirm_input(void)
{
	if (PRESSED(btn_ok) || touch_tap) {
		confirm_open = 0;
		if (!PRESSED(btn_ok))
			return;
		if (confirm_action == A_EXIT) {
			app_running = 0;
		} else if (confirm_action == A_INVITE_JOIN) {
			char b[80];
			snprintf(b, sizeof(b), "/join %s", g_invite_chan);
			irc_user_input(confirm_arg, 0, b);
		} else if (confirm_action == 1000) {   /* delete server */
			int i = confirm_arg;
			irc_disconnect(i, NULL);
			g_cfg.servers[i].deleted = 1;
			config_save();
			ensure_valid_view();
			list_sel = 0;
		}
	} else if (PRESSED(btn_back)) {
		confirm_open = 0;
	}
}

/* ------------------------------------------------------------------ */
/* Chat screen                                                        */
/* ------------------------------------------------------------------ */
static void chat_input(void)
{
	Server *s = cur_server();
	Chan *c = cur_chan();

	if (PRESSED(SCE_CTRL_TRIANGLE) || PRESSED(SCE_CTRL_START)) {
		open_menu();
		return;
	}
	if (PRESSED(SCE_CTRL_SELECT)) {
		do_action(A_SERVERS);
		return;
	}
	if (REPEAT(SCE_CTRL_LTRIGGER) || REPEAT(SCE_CTRL_LEFT)) cycle_view(-1);
	if (REPEAT(SCE_CTRL_RTRIGGER) || REPEAT(SCE_CTRL_RIGHT)) cycle_view(1);
	if (!s || !c)
		return;
	if (PRESSED(btn_ok))
		open_chat_ime("");
	if (PRESSED(SCE_CTRL_SQUARE) && c->type != CH_STATUS)
		do_action(A_TR_IN);
	if (PRESSED(btn_back)) {
		if (c->scroll > 0)
			c->scroll = 0;
		else if (c->type != CH_STATUS)
			open_quick();
	}
	int page = visible_lines() - 2;
	if (REPEAT(SCE_CTRL_UP)) c->scroll += 3;
	if (REPEAT(SCE_CTRL_DOWN)) c->scroll -= 3;
	(void)page;

	/* touch */
	static int acc;
	if (touch_down && touch_moved && touch_sx > SIDE_W && touch_dy) {
		acc += touch_dy;
		while (acc >= lh) { c->scroll++; acc -= lh; }
		while (acc <= -lh) { c->scroll--; acc += lh; }
		g_dirty = 1;
	}
	if (!touch_down)
		acc = 0;
	if (touch_tap) {
		if (touch_tap_x < SIDE_W && touch_tap_y > TOP_H) {
			int rh = lh + 6;
			build_rows();
			int top = TOP_H + 4;
			int r = side_first + (touch_tap_y - top) / rh;
			if (r >= 0 && r < nrows)
				set_view(rows[r].sidx, rows[r].c->uid);
		} else if (touch_tap_y > SCR_H - BOT_H) {
			if (touch_tap_x < SIDE_W + 280)
				open_chat_ime("");
			else
				open_menu();
		} else if (touch_tap_x > SIDE_W && touch_tap_y >= chat_top() && touch_tap_y < chat_bottom()) {
			for (int i = 0; i < nvis; i++)
				if (touch_tap_y >= vis_y[i] && touch_tap_y < vis_y[i] + lh) {
					open_msg_menu(c, vis_id[i]);
					break;
				}
		}
	}
	clamp_scroll(c);
}

static void draw_chat_screen(void)
{
	Server *s = cur_server();
	Chan *c = cur_chan();
	char title[160] = "", sub[420] = "";
	if (s && c) {
		if (c->type == CH_STATUS) {
			snprintf(title, sizeof(title), "%s", g_cfg.servers[s->idx].name);
			snprintf(sub, sizeof(sub), "%s  ·  %s  ·  %s%s", s->cfg.host, s->nick[0] ? s->nick : s->cfg.nick, state_text(s->state),
			         s->away ? T("  ·  ausente") : "");
		} else if (c->type == CH_CHANNEL) {
			snprintf(title, sizeof(title), "%s", c->name);
			snprintf(sub, sizeof(sub), T("%d usuarios%s%s"), c->nusers, c->topic[0] ? "  ·  " : "", c->topic);
		} else {
			snprintf(title, sizeof(title), "@%s", c->name);
			snprintf(sub, sizeof(sub), T("privado en %s"), g_cfg.servers[s->idx].name);
		}
	}
	vita2d_draw_rectangle(SIDE_W, TOP_H, SCR_W - SIDE_W, SCR_H - TOP_H, C_BG);
	draw_topbar(title, sub);
	draw_sidebar();
	if (c)
		draw_chat(c);
	else
		draw_text(SIDE_W + 20, TOP_H + 20, C_DIM, T("Agrega un servidor desde el menú (△ → Servidores)."));
	draw_bottombar_chat(c);
}

/* ------------------------------------------------------------------ */
/* Servers screen                                                     */
/* ------------------------------------------------------------------ */
static int srv_rows[MAX_SERVERS + 1];
static int srv_nrows;

static void build_srv_rows(void)
{
	srv_nrows = 0;
	for (int i = 0; i < g_cfg.nservers; i++)
		if (!g_cfg.servers[i].deleted)
			srv_rows[srv_nrows++] = i;
	srv_rows[srv_nrows++] = -1;   /* "add" row */
}

static void servers_input(void)
{
	build_srv_rows();
	int act = list_nav(srv_nrows);
	int idx = srv_rows[list_sel];
	if (PRESSED(btn_ok) || act) {
		if (idx < 0)
			open_presets();
		else
			start_edit_server(idx);
	} else if (PRESSED(SCE_CTRL_SQUARE) && idx >= 0) {
		if (g_servers[idx].want_connect)
			irc_disconnect(idx, NULL);
		else
			irc_connect(idx);
	} else if (PRESSED(SCE_CTRL_TRIANGLE) && idx >= 0) {
		char b[160];
		snprintf(b, sizeof(b), T("¿Eliminar el servidor \"%s\"?"), g_cfg.servers[idx].name);
		ask_confirm(b, 1000, idx);
	} else if (PRESSED(btn_back)) {
		screen = SCR_CHAT;
	}
}

static void draw_servers_screen(void)
{
	build_srv_rows();
	vita2d_draw_rectangle(0, TOP_H, SCR_W, SCR_H - TOP_H, C_BG);
	draw_topbar(T("Servidores"), T("configura tus redes IRC"));
	int rh = lh + 10;
	list_clamp(srv_nrows, rh);
	int vis = list_rows_visible(rh);
	for (int i = list_top; i < srv_nrows && i < list_top + vis; i++) {
		int y = TOP_H + 8 + (i - list_top) * rh;
		draw_list_row(i, y, rh, i == list_sel);
		int idx = srv_rows[i];
		if (idx < 0) {
			draw_text(30, y + 4, C_ACCENT, T("+ Añadir servidor"));
			continue;
		}
		ServerCfg *sc = &g_cfg.servers[idx];
		Server *s = &g_servers[idx];
		vita2d_draw_fill_circle(30, y + rh / 2, 5, state_color(s->state));
		draw_text_fit(46, y + 4, 300, C_TEXT, sc->name);
		char b[200];
		snprintf(b, sizeof(b), "%s:%d%s  ·  %s", sc->host, sc->port, sc->ssl ? " SSL" : "", sc->nick);
		draw_text_fit(360, y + 4, 400, C_DIM, b);
		draw_text(SCR_W - 30 - text_w(state_text(s->state)), y + 4, state_color(s->state), state_text(s->state));
	}
	const int g[] = { glyph_ok, G_SQUARE, G_TRIANGLE, glyph_back };
	const char *l[] = { T("Editar"), T("Conectar/Desconectar"), T("Eliminar"), T("Volver") };
	draw_bottombar_hints(4, g, l);
}

/* ------------------------------------------------------------------ */
/* Server editor                                                      */
/* ------------------------------------------------------------------ */
enum { F_NAME, F_HOST, F_PORT, F_SSL, F_NICK, F_USER, F_REAL, F_PASS, F_AUTH, F_CHANS, F_AUTO, F_SAVE, F_CANCEL, F_COUNT };

static void start_edit_server(int idx)
{
	edit_idx = idx;
	if (idx >= 0) {
		edit_cfg = g_cfg.servers[idx];
	} else {
		config_server_defaults(&edit_cfg);
		strcpy(edit_cfg.name, T("Nuevo servidor"));
		edit_cfg.host[0] = 0;
		edit_cfg.channels[0] = 0;
	}
	screen = SCR_EDIT;
	list_sel = list_top = 0;
}

static void edit_save(void)
{
	if (!edit_cfg.name[0] || !edit_cfg.host[0] || !edit_cfg.nick[0]) {
		ui_toast(T("Nombre, host y nick son obligatorios"));
		return;
	}
	if (edit_cfg.port <= 0 || edit_cfg.port > 65535)
		edit_cfg.port = edit_cfg.ssl ? 6697 : 6667;
	pthread_mutex_lock(&g_lock);
	int idx = edit_idx;
	if (idx < 0) {
		/* reuse a deleted, idle slot or append */
		for (int i = 0; i < g_cfg.nservers; i++) {
			if (g_cfg.servers[i].deleted && g_servers[i].state == SS_OFF && !g_servers[i].want_connect) {
				idx = i;
				Server *s = &g_servers[i];
				while (s->nchans > 1)
					irc_close_chan(s, s->chans[s->nchans - 1]);
				irc_user_input(i, s->chans[0]->uid, "/clear");
				break;
			}
		}
		if (idx < 0) {
			if (g_cfg.nservers >= MAX_SERVERS) {
				pthread_mutex_unlock(&g_lock);
				ui_toast(T("Máximo %d servidores"), MAX_SERVERS);
				return;
			}
			idx = g_cfg.nservers++;
		}
	}
	edit_cfg.deleted = 0;
	g_cfg.servers[idx] = edit_cfg;
	pthread_mutex_unlock(&g_lock);
	config_save();
	irc_sync_from_config();
	Server *s = &g_servers[idx];
	if (s->state == SS_ONLINE)
		ui_toast(T("Guardado. Reconecta para aplicar los cambios."));
	else
		ui_toast(T("Servidor guardado"));
	if (edit_idx < 0) {
		if (edit_cfg.autoconnect)
			irc_connect(idx);
		set_view(idx, s->chans[0]->uid);
	}
	screen = SCR_SERVERS;
}

static void edit_field_label(int f, char *label, char *value, int n)
{
	value[0] = 0;
	switch (f) {
	case F_NAME:  strcpy(label, T("Nombre")); snprintf(value, n, "%s", edit_cfg.name); break;
	case F_HOST:  strcpy(label, "Host"); snprintf(value, n, "%s", edit_cfg.host); break;
	case F_PORT:  strcpy(label, T("Puerto")); snprintf(value, n, "%d", edit_cfg.port); break;
	case F_SSL:   strcpy(label, "SSL/TLS"); snprintf(value, n, "%s", edit_cfg.ssl ? T("Sí") : "No"); break;
	case F_NICK:  strcpy(label, "Nick"); snprintf(value, n, "%s", edit_cfg.nick); break;
	case F_USER:  strcpy(label, T("Cuenta (usuario)")); snprintf(value, n, "%s", edit_cfg.user[0] ? edit_cfg.user : T("(igual al nick)")); break;
	case F_REAL:  strcpy(label, T("Nombre real")); snprintf(value, n, "%s", edit_cfg.realname); break;
	case F_PASS:  strcpy(label, T("Contraseña")); snprintf(value, n, "%s", edit_cfg.password[0] ? "********" : T("(ninguna)")); break;
	case F_AUTH:  strcpy(label, T("Autenticación")); snprintf(value, n, "< %s >", auth_name(edit_cfg.auth)); break;
	case F_CHANS: strcpy(label, T("Canales al conectar")); snprintf(value, n, "%s", edit_cfg.channels[0] ? edit_cfg.channels : T("(ninguno)")); break;
	case F_AUTO:  strcpy(label, T("Conectar al iniciar")); snprintf(value, n, "%s", edit_cfg.autoconnect ? T("Sí") : "No"); break;
	case F_SAVE:  strcpy(label, T("Guardar")); break;
	case F_CANCEL: strcpy(label, T("Cancelar")); break;
	}
}

static void edit_input(void)
{
	int act = list_nav(F_COUNT);
	int f = list_sel;
	if (f == F_AUTH && (REPEAT(SCE_CTRL_LEFT) || REPEAT(SCE_CTRL_RIGHT))) {
		int d = REPEAT(SCE_CTRL_LEFT) ? -1 : 1;
		edit_cfg.auth = (edit_cfg.auth + d + AUTH_COUNT) % AUTH_COUNT;
	}
	if (PRESSED(btn_back)) {
		screen = SCR_SERVERS;
		return;
	}
	if (!(PRESSED(btn_ok) || act))
		return;
	switch (f) {
	case F_NAME:  ime.field = edit_cfg.name; ime.fieldlen = sizeof(edit_cfg.name); ime_open(IME_FIELD, T("Nombre del servidor"), edit_cfg.name, 40, 0, 0); break;
	case F_HOST:  ime.field = edit_cfg.host; ime.fieldlen = sizeof(edit_cfg.host); ime_open(IME_FIELD, T("Host (ej: irc.libera.chat)"), edit_cfg.host, 120, 0, 0); break;
	case F_PORT: {
		char b[8];
		snprintf(b, sizeof(b), "%d", edit_cfg.port);
		ime.intfield = &edit_cfg.port;
		ime_open(IME_FIELD_INT, T("Puerto (6697 SSL / 6667 sin SSL)"), b, 5, 0, 1);
		break;
	}
	case F_SSL:
		edit_cfg.ssl = !edit_cfg.ssl;
		if (edit_cfg.ssl && edit_cfg.port == 6667) edit_cfg.port = 6697;
		if (!edit_cfg.ssl && edit_cfg.port == 6697) edit_cfg.port = 6667;
		break;
	case F_NICK:  ime.field = edit_cfg.nick; ime.fieldlen = sizeof(edit_cfg.nick); ime_open(IME_FIELD, "Nick", edit_cfg.nick, 30, 0, 0); break;
	case F_USER:  ime.field = edit_cfg.user; ime.fieldlen = sizeof(edit_cfg.user); ime_open(IME_FIELD, T("Cuenta para NickServ/SASL (vacío = nick)"), edit_cfg.user, 40, 0, 0); break;
	case F_REAL:  ime.field = edit_cfg.realname; ime.fieldlen = sizeof(edit_cfg.realname); ime_open(IME_FIELD, T("Nombre real"), edit_cfg.realname, 60, 0, 0); break;
	case F_PASS:  ime.field = edit_cfg.password; ime.fieldlen = sizeof(edit_cfg.password); ime_open(IME_FIELD, T("Contraseña"), edit_cfg.password, 120, 1, 0); break;
	case F_AUTH:  edit_cfg.auth = (edit_cfg.auth + 1) % AUTH_COUNT; break;
	case F_CHANS: ime.field = edit_cfg.channels; ime.fieldlen = sizeof(edit_cfg.channels); ime_open(IME_FIELD, T("Canales separados por coma (#a,#b)"), edit_cfg.channels, 500, 0, 0); break;
	case F_AUTO:  edit_cfg.autoconnect = !edit_cfg.autoconnect; break;
	case F_SAVE:  edit_save(); break;
	case F_CANCEL: screen = SCR_SERVERS; break;
	}
}

static void draw_form(const char *title, const char *sub, int nfields,
                      void (*labeler)(int, char *, char *, int), int nbuttons)
{
	vita2d_draw_rectangle(0, TOP_H, SCR_W, SCR_H - TOP_H, C_BG);
	draw_topbar(title, sub);
	int rh = lh + 10;
	list_clamp(nfields, rh);
	int vis = list_rows_visible(rh);
	for (int i = list_top; i < nfields && i < list_top + vis; i++) {
		int y = TOP_H + 8 + (i - list_top) * rh;
		draw_list_row(i, y, rh, i == list_sel);
		char label[64], value[600];
		labeler(i, label, value, sizeof(value));
		if (i >= nfields - nbuttons) {
			draw_text(30, y + 4, i == list_sel ? C_ACCENT : C_TEXT, label);
		} else {
			draw_text(30, y + 4, C_DIM, label);
			draw_text_fit(300, y + 4, SCR_W - 330, C_TEXT, value);
		}
	}
}

static void draw_edit_screen(void)
{
	draw_form(edit_idx < 0 ? T("Nuevo servidor") : T("Editar servidor"), edit_cfg.name, F_COUNT, edit_field_label, 2);
	const int g[] = { glyph_ok, G_DPAD, glyph_back };
	const char *l[] = { T("Editar / cambiar"), T("Elegir"), T("Volver sin guardar") };
	draw_bottombar_hints(3, g, l);
}

/* ------------------------------------------------------------------ */
/* Settings                                                           */
/* ------------------------------------------------------------------ */
enum { S_KEY, S_MODEL, S_LANG_IN, S_LANG_OUT, S_USAGE, S_IMGUR, S_HIGHLIGHT, S_IGNORE, S_SOUND, S_COLORS, S_JOINS, S_AWAKE, S_CPU, S_FONT, S_UILANG, S_ABOUT, S_COUNT };

static void settings_label(int f, char *label, char *value, int n)
{
	value[0] = 0;
	switch (f) {
	case S_KEY:
		strcpy(label, T("API key de OpenAI (ChatGPT)"));
		if (g_cfg.openai_key[0]) {
			int len = strlen(g_cfg.openai_key);
			snprintf(value, n, "%.3s...%s", g_cfg.openai_key, len > 4 ? g_cfg.openai_key + len - 4 : "");
		} else {
			snprintf(value, n, T("(sin configurar)"));
		}
		break;
	case S_MODEL:    strcpy(label, T("Modelo")); snprintf(value, n, "%s", g_cfg.openai_model); break;
	case S_LANG_IN:  strcpy(label, T("Traducir recibidos a")); snprintf(value, n, "< %s >", g_lang_names[lang_index(g_cfg.lang_in)]); break;
	case S_LANG_OUT: strcpy(label, T("Traducir mis mensajes a")); snprintf(value, n, "< %s >", g_lang_names[lang_index(g_cfg.lang_out)]); break;
	case S_USAGE:
		strcpy(label, T("Uso de OpenAI"));
		snprintf(value, n, T("hoy %ld tokens  ·  total %ld tokens (%ld pedidos)"), g_usage_today, g_usage_total, g_usage_requests);
		break;
	case S_HIGHLIGHT: strcpy(label, T("Palabras que te avisan")); snprintf(value, n, "%s", g_cfg.highlight[0] ? g_cfg.highlight : T("(solo tu nick)")); break;
	case S_IGNORE:   strcpy(label, T("Nicks ignorados")); snprintf(value, n, "%s", g_cfg.ignore[0] ? g_cfg.ignore : T("(ninguno)")); break;
	case S_SOUND:    strcpy(label, T("Sonido en privados y menciones")); snprintf(value, n, "%s", g_cfg.sound ? T("Sí") : "No"); break;
	case S_COLORS:   strcpy(label, T("Colores y negritas de IRC")); snprintf(value, n, "%s", g_cfg.irc_colors ? T("Sí") : "No"); break;
	case S_IMGUR:    strcpy(label, T("Client-ID de Imgur")); snprintf(value, n, "%s", g_cfg.imgur_id[0] ? g_cfg.imgur_id : T("(sin configurar)")); break;
	case S_JOINS:    strcpy(label, T("Mostrar entradas/salidas")); snprintf(value, n, "%s", g_cfg.show_joins ? T("Sí") : "No"); break;
	case S_AWAKE:    strcpy(label, T("Evitar suspensión conectado")); snprintf(value, n, "%s", g_cfg.keep_awake ? T("Sí") : "No"); break;
	case S_CPU:      strcpy(label, T("CPU (menos = más batería)")); snprintf(value, n, "< %d MHz >", g_cfg.cpu_mhz); break;
	case S_FONT:     strcpy(label, T("Tamaño de texto")); snprintf(value, n, "< %d%% >", g_cfg.font_pct); break;
	case S_UILANG:
		strcpy(label, T("Idioma de la interfaz"));
		snprintf(value, n, "< %s >", g_cfg.ui_lang == 1 ? "Español" : g_cfg.ui_lang == 2 ? "English" : T("Automático"));
		break;
	case S_ABOUT:    strcpy(label, T("Acerca de")); snprintf(value, n, T("VitaIRC %s  ·  código abierto"), APP_VERSION); break;
	}
}

static void ui_lang_apply(void)
{
	if (g_cfg.ui_lang == 1) {
		g_ui_en = 0;
	} else if (g_cfg.ui_lang == 2) {
		g_ui_en = 1;
	} else {
		int lang = SCE_SYSTEM_PARAM_LANG_SPANISH;
		sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
		g_ui_en = lang != SCE_SYSTEM_PARAM_LANG_SPANISH;
	}
}

static void apply_cpu(void)
{
	scePowerSetArmClockFrequency(g_cfg.cpu_mhz);
	scePowerSetBusClockFrequency(g_cfg.cpu_mhz >= 333 ? 222 : 166);
	scePowerSetGpuClockFrequency(g_cfg.cpu_mhz >= 333 ? 222 : 111);
}

static void invalidate_layouts(void)
{
	pthread_mutex_lock(&g_lock);
	for (int i = 0; i < g_nservers; i++)
		for (int k = 0; k < g_servers[i].nchans; k++) {
			Chan *c = g_servers[i].chans[k];
			for (int j = 0; j < c->count; j++)
				irc_msg_at(c, j)->lay_w = 0;
		}
	pthread_mutex_unlock(&g_lock);
}

static void settings_input(void)
{
	int act = list_nav(S_COUNT);
	int f = list_sel;
	int d = REPEAT(SCE_CTRL_LEFT) ? -1 : REPEAT(SCE_CTRL_RIGHT) ? 1 : 0;
	if (!d && (PRESSED(btn_ok) || act) && (f == S_LANG_IN || f == S_LANG_OUT || f == S_CPU || f == S_FONT || f == S_UILANG))
		d = 1;
	int changed = 0;
	if (d) {
		if (f == S_LANG_IN) {
			strcpy(g_cfg.lang_in, g_lang_codes[(lang_index(g_cfg.lang_in) + d + g_lang_count) % g_lang_count]);
			changed = 1;
		} else if (f == S_LANG_OUT) {
			strcpy(g_cfg.lang_out, g_lang_codes[(lang_index(g_cfg.lang_out) + d + g_lang_count) % g_lang_count]);
			changed = 1;
		} else if (f == S_CPU) {
			static const int mhz[] = { 222, 333, 444 };
			int i = g_cfg.cpu_mhz == 222 ? 0 : g_cfg.cpu_mhz == 333 ? 1 : 2;
			g_cfg.cpu_mhz = mhz[(i + d + 3) % 3];
			apply_cpu();
			changed = 1;
		} else if (f == S_UILANG) {
			g_cfg.ui_lang = (g_cfg.ui_lang + d + 3) % 3;
			ui_lang_apply();
			changed = 1;
		} else if (f == S_FONT) {
			static const int pct[] = { 85, 100, 115, 130 };
			int i = 1;
			for (int k = 0; k < 4; k++) if (pct[k] == g_cfg.font_pct) i = k;
			g_cfg.font_pct = pct[(i + d + 4) % 4];
			font_setup();
			invalidate_layouts();
			changed = 1;
		}
	}
	if (PRESSED(btn_ok) || act) {
		switch (f) {
		case S_KEY:   ime.field = g_cfg.openai_key; ime.fieldlen = sizeof(g_cfg.openai_key); ime_open(IME_FIELD, T("API key de OpenAI (sk-...)"), g_cfg.openai_key, 250, 0, 0); break;
		case S_MODEL: ime.field = g_cfg.openai_model; ime.fieldlen = sizeof(g_cfg.openai_model); ime_open(IME_FIELD, T("Modelo (ej: gpt-4o-mini)"), g_cfg.openai_model, 46, 0, 0); break;
		case S_IMGUR: ime.field = g_cfg.imgur_id; ime.fieldlen = sizeof(g_cfg.imgur_id); ime_open(IME_FIELD, T("Client-ID de Imgur"), g_cfg.imgur_id, 60, 0, 0); break;
		case S_JOINS: g_cfg.show_joins = !g_cfg.show_joins; changed = 1; break;
		case S_SOUND: g_cfg.sound = !g_cfg.sound; changed = 1; if (g_cfg.sound) g_beep_req = 1; break;
		case S_COLORS: g_cfg.irc_colors = !g_cfg.irc_colors; changed = 1; break;
		case S_HIGHLIGHT: ime.field = g_cfg.highlight; ime.fieldlen = sizeof(g_cfg.highlight); ime_open(IME_FIELD, T("Palabras separadas por coma (ej: vita,henkaku)"), g_cfg.highlight, 250, 0, 0); break;
		case S_IGNORE: ime.field = g_cfg.ignore; ime.fieldlen = sizeof(g_cfg.ignore); ime_open(IME_FIELD, T("Nicks separados por coma (se permite *)"), g_cfg.ignore, 500, 0, 0); break;
		case S_AWAKE: g_cfg.keep_awake = !g_cfg.keep_awake; changed = 1; break;
		}
	}
	if (changed)
		config_save();
	if (PRESSED(btn_back))
		screen = SCR_CHAT;
}

static void draw_settings_screen(void)
{
	draw_form(T("Ajustes"), T("se guardan en ux0:data/VitaIRC/config.ini"), S_COUNT, settings_label, 0);
	const int g[] = { glyph_ok, G_DPAD, glyph_back };
	const char *l[] = { T("Editar / cambiar"), T("Cambiar opción"), T("Volver") };
	draw_bottombar_hints(3, g, l);
}

/* ------------------------------------------------------------------ */
/* Users / channel list                                               */
/* ------------------------------------------------------------------ */
static void users_snapshot(void)
{
	pthread_mutex_lock(&g_lock);
	Chan *c = cur_chan();
	free(ul_users);
	ul_users = NULL;
	ul_n = 0;
	if (c && c->nusers) {
		ul_users = malloc(c->nusers * sizeof(ChanUser));
		if (ul_users) {
			memcpy(ul_users, c->users, c->nusers * sizeof(ChanUser));
			ul_n = c->nusers;
		}
	}
	pthread_mutex_unlock(&g_lock);
}

static void users_input(void)
{
	int act = list_nav(ul_n);
	Server *s = cur_server();
	Chan *c = cur_chan();
	if (PRESSED(btn_back) || !s) {
		screen = SCR_CHAT;
		return;
	}
	if (!ul_n)
		return;
	const char *nick = ul_users[list_sel].nick;
	char cmd[80];
	if (PRESSED(btn_ok) || act) {
		open_user_menu(nick);
	} else if (PRESSED(SCE_CTRL_SQUARE)) {
		snprintf(cmd, sizeof(cmd), "/whois %s", nick);
		irc_user_input(s->idx, c ? c->uid : 0, cmd);
		screen = SCR_CHAT;
	} else if (PRESSED(SCE_CTRL_TRIANGLE)) {
		snprintf(cmd, sizeof(cmd), "%s: ", nick);
		screen = SCR_CHAT;
		open_chat_ime(cmd);
	}
}

static void draw_users_screen(void)
{
	Chan *c = cur_chan();
	vita2d_draw_rectangle(0, TOP_H, SCR_W, SCR_H - TOP_H, C_BG);
	char sub[64];
	snprintf(sub, sizeof(sub), T("%d usuarios"), ul_n);
	draw_topbar(c ? c->name : T("Usuarios"), sub);
	int rh = lh + 10;
	list_clamp(ul_n, rh);
	int vis = list_rows_visible(rh);
	for (int i = list_top; i < ul_n && i < list_top + vis; i++) {
		int y = TOP_H + 8 + (i - list_top) * rh;
		draw_list_row(i, y, rh, i == list_sel);
		char p[2] = { ul_users[i].prefix, 0 };
		draw_text(30, y + 4, C_ACCENT, p);
		draw_text(50, y + 4, ul_users[i].away ? C_FAINT : nick_color(ul_users[i].nick), ul_users[i].nick);
		if (ul_users[i].away)
			draw_text(60 + text_w(ul_users[i].nick), y + 4, C_FAINT, T("(ausente)"));
	}
	const int g[] = { glyph_ok, G_SQUARE, G_TRIANGLE, glyph_back };
	const char *l[] = { T("Opciones"), "WHOIS", T("Mencionar"), T("Volver") };
	draw_bottombar_hints(4, g, l);
}

static int listed_cmp(const void *a, const void *b)
{
	return ((const ListedChan *)b)->users - ((const ListedChan *)a)->users;
}

static void chanlist_input(void)
{
	Server *s = cur_server();
	if (PRESSED(btn_back) || !s) {
		screen = SCR_CHAT;
		return;
	}
	pthread_mutex_lock(&g_lock);
	int n = s->nlisted;
	int act = list_nav(n);
	char name[64] = "";
	if (n && list_sel < n)
		str_copy(name, s->listed[list_sel].name, sizeof(name));
	pthread_mutex_unlock(&g_lock);
	Chan *c = cur_chan();
	if ((PRESSED(btn_ok) || act) && name[0]) {
		char cmd[80];
		snprintf(cmd, sizeof(cmd), "/join %s", name);
		irc_user_input(s->idx, c ? c->uid : 0, cmd);
		screen = SCR_CHAT;
	} else if (PRESSED(SCE_CTRL_SQUARE)) {
		irc_user_input(s->idx, c ? c->uid : 0, "/list");
		list_sel = list_top = 0;
	}
}

static void draw_chanlist_screen(void)
{
	Server *s = cur_server();
	vita2d_draw_rectangle(0, TOP_H, SCR_W, SCR_H - TOP_H, C_BG);
	char sub[96];
	if (!s) return;
	static int sorted_count = -1;
	if (s->listing == 2 && sorted_count != s->nlisted) {
		qsort(s->listed, s->nlisted, sizeof(ListedChan), listed_cmp);
		sorted_count = s->nlisted;
	}
	if (s->listing != 2)
		sorted_count = -1;
	snprintf(sub, sizeof(sub), T("%s  ·  %d canales%s"), g_cfg.servers[s->idx].name, s->nlisted,
	         s->listing == 1 ? T("  ·  cargando...") : "");
	draw_topbar(T("Canales"), sub);
	int rh = lh + 10;
	list_clamp(s->nlisted, rh);
	int vis = list_rows_visible(rh);
	for (int i = list_top; i < s->nlisted && i < list_top + vis; i++) {
		ListedChan *e = &s->listed[i];
		int y = TOP_H + 8 + (i - list_top) * rh;
		draw_list_row(i, y, rh, i == list_sel);
		draw_text_fit(30, y + 4, 230, C_CHAN, e->name);
		char u[16];
		snprintf(u, sizeof(u), "%d", e->users);
		draw_text(270, y + 4, C_ACCENT, u);
		draw_text_fit(340, y + 4, SCR_W - 370, C_DIM, e->topic);
	}
	if (!s->nlisted)
		draw_text(30, TOP_H + 20, C_DIM, s->state == SS_ONLINE ? T("Esperando la lista del servidor...") : T("Conéctate primero al servidor."));
	const int g[] = { glyph_ok, G_SQUARE, glyph_back };
	const char *l[] = { T("Unirse"), T("Actualizar"), T("Volver") };
	draw_bottombar_hints(3, g, l);
}

/* ------------------------------------------------------------------ */
/* File browser (Imgur upload)                                        */
/* ------------------------------------------------------------------ */
static int is_image(const char *n)
{
	const char *e = strrchr(n, '.');
	return e && (!strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg") || !strcasecmp(e, ".png") || !strcasecmp(e, ".gif"));
}

static int fe_cmp(const void *a, const void *b)
{
	const FileEnt *x = a, *y = b;
	if (x->dir != y->dir) return y->dir - x->dir;
	return -str_icmp(x->name, y->name);   /* newest-looking names (dates) first */
}

static void fb_load(void)
{
	free(fb_ents);
	fb_ents = NULL;
	fb_n = 0;
	int cap = 0;
	SceUID d = sceIoDopen(fb_path);
	if (d < 0 && strcmp(fb_path, "ux0:")) {
		strcpy(fb_path, "ux0:");
		d = sceIoDopen(fb_path);
	}
	if (d >= 0) {
		SceIoDirent e;
		while (sceIoDread(d, &e) > 0) {
			int dir = SCE_S_ISDIR(e.d_stat.st_mode);
			if (!dir && !is_image(e.d_name))
				continue;
			if (fb_n == cap) {
				cap = cap ? cap * 2 : 64;
				FileEnt *n = realloc(fb_ents, cap * sizeof(FileEnt));
				if (!n) break;
				fb_ents = n;
			}
			str_copy(fb_ents[fb_n].name, e.d_name, sizeof(fb_ents[0].name));
			fb_ents[fb_n].dir = dir;
			fb_n++;
		}
		sceIoDclose(d);
	}
	if (fb_n)
		qsort(fb_ents, fb_n, sizeof(FileEnt), fe_cmp);
	list_sel = list_top = 0;
}

static void fb_up(void)
{
	size_t n = strlen(fb_path);
	if (n && fb_path[n - 1] == '/')
		fb_path[--n] = 0;
	char *slash = strrchr(fb_path, '/');
	char *colon = strchr(fb_path, ':');
	if (slash && slash > colon)
		slash[1] = 0;
	else if (colon)
		colon[1] = 0;
	fb_load();
}

static void files_input(void)
{
	int act = list_nav(fb_n);
	if (PRESSED(btn_back)) {
		if (!strcmp(fb_path, "ux0:"))
			screen = SCR_CHAT;
		else
			fb_up();
		return;
	}
	if (PRESSED(SCE_CTRL_TRIANGLE)) {
		screen = SCR_CHAT;
		return;
	}
	if (!(PRESSED(btn_ok) || act) || !fb_n)
		return;
	FileEnt *e = &fb_ents[list_sel];
	char path[800];
	size_t n = strlen(fb_path);
	snprintf(path, sizeof(path), "%s%s%s", fb_path, (n && fb_path[n - 1] != '/' && fb_path[n - 1] != ':') ? "/" : "", e->name);
	if (e->dir) {
		snprintf(fb_path, sizeof(fb_path), "%s/", path);
		fb_load();
	} else {
		open_viewer_file(path);
	}
}

static void draw_files_screen(void)
{
	vita2d_draw_rectangle(0, TOP_H, SCR_W, SCR_H - TOP_H, C_BG);
	draw_topbar(T("Subir imagen a Imgur"), fb_path);
	int rh = lh + 10;
	list_clamp(fb_n, rh);
	int vis = list_rows_visible(rh);
	for (int i = list_top; i < fb_n && i < list_top + vis; i++) {
		int y = TOP_H + 8 + (i - list_top) * rh;
		draw_list_row(i, y, rh, i == list_sel);
		draw_text(30, y + 4, fb_ents[i].dir ? C_ACCENT : C_TEXT, fb_ents[i].dir ? T("[carpeta]") : "");
		draw_text_fit(fb_ents[i].dir ? 30 + text_w(T("[carpeta] ")) : 30, y + 4, SCR_W - 80,
		              fb_ents[i].dir ? C_ACCENT : C_TEXT, fb_ents[i].name);
	}
	if (!fb_n)
		draw_text(30, TOP_H + 20, C_DIM, T("No hay imágenes (jpg/png/gif) en esta carpeta."));
	const int g[] = { glyph_ok, glyph_back, G_TRIANGLE };
	const char *l[] = { T("Abrir / subir"), T("Subir de carpeta"), T("Cancelar") };
	draw_bottombar_hints(3, g, l);
}

/* ------------------------------------------------------------------ */
/* Help                                                               */
/* ------------------------------------------------------------------ */
static void draw_help_screen(void)
{
	vita2d_draw_rectangle(0, TOP_H, SCR_W, SCR_H - TOP_H, C_BG);
	draw_topbar(T("Ayuda"), T("controles y comandos"));
	static const char *lines[] = {
		"Botón confirmar: escribir mensaje (teclado en pantalla). También puedes tocar la barra inferior.",
		"Triángulo / START: menú.   Cuadrado: traducir mensajes recibidos con ChatGPT en este canal.",
		"L / R o izquierda / derecha: cambiar de canal.   Arriba / abajo o deslizar: desplazar mensajes.",
		"SELECT: servidores.   Toca un canal en la barra lateral para abrirlo.",
		"Botón volver: respuestas rápidas (nicks y mensajes enviados). Toca un mensaje para ver sus opciones.",
		"",
		"Comandos: /join #canal  /part  /msg nick texto  /query nick  /me acción  /nick nuevo",
		"/topic texto  /whois nick  /list  /notice  /quote RAW  /connect  /disconnect  /clear  /help",
		"",
		"Traducción: pon tu API key de OpenAI en Ajustes. Los recibidos se traducen al idioma elegido y",
		"\"Traducir mis mensajes\" envía lo que escribes traducido al idioma de salida.",
		"Imgur: crea un Client-ID en api.imgur.com/oauth2/addclient (tipo anónimo) y ponlo en Ajustes.",
		"Las capturas de la Vita están en ux0:picture/SCREENSHOT.",
		"Bouncer (soju/ZNC): agrégalo como servidor; los mensajes perdidos llegan con su hora real.",
		"Usuarios: confirmar abre opciones (privado, ignorar y, si eres operador, op, voz, kick y ban).",
		"",
		"Segundo plano: la Vita suspende las apps al apagar la pantalla con el botón POWER o al salir.",
		"Con \"Evitar suspensión\" activo, VitaIRC sigue conectado mientras esté abierta, y se",
		"reconecta solo al volver de la suspensión.",
	};
	int y = TOP_H + 14;
	for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
		draw_text_fit(24, y, SCR_W - 48, i < 5 ? C_TEXT : C_DIM, T(lines[i]));
		y += lh + 2;
	}
	const int g[] = { glyph_back };
	const char *l[] = { T("Volver") };
	draw_bottombar_hints(1, g, l);
}

/* ------------------------------------------------------------------ */
/* IME results                                                        */
/* ------------------------------------------------------------------ */
static void ime_apply(char *text);

static void ime_poll(void)
{
	if (!ime.active)
		return;
	if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
		return;
	SceImeDialogResult res;
	memset(&res, 0, sizeof(res));
	sceImeDialogGetResult(&res);
	char text[2048];
	utf16_to_utf8(ime.buf, text, sizeof(text));
	sceImeDialogTerm();
	ime.active = 0;
	g_dirty = 1;
	if (res.button != SCE_IME_DIALOG_BUTTON_ENTER) {
		if (ime.kind == IME_CHAT) {        /* keep what was typed */
			str_copy(draft, text, sizeof(draft));
			draft_sidx = ime.sidx;
			draft_uid = ime.uid;
		}
		return;
	}

	pthread_mutex_lock(&g_lock);
	ime_apply(text);
	pthread_mutex_unlock(&g_lock);
}

static void ime_apply(char *text)
{
	char cmd[2200];
	switch (ime.kind) {
	case IME_CHAT:
		if (draft_sidx == ime.sidx && draft_uid == ime.uid)
			draft[0] = 0;
		if (text[0]) {
			/* remember it for T("Respuestas rápidas") */
			int dup = -1;
			for (int i = 0; i < sent_n; i++)
				if (!strcmp(sent_hist[i], text)) dup = i;
			int move = dup >= 0 ? dup : (sent_n < SENT_MAX ? sent_n++ : SENT_MAX - 1);
			memmove(sent_hist[1], sent_hist[0], move * sizeof(sent_hist[0]));
			str_copy(sent_hist[0], text, sizeof(sent_hist[0]));
			irc_user_input(ime.sidx, ime.uid, text);
			pthread_mutex_lock(&g_lock);
			Chan *c = irc_find_chan_uid(&g_servers[ime.sidx], ime.uid);
			if (c) c->scroll = 0;
			pthread_mutex_unlock(&g_lock);
		}
		break;
	case IME_JOIN:
		str_trim(text);
		if (text[0] && strcmp(text, "#")) {
			snprintf(cmd, sizeof(cmd), "/join %s", text);
			irc_user_input(ime.sidx, 0, cmd);
		}
		break;
	case IME_PM:
		str_trim(text);
		if (text[0]) {
			snprintf(cmd, sizeof(cmd), "/query %s", text);
			irc_user_input(ime.sidx, 0, cmd);
		}
		break;
	case IME_NICK:
		str_trim(text);
		if (text[0]) {
			snprintf(cmd, sizeof(cmd), "/nick %s", text);
			irc_user_input(ime.sidx, 0, cmd);
		}
		break;
	case IME_TOPIC:
		snprintf(cmd, sizeof(cmd), "/topic %s", text);
		irc_user_input(ime.sidx, ime.uid, cmd);
		break;
	case IME_FIELD:
		str_trim(text);
		str_copy(ime.field, text, ime.fieldlen);
		if (screen == SCR_SETTINGS)
			config_save();
		break;
	case IME_AWAY:
		snprintf(cmd, sizeof(cmd), "/away %s", text[0] ? text : T("Ausente"));
		irc_user_input(ime.sidx, 0, cmd);
		break;
	case IME_SEARCH:
		str_trim(text);
		search_results(text);
		break;
	case IME_KICK:
		snprintf(cmd, sizeof(cmd), "/kick %s %s", u_nick, text);
		irc_user_input(ime.sidx, ime.uid, cmd);
		screen = SCR_CHAT;
		break;
	case IME_FIELD_INT:
		if (text[0])
			*ime.intfield = atoi(text);
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Demo / screenshot mode (development builds only: -DVITAIRC_DEMO=ON) */
/* ------------------------------------------------------------------ */
#ifdef VITAIRC_DEMO
static int      demo_step;
static uint64_t demo_start;
static char     demo_shot[32];

static Msg *demo_msg(Server *s, Chan *c, int type, const char *nick, const char *text, const char *trans)
{
	Msg *m = irc_add_msg(s, c, type, nick, text, nick && !strcmp(nick, s->nick));
	if (trans) {
		m->trans = str_dup(trans);
		m->trans_state = TR_DONE;
	}
	return m;
}

static void demo_fill(void)
{
	Server *s = &g_servers[0];
	if (!s->nick[0])
		strcpy(s->nick, s->cfg.nick);
	Chan *c = irc_get_chan(s, "#vitasdk", CH_CHANNEL);
	c->joined = 1;
	str_copy(c->topic, "VitaSDK - homebrew toolchain for PS Vita | https://vitasdk.org", sizeof(c->topic));
	static const char *nicks[] = { "@kuro", "@lumi", "+pixel", "bytebard", "neko_dev", "tako", "retro_dan", "vitamin", "sdkuser", "henkaku_fan" };
	for (unsigned i = 0; i < sizeof(nicks) / sizeof(nicks[0]); i++) {
		char n[40];
		str_copy(n, nicks[i][0] == '@' || nicks[i][0] == '+' ? nicks[i] + 1 : nicks[i], sizeof(n));
		if (c->nusers == c->cap_users) {
			c->cap_users = c->cap_users ? c->cap_users * 2 : 32;
			c->users = realloc(c->users, c->cap_users * sizeof(ChanUser));
		}
		str_copy(c->users[c->nusers].nick, n, sizeof(c->users[0].nick));
		c->users[c->nusers].prefix = (nicks[i][0] == '@' || nicks[i][0] == '+') ? nicks[i][0] : 0;
		c->nusers++;
	}
	irc_sort_users(s, c);
	if (!g_cfg.imgur_id[0])
		strcpy(g_cfg.imgur_id, "demo");
	strcpy(g_cfg.highlight, "vitachat,henkaku");
	irc_add_msg(s, c, MT_JOIN, NULL, T("Te uniste al canal"), 0);
	{
		char tb[160];
		snprintf(tb, sizeof(tb), T("Tema: %s"), "VitaSDK - homebrew toolchain for PS Vita | https://vitasdk.org");
		irc_add_msg(s, c, MT_TOPIC, NULL, tb, 0);
	}
#ifdef VITAIRC_DEMO_EN
	demo_msg(s, c, MT_MSG, "lumi", "qualcuno ha provato la nuova build di vitaGL con l'ultimo SDK?", "has anyone tried the new vitaGL build with the latest SDK?");
	demo_msg(s, c, MT_MSG, "retro_dan", "sí, aquí funciona bien. recuerda actualizar los paquetes con vdpm primero", "yes, it works fine here. remember to update the packages with vdpm first");
	irc_add_msg(s, c, MT_JOIN, NULL, "pixel joined", 0);
	demo_msg(s, c, MT_ACTION, "neko_dev", "está portando un nuevo emulador a la Vita", "is porting a new emulator to the Vita");
	demo_msg(s, c, MT_MSG, s->nick, "hi! I'm testing VitaIRC on my Vita, a native IRC client", NULL);
	Msg *m = demo_msg(s, c, MT_MSG, "tako", "bem-vindo! parece ótimo, é open source?", "welcome! looks great, is it open source?");
	m->highlight = 1;
	demo_msg(s, c, MT_MSG, s->nick, "yes, MIT licensed. it's going up on GitHub soon", NULL);
#else
	demo_msg(s, c, MT_MSG, "lumi", "anyone tried the new vitaGL build with the latest SDK snapshot?", "¿alguien probó el nuevo build de vitaGL con el último snapshot del SDK?");
	demo_msg(s, c, MT_MSG, "retro_dan", "yes, works fine here. remember to run vdpm to update the packages first", "sí, aquí funciona bien. recuerda ejecutar vdpm para actualizar los paquetes primero");
	irc_add_msg(s, c, MT_JOIN, NULL, "pixel se unió", 0);
	demo_msg(s, c, MT_ACTION, "neko_dev", "is porting a new emulator to the Vita", "está portando un nuevo emulador a la Vita");
	demo_msg(s, c, MT_MSG, s->nick, "hola! estoy probando VitaIRC desde mi Vita, un cliente IRC nativo", NULL);
	Msg *m = demo_msg(s, c, MT_MSG, "tako", "welcome! that looks nice, is it open source?", "¡bienvenido! se ve bien, ¿es de código abierto?");
	m->highlight = 1;
	demo_msg(s, c, MT_MSG, s->nick, "sí, MIT. lo subiré pronto a GitHub", NULL);
#endif
	demo_msg(s, c, MT_MSG, "bytebard", "nice work! here's my setup: https://i.imgur.com/V1taIRC.png", NULL);
	demo_msg(s, c, MT_MSG, "kuro", "\x02" "release" "\x02" ": \x03" "09VitaChat 1.2\x03 is out, \x1f" "update now\x0f" " \x03" "04,01 HOT \x03", NULL);

	c->trans_in = 1;
	Chan *q = irc_get_chan(s, "kuro", CH_QUERY);
	irc_add_msg(s, q, MT_MSG, "kuro", "hey, cool client!", 0);
	q->unread = 1;
	q->mention = 1;
	Chan *h = irc_get_chan(s, "#henkaku", CH_CHANNEL);
	h->joined = 1;
	irc_add_msg(s, h, MT_MSG, "vitamin", "new enso build is out", 0);
	h->unread = 7;
	Chan *p = irc_get_chan(s, "#psvita", CH_CHANNEL);
	p->joined = 1;
	irc_add_msg(s, p, MT_MSG, "someone", "hi", 0);
	p->unread = 23;
	set_view(0, c->uid);
	c->unread = 0;
}

static void demo_tick(void)
{
	uint64_t t = now_ms() - demo_start;
	static uint64_t step_at;
	if (demo_shot[0] || t < 8000)
		return;
	if (screen == SCR_IMAGE && iv_loading && now_ms() - step_at < 15000)
		return;
	menu_open = 0;
	confirm_open = 0;
	Server *s = &g_servers[0];
	Chan *c = irc_find_chan(s, "#vitasdk");
	switch (demo_step) {
	case 0: strcpy(demo_shot, "01_status"); break;
	case 1:
		demo_fill();
		c = irc_find_chan(s, "#vitasdk");
#ifdef VITAIRC_DEMO_EN
		str_copy(sent_hist[0], "yes, MIT licensed. it's going up on GitHub soon", sizeof(sent_hist[0]));
		str_copy(sent_hist[1], "hi! I'm testing VitaIRC on my Vita", sizeof(sent_hist[1]));
		strcpy(g_cfg.lang_in, "en");
		strcpy(g_cfg.lang_out, "es");
#else
		str_copy(sent_hist[0], "sí, MIT. lo subiré pronto a GitHub", sizeof(sent_hist[0]));
		str_copy(sent_hist[1], "hola! estoy probando VitaIRC desde mi Vita", sizeof(sent_hist[1]));
#endif
		sent_n = 2;
		screen = SCR_CHAT;
		strcpy(demo_shot, "02_chat");
		break;
	case 2: open_menu(); strcpy(demo_shot, "03_menu"); break;
	case 3: open_quick(); strcpy(demo_shot, "04_quick"); break;
	case 4:
		if (c)
			for (int i = c->count - 1; i >= 0; i--)
				if (!strcmp(irc_msg_at(c, i)->nick, "bytebard")) {
					open_msg_menu(c, irc_msg_at(c, i)->id);
					break;
				}
		strcpy(demo_shot, "05_message");
		break;
	case 5: open_viewer_file("app0:sce_sys/livearea/contents/bg.png"); step_at = now_ms(); break;
	case 6: strcpy(demo_shot, "06_viewer"); break;
	case 7: iv_free(); screen = SCR_CHAT; open_sub_trans(); strcpy(demo_shot, "07_translation"); break;
	case 8: search_results("vita"); strcpy(demo_shot, "08_search"); break;
	case 9: screen = SCR_CHAT; jump_to(c, (uint32_t)menu[1].arg); strcpy(demo_shot, "09_jump"); break;
	case 10:
		if (c) {
			int found = 0;
			for (int i = 0; i < c->nusers; i++)
				if (!str_icmp(c->users[i].nick, s->nick)) { c->users[i].prefix = '@'; found = 1; }
			if (!found && c->nusers < c->cap_users) {
				str_copy(c->users[c->nusers].nick, s->nick, sizeof(c->users[0].nick));
				c->users[c->nusers++].prefix = '@';
			}
			if (c->nusers > 2)
				c->users[c->nusers - 1].away = 1;
			irc_sort_users(s, c);
		}
		users_snapshot(); screen = SCR_USERS; list_sel = 3; open_user_menu(ul_users[3].nick); strcpy(demo_shot, "10_user_ops"); break;
	case 11: screen = SCR_SERVERS; list_sel = 0; open_presets(); strcpy(demo_shot, "11_presets"); break;
	case 12: screen = SCR_CHAT; do_action(A_CAMERA); strcpy(demo_shot, "12_camera"); break;
	case 13: cam_close(); screen = SCR_SETTINGS; list_sel = S_HIGHLIGHT; strcpy(demo_shot, "13_settings"); break;
	case 14: screen = SCR_HELP; strcpy(demo_shot, "14_help"); break;
#ifdef VITAIRC_DEMO_EN
	case 15: g_ui_en = 0; screen = SCR_CHAT; strcpy(demo_shot, "15_chat_es"); break;
	case 16: open_menu(); strcpy(demo_shot, "16_menu_es"); break;
#else
	case 15: g_ui_en = 1; screen = SCR_CHAT; strcpy(demo_shot, "15_chat_en"); break;
	case 16: open_menu(); strcpy(demo_shot, "16_menu_en"); break;
#endif
	case 17: screen = SCR_CHAT; ask_confirm(T("¿Salir de VitaIRC? Se cerrarán las conexiones."), A_EXIT, 0); strcpy(demo_shot, "17_confirm"); break;
	case 18: app_running = 0; break;
	}
	demo_step++;
	g_dirty = 1;
}

/* Writes a marker so a host script can screenshot the emulator window,
 * then holds the screen for a moment. */
static void demo_capture(void)
{
	static uint64_t since;
	if (!demo_shot[0])
		return;
	g_dirty = 1;
	if (!since) {
		since = now_ms();
		FILE *f = fopen(DATA_DIR "/demo_step.txt", "w");
		if (f) {
			fprintf(f, "%s\n", demo_shot);
			fclose(f);
		}
	}
	if (now_ms() - since < 3000)
		return;
	since = 0;
	demo_shot[0] = 0;
}
#endif

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */
static void system_init(void)
{
	sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
	static char net_mem[1 * 1024 * 1024];
	SceNetInitParam np;
	np.memory = net_mem;
	np.size = sizeof(net_mem);
	np.flags = 0;
	sceNetInit(&np);
	sceNetCtlInit();

	SceAppUtilInitParam ip;
	SceAppUtilBootParam bp;
	memset(&ip, 0, sizeof(ip));
	memset(&bp, 0, sizeof(bp));
	sceAppUtilInit(&ip, &bp);

	SceCommonDialogConfigParam cp;
	sceCommonDialogConfigParamInit(&cp);
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, (int *)&cp.language);
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, (int *)&cp.enterButtonAssign);
	sceCommonDialogSetConfigParam(&cp);
	if (cp.enterButtonAssign == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) {
		btn_ok = SCE_CTRL_CIRCLE;
		btn_back = SCE_CTRL_CROSS;
		glyph_ok = G_CIRCLE;
		glyph_back = G_CROSS;
	}

	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
}

static void check_wifi(void)
{
	int st = 0;
	if (sceNetCtlInetGetState(&st) >= 0)
		wifi_ok = (st == SCE_NETCTL_STATE_CONNECTED);
}

int main(void)
{
	system_init();
	config_load();
	ui_lang_apply();
#ifdef VITAIRC_DEMO
	g_ui_en = 0;   /* screenshots start in Spanish, then switch to English */
#endif
#ifdef VITAIRC_DEMO_EN
	g_ui_en = 1;   /* English screenshots, then Spanish */
#endif
	apply_cpu();

	vita2d_init();
	vita2d_set_clear_color(C_BG);
	vita2d_set_vblank_wait(1);
	font = vita2d_load_default_pgf();
	font_setup();

	conn_global_init();
	irc_init();
	irc_sync_from_config();
#ifdef VITAIRC_DEMO
	/* screenshots use made-up data only: no real channels or old windows */
	for (int i = 0; i < g_cfg.nservers; i++)
		g_servers[i].cfg.channels[0] = g_cfg.servers[i].channels[0] = 0;
#else
	pthread_mutex_lock(&g_lock);
	session_restore();
	pthread_mutex_unlock(&g_lock);
#endif
	net_init();
	sound_init();
	check_wifi();

	for (int i = 0; i < g_cfg.nservers; i++)
		if (g_cfg.servers[i].autoconnect && !g_cfg.servers[i].deleted)
			irc_connect(i);
	ensure_valid_view();

	last_frame_ms = last_tick_ms = now_ms();
#ifdef VITAIRC_DEMO
	demo_start = now_ms();
#endif
	int last_minute = -1;

	while (app_running) {
		uint64_t now = now_ms();
		/* a long gap means the console was suspended: check the links */
		if (now - last_frame_ms > 4000) {
			irc_resume_check();
			check_wifi();
			g_dirty = 1;
		}
		last_frame_ms = now;
		if (now - last_tick_ms > 5000) {
			last_tick_ms = now;
			check_wifi();
			int any_online = 0;
			for (int i = 0; i < g_nservers; i++)
				if (g_servers[i].want_connect)
					any_online = 1;
			if (g_cfg.keep_awake && any_online)
				sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
		}

		input_read();
		touch_read();
		int activity = pad_now || pad_prev || touch_down || touch_was_down;

		pthread_mutex_lock(&g_lock);
#ifdef VITAIRC_DEMO
		demo_tick();
#endif
		if (!ime.active) {
			if (confirm_open)
				confirm_input();
			else if (menu_open)
				menu_input();
			else switch (screen) {
			case SCR_CHAT:     chat_input(); break;
			case SCR_SERVERS:  servers_input(); break;
			case SCR_EDIT:     edit_input(); break;
			case SCR_SETTINGS: settings_input(); break;
			case SCR_USERS:    users_input(); break;
			case SCR_CHANLIST: chanlist_input(); break;
			case SCR_FILES:    files_input(); break;
			case SCR_HELP:     if (PRESSED(btn_back) || PRESSED(btn_ok) || touch_tap) screen = SCR_CHAT; break;
			case SCR_IMAGE:    image_input(); break;
			case SCR_CAMERA:   camera_input(); break;
			}
		}
		if (g_invite_pending && !confirm_open && !menu_open && !ime.active) {
			g_invite_pending = 0;
			char b[160];
			snprintf(b, sizeof(b), T("Te invitaron a %s. ¿Unirte?"), g_invite_chan);
			ask_confirm(b, A_INVITE_JOIN, g_invite_sidx);
		}
		if (g_focus_req) {
			g_focus_req = 0;
			set_view(g_focus_sidx, g_focus_uid);
			screen = SCR_CHAT;
		}
		{
			Chan *c = cur_chan();
			if (c && screen == SCR_CHAT && (c->unread || c->mention)) {
				c->unread = 0;
				c->mention = 0;
			}
		}
		ensure_valid_view();
		if (g_fetch_done) {
			if (screen == SCR_IMAGE && iv_loading) {
				iv_loading = 0;
				if (g_fetch_done == 1)
					iv_decode(g_fetch_buf, g_fetch_len);
				else
					snprintf(iv_err, sizeof(iv_err), T("No se pudo descargar: %s"), g_fetch_err);
			}
			free(g_fetch_buf);
			g_fetch_buf = NULL;
			g_fetch_done = 0;
		}
		int upload = g_upload_done;
		char upload_text[256];
		str_copy(upload_text, g_upload_result, sizeof(upload_text));
		int up_sidx = g_upload_sidx;
		uint32_t up_uid = g_upload_uid;
		g_upload_done = 0;
		pthread_mutex_unlock(&g_lock);

		ime_poll();
		if (upload == 1 && !ime.active) {
			ime.sidx = up_sidx;
			ime.uid = up_uid;
			ime_open(IME_CHAT, T("Imagen subida: agrega un texto o envía el enlace"), upload_text, 450, 0, 0);
		} else if (upload == -1) {
			ui_toast(T("Error al subir: %s"), upload_text);
		}

		int minute = (int)(now / 60000);
		int toast_live = g_toast[0] && now <= g_toast_until + 100;
		if (!(g_dirty || activity || ime.active || toast_live || minute != last_minute)) {
			sceKernelDelayThread(16 * 1000);
			continue;
		}
		last_minute = minute;
		g_dirty = 0;

		vita2d_start_drawing();
		vita2d_clear_screen();
		pthread_mutex_lock(&g_lock);
		switch (screen) {
		case SCR_CHAT:     draw_chat_screen(); break;
		case SCR_SERVERS:  draw_servers_screen(); break;
		case SCR_EDIT:     draw_edit_screen(); break;
		case SCR_SETTINGS: draw_settings_screen(); break;
		case SCR_USERS:    draw_users_screen(); break;
		case SCR_CHANLIST: draw_chanlist_screen(); break;
		case SCR_FILES:    draw_files_screen(); break;
		case SCR_HELP:     draw_help_screen(); break;
		case SCR_IMAGE:    draw_image_screen(); break;
		case SCR_CAMERA:   draw_camera_screen(); break;
		}
		if (menu_open)
			draw_menu();
		if (confirm_open)
			draw_confirm();
		draw_toast();
		pthread_mutex_unlock(&g_lock);
		vita2d_end_drawing();
		vita2d_common_dialog_update();
		vita2d_swap_buffers();
#ifdef VITAIRC_DEMO
		demo_capture();
#endif
	}

	/* goodbye */
	irc_quit_all();
	uint64_t until = now_ms() + 1200;
	while (now_ms() < until) {
		int busy = 0;
		for (int i = 0; i < g_nservers; i++)
			if (g_servers[i].state != SS_OFF)
				busy = 1;
		if (!busy)
			break;
		sceKernelDelayThread(50 * 1000);
	}
	g_app_quit = 1;
	net_shutdown();
	vita2d_fini();
	vita2d_free_pgf(font);
	sceKernelExitProcess(0);
	return 0;
}
