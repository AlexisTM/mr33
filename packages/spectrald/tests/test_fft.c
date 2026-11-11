#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chan.h"
#include "fft.h"
#include "util.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static int sample_gain = 80;

static size_t make_sample(uint8_t *p, int freq1, int cw, int noise, int rssi,
			  int max_exp, const uint8_t *bins, int n)
{
	int tlen = FFT_ATH10K_HDR - 3 + n;

	memset(p, 0, FFT_ATH10K_HDR);
	p[0] = FFT_SAMPLE_ATH10K;
	p[1] = tlen >> 8;
	p[2] = tlen;
	p[3] = cw;
	p[4] = freq1 >> 8;
	p[5] = freq1;
	p[8] = (uint16_t)noise >> 8;
	p[9] = (uint16_t)noise;
	p[12] = sample_gain >> 8;
	p[13] = sample_gain;
	p[16] = 0x01;
	p[23] = 0x02;
	p[25] = (uint8_t)rssi;
	p[28] = max_exp;
	memcpy(p + FFT_ATH10K_HDR, bins, n);
	return FFT_ATH10K_HDR + n;
}

static int dbm(uint8_t v)
{
	return v - FFT_DBM_OFFSET;
}

static void test_b64(void)
{
	char out[16];

	base64_encode((const uint8_t *)"abc", 3, out);
	CHECK(!strcmp(out, "YWJj"));
	base64_encode((const uint8_t *)"ab", 2, out);
	CHECK(!strcmp(out, "YWI="));
	base64_encode((const uint8_t *)"a", 1, out);
	CHECK(!strcmp(out, "YQ=="));
	base64_encode((const uint8_t *)"", 0, out);
	CHECK(!strcmp(out, ""));
	CHECK(base64_size(3) == 5 && base64_size(4) == 9 && base64_size(0) == 1);
}

static void test_parse(void)
{
	uint8_t bins[64], buf[512];
	struct fft_sample s;
	size_t len;

	for (int i = 0; i < 64; i++)
		bins[i] = i;

	len = make_sample(buf, 2437, 22, -95, 20, 3, bins, 64);
	CHECK(fft_parse(buf, len, &s) == (int)len);
	CHECK(s.freq1 == 2437 && s.chan_width == 22 && s.noise == -95 && s.rssi == 20);
	CHECK(s.max_exp == 3 && s.n_bins == 64 && s.bins[63] == 63);
	CHECK(s.tsf == 0x0100000000000002ULL);

	CHECK(fft_parse(buf, len - 1, &s) == 0);
	CHECK(fft_parse(buf, 2, &s) == 0);

	buf[0] = 1;
	CHECK(fft_parse(buf, len, &s) == -1);
	buf[0] = FFT_SAMPLE_ATH10K;

	buf[2] = 10;
	CHECK(fft_parse(buf, len, &s) == -1);

	len = make_sample(buf, 9000, 22, -95, 20, 0, bins, 64);
	CHECK(fft_parse(buf, len, &s) == -1);
}

static void test_resync(void)
{
	uint8_t bins[128] = { 0 }, buf[1024];
	size_t len = 0, off = 0;
	struct fft_sample s;
	int found = 0, skipped = 0;

	memset(buf, 0xaa, 7);
	len = 7;
	len += make_sample(buf + len, 5180, 22, -96, 10, 0, bins, 64);
	len += make_sample(buf + len, 5200, 22, -96, 10, 0, bins, 128);

	while (off < len) {
		int n = fft_parse(buf + off, len - off, &s);

		if (n == 0)
			break;
		if (n < 0) {
			off++;
			skipped++;
			continue;
		}
		off += n;
		found++;
	}

	CHECK(found == 2 && skipped == 7 && off == len);
}

