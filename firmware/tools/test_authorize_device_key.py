"""Runs authorize_device_key.sh with HOME in a temp dir: the real ~/.ssh is never touched.

Checks the optional --from list (no from= without it, and a hint that recommends one), the refusals of
a --from that could break out of the quoted option, and the script's other safety checks.
"""
import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / 'authorize_device_key.sh'
KEY = 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIPreviewOnlyNotARealKey0123456789abcdefgh claude-companion@tab5'
BLOB = KEY.split()[1]


class AuthorizeDeviceKeyTest(unittest.TestCase):
    def setUp(self):
        self.home = tempfile.mkdtemp(prefix='cc-auth-')
        self.auth = Path(self.home) / '.ssh' / 'authorized_keys'

    def tearDown(self):
        for root, dirs, files in os.walk(self.home, topdown=False):
            for f in files:
                os.unlink(os.path.join(root, f))
            for d in dirs:
                os.rmdir(os.path.join(root, d))
        os.rmdir(self.home)

    def run_script(self, *args):
        env = dict(os.environ, HOME=self.home)
        return subprocess.run(['sh', str(SCRIPT), *args], env=env, capture_output=True, text=True)

    def lines(self):
        return self.auth.read_text().splitlines() if self.auth.exists() else []

    def test_no_from_writes_no_restriction_and_hints(self):
        r = self.run_script(KEY)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.lines(), [f'ssh-ed25519 {BLOB} claude-companion'])
        self.assertIn('--from 192.168.1.0/24,100.64.0.0/10', r.stderr)
        self.assertEqual(stat.S_IMODE(self.auth.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(self.auth.parent.stat().st_mode), 0o700)

    def test_from_list_is_written(self):
        r = self.run_script('--from', '192.168.1.0/24,100.64.0.0/10', KEY)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.lines(), [f'from="192.168.1.0/24,100.64.0.0/10" ssh-ed25519 {BLOB} claude-companion'])
        self.assertNotIn('hint', r.stderr)

    def test_from_single_network(self):
        r = self.run_script('--from', '192.168.1.0/24', KEY)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.lines(), [f'from="192.168.1.0/24" ssh-ed25519 {BLOB} claude-companion'])

    def test_from_that_could_break_out_is_refused(self):
        for bad in ['192.168.1.0/24" command="sh', '192.168.1.0/24 ,10.0.0.0/8', '10.0.0.0/8\n1.2.3.4',
                    "10.0.0.0/8'", '10.0.0.0/8;', '$(id)', '', ',10.0.0.0/8', '10.0.0.0/8,', '10.0.0.0/8,,1.2.3.4']:
            with self.subTest(bad=bad):
                r = self.run_script('--from', bad, KEY)
                self.assertNotEqual(r.returncode, 0)
                self.assertEqual(self.lines(), [])

    def test_from_without_a_value_is_a_usage_error(self):
        r = self.run_script('--from')
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('usage', r.stderr)
        self.assertEqual(self.lines(), [])

    def test_duplicate_is_refused(self):
        self.assertEqual(self.run_script(KEY).returncode, 0)
        before = self.auth.read_text()
        r = self.run_script('--from', '192.168.1.0/24', KEY)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('already', r.stderr)
        self.assertEqual(self.auth.read_text(), before)

    def test_other_checks_stay(self):
        for bad in [f'command="sh" {KEY}', 'ssh-ed25519', f'ssh-ed25519 not*base64 x', f'{KEY}\nssh-rsa AAAA x',
                    'ssh-dss AAAAB3 x']:
            with self.subTest(bad=bad):
                r = self.run_script(bad)
                self.assertNotEqual(r.returncode, 0)
                self.assertEqual(self.lines(), [])
        self.assertNotEqual(self.run_script().returncode, 0)
        self.assertNotEqual(self.run_script(KEY, KEY).returncode, 0)

    def test_appends_on_a_fresh_line(self):
        self.auth.parent.mkdir(mode=0o700)
        self.auth.write_text('ssh-ed25519 AAAAexisting other@host')   # no trailing newline
        r = self.run_script('--from', '192.168.1.0/24', KEY)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.lines(), ['ssh-ed25519 AAAAexisting other@host',
                                        f'from="192.168.1.0/24" ssh-ed25519 {BLOB} claude-companion'])


if __name__ == '__main__':
    unittest.main()
