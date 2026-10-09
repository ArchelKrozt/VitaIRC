#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <jpeglib.h>
#include <png.h>
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

/* ---------------- thumbnails ---------------- */

static void fit(int w, int h, int maxw, int maxh, int *ow, int *oh)
{
	*ow = w;
	*oh = h;
	if (w > maxw) {
		*ow = maxw;
		*oh = (int)((long)h * maxw / w);
	}
	if (*oh > maxh) {
		*oh = maxh;
		*ow = (int)((long)w * maxh / h);
	}
	if (*ow < 1) *ow = 1;
	if (*oh < 1) *oh = 1;
}

struct jerr { struct jpeg_error_mgr pub; jmp_buf jb; };

static void jpeg_fail(j_common_ptr ci)
{
	longjmp(((struct jerr *)ci->err)->jb, 1);
}

static unsigned char *jpeg_thumb(const unsigned char *buf, size_t len, int maxw, int maxh, int *w, int *h)
{
	struct jpeg_decompress_struct ci;
	struct jerr je;
	unsigned char *volatile out = NULL, *volatile row = NULL;
	ci.err = jpeg_std_error(&je.pub);
	je.pub.error_exit = jpeg_fail;
	if (setjmp(je.jb)) {
		jpeg_destroy_decompress(&ci);
		free(out);
		free(row);
		return NULL;
	}
	jpeg_create_decompress(&ci);
	jpeg_mem_src(&ci, (unsigned char *)buf, len);
	jpeg_read_header(&ci, TRUE);
	ci.out_color_space = JCS_RGB;
	/* let libjpeg shrink by 1/2, 1/4 or 1/8 while decoding */
	ci.scale_num = 1;
	ci.scale_denom = 1;
	while (ci.scale_denom < 8 && ci.image_width / (ci.scale_denom * 2) >= (unsigned)maxw &&
	       ci.image_height / (ci.scale_denom * 2) >= (unsigned)maxh)
		ci.scale_denom *= 2;
	jpeg_start_decompress(&ci);
	int sw = ci.output_width, sh = ci.output_height, ow, oh;
	fit(sw, sh, maxw, maxh, &ow, &oh);
	out = malloc((size_t)ow * oh * 4);
	row = malloc((size_t)sw * ci.output_components);
	if (!out || !row)
		longjmp(je.jb, 1);
	int next = 0;
	while (ci.output_scanline < (unsigned)sh) {
		int y = ci.output_scanline;
		JSAMPROW rp = row;
		jpeg_read_scanlines(&ci, &rp, 1);
		while (next < oh && next * sh / oh == y) {
			unsigned char *d = out + (size_t)next * ow * 4;
			for (int x = 0; x < ow; x++) {
				const unsigned char *s = row + (size_t)(x * sw / ow) * ci.output_components;
				d[x * 4 + 0] = s[0];
				d[x * 4 + 1] = ci.output_components > 1 ? s[1] : s[0];
				d[x * 4 + 2] = ci.output_components > 2 ? s[2] : s[0];
				d[x * 4 + 3] = 255;
			}
			next++;
		}
	}
	jpeg_finish_decompress(&ci);
	jpeg_destroy_decompress(&ci);
	free(row);
	*w = ow;
	*h = oh;
	return out;
}

typedef struct { const unsigned char *p; size_t len, pos; } PngSrc;

static void png_read_mem(png_structp png, png_bytep data, png_size_t n)
{
	PngSrc *s = png_get_io_ptr(png);
	if (s->pos + n > s->len)
		png_error(png, "eof");
	memcpy(data, s->p + s->pos, n);
	s->pos += n;
}

