#ifndef SPECTRALD_UTIL_H
#define SPECTRALD_UTIL_H

#include <stddef.h>
#include <stdint.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

int64_t now_ms(void);

/* Encoded length including the trailing NUL. */
size_t base64_size(size_t n);
void base64_encode(const uint8_t *in, size_t n, char *out);

#endif
