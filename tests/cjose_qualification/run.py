"""Public-only C backend qualification; private ephemeral keys stay in Python."""

import argparse
import base64
import hashlib
import json
import os
import pathlib
import resource
import selectors
import subprocess
import time

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec, utils
from jwcrypto import jwk, jws

HEADER = {"alg": "ES256", "typ": "application/ci-jws+octets", "kid": "ci-key"}
RFC_KEY = {"kty": "EC", "crv": "P-256", "x": "f83OJ3D2xF1Bg8vub9tLe1gHMzV76e8Tus9uPHvRVEU",
           "y": "x_FEzRu9m36HLN_tue659LNpXW6pCyStikYjKIWI5a0"}
RFC_INPUT = (b"eyJhbGciOiJFUzI1NiJ9.eyJpc3MiOiJqb2UiLA0KICJleHAiOjEzMDA4MTkzODAsDQogImh0dHA6Ly9leGFt"
             b"cGxlLmNvbS9pc19yb290Ijp0cnVlfQ")
RFC_SIGNATURE = "DtEhU3ljbEg8L38VWAfUAqOyKAM6-Xx-F4GawxaepmXFCgfTjDxw5djxLa8ISlSApmWQxfKTUJqPP3-Kg6NU1Q"


def b64(value):
    return base64.urlsafe_b64encode(value).rstrip(b"=")


def unb64(value):
    return base64.urlsafe_b64decode(value + b"=" * (-len(value) % 4))


def public(key):
    result = json.loads(key.export_public())
    assert set(result) <= {"kty", "crv", "x", "y", "kid"}
    return result


def sign_digest(key, digest):
    # Independent OpenSSL-backed signer; only its public projection goes to C.
    der = key.get_op_key("sign").sign(digest, ec.ECDSA(utils.Prehashed(hashes.SHA256())))
    r, s = utils.decode_dss_signature(der)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def signed_raw(key, header, payload):
    signing_input = b64(header) + b"." + b64(payload)
    return signing_input + b"." + b64(sign_digest(key, hashlib.sha256(signing_input).digest()))


def python_verify(key, token, payload):
    verifier = jws.JWS()
    verifier.allowed_algs = ["ES256"]
    verifier.deserialize(token.decode("ascii"))
    verifier.verify(jwk.JWK(**public(key)), alg="ES256")
    assert verifier.payload == payload


