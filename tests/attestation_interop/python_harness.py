"""Independent J2 verifier / signer. Credentials never appear in diagnostics."""

import argparse
import base64
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import re
import sys

from joserfc import jws
from joserfc.errors import JoseError
from joserfc.jwk import ECKey
from jsonschema.exceptions import ValidationError as SchemaValidationError

from corpus import EXPECTED, HEADER, KID, NOW, encoded, json_bytes
from profile_checks import bounded_tree, check_document, check_header, require
from schema_example import ExportedModels

REGISTRY = jws.JWSRegistry(algorithms=["ES256"])
# joserfc 1.6.0 measures encoded segments, not decoded JSON bytes.
REGISTRY.max_header_length = 684
REGISTRY.max_payload_length = 10923
REGISTRY.max_signature_length = 86


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_bounded(path, limit):
    with path.open("rb") as source:
        result = source.read(limit + 1)
    require(len(result) <= limit)
    return result


def strict_json(raw, limit):
    require(len(raw) <= limit and b"\0" not in raw)
    text = raw.decode("utf-8", errors="strict")
    # Preflight container depth before stdlib builds the bounded JSON tree.
    depth, quoted, escaped = 0, False, False
    for char in text:
        if quoted:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
        elif char == '"':
            quoted = True
        elif char in "[{":
            depth += 1
            require(depth <= 8)
        elif char in "]}":
            depth -= 1

    def pairs(items):
        result = {}
        require(len(items) <= 128)
        for key, value in items:
            require(key not in result)
            result[key] = value
        return result

    def bad_constant(_):
        raise ValueError("invalid JSON constant")

    result = json.loads(text, object_pairs_hook=pairs, parse_constant=bad_constant)
    require(type(result) is dict)
    return bounded_tree(result)


