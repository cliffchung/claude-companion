#!/usr/bin/env python3
"""Capture what `claude attach <id>` draws, for the host screen tests (R19).

Usage: capture_attach_screen.py <short-id> <out.bin>

Runs on the Mac. It spawns `~/.local/bin/claude attach <id>` under a pty at
120x40 with TERM=xterm-256color, reads until the output has been quiet for
1.0 s (at most 5 s), then detaches with Ctrl+Z only (never Left or a bare Esc,
which a dialog would take as an answer). The raw bytes are written with $HOME
replaced by /Users/me. Use it only on a session you started yourself.
"""
import fcntl
import os
import pty
import re
import select
import signal
import struct
import sys
import termios
import time

CLAUDE = os.path.expanduser("~/.local/bin/claude")
COLS, ROWS = 120, 40
QUIET_S = 1.0
MAX_S = 5.0
CTRL_Z = b"\x1a"
ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")


def anonymize(data, home):
    """Replace every occurrence of home with /Users/me."""
    if not home:
        return data
    return data.replace(home.encode(), b"/Users/me")


def read_until_quiet(fd, quiet_s=QUIET_S, max_s=MAX_S, clock=time.monotonic):
    """Read fd until nothing arrives for quiet_s, or max_s in all."""
    out = bytearray()
    start = clock()
    last = start
    while True:
        now = clock()
        if now - start >= max_s or now - last >= quiet_s:
            return bytes(out)
        wait = min(quiet_s - (now - last), max_s - (now - start))
        r, _, _ = select.select([fd], [], [], max(wait, 0))
        if not r:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            return bytes(out)
        if not chunk:
            return bytes(out)
        out += chunk
        last = clock()


EXEC_FAILED = 127


def _reap(pid, wait_s=3.0):
    """Wait up to wait_s for the child to exit; SIGKILL it otherwise, or at once if it
    stopped (a Ctrl+Z that reached the line discipline as SIGTSTP). Returns its wait status."""
    deadline = time.monotonic() + wait_s
    while time.monotonic() < deadline:
        done, status = os.waitpid(pid, os.WNOHANG | os.WUNTRACED)
        if done and os.WIFSTOPPED(status):
            break
        if done:
            return status
        time.sleep(0.1)
    os.kill(pid, signal.SIGKILL)   # also ends a stopped process
    _, status = os.waitpid(pid, 0)
    return status


def capture(short_id, claude=CLAUDE):
    """Returns (bytes drawn before the detach, the child's wait status)."""
    pid, fd = pty.fork()
    if pid == 0:
        try:
            env = dict(os.environ, TERM="xterm-256color", COLUMNS=str(COLS), LINES=str(ROWS))
            os.execve(claude, [claude, "attach", short_id], env)
        except BaseException:
            pass
        os._exit(EXEC_FAILED)   # never fall through into the parent's code
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
    data = read_until_quiet(fd)
    try:
        os.write(fd, CTRL_Z)   # the only byte ever sent
        read_until_quiet(fd, quiet_s=0.5, max_s=2.0)   # drain the detach; not saved
    except OSError:
        pass
    status = _reap(pid)
    os.close(fd)
    return data, status


def main(argv):
    if len(argv) != 3 or not ID_RE.match(argv[1]):
        sys.stderr.write("usage: capture_attach_screen.py <short-id> <out.bin>\n")
        return 2
    raw, status = capture(argv[1])
    if os.WIFEXITED(status) and os.WEXITSTATUS(status) == EXEC_FAILED:
        sys.stderr.write(f"could not run {CLAUDE}\n")
        return 1
    data = anonymize(raw, os.environ.get("HOME", ""))
    with open(argv[2], "wb") as f:
        f.write(data)
    print(f"{len(data)} bytes -> {argv[2]}")
    return 0 if data else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
