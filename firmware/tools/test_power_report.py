import unittest
import power_report as pr


class ParseTests(unittest.TestCase):
    def test_parse_dump_ignores_noise(self):
        lines = ["I (100) app: boot", "@@PLOG 2", "10,7400,-412,3140,0,2",
                 "11,7390,-400,3100,0,3", "@@END", "I (200) x: later"]
        rows = pr.parse_dump(lines)
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0], {"t_s": 10, "mv": 7400, "ma": -412, "mw": 3140, "tag": 0, "flags": 2})
        self.assertIsInstance(rows[1]["mw"], int)

    def test_parse_dump_count_mismatch(self):
        with self.assertRaises(ValueError):
            pr.parse_dump(["@@PLOG 3", "1,2,3,4,0,2", "2,2,3,4,0,2", "@@END"])

    def test_parse_dump_skips_interleaved_log_lines(self):
        lines = ["@@PLOG 2", "10,7400,-412,3140,0,2", "I (5) wifi: scan done", "11,7390,-400,3100,0,3", "@@END"]
        self.assertEqual(len(pr.parse_dump(lines)), 2)
        with self.assertRaises(ValueError):   # a skipped line can't hide a missing row
            pr.parse_dump(["@@PLOG 2", "10,7400,-412,3140,0,2", "I (5) wifi: scan done", "@@END"])

    def test_parse_bench_skips_interleaved_log_lines(self):
        rows = pr.parse_bench(["@@BENCH 1", "I (9) x: noise, with, commas", "settle,60,1,1,1,1,1,usb", "@@END"])
        self.assertEqual(rows[0]["status"], "usb")

    def test_parse_dump_missing_end(self):
        with self.assertRaises(ValueError):
            pr.parse_dump(["@@PLOG 1", "1,2,3,4,0,2"])

    def test_parse_bench(self):
        rows = pr.parse_bench(["noise", "@@BENCH 2", "settle,60,1000,900,1100,50,16667,ok",
                               "active,120,0,0,0,0,0,skipped: not built", "@@END"])
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0]["avg_mw"], 1000)
        self.assertEqual(rows[1]["status"], "skipped: not built")

    def test_parse_bench_count_mismatch(self):
        with self.assertRaises(ValueError):
            pr.parse_bench(["@@BENCH 2", "settle,60,1,1,1,1,1,ok", "@@END"])


class MathTests(unittest.TestCase):
    def test_summarize_per_tag(self):
        s = [dict(t_s=1, mv=0, ma=100, mw=1000, tag=0, flags=2),
             dict(t_s=2, mv=0, ma=300, mw=3000, tag=0, flags=2),
             dict(t_s=3, mv=0, ma=50, mw=500, tag=2, flags=2)]
        out = pr.summarize(s)
        self.assertEqual(out[0]["n"], 2)
        self.assertEqual(out[0]["avg_mw"], 2000)
        self.assertEqual(out[0]["avg_ma"], 200)
        self.assertEqual(out[2]["avg_mw"], 500)
        self.assertEqual(out[0]["mwh"], 2000 * 2 / 3600)

    def test_charging_samples_are_flagged(self):
        self.assertTrue(pr.is_charging(dict(ma=-5, flags=2)))
        self.assertTrue(pr.is_charging(dict(ma=5, flags=7)))
        self.assertFalse(pr.is_charging(dict(ma=5, flags=2)))

    def test_estimate_skips_tags_missing_from_summary(self):
        self.assertAlmostEqual(pr.estimate_hours(1000, {0: 0.5, 9: 0.5}, {0: {"avg_mw": 500}}), 4.0)

    def test_estimate_hours(self):
        summary = {0: {"avg_mw": 3000}, 2: {"avg_mw": 1000}}
        self.assertAlmostEqual(pr.estimate_hours(14800, {0: 0.1, 2: 0.9}, summary), 12.33, places=2)


if __name__ == "__main__":
    unittest.main()
