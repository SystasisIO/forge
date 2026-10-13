#!/usr/bin/env python3
"""Characterize public liboauth2 results, never approve a backend."""

import argparse
import contextlib
import http.server
import json
import os
from pathlib import Path
import socket
import socketserver
import subprocess
import threading
import time
import urllib.parse


TOKEN = "forge-f0-fabricated-token"
SECRET = "forge-f0-fabricated-client-secret"
CASES = (
    "invalid-jwt", "jwks-503", "jwks-unreachable", "inactive",
    "introspection-503", "introspection-timeout", "redirect-307", "redirect-308",
)
TOTAL_TIMEOUT = 35.0


def emit(result):
    print(json.dumps(result, sort_keys=True, separators=(",", ":")), flush=True)


class FixtureServer(http.server.ThreadingHTTPServer):
    daemon_threads = False
    block_on_close = True

    def __init__(self):
        self.lock = threading.Lock()
        self.seen = {}
        self.release_timeout = threading.Event()
        self.peer_url = ""
        self.healthy = True
        super().__init__(("127.0.0.1", 0), Handler)

    def server_bind(self):
        # HTTPServer.server_bind calls getfqdn; isolated loopback needs no DNS.
        socketserver.TCPServer.server_bind(self)
        self.server_name = "127.0.0.1"
        self.server_port = self.server_address[1]

    @property
    def url(self):
        return f"http://127.0.0.1:{self.server_port}"

    def handle_error(self, request, client_address):
        # The default implementation prints a traceback; never emit it here.
        self.healthy = False

    def record(self, path, token=False, secret=False):
        with self.lock:
            self.seen[path] = {"request_seen": True,
                               "token_seen": token, "client_secret_seen": secret}

    def snapshot(self, path):
        with self.lock:
            return dict(self.seen.get(path, {
                "request_seen": False, "token_seen": False,
                "client_secret_seen": False,
            }))


