#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubox/ulog.h>
#include <libubus.h>
#include <linux/nl80211.h>

#include "chan.h"
#include "fft.h"
#include "nl.h"
#include "spectral.h"
#include "util.h"

#define RING_LEN		32
#define MAX_CELLS		2048
#define MAX_REPLY_FRAMES	8
#define MAX_VIEWS		2
#define MAX_CHUNKS		64
#define READ_BUF		(256 * 1024)
#define SWEEP_HALF_SPAN		10.0
#define REGRID_AFTER		50
#define FULL_DWELL		150
#define IFACE_PREFIX		"spectral"
#define BAND_ALL		-2

enum mode { MODE_SWEEP, MODE_CHANNEL, MODE_FULL };

struct params {
	char phy[32];
	enum mode mode;
	int band;
	bool all_chans;
	int freq;
	int width;
	int bins;
	int dwell;
	int interval;
	int threshold;
	int lease;
};

struct frame {
	uint32_t seq;
	uint32_t t;
	uint32_t samples;
	uint8_t *max;
	uint8_t *avg;
	uint8_t *occ;
};

/* Samples that don't match the key are counted, then adopted if they persist */
struct grid_key {
	uint16_t freq1;		/* 0: any channel */
	uint8_t cw;		/* 0: accept every width (full mode) */
	uint16_t bins;
};

/* One band's picture: its own grid and frame history */
struct view {
	int band;
	double lo, hi;
	bool follow;		/* channel mode: grid centred on the samples' freq1 */
	struct grid_key key, cand;
	int cand_count;
	struct fft_grid grid;
	struct frame ring[RING_LEN];
	uint8_t *ring_mem;
};

static struct session {
	bool running;
	struct params req;	/* as asked, to recognise a second viewer */
	struct params p;	/* as applied */
	uint32_t gen;
	int wiphy;
	int ifindex;
	char ifname[IFNAMSIZ];
	bool own_iface;
	bool shared;
	int freq, width, center1;
	uint16_t freqs[NL_MAX_CHANS];
	int n_freqs;
	struct chan_chunk chunks[MAX_CHUNKS];
	int n_chunks, chunk_idx, cur_bins;

	struct spectral sp;
	struct fft_cal cal;
	struct view views[MAX_VIEWS];
	int n_views;
	uint32_t seq;

	int64_t t0, lease_until, last_tick, last_sample, last_trigger, scan_started;
	bool scanning;
	uint64_t samples, rejected, skipped;
	uint32_t tick_samples;
	uint32_t sweeps;
	double rate, sweep_ms;

	struct uloop_timeout frame_timer, scan_timer;
} s;

static char last_error[200];
static char stop_reason[32];
static uint8_t rbuf[READ_BUF];
static size_t rlen;
static struct blob_buf b;
static struct ubus_auto_conn conn;
static struct uloop_fd nl_ufd;

static void session_stop(const char *why);

static void session_fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(last_error, sizeof(last_error), fmt, ap);
	va_end(ap);

	ULOG_ERR("%s\n", last_error);
	session_stop("error");
}

static const char *band_name(int band)
{
	switch (band) {
	case NL_BAND_2G:
		return "2g";
	case NL_BAND_5G:
		return "5g";
	case NL_BAND_6G:
		return "6g";
	case BAND_ALL:
		return "all";
	default:
		return "60g";
	}
}

static int band_parse(const char *name)
{
	if (!strcmp(name, "2g"))
		return NL_BAND_2G;
	if (!strcmp(name, "5g"))
		return NL_BAND_5G;
	if (!strcmp(name, "all"))
		return BAND_ALL;
	return -1;
}

static int band_of(int freq)
{
	return freq < 4000 ? NL_BAND_2G : NL_BAND_5G;
}

/* Every view's history is dropped together, so clients only track one gen */
static void session_regen(void)
{
	s.gen++;
	s.seq = 0;
	for (int i = 0; i < s.n_views; i++)
		for (int j = 0; j < RING_LEN; j++)
			s.views[i].ring[j].seq = 0;
}

static void view_free(struct view *v)
{
	fft_grid_free(&v->grid);
	free(v->ring_mem);
	v->ring_mem = NULL;
}

static int view_setup(struct view *v, double lo, double hi, double res)
{
	int n;

	view_free(v);
	if (fft_grid_init(&v->grid, lo, hi, res, MAX_CELLS))
		return -ENOMEM;

	v->grid.occ_snr = powf(10.0f, s.p.threshold / 10.0f);
	v->grid.trim = s.p.mode == MODE_FULL;
	if (s.p.mode == MODE_SWEEP)
		v->grid.half_span = SWEEP_HALF_SPAN;

	n = v->grid.n;
	v->ring_mem = calloc(RING_LEN * 3, n);
	if (!v->ring_mem)
		return -ENOMEM;

	for (int i = 0; i < RING_LEN; i++) {
		v->ring[i].seq = 0;
		v->ring[i].max = v->ring_mem + (size_t)i * 3 * n;
		v->ring[i].avg = v->ring[i].max + n;
		v->ring[i].occ = v->ring[i].avg + n;
	}

	session_regen();
	return 0;
}

/* Cells slightly wider than a bin, so every cell gets at least one bin */
static double bin_res(int cw, int bins)
{
	return (double)cw / bins * 1.001;
}

static int view_for_key(struct view *v, const struct grid_key *k)
{
	/* full mode mixes widths at a constant 22 MHz / 64 bins */
	double res = k->cw ? bin_res(k->cw, k->bins) : bin_res(22, 64);

	if (v->follow)
		return view_setup(v, k->freq1 - k->cw / 2.0, k->freq1 + k->cw / 2.0, res);

	return view_setup(v, v->lo, v->hi, res);
}

