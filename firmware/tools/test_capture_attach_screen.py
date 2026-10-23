import os
import sys
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

    def test_detach_is_ctrl_z_only(self):
        self.assertEqual(cap.CTRL_Z, b"\x1a")

    def test_rejects_bad_ids(self):
        self.assertEqual(cap.main(["x", "a;rm", "/dev/null"]), 2)
        self.assertEqual(cap.main(["x"]), 2)


if __name__ == "__main__":
    unittest.main()