static void test_flat_power(void)
{
	struct fft_cal cal;
	uint8_t bins[64], buf[512], mx[64], av[64], oc[64];
	struct fft_grid g;
	struct fft_sample s;
	int expect;

	memset(bins, 40, sizeof(bins));
	make_sample(buf, 2412, 22, -95, 30, 1, bins, 64);
	CHECK(fft_parse(buf, sizeof(buf), &s) > 0);
	fft_cal_init(&cal, -40);

	CHECK(!fft_grid_init(&g, 2401, 2423, 22.0 / 64 * 1.001, 4096));
	CHECK(g.n == 64);
	CHECK(fft_grid_add(&g, &cal, &s));

	/* flat FFT: every bin reads the total, in dBm per 20 MHz */
	expect = -95 + 30;
	fft_grid_export(&g, mx, av, oc);
	for (int c = 0; c < 64; c++) {
		CHECK(g.cnt[c] == 1);
		CHECK(dbm(mx[c]) == expect && dbm(av[c]) == expect);
	}

	fft_grid_free(&g);
}

static void test_fft_eval_equivalence(void)
{
	struct fft_cal cal;
	uint8_t bins[128], buf[512], mx[256], av[256], oc[256];
	struct fft_grid g;
	struct fft_sample s;
	double sq = 0;
	int e = 2, worst = 0;

	srand(7);
	for (int i = 0; i < 128; i++)
		bins[i] = rand() % 256;
	bins[5] = 0;

	make_sample(buf, 5210, 88, -101, 12, e, bins, 128);
	CHECK(fft_parse(buf, sizeof(buf), &s) > 0);
	fft_cal_init(&cal, -40);
	CHECK(!fft_grid_init(&g, 5210 - 44, 5210 + 44, 88.0 / 128 * 1.001, 4096));
	CHECK(fft_grid_add(&g, &cal, &s));
	fft_grid_export(&g, mx, av, oc);

	for (int i = 0; i < 128; i++) {
		double d = bins[i] << e;

		sq += d * d;
	}

	/* fft_eval: noise + rssi + 20 log10(data) - 10 log10(sum data^2), + 10 log10(n) per 20 MHz */
	for (int i = 0; i < 128; i++) {
		double d = bins[i] << e;
		double f = 5210 - 44 + 88.0 * (i + 0.5) / 128;
		int c = (int)floor((f - g.start) / g.res);
		int ref = (int)lrint(-101 + 12 + 20 * log10(d ? d : 1) - 10 * log10(sq) + 10 * log10(128));
		int diff;

		/* the byte encoding floors at 1 - FFT_DBM_OFFSET */
		if (ref < 1 - FFT_DBM_OFFSET)
			ref = 1 - FFT_DBM_OFFSET;
		diff = abs(dbm(mx[c]) - ref);

		CHECK(g.cnt[c] == 1);
		if (diff > worst)
			worst = diff;
	}

	CHECK(worst <= 1);
	fft_grid_free(&g);
}

static void test_half_span_and_occupancy(void)
{
	struct fft_cal cal;
	uint8_t bins[64], buf[512], mx[256], av[256], oc[256];
	struct fft_grid g;
	struct fft_sample s;
	int used = 0, strong;
	uint32_t n;

	memset(bins, 2, sizeof(bins));
	bins[40] = 255;
	make_sample(buf, 2437, 22, -95, 25, 0, bins, 64);
	CHECK(fft_parse(buf, sizeof(buf), &s) > 0);
	fft_cal_init(&cal, -40);

	CHECK(!fft_grid_init(&g, 2427, 2447, 22.0 / 64 * 1.001, 4096));
	g.half_span = 10;
	CHECK(fft_grid_add(&g, &cal, &s));
	fft_grid_export(&g, mx, av, oc);

	for (int c = 0; c < g.n; c++)
		used += g.cnt[c];
	CHECK(used == 58);

	strong = (int)floor((2437 - 11 + 22.0 * 40.5 / 64 - g.start) / g.res);
	CHECK(oc[strong] == 100);
	CHECK(oc[strong - 5] == 0);
	CHECK(dbm(mx[strong]) > dbm(mx[strong - 5]) + 30);

	fft_grid_clear(&g);
	fft_grid_export(&g, mx, av, oc);
	CHECK(mx[0] == 0 && av[0] == 0 && oc[0] == FFT_OCC_NONE);

	/* no noise estimate: still measured by gain, just not used to calibrate */
	s.noise = 0;
	n = cal.n;
	CHECK(fft_grid_add(&g, &cal, &s) && cal.n == n);

	/* strong rssi with empty bins: a broken report, not a flat signal */
	memset(bins, 1, sizeof(bins));
	make_sample(buf, 2437, 22, -95, 30, 1, bins, 64);
	CHECK(fft_parse(buf, sizeof(buf), &s) > 0);
	CHECK(!fft_grid_add(&g, &cal, &s));
	fft_grid_free(&g);
}

