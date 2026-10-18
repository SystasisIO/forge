"""Same-host raw-bit comparison, not cross-platform determinism acceptance."""

import os
import subprocess
import sys


def main():
    if len(sys.argv) not in (4, 5):
        raise SystemExit("usage: compare.py PROBE FIXTURE.wasm FIXTURE.aot [corpus]")
    case = sys.argv[4] if len(sys.argv) == 5 else "numeric"
    expected = 3165 if case == "corpus" else 7
    reference = None
    modes = ("nearest", "up", "down", "zero")
    for artifact in sys.argv[2:4]:
        for mode in modes:
            result = subprocess.run(
                [sys.argv[1], case, artifact], text=True, capture_output=True, timeout=10,
                env={**os.environ, "FORGE_WAMR_HOST_ROUNDING": mode},
            )
            if result.returncode:
                raise SystemExit(f"probe failed for {artifact}, rounding={mode}: {result.stderr}")
            # Only test-owned records are comparable, not upstream diagnostics.
            records = [line for line in result.stdout.splitlines() if line.startswith("bits ")]
            if len(records) != expected or len(set(line.split()[1] for line in records)) != expected:
                raise SystemExit("missing or duplicate numeric probe records")
            if reference is not None and reference != records:
                mismatch = [(a, b) for a, b in zip(reference, records) if a != b][:10]
                raise SystemExit(f"mismatch for {artifact}, rounding={mode}: {mismatch}")
            reference = records
    print(f"PASS: {expected} records agree across interpreter/AOT and four host rounding modes; full numeric gate remains open")


if __name__ == "__main__":
    main()
