#!/usr/bin/env python3
# Diff two SOH_BENCH_DUMP matrix traces (<frame>.ppm.mtx, §8.32): every G_MTX in order, with its
# parameters, whether frame interpolation replaced it, and which elements differ.
import sys, struct, numpy as np
def read(path):
    d = open(path, "rb").read(); n = len(d) // 72
    return [(struct.unpack_from("<2I", d, i * 72), np.frombuffer(d, "<f4", 16, i * 72 + 8).reshape(4, 4)) for i in range(n)]
a, b = read(sys.argv[1]), read(sys.argv[2])
print(f"matrices {len(a)} vs {len(b)}")
n = 0
for i, ((ha, ma), (hb, mb)) in enumerate(zip(a, b)):
    if ha != hb or not np.array_equal(ma, mb):
        n += 1
        if n <= 8:
            print(f"#{i} params {ha[0]:#x} repl {ha[1]} vs {hb[1]}; differing elements {list(zip(*np.nonzero(ma != mb)))}")
            print("  a", ma.tolist()); print("  b", mb.tolist())
print(f"{n} of {min(len(a), len(b))} matrices differ")