def decode_segment(segment, limit):
    require(re.fullmatch(r"[A-Za-z0-9_-]+", segment) is not None)
    require(len(segment) <= (limit * 8 + 5) // 6)
    value = base64.b64decode(segment + "=" * (-len(segment) % 4), altchars=b"-_", validate=True)
    require(len(value) <= limit and encoded(value) == segment)
    return value


def public_key(path):
    data = strict_json(read_bounded(path, 4096), 4096)
    require(set(data) <= {"kty", "crv", "x", "y", "kid", "alg", "use", "key_ops"})
    require({"kty", "crv", "x", "y"} <= set(data))
    require(data["kty"] == "EC" and data["crv"] == "P-256")
    require(data.get("kid", KID) == KID and data.get("alg", "ES256") == "ES256")
    require(data.get("use", "sig") == "sig" and data.get("key_ops", ["verify"]) == ["verify"])
    require(len(decode_segment(data["x"], 32)) == 32 and len(decode_segment(data["y"], 32)) == 32)
    # No private-key or token-controlled key source is admitted.
    return ECKey.import_key(data)


def verify(token_path, key_path, consumer):
    token = read_bounded(token_path, 16384).decode("ascii", errors="strict")
    parts = token.split(".")
    require(len(parts) == 3 and all(parts))
    header_raw, payload_raw, signature = (decode_segment(p, cap) for p, cap in zip(parts, (512, 8192, 64)))
    require(len(signature) == 64)
    header = strict_json(header_raw, 512)
    check_header(header)
    # Bound/strict-parse without treating unsigned claims as authenticated.
    document = strict_json(payload_raw, 8192)
    checked = jws.deserialize_compact(token, public_key(key_path), algorithms=["ES256"], registry=REGISTRY)
    require(checked.payload == payload_raw and checked.protected == header)
    check_document(document)
    consumer.validate_json(payload_raw)
    return document


def write(path, raw):
    with path.open("xb") as target:
        target.write(raw)
    path.chmod(0o600)


def artifact(root, relative):
    path = (root / relative).resolve()
    require(path.is_relative_to(root.resolve()))
    return path


def accepted(token, key, consumer):
    try:
        verify(token, key, consumer)
        return True
    except (ValueError, TypeError, KeyError, JoseError, UnicodeError, SchemaValidationError):
        return False


def verify_manifest(root, manifest_path):
    consumer = ExportedModels(root)
    manifest = json.loads(manifest_path.read_text())
    require(manifest["now"] == NOW)
    for case in manifest["cases"]:
        token, key = (artifact(root, case[field]) for field in ("token", "jwk"))
        require(digest(token) == case["token_sha256"] and digest(key) == case["jwk_sha256"])
        require(accepted(token, key, consumer) == case["accept"])


def emit(root):
    require(importlib.metadata.version("joserfc") == "1.6.0")
    require(importlib.metadata.version("pydantic") == "2.14.1")
    output = root / "python"
    output.mkdir(mode=0o700)
    consumer = ExportedModels(root, generate=True)
    vectors = json.loads((root / "vectors.json").read_text())
    consumer.qualify(root, vectors)
    checked = verify(root / "forge.jwt", root / "public.jwk", consumer)
    # The coordinator owns jti generation; every other field must be identical.
    expected = {**EXPECTED, "jti": checked["jti"]}
    require(checked == expected)
    signing_key = ECKey.generate_key("P-256", parameters={"kid": KID})
    other_key = ECKey.generate_key("P-256", parameters={"kid": KID})
    write(output / "public.jwk", json_bytes(signing_key.as_dict(private=False)))
    write(output / "wrong.jwk", json_bytes(other_key.as_dict(private=False)))
    cases = []
    alg = REGISTRY.get_alg("ES256")
    for case in vectors:
        case_id, mode = case["id"], case["mode"]
        signing_input = (case["header"] + "." + case["payload"]).encode("ascii")
        # Library primitive is intentional for signed malformed JSON/header
        # vectors that a conforming high-level writer refuses to construct.
        signature = alg.sign(signing_input, signing_key)
        require(alg.verify(signing_input, signature, signing_key))
        token = signing_input.decode() + "." + encoded(signature)
        if case_id == "valid":
            token = jws.serialize_compact(HEADER, json_bytes(EXPECTED), signing_key, algorithms=["ES256"])
        if mode == "tampered":
            parts = token.split(".")
            raw = base64.urlsafe_b64decode(parts[1] + "=" * (-len(parts[1]) % 4))
            parts[1] = encoded(raw.replace(b'"o_17"', b'"o_18"'))
            token = ".".join(parts)
        elif mode == "padding":
            parts = token.split(".")
            parts[0] += "="
            token = ".".join(parts)
        elif mode == "noncanonical":
            alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
            token = token[:-1] + alphabet[alphabet.index(token[-1]) | 1]
        elif mode == "extra-segment":
            token += ".eA"
        elif mode == "empty-payload":
            token = token.split(".")[0] + ".." + token.split(".")[2]
        elif mode == "short-signature":
            token = signing_input.decode() + "." + encoded(signature[:-1])
        key_name = "wrong.jwk" if mode == "wrong-key" else "public.jwk"
        token_path = output / (case_id + ".jwt")
        write(token_path, token.encode("ascii"))
        key_path = output / key_name
        require(accepted(token_path, key_path, consumer) == case["accept"])
        cases.append({"id": case_id, "accept": case["accept"], "mode": mode,
                      "token": str(token_path.relative_to(root)), "jwk": str(key_path.relative_to(root)),
                      "token_sha256": digest(token_path), "jwk_sha256": digest(key_path)})
    write(output / "manifest.json", json_bytes({
        "version": 1, "producer": "joserfc-1.6.0", "now": NOW, "verified_forge": True,
        "forge_sha256": digest(root / "forge.jwt"), "public_sha256": digest(root / "public.jwk"),
        "vectors_sha256": digest(root / "vectors.json"), "cases": cases,
        "schema_sha256": digest(root / "document.schema.json"),
        "generated_sha256": digest(output / "models.py"),
    }))


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("emit", "verify-set"))
    parser.add_argument("directory", type=Path)
    parser.add_argument("manifest", nargs="?", type=Path)
    args = parser.parse_args()
    try:
        if args.mode == "emit":
            emit(args.directory.resolve())
        else:
            require(args.manifest is not None)
            verify_manifest(args.directory.resolve(), args.manifest)
    except Exception:
        print("J2 HARNESS_ERROR (details suppressed)", file=sys.stderr)
        return 2
    print("J2 PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
