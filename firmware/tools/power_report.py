#!/usr/bin/env python3
"""Read the Tab5 power log or bench over the console and report battery estimates.

usage: power_report.py --port PORT (--log | --bench) [--capacity-mwh N]
"""
import argparse
import re
import sys
import time

BENCH_FIELDS = ("name", "seconds", "avg_mw", "min_mw", "max_mw", "avg_ma", "mwh_x1000", "status")
DUMP_FIELDS = ("t_s", "mv", "ma", "mw", "tag", "flags")


DUMP_ROW = re.compile(r"^-?\d+(,-?\d+){5}$")
BENCH_ROW = re.compile(r"^[^,]+(,-?\d+){6},.+$")
F_CHARGING = 4


def is_charging(s):
    """A sample taken while charging (current flowing in, or the charge pin low) is not battery use."""
    return s["ma"] < 0 or bool(s["flags"] & F_CHARGING)


def _block(lines, marker, row_re):
    """Rows between '@@<marker> <n>' and '@@END'. Lines outside the block, and log lines that
    interleave inside it, are skipped; the row count is still enforced."""
    head = "@@" + marker + " "
    rows, count, open_ = [], None, False
    for raw in lines:
        line = raw.strip()
        if not open_:
            if line.startswith(head):
                try:
                    count = int(line[len(head):])
                except ValueError:
                    raise ValueError("bad @@%s header: %r" % (marker, line))
                open_ = True
            continue
        if line == "@@END":
            if len(rows) != count:
                raise ValueError("@@%s said %d rows, got %d" % (marker, count, len(rows)))
            return rows
        if row_re.match(line):
            rows.append(line)
    raise ValueError("missing @@END" if open_ else "no @@%s block" % marker)


def parse_dump(lines):
    out = []
    for row in _block(lines, "PLOG", DUMP_ROW):
        parts = row.split(",")
        if len(parts) != len(DUMP_FIELDS):
            raise ValueError("bad PLOG row: %r" % row)
        out.append({k: int(v) for k, v in zip(DUMP_FIELDS, parts)})
    return out


def parse_bench(lines):
    out = []
    for row in _block(lines, "BENCH", BENCH_ROW):
        parts = row.split(",", len(BENCH_FIELDS) - 1)
        if len(parts) != len(BENCH_FIELDS):
            raise ValueError("bad BENCH row: %r" % row)
        d = dict(zip(BENCH_FIELDS, parts))
        for k in BENCH_FIELDS[1:7]:
            d[k] = int(d[k])
        out.append(d)
    return out


def summarize(samples):
    acc = {}
    for s in samples:
        a = acc.setdefault(s["tag"], {"n": 0, "mw": 0, "ma": 0})
        a["n"] += 1
        a["mw"] += s["mw"]
        a["ma"] += s["ma"]
    out = {}
    for tag, a in acc.items():
        avg_mw = a["mw"] / a["n"]
        out[tag] = {"n": a["n"], "avg_mw": avg_mw, "avg_ma": a["ma"] / a["n"],
                    "mwh": a["mw"] / 3600.0}   # 1 Hz: n samples = n seconds
    return out


def estimate_hours(capacity_mwh, mix, summary):
    """Battery life for a usage mix {tag: fraction of time}, from each tag's average power."""
    mw = sum(frac * summary[tag]["avg_mw"] for tag, frac in mix.items() if tag in summary)
    return capacity_mwh / mw if mw > 0 else float("inf")


def _read_block(port, command, marker, timeout=60):
    import serial  # only the CLI needs pyserial
    with serial.Serial(port, 115200, timeout=0.5) as s:
        s.reset_input_buffer()
        s.write((command + "\r\n").encode())
        s.flush()
        lines, deadline = [], time.time() + timeout
        while time.time() < deadline:
            raw = s.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", "replace").rstrip()
            lines.append(line)
            if line.strip() == "@@END" and any(l.startswith("@@" + marker) for l in lines):
                return lines
    raise ValueError("timed out waiting for @@END")


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--log", action="store_true")
    g.add_argument("--bench", action="store_true")
    ap.add_argument("--include-charging", action="store_true", help="keep charging samples in --log")
    ap.add_argument("--capacity-mwh", type=float, default=14800)
    a = ap.parse_args(argv)
    try:
        if a.log:
            samples = parse_dump(_read_block(a.port, "power log dump", "PLOG"))
            total = len(samples)
            if not a.include_charging:
                samples = [s for s in samples if not is_charging(s)]
            summ = summarize(samples)
            print("%d samples (%d charging samples left out)" % (total, total - len(samples)))
            print("tag     n   avg_mw  avg_ma     mWh")
            for tag in sorted(summ):
                r = summ[tag]
                print("0x%02x %5d %8.0f %7.0f %7.2f" % (tag, r["n"], r["avg_mw"], r["avg_ma"], r["mwh"]))
            if summ:
                mix = {t: summ[t]["n"] / len(samples) for t in summ}
                print("estimated battery life: %.1f h (estimate)" % estimate_hours(a.capacity_mwh, mix, summ))
        else:
            rows = parse_bench(_read_block(a.port, "power bench report", "BENCH"))
            print("%-14s %5s %7s %7s %7s %7s %8s  %s" % ("step", "s", "avg_mw", "min_mw", "max_mw", "avg_ma", "mWh", "status"))
            for r in rows:
                print("%-14s %5d %7d %7d %7d %7d %8.2f  %s" % (r["name"], r["seconds"], r["avg_mw"], r["min_mw"],
                                                               r["max_mw"], r["avg_ma"], r["mwh_x1000"] / 1000.0, r["status"]))
            summ = {i: {"avg_mw": r["avg_mw"]} for i, r in enumerate(rows) if r["status"] == "ok" and r["avg_mw"] > 0}
            if summ:
                mix = {i: 1.0 / len(summ) for i in summ}
                print("estimated battery life: %.1f h (estimate)" % estimate_hours(a.capacity_mwh, mix, summ))
    except ValueError as e:
        print("power_report: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
