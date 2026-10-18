#!/usr/bin/env python3
# Report 12's Tier 1 table: usage tier1table.py <dir> <tag> ... (tags like t1-market-750, t1h-field-1250).
import re, math, os, sys
S = sys.argv[1]  # a directory of <tag>.txt files, e.g. docs/pyra/data/12
T = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447}

def load(tag):
    seg = {}
    for line in open(f'{S}/{tag}.txt', errors='replace'):
        m = re.search(r'BENCH (segment|oncpu|rthread) (\d+) (.*)', line)
        if not m: continue
        i = int(m.group(2)); kv = dict(re.findall(r'(\w+)=([^\s]+)', m.group(3)))
        if i in seg and m.group(1) in seg[i]: continue
        seg.setdefault(i, {})[m.group(1)] = kv
    return seg

def stats(tag, pin, armA):
    seg = load(tag)
    ok = lambda s: all(v == pin for v in s['segment']['cpu_mhz'].split('->') + [s['segment']['cpu_min_mhz'], s['segment']['cpu_max_mhz']])
    rows = {'period': [], 'total': []}; dropped = 0; temps = []
    for i in range(0, max(seg) + 1, 2):
        if i + 1 not in seg: break
        p = [seg[i], seg[i + 1]]
        if not all(ok(s) for s in p): dropped += 1; continue
        A, B = (p[0], p[1]) if p[0]['segment']['arm'] == armA else (p[1], p[0])
        for s in p: temps += [float(x) for x in s['segment']['temp_c'].split('->')]
        rows['period'].append((float(A['segment']['period_us']), float(B['segment']['period_us'])))
        tot = lambda s: float(s['oncpu']['thread_us']) + float(s.get('rthread', {}).get('gl_us', 0))
        rows['total'].append((tot(A), tot(B)))
    out = {}
    for k, v in rows.items():
        n = len(v); a = sum(x for x, _ in v) / n; b = sum(y for _, y in v) / n
        d = [y - x for x, y in v]; mu = sum(d) / n
        sd = math.sqrt(sum((x - mu) ** 2 for x in d) / (n - 1)); se = sd / math.sqrt(n)
        out[k] = (n, a, b, mu, mu / se, T.get(n - 1, 2.0) * se)
    return out, dropped, (min(temps), max(temps))

print('| Run | Clock | Fixture | Pairs (dropped) | Stock period | Shipped period | Δ | t | MDE | Stock fps → shipped fps | Render CPU Δ (main+GL) | Die °C |')
print('|---|---|---|---|---|---|---|---|---|---|---|---|')
for tag in sys.argv[2:]:
    if not os.path.exists(f'{S}/{tag}.txt'): continue
    kind, fix, mhz = tag.split('-')
    o, dr, (t0, t1) = stats(tag, mhz, 'stock')
    n, a, b, mu, t, mde = o['period']
    tn, ta, tb, tmu, tt, tmde = o['total']
    label = 'all switches' if kind == 't1' else 'three-point held'
    print(f'| {label} | {mhz} MHz | {fix} | {n} ({dr}) | {a/1000:.2f} ms | {b/1000:.2f} ms | {100*mu/a:+.1f}% | {t:.0f} | {mde:.0f} µs ({100*mde/a:.1f}%) | '
          f'{1e6/a:.1f} → {1e6/b:.1f} | {100*tmu/ta:+.1f}% (t {tt:.0f}) | {t0:.0f}-{t1:.0f} |')
