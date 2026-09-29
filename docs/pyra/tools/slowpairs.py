#!/usr/bin/env python3
# Real-play miss rates from `bench.sh watch blocks` output (§11.14, TESTING trap 26): frames over 40 ms
# (hist_ms 50 + 67 + over) per scene and arm, pooled, and paired by block, B minus A, with t and a 95% MDE.
# Usage: slowpairs.py blocks.txt arm <A> <B> [MHz]   - the A/B CVar's arms, blocks off the pin dropped
#        slowpairs.py blocks.txt clock <A> <B>       - arms are the pinned clock (clockflip.sh); blocks that
#                                                      straddle a clock change are dropped
#        slowpairs.py watch.log volt <A> <B> voltflip.log [MHz] - arms are the 750 MHz point's voltage (voltflip.sh,
#                                                      §8.51); needs watch.log itself (the block lines' timestamps),
#                                                      and drops blocks that overlap a switch
# Also pairs thread_cpu_us (main-thread CPU per frame) the same way.
# Pairs are consecutive kept blocks with different arms in the same scene, each block used once.
import re, sys, math, collections
key, A, B = sys.argv[2], sys.argv[3], sys.argv[4]
rest = sys.argv[5:]
flips = []
def secs(t):
    h, m, x = t.split(':'); return int(h) * 3600 + int(m) * 60 + float(x)
if key == 'volt':
    for l in open(rest.pop(0)):
        if not l.startswith('#'):
            t0, t1, uv = l.split()[:3]; flips.append((secs(t0), secs(t1), uv))
mhz = rest[0] if rest else None
dropped = 0
names = {'32': 'Market', '96': 'Trail'}
bl = {}
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'watch-block (\d+) (.*)', line)
    if not m or int(m.group(1)) in bl:
        continue
    d = dict(re.findall(r'(\w+)=([^\s]+)', m.group(2)))
    if d['cpu_min_mhz'] != d['cpu_max_mhz'] or (mhz and d['cpu_max_mhz'] != mhz):
        continue
    if key == 'volt':
        ts = re.match(r'\[(\d\d:\d\d:[\d.]+)\]', line)
        if not ts:
            sys.exit('volt needs watch.log lines with their [HH:MM:SS.mmm] prefix, not `watch blocks` output')
        end = secs(ts.group(1)); start = end - int(d['frames']) * float(d['period_us']) / 1e6
        if any(t0 - 0.3 < end and t1 + 0.3 > start for t0, t1, _ in flips):
            dropped += 1; continue
        before = [uv for t0, t1, uv in flips if t1 <= start]
        if not before:
            continue
        d['k'] = before[-1]
    else:
        d['k'] = d['arm'] if key == 'arm' else d['cpu_max_mhz']
    if d['k'] not in (A, B):
        continue
    h = dict(x.split(':') for x in d['hist_ms'].split('/'))
    d['slow'], d['o67'], d['n'] = int(h['50']) + int(h['67']) + int(h['+']), int(h['+']), int(d['frames'])
    d['s'] = names.get(d['scene'], 'other')
    bl[int(m.group(1))] = d
pool = collections.defaultdict(lambda: [0, 0, 0, 0])
for d in bl.values():
    for s in (d['s'], 'all'):
        p = pool[(s, d['k'])]; p[0] += d['slow']; p[1] += d['n']; p[2] += 1; p[3] += d['o67']
print(f'{len(bl)} blocks kept' + (f', {dropped} dropped across a switch' if key == 'volt' else '') + '; frames over 40 ms (over 67 ms)')
for s in ('Market', 'Trail', 'other', 'all'):
    row = [f'{100*p[0]/p[1]:.2f}% ({p[0]}/{p[1]}, {p[2]} blk; {100*p[3]/p[1]:.2f}%)' if p[1] else '-'
           for p in (pool[(s, A)], pool[(s, B)])]
    print(f'{s:6s} {A}: {row[0]:38s} {B}: {row[1]}')
ds = collections.defaultdict(list); dc = collections.defaultdict(list); order = sorted(bl); k = 0
while k < len(order) - 1:
    a, b = bl[order[k]], bl[order[k + 1]]
    if a['k'] != b['k'] and a['scene'] == b['scene']:
        x, y = (a, b) if a['k'] == A else (b, a)
        r = 100 * (y['slow'] / y['n'] - x['slow'] / x['n'])
        ds[a['s']].append(r); ds['all'].append(r); k += 2
        c = float(y['thread_cpu_us']) - float(x['thread_cpu_us'])
        dc[a['s']].append((c, float(x['thread_cpu_us']))); dc['all'].append((c, float(x['thread_cpu_us'])))
    else:
        k += 1
for s in ('Market', 'Trail', 'all'):
    x = ds[s]; n = len(x)
    if n > 1:
        mu = sum(x) / n; se = math.sqrt(sum((v - mu) ** 2 for v in x) / (n - 1)) / math.sqrt(n)
        print(f'paired {s:6s} n={n:2d}  {B}-{A} {mu:+.2f} points  t={mu/se:.2f}  mde={(2.0 if n > 30 else 2.1)*se:.2f}')
for s in ('Market', 'Trail', 'all'):
    x = [c for c, _ in dc[s]]; n = len(x)
    if n > 1:
        mu = sum(x) / n; se = math.sqrt(sum((v - mu) ** 2 for v in x) / (n - 1)) / math.sqrt(n)
        base = sum(a for _, a in dc[s]) / n
        print(f'paired {s:6s} n={n:2d}  thread_cpu_us A {base:.0f}  {B}-{A} {mu:+.0f} us ({100*mu/base:+.1f}%)  t={mu/se:.2f}  mde={(2.0 if n > 30 else 2.1)*se:.0f}')
