#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "fft.h"

static uint16_t be16(const uint8_t *p)
{
	return p[0] << 8 | p[1];
}

static uint64_t be64(const uint8_t *p)
{
	uint64_t v = 0;

	for (int i = 0; i < 8; i++)
		v = v << 8 | p[i];
	return v;
}

int fft_parse(const uint8_t *p, size_t len, struct fft_sample *s)
{
	size_t tlen, n_bins;

	if (len < 3)
		return 0;

	if (p[0] != FFT_SAMPLE_ATH10K)
		return -1;

	tlen = be16(p + 1);
	if (tlen < FFT_ATH10K_HDR - 3 + FFT_MIN_BINS ||
	    tlen > FFT_ATH10K_HDR - 3 + FFT_MAX_BINS)
		return -1;

	if (len < FFT_ATH10K_HDR)
		return 0;

	s->chan_width = p[3];
	s->freq1 = be16(p + 4);
	s->freq2 = be16(p + 6);
	s->noise = (int16_t)be16(p + 8);
	s->gain = be16(p + 12);
	s->tsf = be64(p + 16);
	s->rssi = (int8_t)p[25];
	s->max_exp = p[28];

	/* Cheap sanity checks so a desynced stream can't pass as a sample */
	if (s->freq1 < 2300 || s->freq1 > 7200 || s->chan_width < 5 ||
	    s->chan_width > 200 || s->max_exp > 7)
		return -1;

	if (len < 3 + tlen)
		return 0;

	n_bins = tlen - (FFT_ATH10K_HDR - 3);
	s->n_bins = n_bins;
	s->bins = p + FFT_ATH10K_HDR;

	return 3 + tlen;
}

int fft_grid_init(struct fft_grid *g, double lo, double hi, double res, int max_cells)
{
	int n;

	memset(g, 0, sizeof(*g));

	if (hi <= lo || res <= 0)
		return -1;

	n = (int)ceil((hi - lo) / res);
	if (n > max_cells) {
		n = max_cells;
		res = (hi - lo) / n;
	}

	g->start = lo;
	g->res = res;
	g->n = n;
	g->occ_snr = 10.0f;
	g->max = calloc(n, sizeof(*g->max));
	g->sum = calloc(n, sizeof(*g->sum));
	g->cnt = calloc(n, sizeof(*g->cnt));
	g->occ = calloc(n, sizeof(*g->occ));

	if (!g->max || !g->sum || !g->cnt || !g->occ) {
		fft_grid_free(g);
		return -1;
	}

	return 0;
}

void fft_grid_free(struct fft_grid *g)
{
	free(g->max);
	free(g->sum);
	free(g->cnt);
	free(g->occ);
	memset(g, 0, sizeof(*g));
}

void fft_grid_clear(struct fft_grid *g)
{
	memset(g->max, 0, g->n * sizeof(*g->max));
	memset(g->sum, 0, g->n * sizeof(*g->sum));
	memset(g->cnt, 0, g->n * sizeof(*g->cnt));
	memset(g->occ, 0, g->n * sizeof(*g->occ));
	g->samples = 0;
}

/* Quickselect: the k-th smallest of a[0..n-1], reordering a */
static uint32_t kth(uint32_t *a, int n, int k)
{
	int lo = 0, hi = n - 1;

	while (lo < hi) {
		uint32_t pivot = a[(lo + hi) / 2], t;
		int i = lo, j = hi;

		while (i <= j) {
			while (a[i] < pivot)
				i++;
			while (a[j] > pivot)
				j--;
			if (i <= j) {
				t = a[i];
				a[i++] = a[j];
				a[j--] = t;
			}
		}

		if (k <= j)
			hi = j;
		else if (k >= i)
			lo = i;
		else
			break;
	}

	return a[k];
}

void fft_cal_init(struct fft_cal *cal, float c)
{
	memset(cal, 0, sizeof(*cal));
	cal->c = c;
}

/*
 * Follows the quietest capture median of a chunk down at once and up only
 * slowly (~0.3 dB/s at typical rates), so a transmission that fills the
 * whole chunk still counts as occupied instead of becoming the reference.
 */
