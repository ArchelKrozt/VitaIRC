#ifndef VITAIRC_HISTORY_H
#define VITAIRC_HISTORY_H

#include "irc.h"

#define HIST_LOAD_LINES 150

/* Set while history is being replayed into a window: no logging, no
 * unread counters, no translations. */
extern int g_loading_history;

void hist_log(Server *s, Chan *c, Msg *m);
void hist_load(Server *s, Chan *c);
void hist_close(Chan *c);

/* Open windows (and their translation toggles) survive app restarts. */
void session_save(void);
void session_restore(void);

#endif
