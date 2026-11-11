#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <netlink/attr.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/genl.h>
#include <netlink/msg.h>
#include <linux/nl80211.h>

#include "nl.h"

static struct nl_sock *cmd_sk, *ev_sk;
static struct nl_cb *ev_cb;
static int nl80211_id = -1;
static int ioctl_fd = -1;

static nl_event_cb ev_user_cb;
static void *ev_user_priv;

static int cb_error(struct sockaddr_nl *nla, struct nlmsgerr *err, void *arg)
{
	(void)nla;
	*(int *)arg = err->error;
	return NL_STOP;
}

static int cb_finish(struct nl_msg *msg, void *arg)
{
	(void)msg;
	*(int *)arg = 0;
	return NL_SKIP;
}

static int cb_ack(struct nl_msg *msg, void *arg)
{
	(void)msg;
	*(int *)arg = 0;
	return NL_STOP;
}

static int cb_no_seq(struct nl_msg *msg, void *arg)
{
	(void)msg;
	(void)arg;
	return NL_OK;
}

static struct nlattr **parse(struct nl_msg *msg, struct nlattr **tb)
{
	struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));

	nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
		  genlmsg_attrlen(gnlh, 0), NULL);
	return tb;
}

static struct nl_msg *msg_new(int cmd, int flags)
{
	struct nl_msg *msg = nlmsg_alloc();

	if (msg)
		genlmsg_put(msg, 0, 0, nl80211_id, 0, flags, cmd, 0);
	return msg;
}

static int msg_exec(struct nl_msg *msg, int (*valid)(struct nl_msg *, void *), void *arg)
{
	struct nl_cb *cb;
	int err = 1;

	if (!msg)
		return -ENOMEM;

	cb = nl_cb_alloc(NL_CB_DEFAULT);
	if (!cb) {
		nlmsg_free(msg);
		return -ENOMEM;
	}

	if (nl_send_auto_complete(cmd_sk, msg) < 0) {
		err = -EIO;
		goto out;
	}

	nl_cb_err(cb, NL_CB_CUSTOM, cb_error, &err);
	nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, cb_finish, &err);
	nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, cb_ack, &err);
	if (valid)
		nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, valid, arg);

	while (err > 0)
		if (nl_recvmsgs(cmd_sk, cb) < 0 && err > 0)
			err = -EIO;

out:
	nl_cb_put(cb);
	nlmsg_free(msg);
	return err;
}

int nl_init(void)
{
	int grp;

	ioctl_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	cmd_sk = nl_socket_alloc();
	ev_sk = nl_socket_alloc();
	ev_cb = nl_cb_alloc(NL_CB_DEFAULT);
	if (ioctl_fd < 0 || !cmd_sk || !ev_sk || !ev_cb)
		return -ENOMEM;

	if (genl_connect(cmd_sk) || genl_connect(ev_sk))
		return -ECONNREFUSED;

	nl80211_id = genl_ctrl_resolve(cmd_sk, "nl80211");
	if (nl80211_id < 0)
		return -ENOENT;

	grp = genl_ctrl_resolve_grp(ev_sk, "nl80211", "scan");
	if (grp >= 0)
		nl_socket_add_membership(ev_sk, grp);
	grp = genl_ctrl_resolve_grp(ev_sk, "nl80211", "config");
	if (grp >= 0)
		nl_socket_add_membership(ev_sk, grp);

	nl_socket_set_nonblocking(ev_sk);
	nl_cb_set(ev_cb, NL_CB_SEQ_CHECK, NL_CB_CUSTOM, cb_no_seq, NULL);

	return 0;
}

void nl_done(void)
{
	if (ev_cb)
		nl_cb_put(ev_cb);
	if (ev_sk)
		nl_socket_free(ev_sk);
	if (cmd_sk)
		nl_socket_free(cmd_sk);
	if (ioctl_fd >= 0)
		close(ioctl_fd);
	ev_cb = NULL;
	ev_sk = cmd_sk = NULL;
	ioctl_fd = -1;
}