static float noise_ref(struct fft_cal *cal, const struct fft_sample *s, float med)
{
	struct fft_noise *r = NULL;

	for (int i = 0; i < cal->n_noise; i++)
		if (cal->noise[i].freq1 == s->freq1 && cal->noise[i].cw == s->chan_width)
			r = &cal->noise[i];

	if (!r) {
		if (cal->n_noise >= FFT_NOISE_REFS)
			return med;
		r = &cal->noise[cal->n_noise++];
		r->freq1 = s->freq1;
		r->cw = s->chan_width;
		r->ref = med;
	}

	if (med < r->ref)
		r->ref = med;
	else
		r->ref *= 1.0002f;

	return r->ref;
}

static void fft_cal_update(struct fft_cal *cal, const struct fft_sample *s, uint64_t sq)
{
	float implied;

	if (s->gain > cal->gmax)
		cal->gmax = s->gain;
	if (s->gain + 3 < cal->gmax)
		return;

	implied = s->noise + s->rssi - 10.0f * log10f((float)sq) + s->gain +
		  10.0f * log10f(s->n_bins);
	cal->n++;
	cal->c += (implied - cal->c) / (cal->n < 200 ? cal->n : 200);
}

bool fft_grid_add(struct fft_grid *g, struct fft_cal *cal, const struct fft_sample *s)
{
	int n = s->n_bins, e = s->max_exp;
	double bw = s->chan_width, f0, step, half;
	uint32_t sorted[FFT_MAX_BINS], med, peak = 0;
	uint64_t sq = 0;
	float k, thr;

	for (int i = 0; i < n; i++) {
		uint32_t d = (uint32_t)s->bins[i] << e;

		sq += d * d;
		sorted[i] = d * d;
		if (s->bins[i] > peak)
			peak = s->bins[i];
	}

	/*
	 * Some reports claim a strong signal but carry near-empty bins; spread
	 * over the band they draw a flat line at the rssi level.
	 */
	if (!sq || (s->rssi >= 15 && peak <= 4))
		return false;

	/* nf_chains is 0 or garbage when the firmware has no estimate yet */
	if (s->noise >= -130 && s->noise <= -40)
		fft_cal_update(cal, s, sq);
	if (!cal->n)
		return false;

	k = powf(10.0f, (cal->c - s->gain) / 10.0f);

	/*
	 * The firmware noise field isn't consistent across widths, so occupancy
	 * is measured against the chunk's own quiet-time median bin, which is
	 * ln 2 times the mean for noise-only bins.
	 */
	med = kth(sorted, n, n / 2);
	thr = noise_ref(cal, s, k * (med ? med : 1)) / 0.693f * g->occ_snr;

	f0 = s->freq1 - bw / 2;
	step = bw / n;
	/* the driver reports 22/44/88 MHz for 20/40/80 */
	half = g->trim ? bw / 2.2 + g->res : g->half_span;

	for (int i = 0; i < n; i++) {
		double f = f0 + step * (i + 0.5);
		uint32_t d, v;
		float p;
		int c;

		if (half > 0 && fabs(f - s->freq1) > half)
			continue;

		c = (int)floor((f - g->start) / g->res);
		if (c < 0 || c >= g->n)
			continue;

		d = (uint32_t)s->bins[i] << e;
		v = d * d;
		p = k * (v ? v : 1);

		if (p > g->max[c])
			g->max[c] = p;
		g->sum[c] += p;
		g->cnt[c]++;
		if (p > thr)
			g->occ[c]++;
	}

	g->samples++;
	return true;
}

static uint8_t enc_dbm(float mw)
{
	long v = lrintf(10.0f * log10f(mw)) + FFT_DBM_OFFSET;

	if (v < 1)
		return 1;
	if (v > 255)
		return 255;
	return v;
}

void fft_grid_export(const struct fft_grid *g, uint8_t *max, uint8_t *avg, uint8_t *occ)
{
	for (int c = 0; c < g->n; c++) {
		uint32_t n = g->cnt[c];

		if (!n) {
			max[c] = avg[c] = 0;
			occ[c] = FFT_OCC_NONE;
			continue;
		}

		max[c] = enc_dbm(g->max[c]);
		avg[c] = enc_dbm(g->sum[c] / n);
		occ[c] = (g->occ[c] * 100 + n / 2) / n;
	}
}