static bool key_match(const struct grid_key *k, const struct fft_sample *smp)
{
	if (!k->cw)
		return true;

	return (!k->freq1 || k->freq1 == smp->freq1) &&
	       k->cw == smp->chan_width && k->bins == smp->n_bins;
}

static struct view *view_for(const struct fft_sample *smp)
{
	if (s.n_views == 1)
		return &s.views[0];

	for (int i = 0; i < s.n_views; i++)
		if (s.views[i].band == band_of(smp->freq1))
			return &s.views[i];

	return NULL;
}

static void handle_sample(const struct fft_sample *smp)
{
	struct view *v = view_for(smp);

	if (!v) {
		s.rejected++;
		return;
	}

	if (!key_match(&v->key, smp)) {
		struct grid_key k = {
			.freq1 = v->follow ? smp->freq1 : 0,
			.cw = smp->chan_width,
			.bins = smp->n_bins,
		};

		s.rejected++;
		if (memcmp(&k, &v->cand, sizeof(k))) {
			v->cand = k;
			v->cand_count = 0;
		}

		/* the firmware disagrees with what we expected: follow it */
		if (++v->cand_count < REGRID_AFTER)
			return;

		ULOG_INFO("regrid to %u MHz, width %u, %u bins\n", k.freq1, k.cw, k.bins);
		v->key = k;
		v->cand_count = 0;
		if (view_for_key(v, &k)) {
			session_fail("out of memory");
			return;
		}
	}

	v->cand_count = 0;
	if (fft_grid_add(&v->grid, &s.cal, smp)) {
		s.samples++;
		s.tick_samples++;
		s.last_sample = s.last_tick;
	} else {
		s.rejected++;
	}
}

static void read_samples(void)
{
	/* bounded per tick so a sample flood can't stall ubus */
	for (int round = 0; round < 16 && s.running; round++) {
		ssize_t r = spectral_read(&s.sp, rbuf + rlen, sizeof(rbuf) - rlen);
		size_t off = 0;

		if (r <= 0)
			break;

		rlen += r;
		while (off < rlen && s.running) {
			struct fft_sample smp;
			int n = fft_parse(rbuf + off, rlen - off, &smp);

			if (!n)
				break;
			if (n < 0) {
				s.skipped++;
				off++;
				continue;
			}

			off += n;
			handle_sample(&smp);
		}

		memmove(rbuf, rbuf + off, rlen - off);
		rlen -= off;
	}
}

static void push_frame(int64_t now)
{
	uint32_t seq = s.seq + 1;
	int64_t dt = now - s.last_tick;

	for (int i = 0; i < s.n_views; i++) {
		struct view *v = &s.views[i];
		struct frame *f = &v->ring[seq % RING_LEN];

		f->seq = seq;
		f->t = now - s.t0;
		f->samples = v->grid.samples;
		fft_grid_export(&v->grid, f->max, f->avg, f->occ);
		fft_grid_clear(&v->grid);
	}

	s.seq = seq;
	if (dt > 0)
		s.rate = s.rate * 0.8 + s.tick_samples * 1000.0 / dt * 0.2;
	s.tick_samples = 0;
}

static void watchdog(int64_t now)
{
	if (s.p.mode == MODE_SWEEP) {
		int dwell = s.p.dwell ? s.p.dwell : 150;

		if (s.scanning && now - s.scan_started > s.n_freqs * (dwell + 50) + 3000) {
			ULOG_WARN("scan on %s timed out, restarting\n", s.ifname);
			nl_scan_abort(s.ifindex);
			s.scanning = false;
			uloop_timeout_set(&s.scan_timer, 100);
		}
		return;
	}

	/* full mode retriggers on every hop */
	if (s.p.mode == MODE_FULL)
		return;

	/* vdev restarts (channel switch, DFS) silently drop the spectral config */
	if (now - s.last_sample > 3000 && now - s.last_trigger > 3000) {
		spectral_ctl(&s.sp, "trigger");
		s.last_trigger = now;
	}
}

static void frame_tick(struct uloop_timeout *t)
{
	int64_t now = now_ms();

	read_samples();
	if (!s.running)
		return;

	push_frame(now);
	s.last_tick = now;

	if (now > s.lease_until) {
		session_stop("idle");
		return;
	}

	watchdog(now);
	uloop_timeout_set(t, s.p.interval);
}

static void scan_kick(struct uloop_timeout *t)
{
	int err;

	if (!s.running || s.scanning)
		return;

	/* rearm every sweep, the firmware may drop spectral at scan end */
	spectral_ctl(&s.sp, "trigger");

	err = nl_scan_trigger(s.ifindex, s.freqs, s.n_freqs, s.p.dwell);
	if (err == -EBUSY) {
		uloop_timeout_set(t, 200);
		return;
	}
	if (err) {
		session_fail("scan on %s failed: %s", s.ifname, strerror(-err));
		return;
	}

	s.scanning = true;
	s.scan_started = now_ms();
}

/* Replaces a block the driver refused with its 20 MHz members */
static bool split_chunk(int i)
{
	struct chan_chunk c = s.chunks[i];
	int parts = c.width / 20;

	if (parts < 2 || s.n_chunks + parts - 1 > MAX_CHUNKS)
		return false;

	memmove(&s.chunks[i + parts], &s.chunks[i + 1],
		(s.n_chunks - i - 1) * sizeof(s.chunks[0]));
	for (int k = 0; k < parts; k++) {
		int f = c.center1 - c.width / 2 + 10 + 20 * k;

		s.chunks[i + k].freq = f;
		s.chunks[i + k].center1 = f;
		s.chunks[i + k].width = 20;
	}

	s.n_chunks += parts - 1;
	return true;
}

