#include <stdbool.h>
#include <stdlib.h>

#include "chan.h"
#include "util.h"

int chan_number(int freq)
{
	if (freq == 2484)
		return 14;
	if (freq >= 2407 && freq < 2484)
		return (freq - 2407) / 5;
	if (freq >= 4910 && freq <= 4980)
		return (freq - 4000) / 5;
	if (freq > 5000 && freq < 5950)
		return (freq - 5000) / 5;
	if (freq > 5950 && freq <= 7115)
		return (freq - 5950) / 5;
	return 0;
}

int chan_center1(int freq, int width)
{
	static const int c40[] = { 38, 46, 54, 62, 102, 110, 118, 126, 134, 142,
				   151, 159, 167, 175 };
	static const int c80[] = { 42, 58, 106, 122, 138, 155, 171 };
	const int *tab;
	int ch, n;

	if (width == 20)
		return freq;

	if (freq < 5000) {
		if (width != 40 || freq < 2412 || freq > 2472)
			return 0;
		/* HT40+ below channel 8, HT40- from it */
		return freq <= 2442 ? freq + 10 : freq - 10;
	}

	if (width == 40) {
		tab = c40;
		n = ARRAY_LEN(c40);
	} else if (width == 80) {
		tab = c80;
		n = ARRAY_LEN(c80);
	} else {
		return 0;
	}

	ch = chan_number(freq);
	for (int i = 0; i < n; i++)
		if (abs(ch - tab[i]) * 5 < width / 2)
			return 5000 + tab[i] * 5;

	return 0;
}

int chan_sweep_cover(const uint16_t *f, int n, double half, uint16_t *out)
{
	double covered = -1;
	int k = 0, i = 0;

	while (i < n) {
		double want;
		int best;

		if (f[i] + half <= covered) {
			i++;
			continue;
		}

		/* leftmost point still uncovered, then the furthest channel reaching it */
		want = f[i] - half > covered ? f[i] - half : covered;
		best = i;
		for (int j = i + 1; j < n && f[j] - half <= want; j++)
			best = j;

		out[k++] = f[best];
		covered = f[best] + half;
		i = best + 1;
	}

	return k;
}

static bool has(const uint16_t *f, int n, int freq)
{
	for (int i = 0; i < n; i++)
		if (f[i] == freq)
			return true;
	return false;
}

static int add_cand(struct chan_chunk *c, int k, int max, int freq, int center1, int width)
{
	for (int i = 0; i < k; i++)
		if (c[i].center1 == center1 && c[i].width == width)
			return k;
	if (k >= max)
		return k;

	c[k].freq = freq;
	c[k].center1 = center1;
	c[k].width = width;
	return k + 1;
}

int chan_chunks(const uint16_t *f, int n, int max_width, struct chan_chunk *out, int max)
{
	struct chan_chunk cand[256];
	double covered = -1;
	int nc = 0, k = 0;

	for (int i = 0; i < n; i++) {
		int p = f[i], c;

		nc = add_cand(cand, nc, ARRAY_LEN(cand), p, p, 20);

		if (max_width >= 40 && p < 5000) {
			/* 2.4 GHz HT40: the secondary sits 20 MHz above or below */
			if (has(f, n, p + 20))
				nc = add_cand(cand, nc, ARRAY_LEN(cand), p, p + 10, 40);
			if (has(f, n, p - 20))
				nc = add_cand(cand, nc, ARRAY_LEN(cand), p, p - 10, 40);
		} else if (max_width >= 40) {
			c = chan_center1(p, 40);
			if (c && has(f, n, c - 10) && has(f, n, c + 10))
				nc = add_cand(cand, nc, ARRAY_LEN(cand), p, c, 40);
		}

		if (max_width >= 80 && p > 5000) {
			c = chan_center1(p, 80);
			if (c && has(f, n, c - 30) && has(f, n, c - 10) &&
			    has(f, n, c + 10) && has(f, n, c + 30))
				nc = add_cand(cand, nc, ARRAY_LEN(cand), p, c, 80);
		}
	}

	for (int i = 0; i < n && k < max; i++) {
		double want, reach = -1;
		int best = -1;

		if (f[i] + 10 <= covered + 0.5)
			continue;

		/* leftmost uncovered point, then the block reaching furthest right from it */
		want = f[i] - 10 > covered ? f[i] - 10 : covered;
		for (int j = 0; j < nc; j++) {
			double lo = cand[j].center1 - cand[j].width / 2.0;
			double hi = cand[j].center1 + cand[j].width / 2.0;

			if (lo <= want + 0.5 && hi > reach) {
				best = j;
				reach = hi;
			}
		}

		out[k++] = cand[best];
		covered = reach;
	}

	return k;
}
