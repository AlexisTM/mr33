#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "spectral.h"

const char *spectral_debugfs = "/sys/kernel/debug/ieee80211";

bool spectral_supported(const char *phy)
{
	char path[256];

	snprintf(path, sizeof(path), "%s/%s/ath10k/spectral_scan_ctl", spectral_debugfs, phy);
	return access(path, W_OK) == 0;
}

int spectral_open(struct spectral *sp, const char *phy)
{
	char path[256];

	sp->fd = -1;
	snprintf(sp->dir, sizeof(sp->dir), "%s/%s/ath10k", spectral_debugfs, phy);

	if (!spectral_supported(phy))
		return -ENOTSUP;

	/* ath10k opens its relay channel as global: one file, not one per CPU */
	snprintf(path, sizeof(path), "%s/spectral_scan0", sp->dir);
	sp->fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

	return sp->fd < 0 ? -errno : 0;
}

void spectral_close(struct spectral *sp)
{
	if (sp->fd >= 0)
		close(sp->fd);
	sp->fd = -1;
}

static int write_file(const char *dir, const char *file, const char *val)
{
	char path[256];
	ssize_t len = strlen(val), r;
	int fd, err = 0;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	r = write(fd, val, len);
	if (r != len)
		err = r < 0 ? -errno : -EIO;

	close(fd);
	return err;
}

int spectral_ctl(struct spectral *sp, const char *cmd)
{
	return write_file(sp->dir, "spectral_scan_ctl", cmd);
}

int spectral_set_bins(struct spectral *sp, int bins)
{
	char val[16];

	snprintf(val, sizeof(val), "%d", bins);
	return write_file(sp->dir, "spectral_bins", val);
}

ssize_t spectral_read(struct spectral *sp, void *buf, size_t len)
{
	ssize_t r = read(sp->fd, buf, len);

	return r < 0 ? 0 : r;
}

void spectral_drain(struct spectral *sp)
{
	char buf[16384];

	/* the relay buffer is 1140 * 2500 bytes, this bounds the loop well above it */
	for (int i = 0; i < 1000; i++)
		if (spectral_read(sp, buf, sizeof(buf)) <= 0)
			break;
}