static void drop_chunk(int i)
{
	memmove(&s.chunks[i], &s.chunks[i + 1], (s.n_chunks - i - 1) * sizeof(s.chunks[0]));
	s.n_chunks--;
	if (s.chunk_idx >= s.n_chunks)
		s.chunk_idx = 0;
}

static void hop(struct uloop_timeout *t)
{
	int64_t now = now_ms();
	struct chan_chunk *c;
	int err, bins;

	if (!s.running)
		return;

	if (!s.chunk_idx) {
		if (s.scan_started) {
			double dur = now - s.scan_started;

			s.sweeps++;
			s.sweep_ms = s.sweeps == 1 ? dur : s.sweep_ms * 0.8 + dur * 0.2;
		}
		s.scan_started = now;
	}

	c = &s.chunks[s.chunk_idx];

	/* 64 bins per 20 MHz keeps 344 kHz cells whatever the width */
	bins = c->width / 20 * 64;
	if (bins != s.cur_bins && !spectral_set_bins(&s.sp, bins))
		s.cur_bins = bins;

	err = nl_set_channel(s.ifindex, c->freq, c->width, c->center1);
	if (err) {
		ULOG_WARN("%d MHz block at %d MHz refused: %s\n", c->width, c->center1, strerror(-err));
		if (!split_chunk(s.chunk_idx))
			drop_chunk(s.chunk_idx);
		if (!s.n_chunks) {
			session_fail("%s refused every channel", s.ifname);
			return;
		}
		uloop_timeout_set(t, 1);
		return;
	}

	spectral_ctl(&s.sp, "trigger");
	s.chunk_idx = (s.chunk_idx + 1) % s.n_chunks;
	uloop_timeout_set(t, s.p.dwell);
}

static void nl_event(int cmd, int ifindex, void *priv)
{
	int64_t dur;

	(void)priv;
	if (!s.running || ifindex != s.ifindex)
		return;

	switch (cmd) {
	case NL80211_CMD_NEW_SCAN_RESULTS:
	case NL80211_CMD_SCAN_ABORTED:
		if (!s.scanning)
			return;
		s.scanning = false;
		if (cmd == NL80211_CMD_NEW_SCAN_RESULTS) {
			dur = now_ms() - s.scan_started;
			s.sweeps++;
			s.sweep_ms = s.sweeps == 1 ? dur : s.sweep_ms * 0.8 + dur * 0.2;
		}
		uloop_timeout_set(&s.scan_timer, 1);
		break;
	case NL80211_CMD_DEL_INTERFACE:
		session_fail("interface %s was removed", s.ifname);
		break;
	}
}

static void nl_fd_cb(struct uloop_fd *fd, unsigned int events)
{
	(void)fd;
	(void)events;
	nl_event_read(nl_event, NULL);
}

static bool own_iface_name(const char *name)
{
	return !strncmp(name, IFACE_PREFIX, strlen(IFACE_PREFIX)) &&
	       isdigit((unsigned char)name[strlen(IFACE_PREFIX)]);
}

static void del_stale_ifaces(int wiphy)
{
	struct nl_iface ifs[NL_MAX_IFACES];
	int n = nl_ifaces(ifs, NL_MAX_IFACES);

	for (int i = 0; i < n; i++)
		if (own_iface_name(ifs[i].name) && (wiphy < 0 || ifs[i].wiphy == wiphy))
			nl_iface_del(ifs[i].ifindex);
}

static void session_stop(const char *why)
{
	if (!s.running)
		return;

	s.running = false;
	snprintf(stop_reason, sizeof(stop_reason), "%s", why);
	uloop_timeout_cancel(&s.frame_timer);
	uloop_timeout_cancel(&s.scan_timer);

	if (s.scanning)
		nl_scan_abort(s.ifindex);
	s.scanning = false;

	if (s.sp.fd >= 0) {
		spectral_ctl(&s.sp, "disable");
		spectral_drain(&s.sp);
		spectral_close(&s.sp);
	}

	if (s.own_iface) {
		nl_iface_up(s.ifname, false);
		nl_iface_del(s.ifindex);
		s.own_iface = false;
	}

	for (int i = 0; i < s.n_views; i++)
		view_free(&s.views[i]);
	rlen = 0;

	ULOG_INFO("stopped on %s (%s)\n", s.p.phy, why);
}

static int add_own_iface(int type)
{
	int ifindex;

	snprintf(s.ifname, sizeof(s.ifname), IFACE_PREFIX "%d", s.wiphy);
	del_stale_ifaces(s.wiphy);

	ifindex = nl_iface_add(s.wiphy, s.ifname, type);
	if (ifindex < 0)
		return ifindex;

	s.ifindex = ifindex;
	s.own_iface = true;

	/* first interface up boots the radio firmware, this can take a second */
	return nl_iface_up(s.ifname, true);
}

static int pick_bins(const struct params *p, int width)
{
	if (p->bins)
		return p->bins;
	if (width >= 80)
		return 256;
	if (width >= 40)
		return 128;
	return 64;
}

static const struct nl_iface *busy_iface(const struct nl_iface *ifs, int n, int wiphy)
{
	for (int i = 0; i < n; i++)
		if (ifs[i].wiphy == wiphy && ifs[i].up && !own_iface_name(ifs[i].name))
			return &ifs[i];
	return NULL;
}

