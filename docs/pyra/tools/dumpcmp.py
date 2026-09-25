#!/usr/bin/env python3
# Compare SOH_BENCH_DUMP frames (§8.32): the same frame of every segment, taken in-process, so both
# arms show the same frame whatever their speed. seg<k>-f<n>.ppm; the n+1 frame is a same-arm,
# next-frame control. Usage: dumpcmp.py <dir> [segA segB ...] - every segment against the first.
import sys, glob, re, numpy as np
from PIL import Image
d = sys.argv[1]
files = sorted(glob.glob(f"{d}/seg*-f*.ppm"), key=lambda p: tuple(map(int, re.findall(r"\d+", p.split("/")[-1]))))
im = {p.split("/")[-1][:-4]: np.asarray(Image.open(p).convert("RGB")).astype(int) for p in files}
names = list(im)
if len(sys.argv) > 2:
    names = [n for n in names if n.split("-")[0][3:] in sys.argv[2:]]
ref = names[0]
for n in names[1:]:
    diff = np.abs(im[ref] - im[n])
    m = diff.any(axis=2)
    ys, xs = np.nonzero(m)
    where = f"rows {ys.min()}-{ys.max()} cols {xs.min()}-{xs.max()}" if len(ys) else ""
    print(f"{ref} vs {n:12s} pixels {m.sum():7d}  mean {diff.mean():.3f}  max {diff.max():3d}  {where}")
