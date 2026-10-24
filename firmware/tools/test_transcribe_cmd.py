"""Runs cc_cmd_transcribe / cc_cmd_voice_check output locally, the way sshd's login shell runs it.

The command comes from `cc_cmd_print transcribe|voicecheck <whisper> <model>`. Set CC_CMD_PRINT to
the built binary; the default is firmware/test/host/build/cc_cmd_print (build the host tests
first). It runs as `/bin/zsh -c "<command>"` on a plain pipe (no pty), as on a non-pty exec
channel. The fake whisper-cli is a /bin/sh script in a temp dir; test_real_whisper runs the real
one on speech made by `say`. Nothing is run over SSH.
"""
import os
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

PRINT = Path(os.environ.get('CC_CMD_PRINT') or
             Path(__file__).resolve().parent.parent / 'test' / 'host' / 'build' / 'cc_cmd_print')
REAL_WHISPER = '/opt/homebrew/bin/whisper-cli'            # CC_WHISPER_DEFAULT
REAL_MODEL = '~/.local/share/whisper/ggml-base.en.bin'    # CC_WHISPER_MODEL_DEFAULT

FAKE = """#!/bin/sh
D=$(dirname "$0")
for a in "$@"; do printf '%s\\n' "$a"; done > "$D/args.txt"
wc -c | tr -d ' ' > "$D/n.txt"
printf ' Hello there.\\n'
"""
KEEP = ('args.txt', 'n.txt')


def pgrep(pattern):
    r = subprocess.run(['pgrep', '-f', pattern], capture_output=True, text=True)
    return [int(p) for p in r.stdout.split()]


def tree(root):
    out = set()
    for d, _, files in os.walk(root):
        for f in files:
            if f not in KEEP:
                out.add(os.path.join(d, f))
    return out


