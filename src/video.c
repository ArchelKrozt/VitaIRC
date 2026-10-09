#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <pthread.h>
#include <curl/curl.h>
#include <psp2/avplayer.h>
#include <psp2/audioout.h>
#include <psp2/gxm.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "video.h"
#include "irc.h"
#include "config.h"
#include "util.h"
#include "i18n.h"

#define VIDEO_DIR   DATA_DIR "/cache"
#define VIDEO_PATH  VIDEO_DIR "/video.mp4"
#define VIDEO_MAX   (150LL * 1024 * 1024)

static volatile int state;
static volatile int progress;
static volatile int cancel;
static char         err[160];
static char         url[512];
static pthread_t    dl_thread, audio_thread;
static int          dl_running, audio_running;
static volatile int downloaded;          /* set by the download thread */

static SceAvPlayerHandle player;
static int               module_loaded;
static vita2d_texture    frame_tex;      /* points at the decoder's frame memory */
static int               have_frame;
static uint64_t          duration;

/* ---------------- download ---------------- */

static int on_progress(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	(void)ud; (void)ultotal; (void)ulnow;
	if (dltotal > VIDEO_MAX)
		return 1;
	if (dltotal > 0) {
		progress = (int)(dlnow * 100 / dltotal);
		g_dirty = 1;
	}
	return cancel || g_app_quit;
}

static size_t on_data(void *ptr, size_t size, size_t n, void *ud)
{
	return fwrite(ptr, size, n, (FILE *)ud);
}

static void *download(void *arg)
{
	(void)arg;
	sceIoMkdir(VIDEO_DIR, 0777);
	FILE *f = fopen(VIDEO_PATH, "wb");
	CURL *c = f ? curl_easy_init() : NULL;
	if (!c) {
		if (f) fclose(f);
		snprintf(err, sizeof(err), "%s", T("No se pudo guardar el archivo"));
		state = VS_ERROR;
		return NULL;
	}
	FILE *ca = fopen(CA_PATH, "r");
	if (ca) {
		fclose(ca);
		curl_easy_setopt(c, CURLOPT_CAINFO, CA_PATH);
	} else {
		curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
	}
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "VitaIRC/" APP_VERSION " (PlayStation Vita)");
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
	CURLcode rc = curl_easy_perform(c);
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	fclose(f);
	if (cancel)
		return NULL;
	if (rc == CURLE_ABORTED_BY_CALLBACK)
		snprintf(err, sizeof(err), T("El video pesa más de %d MB"), (int)(VIDEO_MAX / 1024 / 1024));
	else if (rc != CURLE_OK)
		snprintf(err, sizeof(err), T("Red: %s"), curl_easy_strerror(rc));
	else if (code >= 300)
		snprintf(err, sizeof(err), "HTTP %ld", code);
	if (rc != CURLE_OK || code >= 300) {
		state = VS_ERROR;
		g_dirty = 1;
		return NULL;
	}
	downloaded = 1;
	g_dirty = 1;
	return NULL;
}

/* ---------------- player ---------------- */

static void *mem_alloc(void *p, uint32_t align, uint32_t size)
{
	(void)p;
	return memalign(align, size);
}

static void mem_free(void *p, void *ptr)
{
	(void)p;
	free(ptr);
}

/* Video frames live in GPU-mapped memory so they can be drawn directly. */
static void *gpu_alloc(void *p, uint32_t align, uint32_t size)
{
	(void)p;
	(void)align;
	void *base = NULL;
	size = (size + 0x3FFFF) & ~0x3FFFF;
	SceUID blk = sceKernelAllocMemBlock("vitairc_video", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, NULL);
	if (blk < 0)
		return NULL;
	sceKernelGetMemBlockBase(blk, &base);
	if (sceGxmMapMemory(base, size, SCE_GXM_MEMORY_ATTRIB_RW) < 0) {
		sceKernelFreeMemBlock(blk);
		return NULL;
	}
	return base;
}

static void gpu_free(void *p, void *ptr)
{
	(void)p;
	SceUID blk = sceKernelFindMemBlockByAddr(ptr, 0);
	sceGxmUnmapMemory(ptr);
	if (blk >= 0)
		sceKernelFreeMemBlock(blk);
}