static int band_chans(const struct nl_phy *ph, int band, uint16_t *out)
{
	int n = 0;

	/* nl80211 lists channels in ascending order, cover and chunks rely on it */
	for (int i = 0; i < ph->n_chans; i++)
		if (ph->chans[i].band == band && !ph->chans[i].disabled)
			out[n++] = ph->chans[i].freq;
	return n;
}

/* Full mode wants an idle radio; with no phy given, the one covering most bands */
static const struct nl_phy *pick_full_phy(const struct nl_phy *phys, int n_phys,
					  const struct nl_iface *ifs, int n_ifs, int band)
{
	const struct nl_phy *best = NULL;
	uint16_t tmp[NL_MAX_CHANS];
	int best_score = 0;

	for (int i = 0; i < n_phys; i++) {
		const struct nl_phy *ph = &phys[i];
		int score = 0;

		if (!spectral_supported(ph->name) || busy_iface(ifs, n_ifs, ph->idx))
			continue;

		for (int bd = NL_BAND_2G; bd <= NL_BAND_5G; bd++)
			if ((band == BAND_ALL || band == bd) && band_chans(ph, bd, tmp))
				score++;

		if (score > best_score) {
			best = ph;
			best_score = score;
		}
	}

	return best;
}

static int setup_full(const struct nl_phy *ph, char *err, size_t errlen)
{
	uint16_t list[NL_MAX_CHANS];

	for (int bd = NL_BAND_2G; bd <= NL_BAND_5G; bd++) {
		struct view *v;
		int n, first, k;

		if (s.p.band != BAND_ALL && s.p.band != bd)
			continue;

		n = band_chans(ph, bd, list);
		if (!n)
			continue;

		first = s.n_chunks;
		k = chan_chunks(list, n, 80, &s.chunks[first], MAX_CHUNKS - first);
		s.n_chunks += k;

		v = &s.views[s.n_views++];
		v->band = bd;
		v->lo = 1e9;
		v->hi = 0;
		for (int i = first; i < s.n_chunks; i++) {
			double lo = s.chunks[i].center1 - s.chunks[i].width / 2.0;
			double hi = s.chunks[i].center1 + s.chunks[i].width / 2.0;

			v->lo = lo < v->lo ? lo : v->lo;
			v->hi = hi > v->hi ? hi : v->hi;
		}
	}

	if (!s.n_chunks) {
		snprintf(err, errlen, "%s has no usable channels in that band", ph->name);
		return -EINVAL;
	}

	if (!s.p.dwell)
		s.p.dwell = FULL_DWELL;
	s.p.bins = 0;
	s.scan_timer.cb = hop;

	return add_own_iface(NL80211_IFTYPE_MONITOR);
}

