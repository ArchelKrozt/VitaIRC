#ifndef VITAIRC_CAMERA_H
#define VITAIRC_CAMERA_H

#include <vita2d.h>

#define CAM_W 640
#define CAM_H 480

/* The camera writes straight into a vita2d texture shown by the UI. */
int  cam_open(int back);
void cam_close(void);
int  cam_is_open(void);
void cam_update(void);
vita2d_texture *cam_texture(void);
/* Encodes the current frame as JPEG. Returns 0 on success. */
int  cam_save_jpeg(const char *path);

#endif