def command(kind, whisper, model):
    r = subprocess.run([str(PRINT), kind, whisper, model], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return r.stdout.rstrip('\n')


@unittest.skipUnless(PRINT.exists(), f'{PRINT} missing: build the host tests first')
class TranscribeCmdTest(unittest.TestCase):
    def setUp(self):
        self.dir = os.path.realpath(tempfile.mkdtemp(prefix='ccv-', dir='/tmp'))  # <= 63 chars
        self.home = os.path.realpath(tempfile.mkdtemp(prefix='ccvh-', dir='/tmp'))
        self.whisper = os.path.join(self.dir, 'whisper-cli')
        self.model = os.path.join(self.dir, 'model.bin')
        Path(self.whisper).write_text(FAKE)
        os.chmod(self.whisper, 0o755)
        Path(self.model).write_bytes(b'm')
        self.before = tree(self.dir) | tree(self.home)

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)
        shutil.rmtree(self.home, ignore_errors=True)

    def run_cmd(self, kind, whisper=None, model=None, data=b'', home=None):
        cmd = command(kind, whisper or self.whisper, model or self.model)
        env = {'HOME': home or self.home, 'PATH': '/usr/bin:/bin'}
        p = subprocess.Popen(['/bin/zsh', '-c', cmd], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        try:
            p.stdin.write(data)       # BrokenPipeError here means stdin wasn't drained
            p.stdin.close()
        except BrokenPipeError:
            p.kill()
            p.wait()
            self.fail('BrokenPipeError: the command did not drain stdin')
        try:
            out, err = p.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            p.kill()
            raise
        p.wait()
        self.assertEqual(pgrep(self.dir), [], 'process left running')
        self.assertEqual(tree(self.dir) | tree(self.home), self.before, 'files left behind')
        return p.returncode, out, err

    def test_fake_args_exact(self):
        rc, _, _ = self.run_cmd('transcribe', data=b'x')
        self.assertEqual(rc, 0)
        want = f'-m\n{self.model}\n-l\nen\n-nt\n-np\n-otxt\n-of\n-\n-f\n-\n'
        self.assertEqual(Path(self.dir, 'args.txt').read_text(), want)

    def test_stdin_passes_through(self):
        rc, out, _ = self.run_cmd('transcribe', data=bytes(100044))
        self.assertEqual(rc, 0)
        self.assertEqual(Path(self.dir, 'n.txt').read_text().strip(), '100044')
        self.assertEqual(out, b' Hello there.\n')

    def test_tilde_model_path(self):
        Path(self.home, 'm.bin').write_bytes(b'm')
        self.before = tree(self.dir) | tree(self.home)
        rc, out, _ = self.run_cmd('transcribe', model='~/m.bin', data=b'x')
        self.assertEqual(rc, 0)
        self.assertEqual(out, b' Hello there.\n')
        self.assertIn(os.path.join(self.home, 'm.bin'),
                      Path(self.dir, 'args.txt').read_text())

    def test_missing_whisper_120(self):
        rc, out, _ = self.run_cmd('transcribe', whisper=os.path.join(self.dir, 'nope'),
                                  data=bytes(960044))
        self.assertEqual((rc, out), (120, b''))

    def test_missing_model_121(self):
        rc, out, _ = self.run_cmd('transcribe', model=os.path.join(self.dir, 'nope.bin'),
                                  data=bytes(960044))
        self.assertEqual((rc, out), (121, b''))

    def test_precheck_drains_stdin(self):
        # run_cmd fails on BrokenPipeError; a 4 MB payload is far past the pipe buffer
        rc, _, _ = self.run_cmd('transcribe', whisper=os.path.join(self.dir, 'nope'),
                                data=bytes(4 * 1024 * 1024))
        self.assertEqual(rc, 120)

    def test_voicecheck_codes(self):
        for kw, want in (({}, 0),
                         ({'whisper': os.path.join(self.dir, 'nope')}, 120),
                         ({'model': os.path.join(self.dir, 'nope.bin')}, 121)):
            rc, out, _ = self.run_cmd('voicecheck', **kw)
            self.assertEqual((rc, out), (want, b''))
        self.assertFalse(Path(self.dir, 'args.txt').exists(), 'voicecheck must not run whisper')

    def test_no_files_left(self):
        for kind in ('transcribe', 'voicecheck'):
            self.run_cmd(kind, data=b'x')     # run_cmd compares the file set after every run

    def test_no_process_left(self):
        self.run_cmd('transcribe', data=bytes(1000))
        self.assertEqual(pgrep(self.dir), [])


class RealWhisperTest(unittest.TestCase):
    @unittest.skipUnless(PRINT.exists(), f'{PRINT} missing: build the host tests first')
    @unittest.skipUnless(os.access(REAL_WHISPER, os.X_OK) and
                         os.access(os.path.expanduser(REAL_MODEL), os.R_OK) and
                         shutil.which('say'), 'whisper-cli, model or say not available')
    def test_real_whisper(self):
        d = os.path.realpath(tempfile.mkdtemp(prefix='ccvr-', dir='/tmp'))
        try:
            wav = os.path.join(d, 't.wav')
            subprocess.run(['say', '-o', wav, '--data-format=LEI16@16000',
                            'Please run the tests and then commit the change.'], check=True)
            cmd = command('transcribe', REAL_WHISPER, REAL_MODEL)
            t0 = time.time()
            with open(wav, 'rb') as f:
                r = subprocess.run(['/bin/zsh', '-c', cmd], stdin=f, capture_output=True,
                                   env={'HOME': os.path.expanduser('~'), 'PATH': '/usr/bin:/bin'},
                                   timeout=30)
            print(f'\nreal whisper wall time: {time.time() - t0:.2f} s')
            text = r.stdout.decode().lower()
            self.assertEqual(r.returncode, 0, r.stderr[-300:])
            self.assertIn('run the tests', text)
            self.assertIn('commit', text)
            os.unlink(wav)
            self.assertEqual(os.listdir(d), [])
            self.assertEqual(pgrep(d), [])
        finally:
            shutil.rmtree(d, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()
