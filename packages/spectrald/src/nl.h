#ifndef SPECTRALD_NL_H
#define SPECTRALD_NL_H

#include <net/if.h>
#include <stdbool.h>
#include <stdint.h>

#define NL_MAX_CHANS	96
#define NL_MAX_PHYS	8
#define NL_MAX_IFACES	32

enum nl_band { NL_BAND_2G, NL_BAND_5G, NL_BAND_60G, NL_BAND_6G };

struct nl_chan {
	uint16_t freq;
	uint8_t band;
	bool disabled;
	bool radar;
};

struct nl_phy {
	int idx;
	char name[32];
	bool scan_dwell;
	int n_chans;
	struct nl_chan chans[NL_MAX_CHANS];
};

struct nl_iface {
	int ifindex;
	int wiphy;
	int type;	/* enum nl80211_iftype */
	char name[IFNAMSIZ];
	bool up;
	int freq;	/* 0 when not on a channel */
	int width;	/* MHz */
	int center1;
};

int nl_init(void);
void nl_done(void);

int nl_phys(struct nl_phy *phys, int max);
int nl_ifaces(struct nl_iface *ifs, int max);
const char *nl_iftype_name(int type);

/* Returns the new ifindex or -errno */
int nl_iface_add(int wiphy, const char *name, int type);
int nl_iface_del(int ifindex);
int nl_iface_up(const char *name, bool up);

int nl_set_channel(int ifindex, int freq, int width, int center1);
/* Passive scan of freqs; dwell in ms, 0 for the driver default */
int nl_scan_trigger(int ifindex, const uint16_t *freqs, int n, int dwell);
int nl_scan_abort(int ifindex);

/* Event socket for scan and interface notifications */
int nl_event_fd(void);
typedef void (*nl_event_cb)(int cmd, int ifindex, void *priv);
void nl_event_read(nl_event_cb cb, void *priv);

#endif
