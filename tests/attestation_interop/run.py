"""CI-only J2/J3 execution; --syntax is dependency-free and never builds/runs JOSE."""

import argparse
import ast
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.request

from corpus import AUDIENCE, ISSUER, KID, MAX_LIFETIME, NOW, PURPOSE, SCHEMA, SKEW, json_bytes, vectors

HERE = Path(__file__).resolve().parent
TIMEOUT = 120


class HarnessError(Exception):
    pass


class InteropFailure(Exception):
    pass


def require(condition):
    if not condition:
        raise HarnessError()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def syntax():
    for path in HERE.glob("*.py"):
        ast.parse(path.read_text(encoding="utf-8"), filename=path.name)
    lock = json.loads((HERE / "maven.lock.json").read_text())
    require(re.fullmatch("[0-9a-f]{64}", lock["sha256"]) is not None)
    require(lock["coordinate"] == "com.nimbusds:nimbus-jose-jwt:10.5")
    requirements = (HERE / "requirements.txt").read_text()
    require("--require-hashes" in requirements and "--only-binary=:all:" in requirements)
    pins = re.findall(r"(?m)^([a-z0-9_-]+)==([0-9.]+)((?: \\\n    --hash=sha256:[0-9a-f]{64})+)", requirements)
    require(len(pins) == 31 and len({name for name, _, _ in pins}) == 31)
    cases = vectors()
    require(len({c["id"] for c in cases}) == len(cases))
    require(all(c["id"].isascii() and set(c["id"]) <= set("abcdefghijklmnopqrstuvwxyz0123456789-_") for c in cases))
    print(f"SYNTAX PASS: Python AST / lock JSON / {len(pins)} hashed pins / {len(cases)} unsigned vectors; Java and interop NOT_RUN")


def invoke(command, stage, evidence):
    evidence["last_step"] = stage
    env = {**os.environ, "PYTHONDONTWRITEBYTECODE": "1"}
    # Never inherit stdout/stderr: even a failing dependency/driver must not put
    # a credential, private key, or exception input into CI logs or evidence.
    try:
        process = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 timeout=TIMEOUT, env=env, check=False)
    except (OSError, subprocess.TimeoutExpired):
        raise HarnessError() from None
    return process


def fetch_jar(root, supplied):
    lock = json.loads((HERE / "maven.lock.json").read_text())
    if supplied:
        path = Path(supplied).resolve()
        require(path.is_file() and sha(path) == lock["sha256"])
        return path
    url = lock["url"]
    require(url == "https://repo.maven.apache.org/maven2/com/nimbusds/nimbus-jose-jwt/10.5/nimbus-jose-jwt-10.5.jar")
    with urllib.request.urlopen(url, timeout=30) as response:
        require(response.url == url)
        raw = response.read(4 * 1024 * 1024 + 1)
    require(len(raw) <= 4 * 1024 * 1024 and hashlib.sha256(raw).hexdigest() == lock["sha256"])
    path = root / "nimbus-jose-jwt-10.5.jar"
    with path.open("xb") as target:
        target.write(raw)
    return path


def checked_artifact(root, relative):
    path = (root / relative).resolve()
    require(path.is_relative_to(root) and path.is_file())
    return path


def manifest(root, producer, expected_vectors):
    path = root / producer / "manifest.json"
    value = json.loads(path.read_text())
    require(value["version"] == 1 and value["now"] == NOW and value["verified_forge"] is True)
    require(value["forge_sha256"] == sha(root / "forge.jwt") and value["public_sha256"] == sha(root / "public.jwk"))
    require(value["vectors_sha256"] == sha(root / "vectors.json"))
    require(value["schema_sha256"] == sha(root / "document.schema.json"))
    require([c["id"] for c in value["cases"]] == [c["id"] for c in expected_vectors])
    for expected, case in zip(expected_vectors, value["cases"]):
        require(case["accept"] is expected["accept"] and case["mode"] == expected["mode"])
        token, key = (checked_artifact(root, case[field]) for field in ("token", "jwk"))
        require(sha(token) == case["token_sha256"] and sha(key) == case["jwk_sha256"])
        if case["accept"]:
            raw = token.read_bytes().split(b".")[1]
            payload = json.loads(base64.urlsafe_b64decode(raw + b"=" * (-len(raw) % 4)))
            reference = expected["payload"].encode()
            require(payload == json.loads(base64.urlsafe_b64decode(reference + b"=" * (-len(reference) % 4))))
    return value


def driver_verify(driver, token, key, expected, case_id, gate, evidence):
    process = invoke([str(driver), "verify", str(token), str(key)], gate + ":" + case_id, evidence)
    require(process.returncode in (0, 1))
    require(process.stdout.strip() == (b"ACCEPT" if process.returncode == 0 else b"REJECT"))
    matches = (process.returncode == 0) == expected
    evidence["cases"].append({"gate": gate, "id": case_id, "expected_accept": expected,
                              "driver_exit": process.returncode, "status": "PASS" if matches else "FAILED"})
    if not matches:
        evidence["gates"][gate] = "FAILED"
        raise InteropFailure()