struct phy_ctx {
	struct nl_phy *phys;
	int max;
	int n;
};

static struct nl_phy *phy_get(struct phy_ctx *ctx, int idx)
{
	struct nl_phy *p;

	for (int i = 0; i < ctx->n; i++)
		if (ctx->phys[i].idx == idx)
			return &ctx->phys[i];

	if (ctx->n >= ctx->max)
		return NULL;

	p = &ctx->phys[ctx->n++];
	memset(p, 0, sizeof(*p));
	p->idx = idx;
	return p;
}

static void phy_add_chan(struct nl_phy *p, int band, struct nlattr *attr)
{
	struct nlattr *tb[NL80211_FREQUENCY_ATTR_MAX + 1];
	struct nl_chan *c;
	uint32_t freq;

	nla_parse(tb, NL80211_FREQUENCY_ATTR_MAX, nla_data(attr), nla_len(attr), NULL);
	if (!tb[NL80211_FREQUENCY_ATTR_FREQ])
		return;

	freq = nla_get_u32(tb[NL80211_FREQUENCY_ATTR_FREQ]);
	for (int i = 0; i < p->n_chans; i++)
		if (p->chans[i].freq == freq)
			return;

	if (p->n_chans >= NL_MAX_CHANS)
		return;

	c = &p->chans[p->n_chans++];
	c->freq = freq;
	c->band = band;
	c->disabled = !!tb[NL80211_FREQUENCY_ATTR_DISABLED];
	c->radar = !!tb[NL80211_FREQUENCY_ATTR_RADAR];
}

static int phy_cb(struct nl_msg *msg, void *arg)
{
	struct nlattr *tb[NL80211_ATTR_MAX + 1];
	struct nlattr *band, *freq;
	struct nl_phy *p;
	int rem, rem2;

	parse(msg, tb);
	if (!tb[NL80211_ATTR_WIPHY])
		return NL_SKIP;

	p = phy_get(arg, nla_get_u32(tb[NL80211_ATTR_WIPHY]));
	if (!p)
		return NL_SKIP;

	if (tb[NL80211_ATTR_WIPHY_NAME])
		snprintf(p->name, sizeof(p->name), "%s",
			 nla_get_string(tb[NL80211_ATTR_WIPHY_NAME]));

	if (tb[NL80211_ATTR_EXT_FEATURES]) {
		const uint8_t *ft = nla_data(tb[NL80211_ATTR_EXT_FEATURES]);
		int len = nla_len(tb[NL80211_ATTR_EXT_FEATURES]);
		int bit = NL80211_EXT_FEATURE_SET_SCAN_DWELL;

		p->scan_dwell = bit / 8 < len && (ft[bit / 8] & (1 << (bit % 8)));
	}

	if (!tb[NL80211_ATTR_WIPHY_BANDS])
		return NL_SKIP;

	nla_for_each_nested(band, tb[NL80211_ATTR_WIPHY_BANDS], rem) {
		struct nlattr *tbb[NL80211_BAND_ATTR_MAX + 1];

		nla_parse(tbb, NL80211_BAND_ATTR_MAX, nla_data(band), nla_len(band), NULL);
		if (!tbb[NL80211_BAND_ATTR_FREQS])
			continue;

		nla_for_each_nested(freq, tbb[NL80211_BAND_ATTR_FREQS], rem2)
			phy_add_chan(p, nla_type(band), freq);
	}

	return NL_SKIP;
}

int nl_phys(struct nl_phy *phys, int max)
{
	struct phy_ctx ctx = { .phys = phys, .max = max };
	struct nl_msg *msg = msg_new(NL80211_CMD_GET_WIPHY, NLM_F_DUMP);
	int err;

	if (msg)
		nla_put_flag(msg, NL80211_ATTR_SPLIT_WIPHY_DUMP);

	err = msg_exec(msg, phy_cb, &ctx);
	return err < 0 ? err : ctx.n;
}

struct iface_ctx {
	struct nl_iface *ifs;
	int max;
	int n;
};

