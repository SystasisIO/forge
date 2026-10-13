"""Check delivery after main-thread TLS teardown and bounded safe stderr."""

import subprocess
import sys


def main() -> None:
    result = subprocess.run(
        [sys.argv[1]], capture_output=True, text=True, timeout=10, check=True
    )
    assert result.stdout == "static destructor delivered\n", result.stdout
    assert result.stderr == "Forge logging failed; records may be lost\n", result.stderr


if __name__ == "__main__":
    main()
