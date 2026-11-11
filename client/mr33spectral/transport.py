"""Ways to reach the router's `spectral` ubus object."""

import json
import shlex
import subprocess
import urllib.error
import urllib.request

ANON_SESSION = "0" * 32
UBUS_PERMISSION_DENIED = 6

UBUS_STATUS = {
    1: "invalid command",
    2: "invalid argument",
    3: "method not found",
    4: "not found (is spectrald running?)",
    5: "no data",
    6: "permission denied",
    7: "timeout",
    8: "not supported",
    9: "unknown error",
    10: "connection failed",
}


class UbusError(Exception):
    def __init__(self, code, method=""):
        self.code = code
        super().__init__("ubus %s failed: %s (%d)" % (method, UBUS_STATUS.get(code, "?"), code))


class HttpTransport:
    """uhttpd's JSON-RPC endpoint, the same one LuCI uses.

    For a link-local router, put the zone in the URL: http://[fe80::1%25eth0]
    """

    def __init__(self, url, user="root", password="", timeout=10):
        self.endpoint = url.rstrip("/") + "/ubus"
        self.user = user
        self.password = password
        self.timeout = timeout
        self.session = None
        self._id = 0

    def _rpc(self, params):
        self._id += 1
        body = json.dumps({"jsonrpc": "2.0", "id": self._id, "method": "call", "params": params}).encode()
        req = urllib.request.Request(self.endpoint, data=body, headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=self.timeout) as r:
            msg = json.load(r)

        if "error" in msg:
            raise UbusError(UBUS_PERMISSION_DENIED if msg["error"].get("code") == -32002 else 9, params[2])

        result = msg.get("result") or [9]
        if result[0] != 0:
            raise UbusError(result[0], params[2])
        return result[1] if len(result) > 1 else {}

    def login(self):
        reply = self._rpc([ANON_SESSION, "session", "login", {"username": self.user, "password": self.password}])
        self.session = reply["ubus_rpc_session"]

    def call(self, obj, method, args=None):
        if not self.session:
            self.login()
        try:
            return self._rpc([self.session, obj, method, args or {}])
        except UbusError as e:
            # sessions expire after 5 idle minutes
            if e.code != UBUS_PERMISSION_DENIED:
                raise
            self.login()
            return self._rpc([self.session, obj, method, args or {}])


class SshTransport:
    """`ssh <host> ubus call ...`, multiplexed over one connection."""

    def __init__(self, host, ssh_options=(), timeout=20, control_path=None):
        self.host = host
        self.timeout = timeout
        self.base = ["ssh", "-o", "BatchMode=yes"]
        if control_path:
            self.base += ["-o", "ControlMaster=auto", "-o", "ControlPath=" + control_path,
                          "-o", "ControlPersist=120"]
        for opt in ssh_options:
            self.base += ["-o", opt]

    def command(self, obj, method, args=None):
        remote = "ubus call %s %s %s" % (obj, method, shlex.quote(json.dumps(args or {})))
        return self.base + [self.host, remote]

    def call(self, obj, method, args=None):
        p = subprocess.run(self.command(obj, method, args), capture_output=True, text=True, timeout=self.timeout)
        if p.returncode:
            err = (p.stderr or p.stdout).strip()
            raise UbusError(p.returncode if p.returncode < 16 else 9, "%s (%s)" % (method, err))
        return json.loads(p.stdout or "{}")
