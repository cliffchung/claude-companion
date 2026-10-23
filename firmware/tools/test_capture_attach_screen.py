import os
import shutil
import signal
import sys
import tempfile
import textwrap
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capture_attach_screen as cap  # noqa: E402


class CaptureTests(unittest.TestCase):
    def test_anonymize_replaces_every_home(self):
        data = b"cd /Users/alice/x && ls /Users/alice/y"
        self.assertEqual(cap.anonymize(data, "/Users/alice"), b"cd /Users/me/x && ls /Users/me/y")
        self.assertEqual(cap.anonymize(data, ""), data)

    def test_read_until_quiet_stops_after_quiet(self):
        r, w = os.pipe()
        try:
            os.write(w, b"hello")
            t0 = time.monotonic()
            out = cap.read_until_quiet(r, quiet_s=0.2, max_s=2.0)
            self.assertEqual(out, b"hello")
            self.assertLess(time.monotonic() - t0, 1.0)
        finally:
            os.close(r)
            os.close(w)

    def test_read_until_quiet_caps_total_time(self):
        r, w = os.pipe()
        try:
            t0 = time.monotonic()
            # writes keep coming faster than quiet_s; max_s ends it
            pid = os.fork()
            if pid == 0:
                os.close(r)
                end = time.monotonic() + 1.5
                while time.monotonic() < end:
                    os.write(w, b"x")
                    time.sleep(0.05)
                os._exit(0)
            out = cap.read_until_quiet(r, quiet_s=0.5, max_s=0.6)
            took = time.monotonic() - t0
            os.waitpid(pid, 0)
            self.assertGreater(len(out), 0)
            self.assertLess(took, 1.2)
        finally:
            os.close(r)
            os.close(w)

    def _fake_claude(self, body):
        """A stand-in for `claude attach`: draws a banner, takes the tty raw (as attach does),
        records every byte it receives into $CAP_OUT, then runs body."""
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, True)
        path = os.path.join(d, "claude")
        with open(path, "w") as f:
            f.write(f"#!{sys.executable}\n" + textwrap.dedent("""\
                import os, select, signal, sys, time, tty
                sys.stdout.write("fake attach: Do you want to proceed?\\r\\n"); sys.stdout.flush()
                tty.setraw(0)
                got = b""
                end = time.monotonic() + 4
                while time.monotonic() < end and b"\\x1a" not in got:
                    r, _, _ = select.select([0], [], [], 0.1)
                    if r:
                        got += os.read(0, 64)
                time.sleep(0.3)   # anything sent after the detach is recorded too
                r, _, _ = select.select([0], [], [], 0)
                if r:
                    got += os.read(0, 64)
                open(os.environ["CAP_OUT"], "wb").write(got)
                """) + textwrap.dedent(body))
        os.chmod(path, 0o755)
        out = os.path.join(d, "received")
        os.environ["CAP_OUT"] = out
        self.addCleanup(os.environ.pop, "CAP_OUT", None)
        return path, out

    def test_detach_sends_only_ctrl_z(self):
        claude, out = self._fake_claude("sys.exit(0)\n")
        data, status = cap.capture("abc123", claude=claude)
        self.assertIn(b"Do you want to proceed?", data)
        with open(out, "rb") as f:
            self.assertEqual(f.read(), b"\x1a")
        self.assertTrue(os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0)

    def test_stopped_child_does_not_hang(self):
        claude, _ = self._fake_claude("os.kill(os.getpid(), signal.SIGSTOP)\ntime.sleep(60)\n")
        t0 = time.monotonic()
        data, status = cap.capture("abc123", claude=claude)
        self.assertLess(time.monotonic() - t0, 5.0)
        self.assertTrue(os.WIFSIGNALED(status) and os.WTERMSIG(status) == signal.SIGKILL)

    def test_failed_exec_exits_127(self):
        data, status = cap.capture("abc123", claude="/nonexistent/claude")
        self.assertTrue(os.WIFEXITED(status))
        self.assertEqual(os.WEXITSTATUS(status), cap.EXEC_FAILED)

    def test_rejects_bad_ids(self):
        self.assertEqual(cap.main(["x", "a;rm", "/dev/null"]), 2)
        self.assertEqual(cap.main(["x"]), 2)


if __name__ == "__main__":
    unittest.main()