static int width_mhz(uint32_t w)
{
	switch (w) {
	case NL80211_CHAN_WIDTH_5:
		return 5;
	case NL80211_CHAN_WIDTH_10:
		return 10;
	case NL80211_CHAN_WIDTH_40:
		return 40;
	case NL80211_CHAN_WIDTH_80:
		return 80;
	case NL80211_CHAN_WIDTH_80P80:
	case NL80211_CHAN_WIDTH_160:
		return 160;
	default:
		return 20;
	}
}

static bool iface_is_up(const char *name)
{
	struct ifreq ifr = { 0 };

	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", name);
	if (ioctl(ioctl_fd, SIOCGIFFLAGS, &ifr))
		return false;
	return ifr.ifr_flags & IFF_UP;
}

static int iface_cb(struct nl_msg *msg, void *arg)
{
	struct nlattr *tb[NL80211_ATTR_MAX + 1];
	struct iface_ctx *ctx = arg;
	struct nl_iface *i;

	parse(msg, tb);
	if (!tb[NL80211_ATTR_IFINDEX] || !tb[NL80211_ATTR_WIPHY] || ctx->n >= ctx->max)
		return NL_SKIP;

	i = &ctx->ifs[ctx->n++];
	memset(i, 0, sizeof(*i));
	i->ifindex = nla_get_u32(tb[NL80211_ATTR_IFINDEX]);
	i->wiphy = nla_get_u32(tb[NL80211_ATTR_WIPHY]);

	if (tb[NL80211_ATTR_IFNAME])
		snprintf(i->name, sizeof(i->name), "%s", nla_get_string(tb[NL80211_ATTR_IFNAME]));
	if (tb[NL80211_ATTR_IFTYPE])
		i->type = nla_get_u32(tb[NL80211_ATTR_IFTYPE]);
	if (tb[NL80211_ATTR_WIPHY_FREQ])
		i->freq = nla_get_u32(tb[NL80211_ATTR_WIPHY_FREQ]);
	if (tb[NL80211_ATTR_CHANNEL_WIDTH])
		i->width = width_mhz(nla_get_u32(tb[NL80211_ATTR_CHANNEL_WIDTH]));
	if (tb[NL80211_ATTR_CENTER_FREQ1])
		i->center1 = nla_get_u32(tb[NL80211_ATTR_CENTER_FREQ1]);

	i->up = iface_is_up(i->name);
	return NL_SKIP;
}

int nl_ifaces(struct nl_iface *ifs, int max)
{
	struct iface_ctx ctx = { .ifs = ifs, .max = max };
	int err = msg_exec(msg_new(NL80211_CMD_GET_INTERFACE, NLM_F_DUMP), iface_cb, &ctx);

	return err < 0 ? err : ctx.n;
}

const char *nl_iftype_name(int type)
{
	switch (type) {
	case NL80211_IFTYPE_ADHOC:
		return "adhoc";
	case NL80211_IFTYPE_STATION:
		return "sta";
	case NL80211_IFTYPE_AP:
		return "ap";
	case NL80211_IFTYPE_AP_VLAN:
		return "ap_vlan";
	case NL80211_IFTYPE_WDS:
		return "wds";
	case NL80211_IFTYPE_MONITOR:
		return "monitor";
	case NL80211_IFTYPE_MESH_POINT:
		return "mesh";
	default:
		return "other";
	}
}

static int newif_cb(struct nl_msg *msg, void *arg)
{
	struct nlattr *tb[NL80211_ATTR_MAX + 1];

	parse(msg, tb);
	if (tb[NL80211_ATTR_IFINDEX])
		*(int *)arg = nla_get_u32(tb[NL80211_ATTR_IFINDEX]);
	return NL_SKIP;
}

int nl_iface_add(int wiphy, const char *name, int type)
{
	struct nl_msg *msg = msg_new(NL80211_CMD_NEW_INTERFACE, 0);
	int ifindex = 0, err;

	if (msg) {
		nla_put_u32(msg, NL80211_ATTR_WIPHY, wiphy);
		nla_put_string(msg, NL80211_ATTR_IFNAME, name);
		nla_put_u32(msg, NL80211_ATTR_IFTYPE, type);
	}

	err = msg_exec(msg, newif_cb, &ifindex);
	if (err < 0)
		return err;

	if (!ifindex)
		ifindex = if_nametoindex(name);
	return ifindex ? ifindex : -ENODEV;
}

