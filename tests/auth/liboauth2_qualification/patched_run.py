#!/usr/bin/env python3
"""Patched F0 loopback regression, not complete backend qualification.

The C++ child owns disposable keys, JWT signing, verification and host policy.
Python only supplies bounded HTTP fixtures and checks sanitized observations.
Exit 0: this regression matrix passed; 1: regression failure; 2: harness error.
No historical upstream evidence is overwritten. No build is launched here.
"""

import argparse
import contextlib
import http.client
import http.server
import json
import os
from pathlib import Path
import socket
import socketserver
import struct
import subprocess
import sys
import threading
import time
import urllib.parse


TOKEN = "forge-f0-patched-token-canary"
SECRET = "forge-f0-patched-secret-canary"
ASSERTION = "forge-f0-patched-assertion-canary"
ISSUER = "https://forge-f0.invalid/issuer-a"
AUDIENCE = "forge-f0-resource"
CASES = (
    "jwt-positive", "jwt-wrong-issuer", "jwt-wrong-audience", "jwt-wrong-alg",
    "jwt-expired", "jwt-future-nbf", "jwt-missing-exp", "jwt-missing-iss",
    "jwt-missing-aud", "jwt-missing-sub", "jwt-bad-signature", "jwt-malformed",
    "jwt-malformed-payload", "jwt-id-token", "jwt-missing-typ", "jwt-other-key-same-kid",
    "jwt-type-iss", "jwt-type-aud", "jwt-type-sub", "jwt-type-exp", "jwt-type-nbf",
    "jwt-duplicate-iss", "jwt-duplicate-aud", "jwt-duplicate-sub", "jwt-duplicate-exp", "jwt-duplicate-nbf",
    "jwt-audience-array", "jwt-audience-array-mixed", "jwt-hs-rsa-key-confusion",
    "jwt-ec-rsa-key-confusion", "jwt-rsa-oct-key-confusion", "jwt-alg-none",
    "jwks-positive", "jwks-rotation", "jwks-cached-unknown-kid", "jwks-503-first",
    "jwks-503-refresh", "jwks-malformed", "jwks-oversized", "jwks-transport-reset",
    "jwks-timeout", "introspection-positive", "introspection-inactive",
    "jwks-two-issuers-same-kid", "jwks-shared-uri-policy-isolation",
    "jwks-expiry-cache-off", "jwks-uri-cache-off", "jwks-url-as-bearer",
    "jwks-key-limit-positive", "jwks-key-limit-first", "jwks-key-limit-refresh",
    "jwks-kid-throttle", "jwks-refresh-expiry-throttled",
    "jwks-clone-coalescing", "jwks-clone-wait-timeout",
    "jwks-long-unknown-kid-refresh", "jwks-long-lock-wait-expiry",
    "introspection-cache-off", "introspection-expiry-cache-off",
    "introspection-type-iss", "introspection-type-aud", "introspection-type-sub",
    "introspection-type-exp", "introspection-type-nbf", "introspection-type-active",
    "introspection-duplicate-iss", "introspection-duplicate-aud", "introspection-duplicate-sub",
    "introspection-duplicate-exp", "introspection-duplicate-nbf", "introspection-duplicate-active",
    "introspection-missing-iss", "introspection-missing-aud", "introspection-missing-sub",
    "introspection-missing-exp", "introspection-wrong-issuer", "introspection-wrong-audience",
    "introspection-expired", "introspection-503", "introspection-401",
    "introspection-timeout", "introspection-transport-reset", "introspection-status-null",
    "redirect-307", "redirect-308", "http-503-status-null",
)
TOTAL_TIMEOUT = 120.0
CHILD_TIMEOUT = 12.0
LONG_REFRESH_CASES = {"jwks-long-unknown-kid-refresh", "jwks-long-lock-wait-expiry"}
LONG_RESPONSE_HOLD = 2.0
COORDINATOR_NOT_RUN = (
    "cross-platform-manifest", "https-tls", "dependency-license-advisory-closure", "package-export",
)
KINDS = {
    "success", "credential-rejected", "transport-error", "timeout", "http-error",
    "malformed-response", "response-too-large", "redirect-limit", "internal-error",
    "key-limit", "refresh-timeout", "refresh-throttled", "legacy", "http-result",
}
HTTP_ERRORS = {"ok", "transport-error", "timeout", "response-too-large", "redirect-limit", "internal-error"}
F0_MATRIX = {
    "jwt_claims_algorithm_key_policy": tuple(case for case in CASES if case.startswith("jwt-")),
    "protected_access_token_purpose": ("jwt-id-token", "jwt-missing-typ"),
    "issuer_policy_cache_isolation": ("jwks-two-issuers-same-kid", "jwks-shared-uri-policy-isolation"),
    "cache_disable_and_expiry": ("jwks-expiry-cache-off", "jwks-uri-cache-off", "jwks-url-as-bearer",
                                "introspection-cache-off", "introspection-expiry-cache-off"),
    "opaque_introspection_claims": tuple(case for case in CASES if case.startswith("introspection-")),
    "jwks_size_key_and_decode_bounds": ("jwks-key-limit-positive", "jwks-key-limit-first", "jwks-key-limit-refresh",
                                        "jwks-malformed", "jwks-oversized"),
    "key_rotation_refresh_throttle_clone_wait": ("jwks-positive", "jwks-rotation", "jwks-cached-unknown-kid", "jwks-kid-throttle",
                                                "jwks-refresh-expiry-throttled", "jwks-clone-coalescing", "jwks-clone-wait-timeout",
                                                "jwks-long-unknown-kid-refresh", "jwks-long-lock-wait-expiry"),
    "http_transport_timeout_redirect_status": ("jwks-503-first", "jwks-503-refresh", "jwks-transport-reset",
                                               "jwks-timeout", "introspection-503", "introspection-401",
                                               "introspection-timeout", "introspection-transport-reset",
                                               "introspection-status-null", "redirect-307", "redirect-308", "http-503-status-null"),
}


