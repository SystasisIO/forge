"""Fail closed on a duplicate/old common provider, then run the real C probe."""

import argparse
from collections import Counter
from pathlib import Path
import re
import subprocess


SYMBOLS = tuple(
    "wasm_runtime_" + name
    for name in (
        "instantiate_internal",
        "initialize_with_exec_env",
        "call_wasm",
        "invoke_native",
        "invoke_native_raw",
        "call_indirect",
    )
)


def verify(link_map: str, nm_output: str) -> None:
    if not re.search(r"forge_wamr_fp_status_common\.dir/[^\n]*wasm_runtime_common\.c\.o", link_map):
        raise ValueError("missing explicit fault-injected common object in link map")
    archive = r"libforge_vm_wasm_interpret\.a(?:\[\d+\])?\([^\n)]*"
    if re.search(archive + r"wasm_runtime_common\.c\.o\)", link_map):
        raise ValueError("uninjected common member extracted from the runtime archive")
    if not re.search(archive + r"wasm_exec_env\.c\.o\)", link_map):
        raise ValueError("missing exec-env allocator from the real runtime archive")
    counts = Counter()
    for line in nm_output.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[-2].upper() != "U":
            symbol = fields[-1].removeprefix("_")
            if symbol in SYMBOLS:
                counts[symbol] += 1
    if any(counts[symbol] != 1 for symbol in SYMBOLS):
        raise ValueError(f"expected one definition per common wrapper: {dict(counts)}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--map", required=True, type=Path)
    parser.add_argument("--nm", required=True)
    args = parser.parse_args()
    symbols = subprocess.run(
        [args.nm, "-g", str(args.binary)], check=True, capture_output=True, text=True, timeout=10
    )
    verify(args.map.read_bytes().decode("latin-1"), symbols.stdout)
    print("PASS one injected common provider plus the real same-profile runtime archive", flush=True)
    subprocess.run([str(args.binary)], check=True, timeout=10)


if __name__ == "__main__":
    main()
