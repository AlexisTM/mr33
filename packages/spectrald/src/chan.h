#ifndef SPECTRALD_CHAN_H
#define SPECTRALD_CHAN_H

#include <stdint.h>

struct chan_chunk {
	uint16_t freq;		/* primary 20 MHz channel to tune to */
	uint16_t center1;
	uint8_t width;
};

int chan_number(int freq);

/* Center of the 40/80 MHz block holding primary channel freq, 0 if there is none. */
int chan_center1(int freq, int width);

/*
 * Picks the fewest channels from sorted freqs whose [f - half, f + half]
 * spans cover every channel's span. Returns the count written to out.
 */
int chan_sweep_cover(const uint16_t *freqs, int n, double half, uint16_t *out);

/*
 * Covers sorted freqs (one band) with the fewest tunable blocks of at most
 * max_width MHz whose 20 MHz members are all enabled. Returns the count.
 */
int chan_chunks(const uint16_t *freqs, int n, int max_width, struct chan_chunk *out, int max);

#endif
