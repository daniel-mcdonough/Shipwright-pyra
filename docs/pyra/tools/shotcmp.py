#!/usr/bin/env python3
# In-process visual check (§8.31): compare grab.sh's six frames. A = arm 0 (segment 0), B = arm 1
# (segment 1), C = arm 0 again (segment 3, the same-arm control). Counts differing pixels overall and
# where each arm's two grabs agree. Usage: cmp.py <dir with A-1.png ... C-2.png>
import sys, numpy as np
from PIL import Image
d = sys.argv[1]
im = {k: np.asarray(Image.open(f"{d}/{k}.png").convert("RGB")).astype(int) for k in ("A-1","A-2","B-1","B-2","C-1","C-2")}
def diff(a, b): return np.any(im[a] != im[b], axis=2)
stable = ~diff("A-1","A-2") & ~diff("B-1","B-2") & ~diff("C-1","C-2")
print(f"within-arm unstable: A {diff('A-1','A-2').sum()}  B {diff('B-1','B-2').sum()}  C {diff('C-1','C-2').sum()}")
for x, y, what in (("A-1","B-1","off vs on"), ("A-1","C-1","off vs off (control)"), ("B-1","C-1","on vs off")):
    m = diff(x, y); s = m & stable
    ys, xs = np.nonzero(m)
    where = f"rows {ys.min()}-{ys.max()} cols {xs.min()}-{xs.max()}" if len(ys) else ""
    print(f"{what:22s} all {m.sum():7d}  stable-only {s.sum():7d}  max chan diff {np.abs(im[x]-im[y]).max():3d}  {where}")
