#!/usr/bin/env python3

import argparse
import hashlib
import json
from pathlib import Path


ARCHIVES = (
    "libforge_guest_runtime.a",
    "libforge_guest_raw.a",
    "libforge_guest_net_http_values.a",
    "libforge_guest_api_wasm.a",
    "libforge_guest_codec_base64.a",
    "libforge_guest_codec_base58.a",
    "libforge_guest_codec_hex.a",
    "libforge_guest_chain_protocol.a",
    "libforge_guest_contract.a",
    "libm.a",
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lib-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    entries = []
    for name in ARCHIVES:
        path = args.lib_dir / name
        if not path.is_file():
            raise SystemExit(f"missing Forge Contract foundation archive: {path}")
        entries.append(
            {
                "name": name,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            }
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({"version": 1, "archives": entries}, indent=2) + "\n")


if __name__ == "__main__":
    main()
