"""mr33-spectral: pull spectrum data from the MR33 without LuCI."""

import argparse
import csv
import getpass
import json
import math
import os
import sys
import time

from .client import Client, SpectralError
from .transport import UbusError

CHANNELS = {
    "2g": [(c, 2407 + 5 * c) for c in range(1, 14)],
    "5g": [(c, 5000 + 5 * c) for c in list(range(36, 65, 4)) + list(range(100, 145, 4)) + list(range(149, 178, 4))],
}


def connect(a):
    if a.url:
        password = os.environ.get(a.password_env, "") if a.password_env else None
        if password is None:
            password = getpass.getpass("password for %s: " % a.user)
        return Client.http(a.url, a.user, password)

    opts = list(a.ssh_opt or [])
    if a.via:
        opts.append("ProxyCommand=nc -6 %%h%%%%%s %%p" % a.via)
    return Client.ssh(a.ssh, opts, control_path=os.path.expanduser("~/.ssh/mr33spectral-%r@%h:%p"))


def start_params(a):
    p = {"mode": a.mode, "band": a.band, "phy": a.phy, "dwell": a.dwell, "lease": a.lease}
    if a.mode == "channel":
        p.pop("band")
        p.update(freq=a.freq, width=a.width)
    return p


def cmd_radios(c, a):
    for r in c.radios():
        used = [i["name"] for i in r.get("ifaces", []) if i.get("up") and not i.get("own")]
        print("%-5s %-11s spectral=%-5s bands=%-7s %s" % (
            r["phy"], r.get("driver", "?"), r.get("spectral"), "+".join(r.get("bands", {})),
            "in use by " + ",".join(used) if used else "idle"))


def cmd_status(c, a):
    print(json.dumps(c.status(), indent=2))


def cmd_stop(c, a):
    c.stop()


def cmd_record(c, a):
    out = open(a.out, "w", newline="") if a.out != "-" else sys.stdout
    writer = None
    if a.format == "csv":
        writer = csv.writer(out)
        writer.writerow(["t_ms", "seq", "band", "freq_mhz", "max_dbm", "avg_dbm", "occ_pct"])

    end = time.monotonic() + a.seconds if a.seconds else None
    n = 0
    with c.session(**start_params(a)) as frames:
        for f in frames:
            if a.format == "jsonl":
                out.write(json.dumps(f.to_dict()) + "\n")
            else:
                for fr, mx, av, oc in zip(f.freqs, f.max, f.avg, f.occ):
                    if not math.isnan(av):
                        writer.writerow([f.t_ms, f.seq, f.band, "%.3f" % fr, mx, av, "" if math.isnan(oc) else oc])
            n += 1
            if end and time.monotonic() >= end:
                break
    if out is not sys.stdout:
        out.close()
        print("%d frames written to %s" % (n, a.out), file=sys.stderr)


def bar(v, lo=-100, hi=-30, width=30):
    if math.isnan(v):
        return "-".ljust(width)
    k = max(0, min(width, int((v - lo) / (hi - lo) * width)))
    return ("#" * k).ljust(width)


def cmd_watch(c, a):
    last = {}
    shown = time.monotonic()
    with c.session(**start_params(a)) as frames:
        for f in frames:
            last[f.band] = last.get(f.band, {})
            for ch, mhz in CHANNELS.get(f.band, []):
                s = f.channel(mhz)
                if s:
                    last[f.band][ch] = s
            if time.monotonic() - shown < a.refresh:
                continue
            shown = time.monotonic()

            lines = ["\x1b[H\x1b[2J" + time.strftime("%H:%M:%S") + "  peak dBm/20 MHz per channel (max | occupancy)"]
            for band in sorted(last):
                lines.append("")
                lines.append("2.4 GHz" if band == "2g" else "5 GHz")
                for ch in sorted(last[band]):
                    s = last[band][ch]
                    occ = "" if math.isnan(s["occ"]) else "%3.0f%%" % s["occ"]
                    lines.append("  ch %3d %5.0f %s %s" % (ch, s["max"], bar(s["max"]), occ))
            print("\n".join(lines), flush=True)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="mr33-spectral", description=__doc__)
    conn = ap.add_argument_group("connection (SSH by default)")
    conn.add_argument("--ssh", default="mr33", help="ssh host or alias (default: mr33)")
    conn.add_argument("--via", help="reach a link-local alias through this interface, e.g. wlp59s0")
    conn.add_argument("--ssh-opt", action="append", help="extra ssh -o option, repeatable")
    conn.add_argument("--url", help="use HTTP instead, e.g. http://192.168.1.1 or http://[fe80::1%%25eth0]")
    conn.add_argument("--user", default="root")
    conn.add_argument("--password-env", help="read the HTTP password from this environment variable")

    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("radios", "status", "stop"):
        sub.add_parser(name)

    for name in ("record", "watch"):
        p = sub.add_parser(name)
        p.add_argument("--mode", default="full", choices=["full", "channel", "sweep"])
        p.add_argument("--band", default="all", choices=["2g", "5g", "all"])
        p.add_argument("--phy")
        p.add_argument("--dwell", type=int, metavar="MS",
                       help="ms per chunk or channel, at least 10 (default 150)")
        p.add_argument("--freq", type=int, help="channel mode on an idle radio, MHz")
        p.add_argument("--width", type=int, choices=[20, 40, 80])
        p.add_argument("--lease", type=int, default=15)
        if name == "record":
            p.add_argument("--seconds", type=float, default=10, help="0 = until Ctrl-C")
            p.add_argument("--format", default="jsonl", choices=["jsonl", "csv"])
            p.add_argument("--out", default="-", help="file, or - for stdout")
        else:
            p.add_argument("--refresh", type=float, default=1.0)

    a = ap.parse_args(argv)
    if a.cmd in ("record", "watch") and a.mode == "sweep" and a.band == "all":
        a.band = "2g"

    try:
        globals()["cmd_" + a.cmd](connect(a), a)
    except KeyboardInterrupt:
        pass
    except (SpectralError, UbusError, OSError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
