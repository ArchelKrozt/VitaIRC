#include <stdint.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include "sound.h"
#include "irc.h"
#include "util.h"

#define RATE  48000
#define GRAIN 256
#define NOTE  (RATE * 90 / 1000)          /* 90 ms per note */
#define TOTAL (((NOTE * 2) / GRAIN + 1) * GRAIN)

static int16_t chime[TOTAL];

static void build_chime(void)
{
	const float freq[2] = { 880.0f, 1318.5f };   /* A5 -> E6 */
	memset(chime, 0, sizeof(chime));
	for (int n = 0; n < 2; n++) {
		for (int i = 0; i < NOTE; i++) {
			float t = (float)i / RATE;
			float env = 1.0f - (float)i / NOTE;           /* fade out */
			float att = i < 240 ? i / 240.0f : 1.0f;      /* no click */
			chime[n * NOTE + i] = (int16_t)(sinf(2 * 3.14159265f * freq[n] * t) * 7000 * env * att);
		}
	}
}

static void *sound_thread(void *arg)
{
	(void)arg;
	int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, RATE, SCE_AUDIO_OUT_MODE_MONO);
	if (port < 0)
		return NULL;
	uint64_t last = 0;
	while (!g_app_quit) {
		if (!g_beep_req) {
			sceKernelDelayThread(50 * 1000);
			continue;
		}
		g_beep_req = 0;
		if (now_ms() - last < 1500)      /* don't machine-gun on busy channels */
			continue;
		last = now_ms();
		for (int i = 0; i < TOTAL; i += GRAIN)
			sceAudioOutOutput(port, &chime[i]);
	}
	sceAudioOutReleasePort(port);
	return NULL;
}

void sound_init(void)
{
	build_chime();
	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 32 * 1024);
	pthread_create(&th, &attr, sound_thread, NULL);
	pthread_attr_destroy(&attr);
}
