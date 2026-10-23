import base64
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from screenshot import decode_frame


def make_frame(data, w=1280, h=720, declared=None):
    b64 = base64.b64encode(data).decode()
    lines = [b64[i:i + 76] for i in range(0, len(b64), 76)]
    n = len(data) if declared is None else declared
    return ["@@SHOT jpeg %d %d %d" % (n, w, h)] + lines + ["@@END"]


class DecodeFrameTest(unittest.TestCase):
    def test_decode_frame_ignores_noise(self):
        data = b"\xff\xd8hello\xff\xd9"
        lines = ["I (12) wifi: noise", "tab5> screenshot"] + make_frame(data) + ["I (99) x: after"]
        self.assertEqual(decode_frame(lines), (data, 1280, 720))

    def test_missing_end_raises(self):
        lines = make_frame(b"\xff\xd8hello\xff\xd9")[:-1]
        with self.assertRaises(ValueError):
            decode_frame(lines)

    def test_length_mismatch_raises(self):
        with self.assertRaises(ValueError):
            decode_frame(make_frame(b"\xff\xd8hello\xff\xd9", declared=99))


if __name__ == "__main__":
    unittest.main()
