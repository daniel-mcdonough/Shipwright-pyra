#!/usr/bin/env python3
# Paired real-play A/B from `bench.sh watch blocks` output (§8.25, §8.30): blocks pair as (0,1),(2,3)...
# A pair is dropped if either block left the 1250 MHz pin or the two blocks are in different scenes.
# Reports render-thread CPU per frame (thread_cpu_us) as B minus A, with t and a 95% MDE.
import re, sys, math
blocks = {}
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'watch-block (\d+) (.*)', line)
    if m and int(m.group(1)) not in blocks:
        blocks[int(m.group(1))] = dict(re.findall(r'(\w+)=([^\s]+)', m.group(2)))
armA = sys.argv[2]
ds, a_sum, dropped = [], 0.0, {'pin': 0, 'scene': 0}
for i in range(0, max(blocks) + 1 if blocks else 0, 2):
    if i not in blocks or i + 1 not in blocks:
        continue
    p = (blocks[i], blocks[i + 1])
    if any(b['cpu_min_mhz'] != '1250' or b['cpu_max_mhz'] != '1250' for b in p):
        dropped['pin'] += 1; continue
    if p[0]['scene'] != p[1]['scene']:
        dropped['scene'] += 1; continue
    A, B = (p[0], p[1]) if p[0]['arm'] == armA else (p[1], p[0])
    a, b = float(A['thread_cpu_us']), float(B['thread_cpu_us'])
    ds.append(b - a); a_sum += a
n = len(ds)
scenes = sorted({b['scene'] for b in blocks.values()})
print(f'{len(blocks)} blocks, scenes {",".join(scenes)}; pairs used {n}, dropped {dropped}')
if n > 1:
    mu = sum(ds) / n; sd = math.sqrt(sum((x - mu) ** 2 for x in ds) / (n - 1)); se = sd / math.sqrt(n)
    tcrit = 2.0 if n > 30 else 2.1
    print(f'thread_cpu_us A={a_sum/n:.0f} delta={mu:+.0f} us ({100*mu/(a_sum/n):+.1f}%) t={mu/se:.2f} mde={tcrit*se:.0f}')