class Handler(http.server.BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(2.0)

    def log_message(self, *args):
        pass

    def reply(self, status, body=b'{"active":false}', location=None):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        if location is not None:
            self.send_header("Location", location)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        self.server.record(self.path)
        self.reply(503, b'{"keys":[]}')

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        if not 0 < length <= 8192:
            self.server.healthy = False
            self.reply(400)
            return
        body = self.rfile.read(length)
        if len(body) != length:
            self.server.healthy = False
            return
        fields = urllib.parse.parse_qs(body.decode("ascii"), max_num_fields=8)
        token_seen = fields.get("token") == [TOKEN]
        secret_seen = fields.get("client_secret") == [SECRET]
        self.server.record(self.path, token_seen, secret_seen)
        if self.path == "/introspection-timeout":
            # No headers or body: the runner releases this wait after the child.
            self.server.release_timeout.wait(TOTAL_TIMEOUT)
            return
        if self.path in ("/redirect-307", "/redirect-308"):
            status = 307 if self.path.endswith("307") else 308
            self.reply(status, b"", self.server.peer_url + f"/receive-{status}")
        elif self.path == "/introspection-503":
            self.reply(503)
        else:
            self.reply(200)


@contextlib.contextmanager
def running_server():
    server = FixtureServer()
    thread = threading.Thread(target=server.serve_forever,
                              kwargs={"poll_interval": 0.05})
    try:
        thread.start()
    except BaseException:
        server.server_close()
        raise
    try:
        yield server
    finally:
        server.release_timeout.set()
        server.shutdown()
        thread.join(timeout=2.0)
        server.server_close()


def run_probe(probe, case, endpoint, environment, deadline):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise RuntimeError("deadline")
    child = subprocess.run(
        [str(probe), case, endpoint], stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        timeout=min(21.0, remaining), env=environment, check=False,
    )
    # Never replay child output. Only re-emit a validated public result record.
    if child.returncode != 0 or child.stderr or len(child.stdout) > 512:
        raise RuntimeError("probe")
    if any(value.encode("ascii") in child.stdout for value in (TOKEN, SECRET)):
        raise RuntimeError("redaction")
    result = json.loads(child.stdout)
    if (not isinstance(result, dict)
            or set(result) != {"case", "accepted", "status", "completed"}
            or result["case"] != case
            or type(result["accepted"]) is not bool
            or type(result["completed"]) is not bool
            or not result["completed"]
            or type(result["status"]) is not int
            or not 0 <= result["status"] <= 599):
        raise RuntimeError("output")
    return result


def characterize(probe):
    deadline = time.monotonic() + TOTAL_TIMEOUT
    environment = {key: value for key, value in os.environ.items()
                   if key.lower() not in {
                       "http_proxy", "https_proxy", "all_proxy", "no_proxy",
                   }}
    environment["NO_PROXY"] = "127.0.0.1,localhost"
    results = {}
    fixture_valid = True
    with running_server() as origin, running_server() as receiver, \
            socket.socket(socket.AF_INET, socket.SOCK_STREAM) as unreachable:
        origin.peer_url = receiver.url
        # A bound, non-listening socket reserves a port without a close/rebind race.
        unreachable.bind(("127.0.0.1", 0))
        refused_url = f"http://127.0.0.1:{unreachable.getsockname()[1]}/jwks"
        for case in CASES:
            endpoint = refused_url if case == "jwks-unreachable" else origin.url + "/" + case
            try:
                result = run_probe(probe, case, endpoint, environment, deadline)
            finally:
                if case == "introspection-timeout":
                    origin.release_timeout.set()
            if case not in ("invalid-jwt", "jwks-unreachable"):
                observed = origin.snapshot("/" + case)
                fixture_valid &= observed["request_seen"]
                if not case.startswith("jwks-"):
                    fixture_valid &= observed["token_seen"] and observed["client_secret_seen"]
                result["request_seen"] = observed["request_seen"]
            if case.startswith("redirect-"):
                forwarded = receiver.snapshot("/receive-" + case[-3:])
                result["post_forwarded"] = forwarded["request_seen"]
                result["token_forwarded"] = forwarded["token_seen"]
                result["client_secret_forwarded"] = forwarded["client_secret_seen"]
            results[case] = result
            emit(result)
        fixture_valid &= origin.healthy and receiver.healthy

    rejected = lambda case: not results[case]["accepted"]
    # Invalid measurement must not claim even an individual gap was reproduced.
    jwt_gap = fixture_valid and all(
        rejected(case) and results[case]["status"] == 401
        for case in ("invalid-jwt", "jwks-503", "jwks-unreachable"))
    timeout_gap = fixture_valid and all(
        rejected(case) and results[case]["status"] == 401
        for case in ("inactive", "introspection-timeout"))
    control = (fixture_valid and rejected("introspection-503")
               and results["introspection-503"]["status"] == 503)
    redirects = fixture_valid and all(
        results[case][field]
        for case in ("redirect-307", "redirect-308")
        for field in ("post_forwarded", "token_forwarded", "client_secret_forwarded"))
    gap_reproduced = fixture_valid and jwt_gap and timeout_gap and control and redirects
    emit({"case": "summary", "accepted": False, "status": 0, "completed": True,
          "fixture_valid": fixture_valid, "jwks_gap_reproduced": jwt_gap,
          "timeout_gap_reproduced": timeout_gap, "http_503_control_preserved": control,
          "redirect_forwarding_reproduced": redirects, "gap_reproduced": gap_reproduced,
          "backend_approved": False})
    return 0 if gap_reproduced else (1 if fixture_valid else 2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True, type=Path)
    args = parser.parse_args()
    try:
        return characterize(args.probe.resolve(strict=True))
    except Exception:
        # This intentionally omits exception text, command lines and child logs.
        emit({"case": "harness", "accepted": False, "status": 0,
              "completed": False, "harness_error": True, "backend_approved": False})
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
