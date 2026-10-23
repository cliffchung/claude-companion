"""Runs the long-poll watch loop (cc_cmd_watch's output) locally, the way sshd runs it.

The command comes from firmware/test/host/build/cc_cmd_print (build the host tests first).
It runs as `/bin/zsh -c "<command>"` under pty.fork(), so the pty is the controlling terminal
as in a pty exec channel on the Mac. `claude` is a fake in a temp dir that prints state.json;
the real one is never run. Every test kills its process group and checks that nothing with the
temp dir in its command line is left running.
"""
import os
import pty
import select
import shutil
import signal
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

PRINT = Path(__file__).resolve().parent.parent / 'test' / 'host' / 'build' / 'cc_cmd_print'

FAKE = """#!/bin/sh
D=$(dirname "$0")
{slow}
cat "$D/state.json"
"""
# The slow fake sleeps through a binary whose path contains the temp dir, so pgrep sees it.
SLOW = '"$D/naptime" 30'


def pgrep(pattern):
    r = subprocess.run(['pgrep', '-f', pattern], capture_output=True, text=True)
    return [int(p) for p in r.stdout.split()]


@unittest.skipUnless(PRINT.exists(), f'{PRINT} missing: build the host tests first')
class WatchLoopTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix='ccw-', dir='/tmp')   # cc_path_valid: 63 chars at most
        self.dir = os.path.realpath(self.dir)
        self.pid = None
        self.fd = None
        self.buf = b''
        self.lines = []

    def tearDown(self):
        if self.pid:
            try:
                os.killpg(self.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
            self.close_master()
            self.reap(block=True)
        left = []
        for _ in range(20):
            left = pgrep(self.dir)
            if not left:
                break
            time.sleep(0.1)
        try:
            self.assertEqual([], left, f'processes left running: {left}')
        finally:
            for p in left:
                try:
                    os.kill(p, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            shutil.rmtree(self.dir, ignore_errors=True)

    # ---- helpers ----

    def fake(self, state='[1]', slow=False, where=None):
        d = Path(where or self.dir)
        d.mkdir(parents=True, exist_ok=True)
        f = d / 'claude'
        f.write_text(FAKE.format(slow=SLOW if slow else ''))
        f.chmod(0o755)
        (d / 'state.json').write_text(state)
        if slow:
            os.symlink('/bin/sleep', d / 'naptime')
        return d

    def command(self, claude, i, h, t, life):
        r = subprocess.run([str(PRINT), claude, str(i), str(h), str(t), str(life)],
                           capture_output=True, text=True)
        self.assertEqual(0, r.returncode, r.stderr)
        return r.stdout.rstrip('\n')

    def start(self, cmd, home=None):
        env = dict(os.environ)
        if home:
            env['HOME'] = home
        pid, fd = pty.fork()
        if pid == 0:
            try:
                os.execve('/bin/zsh', ['/bin/zsh', '-c', cmd], env)
            finally:
                os._exit(127)
        self.pid, self.fd = pid, fd

    def close_master(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def reap(self, block=False):
        """True once the loop's shell has exited."""
        if not self.pid:
            return True
        try:
            done, _ = os.waitpid(self.pid, 0 if block else os.WNOHANG)
        except ChildProcessError:
            return True
        return done != 0

    def keepalive(self):
        os.write(self.fd, b'\n')

    def pump(self, seconds, keepalive_every=None, until=None):
        """Reads lines for up to seconds; returns the first line matching until (or None)."""
        end = time.time() + seconds
        next_ka = time.time() if keepalive_every else None
        while True:
            now = time.time()
            if next_ka is not None and now >= next_ka:
                self.keepalive()
                next_ka = now + keepalive_every
            if now >= end:
                return None
            wait = end - now
            if next_ka is not None:
                wait = min(wait, max(0.0, next_ka - now))
            r, _, _ = select.select([self.fd], [], [], min(wait, 0.2))
            if r:
                try:
                    data = os.read(self.fd, 4096)
                except OSError:
                    data = b''
                if not data:   # the slave side closed: the loop has exited
                    return None
                self.buf += data
                while b'\n' in self.buf:
                    line, self.buf = self.buf.split(b'\n', 1)
                    line = line.decode('utf-8', 'replace').rstrip('\r')
                    self.lines.append(line)
                    if until and until(line):
                        return line
            elif self.reap():
                return None

    def wait_exit(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if self.reap():
                self.pid = None
                return True
            time.sleep(0.05)
        return False

    def begins(self):
        return [l for l in self.lines if l.startswith('@@CC-BEGIN ')]

    # ---- tests ----

    def test_change_detection(self):
        d = self.fake('[1]')
        self.start(self.command(str(d / 'claude'), 1, 3, 60, 600))
        self.assertIsNotNone(self.pump(5, 1, lambda l: l == '@@CC-END'), self.lines)
        self.assertEqual(['@@CC-BEGIN 0'], self.begins())
        self.assertIn('[1]', self.lines)
        (d / 'state.json').write_text('[2]')
        t0 = time.time()
        self.assertIsNotNone(self.pump(2, 1, lambda l: l == '@@CC-END'), self.lines)
        self.assertLess(time.time() - t0, 2.0)
        self.assertIn('[2]', self.lines)
        n = len(self.begins())
        self.assertEqual(2, n)
        self.assertIsNotNone(self.pump(3 + 1, 1, lambda l: l.startswith('@@CC-HB ')), self.lines)
        self.assertEqual(n, len(self.begins()), 'a block repeated with unchanged output')

    def test_quiet_exit(self):
        d = self.fake()
        self.start(self.command(str(d / 'claude'), 1, 60, 3, 600))
        t0 = time.time()
        line = self.pump(3 + 1 + 1, None, lambda l: l.startswith('@@CC-BYE'))
        self.assertEqual('@@CC-BYE quiet', line, self.lines)
        self.assertTrue(self.wait_exit(max(0.5, 3 + 1 + 1 - (time.time() - t0))))

    def test_keepalive_keeps_running(self):
        d = self.fake()
        self.start(self.command(str(d / 'claude'), 1, 60, 3, 600))
        self.pump(8, 1, lambda l: l.startswith('@@CC-BYE'))
        self.assertFalse([l for l in self.lines if l.startswith('@@CC-BYE')], self.lines)
        self.assertFalse(self.reap(), 'the loop exited while keepalives were arriving')

    def test_max_lifetime(self):
        d = self.fake()
        self.start(self.command(str(d / 'claude'), 1, 60, 60, 5))
        line = self.pump(5 + 1 + 1, 1, lambda l: l.startswith('@@CC-BYE'))
        self.assertEqual('@@CC-BYE life', line, self.lines)
        self.assertTrue(self.wait_exit(1))

    def test_hangup_kills_children(self):
        d = self.fake(slow=True)
        self.start(self.command(str(d / 'claude'), 1, 60, 60, 600))
        self.pump(1.5, 1)
        self.assertTrue(pgrep(str(d / 'naptime')), 'the slow fake is not running yet')
        self.close_master()
        end = time.time() + 2
        left = None
        while time.time() < end:
            self.reap()
            left = pgrep(self.dir)
            if not left:
                break
            time.sleep(0.1)
        self.assertEqual([], left, 'the shell or the fake claude survived the hangup')

    def test_suspended_loop_exits_on_resume(self):
        d = self.fake()
        self.start(self.command(str(d / 'claude'), 1, 60, 3, 600))
        self.pump(2, 1)
        os.killpg(self.pid, signal.SIGSTOP)
        time.sleep(6)   # a sleeping Mac: no keepalives arrive
        os.killpg(self.pid, signal.SIGCONT)
        line = self.pump(1 + 1, None, lambda l: l.startswith('@@CC-BYE'))
        self.assertEqual('@@CC-BYE quiet', line, self.lines)
        self.assertTrue(self.wait_exit(1))

    def test_blocked_output_still_exits(self):
        """The Tab5 vanished mid-block (Wi-Fi drop, reboot): sshd stops draining the pty, so the
        loop's write blocks. It must still exit when the keepalives stop (found on hardware)."""
        d = self.fake('[' + 'a' * 300000 + ']')
        self.start(self.command(str(d / 'claude'), 1, 60, 3, 600))
        # never read the master and never write: the pty fills and stays full
        end = time.time() + 3 + 1 + 2
        left = pgrep(self.dir)
        while time.time() < end and left:
            time.sleep(0.2)
            left = pgrep(self.dir)
        self.assertEqual([], left, 'the loop survived with its output blocked')

    def test_stdin_eof_exits(self):
        d = self.fake()
        self.start(self.command(str(d / 'claude'), 1, 60, 60, 600))
        self.assertIsNotNone(self.pump(5, 1, lambda l: l == '@@CC-END'), self.lines)
        os.write(self.fd, b'\x04')   # ^D on the pty: the reader's read sees EOF
        line = self.pump(2, None, lambda l: l.startswith('@@CC-BYE'))
        self.assertEqual('@@CC-BYE eof', line, self.lines)
        self.assertTrue(self.wait_exit(1))

    def test_tilde_path(self):
        home = Path(self.dir) / 'home'
        self.fake('[7]', where=home / 'bin')
        self.start(self.command('~/bin/claude', 1, 60, 60, 600), home=str(home))
        self.assertIsNotNone(self.pump(5, 1, lambda l: l == '@@CC-END'), self.lines)
        self.assertEqual(['@@CC-BEGIN 0'], self.begins())
        self.assertIn('[7]', self.lines)

    def test_blocked_output_plus_stdin_eof(self):
        """Output blocked (the pty is never read) and stdin EOF; T=60, so only EOF can end it."""
        d = self.fake('[' + 'a' * 300000 + ']')
        self.start(self.command(str(d / 'claude'), 1, 60, 60, 600))
        time.sleep(1.5)
        self.assertTrue(pgrep(self.dir), 'the loop is not running')
        os.write(self.fd, b'\x04')
        end = time.time() + 4
        left = pgrep(self.dir)
        while time.time() < end and left:
            time.sleep(0.1)
            left = pgrep(self.dir)
        self.assertEqual([], left, 'the loop survived EOF with its output blocked')

    def test_not_group_leader_spares_the_caller(self):
        """Run without a new session or pty, inside a scratch process group that also holds a
        sentinel: the loop's self-kill must not reach the sentinel (only sshd makes the loop a
        group leader)."""
        d = self.fake()
        cmd = self.command(str(d / 'claude'), 1, 60, 60, 600)
        sentinel = os.path.join(self.dir, 'naptime')
        os.symlink('/bin/sleep', sentinel)
        env = dict(os.environ, CMD=cmd, SENT=sentinel)
        # stdin is /dev/null, so the reader sees EOF at once and the loop kills itself
        p = subprocess.Popen(['/bin/sh', '-c', '"$SENT" 30 & /bin/zsh -c "$CMD" </dev/null; wait'],
                             env=env, start_new_session=True,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            end = time.time() + 2
            while time.time() < end and not pgrep(str(d / 'claude')):
                time.sleep(0.02)
            end = time.time() + 3
            loop = pgrep(str(d / 'claude'))
            while time.time() < end and loop:
                time.sleep(0.1)
                loop = pgrep(str(d / 'claude'))
            self.assertEqual([], loop, 'the loop did not exit on EOF')
            time.sleep(0.5)   # a stray kill would have landed by now
            self.assertTrue(pgrep(sentinel), "the loop's self-kill reached the caller's group")
        finally:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            p.wait()


if __name__ == '__main__':
    unittest.main()
