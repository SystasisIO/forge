#!/usr/bin/env python3
"""Coordinator-run real Keycloak fixture; credentials and tokens are never printed."""

import argparse
import contextlib
import importlib.metadata
import json
import logging
import os
from pathlib import Path
import resource
import secrets
import select
import signal
import stat
import subprocess
import sys
import tempfile
import time
import warnings


BASE = "https://localhost:18443"
ISSUER = BASE + "/realms/forge-live"
ADMIN = BASE + "/admin/realms/forge-live"
TOKEN_URL = ISSUER + "/protocol/openid-connect/token"
JWKS_URL = ISSUER + "/protocol/openid-connect/certs"
INTROSPECTION_URL = TOKEN_URL + "/introspect"
MARKER = "forge.auth.live.fixture.id"
CLIENTS = ("forge-workload", "forge-resource", "forge-fixture-admin")
VERSIONS = {"Authlib": "1.8.0", "requests": "2.34.2", "joserfc": "1.7.5"}
DRIVER_LABELS = (
    "PASS/jwt-http", "PASS/jwt-ws", "PASS/introspection-http",
    "PASS/introspection-ws", "READY/rotation", "PASS/rotation", "PASS/shutdown",
)
DRIVER_FAILURES = {
    ("FAIL/" + stage).encode("ascii"): stage
    for stage in (
        "initial-input", "configuration", "jwt-evidence", "introspection-evidence",
        "server-start", "jwt-http", "jwt-ws", "introspection-http",
        "introspection-ws", "rotation-input", "rotation", "shutdown",
        "cancellation-drain", "hard-deadline",
    )
}
MAX_JSON = 1024 * 1024
MAX_TOKEN = 16 * 1024
TIMEOUT = 190


class Failure(Exception):
    """Only fixture-owned stage labels may escape to the coordinator."""


def require(condition, stage):
    if not condition:
        raise Failure(stage)


def emit(label):
    print(label, flush=True)


