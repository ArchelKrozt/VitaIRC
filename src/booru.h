#ifndef VITAIRC_BOORU_H
#define VITAIRC_BOORU_H

#include <stdint.h>
#include <stddef.h>
#include <vita2d.h>

/* Image search ("booru" sites) with favorites. Network work runs on its own
 * thread; the lists below are shared with the UI under g_lock. */

enum { BE_SAFEBOORU = 0, BE_DANBOORU, BE_SANKAKU, BE_COUNT };

enum { TS_NONE = 0, TS_LOADING, TS_RAW, TS_READY, TS_ERROR };

typedef struct {
	int   engine;
	char  id[24];
	char  rating;              /* 'g' general, 's' sensitive, 'q' questionable, 'e' explicit */
	int   w, h;
	char  ext[8];
	long  file_size;
	char  thumb_url[512];
	char  view_url[512];       /* sample shown in the viewer */
	char  file_url[512];       /* original */
	char  tags[160];
	/* thumbnail, owned by the UI thread */
	vita2d_texture *tex;
	unsigned char  *raw;
	size_t          rawlen;
	int             tstate;    /* TS_* */
} BPost;

#define BOORU_MAX_RESULTS 240
#define BOORU_MAX_FAVS    300

/* search results and favorites (protected by g_lock) */
extern BPost *g_bres;
extern int    g_bres_n;
extern int    g_bres_more;       /* the service has more pages */
extern int    g_bres_loading;
extern char   g_bres_err[160];
extern int    g_bres_engine;
extern char   g_bres_query[128];
extern BPost *g_bfav;
extern int    g_bfav_n;

void booru_init(void);
const char *booru_engine_name(int e);

/* New search (clears the results). Caller holds g_lock. */
void booru_search(int engine, const char *query);
/* Next page of the current search, if any. Caller holds g_lock. */
void booru_more(void);
/* Downloads the thumbnail of result i (sets tstate/raw). Caller holds g_lock. */
void booru_thumb(int i);

/* Viewer download into g_fetch_* (see net.h). */
void booru_view(const BPost *p);
/* Link to the chat: direct for permanent links, re-uploaded otherwise.
 * The result arrives like an upload (g_upload_*). */
void booru_send(const BPost *p, int sidx, uint32_t uid);
/* Saves the original to ux0:picture/VitaIRC/ (WebP becomes JPEG). */
void booru_save(const BPost *p);

/* Favorites (UI thread, g_lock held). toggle returns 1 when it is now a favorite. */
int  booru_is_fav(int engine, const char *id);
int  booru_fav_toggle(const BPost *p, vita2d_texture *thumb);
void booru_fav_thumb_path(const BPost *p, char *out, int n);
/* Web page of a post (for "open in browser") */
void booru_page_url(const BPost *p, char *out, int n);

#endif