static void test_chan(void)
{
	uint16_t g24[14], out[64];
	uint16_t g5[] = { 5180, 5200, 5220, 5240, 5260, 5280, 5300, 5320,
			  5500, 5520, 5540, 5745, 5765, 5785, 5805, 5825 };
	int n;

	CHECK(chan_number(2412) == 1 && chan_number(2484) == 14);
	CHECK(chan_number(5180) == 36 && chan_number(5825) == 165);

	CHECK(chan_center1(5180, 20) == 5180);
	CHECK(chan_center1(5180, 80) == 5210 && chan_center1(5240, 80) == 5210);
	CHECK(chan_center1(5260, 80) == 5290);
	CHECK(chan_center1(5260, 40) == 5270 && chan_center1(5280, 40) == 5270);
	CHECK(chan_center1(5745, 80) == 5775 && chan_center1(5745, 40) == 5755);
	CHECK(chan_center1(2412, 40) == 2422 && chan_center1(2462, 40) == 2452);
	CHECK(chan_center1(2412, 80) == 0 && chan_center1(5180, 160) == 0);

	for (int i = 0; i < 13; i++)
		g24[i] = 2412 + 5 * i;
	n = chan_sweep_cover(g24, 13, 10, out);
	CHECK(n == 4 && out[0] == 2412 && out[1] == 2432 && out[2] == 2452 && out[3] == 2472);

	g24[13] = 2484;
	n = chan_sweep_cover(g24, 14, 10, out);
	CHECK(n == 5 && out[4] == 2484);

	n = chan_sweep_cover(g5, ARRAY_LEN(g5), 10, out);
	CHECK(n == (int)ARRAY_LEN(g5) && !memcmp(out, g5, sizeof(g5)));

	n = chan_sweep_cover(g24, 1, 10, out);
	CHECK(n == 1 && out[0] == 2412);
}

static void test_chunks(void)
{
	uint16_t g24[13], g5[] = { 5180, 5200, 5220, 5240, 5260, 5280, 5300, 5320, 5500, 5520,
				   5540, 5560, 5580, 5600, 5620, 5640, 5660, 5680, 5700, 5720,
				   5745, 5765, 5785, 5805, 5825, 5845, 5865 };
	static const int c5[] = { 5210, 5290, 5530, 5610, 5690, 5775, 5835, 5865 };
	static const int w5[] = { 80, 80, 80, 80, 80, 80, 40, 20 };
	struct chan_chunk out[32];
	int n;

	for (int i = 0; i < 13; i++)
		g24[i] = 2412 + 5 * i;

	/* 2402-2482 MHz in two HT40 chunks, one HT40+ and one HT40- */
	n = chan_chunks(g24, 13, 80, out, 32);
	CHECK(n == 2);
	CHECK(out[0].center1 == 2422 && out[0].width == 40 && out[0].freq == 2412);
	CHECK(out[1].center1 == 2462 && out[1].width == 40);

	/* the MR33's 5 GHz list: ch 165-173 has no full 80 MHz block */
	n = chan_chunks(g5, ARRAY_LEN(g5), 80, out, 32);
	CHECK(n == (int)ARRAY_LEN(c5));
	for (int i = 0; i < n && i < (int)ARRAY_LEN(c5); i++)
		CHECK(out[i].center1 == c5[i] && out[i].width == w5[i]);

	n = chan_chunks(g5, 8, 20, out, 32);
	CHECK(n == 8 && out[0].width == 20 && out[7].center1 == 5320);

	n = chan_chunks(g5, 8, 40, out, 32);
	CHECK(n == 4 && out[0].center1 == 5190 && out[3].center1 == 5310);
}