static unsigned char *png_thumb(const unsigned char *buf, size_t len, int maxw, int maxh, int *w, int *h)
{
	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop info = png ? png_create_info_struct(png) : NULL;
	unsigned char *volatile out = NULL, *volatile row = NULL;
	if (!info) {
		png_destroy_read_struct(&png, NULL, NULL);
		return NULL;
	}
	if (setjmp(png_jmpbuf(png))) {
		png_destroy_read_struct(&png, &info, NULL);
		free(out);
		free(row);
		return NULL;
	}
	PngSrc src = { buf, len, 0 };
	png_set_read_fn(png, &src, png_read_mem);
	png_read_info(png, info);
	int sw = png_get_image_width(png, info), sh = png_get_image_height(png, info);
	int ct = png_get_color_type(png, info);
	if (png_get_bit_depth(png, info) == 16)
		png_set_strip_16(png);
	if (ct == PNG_COLOR_TYPE_PALETTE)
		png_set_palette_to_rgb(png);
	if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA)
		png_set_gray_to_rgb(png);
	if (png_get_valid(png, info, PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(png);
	png_set_expand(png);
	png_set_filler(png, 0xff, PNG_FILLER_AFTER);
	int passes = png_set_interlace_handling(png);
	png_read_update_info(png, info);
	if (passes > 1 && (long)sw * sh > 2048L * 2048) {
		png_destroy_read_struct(&png, &info, NULL);
		return NULL;                  /* big interlaced PNG: needs the whole image */
	}
	int ow, oh;
	fit(sw, sh, maxw, maxh, &ow, &oh);
	if (passes > 1) {
		/* interlaced: decode whole (small enough), then sample */
		unsigned char *full = malloc((size_t)sw * sh * 4);
		png_bytep *rows = malloc(sh * sizeof(png_bytep));
		out = malloc((size_t)ow * oh * 4);
		if (!full || !rows || !out) {
			free(full);
			free(rows);
			png_error(png, "oom");
		}
		for (int y = 0; y < sh; y++)
			rows[y] = full + (size_t)y * sw * 4;
		png_read_image(png, rows);
		for (int y = 0; y < oh; y++)
			for (int x = 0; x < ow; x++)
				memcpy(out + ((size_t)y * ow + x) * 4, full + ((size_t)(y * sh / oh) * sw + x * sw / ow) * 4, 4);
		free(full);
		free(rows);
	} else {
		out = malloc((size_t)ow * oh * 4);
		row = malloc(png_get_rowbytes(png, info) > (size_t)sw * 4 ? png_get_rowbytes(png, info) : (size_t)sw * 4);
		if (!out || !row)
			png_error(png, "oom");
		int next = 0;
		for (int y = 0; y < sh; y++) {
			png_read_row(png, row, NULL);
			while (next < oh && next * sh / oh == y) {
				for (int x = 0; x < ow; x++)
					memcpy(out + ((size_t)next * ow + x) * 4, row + (size_t)(x * sw / ow) * 4, 4);
				next++;
			}
		}
		free(row);
		row = NULL;
	}
	png_destroy_read_struct(&png, &info, NULL);
	*w = ow;
	*h = oh;
	return out;
}

unsigned char *img_decode_thumb(const unsigned char *buf, size_t len, int maxw, int maxh, int *w, int *h)
{
	if (len > 8 && buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G')
		return png_thumb(buf, len, maxw, maxh, w, h);
	if (len > 3 && buf[0] == 0xFF && buf[1] == 0xD8)
		return jpeg_thumb(buf, len, maxw, maxh, w, h);
	if (img_is_webp(buf, len)) {
		WebPBitstreamFeatures f;
		if (WebPGetFeatures(buf, len, &f) != VP8_STATUS_OK)
			return NULL;
		int ow, oh;
		fit(f.width, f.height, maxw, maxh, &ow, &oh);
		return webp_decode_rgba(buf, len, ow > oh ? ow : oh, w, h);
	}
	return NULL;
}

vita2d_texture *tex_from_rgba(const unsigned char *px, int w, int h)
{
	vita2d_texture *t = vita2d_create_empty_texture(w, h);
	if (!t)
		return NULL;
	unsigned char *d = vita2d_texture_get_datap(t);
	int stride = vita2d_texture_get_stride(t);
	for (int y = 0; y < h; y++)
		memcpy(d + (size_t)y * stride, px + (size_t)y * w * 4, (size_t)w * 4);
	return t;
}
