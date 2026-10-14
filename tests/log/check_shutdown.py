"""Check delivery after main-thread TLS teardown without emergency I/O."""

import subprocess
import sys


def main() -> None:
    result = subprocess.run(
        [sys.argv[1]], capture_output=True, text=True, timeout=10, check=True
    )
    assert result.stdout == "static destructor delivered\n", result.stdout
    assert result.stderr == "", result.stderr


if __name__ == "__main__":
    main()
