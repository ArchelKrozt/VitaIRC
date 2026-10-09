#ifndef VITAIRC_VIDEO_H
#define VITAIRC_VIDEO_H

#include <stdint.h>
#include <vita2d.h>

/* MP4 player: the video is downloaded to the memory card, then played with
 * the system's hardware decoder (SceAvPlayer: H.264 + AAC). */

enum { VS_IDLE = 0, VS_DOWNLOADING, VS_PLAYING, VS_PAUSED, VS_ENDED, VS_ERROR };

void video_open(const char *url);
void video_close(void);
/* UI thread, once per frame: starts playback when the download is done. */
void video_update(void);
int  video_state(void);
int  video_progress(void);              /* download, 0-100 */
const char *video_error(void);
/* Latest frame (UI thread), NULL when there is none yet. */
vita2d_texture *video_frame(void);
void video_toggle_pause(void);
void video_seek(int seconds);
uint64_t video_time_ms(void);
uint64_t video_duration_ms(void);

#endif