class HarnessError(Exception):
    pass


def emit(value):
    print(json.dumps(value, sort_keys=True, separators=(",", ":")), flush=True)


def require(condition):
    if not condition:
        raise HarnessError()


def empty_observation():
    return {"count": 0, "token_seen": False, "secret_seen": False, "assertion_seen": False}


class FixtureServer(socketserver.TCPServer):
    allow_reuse_address = False
    request_queue_size = 4

    def __init__(self, case, receiver=False):
        self.case = case
        self.receiver = receiver
        self.lock = threading.Lock()
        self.seen = {}
        self.keys = None
        self.peer_url = ""
        self.healthy = True
        self.release_timeout = threading.Event()
        self.request_started = threading.Event()
        self.request_started_at = 0.0
        self.response_released_at = 0.0
        self.release_response = threading.Event()
        self.origin = None
        self.control_phases = set()
        self.fixed_expiry = None
        super().__init__(("127.0.0.1", 0), Handler)

    def server_bind(self):
        # TCPServer avoids HTTPServer's reverse-DNS getfqdn during startup.
        socketserver.TCPServer.server_bind(self)
        self.server_name = "127.0.0.1"
        self.server_port = self.server_address[1]

    @property
    def url(self):
        return f"http://127.0.0.1:{self.server_port}"

    def handle_error(self, request, client_address):
        # No exception text, payload, traceback, URL or vendor diagnostic.
        self.healthy = False

    def record(self, path, body=b"", headers=b""):
        wire = body + headers + path.encode("ascii")
        with self.lock:
            result = self.seen.setdefault(path, empty_observation())
            result["count"] += 1
            for name, canary in (("token_seen", TOKEN), ("secret_seen", SECRET), ("assertion_seen", ASSERTION)):
                result[name] |= canary.encode("ascii") in wire
            return result["count"]

    def snapshot(self, path):
        with self.lock:
            entries = [item for name, item in self.seen.items() if name == path or name.startswith(path + "/")]
            return {"count": sum(item["count"] for item in entries),
                    **{field: any(item[field] for item in entries)
                       for field in ("token_seen", "secret_seen", "assertion_seen")}}

    def forwarded(self):
        with self.lock:
            entries = [item for path, item in self.seen.items() if path != "/receiver-control"]
            return {
                "count": sum(item["count"] for item in entries),
                **{field: any(item[field] for item in entries)
                   for field in ("token_seen", "secret_seen", "assertion_seen")},
            }


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.settimeout(2.0)

    def log_message(self, *args):
        pass

    def reply(self, status, body=b"", location=None):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        if location is not None:
            self.send_header("Location", location)
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            # Expected for bounded-body rejection, never a security verdict.
            pass

    def read_body(self):
        require(self.headers.get("Transfer-Encoding") is None)
        length = int(self.headers.get("Content-Length", "0"))
        require(0 < length <= 8192)
        body = self.rfile.read(length)
        require(len(body) == length)
        return body

    def active_body(self):
        case = self.server.case
        value = {"active": True, "iss": ISSUER, "aud": AUDIENCE,
                 "sub": "forge-f0-subject", "exp": int(time.time()) + 3600, "nbf": int(time.time()) - 120}
        if case == "introspection-expiry-cache-off":
            if self.server.fixed_expiry is None:
                self.server.fixed_expiry = int(time.time()) + 2
            value["exp"] = self.server.fixed_expiry
        if case == "introspection-cache-off" and self.server.snapshot(self.path)["count"] == 2:
            value["active"] = False
        if case == "introspection-inactive":
            value["active"] = False
        if case.startswith("introspection-missing-"):
            value.pop(case.removeprefix("introspection-missing-"))
        if case == "introspection-wrong-issuer":
            value["iss"] = "https://forge-f0.invalid/issuer-b"
        if case == "introspection-wrong-audience":
            value["aud"] = "forge-f0-other-resource"
        if case == "introspection-expired":
            value["exp"] = int(time.time()) - 120
        if case.startswith("introspection-type-"):
            field = case.removeprefix("introspection-type-")
            value[field] = "wrong-type" if field in {"exp", "nbf", "active"} else 42
        body = json.dumps(value, separators=(",", ":"))
        if case.startswith("introspection-duplicate-"):
            field = case.removeprefix("introspection-duplicate-")
            body = body[:-1] + "," + json.dumps(field) + ":" + json.dumps(value[field]) + "}"
        return body.encode("ascii")

    def do_GET(self):
        count = self.server.record(self.path, headers=str(self.headers).encode("ascii"))
        if self.server.receiver:
            self.reply(200, self.active_body())
            return
        base = "/" + self.server.case
        require(self.path == base or (self.server.case == "jwks-two-issuers-same-kid"
                and self.path in {base + "/issuer-a", base + "/issuer-b"}))
        if self.server.case == "http-503-status-null":
            self.reply(503, self.active_body())
            return
        require(self.server.keys is not None)
        case = self.server.case
        if case in {"jwks-clone-coalescing", "jwks-clone-wait-timeout"} or (case in LONG_REFRESH_CASES and count == 2):
            self.server.request_started_at = time.monotonic()
            self.server.request_started.set()
            require(self.server.release_response.wait(CHILD_TIMEOUT))
        if case == "jwks-timeout":
            self.server.release_timeout.wait(CHILD_TIMEOUT)
            return
        if case == "jwks-malformed":
            self.reply(200, b'{"keys":[')
            return
        index = 1 if case in ("jwks-rotation", "jwks-503-refresh") and count > 1 else 0
        if case == "jwks-two-issuers-same-kid":
            index = 1 if self.path.endswith("/issuer-b") else 0
        if case == "jwks-key-limit-refresh" and count > 1:
            index = 1
        document = {"keys": [self.server.keys[index]]}
        if case == "jwks-key-limit-first" or (case == "jwks-key-limit-refresh" and count > 1):
            # Unusable entries ALSO count toward the parsing/key-work bound.
            document["keys"].append({"kty": "not-a-supported-key-type"})
        if case == "jwks-oversized":
            document["padding"] = "x" * 8192
        status = 503 if case == "jwks-503-first" or (case == "jwks-503-refresh" and count == 2) else 200
        self.reply(status, json.dumps(document, separators=(",", ":")).encode("ascii"))

    def do_POST(self):
        body = self.read_body()
        headers = str(self.headers).encode("ascii")
        self.server.record(self.path, body, headers)
        if self.path == "/fixture-control":
            require(self.server.receiver and self.server.origin is not None)
            document = json.loads(body)
            require(isinstance(document, dict) and set(document) == {"phase"})
            phase = document["phase"]
            require(phase in {"await", "release", "long-release"} and phase not in self.server.control_phases)
            if phase == "await":
                require(self.server.origin.request_started.wait(0.8))
            else:
                require("await" in self.server.control_phases)
                if phase == "long-release":
                    require(self.server.case in LONG_REFRESH_CASES and self.server.origin.request_started_at > 0)
                    delay = max(0.0, self.server.origin.request_started_at + LONG_RESPONSE_HOLD - time.monotonic())
                    # Hold an observed, real transfer beyond the configured one-second
                    # cooldown. Teardown cancels this finite wait on child failure.
                    require(not self.server.release_timeout.wait(delay))
                self.server.origin.response_released_at = time.monotonic()
                self.server.origin.release_response.set()
            self.server.control_phases.add(phase)
            self.reply(204)
            return
        if self.path == "/fixture-keys":
            require(not self.server.receiver and self.server.keys is None)
            document = json.loads(body)
            require(isinstance(document, dict) and set(document) == {"keys"})
            keys = document["keys"]
            require(isinstance(keys, list) and len(keys) == 2)
            kids = ("same-kid", "same-kid") if self.server.case == "jwks-two-issuers-same-kid" else ("key-a", "key-b")
            for key, kid in zip(keys, kids):
                require(isinstance(key, dict) and set(key) == {"kty", "kid", "e", "n"})
                require(key["kty"] == "RSA" and key["kid"] == kid)
                require(all(isinstance(key[field], str) and 0 < len(key[field]) <= 1024 for field in ("e", "n")))
            require(keys[0]["n"] != keys[1]["n"])
            self.server.keys = keys
            self.reply(204)
            return
        fields = urllib.parse.parse_qs(body.decode("ascii"), max_num_fields=8, strict_parsing=True)
        require(fields.get("token") == [TOKEN] and fields.get("client_secret") == [SECRET])
        if self.server.receiver:
            self.reply(200, self.active_body())
            return
        require(self.path == "/" + self.server.case)
        case = self.server.case
        if case == "introspection-timeout":
            self.server.release_timeout.wait(CHILD_TIMEOUT)
            return
        if case.startswith("redirect-"):
            self.reply(int(case[-3:]), self.active_body(), self.server.peer_url + "/receive")
            return
        status = 503 if case in ("introspection-503", "introspection-status-null") else (
            401 if case == "introspection-401" else 200)
        self.reply(status, self.active_body())


