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


def unread_prompt_cancel(binary):
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

        # Establish unread output only after entry completed; do not accidentally test entry's drain.
        os.write(slave, b"Unread prompt: ")
        require(select.select([master], [], [], 0.5)[0], "Private PTY has no unread prompt output")
        os.write(master, b"fixture-password")
        started = time.monotonic()
        os.write(control_write, b"c")
        os.close(control_write)
        control_write = -1
        try:
            # Deliberately leave master unread: restoration must not wait for prompt output to drain.
            process.wait(timeout=1.5)
        except subprocess.TimeoutExpired:
            raise AssertionError("Password cancellation waited for unread prompt output while restoring termios")
        require(process.returncode == 130, "Unread-prompt cancellation lost its typed outcome")
        require(time.monotonic() - started < 1.5, "Unread-prompt cancellation did not settle promptly")
        require(termios.tcgetattr(slave) == original, "Unread-prompt cancellation did not restore full termios")
        require(select.select([master], [], [], 0)[0], "Password cancellation discarded unread prompt output")

        # The partial hidden line must not survive cancellation into the caller's next terminal input.
        os.write(master, b"\n")
        require(select.select([slave], [], [], 0.5)[0], "Restored terminal did not accept its next input line")
        require(os.read(slave, 64) == b"\n", "Cancelled hidden input survived terminal restoration")
        available(master, output)
        require(b"fixture-password" not in output, "Hidden password was echoed or printed")
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


def unread_entry_cancel(binary):
    master, slave = os.openpty()
    control_read, control_write = os.pipe()
    original = termios.tcgetattr(slave)
    process = None
    output = bytearray()
    try:
        os.write(slave, b"Earlier public terminal output: ")
        require(select.select([master], [], [], 0.5)[0], "Private PTY has no unread output before password entry")
        # Observe the helper prompt through a separate pipe without reading any pending PTY output.
        process = subprocess.Popen([str(binary), "terminal", str(control_read)],
                                   stdin=slave, stdout=slave, stderr=subprocess.PIPE, pass_fds=(control_read,))
        os.close(control_read)
        control_read = -1
        deadline = time.monotonic() + 5
        while b"Secret: " not in output:
            require(process.poll() is None and time.monotonic() < deadline,
                    "Password helper did not write its prompt before hidden input")
            if select.select([process.stderr], [], [], 0.02)[0]:
                output.extend(os.read(process.stderr.fileno(), 4096))
        # The helper prompt proves read_password passed its initial stop check; the PTY remains unread.
        started = time.monotonic()
        os.write(control_write, b"c")
        os.close(control_write)
        control_write = -1
        try:
            process.wait(timeout=1.5)
        except subprocess.TimeoutExpired:
            if termios.tcgetattr(slave)[3] & termios.ECHO:
                raise AssertionError("Password entry cancellation waited for unread output before hiding echo")
            raise AssertionError("Password entry cancellation waited for unread output after hiding echo")
        require(process.returncode == 130, "Unread-entry cancellation lost its typed outcome")
        require(time.monotonic() - started < 1.5, "Unread-entry cancellation did not settle promptly")
        require(termios.tcgetattr(slave) == original, "Unread-entry cancellation did not restore full termios")
        require(select.select([master], [], [], 0)[0], "Password cancellation discarded unread terminal output")
        output.extend(process.stderr.read())
        available(master, output)
        require(b"fixture-password" not in output, "Hidden password was echoed or printed")
    finally:
        if control_write >= 0:
            os.close(control_write)
        if control_read >= 0:
            os.close(control_read)
        if process is not None:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            process.stderr.close()
        os.close(master)
        os.close(slave)


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
    parser.add_argument("--gate", choices=("basic", "fifo", "unread-prompt", "unread-entry"), default="basic")
    arguments = parser.parse_args()
    binary = Path(arguments.binary).resolve()
    if arguments.gate == "fifo":
        fifo_case(binary)
        print("Forge password file rejects FIFO without blocking")
        return
    if arguments.gate == "unread-prompt":
        unread_prompt_cancel(binary)
        print("Forge hidden password cancellation restores termios without draining unread prompt output")
        return
    if arguments.gate == "unread-entry":
        unread_entry_cancel(binary)
        print("Forge hidden password entry cancels without draining earlier terminal output")
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
