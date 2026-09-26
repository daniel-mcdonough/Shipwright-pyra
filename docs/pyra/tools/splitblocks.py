#!/usr/bin/env python3
# The main thread's blocks per frame period from a bench run.log (BENCH split lines, report 8 §8.43),
# averaged over the segments of one arm that stayed on the pin, with the closure checks:
#   cpu_sum (every block)            against the oncpu line's thread_us
#   gui_start .. eof (the window)    against the oncpu line's window_us
# usage: splitblocks.py run.log [arm] [MHz]   (arm: only segments of that arm; MHz: the pin, default 1000)
import re, sys, math
ARM = sys.argv[2] if len(sys.argv) > 2 else None
PIN = sys.argv[3] if len(sys.argv) > 3 else '1000'
WINDOW = ('gui_start', 'fb_start', 'run', 'gui_end', 'eof')
seg = {}
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'BENCH (segment|oncpu|split) (\d+) (.*)', line)
    if not m: continue
    i = int(m.group(2))
    if i in seg and m.group(1) in seg[i]: continue  # first occurrence only (TESTING.md trap 13)
    seg.setdefault(i, {})[m.group(1)] = m.group(3)
rows = []
for i in sorted(seg):
    s = seg[i]
    if 'split' not in s or 'segment' not in s or 'oncpu' not in s: continue
    k = dict(re.findall(r'(\w+)=([^\s]+)', s['segment']))
    if ARM is not None and k['arm'] != ARM: continue
    a, b = k['cpu_mhz'].split('->')
    if not all(v == PIN for v in (a, b, k['cpu_min_mhz'], k['cpu_max_mhz'])):
        print(f'segment {i}: off the {PIN} MHz pin, dropped'); continue
    on = dict(re.findall(r'(\w+)=([^\s]+)', s['oncpu']))
    sp = s['split']
    blocks = {n: tuple(map(float, v.split('/'))) for n, v in re.findall(r' (\w+)=([\d.]+/[\d.]+)', sp)}
    head = dict(re.findall(r'(\w+)=([\d.]+)(?:\s|$)', sp))
    rows.append(dict(i=i, thread=float(on['thread_us']), window=float(on['window_us']),
                     period=float(k['period_us']), ticks=float(head['ticks']), blocks=blocks))
if not rows: sys.exit('no BENCH split lines on the pin')
n = len(rows)
def mean_sd(xs):
    mu = sum(xs) / len(xs)
    return mu, (math.sqrt(sum((x - mu) ** 2 for x in xs) / (len(xs) - 1)) if len(xs) > 1 else float('nan'))
names = list(rows[0]['blocks'])
print(f'{n} segments: {", ".join(str(r["i"]) for r in rows)}; ticks per frame {mean_sd([r["ticks"] for r in rows])[0]:.3f}')
print(f'{"block":16s} {"cpu_us":>9s} {"sd":>6s} {"wall_us":>9s} {"cpu/wall":>8s}')
for nm in names:
    c = mean_sd([r['blocks'][nm][0] for r in rows]); w = mean_sd([r['blocks'][nm][1] for r in rows])
    print(f'{nm:16s} {c[0]:9.1f} {c[1]:6.1f} {w[0]:9.1f} {100*c[0]/w[0] if w[0] else 0:7.0f}%')
cs = mean_sd([sum(b[0] for b in r['blocks'].values()) for r in rows])
ws = mean_sd([sum(b[1] for b in r['blocks'].values()) for r in rows])
win = mean_sd([sum(r['blocks'][x][0] for x in WINDOW) for r in rows])
th = mean_sd([r['thread'] for r in rows]); wu = mean_sd([r['window'] for r in rows])
pe = mean_sd([r['period'] for r in rows])
d1 = mean_sd([sum(b[0] for b in r['blocks'].values()) - r['thread'] for r in rows])
d2 = mean_sd([sum(r['blocks'][x][0] for x in WINDOW) - r['window'] for r in rows])
print(f'{"sum":16s} {cs[0]:9.1f} {cs[1]:6.1f} {ws[0]:9.1f}   (period_us {pe[0]:.1f})')
print(f'closure: cpu_sum - thread_us = {d1[0]:+.1f} us (sd {d1[1]:.1f}); thread_us {th[0]:.1f}')
print(f'closure: window blocks - window_us = {d2[0]:+.1f} us (sd {d2[1]:.1f}); window blocks {win[0]:.1f}, window_us {wu[0]:.1f}')
print(f'outside the window: blocks {cs[0]-win[0]:.1f} us against thread_us - window_us {th[0]-wu[0]:.1f} us')