def run(args, evidence):
    root = args.work.resolve()
    driver = args.driver.resolve()
    require(driver.is_file() and os.access(driver, os.X_OK))
    require(not root.exists())
    root.mkdir(parents=True, mode=0o700)
    cases = vectors()
    with (root / "vectors.json").open("xb") as target:
        target.write(json_bytes(cases))
    jar = fetch_jar(root, args.nimbus_jar or os.environ.get("NIMBUS_JAR"))
    require(shutil.which(args.java) is not None)
    java = [args.java, "--class-path", str(jar), str(HERE / "AttestationInterop.java")]
    python = [sys.executable, "-B", str(HERE / "python_harness.py")]
    evidence["inputs"] = {
        "driver_sha256": sha(driver), "nimbus_jar_sha256": sha(jar), "vectors_sha256": sha(root / "vectors.json"),
        "sources": {p.name: sha(p) for p in sorted(HERE.iterdir())
                    if p.suffix in (".py", ".java", ".json") or p.name == "requirements.txt"},
    }
    require(invoke([str(driver), "emit", str(root)], "forge-emit", evidence).returncode == 0)
    require(invoke([str(driver), "schema", str(root)], "forge-schema-export", evidence).returncode == 0)
    require((root / "forge.jwt").is_file() and (root / "public.jwk").is_file())
    require((root / "document.schema.json").is_file())
    # Baseline must be accepted by Forge itself, never just by external parsers.
    driver_verify(driver, root / "forge.jwt", root / "public.jwk", True, "forge-baseline", "J2", evidence)
    python_result = invoke(python + ["emit", str(root)], "python-verify-forge-and-emit", evidence)
    schema_report = root / "python" / "schema_evidence.json"
    if schema_report.exists():
        safe_schema = json.loads(schema_report.read_text())
        evidence["schema_checks"] = safe_schema["rows"]
        evidence["schema_qualification"] = {
            **{key: safe_schema[key] for key in ("status", "schema_sha256", "generated_sha256", "versions", "donor_hashes")},
            "report_sha256": sha(schema_report),
        }
        if safe_schema["status"] == "FAILED":
            evidence["gates"]["J2"] = "FAILED"
            raise InteropFailure()
    require(python_result.returncode == 0)
    require(invoke(java + ["emit", str(root)], "java-verify-forge-and-emit", evidence).returncode == 0)
    manifests = {name: manifest(root, name, cases) for name in ("python", "java")}
    # Each independent verifier must agree on the other library's entire corpus.
    require(invoke(python + ["verify-set", str(root), str(root / "java" / "manifest.json")],
                   "python-verify-java", evidence).returncode == 0)
    require(invoke(java + ["verify-set", str(root), str(root / "python" / "manifest.json")],
                   "java-verify-python", evidence).returncode == 0)
    for name, gate in (("python", "J2"), ("java", "J3")):
        for case in manifests[name]["cases"]:
            driver_verify(driver, checked_artifact(root, case["token"]), checked_artifact(root, case["jwk"]),
                          case["accept"], case["id"], gate, evidence)
        evidence["gates"][gate] = "PASS"
    evidence["artifacts"] = {
        "forge_sha256": sha(root / "forge.jwt"), "public_sha256": sha(root / "public.jwk"),
        "python_manifest_sha256": sha(root / "python" / "manifest.json"),
        "java_manifest_sha256": sha(root / "java" / "manifest.json"),
        "document_schema_sha256": sha(root / "document.schema.json"),
        "generated_models_sha256": sha(root / "python" / "models.py"),
        "schema_evidence_sha256": sha(root / "python" / "schema_evidence.json"),
    }
    evidence["last_step"] = "complete"


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--syntax", action="store_true")
    parser.add_argument("--driver", type=Path)
    parser.add_argument("--work", type=Path)
    parser.add_argument("--evidence", type=Path)
    parser.add_argument("--nimbus-jar", help="Optional pre-fetched exact pinned JAR; otherwise fetch from Maven Central")
    parser.add_argument("--java", default="java", help="JDK 17+ source launcher, used in CI only")
    args = parser.parse_args()
    if args.syntax:
        syntax()
        return 0
    if not all((args.driver, args.work, args.evidence)):
        parser.error("execution requires --driver, --work and --evidence")
    report = {"version": 1, "status": "NOT_RUN", "gates": {"J2": "NOT_RUN", "J3": "NOT_RUN"},
              "profile": {"now": NOW, "max_lifetime": MAX_LIFETIME, "clock_skew": SKEW,
                          "kid": KID, "iss": ISSUER, "aud": AUDIENCE, "purpose": PURPOSE, "schema": SCHEMA},
              "cases": [], "last_step": "preflight"}
    try:
        require(not args.evidence.exists())
        run(args, report)
        report["status"] = "PASS"
    except InteropFailure:
        report["status"] = "FAILED"
    except Exception:
        report["status"] = "HARNESS_ERROR"
    try:
        args.evidence.parent.mkdir(parents=True, exist_ok=True)
        with args.evidence.open("x", encoding="utf-8") as target:
            json.dump(report, target, indent=2, sort_keys=True)
            target.write("\n")
    except Exception:
        print("HARNESS_ERROR: evidence not written", file=sys.stderr)
        return 2
    print("J2/J3 " + report["status"] + "; last_step=" + report["last_step"])
    return {"PASS": 0, "FAILED": 1, "HARNESS_ERROR": 2}[report["status"]]


if __name__ == "__main__":
    sys.exit(main())
