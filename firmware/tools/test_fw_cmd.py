"""Runs cc_cmd_fw_info / cc_cmd_fw_read output locally, the way sshd runs a non-pty exec (Plan 6 Task 2).

The commands come from `cc_cmd_print fwinfo|fwread <path>` (CC_CMD_PRINT, default
firmware/test/host/build/cc_cmd_print). Each runs as `<shell> -c "<command>"` on plain pipes, for /bin/sh and
/bin/zsh, with a temp HOME and PATH=/usr/bin:/bin. Nothing is run over SSH and nothing is written outside the temp
dirs. The real-image tests run `fw_print check` (FW_PRINT, default firmware/test/host/build/fw_print) on
firmware/build/claude_companion.bin when it exists, and on scratch copies of it with one fault each.
"""
import hashlib
import os
import random
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HOST_BUILD = Path(__file__).resolve().parent.parent / 'test' / 'host' / 'build'
PRINT = Path(os.environ.get('CC_CMD_PRINT') or HOST_BUILD / 'cc_cmd_print')
FW_PRINT = Path(os.environ.get('FW_PRINT') or HOST_BUILD / 'fw_print')
REAL_BIN = Path(__file__).resolve().parent.parent / 'build' / 'claude_companion.bin'
SHELLS = [s for s in ('/bin/sh', '/bin/zsh') if os.path.exists(s)]
HDR = 288