static int session_start(const struct params *p, char *err, size_t errlen)
{
	struct nl_phy phys[NL_MAX_PHYS];
	struct nl_iface ifs[NL_MAX_IFACES];
	const struct nl_iface *busy;
	const struct nl_phy *ph = NULL;
	uint16_t all[NL_MAX_CHANS];
	int n_phys, n_ifs, n_all, ret;
	struct grid_key key = { 0 };
	uint32_t gen = s.gen;

	n_phys = nl_phys(phys, NL_MAX_PHYS);
	n_ifs = nl_ifaces(ifs, NL_MAX_IFACES);

	if (p->mode == MODE_FULL && !p->phy[0]) {
		ph = pick_full_phy(phys, n_phys, ifs, n_ifs, p->band);
		if (!ph) {
			snprintf(err, errlen, "no idle radio with spectral support: disable one in "
				 "/etc/config/wireless or use channel mode");
			return -EBUSY;
		}
	} else {
		for (int i = 0; i < n_phys; i++)
			if (!strcmp(phys[i].name, p->phy))
				ph = &phys[i];
	}

	if (!ph) {
		snprintf(err, errlen, "no radio named %s", p->phy);
		return -ENODEV;
	}

	if (!spectral_supported(ph->name)) {
		snprintf(err, errlen, "%s has no ath10k spectral debugfs: install the "
			 "kmods built with CONFIG_PACKAGE_ATH_SPECTRAL", ph->name);
		return -ENOTSUP;
	}

	memset(&s, 0, sizeof(s));
	s.gen = gen;
	s.sp.fd = -1;
	s.req = *p;
	s.p = *p;
	snprintf(s.p.phy, sizeof(s.p.phy), "%s", ph->name);
	s.wiphy = ph->idx;
	s.frame_timer.cb = frame_tick;
	s.scan_timer.cb = scan_kick;
	fft_cal_init(&s.cal, -40);
	last_error[0] = 0;

	busy = busy_iface(ifs, n_ifs, ph->idx);

	if ((p->mode == MODE_SWEEP || p->mode == MODE_FULL) && busy) {
		snprintf(err, errlen, "%s is in use by %s (%s): scanning would take it "
			 "off-channel, use channel mode or disable that radio",
			 ph->name, busy->name, nl_iftype_name(busy->type));
		return -EBUSY;
	}

	if (p->mode == MODE_FULL) {
		ret = setup_full(ph, err, errlen);
		/* nothing to undo when no channel was found */
		if (!s.n_chunks)
			return ret;
	} else if (p->mode == MODE_SWEEP) {
		if (p->band == BAND_ALL) {
			snprintf(err, errlen, "sweep covers one band, use full mode for both");
			return -EINVAL;
		}

		n_all = band_chans(ph, p->band, all);
		if (!n_all) {
			snprintf(err, errlen, "%s has no usable channels in that band", ph->name);
			return -EINVAL;
		}

		if (p->all_chans) {
			memcpy(s.freqs, all, n_all * sizeof(all[0]));
			s.n_freqs = n_all;
		} else {
			s.n_freqs = chan_sweep_cover(all, n_all, SWEEP_HALF_SPAN, s.freqs);
		}

		s.p.bins = pick_bins(p, 20);
		if (!ph->scan_dwell)
			s.p.dwell = 0;
		/* NL80211_ATTR_MEASUREMENT_DURATION is a u16 */
		if (s.p.dwell > 65535)
			s.p.dwell = 65535;
		key.cw = 22;
		key.bins = s.p.bins;

		s.n_views = 1;
		s.views[0].band = p->band;
		s.views[0].lo = s.freqs[0] - SWEEP_HALF_SPAN;
		s.views[0].hi = s.freqs[s.n_freqs - 1] + SWEEP_HALF_SPAN;
		s.views[0].key = key;

		ret = add_own_iface(NL80211_IFTYPE_STATION);
	} else {
		if (busy && busy->freq) {
			s.shared = true;
			s.ifindex = busy->ifindex;
			snprintf(s.ifname, sizeof(s.ifname), "%s", busy->name);
			s.freq = busy->freq;
			s.width = busy->width;
			s.center1 = busy->center1 ? busy->center1 : busy->freq;
			ret = 0;
		} else if (busy) {
			snprintf(err, errlen, "%s is up on %s but not on a channel yet",
				 busy->name, ph->name);
			return -EBUSY;
		} else {
			bool found = false;

			for (int i = 0; i < ph->n_chans; i++)
				if (ph->chans[i].freq == p->freq && !ph->chans[i].disabled)
					found = true;

			s.freq = p->freq;
			s.width = p->width ? p->width : 20;
			s.center1 = chan_center1(s.freq, s.width);
			if (!found || !s.center1) {
				snprintf(err, errlen, "no usable %d MHz channel at %d MHz on %s",
					 s.width, s.freq, ph->name);
				return -EINVAL;
			}

			ret = add_own_iface(NL80211_IFTYPE_MONITOR);
			if (!ret)
				ret = nl_set_channel(s.ifindex, s.freq, s.width, s.center1);
		}

		s.p.bins = pick_bins(p, s.width);
		/* the driver drops 80 MHz samples with 64 bins */
		if (s.width >= 80 && s.p.bins < 128)
			s.p.bins = 128;

		s.n_views = 1;
		s.views[0].band = band_of(s.freq);
		s.views[0].follow = true;
		s.views[0].key.freq1 = s.center1;
		s.views[0].key.cw = s.width * 11 / 10;
		s.views[0].key.bins = s.p.bins;
	}

	if (!ret)
		ret = spectral_open(&s.sp, ph->name);
	if (!ret) {
		spectral_ctl(&s.sp, "disable");
		spectral_drain(&s.sp);
		if (s.p.bins)
			ret = spectral_set_bins(&s.sp, s.p.bins);
	}
	for (int i = 0; !ret && i < s.n_views; i++)
		ret = view_for_key(&s.views[i], &s.views[i].key);
	if (!ret)
		ret = spectral_ctl(&s.sp, "background");
	if (!ret && p->mode != MODE_FULL)
		ret = spectral_ctl(&s.sp, "trigger");

	s.running = true;
	if (ret) {
		snprintf(err, errlen, "setting up %s failed: %s", ph->name, strerror(-ret));
		session_stop("error");
		return ret;
	}

	s.t0 = s.last_tick = s.last_sample = s.last_trigger = now_ms();
	s.lease_until = s.t0 + s.p.lease * 1000;
	uloop_timeout_set(&s.frame_timer, s.p.interval);
	if (p->mode != MODE_CHANNEL)
		uloop_timeout_set(&s.scan_timer, 1);

	ULOG_INFO("started %s on %s via %s, %d views\n",
		  p->mode == MODE_FULL ? "full" : p->mode == MODE_SWEEP ? "sweep" : "channel",
		  ph->name, s.ifname, s.n_views);
	return 0;
}

static const char *mode_name(enum mode m)
{
	switch (m) {
	case MODE_FULL:
		return "full";
	case MODE_SWEEP:
		return "sweep";
	default:
		return "channel";
	}
}

static void add_grid(const struct view *v)
{
	void *t = blobmsg_open_table(&b, "grid");

	blobmsg_add_double(&b, "start", v->grid.start);
	blobmsg_add_double(&b, "res", v->grid.res);
	blobmsg_add_u32(&b, "n", v->grid.n);
	blobmsg_close_table(&b, t);
}

static void add_b64(const char *name, const uint8_t *data, int n)
{
	char *buf = blobmsg_alloc_string_buffer(&b, name, base64_size(n));

	base64_encode(data, n, buf);
	blobmsg_add_string_buffer(&b);
}

static void add_frames(const struct view *v, uint32_t since)
{
	uint32_t first;
	void *a, *t;

	if (since > s.seq)
		since = 0;
	first = since + 1;
	if (s.seq >= MAX_REPLY_FRAMES && first + MAX_REPLY_FRAMES <= s.seq)
		first = s.seq - MAX_REPLY_FRAMES + 1;

	a = blobmsg_open_array(&b, "frames");
	for (uint32_t seq = first; seq && seq <= s.seq; seq++) {
		const struct frame *f = &v->ring[seq % RING_LEN];

		if (f->seq != seq)
			continue;

		t = blobmsg_open_table(&b, NULL);
		blobmsg_add_u32(&b, "seq", f->seq);
		blobmsg_add_u32(&b, "t", f->t);
		blobmsg_add_u32(&b, "samples", f->samples);
		add_b64("max", f->max, v->grid.n);
		add_b64("avg", f->avg, v->grid.n);
		add_b64("occ", f->occ, v->grid.n);
		blobmsg_close_table(&b, t);
	}
	blobmsg_close_array(&b, a);
}

