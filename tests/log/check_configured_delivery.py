"""An unread OS stderr pipe must not stall HTTP or producers."""

import selectors
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> None:
    process = subprocess.Popen(
        [sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            assert selector.select(20), "HTTP made no progress while stderr was blocked"
            first = process.stdout.readline()
        assert first == b"HTTP_PROGRESS_WITH_BLOCKED_STDERR\n", first
        # Only now drain the pipe. A synchronous implementation cannot reach here.
        stdout, stderr = process.communicate(b"drain\n", timeout=20)
        assert process.returncode == 0, (process.returncode, stdout, stderr[-2048:])
        assert stdout == b"DRAINED\n", stdout
        assert b"late record is dropped" not in stderr
        assert len(stderr) > 65536, "fixture did not produce enough output to fill a pipe"
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate(timeout=5)
    with tempfile.TemporaryDirectory() as directory:
        subprocess.run(
            [sys.argv[1], "failed-output"], capture_output=True, check=True, timeout=20,
        )
        subprocess.run(
            [sys.argv[1], "retention", str(Path(directory) / "sentinel.log")],
            capture_output=True, check=True, timeout=20,
        )
        if hasattr(os, "mkfifo"):
            marker = Path(directory) / "preparing.log"
            marker.write_text("not-yet-prepared", encoding="utf-8")
            fifo = Path(directory) / "blocked-output"
            os.mkfifo(fifo)
            subprocess.run(
                [sys.argv[1], "configure-shutdown", str(marker), str(fifo)],
                capture_output=True, check=True, timeout=20,
            )


if __name__ == "__main__":
    main()