def command(kind, path):
    r = subprocess.run([str(PRINT), kind, path], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return r.stdout.rstrip('\n')


def pgrep(pattern):
    r = subprocess.run(['pgrep', '-f', pattern], capture_output=True, text=True)
    return [int(p) for p in r.stdout.split()]


def tree(root):
    out = set()
    for d, dirs, files in os.walk(root):
        for f in files + dirs:
            out.add(os.path.join(d, f))
    return out


def walk_image(data):
    """Python's own walk of an ESP image: (hashed_len, checksum_ok, hash_ok, ends_exactly)."""
    segs = data[1]
    off = 24
    ck = 0xEF
    for _ in range(segs):
        n = int.from_bytes(data[off + 4:off + 8], 'little')
        off += 8
        for b in data[off:off + n]:
            ck ^= b
        off += n
    padded = (off + 1 + 15) & ~15
    stored = data[padded - 1]
    hashed = hashlib.sha256(data[:padded]).digest()
    return padded, ck == stored, hashed == data[padded:padded + 32], padded + 32 == len(data)


@unittest.skipUnless(PRINT.exists(), f'{PRINT} missing: build the host tests first')
class FwCmdTest(unittest.TestCase):
    def setUp(self):
        self.dir = os.path.realpath(tempfile.mkdtemp(prefix='ccf-', dir='/tmp'))   # <= 63 chars with a name
        self.home = os.path.realpath(tempfile.mkdtemp(prefix='ccfh-', dir='/tmp'))
        self.addCleanup(self.cleanup)
        self.before = (tree(self.dir), tree(self.home))

    def cleanup(self):
        for d in (self.dir, self.home):
            for root, dirs, _ in os.walk(d):
                for x in dirs:
                    os.chmod(os.path.join(root, x), 0o755)
            shutil.rmtree(d, ignore_errors=True)

    def file(self, name, data):
        p = os.path.join(self.dir, name)
        Path(p).write_bytes(data)
        self.before = (tree(self.dir), tree(self.home))
        return p

    def run_cmd(self, shell, kind, path):
        cmd = command(kind, path)
        r = subprocess.run([shell, '-c', cmd], env={'HOME': self.home, 'PATH': '/usr/bin:/bin'},
                           capture_output=True, timeout=30)
        return r

    def assert_nothing_left(self):
        self.assertEqual((tree(self.dir), tree(self.home)), self.before)
        self.assertEqual(pgrep(self.dir), [])
        self.assertEqual(pgrep(self.home), [])

    def check_info(self, out, data):
        line, _, rest = out.partition(b'\n')
        self.assertEqual(line.decode(), f'{len(data)} {hashlib.sha256(data).hexdigest()}')
        self.assertEqual(rest, data[:HDR])

    def test_info_line_and_header(self):
        data = random.randbytes(1_000_000)
        p = self.file('fw.bin', data)
        for sh in SHELLS:
            r = self.run_cmd(sh, 'fwinfo', p)
            self.assertEqual(r.returncode, 0, (sh, r.stderr))
            self.check_info(r.stdout, data)
        self.assert_nothing_left()

    def test_info_small_file(self):
        data = random.randbytes(100)
        p = self.file('fw.bin', data)
        for sh in SHELLS:
            r = self.run_cmd(sh, 'fwinfo', p)
            self.assertEqual(r.returncode, 0, sh)
            self.check_info(r.stdout, data)
            self.assertEqual(len(r.stdout.partition(b'\n')[2]), 100)
        self.assert_nothing_left()

    def test_read_exact(self):
        data = random.randbytes(8 * 1024 * 1024)
        p = self.file('fw.bin', data)
        for sh in SHELLS:
            r = self.run_cmd(sh, 'fwread', p)
            self.assertEqual(r.returncode, 0, sh)
            self.assertEqual(r.stdout, data)
        self.assert_nothing_left()

    def test_tilde_path(self):
        data = random.randbytes(5000)
        Path(self.home, 'fw.bin').write_bytes(data)
        self.before = (tree(self.dir), tree(self.home))
        for sh in SHELLS:
            r = self.run_cmd(sh, 'fwinfo', '~/fw.bin')
            self.assertEqual(r.returncode, 0, (sh, r.stderr))
            self.check_info(r.stdout, data)
            r = self.run_cmd(sh, 'fwread', '~/fw.bin')
            self.assertEqual(r.returncode, 0, sh)
            self.assertEqual(r.stdout, data)
        self.assert_nothing_left()

    def test_missing_dir_unreadable_120(self):
        os.mkdir(os.path.join(self.dir, 'adir'))
        locked = self.file('locked.bin', b'x' * 1000)
        os.chmod(locked, 0o000)
        self.before = (tree(self.dir), tree(self.home))
        paths = [os.path.join(self.dir, 'missing.bin'), os.path.join(self.dir, 'adir')]
        if os.geteuid() != 0:   # root reads a mode 000 file
            paths.append(locked)
        for sh in SHELLS:
            for kind in ('fwinfo', 'fwread'):
                for p in paths:
                    r = self.run_cmd(sh, kind, p)
                    self.assertEqual(r.returncode, 120, (sh, kind, p))
                    self.assertEqual(r.stdout, b'', (sh, kind, p))
        os.chmod(locked, 0o600)
        self.assert_nothing_left()

    def test_no_files_left(self):
        p = self.file('fw.bin', random.randbytes(300_000))
        for sh in SHELLS:
            for kind in ('fwinfo', 'fwread'):
                self.run_cmd(sh, kind, p)
                self.run_cmd(sh, kind, os.path.join(self.dir, 'nope.bin'))
                self.assertEqual((tree(self.dir), tree(self.home)), self.before)

    def test_no_process_left(self):
        p = self.file('fw.bin', random.randbytes(4 * 1024 * 1024))
        for sh in SHELLS:
            # a reader that stops after 64 KB (as a cancelled read): cat gets SIGPIPE and ends
            proc = subprocess.Popen([sh, '-c', command('fwread', p)], env={'HOME': self.home, 'PATH': '/usr/bin:/bin'},
                                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            proc.stdout.read(65536)
            proc.stdout.close()
            proc.wait(timeout=10)
            self.run_cmd(sh, 'fwinfo', p)
        self.assertEqual(pgrep(self.dir), [])
        self.assert_nothing_left()


@unittest.skipUnless(FW_PRINT.exists() and REAL_BIN.exists(), 'needs fw_print and firmware/build/claude_companion.bin')
class RealImageTest(unittest.TestCase):
    def setUp(self):
        self.dir = os.path.realpath(tempfile.mkdtemp(prefix='ccr-', dir='/tmp'))
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.data = REAL_BIN.read_bytes()

    def check(self, path):
        r = subprocess.run([str(FW_PRINT), 'check', path], capture_output=True, text=True)
        lines = dict(l.split(' ', 1) for l in r.stdout.splitlines() if ' ' in l)
        return r.returncode, lines

    def copy(self, name, data):
        p = os.path.join(self.dir, name)
        Path(p).write_bytes(data)
        return p

    def test_real_image_ok(self):
        rc, lines = self.check(str(REAL_BIN))
        self.assertEqual(rc, 0, lines)
        self.assertEqual(lines['header'], 'FW_HDR_OK')
        self.assertEqual(lines['layout'], 'FW_LAYOUT_OK')
        self.assertEqual(lines['project'], 'claude_companion')
        self.assertEqual(lines['hash'], 'ok')
        hashed, ck_ok, hash_ok, exact = walk_image(self.data)
        self.assertTrue(ck_ok and hash_ok and exact)
        self.assertEqual(int(lines['hashed_len']), hashed)
        version = self.data[48:80].split(b'\0', 1)[0].decode()
        self.assertEqual(lines['version'], version)

    def test_real_image_refusals(self):
        d = self.data
        mid = len(d) // 2
        flipped = bytearray(d)
        flipped[mid] ^= 0x01
        rc, lines = self.check(self.copy('flip.bin', bytes(flipped)))
        self.assertEqual(rc, 1)
        self.assertTrue(lines['layout'] != 'FW_LAYOUT_OK' or lines['hash'] == 'bad', lines)

        rc, lines = self.check(self.copy('cut.bin', d[:-1000]))
        self.assertEqual(rc, 1)
        self.assertEqual(lines['layout'], 'FW_LAYOUT_TRUNCATED')

        proj = bytearray(d)
        proj[80] = ord('X')
        rc, lines = self.check(self.copy('proj.bin', bytes(proj)))
        self.assertEqual(rc, 1)
        self.assertEqual(lines['header'], 'FW_HDR_PROJECT')

        rc, lines = self.check(self.copy('big.bin', d + b'\0' * (9_000_000 - len(d))))
        self.assertEqual(rc, 1)
        self.assertEqual(lines['header'], 'FW_HDR_TOO_BIG')

        rc, lines = self.check(self.copy('text.bin', b'Not firmware at all.\n' * 4000))
        self.assertEqual(rc, 1)
        self.assertEqual(lines['header'], 'FW_HDR_MAGIC')

        for name in ('flip.bin', 'cut.bin', 'proj.bin', 'big.bin', 'text.bin'):
            os.unlink(os.path.join(self.dir, name))
        self.assertEqual(os.listdir(self.dir), [])


if __name__ == '__main__':
    unittest.main()
