#ifndef VITAIRC_NET_H
#define VITAIRC_NET_H

#include <stdint.h>

/* HTTP worker: ChatGPT translations and Imgur uploads run here so the
 * UI and IRC threads never block on HTTPS. */
void net_init(void);
void net_shutdown(void);

/* Translate an incoming message (result stored in the Msg). */
void tr_request_in(int sidx, uint32_t chan_uid, uint32_t msg_id, const char *text, const char *lang);
/* Translate my message and send it to the channel when done. */
void tr_request_out(int sidx, uint32_t chan_uid, const char *text);
/* Upload an image to Imgur; on success the UI opens the keyboard with the link. */
void img_upload_request(int sidx, uint32_t chan_uid, const char *path);

/* Downloads an image for the in-app viewer. */
void img_fetch_request(const char *url);
/* Translates one message on demand, even if the channel has translation off. */
void tr_request_one(int sidx, uint32_t chan_uid, uint32_t msg_id, const char *text, const char *lang);

int  tr_should_skip(const char *text);

/* OpenAI usage counters (tokens), shown in Settings */
extern long g_usage_today, g_usage_total, g_usage_requests;

/* Status line shown by the UI while the worker is busy ("" when idle). */
extern char     g_net_busy[96];

/* Imgur result, consumed by the UI (protected by g_lock). */
extern int      g_upload_done;     /* 1 = ok, -1 = error */
extern char     g_upload_result[256];
extern int      g_upload_sidx;
extern uint32_t g_upload_uid;

/* Image download result, consumed by the UI (protected by g_lock). */
extern int            g_fetch_done;    /* 1 = ok, -1 = error */
extern unsigned char *g_fetch_buf;     /* owned by the UI once done */
extern size_t         g_fetch_len;
extern char           g_fetch_err[160];

#endif