class ResetHandler(socketserver.BaseRequestHandler):
    def handle(self):
        # A held, non-listening port silently drops SYN on Darwin. An active
        # zero-linger close produces RST without a close/rebind port race.
        self.request.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        with self.server.lock:
            self.server.connections += 1
        self.request.close()


class ResetServer(socketserver.TCPServer):
    request_queue_size = 4

    def __init__(self):
        self.lock = threading.Lock()
        self.connections = 0
        self.healthy = True
        self.release_timeout = threading.Event()
        super().__init__(("127.0.0.1", 0), ResetHandler)

    def handle_error(self, request, client_address):
        self.healthy = False

    def count(self):
        with self.lock:
            return self.connections


@contextlib.contextmanager
def serving(server):
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.02})
    try:
        thread.start()
        yield server
    finally:
        server.release_timeout.set()
        if hasattr(server, "release_response"):
            server.release_response.set()
        if thread.is_alive():
            server.shutdown()
            thread.join(timeout=2.0)
        server.server_close()
        require(not thread.is_alive())


def running_server(case, receiver=False):
    return serving(FixtureServer(case, receiver))


def reset_control(server):
    with socket.create_connection(server.server_address, timeout=2.0) as connection:
        try:
            connection.recv(1)
        except ConnectionResetError:
            pass
        else:
            raise HarnessError()
    require(server.healthy and server.count() == 1)


