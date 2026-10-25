"""check_image_size.py (Plan 6 Task 3, spec §6.5): the headroom maths and the warning, against fixture tables."""
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
SCRIPT = TOOLS / 'check_image_size.py'
FIRMWARE = TOOLS.parent

TWO_SLOTS = """# Name, Type, SubType, Offset, Size, Flags
nvs,      data, nvs,     0x9000,   0x6000,
otadata,  data, ota,     0xF000,   0x2000,
phy_init, data, phy,     0x11000,  0x1000,
app0,     app,  ota_0,   0x20000,  0x7F0000,
app1,     app,  ota_1,   0x810000, 0x7F0000,
"""
ONE_SLOT = """nvs,      data, nvs,     0x9000,   0x6000,
otadata,  data, ota,     0xF000,   0x2000,
main_app, app,  ota_0,   0x20000,  0xF60000,
"""


class CheckImageSizeTest(unittest.TestCase):
    def setUp(self):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        self.dir = d.name

    def run_it(self, csv, size):
        c = os.path.join(self.dir, 'p.csv')
        Path(c).write_text(csv)
        b = os.path.join(self.dir, 'x.bin')
        with open(b, 'wb') as f:
            f.truncate(size)
        return subprocess.run([sys.executable, str(SCRIPT), c, b], capture_output=True, text=True)

    def test_expected_image(self):
        r = self.run_it(TWO_SLOTS, 6_374_957)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), 'image 6374957 B, slot 8323072 B, headroom 1948115 B (23 %)')
        self.assertNotIn('WARNING', r.stdout + r.stderr)

    def test_warning_under_1_mb(self):
        r = self.run_it(TWO_SLOTS, 7_500_000)
        self.assertEqual(r.returncode, 0)
        self.assertIn('WARNING: under 1 MB of headroom', r.stdout + r.stderr)
        r = self.run_it(TWO_SLOTS, 8_323_072 - 1_048_576)
        self.assertNotIn('WARNING', r.stdout + r.stderr)
        r = self.run_it(TWO_SLOTS, 8_323_072 - 1_048_575)
        self.assertIn('WARNING', r.stdout + r.stderr)

    def test_does_not_fit(self):
        r = self.run_it(TWO_SLOTS, 8_323_073)
        self.assertEqual(r.returncode, 1)
        r = self.run_it(TWO_SLOTS, 8_323_072)
        self.assertEqual(r.returncode, 0)

    def test_one_slot_table(self):
        r = self.run_it(ONE_SLOT, 7_415_488)
        self.assertEqual(r.returncode, 0)
        self.assertIn('slot 16121856 B', r.stdout)

    def test_slot_size_matches_fw_rules(self):
        """FW_SLOT_SIZE in fw_rules.h equals both app slots in the real partitions.csv."""
        h = (FIRMWARE / 'main' / 'ota' / 'fw_rules.h').read_text()
        slot = int(re.search(r'#define FW_SLOT_SIZE\s+(0x[0-9A-Fa-f]+)u', h).group(1), 16)
        sizes = []
        for line in (FIRMWARE / 'partitions.csv').read_text().splitlines():
            cols = [c.strip() for c in line.split('#', 1)[0].split(',')]
            if len(cols) >= 5 and cols[1] == 'app':
                sizes.append(int(cols[4], 0))
        self.assertEqual(sizes, [slot, slot])


if __name__ == '__main__':
    unittest.main()