static void test_gain_model(void)
{
	uint8_t bins[64], buf[512], mx[64], av[64], oc[64];
	struct fft_grid g;
	struct fft_cal cal;
	struct fft_sample s;
	float c;

	CHECK(!fft_grid_init(&g, 2401, 2423, 22.0 / 64 * 1.001, 4096));
	fft_cal_init(&cal, -40);

	/* a quiet sample sets the offset: -95 - 10 log10(64 * 16) + 80 + 10 log10(64) */
	memset(bins, 4, sizeof(bins));
	sample_gain = 80;
	make_sample(buf, 2412, 22, -95, 0, 0, bins, 64);
	CHECK(fft_parse(buf, sizeof(buf), &s) > 0 && s.gain == 80);
	CHECK(fft_grid_add(&g, &cal, &s));
	CHECK(fabsf(cal.c - (-95 - 10 * log10f(1024) + 80 + 10 * log10f(64))) < 0.01f);
	c = cal.c;

	/* strong signal: AGC drops 30 dB, rssi overstates by 10 dB; the gain model wins */
	fft_grid_clear(&g);
	memset(bins, 40, sizeof(bins));
	sample_gain = 50;
	make_sample(buf, 2412, 22, -95, 70, 0, bins, 64);
	CHECK(fft_parse(buf, sizeof(buf), &s) > 0);
	CHECK(fft_grid_add(&g, &cal, &s));
	CHECK(cal.c == c);
	fft_grid_export(&g, mx, av, oc);
	CHECK(abs(dbm(mx[10]) - (int)lrintf(10 * log10f(1600) - 50 + c)) <= 1);
	CHECK(dbm(mx[10]) < -95 + 70 - 5);

	sample_gain = 80;
	fft_grid_free(&g);
}

static void test_full_chunk_occupancy(void)
{
	uint8_t bins[64], buf[512], mx[64], av[64], oc[64];
	struct fft_grid g;
	struct fft_cal cal;
	struct fft_sample s;

	CHECK(!fft_grid_init(&g, 5764, 5786, 22.0 / 64 * 1.001, 4096));
	fft_cal_init(&cal, -40);

	/* quiet first, so the chunk learns its noise */
	memset(bins, 4, sizeof(bins));
	make_sample(buf, 5775, 22, -95, 0, 0, bins, 64);
	CHECK(fft_parse(buf, sizeof(buf), &s) > 0);
	CHECK(fft_grid_add(&g, &cal, &s));

	/* then a signal 30 dB up across every bin: its own median would hide it */
	fft_grid_clear(&g);
	memset(bins, 120, sizeof(bins));
	make_sample(buf, 5775, 22, -95, 30, 0, bins, 64);
	CHECK(fft_parse(buf, sizeof(buf), &s) > 0);
	CHECK(fft_grid_add(&g, &cal, &s));
	fft_grid_export(&g, mx, av, oc);
	CHECK(oc[30] == 100);

	fft_grid_free(&g);
}

int main(void)
{
	test_b64();
	test_parse();
	test_resync();
	test_flat_power();
	test_fft_eval_equivalence();
	test_half_span_and_occupancy();
	test_chan();
	test_chunks();
	test_gain_model();
	test_full_chunk_occupancy();

	if (failures)
		fprintf(stderr, "%d check(s) failed\n", failures);
	else
		printf("all tests passed\n");
	return !!failures;
}