def receiver_control(receiver):
    body = urllib.parse.urlencode({"token": TOKEN, "client_secret": SECRET})
    connection = http.client.HTTPConnection("127.0.0.1", receiver.server_port, timeout=2.0)
    try:
        connection.request("POST", "/receiver-control", body, {"Content-Type": "application/x-www-form-urlencoded"})
        response = connection.getresponse()
        require(response.status == 200)
        require(len(response.read(4097)) <= 4096)
    finally:
        connection.close()
    observed = receiver.snapshot("/receiver-control")
    require(observed["count"] == 1 and observed["token_seen"] and observed["secret_seen"])


def validate_record(raw, case):
    require(len(raw) <= 8192 and not any(value.encode("ascii") in raw for value in (TOKEN, SECRET, ASSERTION)))
    result = json.loads(raw)
    require(isinstance(result, dict) and set(result) == {"case", "completed", "crypto_control", "result_cache_disabled", "steps"})
    require(result["case"] == case and result["completed"] is True and result["crypto_control"] is True)
    require(result["result_cache_disabled"] is True)
    steps = result["steps"]
    require(isinstance(steps, list) and 1 <= len(steps) <= 8)
    for step in steps:
        require(isinstance(step, dict) and set(step) == {
            "api", "vendor_accepted", "accepted", "status_null", "payload_present",
            "kind", "http_error", "policy", "status", "endpoint_status", "elapsed_ms",
        })
        require(step["api"] in {"ex", "legacy", "http"} and step["kind"] in KINDS)
        require(step["http_error"] in HTTP_ERRORS and step["policy"] in {"none", "not-applied", "claims", "purpose", "algorithm", "format"})
        require(all(type(step[field]) is bool for field in (
            "vendor_accepted", "accepted", "status_null", "payload_present")))
        require(all(type(step[field]) is int and 0 <= step[field] <= 599 for field in ("status", "endpoint_status")))
        require(type(step["elapsed_ms"]) is int and 0 <= step["elapsed_ms"] <= 10000)
    return result


