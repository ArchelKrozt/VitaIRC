#ifndef VITAIRC_IMGUTIL_H
#define VITAIRC_IMGUTIL_H

#include <stddef.h>
#include <vita2d.h>

/* Image helpers shared by the viewer and the image search. */

/* PNG, JPEG or WebP -> texture. WebP is scaled down to fit maxdim (PNG/JPEG
 * keep their size). NULL when the format is unknown or decoding fails. */
vita2d_texture *img_texture(const unsigned char *buf, size_t len, int maxdim);
int  img_is_webp(const unsigned char *buf, size_t len);
int  img_is_gif(const unsigned char *buf, size_t len);

/* WebP -> malloc'd RGBA (stride w*4), scaled down to fit maxdim. */
unsigned char *webp_decode_rgba(const unsigned char *buf, size_t len, int maxdim, int *w, int *h);

/* Writes RGBA pixels (R,G,B,A in memory) as a JPEG file. Returns 0 on success. */
int  rgba_save_jpeg(const unsigned char *px, int w, int h, int stride, const char *path, int quality);
/* Saves a texture as JPEG, scaled down (nearest) to fit maxdim. */
int  tex_save_jpeg(vita2d_texture *tex, const char *path, int maxdim, int quality);

#endif
