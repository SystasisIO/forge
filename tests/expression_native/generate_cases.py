#!/usr/bin/env python3
"""Embed immutable test inputs; no expression parsing or evaluation happens here."""

import argparse
import hashlib
import json
import re
from pathlib import Path


def generate(corpus: Path, output: Path) -> None:
    original = corpus.read_bytes()
    document = json.loads(original)
    if document.get("schema") != "forge.expression.native.corpus.v1":
        raise ValueError("unsupported corpus schema")
    identifiers = set()
    records = []
    for case in document["cases"]:
        identifier = case["id"]
        if not re.fullmatch(r"[a-z0-9_]+", identifier) or identifier in identifiers:
            raise ValueError("invalid or duplicate case identifier")
        identifiers.add(identifier)
        inputs = []
        names = set()
        for value in case["inputs"]:
            name = value["name"]
            if not re.fullmatch(r"[a-zA-Z_][a-zA-Z_0-9]*", name) or name in names:
                raise ValueError("invalid or duplicate input identifier")
            names.add(name)
            if value["type"] not in {"int64", "uint64", "bool", "list_int64"}:
                raise ValueError("unsupported fixture input type")
            strings = [json.dumps(value[key]) for key in ("name", "type", "value")]
            strings.append("{" + ",".join(json.dumps(item) for item in value["items"]) + "}")
            inputs.append("{" + ",".join(strings) + "}")
        expressions = case["expressions"]
        records.append("{" + ",".join([
            json.dumps(identifier), json.dumps(expressions["cel"]), json.dumps(expressions["exprtk"]),
            "{" + ",".join(inputs) + "}", "true" if case["benchmark"] else "false",
        ]) + "}")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text('inline constexpr std::string_view corpus_sha256 = "' +
                      hashlib.sha256(original).hexdigest() + '";\n' +
                      "inline const std::vector<test_case> cases = {\n" + ",\n".join(records) + "\n};\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, default=Path(__file__).with_name("corpus.json"))
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    generate(arguments.corpus, arguments.output)
