#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare two ab_sample.py dumps (e.g. 120 fps vs 60 fps) and print globals whose change
rate roughly doubles: per-frame counters / accumulators. Usage: ab_compare.py hi.json lo.json"""
import json, struct, sys

def fl(i):
    return struct.unpack("<f", struct.pack("<i", i))[0]

def load(n):
    d = json.load(open(n))
    return d["dt"], {int(k, 16): v for k, v in d["res"].items()}

dt1, a = load(sys.argv[1]); dt2, b = load(sys.argv[2])
for addr in sorted(set(a) & set(b)):
    (x1, y1), (x2, y2) = a[addr], b[addr]
    di1, di2 = (y1 - x1) / dt1, (y2 - x2) / dt2
    if abs(di2) >= 20 and 1.6 < di1 / di2 < 2.4 and di1 * di2 > 0:
        print(f"{addr:08x} INT   {di2:.0f}/s -> {di1:.0f}/s")
