#include <time.h>

#include "util.h"

int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

size_t base64_size(size_t n)
{
	return (n + 2) / 3 * 4 + 1;
}

void base64_encode(const uint8_t *in, size_t n, char *out)
{
	static const char tab[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i;

	for (i = 0; i + 2 < n; i += 3) {
		uint32_t v = in[i] << 16 | in[i + 1] << 8 | in[i + 2];

		*out++ = tab[v >> 18];
		*out++ = tab[(v >> 12) & 63];
		*out++ = tab[(v >> 6) & 63];
		*out++ = tab[v & 63];
	}

	if (i < n) {
		uint32_t v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0);

		*out++ = tab[v >> 18];
		*out++ = tab[(v >> 12) & 63];
		*out++ = i + 1 < n ? tab[(v >> 6) & 63] : '=';
		*out++ = '=';
	}

	*out = 0;
}
