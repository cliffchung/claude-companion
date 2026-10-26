"""stage_update.sh (Plan 6 Task 2, spec §9.4) with STAGE_DIR in a temp dir.

The images come from `fw_print make-test-image` (FW_PRINT, default firmware/test/host/build/fw_print): Task 1's
synthetic image with a real appended SHA-256. Nothing is written outside the temp dirs.
"""
import hashlib
import os
import shutil
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
SCRIPT = TOOLS / 'stage_update.sh'
FW_PRINT = Path(os.environ.get('FW_PRINT') or TOOLS.parent / 'test' / 'host' / 'build' / 'fw_print')


@unittest.skipUnless(FW_PRINT.exists(), f'{FW_PRINT} missing: build the host tests first')
class StageUpdateTest(unittest.TestCase):
    def setUp(self):
        self.src = os.path.realpath(tempfile.mkdtemp(prefix='ccs-'))
        self.stage = os.path.join(os.path.realpath(tempfile.mkdtemp(prefix='ccst-')), 'stage')
        self.home = os.path.realpath(tempfile.mkdtemp(prefix='ccsh-'))
        for d in (self.src, os.path.dirname(self.stage), self.home):
            self.addCleanup(shutil.rmtree, d, True)
        self.img = os.path.join(self.src, 'claude_companion.bin')
        subprocess.run([str(FW_PRINT), 'make-test-image', self.img], check=True)

    def stage_it(self, path):
        env = {'PATH': '/usr/bin:/bin', 'HOME': self.home, 'STAGE_DIR': self.stage}
        return subprocess.run(['/bin/sh', str(SCRIPT), path], env=env, capture_output=True, text=True, timeout=30)

    def staged(self):
        return os.path.join(self.stage, 'claude_companion.bin')

    def test_stages_byte_equal(self):
        r = self.stage_it(self.img)
        self.assertEqual(r.returncode, 0, r.stderr)
        data = Path(self.img).read_bytes()
        self.assertEqual(Path(self.staged()).read_bytes(), data)
        self.assertEqual(sorted(os.listdir(self.stage)), ['claude_companion.bin'])   # no .tmp left
        h = hashlib.sha256(data).hexdigest()
        self.assertEqual(r.stdout, f'staged 1.6.0+abc, {len(data)} B, sha256 {h[:12]}…\n')
        self.assertEqual(os.listdir(self.home), [])   # nothing under HOME with STAGE_DIR set

    def test_version_from_descriptor(self):
        data = bytearray(Path(self.img).read_bytes())
        v = b'1.6.1+3f2a9c1'
        data[48:80] = v + b'\0' * (32 - len(v))
        p = os.path.join(self.src, 'v.bin')
        Path(p).write_bytes(bytes(data))
        r = self.stage_it(p)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(r.stdout.startswith('staged 1.6.1+3f2a9c1, '), r.stdout)

    def test_text_file_refused(self):
        p = os.path.join(self.src, 'notes.txt')
        Path(p).write_text('not firmware\n' * 100)
        r = self.stage_it(p)
        self.assertEqual(r.returncode, 1)
        self.assertTrue(r.stderr.startswith('stage_update: '), r.stderr)
        self.assertFalse(os.path.exists(self.stage) and os.listdir(self.stage))

    def test_other_images_refused(self):
        data = Path(self.img).read_bytes()
        for name, bad in (('nodesc.bin', data[:32] + b'\0\0\0\0' + data[36:]),          # the bootloader's shape
                          ('other.bin', data[:80] + b'other_project\0\0\0\0' + data[97:]),
                          ('noversion.bin', data[:48] + b'\0' * 32 + data[80:])):
            p = os.path.join(self.src, name)
            Path(p).write_bytes(bad)
            r = self.stage_it(p)
            self.assertEqual(r.returncode, 1, name)
            self.assertFalse(os.path.exists(self.staged()), name)

    def test_missing_file_refused(self):
        r = self.stage_it(os.path.join(self.src, 'nope.bin'))
        self.assertEqual(r.returncode, 1)
        self.assertIn('stage_update: ', r.stderr)
        self.assertFalse(os.path.exists(self.staged()))

    def test_replaced_atomically(self):
        a = Path(self.img).read_bytes()
        # a bigger second build (the script checks only the header), so a partial file would be easy to see
        pbig = os.path.join(self.src, 'big.bin')
        Path(pbig).write_bytes(a + b'\x5a' * 4_000_000)
        self.assertEqual(self.stage_it(self.img).returncode, 0)
        ino_a = os.stat(self.staged()).st_ino
        held = open(self.staged(), 'rb')
        self.addCleanup(held.close)

        seen, stop = [], threading.Event()

        def reader():
            while not stop.is_set():
                try:
                    seen.append(Path(self.staged()).read_bytes())
                except FileNotFoundError:
                    seen.append(None)

        t = threading.Thread(target=reader)
        t.start()
        r = self.stage_it(pbig)
        stop.set()
        t.join()
        self.assertEqual(r.returncode, 0, r.stderr)
        want_big = Path(pbig).read_bytes()
        self.assertNotEqual(os.stat(self.staged()).st_ino, ino_a)
        self.assertEqual(held.read(), a)                       # the old file stays whole for a reader holding it
        self.assertTrue(seen)
        for s in seen:
            self.assertIn(s, (a, want_big))                   # never missing, never partial
        self.assertEqual(Path(self.staged()).read_bytes(), want_big)
        self.assertEqual(sorted(os.listdir(self.stage)), ['claude_companion.bin'])


if __name__ == '__main__':
    unittest.main()