int nl_iface_del(int ifindex)
{
	struct nl_msg *msg = msg_new(NL80211_CMD_DEL_INTERFACE, 0);

	if (msg)
		nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifindex);
	return msg_exec(msg, NULL, NULL);
}

int nl_iface_up(const char *name, bool up)
{
	struct ifreq ifr = { 0 };

	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", name);
	if (ioctl(ioctl_fd, SIOCGIFFLAGS, &ifr))
		return -errno;

	if (up)
		ifr.ifr_flags |= IFF_UP;
	else
		ifr.ifr_flags &= ~IFF_UP;

	return ioctl(ioctl_fd, SIOCSIFFLAGS, &ifr) ? -errno : 0;
}

int nl_set_channel(int ifindex, int freq, int width, int center1)
{
	struct nl_msg *msg = msg_new(NL80211_CMD_SET_WIPHY, 0);
	uint32_t w;

	switch (width) {
	case 40:
		w = NL80211_CHAN_WIDTH_40;
		break;
	case 80:
		w = NL80211_CHAN_WIDTH_80;
		break;
	default:
		w = NL80211_CHAN_WIDTH_20;
		center1 = freq;
	}

	if (msg) {
		nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifindex);
		nla_put_u32(msg, NL80211_ATTR_WIPHY_FREQ, freq);
		nla_put_u32(msg, NL80211_ATTR_CHANNEL_WIDTH, w);
		nla_put_u32(msg, NL80211_ATTR_CENTER_FREQ1, center1);
	}

	return msg_exec(msg, NULL, NULL);
}

int nl_scan_trigger(int ifindex, const uint16_t *freqs, int n, int dwell)
{
	struct nl_msg *msg = msg_new(NL80211_CMD_TRIGGER_SCAN, 0);
	struct nlattr *nest;

	/* No NL80211_ATTR_SCAN_SSIDS: a passive scan, nothing is transmitted */
	if (msg) {
		nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifindex);

		nest = nla_nest_start(msg, NL80211_ATTR_SCAN_FREQUENCIES);
		for (int i = 0; i < n; i++)
			nla_put_u32(msg, i + 1, freqs[i]);
		nla_nest_end(msg, nest);

		if (dwell > 0)
			nla_put_u16(msg, NL80211_ATTR_MEASUREMENT_DURATION, dwell);
	}

	return msg_exec(msg, NULL, NULL);
}

int nl_scan_abort(int ifindex)
{
	struct nl_msg *msg = msg_new(NL80211_CMD_ABORT_SCAN, 0);

	if (msg)
		nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifindex);
	return msg_exec(msg, NULL, NULL);
}

int nl_event_fd(void)
{
	return nl_socket_get_fd(ev_sk);
}

static int ev_valid(struct nl_msg *msg, void *arg)
{
	struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
	struct nlattr *tb[NL80211_ATTR_MAX + 1];
	int ifindex = 0;

	(void)arg;
	parse(msg, tb);
	if (tb[NL80211_ATTR_IFINDEX])
		ifindex = nla_get_u32(tb[NL80211_ATTR_IFINDEX]);

	if (ev_user_cb)
		ev_user_cb(gnlh->cmd, ifindex, ev_user_priv);
	return NL_SKIP;
}

void nl_event_read(nl_event_cb cb, void *priv)
{
	ev_user_cb = cb;
	ev_user_priv = priv;
	nl_cb_set(ev_cb, NL_CB_VALID, NL_CB_CUSTOM, ev_valid, NULL);

	/* nonblocking socket: stops with -NLE_AGAIN once drained */
	for (int i = 0; i < 64; i++)
		if (nl_recvmsgs(ev_sk, ev_cb) < 0)
			break;
}
