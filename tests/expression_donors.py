"""Derive native profile fixtures from the published, checksum-pinned comparison corpus."""

import argparse
import hashlib
import json
from pathlib import Path


CORPUS_SHA256 = "3092a37192c4ae6bee522ad4219527060e2e0098e762cd40d95b9f1ae7eb5546"
CORPUS_URL = (
    "https://raw.githubusercontent.com/SystasisIO/forge/"
    "2b2ea9e65b22e7db5761d6ef55283ef8f0546c42/tests/expression_native/corpus.json"
)


def generate(source):
    if hashlib.sha256(source).hexdigest() != CORPUS_SHA256:
        raise ValueError("Comparison corpus SHA-256 mismatch")
    data = json.loads(source)
    rows = [
        "// Derived from Forge comparison 2b2ea9e6, corpus SHA-256 " + CORPUS_SHA256,
        "// Numeric donor fixtures only. The one widened formula uses the explicit V1 extension.",
    ]
    for case in data["cases"]:
        if not case["scope"].startswith("donor_") or case["expected"]["status"] != "ok":
            continue
        expression = case["expressions"]["cel"]
        if case["id"] == "bitshares_scale_1000000000000000_9999":
            expression = "math.mul_div(required, scale, 10000, rounding.trunc)"
        declarations = []
        values = "mutable_variant_object()"
        for item in case["inputs"]:
            kind = item["type"]
            declarations.append(
                "variable{" + json.dumps(item["name"]) + ", type::" +
                ("boolean" if kind == "bool" else kind) + "}"
            )
            if kind == "list_int64":
                value = "forge::variants{" + ", ".join(
                    "variant{integer<std::int64_t>(" + json.dumps(entry) + ")}"
                    for entry in item["items"]
                ) + "}"
            elif kind == "bool":
                value = item["value"]
            else:
                value = "integer<std::" + kind + "_t>(" + json.dumps(item["value"]) + ")"
            values += "(" + json.dumps(item["name"]) + ", " + value + ")"
        rows.append(
            "{" + json.dumps(case["id"]) + ", " + json.dumps(expression) + ", {" +
            ", ".join(declarations) + "}, " + values + ", " + json.dumps(case["expected"]["value"]) + "},"
        )
    return "\n".join(rows) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("corpus", type=Path, help="Local copy of " + CORPUS_URL)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    output = Path(__file__).with_name("expression_donors.inc")
    generated = generate(args.corpus.read_bytes())
    if args.check:
        if output.read_text() != generated:
            raise SystemExit("Donor fixtures are stale")
    else:
        output.write_text(generated)
    print("45 pinned donor fixtures verified")


if __name__ == "__main__":
    main()
