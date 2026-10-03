#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/camera.h>
#include <jpeglib.h>

#include "camera.h"

static vita2d_texture *tex;
static int dev = -1;

int cam_open(int back)
{
	cam_close();
	tex = vita2d_create_empty_texture(CAM_W, CAM_H);
	if (!tex)
		return -1;
	SceCameraInfo info;
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	info.priority = SCE_CAMERA_PRIORITY_SHARE;
	info.format = SCE_CAMERA_FORMAT_ABGR;
	info.resolution = SCE_CAMERA_RESOLUTION_640_480;
	info.framerate = SCE_CAMERA_FRAMERATE_30_FPS;
	info.width = CAM_W;
	info.height = CAM_H;
	info.range = 1;
	info.sizeIBase = CAM_W * CAM_H * 4;
	info.pIBase = vita2d_texture_get_datap(tex);
	info.pitch = vita2d_texture_get_stride(tex) / 4 - CAM_W;
	int d = back ? SCE_CAMERA_DEVICE_BACK : SCE_CAMERA_DEVICE_FRONT;
	if (sceCameraOpen(d, &info) < 0) {
		cam_close();
		return -1;
	}
	if (sceCameraStart(d) < 0) {
		sceCameraClose(d);
		cam_close();
		return -1;
	}
	dev = d;
	return 0;
}

void cam_close(void)
{
	if (dev >= 0) {
		sceCameraStop(dev);
		sceCameraClose(dev);
		dev = -1;
	}
	if (tex) {
		vita2d_wait_rendering_done();
		vita2d_free_texture(tex);
		tex = NULL;
	}
}

int cam_is_open(void)
{
	return dev >= 0;
}

void cam_update(void)
{
	if (dev < 0 || !sceCameraIsActive(dev))
		return;
	SceCameraRead r;
	memset(&r, 0, sizeof(r));
	r.size = sizeof(r);
	sceCameraRead(dev, &r);
}

vita2d_texture *cam_texture(void)
{
	return tex;
}

int cam_save_jpeg(const char *path)
{
	if (!tex)
		return -1;
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	const unsigned char *px = vita2d_texture_get_datap(tex);
	int stride = vita2d_texture_get_stride(tex);
	struct jpeg_compress_struct ci;
	struct jpeg_error_mgr jerr;
	ci.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&ci);
	unsigned char *out = NULL;
	unsigned long outlen = 0;
	jpeg_mem_dest(&ci, &out, &outlen);
	ci.image_width = CAM_W;
	ci.image_height = CAM_H;
	ci.input_components = 3;
	ci.in_color_space = JCS_RGB;
	jpeg_set_defaults(&ci);
	jpeg_set_quality(&ci, 88, TRUE);
	jpeg_start_compress(&ci, TRUE);
	static unsigned char row[CAM_W * 3];
	while (ci.next_scanline < CAM_H) {
		const unsigned char *src = px + ci.next_scanline * stride;
		for (int x = 0; x < CAM_W; x++) {   /* memory order is R,G,B,A */
			row[x * 3 + 0] = src[x * 4 + 0];
			row[x * 3 + 1] = src[x * 4 + 1];
			row[x * 3 + 2] = src[x * 4 + 2];
		}
		JSAMPROW rp = row;
		jpeg_write_scanlines(&ci, &rp, 1);
	}
	jpeg_finish_compress(&ci);
	jpeg_destroy_compress(&ci);
	size_t ok = out ? fwrite(out, 1, outlen, f) : 0;
	fclose(f);
	free(out);
	return ok == outlen && outlen ? 0 : -1;
}