def run_probe(probe, case, origin, endpoint, control, environment, deadline):
    remaining = deadline - time.monotonic()
    require(remaining > 0)
    child = subprocess.run(
        [str(probe), case, origin, endpoint, control], stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        timeout=min(CHILD_TIMEOUT, remaining), env=environment, check=False,
    )
    # Re-emit only schema-validated data, never raw child stdout/stderr.
    require(child.returncode == 0 and not child.stderr)
    return validate_record(child.stdout, case)


def expected_steps(case):
    special = {
        "jwt-rsa-oct-key-confusion": 4, "jwks-two-issuers-same-kid": 8,
        "jwks-shared-uri-policy-isolation": 6, "jwks-expiry-cache-off": 4,
        "jwks-uri-cache-off": 2, "jwks-url-as-bearer": 3,
        "jwks-key-limit-refresh": 3, "jwks-kid-throttle": 7,
        "jwks-refresh-expiry-throttled": 3, "jwks-clone-coalescing": 3,
        "jwks-clone-wait-timeout": 3, "introspection-cache-off": 3,
        "jwks-long-unknown-kid-refresh": 4, "jwks-long-lock-wait-expiry": 4,
        "introspection-expiry-cache-off": 2,
    }
    if case in special:
        return special[case]
    if case.startswith("jwt-"):
        return 1 if case == "jwt-positive" else 3
    if case in ("jwks-rotation", "jwks-cached-unknown-kid", "jwks-503-refresh"):
        return 4
    return 2 if case == "introspection-status-null" else 1


def positive(step):
    return (step["vendor_accepted"] and step["accepted"] and step["payload_present"]
            and step["kind"] == "success" and step["http_error"] == "ok" and step["policy"] == "none")


def rejection(step, kind, raw_status=None, http_error="ok"):
    normalized = (504 if kind in {"timeout", "refresh-timeout"} else
                  raw_status if kind == "http-error" and raw_status is not None and 400 <= raw_status <= 599 else
                  401 if kind in {"credential-rejected", "http-error"} else 502)
    return (not step["vendor_accepted"] and not step["accepted"]
            and not step["payload_present"] and step["policy"] == "not-applied"
            and step["kind"] == kind and step["http_error"] == http_error
            and step["status"] == (0 if step["status_null"] else normalized)
            and (raw_status is None or step["endpoint_status"] == raw_status))


def policy_rejection(step, policy):
    return (step["vendor_accepted"] and not step["accepted"] and step["payload_present"]
            and step["kind"] == "success" and step["http_error"] == "ok" and step["policy"] == policy)