static void add_status(bool frames, uint32_t since)
{
	void *a, *t;

	blobmsg_add_u8(&b, "running", s.running);
	blobmsg_add_u32(&b, "gen", s.gen);
	blobmsg_add_string(&b, "error", last_error);
	if (!s.running) {
		blobmsg_add_string(&b, "reason", stop_reason);
		return;
	}

	blobmsg_add_string(&b, "phy", s.p.phy);
	blobmsg_add_string(&b, "mode", mode_name(s.p.mode));
	blobmsg_add_string(&b, "iface", s.ifname);
	blobmsg_add_u8(&b, "shared", s.shared);
	blobmsg_add_u32(&b, "bins", s.p.mode == MODE_FULL ? s.cur_bins : s.p.bins);
	blobmsg_add_u32(&b, "interval", s.p.interval);
	blobmsg_add_u32(&b, "threshold", s.p.threshold);
	blobmsg_add_u32(&b, "lease", s.p.lease);

	if (s.p.mode == MODE_SWEEP || s.p.mode == MODE_FULL) {
		blobmsg_add_string(&b, "band", band_name(s.p.band));
		blobmsg_add_u32(&b, "dwell", s.p.dwell);
		blobmsg_add_u32(&b, "sweeps", s.sweeps);
		blobmsg_add_u32(&b, "sweep_ms", (uint32_t)s.sweep_ms);
	}

	if (s.p.mode == MODE_SWEEP) {
		blobmsg_add_u8(&b, "all", s.p.all_chans);
		a = blobmsg_open_array(&b, "freqs");
		for (int i = 0; i < s.n_freqs; i++)
			blobmsg_add_u32(&b, NULL, s.freqs[i]);
		blobmsg_close_array(&b, a);
	} else if (s.p.mode == MODE_FULL) {
		a = blobmsg_open_array(&b, "chunks");
		for (int i = 0; i < s.n_chunks; i++) {
			t = blobmsg_open_table(&b, NULL);
			blobmsg_add_u32(&b, "center1", s.chunks[i].center1);
			blobmsg_add_u32(&b, "width", s.chunks[i].width);
			blobmsg_close_table(&b, t);
		}
		blobmsg_close_array(&b, a);
	} else {
		blobmsg_add_u32(&b, "freq", s.freq);
		blobmsg_add_u32(&b, "width", s.width);
		blobmsg_add_u32(&b, "center1", s.center1);
	}

	blobmsg_add_u32(&b, "lease_left", s.lease_until > now_ms() ? s.lease_until - now_ms() : 0);
	blobmsg_add_u64(&b, "samples", s.samples);
	blobmsg_add_u64(&b, "rejected", s.rejected);
	blobmsg_add_u64(&b, "skipped", s.skipped);
	blobmsg_add_u32(&b, "rate", (uint32_t)(s.rate + 0.5));
	blobmsg_add_u32(&b, "seq", s.seq);

	a = blobmsg_open_array(&b, "views");
	for (int i = 0; i < s.n_views; i++) {
		t = blobmsg_open_table(&b, NULL);
		blobmsg_add_string(&b, "band", band_name(s.views[i].band));
		add_grid(&s.views[i]);
		if (frames)
			add_frames(&s.views[i], since);
		blobmsg_close_table(&b, t);
	}
	blobmsg_close_array(&b, a);
}

static int ubus_status(struct ubus_context *ctx, struct ubus_object *obj,
		       struct ubus_request_data *req, const char *method,
		       struct blob_attr *msg)
{
	blob_buf_init(&b, 0);
	add_status(false, 0);
	return ubus_send_reply(ctx, req, b.head);
}

enum { FRAMES_SINCE, __FRAMES_MAX };

static const struct blobmsg_policy frames_policy[__FRAMES_MAX] = {
	[FRAMES_SINCE] = { .name = "since", .type = BLOBMSG_TYPE_INT32 },
};

static int ubus_frames(struct ubus_context *ctx, struct ubus_object *obj,
		       struct ubus_request_data *req, const char *method,
		       struct blob_attr *msg)
{
	struct blob_attr *tb[__FRAMES_MAX];
	uint32_t since = 0;

	blobmsg_parse(frames_policy, __FRAMES_MAX, tb, blob_data(msg), blob_len(msg));
	if (tb[FRAMES_SINCE])
		since = blobmsg_get_u32(tb[FRAMES_SINCE]);

	/* polling is what keeps the session alive */
	if (s.running)
		s.lease_until = now_ms() + s.p.lease * 1000;

	blob_buf_init(&b, 0);
	add_status(true, since);
	return ubus_send_reply(ctx, req, b.head);
}

enum {
	START_PHY,
	START_MODE,
	START_BAND,
	START_ALL,
	START_FREQ,
	START_WIDTH,
	START_BINS,
	START_DWELL,
	START_INTERVAL,
	START_THRESHOLD,
	START_LEASE,
	__START_MAX
};

