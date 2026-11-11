"""Client for spectrald, the MR33 spectrum analyzer daemon."""

import base64
import math
import time
from contextlib import contextmanager
from dataclasses import dataclass, field

from .transport import HttpTransport, SshTransport

DBM_OFFSET = 160
OCC_NONE = 255
NAN = float("nan")


class SpectralError(Exception):
    pass


@dataclass
class Grid:
    start: float
    res: float
    n: int

    @property
    def freqs(self):
        """Centre frequency of each cell, MHz."""
        return [self.start + (i + 0.5) * self.res for i in range(self.n)]

    def index(self, mhz):
        i = int((mhz - self.start) // self.res)
        return i if 0 <= i < self.n else None


@dataclass
class Frame:
    """One band over one frame interval (100 ms by default).

    Levels are dBm per 20 MHz, NaN where the radio didn't visit that cell
    during the interval (normal in full-spectrum mode, which hops).
    """

    seq: int
    t_ms: int
    band: str
    grid: Grid
    samples: int
    max: list = field(repr=False)
    avg: list = field(repr=False)
    occ: list = field(repr=False)

    @property
    def freqs(self):
        return self.grid.freqs

    def channel(self, center_mhz, width=20):
        """avg/max/occupancy over one channel's cells, NaN-free means."""
        lo, hi = center_mhz - width / 2, center_mhz + width / 2
        idx = [i for i, f in enumerate(self.freqs) if lo <= f <= hi and not math.isnan(self.avg[i])]
        if not idx:
            return None
        mean = lambda xs: sum(xs) / len(xs)
        return {
            "avg": mean([self.avg[i] for i in idx]),
            "max": max(self.max[i] for i in idx),
            "occ": mean([self.occ[i] for i in idx if not math.isnan(self.occ[i])] or [NAN]),
        }

    def to_dict(self):
        """JSON-ready: missing cells become None."""
        clean = lambda xs: [None if math.isnan(x) else x for x in xs]
        return {"seq": self.seq, "t_ms": self.t_ms, "band": self.band, "samples": self.samples,
                "grid": {"start": self.grid.start, "res": self.grid.res, "n": self.grid.n},
                "max": clean(self.max), "avg": clean(self.avg), "occ": clean(self.occ)}


def _levels(b64):
    return [b - DBM_OFFSET if b else NAN for b in base64.b64decode(b64)]


def _occ(b64):
    return [NAN if b == OCC_NONE else float(b) for b in base64.b64decode(b64)]


def decode_frames(reply):
    """Frames of a `frames` reply, oldest first, across all bands."""
    out = []
    for v in reply.get("views", []):
        g = Grid(v["grid"]["start"], v["grid"]["res"], v["grid"]["n"])
        for f in v.get("frames", []):
            out.append(Frame(f["seq"], f["t"], v["band"], g, f["samples"],
                             _levels(f["max"]), _levels(f["avg"]), _occ(f["occ"])))
    out.sort(key=lambda f: (f.seq, f.band))
    return out


class Client:
    def __init__(self, transport):
        self.t = transport

    @classmethod
    def http(cls, url, user="root", password="", timeout=10):
        return cls(HttpTransport(url, user, password, timeout))

    @classmethod
    def ssh(cls, host, ssh_options=(), timeout=20, control_path=None):
        return cls(SshTransport(host, ssh_options, timeout, control_path))

    def _call(self, method, args=None):
        return self.t.call("spectral", method, args)

    def radios(self):
        return self._call("radios").get("radios", [])

    def status(self):
        return self._call("status")

    def start(self, mode="full", **params):
        """Starts (or joins) a session. Params as in the README: band, phy, dwell, freq, width, lease..."""
        args = {"mode": mode}
        args.update({k: v for k, v in params.items() if v is not None})
        reply = self._call("start", args)
        if not reply.get("ok"):
            raise SpectralError(reply.get("error") or "start failed")
        return reply

    def stop(self):
        return self._call("stop")

    def frames(self, since=0):
        return self._call("frames", {"since": since})

    def stream(self, interval=0.1, restart_idle=True, **start_params):
        """Yields Frames as they arrive; polling keeps the session alive.

        With start_params the session is (re)started first; otherwise it
        follows whatever session is running. Ends when the session stops,
        except for an idle timeout, which is restarted if restart_idle.
        """
        if start_params:
            self.start(**start_params)

        since, gen = 0, None
        while True:
            t0 = time.monotonic()
            reply = self.frames(since)

            if not reply.get("running"):
                if reply.get("reason") == "idle" and restart_idle and start_params:
                    self.start(**start_params)
                    since, gen = 0, None
                    continue
                if reply.get("error"):
                    raise SpectralError(reply["error"])
                return

            # a new gen means new grids and a seq counter starting over
            if reply.get("gen") != gen:
                gen, since = reply.get("gen"), 0

            # the daemon only returns frames newer than since
            yield from decode_frames(reply)
            since = max(since, reply.get("seq", since))

            time.sleep(max(0.0, interval - (time.monotonic() - t0)))

    @contextmanager
    def session(self, interval=0.1, **start_params):
        """with client.session(band="5g") as frames: for f in frames: ..."""
        try:
            yield self.stream(interval=interval, **start_params)
        finally:
            try:
                self.stop()
            except Exception:
                pass