def assertions(result, observed, forwarded):
    case, steps = result["case"], result["steps"]
    checks = {"step_count": len(steps) == expected_steps(case)}
    if not checks["step_count"]:
        return checks
    if case.startswith("jwt-"):
        checks["positive_controls"] = positive(steps[0]) and (len(steps) == 1 or positive(steps[-1]))
        if case == "jwt-rsa-oct-key-confusion":
            checks["genuine_hs_control"] = positive(steps[1])
            checks["rsa_with_oct_rejected"] = rejection(steps[2], "credential-rejected", 0)
        if len(steps) == 3:
            policy = {"jwt-wrong-alg": "algorithm", "jwt-id-token": "purpose",
                      "jwt-missing-typ": "purpose", "jwt-missing-sub": "claims",
                      "jwt-type-sub": "claims", "jwt-type-nbf": "claims",
                      "jwt-audience-array-mixed": "claims"}.get(case)
            if case == "jwt-audience-array":
                checks["audience_array_positive"] = positive(steps[1])
            elif case.startswith("jwt-duplicate-"):
                checks["strict_vendor_duplicate_rejected"] = rejection(steps[1], "credential-rejected", 0)
            else:
                checks["negative_control"] = policy_rejection(steps[1], policy) if policy else rejection(steps[1], "credential-rejected", 0)
        checks["no_remote_fetch"] = observed["count"] == 0
    elif case in {"jwks-two-issuers-same-kid", "jwks-shared-uri-policy-isolation"}:
        checks["both_issuer_positive_controls"] = all(positive(steps[index]) for index in (0, 1, len(steps)-2, len(steps)-1))
        checks["cross_issuer_and_forged_key_denied"] = all(rejection(step, "credential-rejected") for step in steps[2:-2])
        checks["isolated_uri_cache"] = observed["count"] == (6 if case == "jwks-two-issuers-same-kid" else 1)
    elif case == "jwks-expiry-cache-off":
        checks["cached_positive_controls"] = positive(steps[0]) and positive(steps[1]) and positive(steps[3])
        checks["identical_token_expired_denied"] = rejection(steps[2], "credential-rejected", 0)
        checks["jwks_cache_not_refetched"] = observed["count"] == 1
    elif case == "jwks-url-as-bearer":
        checks["warmed_jwks_positive_controls"] = positive(steps[0]) and positive(steps[2])
        checks["literal_jwks_url_is_not_a_credential"] = rejection(steps[1], "credential-rejected", 0)
        checks["no_url_decision_cache_hit_or_fetch"] = observed["count"] == 1
    elif case == "jwks-uri-cache-off":
        checks["two_public_verifications"] = all(positive(step) for step in steps)
        checks["uri_cache_explicitly_disabled"] = observed["count"] == 2
    elif case.startswith("jwks-key-limit-"):
        if case == "jwks-key-limit-positive":
            checks["at_key_bound_positive"] = positive(steps[0])
        else:
            checks["all_entries_count_before_import"] = rejection(steps[-1], "key-limit", 200)
            if len(steps) == 3:
                checks["cached_positive_controls"] = positive(steps[0]) and positive(steps[1])
        checks["bounded_key_fetches"] = observed["count"] == (2 if len(steps) == 3 else 1)
    elif case == "jwks-kid-throttle":
        checks["cached_positive_controls"] = positive(steps[0]) and positive(steps[-1])
        checks["kid_storm_credential_denials"] = all(rejection(step, "credential-rejected", 0) for step in steps[1:-1])
        checks["no_unknown_kid_refresh_storm"] = observed["count"] == 1
    elif case == "jwks-refresh-expiry-throttled":
        checks["cached_positive_controls"] = positive(steps[0]) and positive(steps[1])
        checks["expired_snapshot_throttled_not_invalid"] = rejection(steps[2], "refresh-throttled", 0)
        checks["cooldown_no_refetch"] = observed["count"] == 1
    elif case in {"jwks-clone-coalescing", "jwks-clone-wait-timeout"}:
        checks["first_and_after_positive"] = positive(steps[0]) and positive(steps[2])
        checks["shared_clone_outcome"] = (positive(steps[1]) if case == "jwks-clone-coalescing"
                                          else rejection(steps[1], "refresh-timeout", 0))
        checks["one_actual_transfer"] = observed["count"] == 1
        if case == "jwks-clone-wait-timeout":
            checks["finite_refresh_wait"] = 50 <= steps[1]["elapsed_ms"] <= 500
    elif case in LONG_REFRESH_CASES:
        checks["warm_and_later_clone_usable"] = positive(steps[0]) and positive(steps[3])
        checks["bounded_long_refresh_fetches"] = observed["count"] == 2
        if case == "jwks-long-unknown-kid-refresh":
            checks["unknown_kid_denied_after_successful_refresh"] = all(
                rejection(step, "credential-rejected") for step in steps[1:3])
            checks["one_successful_http_refresh_one_shared_outcome"] = sorted(
                step["endpoint_status"] for step in steps[1:3]) == [0, 200]
            checks["long_refresh_completed_within_explicit_timeout"] = (
                max(step["elapsed_ms"] for step in steps[1:3]) >= 1500
                and all(step["elapsed_ms"] <= 4500 for step in steps[1:3]))
        else:
            checks["successful_long_refresh_unknown_key_denied"] = rejection(steps[1], "credential-rejected", 200)
            checks["clone_lock_wait_expired_not_transport_timeout"] = rejection(steps[2], "refresh-timeout", 0)
            checks["finite_wait_then_long_fetch_completed"] = (
                50 <= steps[2]["elapsed_ms"] <= 500 and 1500 <= steps[1]["elapsed_ms"] <= 4500)
    elif case == "introspection-cache-off":
        checks["positive_controls"] = positive(steps[0]) and positive(steps[2])
        checks["same_opaque_token_revoked"] = rejection(steps[1], "credential-rejected", 200)
        checks["result_cache_explicitly_disabled"] = observed["count"] == 3
    elif case == "introspection-expiry-cache-off":
        checks["positive_control"] = positive(steps[0])
        checks["expired_opaque_claims_denied"] = policy_rejection(steps[1], "claims")
        checks["result_cache_explicitly_disabled"] = observed["count"] == 2
    elif case.startswith("introspection-duplicate-") or case == "introspection-type-active":
        checks["strict_response_decode"] = rejection(steps[0], "malformed-response", 200)
    elif case.startswith("introspection-type-"):
        checks["strict_claim_type_policy"] = policy_rejection(steps[0], "claims")
    elif case in ("jwks-positive", "jwks-rotation", "jwks-cached-unknown-kid", "jwks-503-refresh"):
        checks["positive_controls"] = positive(steps[0])
        if len(steps) == 4:
            checks["cached_positive"] = positive(steps[1])
            checks["after_control"] = positive(steps[3])
            checks["rotation_or_rejection"] = (
                positive(steps[2]) if case == "jwks-rotation" else
                rejection(steps[2], "credential-rejected") if case == "jwks-cached-unknown-kid" else
                rejection(steps[2], "http-error", 503))
        checks["bounded_fetches_cache_not_result_cache"] = observed["count"] == {
            "jwks-positive": 1, "jwks-rotation": 2, "jwks-cached-unknown-kid": 2, "jwks-503-refresh": 3,
        }[case]
    elif case == "http-503-status-null":
        step = steps[0]
        checks["legacy_http_bool_contract"] = (step["api"] == "http" and step["status_null"]
            and step["vendor_accepted"] and step["accepted"] and step["payload_present"]
            and step["http_error"] == "ok" and step["endpoint_status"] == 503)
    elif case == "introspection-status-null":
        checks["typed_status_null_rejected"] = (steps[0]["status_null"]
            and rejection(steps[0], "http-error", 503))
        checks["legacy_status_null_rejected"] = (steps[1]["api"] == "legacy" and steps[1]["status_null"]
            and not steps[1]["vendor_accepted"] and not steps[1]["accepted"] and not steps[1]["payload_present"])
        checks["two_real_requests"] = observed["count"] == 2
    elif case == "introspection-positive":
        checks["active_real_introspection"] = positive(steps[0])
    elif case.startswith("introspection-missing-") or case in (
            "introspection-wrong-issuer", "introspection-wrong-audience", "introspection-expired"):
        checks["claims_host_policy"] = policy_rejection(steps[0], "claims")
    elif case == "introspection-inactive":
        checks["inactive_backend_rejected"] = rejection(steps[0], "credential-rejected", 200)
    elif case.endswith("-transport-reset"):
        checks["transport_not_invalid"] = rejection(steps[0], "transport-error", 0, "transport-error")
        checks["finite_transport"] = steps[0]["elapsed_ms"] <= 2500
    elif case.endswith("-timeout"):
        checks["timeout_not_invalid"] = rejection(steps[0], "timeout", 0, "timeout")
        checks["explicit_one_second_timeout"] = 700 <= steps[0]["elapsed_ms"] <= 2500
        checks["no_retry"] = observed["count"] == 1
    elif case == "jwks-malformed":
        checks["malformed_response_not_invalid_token"] = rejection(steps[0], "malformed-response", 200)
    elif case == "jwks-oversized":
        checks["response_bounded_before_parse"] = rejection(steps[0], "response-too-large", 200, "response-too-large")
    elif case.startswith("redirect-"):
        checks["redirect_rejected"] = rejection(steps[0], "http-error", int(case[-3:]))
        checks["no_second_origin_request"] = forwarded["count"] == 0
        checks["no_token_or_secret_forwarding"] = not forwarded["token_seen"] and not forwarded["secret_seen"]
    else:
        status = 401 if case == "introspection-401" else 503
        checks["http_endpoint_not_invalid_token"] = rejection(steps[0], "http-error", status)
    if not case.startswith("jwt-") and not case.endswith("-transport-reset") and case != "introspection-status-null":
        bound = next((checks[name] for name in (
            "bounded_fetches_cache_not_result_cache", "isolated_uri_cache", "jwks_cache_not_refetched",
            "uri_cache_explicitly_disabled", "bounded_key_fetches", "no_unknown_kid_refresh_storm",
            "cooldown_no_refetch", "one_actual_transfer", "result_cache_explicitly_disabled",
            "no_url_decision_cache_hit_or_fetch", "bounded_long_refresh_fetches") if name in checks),
            observed["count"] == 1)
        checks["request_bound"] = bound
    return checks


