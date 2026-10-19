"""Build a local fixture binding; it is trusted test installation input, not a token."""

import argparse
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--abi", type=Path, required=True)
parser.add_argument("--metadata", type=Path, required=True)
parser.add_argument("--constraints", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
value = {"version": 1, "root": "fixture::grant", "schema": "https://schemas.example.test/grant/v1"}
for name in ("abi", "metadata", "constraints"):
    value[name] = hashlib.sha256(getattr(args, name).read_bytes()).hexdigest()
args.output.write_text(json.dumps(value, sort_keys=True) + "\n")