static void *audio_loop(void *arg)
{
	(void)arg;
	int port = -1, rate = 0, chans = 0, grain = 0;
	while (audio_running) {
		SceAvPlayerFrameInfo fr;
		if (state != VS_PLAYING || !sceAvPlayerIsActive(player) || !sceAvPlayerGetAudioData(player, &fr)) {
			sceKernelDelayThread(2000);
			continue;
		}
		int c = fr.details.audio.channelCount ? fr.details.audio.channelCount : 2;
		int samples = fr.details.audio.size / (2 * c);
		if (samples <= 0)
			continue;
		if (port < 0) {
			port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, samples, fr.details.audio.sampleRate,
			                           c == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
			rate = fr.details.audio.sampleRate;
			chans = c;
			grain = samples;
			if (port < 0)
				continue;
		} else if (rate != (int)fr.details.audio.sampleRate || chans != c || grain != samples) {
			sceAudioOutSetConfig(port, samples, fr.details.audio.sampleRate,
			                     c == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
			rate = fr.details.audio.sampleRate;
			chans = c;
			grain = samples;
		}
		sceAudioOutOutput(port, fr.pData);       /* blocks for the length of the frame */
	}
	if (port >= 0)
		sceAudioOutReleasePort(port);
	return NULL;
}

static void player_stop(void)
{
	if (audio_running) {
		audio_running = 0;
		pthread_join(audio_thread, NULL);
	}
	if (player) {
		vita2d_wait_rendering_done();
		sceAvPlayerStop(player);
		sceAvPlayerClose(player);
		player = 0;
	}
	have_frame = 0;
}

static int player_start(void)
{
	if (!module_loaded) {
		if (sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER) < 0) {
			snprintf(err, sizeof(err), "%s", T("El reproductor de video no está disponible"));
			return -1;
		}
		module_loaded = 1;
	}
	SceAvPlayerInitData init;
	memset(&init, 0, sizeof(init));
	init.memoryReplacement.allocate = mem_alloc;
	init.memoryReplacement.deallocate = mem_free;
	init.memoryReplacement.allocateTexture = gpu_alloc;
	init.memoryReplacement.deallocateTexture = gpu_free;
	init.basePriority = 0xA0;
	init.numOutputVideoFrameBuffers = 2;
	init.autoStart = 1;
	player = sceAvPlayerInit(&init);
	if (player <= 0 || sceAvPlayerAddSource(player, VIDEO_PATH) < 0) {
		player = 0;
		snprintf(err, sizeof(err), "%s", T("No se pudo abrir el video (la Vita reproduce MP4 H.264)"));
		return -1;
	}
	duration = 0;
	have_frame = 0;
	audio_running = 1;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 64 * 1024);
	pthread_create(&audio_thread, &attr, audio_loop, NULL);
	pthread_attr_destroy(&attr);
	state = VS_PLAYING;
	return 0;
}

/* ---------------- public ---------------- */

void video_open(const char *link)
{
	video_close();
	str_copy(url, link, sizeof(url));
	err[0] = 0;
	progress = 0;
	cancel = 0;
	downloaded = 0;
	state = VS_DOWNLOADING;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 128 * 1024);
	dl_running = pthread_create(&dl_thread, &attr, download, NULL) == 0;
	pthread_attr_destroy(&attr);
	if (!dl_running) {
		state = VS_ERROR;
		snprintf(err, sizeof(err), "%s", T("Error interno (JSON)"));
	}
}

void video_close(void)
{
	cancel = 1;
	if (dl_running) {
		pthread_join(dl_thread, NULL);
		dl_running = 0;
	}
	player_stop();
	sceIoRemove(VIDEO_PATH);
	state = VS_IDLE;
}

void video_update(void)
{
	if (state == VS_DOWNLOADING && downloaded) {
		if (dl_running) {
			pthread_join(dl_thread, NULL);
			dl_running = 0;
		}
		if (player_start() < 0)
			state = VS_ERROR;
	}
	if ((state == VS_PLAYING || state == VS_PAUSED) && player) {
		if (!duration) {
			SceAvPlayerStreamInfo si;
			for (int i = 0; i < 2 && !duration; i++)
				if (sceAvPlayerGetStreamInfo(player, i, &si) == 0)
					duration = si.duration;
		}
		if (state == VS_PLAYING && !sceAvPlayerIsActive(player)) {
			state = VS_ENDED;
			g_dirty = 1;
		}
	}
}

vita2d_texture *video_frame(void)
{
	if (state == VS_PLAYING && player) {
		SceAvPlayerFrameInfo fr;
		if (sceAvPlayerGetVideoData(player, &fr)) {
			sceGxmTextureInitLinear(&frame_tex.gxm_tex, fr.pData, SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1,
			                        fr.details.video.width, fr.details.video.height, 0);
			sceGxmTextureSetMinFilter(&frame_tex.gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
			sceGxmTextureSetMagFilter(&frame_tex.gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
			have_frame = 1;
		}
	}
	return have_frame && player ? &frame_tex : NULL;
}

void video_toggle_pause(void)
{
	if (state == VS_PLAYING && player) {
		sceAvPlayerPause(player);
		state = VS_PAUSED;
	} else if (state == VS_PAUSED && player) {
		sceAvPlayerResume(player);
		state = VS_PLAYING;
	} else if (state == VS_ENDED) {
		player_stop();               /* play again from the start */
		if (player_start() < 0)
			state = VS_ERROR;
	}
}

void video_seek(int seconds)
{
	if (!player || (state != VS_PLAYING && state != VS_PAUSED))
		return;
	int64_t t = (int64_t)sceAvPlayerCurrentTime(player) + seconds * 1000LL;
	if (t < 0)
		t = 0;
	if (duration && (uint64_t)t > duration)
		t = duration > 1000 ? duration - 1000 : 0;
	sceAvPlayerJumpToTime(player, (uint64_t)t);
}

int video_state(void) { return state; }
int video_progress(void) { return progress; }
const char *video_error(void) { return err; }
uint64_t video_time_ms(void) { return player ? sceAvPlayerCurrentTime(player) : 0; }
uint64_t video_duration_ms(void) { return duration; }
