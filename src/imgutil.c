#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#include <webp/decode.h>

#include "imgutil.h"

int img_is_webp(const unsigned char *buf, size_t len)
{
	return len > 12 && !memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "WEBP", 4);
}

int img_is_gif(const unsigned char *buf, size_t len)
{
	return len > 4 && !memcmp(buf, "GIF8", 4);
}

/* Reads the WebP size and the scaled size that fits maxdim. */
static int webp_setup(WebPDecoderConfig *cfg, const unsigned char *buf, size_t len, int maxdim, int *sw, int *sh)
{
	if (!WebPInitDecoderConfig(cfg) || WebPGetFeatures(buf, len, &cfg->input) != VP8_STATUS_OK)
		return -1;
	int w = cfg->input.width, h = cfg->input.height;
	if (w <= 0 || h <= 0)
		return -1;
	*sw = w;
	*sh = h;
	if (maxdim > 0 && (w > maxdim || h > maxdim)) {
		if (w >= h) {
			*sw = maxdim;
			*sh = (int)((long)h * maxdim / w);
		} else {
			*sh = maxdim;
			*sw = (int)((long)w * maxdim / h);
		}
		if (*sw < 1) *sw = 1;
		if (*sh < 1) *sh = 1;
	}
	cfg->options.use_scaling = (*sw != w || *sh != h);
	cfg->options.scaled_width = *sw;
	cfg->options.scaled_height = *sh;
	cfg->output.colorspace = MODE_RGBA;
	cfg->output.is_external_memory = 1;
	return 0;
}

static vita2d_texture *webp_texture(const unsigned char *buf, size_t len, int maxdim)
{
	WebPDecoderConfig cfg;
	int sw, sh;
	if (webp_setup(&cfg, buf, len, maxdim, &sw, &sh) < 0)
		return NULL;
	vita2d_texture *t = vita2d_create_empty_texture(sw, sh);
	if (!t)
		return NULL;
	int stride = vita2d_texture_get_stride(t);
	cfg.output.u.RGBA.rgba = vita2d_texture_get_datap(t);
	cfg.output.u.RGBA.stride = stride;
	cfg.output.u.RGBA.size = (size_t)stride * sh;
	if (WebPDecode(buf, len, &cfg) != VP8_STATUS_OK) {
		vita2d_free_texture(t);
		return NULL;
	}
	return t;
}

unsigned char *webp_decode_rgba(const unsigned char *buf, size_t len, int maxdim, int *w, int *h)
{
	WebPDecoderConfig cfg;
	int sw, sh;
	if (webp_setup(&cfg, buf, len, maxdim, &sw, &sh) < 0)
		return NULL;
	unsigned char *px = malloc((size_t)sw * sh * 4);
	if (!px)
		return NULL;
	cfg.output.u.RGBA.rgba = px;
	cfg.output.u.RGBA.stride = sw * 4;
	cfg.output.u.RGBA.size = (size_t)sw * sh * 4;
	if (WebPDecode(buf, len, &cfg) != VP8_STATUS_OK) {
		free(px);
		return NULL;
	}
	*w = sw;
	*h = sh;
	return px;
}

vita2d_texture *img_texture(const unsigned char *buf, size_t len, int maxdim)
{
	if (len > 8 && buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G')
		return vita2d_load_PNG_buffer(buf);
	if (len > 3 && buf[0] == 0xFF && buf[1] == 0xD8)
		return vita2d_load_JPEG_buffer(buf, len);
	if (img_is_webp(buf, len))
		return webp_texture(buf, len, maxdim);
	return NULL;
}

int rgba_save_jpeg(const unsigned char *px, int w, int h, int stride, const char *path, int quality)
{
	unsigned char *row = malloc(w * 3);
	if (!row)
		return -1;
	struct jpeg_compress_struct ci;
	struct jpeg_error_mgr jerr;
	ci.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&ci);
	unsigned char *out = NULL;
	unsigned long outlen = 0;
	jpeg_mem_dest(&ci, &out, &outlen);
	ci.image_width = w;
	ci.image_height = h;
	ci.input_components = 3;
	ci.in_color_space = JCS_RGB;
	jpeg_set_defaults(&ci);
	jpeg_set_quality(&ci, quality, TRUE);
	jpeg_start_compress(&ci, TRUE);
	while (ci.next_scanline < (unsigned)h) {
		const unsigned char *src = px + (size_t)ci.next_scanline * stride;
		for (int x = 0; x < w; x++) {
			row[x * 3 + 0] = src[x * 4 + 0];
			row[x * 3 + 1] = src[x * 4 + 1];
			row[x * 3 + 2] = src[x * 4 + 2];
		}
		JSAMPROW rp = row;
		jpeg_write_scanlines(&ci, &rp, 1);
	}
	jpeg_finish_compress(&ci);
	jpeg_destroy_compress(&ci);
	free(row);
	int ok = -1;
	FILE *f = out ? fopen(path, "wb") : NULL;
	if (f) {
		ok = fwrite(out, 1, outlen, f) == outlen ? 0 : -1;
		fclose(f);
	}
	free(out);
	return ok;
}

int tex_save_jpeg(vita2d_texture *tex, const char *path, int maxdim, int quality)
{
	int w = vita2d_texture_get_width(tex), h = vita2d_texture_get_height(tex);
	int stride = vita2d_texture_get_stride(tex);
	const unsigned char *px = vita2d_texture_get_datap(tex);
	/* PNG/WebP textures are RGBA, JPEG ones RGB (or grey) */
	SceGxmTextureFormat fmt = vita2d_texture_get_format(tex);
	int bpp = fmt == SCE_GXM_TEXTURE_FORMAT_U8U8U8_BGR ? 3 : fmt == SCE_GXM_TEXTURE_FORMAT_U8_R111 ? 1 : 4;
	int sw = w, sh = h;
	if (maxdim > 0 && (w > maxdim || h > maxdim)) {
		sw = w >= h ? maxdim : (int)((long)w * maxdim / h);
		sh = w >= h ? (int)((long)h * maxdim / w) : maxdim;
	}
	if (sw < 1) sw = 1;
	if (sh < 1) sh = 1;
	unsigned char *rgba = malloc((size_t)sw * sh * 4);
	if (!rgba)
		return -1;
	for (int y = 0; y < sh; y++) {
		const unsigned char *src = px + (size_t)(y * h / sh) * stride;
		for (int x = 0; x < sw; x++) {
			const unsigned char *s = src + (size_t)(x * w / sw) * bpp;
			unsigned char *d = rgba + ((size_t)y * sw + x) * 4;
			d[0] = s[0];
			d[1] = bpp == 1 ? s[0] : s[1];
			d[2] = bpp == 1 ? s[0] : s[2];
			d[3] = 255;
		}
	}
	int r = rgba_save_jpeg(rgba, sw, sh, sw * 4, path, quality);
	free(rgba);
	return r;
}