class Probe:
    def __init__(self, binary, directory):
        self.stderr = (directory / "probe.stderr").open("wb")
        self.process = subprocess.Popen([str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.stderr, start_new_session=True, bufsize=0)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.timings = {}

    def ask(self, op, **fields):
        started = time.monotonic()
        request = {"op": op, **fields}
        buffer = memoryview(json.dumps(request, separators=(",", ":")).encode() + b"\n")
        while buffer:
            written = self.process.stdin.write(buffer)
            if not written:
                raise RuntimeError("probe_stdin")
            buffer = buffer[written:]
        self.process.stdin.flush()
        deadline = time.monotonic() + 15
        line = bytearray()
        while not line.endswith(b"\n"):
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not self.selector.select(timeout=remaining):
                raise RuntimeError("probe_timeout")
            chunk = os.read(self.process.stdout.fileno(), 400000 - len(line))
            if not chunk or len(line) + len(chunk) >= 400000:
                raise RuntimeError("probe_failed_or_unbounded_output")
            line.extend(chunk)
        self.timings.setdefault(op, []).append(time.monotonic() - started)
        return json.loads(line)

    def close(self):
        self.process.stdin.close()
        try:
            code = self.process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            code = 124
        self.selector.close()
        self.stderr.close()
        if code:
            raise RuntimeError("probe_exit")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True, type=pathlib.Path)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--evidence", required=True, type=pathlib.Path)
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    corpus = args.work / "corpus"
    corpus.mkdir(exist_ok=True)
    report = {"status": "RUNNING", "cases": [], "private_key_to_backend": False,
              "profile": "fixture_only", "allocator_scope": "CJOSE+Jansson+OpenSSL hooks, warm process",
              "gaps": ["macOS LeakSanitizer unavailable", "not a production profile codec", "no provider lifecycle/Q6", "Q7 coordinator-owned",
                       "private-key rejection is extension unit-suite coverage; public probe never ingests one",
                       "Jansson/OpenSSL dependencies are not sanitizer-instrumented; backend and fixture are"]}

    def persist():
        args.evidence.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")

    def check(name, operation):
        start = time.monotonic()
        record = {"name": name, "status": "RUNNING"}
        report["cases"].append(record)
        persist()
        try:
            operation()
            record["status"] = "PASS"
        except Exception:
            # Exception strings may contain raw tokens; never emit them.
            record["status"] = "FAILED"
            raise
        finally:
            record["elapsed_seconds"] = round(time.monotonic() - start, 6)
            persist()

    def require(value):
        if not value:
            raise AssertionError("qualification_assertion")

    probe = Probe(args.probe, args.work)
    key = jwk.JWK.generate(kty="EC", crv="P-256")
    other = jwk.JWK.generate(kty="EC", crv="P-256")
    payload = bytes(range(256)) + b"\x00\xffbinary\x00payload"

    def prepare(slot=0, value=payload, header=None, signer=key, fail_at=0):
        return probe.ask("prepare", slot=slot, key=public(signer), header=header or HEADER,
                         payload=value.hex(), fail_at=fail_at)

    def view(slot=0):
        result = probe.ask("view", slot=slot)
        require(result["ok"])
        data, digest = bytes.fromhex(result["input"]), bytes.fromhex(result["digest"])
        require(digest == hashlib.sha256(data).digest())
        return data, digest

    def complete(signature, slot=0, **fields):
        return probe.ask("complete", slot=slot, signature=signature.hex(), **fields)

    def release(slot=0):
        require(probe.ask("release", slot=slot)["ok"])

    def verify(token, signer=key, backend=False):
        return probe.ask("verify_backend" if backend else "verify", token=token.hex(), key=public(signer))["ok"]

    def rfc():
        result = probe.ask("prepare", key=RFC_KEY, header={"alg": "ES256"},
                           payload=unb64(RFC_INPUT.split(b".")[1]).hex())
        require(result["ok"])
        data, _ = view()
        require(data == RFC_INPUT)
        result = complete(unb64(RFC_SIGNATURE.encode()))
        require(result["ok"])
        token = bytes.fromhex(result["token"])
        require(token == RFC_INPUT + b"." + RFC_SIGNATURE.encode())
        (corpus / "rfc7515-a3").write_bytes(token)
        verifier = jws.JWS()
        verifier.allowed_algs = ["ES256"]
        verifier.deserialize(token.decode())
        verifier.verify(jwk.JWK(**RFC_KEY), alg="ES256")
        require(probe.ask("verify_backend", token=token.hex(), key=RFC_KEY)["ok"])
        require(view()[0] == RFC_INPUT)
        release()

    def external():
        require(prepare()["ok"])
        data, digest = view()
        expected = b64(json.dumps(HEADER, separators=(",", ":")).encode()) + b"." + b64(payload)
        require(data == expected)
        result = complete(sign_digest(key, digest))
        require(result["ok"])
        token = bytes.fromhex(result["token"])
        python_verify(key, token, payload)
        require(verify(token))
        require(view()[0] == data)
        again = complete(sign_digest(key, digest))
        require(not again["ok"] and "token" not in again)
        release()

    def python_to_c():
        envelope = jws.JWS(payload)
        envelope.allowed_algs = ["ES256"]
        envelope.add_signature(key, alg="ES256", protected=json.dumps(HEADER, separators=(",", ":")))
        token = envelope.serialize(compact=True).encode()
        require(verify(token))
        python_verify(key, token, payload)
        (corpus / "valid-compact").write_bytes(token)

    def reject_signatures():
        require(prepare()["ok"])
        data, digest = view()
        good = sign_digest(key, digest)
        r, s = int.from_bytes(good[:32], "big"), int.from_bytes(good[32:], "big")
        bad = [b"", good[:63], b"\x01" + good, good + b"\x00", utils.encode_dss_signature(r, s),
               bytes(64), bytes([good[0] ^ 1]) + good[1:], sign_digest(other, digest),
               sign_digest(key, hashlib.sha256(digest).digest()),
               sign_digest(key, hashlib.sha256(data.split(b".")[0] + b"." + b64(b"different-payload")).digest()),
               sign_digest(key, hashlib.sha256(b64(json.dumps({**HEADER, "kid": "different-header"}, separators=(",", ":")).encode()) + b"." + b64(payload)).digest())]
        for value in bad:
            result = complete(value)
            require(not result["ok"] and "token" not in result)
            require(view()[0] == data)
        result = complete(good, release_first=True)
        require(result["ok"])
        python_verify(key, bytes.fromhex(result["token"]), payload)

    def substitution():
        for header in ({**HEADER, "alg": "none"}, {**HEADER, "alg": "HS256"},
                       {**HEADER, "alg": "ES384"}, {**HEADER, "b64": False}):
            require(not prepare(header=header)["ok"])
        wrong_curve = jwk.JWK.generate(kty="EC", crv="P-384")
        require(not prepare(signer=wrong_curve)["ok"])
        require(prepare(slot=0)["ok"] and prepare(slot=1, signer=other, value=b"other")["ok"])
        _, digest0 = view(0)
        _, digest1 = view(1)
        require(not complete(sign_digest(other, digest1), slot=0)["ok"])
        require(not complete(sign_digest(key, digest0), slot=1)["ok"])
        require(complete(sign_digest(key, digest0), slot=0)["ok"])
        require(complete(sign_digest(other, digest1), slot=1)["ok"])
        release(0)
        release(1)

    def parallel():
        signatures = []
        values = []
        for slot in range(8):
            value = bytes([slot]) * (slot + 1)
            values.append(value)
            require(prepare(slot=slot, value=value)["ok"])
            signatures.append(sign_digest(key, view(slot)[1]).hex())
        result = probe.ask("parallel", signatures=signatures)
        require(result["ok"])
        for slot, token in enumerate(result["tokens"]):
            python_verify(key, bytes.fromhex(token["token"]), values[slot])
            release(slot)

    def profile_cases():
        header = json.dumps(HEADER, separators=(",", ":")).encode()
        good = signed_raw(key, header, payload)
        # Malformed headers are signed correctly, so rejection cannot hide
        # behind a broken signature or a different trust key.
        bad_headers = {
            "invalid_utf8": b'{"alg":"ES256","typ":"\xff","kid":"ci-key"}',
            "invalid_json": b'{"alg":"ES256",',
            "duplicate_alg": b'{"alg":"none",' + header[1:],
            "duplicate_same_alg": b'{"alg":"ES256",' + header[1:],
            "literal_nul": header[:-1] + b'\x00}',
            "escaped_nul": header.replace(b'ci-key', b'ci-key\\u0000evil'),
            "nonobject": b'[]', "trailing_json": header + b'{}',
            "deep_nesting": b'{"alg":"ES256","typ":"application/ci-jws+octets","kid":' + b'[' * 400 + b'0' + b']' * 400 + b'}',
            "header_overflow": header + b' ' * (1025 - len(header)),
        }
        variations = {
            "none": {**HEADER, "alg": "none"}, "hs256": {**HEADER, "alg": "HS256"},
            "es384": {**HEADER, "alg": "ES384"}, "alg_type": {**HEADER, "alg": 1},
            "typ_type": {**HEADER, "typ": []}, "typ_mismatch": {**HEADER, "typ": "JWT"},
            "kid_type": {**HEADER, "kid": []}, "kid_empty": {**HEADER, "kid": ""},
            "kid_overflow": {**HEADER, "kid": "k" * 129}, "kid_control": {**HEADER, "kid": "x\ny"},
            "kid_unicode": {**HEADER, "kid": "\u00e9"}, "unknown": {**HEADER, "unknown": 1},
            "crit": {**HEADER, "crit": ["unknown"]}, "empty_crit": {**HEADER, "crit": []},
            "b64_false": {**HEADER, "b64": False}, "b64_true": {**HEADER, "b64": True},
            "jku": {**HEADER, "jku": "https://ci.invalid/jwks"},
            "x5u": {**HEADER, "x5u": "https://ci.invalid/cert"}, "jwk": {**HEADER, "jwk": public(other)},
            "x5c": {**HEADER, "x5c": []}, "cty": {**HEADER, "cty": "JWT"},
        }
        variations.update({"missing_" + field: {k: v for k, v in HEADER.items() if k != field} for field in HEADER})
        bad_headers.update({name: json.dumps(value, separators=(",", ":")).encode() for name, value in variations.items()})
        for name, raw in bad_headers.items():
            token = signed_raw(key, raw, payload)
            check("Q4_" + name, lambda token=token: require(not verify(token)))
            (corpus / name).write_bytes(token)
        parts = good.split(b".")
        malformed = {"extra_segment": good + b".extra", "missing_segment": b".".join(parts[:2]),
                     "detached": parts[0] + b".." + parts[2], "embedded_nul": good + b"\x00",
                     "unencoded": parts[0] + b".raw+payload." + parts[2],
                     "token_at_limit": b"A" * 100000, "token_overflow": b"A" * 100001,
                     "empty_header": b"." + parts[1] + b"." + parts[2],
                     "signature_short": parts[0] + b"." + parts[1] + b"." + b64(bytes(63)),
                     "signature_long": parts[0] + b"." + parts[1] + b"." + b64(bytes(65)),
                     "empty_signature": parts[0] + b"." + parts[1] + b"."}
        for index in range(3):
            for suffix in (b"=", b"+", b"/", b" ", b"\n"):
                changed = parts.copy()
                changed[index] += suffix
                malformed[f"base64_{index}_{suffix.hex()}"] = b".".join(changed)
            changed = parts.copy()
            changed[index] = b"A"
            malformed[f"base64_mod1_{index}"] = b".".join(changed)
        alphabet = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
        last = alphabet.index(parts[2][-1])
        malformed["noncanonical_pad_bits"] = b".".join(parts[:2] + [parts[2][:-1] + bytes([alphabet[last | 1]])])
        for name, token in malformed.items():
            check("Q4_" + name, lambda token=token: require(not verify(token)))
            (corpus / name).write_bytes(token)
        check("Q4_wrong_public_key", lambda: require(not verify(good, other)))

    def boundaries():
        for size in (1, 65535, 65536):
            value = bytes([size % 251]) * size
            require(prepare(value=value)["ok"])
            result = complete(sign_digest(key, view()[1]))
            require(result["ok"])
            token = bytes.fromhex(result["token"])
            require(verify(token))
            python_verify(key, token, value)
            release()
        empty_header = json.dumps(HEADER, separators=(",", ":")).encode()
        padded_header = empty_header + b" " * (1024 - len(empty_header))
        require(verify(signed_raw(key, padded_header, b"x")))
        require(verify(signed_raw(key, json.dumps({**HEADER, "kid": "k" * 128}).encode(), b"x")))
        require(not verify(signed_raw(key, empty_header, b"x" * 65537)))
        require(not verify(signed_raw(key, empty_header, b"")))
        # Empty payload is valid backend JWS, but deliberately not this profile.
        require(prepare(value=b"")["ok"])
        result = complete(sign_digest(key, view()[1]))
        require(result["ok"])
        empty_token = bytes.fromhex(result["token"])
        python_verify(key, empty_token, b"")
        require(verify(empty_token, backend=True))
        release()

    def allocation_failures():
        # Warm identical paths before counting; fail exactly one allocation per
        # attempt. No injection while control JSON or responses are processed.
        for _ in range(2):
            require(prepare()["ok"])
            require(complete(sign_digest(key, view()[1]))["ok"])
            release()
        baseline_prepare = prepare()
        require(baseline_prepare["ok"])
        count_prepare = baseline_prepare["allocations"]
        signature = sign_digest(key, view()[1])
        baseline_complete = complete(signature)
        require(baseline_complete["ok"])
        count_complete = baseline_complete["allocations"]
        release()
        require(0 < count_prepare <= 4096 and 0 < count_complete <= 4096)
        results = {}
        for operation, count in (("prepare", count_prepare), ("complete", count_complete)):
            hits = 0
            positions = []
            for index in range(1, count + 1):
                if operation == "prepare":
                    result = prepare(fail_at=index)
                    hits += result["failure_hit"]
                    if not result["ok"]:
                        require(prepare()["ok"])
                    require(complete(sign_digest(key, view()[1]))["ok"])
                else:
                    require(prepare()["ok"])
                    signature = sign_digest(key, view()[1])
                    result = complete(signature, fail_at=index)
                    hits += result["failure_hit"]
                    if not result["ok"]:
                        require("token" not in result)
                        require(complete(signature)["ok"])
                release()
                positions.append({"index": index, "failure_hit": result["failure_hit"], "initial_success": result["ok"], "recovered": True})
            results[operation] = {"measured_allocations": count, "injected_positions": count, "hits": hits, "positions": positions}
            require(hits == count)
        report["allocation_failures"] = results

    persist()
    try:
        check("Q2_RFC7515_A3_input_digest_known_signature", rfc)
        check("Q2_external_digest_sign_python_JWS_verify", external)
        check("Q2_python_JWS_sign_C_verify_binary", python_to_c)
        check("Q3_invalid_signatures_retry_and_independent_lifetimes", reject_signatures)
        check("Q3_key_algorithm_substitution", substitution)
        check("Q3_eight_concurrent_distinct_states", parallel)
        check("Q3_null_arguments", lambda: require(probe.ask("null_arguments")["ok"]))
        profile_cases()
        check("Q5_exact_limits_and_overflow", boundaries)
        check("Q5_allocator_failure_prepare_complete", allocation_failures)
        probe.close()
        report["status"] = "PASS"
    except Exception:
        report["status"] = "FAILED"
        if probe.process.poll() is None:
            probe.process.kill()
            probe.process.wait()
        raise
    finally:
        usage = resource.getrusage(resource.RUSAGE_CHILDREN)
        report["measurements"] = {"child_peak_rss_bytes": usage.ru_maxrss if os.uname().sysname == "Darwin" else usage.ru_maxrss * 1024,
                                  "child_user_seconds": usage.ru_utime, "child_system_seconds": usage.ru_stime,
                                  "rpc_timeout_seconds": 15}
        report["measurements"]["rpc_latency_seconds"] = {
            op: {"count": len(values), "p50": sorted(values)[len(values) // 2],
                 "p95": sorted(values)[min(len(values) - 1, int(len(values) * 0.95))], "max": max(values)}
            for op, values in probe.timings.items()}
        if report["measurements"]["child_peak_rss_bytes"] > 1024 * 1024 * 1024:
            report["status"] = "FAILED"
        persist()
    require(report["status"] == "PASS")


if __name__ == "__main__":
    try:
        main()
    except Exception:
        print("public qualification failed; sanitized case status saved")
        raise SystemExit(1)
