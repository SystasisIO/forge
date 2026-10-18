"""Drive only the password helper's private PTY or stdin; never replace runner stdin."""

import argparse
import errno
import os
from pathlib import Path
import select
import subprocess
import termios
import tempfile
import time


def require(value, message):
    if not value:
        raise AssertionError(message)


def available(master, output):
    if select.select([master], [], [], 0.02)[0]:
        try:
            output.extend(os.read(master, 4096))
        except OSError as failure:
            if failure.errno != errno.EIO:
                raise


def terminal_case(binary, action, expected):
    master, slave = os.openpty()
    control_read, control_write = os.pipe()
    original = termios.tcgetattr(slave)
    process = None
    output = bytearray()
    try:
        process = subprocess.Popen([str(binary), "terminal", str(control_read)],
                                   stdin=slave, stdout=slave, stderr=slave, pass_fds=(control_read,))
        os.close(control_read)
        control_read = -1
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            available(master, output)
            if b"Secret: " in output and not termios.tcgetattr(slave)[3] & termios.ECHO:
                break
            require(process.poll() is None, "Password helper exited before hidden terminal input")
        else:
            raise AssertionError("Password input did not disable echo on its private PTY")
        started = time.monotonic()
        if action == "cancel":
            os.write(control_write, b"c")
        else:
            os.write(master, action)
        os.close(control_write)
        control_write = -1
        while process.poll() is None and time.monotonic() < deadline:
            available(master, output)
        require(process.poll() is not None, "Password input did not finish or join its cancellation")
        available(master, output)
        require(process.returncode == expected, "Password input returned an unexpected typed outcome")
        require(termios.tcgetattr(slave) == original, "Password input did not restore terminal attributes")
        require(b"fixture-password" not in output, "Hidden password was echoed or printed")
        if action == "cancel":
            require(time.monotonic() - started < 1.5, "Password cancellation did not settle promptly")
    finally:
        if control_write >= 0:
            os.close(control_write)
        if control_read >= 0:
            os.close(control_read)
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        os.close(master)
        os.close(slave)


def stdin_cancel(binary):
    control_read, control_write = os.pipe()
    process = None
    try:
        process = subprocess.Popen([str(binary), "stdin", str(control_read)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, pass_fds=(control_read,))
        os.close(control_read)
        control_read = -1
        # Keep stdin open and empty: cancellation must interrupt waiting, not depend on EOF.
        os.write(control_write, b"c")
        os.close(control_write)
        control_write = -1
        process.wait(timeout=2)
        require(process.returncode == 130, "Explicit stdin input ignored cooperative cancellation")
        require(b"fixture-password" not in process.stdout.read() + process.stderr.read(),
                "Password cancellation leaked input material")
    finally:
        if control_write >= 0:
            os.close(control_write)
        if control_read >= 0:
            os.close(control_read)
        if process is not None:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            for stream in (process.stdin, process.stdout, process.stderr):
                stream.close()


def fifo_case(binary):
    control_read, control_write = os.pipe()
    process = None
    try:
        with tempfile.TemporaryDirectory(prefix="forge-password-fifo-") as directory:
            path = Path(directory) / "input"
            os.mkfifo(path, mode=0o600)
            process = subprocess.Popen([str(binary), "file", str(control_read), str(path)],
                                       stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       pass_fds=(control_read,))
            os.close(control_read)
            control_read = -1
            os.close(control_write)
            control_write = -1
            try:
                process.wait(timeout=1.5)
            except subprocess.TimeoutExpired:
                raise AssertionError("Password file input blocked while opening a FIFO before regular-file validation")
            require(process.returncode == 4, "Password file input did not reject a FIFO with its typed input error")
    finally:
        if control_write >= 0:
            os.close(control_write)
        if control_read >= 0:
            os.close(control_read)
        if process is not None:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            process.stdout.close()
            process.stderr.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--gate", choices=("basic", "fifo"), default="basic")
    arguments = parser.parse_args()
    binary = Path(arguments.binary).resolve()
    if arguments.gate == "fifo":
        fifo_case(binary)
        print("Forge password file rejects FIFO without blocking")
        return
    terminal_case(binary, b"fixture-password\n", 0)
    terminal_case(binary, "cancel", 130)
    terminal_case(binary, b"\x04", 4)
    terminal_case(binary, b"\x00\n", 4)
    terminal_case(binary, b"x" * 65 + b"\n", 3)
    stdin_cancel(binary)
    print("Forge hidden password and explicit stdin cancellation passed")


if __name__ == "__main__":
    main()