def qualify(probe):
    deadline = time.monotonic() + TOTAL_TIMEOUT
    environment = {key: value for key, value in os.environ.items()
                   if key.lower() not in {"http_proxy", "https_proxy", "all_proxy", "no_proxy"}}
    environment["NO_PROXY"] = "127.0.0.1,localhost"
    observations = []
    fixture_valid = True
    for case in CASES:
        record = {"case": case, "checks": {}, "steps": [], "origin": empty_observation(), "receiver": empty_observation()}
        try:
            with contextlib.ExitStack() as stack:
                origin = stack.enter_context(running_server(case))
                receiver = stack.enter_context(running_server(case, True))
                reset = stack.enter_context(serving(ResetServer())) if case.endswith("-transport-reset") else None
                origin.peer_url = receiver.url
                receiver.origin = origin
                if case.startswith("redirect-"):
                    receiver_control(receiver)
                if reset is not None:
                    reset_control(reset)
                endpoint = (f"http://127.0.0.1:{reset.server_address[1]}/{case}"
                            if reset is not None else origin.url + "/" + case)
                try:
                    result = run_probe(probe, case, origin.url, endpoint, receiver.url, environment, deadline)
                finally:
                    origin.release_timeout.set()
                observed, forwarded = origin.snapshot("/" + case), receiver.forwarded()
                require(origin.healthy and receiver.healthy)
                if reset is not None:
                    require(reset.healthy and reset.count() >= 2)
                    record["reset_control_observed"] = True
                    record["probe_reset_connections"] = reset.count() - 1
                if case.startswith("jwks-"):
                    require(origin.snapshot("/fixture-keys")["count"] == 1 and origin.keys is not None)
                if not case.startswith("jwt-") and not case.endswith("-transport-reset"):
                    require(observed["count"] > 0)
                    if case.startswith("introspection-") or case.startswith("redirect-"):
                        require(observed["token_seen"] and observed["secret_seen"])
                record.update(steps=result["steps"], origin=observed, receiver=forwarded,
                              result_cache_disabled=result["result_cache_disabled"], checks=assertions(result, observed, forwarded))
                if case in {"jwks-clone-coalescing", "jwks-clone-wait-timeout"} or case in LONG_REFRESH_CASES:
                    phases = {"await", "long-release"} if case in LONG_REFRESH_CASES else {"await", "release"}
                    require(origin.request_started.is_set() and receiver.control_phases == phases)
                    record["held_fetch_control_observed"] = True
                    if case in LONG_REFRESH_CASES:
                        require(origin.response_released_at - origin.request_started_at >= LONG_RESPONSE_HOLD)
                        record["refresh_outlasted_cooldown_observed"] = True
                if reset is not None:
                    record["checks"]["no_retry"] = reset.count() == 2
            # Include teardown health; a still-live server must not leave PASS evidence.
            require(origin.healthy and receiver.healthy)
            require(reset is None or reset.healthy)
        except (Exception, KeyboardInterrupt):
            fixture_valid = False
            record["harness_error"] = True
        observations.append(record)
    # Measurement honesty: ANY harness failure suppresses every security PASS flag.
    all_passed = fixture_valid and all(item["checks"] and all(item["checks"].values()) for item in observations)
    for record in observations:
        record["checks"] = {name: fixture_valid and value for name, value in record["checks"].items()}
        record["verdict"] = "HARNESS_ERROR" if not fixture_valid else (
            "PASS" if all(record["checks"].values()) else "FAIL")
        emit(record)
    indexed = {item["case"]: item for item in observations}
    matrix = {gate: "HARNESS_ERROR" if not fixture_valid else (
        "PASS" if all(indexed[case]["verdict"] == "PASS" for case in cases) else "FAIL")
        for gate, cases in F0_MATRIX.items()}
    emit({"fixture_valid": fixture_valid, "regression_passed": all_passed,
          "case_count": len(CASES), "f0_public_api_ready": all_passed,
          "scope": "patched-liboauth2-public-api-f0", "cjose": "0.6.2.8",
          "platform": sys.platform,
          "expected_cjose_source_sha": "e787ace6a203d483038a793fd58f958a2f9b48c1",
          "f0_matrix": matrix, "required_result_cache_option": "verify.cache=none",
          "coordinator_not_run": COORDINATOR_NOT_RUN})
    return 2 if not fixture_valid else (0 if all_passed else 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    args = parser.parse_args()
    try:
        require(args.probe.is_file() and os.access(args.probe, os.X_OK))
        return qualify(args.probe.resolve())
    except (Exception, KeyboardInterrupt):
        emit({"fixture_valid": False, "regression_passed": False,
              "f0_public_api_ready": False, "harness_error": True})
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
