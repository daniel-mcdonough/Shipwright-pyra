#!/usr/bin/env python3
# Real play by scene, from a watch-mode log (report 8 §8.43, NEXT.md task 0 (c)).
#   watch-block lines (300 frames each): frames over 34 ms, the longest, and for frames over 40 ms
#     the render thread's CPU share across them (high = computing, low = waiting) and their swap.
#   split + split_blocks lines (SOH_BENCH_SPLIT=<ticks>): the §8.43 blocks, converted from CPU ms
#     per tick to CPU us per frame with the window's frames_per_tick, averaged by scene.
# usage: watchsplit.py watch.log [MHz [fps]]   (MHz: the pin, default 1000; windows off it are dropped.
#        fps: the run's frame-rate target, default 30. hist_ms and slow assume 30.)
import re, sys, os
from collections import defaultdict, OrderedDict

PIN = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
TARGET = int(sys.argv[3]) if len(sys.argv) > 3 else 30
here = os.path.dirname(os.path.abspath(__file__))
names = {}
try:
    for line in open(os.path.join(here, '../../../soh/include/tables/scene_table.h')):
        m = re.match(r'/\* (0x[0-9A-Fa-f]+) \*/ DEFINE_SCENE\([^,]+, [^,]+, SCENE_(\w+)', line)
        if m: names[int(m.group(1), 16)] = m.group(2).lower()
except OSError:
    pass
def sname(s): return f'{s:#04x} {names.get(s, "?")}'

def kv(s): return dict(re.findall(r'(\w+)=([^\s]+)', s))

blocks = defaultdict(list)        # scene -> watch-block dicts
splits = defaultdict(list)        # scene -> (frames_per_tick, fps, {block: ms/tick})
dropped = defaultdict(int)
rates = defaultdict(list)          # scene -> (frames_per_tick, fps) per split window
last_split = None
load_scene = None
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'Benchmark (watch-block|split_blocks|split) (.*)', line)
    if not m: continue
    kind, rest = m.groups()
    if kind == 'watch-block':
        k = kv(rest)
        if int(k['cpu_min_mhz']) != PIN or int(k['cpu_max_mhz']) != PIN:
            dropped['watch-block'] += 1; continue
        blocks[int(k['scene'])].append(k)
    elif kind == 'split':
        k = kv(rest)
        if 'scene' not in k: last_split = None; continue   # the per-segment BENCH form
        lo, hi = map(int, k['cpu_mhz'].split('-'))
        last_split = None if (lo != PIN or hi != PIN) else k
        if last_split is None: dropped['split'] += 1
    else:  # split_blocks follows its split line
        if last_split is None: continue
        b = {n: float(v) for n, v in re.findall(r' (\w+)=([\d.]+)', rest) if n != 'ticks'}
        splits[int(last_split['scene'], 16)].append(
            (float(last_split['frames_per_tick']), float(last_split['fps']), b))
        sc = int(last_split['scene'], 16)
        if load_scene is None: load_scene = sc
        rates[sc].append((float(last_split['frames_per_tick']), float(last_split['fps'])))
        last_split = None

if dropped: print('dropped off the pin:', dict(dropped))
# The game's own frame count per split window (~5-7 s) against the run's target (TARGET, default 30;
# pass 20 for an InterpolationFPS=20 run). Unlike hist_ms it works at any rate. frames_per_tick does not
# give the target: some states run a tick per frame at 30 fps, and some windows of a 20 fps run go over
# 20. So a window's loss is its shortfall below the target, never negative, and a window over the
# target is counted separately rather than averaged against the misses. The run's first window is the load.
print(f'\n== frame rate by scene, per split window, target {TARGET} fps ==')
print(f'{"scene":28s} {"win":>4s} {"lost%":>6s} {"min":>6s} {"<98.3%":>7s} {"<90%":>5s} {"over":>5s}')
allr = []
first = True
for s_ in sorted(rates, key=lambda s: -len(rates[s])):
    v = [fps for _, fps in rates[s_]]
    if s_ == load_scene: v = v[1:]
    if not v: continue
    allr += v
    lost = sum(max(0.0, 1 - x / TARGET) for x in v) / len(v)
    print(f'{sname(s_):28s} {len(v):4d} {100*lost:6.1f} {min(v):6.2f} {sum(1 for x in v if x < 0.983*TARGET):7d}'
          f' {sum(1 for x in v if x < 0.9*TARGET):5d} {sum(1 for x in v if x > 1.02*TARGET):5d}')
