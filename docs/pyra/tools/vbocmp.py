#!/usr/bin/env python3
# Diff two SOH_BENCH_DUMP vertex traces (<frame>.ppm.vbo, §8.32) draw by draw: which draws differ,
# in which float of the vertex (column), and by how much. Usage: vbocmp.py a.vbo b.vbo
import sys, struct, numpy as np
def read(path):
    data = open(path, "rb").read(); draws = []; o = 0
    while o < len(data):
        n, tris, prog, nf = struct.unpack_from("<4I", data, o); o += 16
        f = np.frombuffer(data, dtype="<f4", count=n, offset=o); o += 4 * n
        draws.append((tris, prog, nf, f))
    return draws
a, b = read(sys.argv[1]), read(sys.argv[2])
print(f"draws {len(a)} vs {len(b)}")
cols = {}
ndiff = 0
for i, (da, db) in enumerate(zip(a, b)):
    if da[:3] != db[:3] or len(da[3]) != len(db[3]):
        print(f"draw {i}: header differs {da[:3]} {len(da[3])} vs {db[:3]} {len(db[3])}"); ndiff += 1; continue
    fa, fb = da[3], db[3]
    bad = ~((fa == fb) | (np.isnan(fa) & np.isnan(fb)))
    if bad.any():
        ndiff += 1
        idx = np.nonzero(bad)[0]
        c = idx % da[2]
        for k in np.unique(c):
            m = idx[c == k]
            d = np.abs(fa[m] - fb[m])
            cols.setdefault((da[2], int(k)), []).append(float(np.nanmax(d)))
        if ndiff <= 15:
            j = idx[0]
            print(f"draw {i} prog {da[1]} nf {da[2]} tris {da[0]}: {len(idx)} floats differ, cols {sorted(set(c.tolist()))}, "
                  f"first at vertex {j // da[2]} col {j % da[2]}: {fa[j]!r} vs {fb[j]!r}")
print(f"{ndiff} of {min(len(a), len(b))} draws differ")
for (nf, k), v in sorted(cols.items()):
    print(f"  nf {nf} col {k}: {len(v)} draws, max |diff| {max(v):.6g}")
