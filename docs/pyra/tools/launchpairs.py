#!/usr/bin/env python3
# Report 12 Tiers 2 and 3: pairs launches of two builds (benchlite.cpp's BENCHL lines).
# usage: launchpairs.py <dir> <tag> <labelA> <labelB> [MHz]
# Reads <dir>/<tag>-<n>-<label>.out (bench.sh's output for launch n). Launches 2k and 2k+1 are a pair
# (the driver runs them ABBA). A launch's value is the mean of its segments; a pair is dropped if any
# segment of either launch left the pin. Prints per-pair rows, then period, process CPU and the share of
# frames over 40 ms (for a capped run) with t and MDE.
import glob, math, re, sys
d, tag, la, lb = sys.argv[1:5]
pin = sys.argv[5] if len(sys.argv) > 5 else '750'
T = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365}

def launch(n):
    hits = glob.glob(f'{d}/{tag}-{n}-*.out')
    if not hits:
        return None
    segs = [dict(re.findall(r'(\w+)=([^\s]+)', l)) for l in open(hits[0], errors='replace') if l.startswith('BENCHL segment')]
    if not segs:
        return None
    ok = all(all(v == pin for v in s['cpu_mhz'].split('->') + [s['cpu_min_mhz'], s['cpu_max_mhz']]) for s in segs)
    f = lambda k: sum(float(s[k]) for s in segs) / len(segs)
    frames = sum(int(s['frames']) for s in segs)
    return dict(label=segs[0]['label'], ok=ok, period=f('period_us'), proc=f('proc_us'), thread=f('thread_us'),
                over40=100.0 * sum(int(s['over40']) for s in segs) / frames,
                temp=f"{segs[0]['temp_c'].split('->')[0]}-{segs[-1]['temp_c'].split('->')[1]}",
                fb=segs[0]['fb'])

pairs, n, dropped = [], 0, 0
while True:
    x, y = launch(n), launch(n + 1)
    if x is None or y is None:
        break
    A, B = (x, y) if x['label'] == la else (y, x)
    if not (A['ok'] and B['ok']):
        dropped += 1
        print(f'pair {n//2}: off the pin, dropped')
    else:
        pairs.append((A, B))
        print(f"pair {n//2}: {la} {A['period']:.0f} us ({A['temp']} C, fb {A['fb']})  {lb} {B['period']:.0f} us "
              f"({B['temp']} C, fb {B['fb']})  delta {B['period']-A['period']:+.0f} us")
    n += 2
print(f'{tag}: {len(pairs)} pairs, {dropped} dropped')
for k, unit in (('period', 'us'), ('proc', 'us'), ('thread', 'us'), ('over40', '%')):
    m = len(pairs)
    if m < 2:
        break
    a = sum(p[0][k] for p in pairs) / m
    b = sum(p[1][k] for p in pairs) / m
    ds = [p[1][k] - p[0][k] for p in pairs]
    mu = sum(ds) / m
    sd = math.sqrt(sum((v - mu) ** 2 for v in ds) / (m - 1))
    se = sd / math.sqrt(m)
    t = mu / se if se else float('inf')
    rel = f' ({100*mu/a:+.1f}%)' if a and unit == 'us' else ''
    extra = f'  fps {1e6/a:.1f} -> {1e6/b:.1f}' if k == 'period' else ''
    print(f'{k:7s} A={a:.1f} B={b:.1f} delta={mu:+.1f} {unit}{rel} t={t:.1f} mde={T.get(m-1, 2.0)*se:.1f} {unit}{extra}')