static const struct blobmsg_policy start_policy[__START_MAX] = {
	[START_PHY] = { .name = "phy", .type = BLOBMSG_TYPE_STRING },
	[START_MODE] = { .name = "mode", .type = BLOBMSG_TYPE_STRING },
	[START_BAND] = { .name = "band", .type = BLOBMSG_TYPE_STRING },
	[START_ALL] = { .name = "all", .type = BLOBMSG_TYPE_BOOL },
	[START_FREQ] = { .name = "freq", .type = BLOBMSG_TYPE_INT32 },
	[START_WIDTH] = { .name = "width", .type = BLOBMSG_TYPE_INT32 },
	[START_BINS] = { .name = "bins", .type = BLOBMSG_TYPE_INT32 },
	[START_DWELL] = { .name = "dwell", .type = BLOBMSG_TYPE_INT32 },
	[START_INTERVAL] = { .name = "interval", .type = BLOBMSG_TYPE_INT32 },
	[START_THRESHOLD] = { .name = "threshold", .type = BLOBMSG_TYPE_INT32 },
	[START_LEASE] = { .name = "lease", .type = BLOBMSG_TYPE_INT32 },
};

static int get_int(struct blob_attr *a, int def, int lo, int hi, bool *ok)
{
	int v;

	if (!a)
		return def;

	v = (int32_t)blobmsg_get_u32(a);
	if (v < lo || v > hi)
		*ok = false;
	return v;
}

static int reply_error(struct ubus_context *ctx, struct ubus_request_data *req,
		       const char *msg)
{
	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "ok", false);
	blobmsg_add_string(&b, "error", msg);
	return ubus_send_reply(ctx, req, b.head);
}

static bool params_equal(const struct params *a, const struct params *b)
{
	return !strcmp(a->phy, b->phy) && a->mode == b->mode && a->band == b->band &&
	       a->all_chans == b->all_chans && a->freq == b->freq && a->width == b->width &&
	       a->bins == b->bins && a->dwell == b->dwell && a->interval == b->interval &&
	       a->threshold == b->threshold;
}

static int ubus_start(struct ubus_context *ctx, struct ubus_object *obj,
		      struct ubus_request_data *req, const char *method,
		      struct blob_attr *msg)
{
	struct blob_attr *tb[__START_MAX];
	struct params p;
	char err[200];
	bool ok = true;

	memset(&p, 0, sizeof(p));
	blobmsg_parse(start_policy, __START_MAX, tb, blob_data(msg), blob_len(msg));

	p.mode = MODE_FULL;
	if (tb[START_MODE]) {
		const char *m = blobmsg_get_string(tb[START_MODE]);

		if (!strcmp(m, "channel"))
			p.mode = MODE_CHANNEL;
		else if (!strcmp(m, "sweep"))
			p.mode = MODE_SWEEP;
		else if (strcmp(m, "full"))
			return reply_error(ctx, req, "mode must be full, sweep or channel");
	}

	if (tb[START_PHY])
		snprintf(p.phy, sizeof(p.phy), "%s", blobmsg_get_string(tb[START_PHY]));
	else if (p.mode != MODE_FULL)
		return reply_error(ctx, req, "phy is required");

	p.band = p.mode == MODE_FULL ? BAND_ALL : NL_BAND_2G;
	if (tb[START_BAND] && (p.band = band_parse(blobmsg_get_string(tb[START_BAND]))) == -1)
		return reply_error(ctx, req, "band must be 2g, 5g or all");

	p.all_chans = tb[START_ALL] && blobmsg_get_bool(tb[START_ALL]);
	p.freq = get_int(tb[START_FREQ], 0, 0, 7200, &ok);
	p.width = get_int(tb[START_WIDTH], 20, 20, 80, &ok);
	p.bins = get_int(tb[START_BINS], 0, 0, FFT_MAX_BINS, &ok);
	p.dwell = get_int(tb[START_DWELL], 0, 0, INT_MAX, &ok);
	p.interval = get_int(tb[START_INTERVAL], 100, 50, 2000, &ok);
	p.threshold = get_int(tb[START_THRESHOLD], 10, 0, 40, &ok);
	p.lease = get_int(tb[START_LEASE], 10, 2, 3600, &ok);

	if (!ok)
		return reply_error(ctx, req, "parameter out of range");
	if (p.width != 20 && p.width != 40 && p.width != 80)
		return reply_error(ctx, req, "width must be 20, 40 or 80");
	if (p.bins && p.bins != 64 && p.bins != 128 && p.bins != 256)
		return reply_error(ctx, req, "bins must be 64, 128 or 256");
	if (p.dwell && p.dwell < 10)
		return reply_error(ctx, req, "dwell must be at least 10 ms");

	/* a second viewer asking for the same thing joins the running session */
	if (s.running) {
		if (params_equal(&s.req, &p)) {
			s.lease_until = now_ms() + s.p.lease * 1000;
			goto reply;
		}
		session_stop("restart");
	}

	if (session_start(&p, err, sizeof(err)) < 0) {
		snprintf(last_error, sizeof(last_error), "%s", err);
		return reply_error(ctx, req, err);
	}

reply:
	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "ok", true);
	add_status(false, 0);
	return ubus_send_reply(ctx, req, b.head);
}

static int ubus_stop(struct ubus_context *ctx, struct ubus_object *obj,
		     struct ubus_request_data *req, const char *method,
		     struct blob_attr *msg)
{
	session_stop("stopped");
	return ubus_status(ctx, obj, req, method, msg);
}

static void add_driver(const char *phy)
{
	char path[128], link[PATH_MAX];
	ssize_t n;

	snprintf(path, sizeof(path), "/sys/class/ieee80211/%s/device/driver", phy);
	n = readlink(path, link, sizeof(link) - 1);
	if (n < 0)
		return;

	link[n] = 0;
	blobmsg_add_string(&b, "driver", basename(link));
}

