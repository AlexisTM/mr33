import base64
import http.server
import json
import math
import os
import shlex
import sys
import threading
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from mr33spectral import Client, HttpTransport, SpectralError, SshTransport, UbusError, decode_frames  # noqa: E402


def b64(vals):
    return base64.b64encode(bytes(vals)).decode()


def view(band, start, frames, n=4, res=0.5):
    return {"band": band, "grid": {"start": start, "res": res, "n": n}, "frames": frames}


def frame(seq, mx, av, oc):
    return {"seq": seq, "t": seq * 100, "samples": 10, "max": b64(mx), "avg": b64(av), "occ": b64(oc)}


class FakeTransport:
    def __init__(self, replies):
        self.replies = list(replies)
        self.calls = []

    def call(self, obj, method, args=None):
        self.calls.append((method, args))
        if method == "start":
            return {"ok": True}
        if method == "stop":
            return {"running": False}
        return self.replies.pop(0)


class DecodeTest(unittest.TestCase):
    def test_levels_and_gaps(self):
        reply = {"views": [view("5g", 5170, [frame(3, [60, 0, 100, 90], [56, 0, 70, 66], [0, 255, 50, 100])])]}
        f, = decode_frames(reply)
        self.assertEqual((f.band, f.seq, f.t_ms), ("5g", 3, 300))
        self.assertEqual(f.max[0], -100)
        self.assertTrue(math.isnan(f.max[1]) and math.isnan(f.occ[1]))
        self.assertEqual(f.avg[2], -90)
        self.assertEqual(f.freqs[0], 5170.25)
        self.assertEqual(f.grid.index(5171.2), 2)
        self.assertIsNone(f.grid.index(5100))

    def test_channel_summary_and_json(self):
        reply = {"views": [view("2g", 2427, [frame(1, [60, 70, 80, 0], [50, 60, 70, 0], [0, 10, 20, 255])], n=4, res=5)]}
        f, = decode_frames(reply)
        s = f.channel(2437, width=20)
        self.assertEqual(s["max"], -80)
        self.assertAlmostEqual(s["avg"], -100)
        self.assertAlmostEqual(s["occ"], 10)
        d = json.loads(json.dumps(f.to_dict()))
        self.assertIsNone(d["max"][3])

    def test_frames_sorted_across_bands(self):
        reply = {"views": [view("2g", 2402, [frame(2, [1] * 4, [1] * 4, [0] * 4), frame(3, [1] * 4, [1] * 4, [0] * 4)]),
                           view("5g", 5170, [frame(2, [1] * 4, [1] * 4, [0] * 4)])]}
        self.assertEqual([(f.seq, f.band) for f in decode_frames(reply)], [(2, "2g"), (2, "5g"), (3, "2g")])


class StreamTest(unittest.TestCase):
    def test_follows_seq_and_gen_then_ends(self):
        fr = lambda s: frame(s, [60] * 4, [60] * 4, [0] * 4)
        t = FakeTransport([
            {"running": True, "gen": 1, "seq": 2, "views": [view("2g", 2402, [fr(1), fr(2)])]},
            {"running": True, "gen": 1, "seq": 3, "views": [view("2g", 2402, [fr(3)])]},
            {"running": True, "gen": 2, "seq": 1, "views": [view("5g", 5170, [fr(1)])]},
            {"running": False, "reason": "stopped", "error": ""},
        ])
        got = [(f.band, f.seq) for f in Client(t).stream(interval=0, band="all")]
        self.assertEqual(got, [("2g", 1), ("2g", 2), ("2g", 3), ("5g", 1)])
        self.assertEqual(t.calls[0], ("start", {"mode": "full", "band": "all"}))
        self.assertEqual([a["since"] for m, a in t.calls if m == "frames"], [0, 2, 3, 1])

    def test_idle_restarts_and_error_raises(self):
        # following someone else's session: an idle stop just ends the stream
        t = FakeTransport([{"running": False, "reason": "idle"}])
        self.assertEqual(list(Client(t).stream(interval=0)), [])
        t = FakeTransport([{"running": False, "reason": "idle"},
                           {"running": False, "reason": "error", "error": "boom"}])
        with self.assertRaises(SpectralError):
            list(Client(t).stream(interval=0, band="5g"))
        self.assertEqual([m for m, _ in t.calls].count("start"), 2)

    def test_start_failure(self):
        class T(FakeTransport):
            def call(self, obj, method, args=None):
                return {"ok": False, "error": "no idle radio"}
        with self.assertRaisesRegex(SpectralError, "no idle radio"):
            Client(T([])).start()

    def test_session_stops(self):
        t = FakeTransport([{"running": False, "reason": "stopped"}])
        with Client(t).session(interval=0) as frames:
            list(frames)
        self.assertEqual(t.calls[-1][0], "stop")


class FakeUbus(http.server.BaseHTTPRequestHandler):
    sessions = set()
    issued = 0
    expire_next = False

    def log_message(self, *a):
        pass

    def do_POST(self):
        msg = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        sid, obj, method, args = msg["params"]
        if (obj, method) == ("session", "login"):
            if args["password"] != "secret":
                result = [6]
            else:
                FakeUbus.issued += 1
                sid = "s%d" % FakeUbus.issued
                self.sessions.add(sid)
                result = [0, {"ubus_rpc_session": sid}]
        elif sid not in self.sessions or FakeUbus.expire_next:
            FakeUbus.expire_next = False
            self.sessions.discard(sid)
            result = [6]
        else:
            result = [0, {"echo": method, "args": args}]
        body = json.dumps({"jsonrpc": "2.0", "id": msg["id"], "result": result}).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class HttpTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakeUbus)
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()
        cls.url = "http://127.0.0.1:%d" % cls.srv.server_address[1]

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()

    def test_login_call_and_relogin(self):
        t = HttpTransport(self.url, password="secret")
        self.assertEqual(t.call("spectral", "status"), {"echo": "status", "args": {}})
        first = t.session
        FakeUbus.expire_next = True
        self.assertEqual(t.call("spectral", "frames", {"since": 4})["args"], {"since": 4})
        self.assertNotEqual(t.session, first)

    def test_bad_password(self):
        with self.assertRaises(UbusError) as e:
            HttpTransport(self.url, password="nope").call("spectral", "status")
        self.assertEqual(e.exception.code, 6)


class SshTest(unittest.TestCase):
    def test_command_quotes_json(self):
        t = SshTransport("mr33", ["ProxyCommand=nc -6 %h%%wlp59s0 %p"], control_path="/tmp/cm")
        cmd = t.command("spectral", "start", {"band": "5g", "note": "it's"})
        self.assertEqual(cmd[-2], "mr33")
        self.assertIn("ProxyCommand=nc -6 %h%%wlp59s0 %p", cmd)
        remote = shlex.split(cmd[-1])
        self.assertEqual(remote[:4], ["ubus", "call", "spectral", "start"])
        self.assertEqual(json.loads(remote[4]), {"band": "5g", "note": "it's"})

    def test_errors_map_to_ubus_codes(self):
        t = SshTransport("mr33")
        done = mock.Mock(returncode=4, stdout="", stderr="Command failed: Not found")
        with mock.patch("subprocess.run", return_value=done):
            with self.assertRaises(UbusError) as e:
                t.call("spectral", "status")
        self.assertEqual(e.exception.code, 4)
        ok = mock.Mock(returncode=0, stdout='{"running": false}', stderr="")
        with mock.patch("subprocess.run", return_value=ok):
            self.assertEqual(t.call("spectral", "status"), {"running": False})


if __name__ == "__main__":
    unittest.main()