def private_bytes(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as stream:
        metadata = os.fstat(stream.fileno())
        require(stat.S_ISREG(metadata.st_mode) and metadata.st_mode & 0o077 == 0,
                "private-inputs")
        require(0 < metadata.st_size <= MAX_JSON, "private-inputs")
        data = stream.read(MAX_JSON + 1)
    require(len(data) <= MAX_JSON, "private-inputs")
    return data


def private_json(path):
    return json.loads(private_bytes(path))


def write_private(path, content, replace):
    fd, temporary = tempfile.mkstemp(prefix=".resource-acceptance-", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            os.fchmod(stream.fileno(), 0o600)
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        if replace:
            os.replace(temporary, path)
        else:
            os.link(temporary, path)
    finally:
        with contextlib.suppress(FileNotFoundError):
            os.unlink(temporary)


def prepare(directory, replace):
    require(not directory.is_symlink(), "private-inputs")
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    require(directory.stat().st_mode & 0o077 == 0, "private-inputs")
    import_directory = directory / "import"
    require(not import_directory.is_symlink(), "private-inputs")
    import_directory.mkdir(mode=0o700, exist_ok=True)
    require(import_directory.stat().st_mode & 0o077 == 0, "private-inputs")
    import_path = import_directory / "forge-live-realm.json"
    credentials_path = directory / "resource-credentials.json"
    login_path = directory / "login.txt"
    for path in (import_path, credentials_path):
        if path.exists() or path.is_symlink():
            require(replace, "inputs-exist")
            require(private_json(path).get("realm") == "forge-live", "private-inputs")
    if login_path.exists() or login_path.is_symlink():
        require(replace, "inputs-exist")
        private_bytes(login_path)

    with Path(__file__).with_name("realm.json").open(encoding="utf-8") as stream:
        realm = json.load(stream)
    require(realm["realm"] == "forge-live", "template")
    fixture_id = secrets.token_hex(16)
    client_secrets = {client: secrets.token_urlsafe(32) for client in CLIENTS}
    require({c["clientId"] for c in realm["clients"] if not c["publicClient"]}
            == set(CLIENTS), "template")
    for client in realm["clients"]:
        if client["clientId"] in client_secrets:
            client["secret"] = client_secrets[client["clientId"]]
    realm.setdefault("attributes", {})[MARKER] = fixture_id
    password = secrets.token_urlsafe(32)
    realm["users"].append({
        "username": "fixture-user", "enabled": True,
        "firstName": "Forge", "lastName": "Fixture",
        "email": "fixture-user@example.invalid", "emailVerified": True,
        "requiredActions": [], "realmRoles": ["offline_access"],
        "credentials": [{"type": "password", "value": password, "temporary": False}],
    })
    credentials = {
        "version": 1, "realm": "forge-live", "issuer": ISSUER,
        "fixture_id": fixture_id, "clients": client_secrets,
    }
    # Each replacement is atomic; the marker rejects a mismatched pair after interruption.
    write_private(import_path, json.dumps(realm, indent=2) + "\n", replace)
    write_private(credentials_path, json.dumps(credentials, indent=2) + "\n", replace)
    write_private(login_path, "username: fixture-user\npassword: " + password + "\n", replace)
    emit("PASS/prepared")


def dependencies():
    for package, version in VERSIONS.items():
        require(importlib.metadata.version(package) == version, "dependencies")
    from authlib.integrations.requests_client import OAuth2Session
    from joserfc import jwt
    from joserfc.jwk import KeySet
    return OAuth2Session, jwt, KeySet


def response_json(response, status=200):
    require(response.status_code == status and len(response.content) <= MAX_JSON,
            "issuer-response")
    return response.json()


def token_response(response):
    require(response.status_code == 200 and len(response.content) <= MAX_JSON,
            "issuer-token")
    return response


class Issuer:
    def __init__(self, credentials, ca, stack, sdk):
        oauth, self.jwt, self.key_set = sdk
        self.credentials = credentials
        self.clients = {}
        for name in (None, *CLIENTS):
            client = stack.enter_context(oauth(
                client_id=name,
                client_secret=credentials["clients"].get(name),
                token_endpoint_auth_method="client_secret_basic",
                token_endpoint=TOKEN_URL, grant_type="client_credentials",
                leeway=0, default_timeout=(3, 8),
            ))
            client.trust_env = False
            client.verify = str(ca)
            client.max_redirects = 0
            client.register_compliance_hook("access_token_response", token_response)
            self.clients[name] = client

    def public(self, url):
        return response_json(self.clients[None].get(
            url, withhold_token=True, allow_redirects=False))

    def fetch(self, client, scope=None):
        parameters = {"scope": scope} if scope is not None else {}
        result = self.clients[client].fetch_token(
            TOKEN_URL, grant_type="client_credentials", allow_redirects=False,
            **parameters)
        token = result.get("access_token")
        require(isinstance(token, str) and 0 < len(token) <= MAX_TOKEN
                and token.isascii() and all(33 <= ord(c) <= 126 for c in token),
                "issuer-token")
        require(str(result.get("token_type", "")).lower() == "bearer", "issuer-token")
        return token

    def discovery(self):
        metadata = self.public(ISSUER + "/.well-known/openid-configuration")
        for name, value in {
            "issuer": ISSUER, "token_endpoint": TOKEN_URL,
            "jwks_uri": JWKS_URL, "introspection_endpoint": INTROSPECTION_URL,
        }.items():
            require(metadata.get(name) == value, "discovery")

    def admin(self, method, suffix="", **kwargs):
        return self.clients["forge-fixture-admin"].request(
            method, ADMIN + suffix, allow_redirects=False, **kwargs)

    def owned_realm(self):
        self.fetch("forge-fixture-admin")
        realm = response_json(self.admin("GET"))
        require(realm.get("realm") == "forge-live"
                and realm.get("attributes", {}).get(MARKER)
                == self.credentials["fixture_id"], "realm-ownership")
        require(isinstance(realm.get("id"), str) and realm["id"], "realm-ownership")
        return realm["id"]

    def verify_profile(self, token, jwks):
        verified = self.jwt.decode(token, self.key_set.import_key_set(jwks),
                                   algorithms=["RS256"])
        self.jwt.JWTClaimsRegistry(
            iss={"essential": True, "value": ISSUER},
            aud={"essential": True, "value": "forge-resource"},
            sub={"essential": True}, exp={"essential": True},
            iat={"essential": True}, jti={"essential": True},
            client_id={"essential": True, "value": "forge-workload"},
        ).validate(verified.claims)
        require(verified.header.get("typ") == "at+jwt", "token-profile")
        require("profile" in verified.claims.get("scope", "").split(), "token-profile")
        require(type(verified.claims["iat"]) is int
                and type(verified.claims["exp"]) is int
                and 0 < verified.claims["exp"] - verified.claims["iat"] <= 30,
                "token-profile")
        require(verified.claims["exp"] > time.time() + 10, "token-lifetime")
        kid = verified.header.get("kid")
        require(isinstance(kid, str) and kid, "token-profile")
        return kid

    def introspect(self, token):
        response = self.clients["forge-resource"].introspect_token(
            INTROSPECTION_URL, token=token, token_type_hint="access_token",
            allow_redirects=False)
        result = response_json(response)
        require(result.get("active") is True and result.get("iss") == ISSUER
                and result.get("client_id") == "forge-workload", "introspection")
        audience = result.get("aud", [])
        require("forge-resource" in ([audience] if isinstance(audience, str) else audience)
                and "profile" in result.get("scope", "").split(), "introspection")

    def rotate(self, realm_id, original_jwks, original_kid):
        # Re-check the imported ownership marker before the one permitted mutation.
        require(self.owned_realm() == realm_id, "realm-ownership")
        components = response_json(self.admin("GET", "/components", params={
            "parent": realm_id, "type": "org.keycloak.keys.KeyProvider",
        }))
        require(isinstance(components, list) and 0 < len(components) <= 128, "rotation")
        priority = max(int(c.get("config", {}).get("priority", ["0"])[0])
                       for c in components) + 100
        require(0 < priority < 2**31, "rotation")
        response = self.admin("POST", "/components", json={
            "name": "forge-live-rotation-" + secrets.token_hex(8),
            "parentId": realm_id, "providerType": "org.keycloak.keys.KeyProvider",
            "providerId": "rsa-generated",
            "config": {
                "priority": [str(priority)], "enabled": ["true"], "active": ["true"],
                "algorithm": ["RS256"], "keySize": ["2048"],
            },
        })
        require(response.status_code == 201, "rotation")
        token = self.fetch("forge-workload", "profile")
        published = self.public(JWKS_URL)
        kid = self.verify_profile(token, published)
        old = {key["kid"]: key for key in original_jwks["keys"]}
        new = {key["kid"]: key for key in published["keys"]}
        require(kid != original_kid and kid not in old and kid in new, "rotation")
        require(original_kid in new and old[original_kid] == new[original_kid], "rotation")
        # Only aggregate facts escape; no token, subject, jti, kid or key body is printed.
        emit("PASS/issuer-key-changed")
        emit("PASS/issuer-old-key-retained")
        return token


class Driver:
    def __init__(self, executable, ca):
        environment = {key: os.environ[key] for key in (
            "PATH", "LANG", "LC_ALL", "LC_CTYPE", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH",
        ) if key in os.environ}
        self.process = subprocess.Popen(
            [str(executable), "--ca", str(ca)], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0,
            close_fds=True, start_new_session=True, env=environment,
        )
        self.pending = bytearray()
        try:
            os.set_blocking(self.process.stdin.fileno(), False)
            os.set_blocking(self.process.stdout.fileno(), False)
        except BaseException:
            self.close()
            raise

    def send(self, record):
        data = memoryview((json.dumps(record, separators=(",", ":")) + "\n").encode())
        require(len(data) <= 32 * 1024, "driver-input")
        deadline = time.monotonic() + 10
        while data:
            remaining = deadline - time.monotonic()
            require(remaining > 0, "driver-input")
            _, writable, _ = select.select([], [self.process.stdin], [], remaining)
            require(writable, "driver-input")
            written = os.write(self.process.stdin.fileno(), data)
            require(written > 0, "driver-input")
            data = data[written:]

    def expect(self, expected):
        deadline = time.monotonic() + 90
        while b"\n" not in self.pending:
            remaining = deadline - time.monotonic()
            require(remaining > 0, "driver-protocol")
            readable, _, _ = select.select([self.process.stdout], [], [], remaining)
            require(readable, "driver-protocol")
            chunk = os.read(self.process.stdout.fileno(), 1024)
            require(chunk and len(self.pending) + len(chunk) <= 4096, "driver-protocol")
            self.pending.extend(chunk)
        line, _, tail = self.pending.partition(b"\n")
        self.pending = bytearray(tail)
        # Return only our constant stage, never decoded child output or a prefix match.
        failure = DRIVER_FAILURES.get(bytes(line))
        if failure is not None:
            raise Failure(failure)
        require(line == expected.encode("ascii"), "driver-protocol")
        emit(expected)

    def finish(self):
        self.process.stdin.close()
        require(self.process.wait(timeout=5) == 0, "driver-exit")
        require(not self.pending and not os.read(self.process.stdout.fileno(), 4096),
                "driver-protocol")

    def close(self):
        if self.process.poll() is None:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(self.process.pid, signal.SIGKILL)
            self.process.wait(timeout=2)
        self.process.stdin.close()
        self.process.stdout.close()


def run(directory, executable):
    sdk = dependencies()
    require(not directory.is_symlink() and directory.stat().st_mode & 0o077 == 0,
            "private-inputs")
    credentials = private_json(directory / "resource-credentials.json")
    require(credentials.get("version") == 1 and credentials.get("issuer") == ISSUER
            and credentials.get("realm") == "forge-live", "private-inputs")
    require(isinstance(credentials.get("fixture_id"), str)
            and len(credentials["fixture_id"]) == 32, "private-inputs")
    for client in CLIENTS:
        secret = credentials["clients"][client]
        require(isinstance(secret, str) and 32 <= len(secret) <= 4096, "private-inputs")
    ca = (directory / "tls" / "server.crt").resolve(strict=True)
    require(ca.is_file(), "private-inputs")
    executable = executable.resolve(strict=True)
    require(executable.is_file() and os.access(executable, os.X_OK), "driver-executable")
    with contextlib.ExitStack() as stack:
        issuer = Issuer(credentials, ca, stack, sdk)
        issuer.discovery()
        realm_id = issuer.owned_realm()
        original_jwks = issuer.public(JWKS_URL)
        token = issuer.fetch("forge-workload", "profile")
        original_kid = issuer.verify_profile(token, original_jwks)
        issuer.introspect(token)
        emit("PASS/issuer-profile")
        emit("PASS/issuer-introspection")
        driver = Driver(executable, ca)
        stack.callback(driver.close)
        driver.send({"phase": "initial", "token": token,
                     "introspection_client_secret": credentials["clients"]["forge-resource"]})
        for label in DRIVER_LABELS[:5]:
            driver.expect(label)
        rotated = issuer.rotate(realm_id, original_jwks, original_kid)
        driver.send({"phase": "rotation", "token": rotated})
        for label in DRIVER_LABELS[5:]:
            driver.expect(label)
        driver.finish()
    emit("PASS/resource-acceptance")


def timeout_handler(signum, frame):
    raise Failure("timeout")


def interruption_handler(signum, frame):
    raise Failure("interrupted")


class Parser(argparse.ArgumentParser):
    def error(self, message):
        raise Failure("arguments")


def main():
    logging.disable(logging.CRITICAL)
    warnings.simplefilter("ignore")
    parser = Parser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    preparation = commands.add_parser("prepare")
    preparation.add_argument("--directory", type=Path, required=True)
    preparation.add_argument("--replace", action="store_true")
    acceptance = commands.add_parser("run")
    acceptance.add_argument("--directory", type=Path, required=True)
    acceptance.add_argument("--driver", type=Path, required=True)
    stage = "arguments"
    try:
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        args = parser.parse_args()
        signal.signal(signal.SIGALRM, timeout_handler)
        signal.signal(signal.SIGTERM, interruption_handler)
        signal.setitimer(signal.ITIMER_REAL, TIMEOUT)
        stage = "preparation" if args.command == "prepare" else "resource-acceptance"
        if args.command == "prepare":
            prepare(args.directory.absolute(), args.replace)
        else:
            run(args.directory.absolute(), args.driver)
        return 0
    except Failure as error:
        emit("FAIL/" + error.args[0])
        return 124 if error.args[0] == "timeout" else 1
    except KeyboardInterrupt:
        emit("FAIL/interrupted")
        return 130
    except Exception:
        emit("FAIL/" + stage)
        return 1
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)


if __name__ == "__main__":
    sys.exit(main())
