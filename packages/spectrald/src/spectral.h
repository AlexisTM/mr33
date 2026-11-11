#ifndef SPECTRALD_SPECTRAL_H
#define SPECTRALD_SPECTRAL_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

extern const char *spectral_debugfs;

/* ath10k debugfs controls for one phy */
struct spectral {
	char dir[192];
	int fd;
};

bool spectral_supported(const char *phy);
int spectral_open(struct spectral *sp, const char *phy);
void spectral_close(struct spectral *sp);

/* "background", "manual", "trigger" or "disable" */
int spectral_ctl(struct spectral *sp, const char *cmd);
int spectral_set_bins(struct spectral *sp, int bins);

/* Whatever the relay buffer holds, up to len bytes; never blocks. */
ssize_t spectral_read(struct spectral *sp, void *buf, size_t len);
void spectral_drain(struct spectral *sp);

#endif
