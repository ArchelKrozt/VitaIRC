#ifndef VITAIRC_MINIJSON_H
#define VITAIRC_MINIJSON_H

#include <stddef.h>

/* Tiny read-only JSON navigation: enough for the OpenAI and Imgur replies. */

/* Value of `key` inside the object at `obj` (NULL if missing). */
const char *mj_get(const char *obj, const char *key);
/* Element `idx` of the array at `arr` (NULL if missing). */
const char *mj_at(const char *arr, int idx);
/* Decodes the string at `val` into a malloc'd UTF-8 buffer (NULL if not a string). */
char       *mj_str(const char *val);
/* Like mj_str, but numbers and booleans come back as their text; NULL for null/missing. */
char       *mj_scalar(const char *val);

/* Appends `s` as a quoted, escaped JSON string to the malloc'd buffer *buf. */
void        mj_append_str(char **buf, size_t *len, const char *s);
void        mj_append_raw(char **buf, size_t *len, const char *s);

#endif
