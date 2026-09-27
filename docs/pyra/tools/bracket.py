#!/usr/bin/env python3
# Multi-arm probe stats from a bench run.log whose arm list interleaves a baseline, e.g.
# SOH_BENCH_AB_VALUES=0,7,0,24,0,992 (§8.48): every probe segment has a baseline segment on each side,
# and its delta is taken against their mean, which cancels a linear drift as ABBA does for two arms.
# A probe segment is dropped if it or either neighbour left the pin (1250 MHz, or the clock given).
# usage: bracket.py run.log [baseline arm, default 0] [MHz, default 1250]
import re, sys, math
BASE = sys.argv[2] if len(sys.argv) > 2 else '0'
PIN = sys.argv[3] if len(sys.argv) > 3 else '1250'
T = {1:12.706,2:4.303,3:3.182,4:2.776,5:2.571,6:2.447,7:2.365,8:2.306,9:2.262,10:2.228,11:2.201}
seg = {}
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'BENCH (segment|oncpu) (\d+) (.*)', line)
    if not m: continue
    i = int(m.group(2)); kv = dict(re.findall(r'(\w+)=([^\s]+)', m.group(3)))
    if i in seg and m.group(1) in seg[i]: continue  # first occurrence only
    seg.setdefault(i, {})[m.group(1)] = kv
def ok(s):
    k = s['segment']
    a, b = k['cpu_mhz'].split('->')
    return all(v == PIN for v in (a, b, k['cpu_min_mhz'], k['cpu_max_mhz']))
METRICS = (('thread_us', lambda s: float(s['oncpu']['thread_us'])),
           ('period_us', lambda s: float(s['segment']['period_us'])),
           ('win_cpu_us', lambda s: float(s['oncpu']['window_us'])),
           ('swap_us', lambda s: float(s['segment']['swap_us'])))
arms = {}
for i in sorted(seg):
    s = seg[i]
    if 'segment' not in s or 'oncpu' not in s: continue
    arm = s['segment']['arm']
    if arm == BASE: continue
    lo, hi = seg.get(i - 1), seg.get(i + 1)
    if not lo or not hi or lo['segment']['arm'] != BASE or hi['segment']['arm'] != BASE:
        print(f'segment {i} (arm {arm}): not bracketed by the baseline, dropped'); continue
    bad = [j for j, x in ((i - 1, lo), (i, s), (i + 1, hi)) if not ok(x)]
    if bad:
        print(f'segment {i} (arm {arm}): segments {bad} off the pin, dropped'); continue
    d = {k: (g(s), (g(lo) + g(hi)) / 2) for k, g in METRICS}
    arms.setdefault(arm, []).append(d)
    print(f"segment {i}: arm {arm} temp {s['segment']['temp_c']}  "
          + '  '.join(f"{k} {v[0]-v[1]:+.0f}" for k, v in d.items()))
for arm, rows in arms.items():
    n = len(rows)
    print(f'-- arm {arm} against {BASE}')
    for k, _ in METRICS:
        ds = [r[k][0] - r[k][1] for r in rows]; a = sum(r[k][1] for r in rows) / n
        mu = sum(ds) / n; sd = math.sqrt(sum((x - mu) ** 2 for x in ds) / (n - 1)) if n > 1 else float('nan')
        se = sd / math.sqrt(n)
        print(f'{k:10s} n={n} base={a:.0f} delta={mu:+.0f} us ({100*mu/a:+.1f}%) t={mu/se:.1f} mde={T.get(n-1, 2.0)*se:.0f}')
