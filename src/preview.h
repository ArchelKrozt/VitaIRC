#ifndef VITAIRC_PREVIEW_H
#define VITAIRC_PREVIEW_H

#include <stdint.h>
#include <vita2d.h>

/* Link previews in the chat: image thumbnails, video cards and page cards
 * (title, description and picture from the page's og: tags). A worker thread
 * fetches them; the UI shows the ones on screen. All calls need g_lock. */

enum { PK_IMAGE = 0, PK_VIDEO, PK_PAGE };
enum { PS_NONE = 0, PS_LOADING, PS_READY, PS_FAIL };

typedef struct {
	uint32_t hash;
	char     url[300];
	int      kind;          /* PK_* */
	int      state;         /* PS_* */
	int      gif;           /* image the app can't show inline */
	int      playable;      /* video the Vita can play (MP4) */
	int      video_site;    /* page of a video (YouTube...) */
	long     size;          /* video size in bytes, -1 unknown */
	char     title[160];
	char     site[64];
	char     desc[200];
	char     media[400];    /* direct image or video URL */
	unsigned char *rgba;    /* thumbnail from the worker, turned into tex by the UI */
	int      tw, th;
	vita2d_texture *tex;
	uint64_t used;          /* last time it was on screen */
} Preview;

void     pv_init(void);
/* Kind guessed from the URL alone (images and videos by extension). */
int      pv_kind_of(const char *url);
/* Cached preview of a link, or NULL (no request). */
Preview *pv_find(const char *url);
/* Cached preview, created and requested when missing. NULL when the cache is busy. */
Preview *pv_get(const char *url);
/* UI thread, before drawing: thumbnails -> textures, frees evicted textures.
 * Returns 1 when a preview changed size (layouts must be redone). */
int      pv_frame(void);
/* Direct picture for a link the viewer can open (PNG/JPG/WebP, imgur.com/<id>). */
int      pv_image_url(const char *url, char *direct, int n);
/* Direct link to a playable MP4 for a video link (Imgur .gifv...), else the link itself. */
void     pv_video_url(const char *url, char *out, int n);

#endif