static int ubus_radios(struct ubus_context *ctx, struct ubus_object *obj,
		       struct ubus_request_data *req, const char *method,
		       struct blob_attr *msg)
{
	struct nl_phy phys[NL_MAX_PHYS];
	struct nl_iface ifs[NL_MAX_IFACES];
	int n_phys = nl_phys(phys, NL_MAX_PHYS);
	int n_ifs = nl_ifaces(ifs, NL_MAX_IFACES);
	void *arr, *t, *bands, *list, *ia, *it;

	blob_buf_init(&b, 0);
	arr = blobmsg_open_array(&b, "radios");

	for (int i = 0; i < n_phys; i++) {
		const struct nl_phy *ph = &phys[i];
		bool busy = false;

		t = blobmsg_open_table(&b, NULL);
		blobmsg_add_string(&b, "phy", ph->name);
		add_driver(ph->name);
		blobmsg_add_u8(&b, "spectral", spectral_supported(ph->name));
		blobmsg_add_u8(&b, "dwell", ph->scan_dwell);

		bands = blobmsg_open_table(&b, "bands");
		for (int band = NL_BAND_2G; band <= NL_BAND_5G; band++) {
			bool any = false;

			for (int c = 0; c < ph->n_chans; c++)
				any |= ph->chans[c].band == band && !ph->chans[c].disabled;
			if (!any)
				continue;

			list = blobmsg_open_array(&b, band_name(band));
			for (int c = 0; c < ph->n_chans; c++)
				if (ph->chans[c].band == band && !ph->chans[c].disabled)
					blobmsg_add_u32(&b, NULL, ph->chans[c].freq);
			blobmsg_close_array(&b, list);
		}
		blobmsg_close_table(&b, bands);

		list = blobmsg_open_array(&b, "radar");
		for (int c = 0; c < ph->n_chans; c++)
			if (ph->chans[c].radar && !ph->chans[c].disabled)
				blobmsg_add_u32(&b, NULL, ph->chans[c].freq);
		blobmsg_close_array(&b, list);

		ia = blobmsg_open_array(&b, "ifaces");
		for (int j = 0; j < n_ifs; j++) {
			const struct nl_iface *f = &ifs[j];
			bool own = own_iface_name(f->name);

			if (f->wiphy != ph->idx)
				continue;

			busy |= f->up && !own;
			it = blobmsg_open_table(&b, NULL);
			blobmsg_add_string(&b, "name", f->name);
			blobmsg_add_string(&b, "type", nl_iftype_name(f->type));
			blobmsg_add_u8(&b, "up", f->up);
			blobmsg_add_u8(&b, "own", own);
			if (f->freq) {
				blobmsg_add_u32(&b, "freq", f->freq);
				blobmsg_add_u32(&b, "width", f->width);
				blobmsg_add_u32(&b, "center1", f->center1);
			}
			blobmsg_close_table(&b, it);
		}
		blobmsg_close_array(&b, ia);

		blobmsg_add_u8(&b, "busy", busy);
		blobmsg_close_table(&b, t);
	}

	blobmsg_close_array(&b, arr);
	return ubus_send_reply(ctx, req, b.head);
}

static const struct ubus_method methods[] = {
	UBUS_METHOD_NOARG("radios", ubus_radios),
	UBUS_METHOD_NOARG("status", ubus_status),
	UBUS_METHOD("start", ubus_start, start_policy),
	UBUS_METHOD_NOARG("stop", ubus_stop),
	UBUS_METHOD("frames", ubus_frames, frames_policy),
};

static struct ubus_object_type obj_type = UBUS_OBJECT_TYPE("spectral", methods);

static struct ubus_object obj = {
	.name = "spectral",
	.type = &obj_type,
	.methods = methods,
	.n_methods = ARRAY_LEN(methods),
};

static void ubus_connected(struct ubus_context *ctx)
{
	int err = ubus_add_object(ctx, &obj);

	if (err)
		ULOG_ERR("cannot publish ubus object: %s\n", ubus_strerror(err));
}

int main(int argc, char **argv)
{
	int channels = ULOG_SYSLOG, opt, err;
	const char *sock = NULL;

	while ((opt = getopt(argc, argv, "d:s:v")) != -1) {
		switch (opt) {
		case 'd':
			spectral_debugfs = optarg;
			break;
		case 's':
			sock = optarg;
			break;
		case 'v':
			channels |= ULOG_STDIO;
			break;
		default:
			fprintf(stderr, "usage: %s [-d debugfs_ieee80211_dir] [-s ubus_socket] [-v]\n", argv[0]);
			return 1;
		}
	}

	ulog_open(channels, LOG_DAEMON, "spectrald");

	err = nl_init();
	if (err) {
		ULOG_ERR("nl80211 unavailable: %s\n", strerror(-err));
		return 1;
	}

	s.sp.fd = -1;
	del_stale_ifaces(-1);

	uloop_init();

	nl_ufd.fd = nl_event_fd();
	nl_ufd.cb = nl_fd_cb;
	uloop_fd_add(&nl_ufd, ULOOP_READ);

	conn.path = sock;
	conn.cb = ubus_connected;
	ubus_auto_connect(&conn);

	uloop_run();

	session_stop("shutdown");
	uloop_fd_delete(&nl_ufd);
	ubus_auto_shutdown(&conn);
	uloop_done();
	nl_done();
	blob_buf_free(&b);

	return 0;
}
