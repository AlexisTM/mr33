#ifndef SPECTRALD_FFT_H
#define SPECTRALD_FFT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Layout of struct fft_sample_ath10k from drivers/net/wireless/ath/spectral_common.h */
#define FFT_SAMPLE_ATH10K	3
#define FFT_ATH10K_HDR		29
#define FFT_MIN_BINS		64
#define FFT_MAX_BINS		256

struct fft_sample {
	uint16_t freq1;
	uint16_t freq2;
	uint8_t chan_width;	/* the driver reports 22/44/88 for 20/40/80 MHz */
	int16_t noise;
	int8_t rssi;
	uint8_t max_exp;
	uint16_t gain;		/* total_gain_db, the receiver AGC gain */
	uint64_t tsf;
	uint16_t n_bins;
	const uint8_t *bins;
};

/* >0: bytes used by one sample, 0: need more data, -1: no sample starts at p */
int fft_parse(const uint8_t *p, size_t len, struct fft_sample *s);

/*
 * Accumulates samples onto a fixed frequency grid, in linear mW per 20 MHz.
 * Encoded output: power byte = dBm + 160 (0 = no data), occupancy byte =
 * percent of bins above the noise PSD by occ_snr (255 = no data).
 */
struct fft_grid {
	double start;		/* MHz, low edge of cell 0 */
	double res;		/* MHz per cell */
	double half_span;	/* drop bins further than this from freq1, 0 = keep all */
	bool trim;		/* keep only the nominal channel, drop the ~10% skirts */
	int n;
	float occ_snr;
	float *max;
	float *sum;
	uint32_t *cnt;
	uint32_t *occ;
	uint32_t samples;
};

#define FFT_DBM_OFFSET	160
#define FFT_OCC_NONE	255

int fft_grid_init(struct fft_grid *g, double lo, double hi, double res, int max_cells);
void fft_grid_free(struct fft_grid *g);
void fft_grid_clear(struct fft_grid *g);
/*
 * dBm per 20 MHz = 10 log10(bin^2) - gain + c. The offset c is learnt from
 * quiet samples (AGC gain near its maximum), where noise + rssi is right;
 * a strong signal partly outside the FFT window inflates rssi, not the gain
 * model. Measured c: about -41.5 on QCA9887, -37.5 on IPQ4019.
 */
#define FFT_NOISE_REFS	64

/* Quiet-time noise of one tuned chunk, for occupancy */
struct fft_noise {
	uint16_t freq1;
	uint8_t cw;
	float ref;
};

struct fft_cal {
	float c;
	float gmax;
	uint32_t n;
	struct fft_noise noise[FFT_NOISE_REFS];
	int n_noise;
};

void fft_cal_init(struct fft_cal *cal, float c);
bool fft_grid_add(struct fft_grid *g, struct fft_cal *cal, const struct fft_sample *s);
void fft_grid_export(const struct fft_grid *g, uint8_t *max, uint8_t *avg, uint8_t *occ);

#endif