lost = sum(max(0.0, 1 - x / TARGET) for x in allr) / len(allr)
print(f'{"all":28s} {len(allr):4d} {100*lost:6.1f} {min(allr):6.2f} {sum(1 for x in allr if x < 0.983*TARGET):7d}'
      f' {sum(1 for x in allr if x < 0.9*TARGET):5d} {sum(1 for x in allr if x > 1.02*TARGET):5d}')

print(f'\n== watch blocks by scene (300 frames each, {PIN} MHz) ==')
print(f'{"scene":28s} {"blk":>4s} {"min":>5s} {"cpu_us":>7s} {">34ms":>6s} {"%":>5s} {">40":>5s} {">67":>4s}'
      f' {"max_ms":>6s} {"slow_cpu%":>9s} {"swap_ms":>7s} {"temp":>9s}')
tot = defaultdict(float)
for s in sorted(blocks, key=lambda s: -len(blocks[s])):
    bs = blocks[s]
    frames = sum(int(b['frames']) for b in bs)
    minutes = sum(int(b['frames']) * float(b['period_us']) for b in bs) / 60e6
    cpu = sum(int(b['frames']) * float(b['thread_cpu_us']) for b in bs) / frames
    h = [[int(x.split(':')[1]) for x in b['hist_ms'].split('/')] for b in bs]
    over = sum(sum(x[1:]) for x in h); o40 = sum(sum(x[2:]) for x in h); o67 = sum(x[4] for x in h)
    slow_t = sum(int(b['slow']) * float(b['slow_ms']) for b in bs)
    slow_cpu = (sum(int(b['slow']) * float(b['slow_ms']) * float(b['slow_cpu_pct']) for b in bs) / slow_t
                if slow_t else float('nan'))
    slow_n = sum(int(b['slow']) for b in bs)
    swap = sum(int(b['slow']) * float(b['slow_swap_ms']) for b in bs) / slow_n if slow_n else float('nan')
    temps = [float(b['temp_c']) for b in bs]
    print(f'{sname(s):28s} {len(bs):4d} {minutes:5.1f} {cpu:7.0f} {over:6d} {100*over/frames:5.1f} {o40:5d} {o67:4d}'
          f' {max(float(b["max_ms"]) for b in bs):6.1f} {slow_cpu:9.1f} {swap:7.1f} {min(temps):4.1f}-{max(temps):4.1f}')
    for n, v in (('frames', frames), ('over', over), ('o40', o40), ('min', minutes)): tot[n] += v
if tot['frames']:
    print(f'{"all":28s} {"":4s} {tot["min"]:5.1f} {"":7s} {int(tot["over"]):6d} {100*tot["over"]/tot["frames"]:5.1f}'
          f' {int(tot["o40"]):5d}')

print(f'\n== main-thread blocks by scene, CPU us per frame (SOH_BENCH_SPLIT windows) ==')
order = list(OrderedDict.fromkeys(n for v in splits.values() for _, _, b in v for n in b))
scenes = sorted(splits, key=lambda s: -len(splits[s]))
per_frame = {s: {n: sum(b.get(n, 0) / fpt for fpt, _, b in splits[s]) * 1000 / len(splits[s]) for n in order}
             for s in scenes}
allw = [w for s in scenes for w in splits[s]]
if len(scenes) > 1:   # the whole session, every window weighted the same
    scenes.append('all'); names['all'] = 'all'
    splits['all'] = allw
    per_frame['all'] = {n: sum(b.get(n, 0) / fpt for fpt, _, b in allw) * 1000 / len(allw) for n in order}
print(f'{"block":16s}' + ''.join(f'{names.get(s, str(s))[:14]:>15s}' for s in scenes))
print(f'{"windows":16s}' + ''.join(f'{len(splits[s]):15d}' for s in scenes))
print(f'{"fps":16s}' + ''.join(f'{sum(f for _, f, _ in splits[s]) / len(splits[s]):15.1f}' for s in scenes))
for n in order:
    print(f'{n:16s}' + ''.join(f'{per_frame[s][n]:15.0f}' for s in scenes))
print(f'{"sum":16s}' + ''.join(f'{sum(per_frame[s].values()):15.0f}' for s in scenes))
